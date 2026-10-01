#!/usr/bin/env python3
"""A level's sky for VDP2 (engine/sky.c): its skybox, all six faces, as seen on a cylinder round you,
from 63 degrees above the horizon to 19 below, 1024 wide a turn; in 8 x 8 cells of 16 colours, each
cell with whichever of 48 palettes suits it best (colour RAM 0x000-0x1FF and 0x300-0x3FF). tools/bake_map.py writes it beside each level
(cd/DEMO1.SKY), and the game reads it from the disc straight into VDP2's VRAM.

    tools/bake_sky.py data/pak0.pak unit1_ out.SKY [preview.png]

The cylinder's radius is the screen's focal length (160 pixels: src/render.c FOCAL), so looking
level a row is a screen line; Quake 2's box (ref_gl/gl_warp.c): +x "rt", -x "lf", +y "bk", -y "ft",
+z "up", -z "dn", each face's s and t as it maps them. The strip runs to decreasing yaw, yaw 0 (rt's
middle) at x = 0. Its top and bottom rows fade to the colours VDP2 fills above and below it.

Layout (bytes; the file goes in VRAM at 0x20000):
    0x0000   "Q2SK", u16 w, h, rows above the horizon, palettes, the colours above the strip,
             below it, at the zenith (RGB555), 0, then the palettes (16 each, colour 0
             transparent), then a byte a cell: its palette
    0x2000   the cells, 4bpp (32 bytes each), row by row: 128 x 47, then a blank one
"""
import io
import math
import os
import struct
import sys

from PIL import Image, ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2data import Pak, palette

W, ABOVE, BELOW = 1024, 320, 56     # (rows above the horizon: to atan(320 / 160) = 63 degrees; below: 19)
H = ABOVE + BELOW
R = 160.0                           # (the cylinder's radius: the screen's focal length)
NPAL, PASSES = 48, 6
CELLS = 0x2000
SIZE = 0x31800                      # (whole sectors)
FADE = 24                           # (rows at each edge faded to its colour)


def rgb555(c):
    return 0x8000 | ((c[2] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[0] >> 3)


def faces(pak, name):
    """the six faces, 256 x 256 RGB: rt lf bk ft up dn (Quake 2's axes 0-5)"""
    pal = palette(pak)
    out = []
    for suf in ("rt", "lf", "bk", "ft", "up", "dn"):
        img = None
        for ext in ("tga", "pcx"):
            n = "env/%s%s.%s" % (name, suf, ext)
            if pak.has(n):
                img = Image.open(io.BytesIO(pak.read(n)))
                if img.mode == "P":
                    img.putpalette([v for c in pal for v in c])
                img = img.convert("RGB").resize((256, 256))
                break
        out.append(img or Image.new("RGB", (256, 256), (60, 50, 40)))
    return out


# per axis: s, t and the depth as (component, sign) of the direction (gl_warp.c vec_to_st)
VEC_TO_ST = [((1, -1), (2, 1), (0, 1)), ((1, 1), (2, 1), (0, -1)), ((0, 1), (2, 1), (1, 1)),
             ((0, -1), (2, 1), (1, -1)), ((1, -1), (0, -1), (2, 1)), ((1, -1), (0, 1), (2, -1))]


def sample(px, d):
    a = [abs(c) for c in d]
    if a[0] > a[1] and a[0] > a[2]:
        axis = 1 if d[0] < 0 else 0
    elif a[1] > a[2] and a[1] > a[0]:
        axis = 3 if d[1] < 0 else 2
    else:
        axis = 5 if d[2] < 0 else 4
    (si, ss), (ti, ts), (di, ds) = VEC_TO_ST[axis]
    dv = d[di] * ds
    s, t = d[si] * ss / dv, d[ti] * ts / dv
    u = (s + 1) * 128 - 0.5
    v = (1 - t) * 128 - 0.5
    u, v = min(max(u, 0), 255), min(max(v, 0), 255)
    x0, y0 = int(u), int(v)
    x1, y1 = min(x0 + 1, 255), min(y0 + 1, 255)
    fx, fy = u - x0, v - y0
    f = px[axis]
    c00, c10, c01, c11 = f[y0 * 256 + x0], f[y0 * 256 + x1], f[y1 * 256 + x0], f[y1 * 256 + x1]
    return tuple(int(c00[k] * (1 - fx) * (1 - fy) + c10[k] * fx * (1 - fy) + c01[k] * (1 - fx) * fy
                     + c11[k] * fx * fy + 0.5) for k in range(3))


def panorama(fs):
    px = [list(f.getdata()) for f in fs]
    img = Image.new("RGB", (W, H))
    out = []
    for r in range(H):
        z = (ABOVE - r - 0.5) / R
        for x in range(W):
            a = -(x + 0.5) * 2 * math.pi / W
            out.append(sample(px, (math.cos(a), math.sin(a), z)))
    img.putdata(out)
    return img


def cells_of(img):
    cw, ch = W // 8, H // 8
    return [img.crop((cx * 8, cy * 8, cx * 8 + 8, cy * 8 + 8)) for cy in range(ch) for cx in range(cw)]


def fit(cells, npal=NPAL, passes=PASSES):
    """each cell's palette (of npal, 15 colours each): grouped by their colours, a palette for
    each group (median cut), then each cell to the palette that suits it best, and again"""
    means = [tuple(sum(p[k] for p in c.getdata()) / 64 for k in range(3)) for c in cells]
    order = sorted(range(len(cells)), key=lambda i: sum(means[i]))
    centres = [means[order[(2 * j + 1) * len(order) // (2 * npal)]] for j in range(npal)]
    group = [0] * len(cells)
    for _ in range(12):
        for i, m in enumerate(means):
            group[i] = min(range(npal), key=lambda j: sum((m[k] - centres[j][k]) ** 2 for k in range(3)))
        for j in range(npal):
            mem = [means[i] for i in range(len(cells)) if group[i] == j]
            if mem:
                centres[j] = tuple(sum(m[k] for m in mem) / len(mem) for k in range(3))
    whole = Image.new("RGB", (W, H))
    for i, c in enumerate(cells):
        whole.paste(c, ((i % (W // 8)) * 8, (i // (W // 8)) * 8))
    pals = []
    for _ in range(passes):
        pals = []
        for j in range(npal):
            mem = [i for i in range(len(cells)) if group[i] == j]
            if not mem:
                mem = [j % len(cells)]
            strip = Image.new("RGB", (8 * len(mem), 8))
            for k, i in enumerate(mem):
                strip.paste(cells[i], (k * 8, 0))
            q = strip.quantize(colors=15, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            p = q.getpalette()[:45]
            pals.append([tuple(p[k * 3:k * 3 + 3]) for k in range(len(p) // 3)])
        # each cell's error with each palette (whole picture remapped, the difference averaged a cell)
        errs = []
        for p in pals:
            pi = Image.new("P", (1, 1))
            pi.putpalette([v for c in p + [p[0]] * (256 - len(p)) for v in c])
            rm = whole.quantize(palette=pi, dither=Image.Dither.NONE).convert("RGB")
            e = ImageChops.difference(whole, rm).reduce(8)
            errs.append([sum(c) for c in e.getdata()])
        group = [min(range(npal), key=lambda j: errs[j][i]) for i in range(len(cells))]
    return group, pals


def avg(im):
    px = list(im.getdata())
    return tuple(sum(c[k] for c in px) // len(px) for k in range(3))


def fade(img):
    """the top and bottom rows faded to the colours above and below (their averages)"""
    top, bottom = avg(img.crop((0, 0, W, 2))), avg(img.crop((0, H - 2, W, H)))
    for y in range(FADE):
        t = 1 - (y + 0.5) / FADE
        for row, c in ((y, top), (H - 1 - y, bottom)):
            band = img.crop((0, row, W, row + 1))
            img.paste(Image.blend(band, Image.new("RGB", (W, 1), c), t), (0, row))
    return top, bottom


def bake(pak, name, out_path, preview=None):
    fs = faces(pak, name)
    img = panorama(fs)
    top, bottom = fade(img)
    cells = cells_of(img)
    group, pals = fit(cells)
    data = bytearray(SIZE)
    shown = Image.new("RGB", (W, H))
    for i, c in enumerate(cells):
        p = pals[group[i]]
        idx = [min(range(len(p)), key=lambda k: sum((q[j] - p[k][j]) ** 2 for j in range(3))) for q in c.getdata()]
        for y in range(8):
            for x in range(0, 8, 2):
                data[CELLS + i * 32 + y * 4 + x // 2] = (idx[y * 8 + x] + 1) << 4 | (idx[y * 8 + x + 1] + 1)
        if preview:
            shown.paste(Image.new("RGB", (8, 8)), ((i % (W // 8)) * 8, (i // (W // 8)) * 8))
            shown.paste(Image.frombytes("RGB", (8, 8), bytes(v for k in idx for v in p[k])),
                        ((i % (W // 8)) * 8, (i // (W // 8)) * 8))

    above, below, zenith = rgb555(top), rgb555(bottom), rgb555(avg(fs[4]))
    head = struct.pack(">4s8H", b"Q2SK", W, H, ABOVE, NPAL, above, below, zenith, 0)
    for p in pals:
        cols = [0] + [rgb555(c) for c in p] + [0] * (15 - len(p))
        head += struct.pack(">16H", *cols)
    head += bytes(group)
    assert len(head) <= CELLS and CELLS + (len(cells) + 1) * 32 <= SIZE
    data[:len(head)] = head
    with open(out_path, "wb") as f:
        f.write(data)
    if preview:
        top = Image.new("RGB", (W, 2 * H + 4))
        top.paste(img, (0, 0))
        top.paste(shown, (0, H + 4))
        top.save(preview)


if __name__ == "__main__":
    bake(Pak(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
