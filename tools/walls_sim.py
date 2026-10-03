#!/usr/bin/env python3
"""The walls' dynamic lights' DSP programs (engine/walls0.dsp, walls1.dsp, walls2.dsp) run in
tools/dspsim.py on a level's own data, against Python copies of src/render.c's dw_filter (what
walls0.dsp picks) and dw_model (what walls1 and walls2 work out): as OPT=-DWALLS_TEST does on the
Saturn, the same faces (the fitting ones near where you start) and the same three lights.

    python3 tools/walls_sim.py [demo1 demo2 demo3] [--dist 400] [--walls cd/WALLS.BIN]

(after ./build.sh: the baked levels in cd/, the programs in cd/WALLS.BIN). Each level's line: the
faces listed, whether they're dw_filter's, the points, the faces and points different, and the DSP's
instructions and (roughly) cycles. The data RAM starts with junk in it, as the models' job leaves it.
"""
import argparse
import os
import random
import struct

from dspsim import DSP, Mem, s32, load_bin

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CART = 0x02400000                   # (where the level goes, as on the Saturn)
PROGS = 0x06000000                  # (the rest anywhere else)
BUF = 0x06010000
MAXF = 60                           # (walls0.dsp's)


def FIX(x):
    return x << 16


class Level:
    """a baked level (tools/bake_map.py; src/level.c's layout)"""
    def __init__(self, path):
        b = open(path, "rb").read()
        self.b = b
        h = struct.unpack_from(">64I", b, 12)
        self.N = struct.unpack_from(">H", b, 10)[0]
        self.planes_off = h[0]
        self.faces_off, self.nfaces = h[8], h[9]
        self.lights_off = h[12]
        self.axes_off = h[46]
        self.start = struct.unpack_from(">3i", b, h[24])

    def face(self, fi):
        o = self.faces_off + 32 * fi
        org = struct.unpack_from(">3i", self.b, o)
        axes, plane = struct.unpack_from(">HH", self.b, o + 12)
        flags, nu, nv, eu0, eu1, ev0, ev1, _ = struct.unpack_from(">8B", self.b, o + 16)
        firstlight = struct.unpack_from(">I", self.b, o + 28)[0]
        return dict(org=org, axes=axes, plane=plane, flags=flags, nu=nu, nv=nv, eu0=eu0, eu1=eu1, ev0=ev0, ev1=ev1,
                    firstlight=firstlight & 0xFFFFFF)

    def axis(self, a):
        return struct.unpack_from(">6i", self.b, self.axes_off + 24 * a)

    def plane(self, p):
        return struct.unpack_from(">4i", self.b, self.planes_off + 20 * p)

    def light(self, k):
        return struct.unpack_from(">H", self.b, self.lights_off + 2 * k)[0]


def lights_tables(ls):
    """src/render.c's dw_lights: ls, (pos[3], radius, r, g, b) each -> walls2.dsp's, walls0.dsp's"""
    lt = [0] * 32
    lt0 = [0] * 33
    for k, (pos, radius, r, g, b) in enumerate(ls):
        rad = radius >> 16
        r2 = max(rad * rad, 4)
        reach = radius + FIX(2)
        for c in range(3):
            lt[k * 8 + c] = -pos[c]
            lt0[k * 10 + c] = pos[c]
            lt0[k * 10 + 4 + c] = pos[c] + reach
            lt0[k * 10 + 7 + c] = reach - pos[c]
        lt[k * 8 + 3] = r2
        lt[k * 8 + 4] = -(((1 << 24) // r2) << 8)
        lt[k * 8 + 5:k * 8 + 8] = [r, g, b]
        lt0[k * 10 + 3] = radius
    lt[24:32] = lt[0:8]
    lt0[30:33] = [FIX(8), 5, 6]
    return lt, lt0


def dw_filter(lv, fi, lt0, nl):
    """src/render.c's: the lights (bits) walls0.dsp picks for face fi"""
    f = lv.face(fi)
    n0, n1, n2, dist = lv.plane(f["plane"])
    ax = lv.axis(f["axes"])
    s = -1 if f["flags"] & 64 else 1
    mp = m = 0
    for l in range(nl):
        L = lt0[l * 10:l * 10 + 10]
        nd = s32((n0 * L[0] + n1 * L[1] + n2 * L[2]) >> 16)
        md = s32((dist - nd) * s)
        if s32(L[3] + md) > 0 and s32(FIX(8) - md) > 0:
            mp |= 1 << l
    if not mp:
        return 0
    lo, hi = [0] * 3, [0] * 3
    for c in range(3):
        A = s32(ax[c] * f["nu"])
        B = s32(ax[3 + c] * f["nv"])
        M = max(A, 0) + max(B, 0)
        lo[c] = s32(f["org"][c] + A + B - M)
        hi[c] = s32(f["org"][c] + M)
    for l in range(nl):
        L = lt0[l * 10:l * 10 + 10]
        if mp & 1 << l and all(s32(L[4 + c] - lo[c]) >= 0 and s32(L[7 + c] + hi[c]) >= 0 for c in range(3)):
            m |= 1 << l
    return m


def dw_model(lv, fi, lt, mask):
    """src/render.c's: face fi's lit lights with the lights of mask"""
    f = lv.face(fi)
    ax = lv.axis(f["axes"])
    nu, nv, N = f["nu"], f["nv"], lv.N
    rcp = 65536 // N
    dut = [s32((ax[c] * rcp) >> 16) for c in range(3)]
    dvt = [s32((ax[3 + c] * rcp) >> 16) for c in range(3)]
    A = []
    for i in range(nu + 1):
        a = 0 if i == 0 else (nu - 1) * N - f["eu0"] + f["eu1"] if i == nu else i * N - f["eu0"]
        A.append([s32(a * dut[c]) for c in range(3)])
    out = []
    k = 0
    for j in range(nv + 1):
        b = 0 if j == 0 else (nv - 1) * N - f["ev0"] + f["ev1"] if j == nv else j * N - f["ev0"]
        R = [s32(f["org"][c] + s32(b * dvt[c])) for c in range(3)]
        for i in range(nu + 1):
            v = lv.light(f["firstlight"] + k) & 0x7FFF
            s = [v & 31, v >> 5 & 31, v >> 10 & 31]
            P = [s32(R[c] + A[i][c]) for c in range(3)]
            for l in range(3):
                if not mask & 1 << l:
                    continue
                L = lt[l * 8:l * 8 + 8]
                d2 = s32(sum(((P[c] + L[c]) >> 16) ** 2 for c in range(3)))
                if s32(d2 - L[3]) >= 0:
                    continue
                fw = s32(((d2 - L[3]) * L[4]) >> 16)
                for c in range(3):
                    s[c] += s32((fw * L[5 + c]) >> 16)
            out.append(0x8000 + min(s[0], 31) + 32 * min(s[1], 31) + 1024 * min(s[2], 31))
            k += 1
    return out


def run_chain(lv, level_bytes, faces, ls, walls_bin, trace=None, seed=1, odd=False):
    """walls0 -> walls1 -> walls2 -> the models' program's end, as the Saturn chains them: the host's
    numbers as src/render.c's dw_level and dw_params set them. -> (dsp, instructions, walls1.dsp's words
    (index << 16 | where its lit lights are), the lit lights' words, walls2's lights, walls0's). The
    list: the faces' indices, two to a word (odd: starting in the second half of the first)"""
    mem = Mem()
    mem.add(CART, level_bytes)
    mem.add(PROGS, open(walls_bin, "rb").read())
    lt, lt0 = lights_tables(ls)
    nl = len(ls)
    at = {}
    addr = BUF
    for name, words in (("blocks", 2400), ("out", 640), ("list", len(faces) + 1), ("acc", 1 + 2 * MAXF), ("lt", 32),
                        ("lt0", 33)):
        at[name] = addr
        mem.add(addr, bytes(4 * words))
        addr += 4 * words + 64
    hw = [0x7777] * odd + list(faces) + [0x7777]
    for i in range(0, len(hw) - 1, 2):
        mem.w32(at["list"] + 2 * i, hw[i] << 16 | hw[i + 1])
    for i, w in enumerate(lt):
        mem.w32(at["lt"] + 4 * i, w)
    for i, w in enumerate(lt0):
        mem.w32(at["lt0"] + 4 * i, w)

    def sh(a):
        return (a & 0x07FFFFFF) >> 2

    # RAM0[40..63]: engine/dsp.h's dsp_walls_p
    params = [len(faces), 0, sh(at["blocks"]), sh(at["out"]), sh(at["lt"]), 32 if nl == 3 else 8 * nl, nl - 1,
              sh(CART + lv.axes_off), sh(CART + lv.lights_off), lv.N, 65536 // lv.N, sh(PROGS + 3072),
              sh(PROGS + 1024), 0, sh(CART + lv.planes_off), sh(PROGS), 0, sh(PROGS + 2048), sh(at["list"]),
              len(faces), sh(CART + lv.faces_off), int(odd), sh(at["acc"]), sh(at["lt0"])]
    d = DSP(mem)
    d.trace = trace
    rnd = random.Random(seed)
    for b in range(4):
        for i in range(64):
            d.ram[b][i] = rnd.getrandbits(32)       # (what the models' job leaves)
    for i, w in enumerate(params):
        d.ram[0][40 + i] = w & 0xFFFFFFFF
    if odd:
        d.ram[0][25] = mem.r32(at["list"])          # (the host's dsp_walls_word: the first word,
        d.ram[0][58] += 1                           # ...and the list from the next)
    d.prog = load_bin(walls_bin)[768:1024]          # (WALLS.BIN: walls1, walls2, the models', walls0)
    d.start(0)
    n = d.run()
    acc = [mem.r32(at["acc"] + 4 + 4 * k) for k in range(mem.r32(at["acc"]))]
    outw = [mem.r32(at["out"] + 4 * k) for k in range(640)]
    return d, n, acc, outw, lt, lt0


def near_faces(lv, limit=512, dist=400):
    """walls_test's: the faces walls2.dsp has room for (not water) near where you start"""
    out = []
    for fi in range(lv.nfaces):
        f = lv.face(fi)
        if not (f["nu"] < 12 and f["nv"] < 24 and (f["nu"] + 1) * (f["nv"] + 1) <= 62) or f["flags"] & 2:
            continue
        if any(abs((f["org"][i] - lv.start[i]) >> 16) > dist for i in range(3)):
            continue
        out.append(fi)
        if len(out) == limit:
            break
    return out


def test_lights(lv):
    """walls_test's three lights, by where you start"""
    at = [(0, 0, 40), (120, 0, 30), (0, -120, 50)]
    col = [(13, 11, 5), (5, 13, 11), (31, 20, 0)]
    return [([lv.start[i] + FIX(at[k][i]) for i in range(3)], FIX(160 + 60 * k)) + col[k] for k in range(3)]


def check(lv, lvb, faces, ls, walls_bin, odd=False):
    d, n, acc, outw, lt, lt0 = run_chain(lv, lvb, faces, ls, walls_bin, odd=odd)
    want = [fi for fi in faces if dw_filter(lv, fi, lt0, len(ls))][:MAXF]
    listed = [w >> 16 for w in acc]
    nbadf = nbadp = npts = off_want = 0
    for w in acc:
        fi, off = w >> 16, w & 0xFFFF
        f = lv.face(fi)
        np_ = (f["nu"] + 1) * (f["nv"] + 1)
        ref = dw_model(lv, fi, lt, dw_filter(lv, fi, lt0, len(ls)))
        got = []
        for k in range((np_ + 1) // 2):
            got += [outw[off + k] >> 16, outw[off + k] & 0xFFFF]
        bad = sum(got[k] != ref[k] for k in range(np_)) + (off != off_want) * np_
        nbadf += bad != 0
        nbadp += bad
        npts += np_
        off_want += (np_ + 1) // 2
    return dict(listed=len(acc), same_picks=listed == want[:len(listed)], points=npts, bad_faces=nbadf,
                bad_points=nbadp, instructions=n, cycles=d.cycles)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("levels", nargs="*", default=["demo1", "demo2", "demo3"])
    ap.add_argument("--dist", type=int, default=400, help="the faces within this of where you start (units)")
    ap.add_argument("--walls", default=os.path.join(ROOT, "cd", "WALLS.BIN"))
    a = ap.parse_args()
    for m in a.levels:
        path = os.path.join(ROOT, "cd", m.upper() + ".MAP")
        lv = Level(path)
        faces = near_faces(lv, dist=a.dist)
        for odd in (False, True):
            print(m, len(faces), "faces", "(odd start)" if odd else "",
                  check(lv, open(path, "rb").read(), faces, test_lights(lv), a.walls, odd))
