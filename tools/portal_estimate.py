#!/usr/bin/env python3
"""How much would portals cull? (an estimate, before building them into the game)

    portal_estimate.py data/pak0.pak demo1 cd/DEMO1.MAP

Makes the BSP's leaf-to-leaf portals again (qbsp's MakeTreePortals), then for
each of the benchmark's views flows the view through them from the camera's
leaf with screen rectangles (each portal's projection cuts the rectangle down;
what the Saturn could afford), and counts the faces and cells drawn now (the
cluster's facevis, facing the camera, on the screen) against those that are
also in a leaf the flow reaches, inside that leaf's rectangle.
"""
import math
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from q2data import Pak, Bsp, SURF_SKY, SURF_NODRAW   # noqa: E402

ON_EPS = 0.1
CONTENTS_SOLID = 1
W, H, FOCAL, CX, CY, NEAR = 320, 224, 160.0, 160.0, 112.0, 8.0

VIEWS = {
    "demo1": [(128, -320, 46, 0x6000, 0), (-1064, 1632, -2, 0x8000, 0), (-1960, 1444, -2, 0xC000, 0),
              (600, -428, -74, 0x4000, 0), (20, -213, 46, 0x7000, 0x800), (-1636, 1488, 142, 0x0000, 0)],
    "demo2": [(832, 2292, -210, 0x8000, 0), (935, 2506, 46, 0x6544, 0), (-103, -300, 30, 0x3D35, 0),
              (-88, -260, 30, 0x4354, 0), (618, -757, -146, 0x1670, 0), (503, -1816, 46, 0x90E5, 0)],
}


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def base_winding(n, d):
    # a big square on the plane
    ax = max(range(3), key=lambda i: abs(n[i]))
    up = (0.0, 0.0, 1.0) if ax != 2 else (1.0, 0.0, 0.0)
    v = dot(up, n)
    up = tuple(up[i] - v * n[i] for i in range(3))
    l = math.sqrt(dot(up, up))
    up = tuple(x / l for x in up)
    rt = (up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0])
    org = tuple(n[i] * d for i in range(3))
    s = 16384.0
    return [tuple(org[i] - rt[i] * s + up[i] * s for i in range(3)), tuple(org[i] + rt[i] * s + up[i] * s for i in range(3)),
            tuple(org[i] + rt[i] * s - up[i] * s for i in range(3)), tuple(org[i] - rt[i] * s - up[i] * s for i in range(3))]


def clip(w, n, d, eps=ON_EPS):
    """split w by the plane: (front, back); None for an empty side"""
    ds = [dot(p, n) - d for p in w]
    sides = [1 if x > eps else -1 if x < -eps else 0 for x in ds]
    if all(s >= 0 for s in sides):
        return w, None
    if all(s <= 0 for s in sides):
        return None, w
    f, b = [], []
    for i, p in enumerate(w):
        q = w[(i + 1) % len(w)]
        if sides[i] >= 0:
            f.append(p)
        if sides[i] <= 0:
            b.append(p)
        if sides[i] and sides[(i + 1) % len(w)] and sides[i] != sides[(i + 1) % len(w)]:
            t = ds[i] / (ds[i] - ds[(i + 1) % len(w)])
            m = tuple(p[k] + t * (q[k] - p[k]) for k in range(3))
            f.append(m)
            b.append(m)
    return (f if len(f) >= 3 else None), (b if len(b) >= 3 else None)


class Node:
    def __init__(self, leaf=None, contents=0):
        self.leaf = leaf                # leaf number, or None for a node (or the outside: -2)
        self.contents = contents
        self.portals = []
        self.plane = None
        self.children = None


class Portal:
    def __init__(self, n, d, w, front, back):
        self.n, self.d, self.w = n, d, w
        self.nodes = [front, back]
        front.portals.append(self)
        back.portals.append(self)

    def remove(self):
        self.nodes[0].portals.remove(self)
        self.nodes[1].portals.remove(self)


def make_portals(b):
    leaves = [Node(i, l["contents"]) for i, l in enumerate(b.leafs)]
    nodes = [Node() for _ in b.nodes]
    for i, nd in enumerate(b.nodes):
        n, d, _ = b.planes[nd[0]]
        nodes[i].plane = (tuple(n), d)
        nodes[i].children = [nodes[c] if c >= 0 else leaves[-1 - c] for c in nd[1]]
    outside = Node(-2, CONTENTS_SOLID)
    head = nodes[b.models[0]["headnode"]]
    mins = [x - 8 for x in b.models[0]["mins"]]
    maxs = [x + 8 for x in b.models[0]["maxs"]]
    bplanes = []
    for i in range(3):
        n = [0.0, 0.0, 0.0]
        n[i] = 1.0
        bplanes.append((tuple(n), mins[i]))
        n = [0.0, 0.0, 0.0]
        n[i] = -1.0
        bplanes.append((tuple(n), -maxs[i]))
    for i, (n, d) in enumerate(bplanes):
        w = base_winding(n, d)
        for j, (n2, d2) in enumerate(bplanes):
            if j != i and w:
                w = clip(w, n2, d2)[0]
        Portal(n, d, w, head, outside)

    def rec(node):
        # this node's own portal: its plane, cut by the portals round its volume
        n, d = node.plane
        w = base_winding(n, d)
        for p in node.portals:
            if not w:
                break
            if p.nodes[0] is node:
                w = clip(w, p.n, p.d)[0]
            else:
                w = clip(w, tuple(-x for x in p.n), -p.d)[0]
        f, bk = node.children
        # the node's portals, split between its children
        for p in list(node.portals):
            side = 0 if p.nodes[0] is node else 1
            other = p.nodes[1 - side]
            p.remove()
            fw, bw = clip(p.w, n, d)
            for part, child in ((fw, f), (bw, bk)):
                if part:
                    if side == 0:
                        Portal(p.n, p.d, part, child, other)
                    else:
                        Portal(p.n, p.d, part, other, child)
        if w:
            Portal(n, d, w, f, bk)
        for c in node.children:
            if c.leaf is None:
                rec(c)
    sys.setrecursionlimit(20000)
    rec(head)
    return leaves


def read_baked(path):
    blob = open(path, "rb").read()
    h = struct.unpack(">60I", blob[12:12 + 240])
    faces = []
    for i in range(h[9]):
        o = h[8] + i * 32
        flags, nu, nv = blob[o + 16], blob[o + 17], blob[o + 18]
        faces.append((flags, nu * nv))
    fv = blob[h[34]:]
    return faces, fv, (h[9] + 7) >> 3


def facevis_row(fv, cluster, nbytes):
    if cluster < 0:
        return b"\xff" * nbytes
    ofs = struct.unpack_from(">I", fv, 4 + cluster * 4)[0]
    out = bytearray()
    while len(out) < nbytes:
        c = fv[ofs]
        if c:
            out.append(c)
            ofs += 1
        else:
            out.extend(b"\0" * fv[ofs + 1])
            ofs += 2
    return bytes(out[:nbytes])


def main():
    pak, name, baked = sys.argv[1], sys.argv[2], sys.argv[3]
    check = "--check" in sys.argv
    union_flow = "--union" in sys.argv
    b = Bsp(Pak(pak).read("maps/%s.bsp" % name))
    leaves = make_portals(b)
    nport = sum(len(l.portals) for l in leaves) // 2
    print("%s: %d leaves, about %d portals" % (name, len(leaves), nport))
    faces, fv, nbytes = read_baked(baked)
    face_polys = [b.face_verts(i) for i in range(len(b.faces))]
    face_norm = []
    for f in b.faces:
        n, d, _ = b.planes[f["plane"]]
        face_norm.append((n, d) if not f["side"] else (tuple(-x for x in n), -d))
    face_leaves = {}
    for li, l in enumerate(b.leafs):
        for fi in b.leaffaces[l["firstface"]:l["firstface"] + l["numfaces"]]:
            face_leaves.setdefault(fi, []).append(li)
    tot = [0, 0, 0, 0]
    for vi, (x, y, z, yaw, pitch) in enumerate(VIEWS[name]):
        cam = (float(x), float(y), float(z))
        a, p = yaw / 65536.0 * 2 * math.pi, pitch / 65536.0 * 2 * math.pi
        fwd = (math.cos(a) * math.cos(p), math.sin(a) * math.cos(p), -math.sin(p))
        right = (math.sin(a), -math.cos(a), 0.0)
        up = (math.cos(a) * math.sin(p), math.sin(a) * math.sin(p), math.cos(p))

        def rect_of(poly):
            vs = []
            for q in poly:
                dq = (q[0] - cam[0], q[1] - cam[1], q[2] - cam[2])
                vs.append((dot(dq, right), dot(dq, up), dot(dq, fwd)))
            # clip to z >= NEAR
            out = []
            for i, v in enumerate(vs):
                u = vs[(i + 1) % len(vs)]
                if v[2] >= NEAR:
                    out.append(v)
                if (v[2] >= NEAR) != (u[2] >= NEAR):
                    t = (NEAR - v[2]) / (u[2] - v[2])
                    out.append(tuple(v[k] + t * (u[k] - v[k]) for k in range(3)))
            if not out:
                return None
            xs = [CX + v[0] * FOCAL / v[2] for v in out]
            ys = [CY - v[1] * FOCAL / v[2] for v in out]
            r = (max(min(xs), 0), max(min(ys), 0), min(max(xs), W), min(max(ys), H))
            return r if r[0] < r[2] and r[1] < r[3] else None

        def raster(poly, fid, zb, idb):
            vs = []
            for q in poly:
                dq = (q[0] - cam[0], q[1] - cam[1], q[2] - cam[2])
                vs.append((dot(dq, right), dot(dq, up), dot(dq, fwd)))
            out = []
            for i, v in enumerate(vs):
                u = vs[(i + 1) % len(vs)]
                if v[2] >= 1.0:
                    out.append(v)
                if (v[2] >= 1.0) != (u[2] >= 1.0):
                    t = (1.0 - v[2]) / (u[2] - v[2])
                    out.append(tuple(v[k] + t * (u[k] - v[k]) for k in range(3)))
            if len(out) < 3:
                return
            pts = [(CX + v[0] * FOCAL / v[2], CY - v[1] * FOCAL / v[2], 1.0 / v[2]) for v in out]
            y0 = max(int(math.ceil(min(p[1] for p in pts) - 0.5)), 0)
            y1 = min(int(math.floor(max(p[1] for p in pts) - 0.5)), H - 1)
            for yy in range(y0, y1 + 1):
                yc = yy + 0.5
                xs = []
                for i, a in enumerate(pts):
                    bb = pts[(i + 1) % len(pts)]
                    if (a[1] <= yc < bb[1]) or (bb[1] <= yc < a[1]):
                        t = (yc - a[1]) / (bb[1] - a[1])
                        xs.append((a[0] + t * (bb[0] - a[0]), a[2] + t * (bb[2] - a[2])))
                if len(xs) < 2:
                    continue
                xs.sort()
                (xa, wa), (xb, wb) = xs[0], xs[-1]
                x0 = max(int(math.ceil(xa - 0.5)), 0)
                x1 = min(int(math.floor(xb - 0.5)), W - 1)
                for xx in range(x0, x1 + 1):
                    t = (xx + 0.5 - xa) / (xb - xa) if xb > xa else 0.0
                    iz = wa + t * (wb - wa)
                    k = yy * W + xx
                    if 1.0 / iz < zb[k]:
                        zb[k] = 1.0 / iz
                        idb[k] = fid

        def inter(r, s):
            q = (max(r[0], s[0]), max(r[1], s[1]), min(r[2], s[2]), min(r[3], s[3]))
            return q if q[0] < q[2] and q[1] < q[3] else None

        # the camera's leaf
        n = b.models[0]["headnode"]
        while n >= 0:
            nd = b.nodes[n]
            pn, pd, _ = b.planes[nd[0]]
            n = nd[1][0] if dot(cam, pn) - pd >= 0 else nd[1][1]
        cl = -1 - n
        cluster = b.leafs[cl]["cluster"]
        row = facevis_row(fv, cluster, nbytes)
        # the flow
        explored = {}
        reached = {}
        stats = [0, 0]                          # portals projected, leaves entered

        def flow(leaf, r, depth):
            rs = explored.setdefault(leaf.leaf, [])
            for s in rs:
                if s[0] <= r[0] and s[1] <= r[1] and s[2] >= r[2] and s[3] >= r[3]:
                    return
            rs.append(r)
            stats[1] += 1
            reached.setdefault(leaf.leaf, []).append(r)
            if depth > 200:
                return
            for pt in leaf.portals:
                other = pt.nodes[1] if pt.nodes[0] is leaf else pt.nodes[0]
                if other.leaf is None or other.leaf < 0 or other.contents & CONTENTS_SOLID:
                    continue
                dist = dot(cam, pt.n) - pt.d
                if abs(dist) < NEAR + 1:
                    pr = r                       # the camera at the portal: all it had
                else:
                    stats[0] += 1
                    pr = rect_of(pt.w)
                    pr = inter(pr, r) if pr else None
                if pr:
                    flow(other, pr, depth + 1)
        if not union_flow:
            flow(leaves[cl], (0, 0, W, H), 0)
        else:
            # one rectangle a leaf (the union of what reaches it), each portal projected once a
            # frame; a leaf goes round again only when its rectangle has grown
            lrect = {cl: (0, 0, W, H)}
            prects = {}
            work = [leaves[cl]]
            queued = {cl}
            while work:
                leaf = work.pop()
                queued.discard(leaf.leaf)
                stats[1] += 1
                r = lrect[leaf.leaf]
                for pt in leaf.portals:
                    other = pt.nodes[1] if pt.nodes[0] is leaf else pt.nodes[0]
                    if other.leaf is None or other.leaf < 0 or other.contents & CONTENTS_SOLID:
                        continue
                    if id(pt) not in prects:
                        dist = dot(cam, pt.n) - pt.d
                        if abs(dist) < NEAR + 1:
                            prects[id(pt)] = (0, 0, W, H)
                        else:
                            stats[0] += 1
                            prects[id(pt)] = rect_of(pt.w)
                    pr = prects[id(pt)]
                    pr = inter(pr, r) if pr else None
                    if not pr:
                        continue
                    o = lrect.get(other.leaf)
                    u = pr if not o else (min(o[0], pr[0]), min(o[1], pr[1]), max(o[2], pr[2]), max(o[3], pr[3]))
                    if u != o:
                        lrect[other.leaf] = u
                        if other.leaf not in queued:
                            queued.add(other.leaf)
                            work.append(other)
            reached = {k: [v] for k, v in lrect.items()}
        cur = [0, 0]
        new = [0, 0]
        lonly = [0, 0]
        for fi, (flags, cells) in enumerate(faces):
            if flags & (1 | 32) or not (row[fi >> 3] >> (fi & 7)) & 1:
                continue
            n, d = face_norm[fi]
            if dot(cam, n) - d <= 0:
                continue
            fr = rect_of(face_polys[fi])
            if not fr:
                continue
            cur[0] += 1
            cur[1] += cells
            if any(inter(fr, s) for li in face_leaves.get(fi, []) for s in reached.get(li, [])):
                new[0] += 1
                new[1] += cells
            if any(li in reached for li in face_leaves.get(fi, [])):
                lonly[0] += 1
                lonly[1] += cells
        if check:
            # (--check: a z-buffer of every face facing the camera: any face with pixels on the
            # screen must be in the portals' set, or they'd cull what's seen)
            zb = [1e30] * (W * H)
            idb = [-1] * (W * H)
            portal_set = set()
            for fi2, (fl2, _) in enumerate(faces):
                if fl2 & (1 | 32) or not (row[fi2 >> 3] >> (fi2 & 7)) & 1:
                    continue
                n2, d2 = face_norm[fi2]
                if dot(cam, n2) - d2 <= 0:
                    continue
                fr2 = rect_of(face_polys[fi2])
                if fr2 and any(inter(fr2, s2) for li in face_leaves.get(fi2, []) for s2 in reached.get(li, [])):
                    portal_set.add(fi2)
            for fi2, (fl2, _) in enumerate(faces):
                if fl2 & (32 | 4 | 8):
                    continue
                n2, d2 = face_norm[fi2]
                if dot(cam, n2) - d2 <= 0:
                    continue
                raster(face_polys[fi2], fi2, zb, idb)
            seen = {}
            for i2 in idb:
                if i2 >= 0:
                    seen[i2] = seen.get(i2, 0) + 1
            missed = [(f2, c2) for f2, c2 in seen.items() if not faces[f2][0] & 1
                      and (row[f2 >> 3] >> (f2 & 7)) & 1 and f2 not in portal_set]
            print("    z-buffer: %d faces seen; in facevis but culled by the portals: %d (%d pixels)"
                  % (len(seen), len(missed), sum(c for _, c in missed)))
        print("  view %d (leaf %d, cluster %d): %d leaves reached; faces %d -> %d, cells %d -> %d (%.0f%%)"
              % (vi + 1, cl, cluster, len(reached), cur[0], new[0], cur[1], new[1], 100.0 * new[1] / max(cur[1], 1)))
        print("    the flow: %d portals projected, %d leaf visits; leaves alone (no rectangle for faces): cells %d (%.0f%%)"
              % (stats[0], stats[1], lonly[1], 100.0 * lonly[1] / max(cur[1], 1)))
        for k, v in enumerate((cur[0], new[0], cur[1], new[1])):
            tot[k] += v
    print("  all: faces %d -> %d, cells %d -> %d (%.0f%%)" % (tot[0], tot[1], tot[2], tot[3], 100.0 * tot[3] / max(tot[2], 1)))


if __name__ == "__main__":
    main()
