#!/usr/bin/env python3
"""Bake a Quake 2 MD2 model for the Saturn (src/model.c).

    tools/bake_md2.py data/pak0.pak models/monsters/soldier/tris.md2 cd/SOLDIER.MDL \\
        --skins=skin,skin_lt,skin_ss --anims=stand1:stand101-stand130,run:run03-run08,...

Triangles that share an edge in both the mesh and the skin become quads.
VDP1 maps a whole texture rectangle onto a quad (no texture coordinates),
so each polygon gets its own little texture: its part of the skin, warped so
the rectangle's corners are the polygon's (a triangle's last two are the
same corner). 4bpp, a 16-colour table each, at most 16x16 so they fit the
renderer's texture cache slots.

Frames keep MD2's packing: a byte a coordinate, scaled and moved per frame,
and a normal index for the shading table (16 yaw steps x 162 normals).
Big-endian; the layout matches src/model.c.
"""
import io
import math
import os
import re
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2data import Pak, palette

MAX_TEX = 16
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rgb555(c):
    return 0x8000 | ((c[2] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[0] >> 3)


def anorms():
    """the 162 normals (tools/anorms.h: Quake 2's client/anorms.h)"""
    src = open(os.path.join(ROOT, "tools", "anorms.h")).read()
    nums = [float(x) for x in re.findall(r"-?\d+\.\d+", src)]
    return [tuple(nums[i:i + 3]) for i in range(0, len(nums), 3)]


class Md2:
    def __init__(self, data):
        h = struct.unpack("<17i", data[:68])
        (self.ident, self.version, self.skinw, self.skinh, framesize, nskins, self.nverts, nst, ntris,
         nglcmds, nframes, ofs_skins, ofs_st, ofs_tris, ofs_frames, _, _) = h
        self.st = [struct.unpack_from("<2h", data, ofs_st + i * 4) for i in range(nst)]
        self.tris = [struct.unpack_from("<6h", data, ofs_tris + i * 12) for i in range(ntris)]
        self.frames = []
        for f in range(nframes):
            o = ofs_frames + f * framesize
            scale = struct.unpack_from("<3f", data, o)
            trans = struct.unpack_from("<3f", data, o + 12)
            name = data[o + 24:o + 40].split(b"\0")[0].decode()
            verts = [struct.unpack_from("<4B", data, o + 40 + i * 4) for i in range(self.nverts)]
            self.frames.append((name, scale, trans, verts))
        self.frame_index = {f[0]: i for i, f in enumerate(self.frames)}

    def pos(self, frame, v):
        _, s, t, verts = self.frames[frame]
        return tuple(verts[v][k] * s[k] + t[k] for k in range(3))


def cross2(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def convex(uv):
    signs = [cross2(uv[i], uv[(i + 1) % 4], uv[(i + 2) % 4]) for i in range(4)]
    return all(s > 1e-3 for s in signs) or all(s < -1e-3 for s in signs)


def tri_normal(m, tri):
    a, b, c = (m.pos(0, tri[i]) for i in range(3))
    u = [b[k] - a[k] for k in range(3)]
    v = [c[k] - a[k] for k in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    ln = math.sqrt(sum(x * x for x in n)) or 1.0
    return tuple(x / ln for x in n)


def make_polys(m):
    """pair triangles into quads: (xyz[4], st[4]); a triangle repeats its last corner"""
    tris = [((t[0], t[1], t[2]), (t[3], t[4], t[5])) for t in m.tris]
    edge = {}
    for ti, (xyz, st) in enumerate(tris):
        for e in range(3):
            key = (xyz[e], xyz[(e + 1) % 3], st[e], st[(e + 1) % 3])
            edge[key] = (ti, e)
    used = [False] * len(tris)
    polys = []
    normals = [tri_normal(m, xyz) for xyz, _ in tris]
    for ti, (xyz, st) in enumerate(tris):
        if used[ti]:
            continue
        best = None
        for e in range(3):
            # the neighbour runs the shared edge the other way, with the same skin coordinates
            key = (xyz[(e + 1) % 3], xyz[e], st[(e + 1) % 3], st[e])
            if key not in edge:
                continue
            tj, ej = edge[key]
            if tj == ti or used[tj]:
                continue
            q = tris[tj][0][(ej + 2) % 3]
            qs = tris[tj][1][(ej + 2) % 3]
            # the quad: this triangle's corners with the neighbour's inserted into the shared edge
            order = [(xyz[(e + k) % 3], st[(e + k) % 3]) for k in range(1, 4)]    # e+1, e+2, e (edge e -> e+1)
            quad = [order[2], (q, qs), order[0], order[1]]                          # e, q, e+1, e+2
            uv = [m.st[s] for _, s in quad]
            if not convex(uv):
                continue
            d = sum(normals[ti][k] * normals[tj][k] for k in range(3))
            if d < 0.75:
                continue
            if best is None or d > best[0]:
                best = (d, tj, quad)
        if best:
            used[ti] = used[best[1]] = True
            polys.append(([v for v, _ in best[2]], [s for _, s in best[2]]))
        else:
            used[ti] = True
            polys.append(([xyz[0], xyz[1], xyz[2], xyz[2]], [st[0], st[1], st[2], st[2]]))
    return polys


class Decimated:
    """the model with its vertices merged on a coarse grid (in the first frame): for when it's far.
    Each vertex goes to its cell's nearest to the cell's middle; triangles that close up go, and so
    do repeats. The corners keep their own skin coordinates (a little out, at that distance)."""
    def __init__(self, m, cells):
        pts = [m.pos(0, v) for v in range(m.nverts)]
        lo = [min(p[k] for p in pts) for k in range(3)]
        hi = [max(p[k] for p in pts) for k in range(3)]
        size = max(hi[k] - lo[k] for k in range(3)) / cells or 1.0
        groups = {}
        for v, p in enumerate(pts):
            groups.setdefault(tuple(int((p[k] - lo[k]) / size) for k in range(3)), []).append(v)
        rep = [0] * m.nverts
        for key, vs in groups.items():
            mid = [lo[k] + (key[k] + 0.5) * size for k in range(3)]
            best = min(vs, key=lambda v: sum((pts[v][k] - mid[k]) ** 2 for k in range(3)))
            for v in vs:
                rep[v] = best
        tris, seen = [], set()
        for t in m.tris:
            a, b, c = rep[t[0]], rep[t[1]], rep[t[2]]
            if a == b or b == c or a == c or (a, b, c) in seen:
                continue
            seen.add((a, b, c))
            tris.append((a, b, c, t[3], t[4], t[5]))
        self.tris, self.st, self.frames, self.nverts = tris, m.st, m.frames, m.nverts
        self.pos = m.pos
        self.used = len(groups)


def poly_texture(skin, st_uv, w, h):
    """the skin warped into a w x h rectangle: texel (s, t) samples bilinear(A, B, C, D)(s, t),
    which is where VDP1's quad mapping puts it"""
    A, B, C, D = st_uv
    img = Image.new("RGB", (w, h))
    px = skin.load()
    sw, sh = skin.size
    out = []
    for j in range(h):
        t = (j + 0.5) / h
        for i in range(w):
            s = (i + 0.5) / w
            top = (A[0] + (B[0] - A[0]) * s, A[1] + (B[1] - A[1]) * s)
            bot = (D[0] + (C[0] - D[0]) * s, D[1] + (C[1] - D[1]) * s)
            u = top[0] + (bot[0] - top[0]) * t
            v = top[1] + (bot[1] - top[1]) * t
            out.append(px[min(max(int(u), 0), sw - 1), min(max(int(v), 0), sh - 1)])
    img.putdata(out)
    return img


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict((a[2:].split("=", 1) + [""])[:2] for a in sys.argv[1:] if a.startswith("--"))
    pak = Pak(args[0])
    m = Md2(pak.read(args[1]))
    pal = palette(pak)
    base = os.path.dirname(args[1])
    skins = opts.get("skins", "skin").split(",")
    shared = "sharedlut" in opts
    anims = []
    for spec in (opts.get("anims") or "base:#0-#0").split(","):
        name, rng = spec.split(":")
        a, b = rng.split("-")
        fa = int(a[1:]) if a.startswith("#") else m.frame_index[a]
        fb = int(b[1:]) if b.startswith("#") else m.frame_index[b]
        anims.append((name, fa, fb))
    # the frames used, in order
    frames = []
    anim_recs = []
    for name, a, b in anims:
        anim_recs.append((name, len(frames), b - a + 1))
        frames.extend(range(a, b + 1))

    polys = make_polys(m)
    # far away (src/render.c draw_model): the mesh on a coarse grid, its own polygons and textures
    # after the full one's (--lod=cells across its size; 0: none)
    lod_cells = int(opts.get("lod") or 0)
    lpolys = make_polys(Decimated(m, lod_cells)) if lod_cells else []
    # the vertices it uses first (src/render.c: far, only the first nfverts are worked out, as a
    # whole mesh is): the polygons and frames renumbered to match
    vorder = list(range(m.nverts))
    if lpolys:
        used = sorted({v for xyz, _ in lpolys for v in xyz})
        vorder = used + sorted(set(range(m.nverts)) - set(used))
        renum = {v: i for i, v in enumerate(vorder)}
        polys = [([renum[v] for v in xyz], st) for xyz, st in polys]
        lpolys = [([renum[v] for v in xyz], st) for xyz, st in lpolys]
    npolys = len(polys)
    # src/render.c MAX_MVERTS / MAX_MPOLYS: a model over them would be drawn with stale vertices
    assert m.nverts <= 416 and npolys <= 432, f"{args[1]}: {m.nverts} vertices, {npolys} polygons: over the renderer's 416 / 432"
    polys = polys + lpolys
    # texture sizes: the polygon's extent in the skin, rounded (width a multiple of 8)
    sizes = []
    for xyz, st in polys:
        uv = [m.st[s] for s in st]
        w = max(math.dist(uv[0], uv[1]), math.dist(uv[3], uv[2]))
        h = max(math.dist(uv[0], uv[3]), math.dist(uv[1], uv[2]))
        tw = min(MAX_TEX, max(8, (int(math.ceil(w)) + 7) & ~7))
        th = min(MAX_TEX, max(2, int(math.ceil(h))))
        sizes.append((tw, th))
    tex_table = []
    ofs = 0
    for tw, th in sizes:
        tex_table.append(struct.pack(">IBBH", ofs, tw, th, 0))
        ofs += (tw * th // 2 + 7) & ~7
    per_skin = ofs
    texdata = bytearray()
    luts = bytearray()
    skin_luts = []                          # (each skin's tables, a polygon's each, before they're merged)
    for sk in skins:
        skin_luts.append([])
        pcx = Image.open(io.BytesIO(pak.read("%s/%s.pcx" % (base, sk))))
        skin = pcx.convert("RGB")
        block = bytearray()
        if shared:
            # one colour table for the whole skin (small things: saves VRAM)
            sq = skin.quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            sp = (sq.getpalette() + [0] * 45)[:45]
            luts += struct.pack(">16H", 0, *[rgb555((sp[i * 3], sp[i * 3 + 1], sp[i * 3 + 2])) for i in range(15)])
        for (xyz, st), (tw, th) in zip(polys, sizes):
            img = poly_texture(skin, [m.st[s] for s in st], tw, th)
            if shared:
                q = img.quantize(palette=sq, dither=Image.Dither.NONE)
            else:
                q = img.quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
                p = (q.getpalette() + [0] * 45)[:45]
                skin_luts[-1].append(struct.pack(">16H", 0, *[rgb555((p[i * 3], p[i * 3 + 1], p[i * 3 + 2]))
                                                              for i in range(15)]))
            idx = [min(i, 14) + 1 for i in q.getdata()]
            data = bytes((idx[i] << 4) | idx[i + 1] for i in range(0, len(idx), 2))
            block += data.ljust((len(data) + 7) & ~7, b"\0")
        texdata += block
    # the colour tables, in VRAM for good (src/render.c render_init): a polygon's each, but none
    # kept twice, and after them a map, each skin's polygon textures to their table (u16s, skin
    # by skin: the loader reads it as it puts the texture in the cache, src/render.c tex_load);
    # the header's count has its top bit set then. --keeplut: a table a polygon, skin by skin, no
    # map (the gun: its kept drawing takes the texture's number for the table's). The texture
    # records' last field is the table (of its skin's) where there's no map.
    lut_of = [0] * len(polys)
    if shared:
        nluts = 1
    elif "keeplut" in opts:
        lut_of = list(range(len(polys)))
        nluts = len(polys)
        for sl in skin_luts:
            luts += b"".join(sl)
    else:
        first = {}
        order = []
        lmap = []
        for sl in skin_luts:
            for key in sl:
                if key not in first:
                    first[key] = len(order)
                    order.append(key)
                lmap.append(first[key])
        luts += b"".join(order) + struct.pack(">%dH" % len(lmap), *lmap)
        nluts = len(order) | 0x80000000
    tex_table = [rec[:6] + struct.pack(">H", lut_of[t]) for t, rec in enumerate(tex_table)]
    # frames: scale and translate (16.16), then the byte vertices (x y z normal), then the
    # normals again as a run (padded to 4 bytes): the CPU reads only those while the DSP takes
    # the words, so a run is a quarter of the lines fetched (src/render.c draw_model)
    fdata = bytearray()
    for f in frames:
        _, s, t, verts = m.frames[f]
        fdata += struct.pack(">6i", *[int(round(x * 65536)) for x in s + t])
        for v in vorder:
            fdata += bytes(verts[v])
        fdata += bytes(verts[v][3] for v in vorder) + bytes((-len(vorder)) & 3)
    # shading: light from above and in front, turned with the model's yaw in 16 steps
    norms = anorms()
    shade = bytearray()
    for k in range(16):
        a = -k * 2 * math.pi / 16 + math.pi * 0.25
        L = (math.cos(a) * 0.6, math.sin(a) * 0.6, 0.8)
        for n in norms:
            d = n[0] * L[0] + n[1] * L[1] + n[2] * L[2]
            shade.append(max(0, min(255, int(128 * (1.0 + 0.45 * d)))))
    pdata = b"".join(struct.pack(">4HHH", *xyz, i, 1 if xyz[2] == xyz[3] else 0)
                     for i, (xyz, st) in enumerate(polys[:npolys]))
    # (--spare=n: room for n more records after them, which the renderer fills: the gun's
    # polygons cut at the near plane, its records all copied in one go)
    pdata += bytes(12 * int(opts.get("spare") or 0))
    ldata = b"".join(struct.pack(">4HHH", *xyz, npolys + i, 1 if xyz[2] == xyz[3] else 0)
                     for i, (xyz, st) in enumerate(lpolys))
    if lpolys:
        # and the vertices it uses (a count, then them: 0, 1, ... now, the first)
        used = sorted({v for xyz, _ in lpolys for v in xyz})
        assert used == list(range(len(used)))
        ldata += struct.pack(">%dH" % (len(used) + 1), len(used), *used)
    adata = b"".join(struct.pack(">12sHH", n.encode()[:12], a, c) for n, a, c in anim_recs)

    ndata = b"".join(struct.pack(">3h", *[int(round(c * 16384)) for c in n]) for n in norms)   # 2.14
    parts = [pdata, bytes(b"".join(tex_table)), bytes(texdata), bytes(luts), bytes(fdata), adata, bytes(shade), ndata,
             ldata]
    hdr_size = 64
    offs = []
    o = hdr_size
    for p in parts:
        offs.append(o)
        o += (len(p) + 15) & ~15
    hdr = b"Q2MD" + struct.pack(">6H8I", m.nverts, npolys, len(frames), len(anims), len(skins), len(polys),
                                *offs[:8]) + struct.pack(">2I", per_skin, nluts) \
        + struct.pack(">2I", offs[8], len(lpolys))              # (at 56: the far mesh's polygons, how many)
    out = bytearray(hdr.ljust(hdr_size, b"\0"))
    for p in parts:
        out += p
        out += b"\0" * (((len(p) + 15) & ~15) - len(p))
    with open(args[2], "wb") as fo:
        fo.write(out)
    ntri = sum(1 for xyz, _ in polys[:npolys] if xyz[2] == xyz[3])
    print("%s: %d verts, %d triangles -> %d polygons (%d quads, %d triangles), far %d, %d frames, %d skins, "
          "textures %d bytes a skin, %d colour tables, total %dK" % (
              args[2], m.nverts, len(m.tris), npolys, npolys - ntri, ntri, len(lpolys), len(frames), len(skins),
              per_skin, nluts & 0x7FFFFFFF, len(out) // 1024))


if __name__ == "__main__":
    main()
