"""Quake 2 data files: the .pak archive, the palette, .wal textures and .bsp maps (version 38).

Layouts from vendor/quake2/qcommon/qfiles.h.
"""
import struct


class Pak:
    """a PACK archive: a directory of (name, offset, length)"""

    def __init__(self, path):
        self.f = open(path, "rb")
        ident, dirofs, dirlen = struct.unpack("<4sii", self.f.read(12))
        if ident != b"PACK":
            raise ValueError(path + ": not a pak file")
        self.f.seek(dirofs)
        d = self.f.read(dirlen)
        self.files = {}
        for i in range(0, dirlen, 64):
            name, ofs, ln = struct.unpack("<56sii", d[i:i + 64])
            self.files[name.split(b"\0")[0].decode("latin-1").lower()] = (ofs, ln)

    def read(self, name):
        ofs, ln = self.files[name.lower()]
        self.f.seek(ofs)
        return self.f.read(ln)

    def has(self, name):
        return name.lower() in self.files


def palette(pak):
    """the 256-colour palette: the last 768 bytes of pics/colormap.pcx"""
    pcx = pak.read("pics/colormap.pcx")
    p = pcx[-768:]
    return [(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]) for i in range(256)]


class Wal:
    """a .wal texture: 8-bit palette indices, 4 mip levels (we use mip 0)"""

    def __init__(self, data):
        name, self.w, self.h = struct.unpack("<32sII", data[:40])
        offs = struct.unpack("<4I", data[40:56])
        self.name = name.split(b"\0")[0].decode("latin-1")
        anim = struct.unpack("<32s", data[56:88])[0]
        self.animname = anim.split(b"\0")[0].decode("latin-1")
        self.flags, self.contents, self.value = struct.unpack("<3i", data[88:100])
        self.pixels = data[offs[0]:offs[0] + self.w * self.h]


LUMPS = ["entities", "planes", "vertexes", "visibility", "nodes", "texinfo", "faces", "lighting",
         "leafs", "leaffaces", "leafbrushes", "edges", "surfedges", "models", "brushes",
         "brushsides", "pop", "areas", "areaportals"]

SURF_LIGHT, SURF_SLICK, SURF_SKY, SURF_WARP = 0x1, 0x2, 0x4, 0x8
SURF_TRANS33, SURF_TRANS66, SURF_FLOWING, SURF_NODRAW = 0x10, 0x20, 0x40, 0x80


def _records(data, fmt):
    size = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, data, i) for i in range(0, len(data) - size + 1, size)]


class Bsp:
    def __init__(self, data):
        ident, version = struct.unpack("<4si", data[:8])
        if ident != b"IBSP" or version != 38:
            raise ValueError("not a Quake 2 bsp")
        self.raw = {}
        for i, name in enumerate(LUMPS):
            ofs, ln = struct.unpack_from("<ii", data, 8 + i * 8)
            self.raw[name] = data[ofs:ofs + ln]
        r = self.raw
        self.entities = r["entities"].split(b"\0")[0].decode("latin-1")
        self.planes = [(p[0:3], p[3], p[4]) for p in _records(r["planes"], "<4fi")]
        self.vertexes = _records(r["vertexes"], "<3f")
        # node: plane, children[2], mins[3], maxs[3], firstface, numfaces
        self.nodes = [(n[0], (n[1], n[2]), n[3:6], n[6:9], n[9], n[10]) for n in _records(r["nodes"], "<3i6h2H")]
        self.texinfo = []
        for t in _records(r["texinfo"], "<8fii32si"):
            self.texinfo.append({"s": t[0:4], "t": t[4:8], "flags": t[8], "value": t[9],
                                 "texture": t[10].split(b"\0")[0].decode("latin-1").lower(), "next": t[11]})
        # face: planenum, side, firstedge, numedges, texinfo, styles[4], lightofs
        self.faces = []
        for f in _records(r["faces"], "<Hhih h4Bi".replace(" ", "")):
            self.faces.append({"plane": f[0], "side": f[1], "firstedge": f[2], "numedges": f[3],
                               "texinfo": f[4], "styles": f[5:9], "lightofs": f[9]})
        self.lighting = r["lighting"]
        # leaf: contents, cluster, area, mins[3], maxs[3], firstleafface, numleaffaces, firstleafbrush, numleafbrushes
        self.leafs = [{"contents": l[0], "cluster": l[1], "area": l[2], "mins": l[3:6], "maxs": l[6:9],
                       "firstface": l[9], "numfaces": l[10], "firstbrush": l[11], "numbrushes": l[12]}
                      for l in _records(r["leafs"], "<i8h4H")]
        self.leaffaces = [x[0] for x in _records(r["leaffaces"], "<H")]
        self.leafbrushes = [x[0] for x in _records(r["leafbrushes"], "<H")]
        self.edges = _records(r["edges"], "<2H")
        self.surfedges = [x[0] for x in _records(r["surfedges"], "<i")]
        self.models = [{"mins": m[0:3], "maxs": m[3:6], "origin": m[6:9], "headnode": m[9],
                        "firstface": m[10], "numfaces": m[11]} for m in _records(r["models"], "<9f3i")]
        self.brushes = [{"firstside": b[0], "numsides": b[1], "contents": b[2]} for b in _records(r["brushes"], "<3i")]
        self.brushsides = [{"plane": s[0], "texinfo": s[1]} for s in _records(r["brushsides"], "<Hh")]
        self.areas = _records(r["areas"], "<2i")
        self.areaportals = _records(r["areaportals"], "<2i")
        vis = r["visibility"]
        if vis:
            n = struct.unpack_from("<i", vis, 0)[0]
            self.numclusters = n
            self.visofs = [struct.unpack_from("<2i", vis, 4 + i * 8) for i in range(n)]
        else:
            self.numclusters = 0
            self.visofs = []

    def face_verts(self, fi):
        """a face's polygon, in order, as world points"""
        f = self.faces[fi]
        out = []
        for i in range(f["numedges"]):
            e = self.surfedges[f["firstedge"] + i]
            v = self.edges[e][0] if e >= 0 else self.edges[-e][1]
            out.append(self.vertexes[v])
        return out

    def face_vert_ids(self, fi):
        f = self.faces[fi]
        out = []
        for i in range(f["numedges"]):
            e = self.surfedges[f["firstedge"] + i]
            out.append(self.edges[e][0] if e >= 0 else self.edges[-e][1])
        return out

    def pvs(self, cluster):
        """the decompressed PVS row of a cluster, as bytes"""
        vis = self.raw["visibility"]
        ofs = self.visofs[cluster][0]
        row = (self.numclusters + 7) >> 3
        out = bytearray()
        while len(out) < row:
            b = vis[ofs]
            ofs += 1
            if b:
                out.append(b)
            else:
                out.extend(b"\0" * vis[ofs])
                ofs += 1
        return bytes(out[:row])

    def parse_entities(self):
        ents, cur = [], None
        tok = _tokens(self.entities)
        for t in tok:
            if t == "{":
                cur = {}
            elif t == "}":
                ents.append(cur)
                cur = None
            else:
                cur[t] = next(tok)
        return ents


def _tokens(s):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
        elif c == '"':
            j = s.index('"', i + 1)
            yield s[i + 1:j]
            i = j + 1
        elif c in "{}":
            yield c
            i += 1
        else:
            j = i
            while j < n and not s[j].isspace():
                j += 1
            yield s[i:j]
            i = j
