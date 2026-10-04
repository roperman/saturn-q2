#!/usr/bin/env python3
"""The texture maker's DSP program (engine/make.dsp) run in tools/dspsim.py on random tiles,
masks and crops, against a Python copy of src/render.c's tex_make (what the CPU makes when the
DSP can't): the host's block and lists laid out as src/render.c's mk_* do.

    python3 tools/make_sim.py [--jobs 200] [--seed 1] [--bin obj/make.bin] [--end-at N]

(after ./build.sh, or python3 tools/dspasm.py engine/make.dsp obj/gen/make.h make_prog
obj/make.bin). --end-at: the host's end flag set after that many jobs are done, as a frame's
end would: the program must stop taking jobs, write how many it took, and load the models'
program (here one that stops at once).
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dspsim import DSP, Mem, load_bin        # noqa: E402

CART = 0x02400000
HWRAM = 0x06040000
VRAM = 0x05C40000                               # (VDP1's: the slots, on the B-bus)
JOBS, JOBW = 16, 14
BLOCK_WORDS = 256 + 7 + 16 + 2 * JOBS * JOBW     # (the program first)
NIB4 = [0x0000, 0xF000, 0x0F00, 0xFF00, 0x00F0, 0xF0F0, 0x0FF0, 0xFFF0,
        0x000F, 0xF00F, 0x0F0F, 0xFF0F, 0x00FF, 0xF0FF, 0x0FFF, 0xFFFF]


class ShiftMem(Mem):
    """a DMA read of the cart come in a word late, the rest shifted (as seen on a Saturn with
    SAROO's cart): reads of the ranges in .bad (base, words) give a stale word first, then
    each word the one before it"""
    def __init__(self):
        super().__init__()
        self.bad = {}
        self.cur = None

    def r32(self, addr):
        addr &= 0x07FFFFFF
        for base, n in self.bad.items():
            if base <= addr < base + 4 * n:
                return 0x5EA1F00D if addr == base else super().r32(addr - 4)
        return super().r32(addr)


def tex_make(kind, tile, mask, x0, y0, w, h):
    """src/render.c's tex_make: the texture's bytes (kind 2 quartered, 3 a masked crop)"""
    out = bytearray()
    if kind == 3:
        wb = w >> 1
        for y in range(h):
            s = tile[(y0 + y) * 8 + (x0 >> 1):]
            m = mask[y]                                 # (the mask's rows are the crop's)
            for k in range(0, wb, 4):
                if x0 & 1:
                    v = ((s[k] << 4 | s[k + 1] >> 4) & 255) << 24 | ((s[k + 1] << 4 | s[k + 2] >> 4) & 255) << 16 \
                        | ((s[k + 2] << 4 | s[k + 3] >> 4) & 255) << 8 | ((s[k + 3] << 4 | s[k + 4] >> 4) & 255)
                else:
                    v = s[k] << 24 | s[k + 1] << 16 | s[k + 2] << 8 | s[k + 3]
                v &= NIB4[(m >> (k * 2)) & 15] << 16 | NIB4[(m >> (k * 2 + 4)) & 15]
                out += struct.pack(">I", v)
    else:
        for y in range(32):
            o = ((y >> 4) * 8 + (y & 7)) * 8 + ((y >> 3) & 1) * 4
            out += tile[o:o + 4]
    return bytes(out)


def job_words(tile, mask, slot, kind, x0, y0, w, h, tb, mb):
    """src/render.c's mk_job: a job's 14 words (tb, mb: the tile's and the mask's bytes, for the checks)"""
    sh = lambda a: (a & 0x07FFFFFF) >> 2
    w32 = lambda b, i: struct.unpack_from(">I", b, 4 * i)[0]
    tx = w32(tb, 0) ^ w32(tb, 31)
    if kind != 3:
        return [sh(tile), sh(tile), sh(slot), 1, 0, 0, 0, 0, 0, 0, 0, 0, tx, 0]
    k, ww = 4 * x0, w // 8
    win = 1 if ww == 1 and 0 < x0 < 8 else 0
    src = 16 + 2 * y0 + (1 if x0 == 8 else 0)
    return [sh(tile), sh(mask), sh(slot), 0, 0, h, src, ww, win, k - 1 if win else 0, 31 - k if win else 0,
            (1 << k) - 1 if win else 0, tx, w32(mb, 0) ^ w32(mb, 7)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--bin", default="obj/make.bin")
    ap.add_argument("--end-at", type=int, default=-1)
    ap.add_argument("--shift", type=int, default=-1,
                    help="this job's tile (or, odd: mask) read comes in a word late")
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    a.jobs = min(a.jobs, 2 * JOBS)

    # work RAM: the block (the program, then the host's), then the models' program; the cart: the
    # tiles and masks; VRAM: the slots
    block = HWRAM
    p0 = block + BLOCK_WORDS * 4
    tiles = CART
    masks = tiles + a.jobs * 128
    ring = VRAM
    mem = ShiftMem()
    mem.add(HWRAM, bytes(p0 + 1024 - HWRAM))
    mem.add(CART, bytes(a.jobs * 160))
    mem.add(VRAM, bytes(a.jobs * 128))
    for i, w in enumerate(load_bin(a.bin)):
        mem.w32(block + 4 * i, w)                 # (the host copies it to its block's start)
    for i in range(256):
        mem.w32(p0 + 4 * i, 0xF0000000)         # (the models' program: END)
    for i in range(16):
        mem.w32(block + (256 + 7 + i) * 4, NIB4[i] << 16)

    jobs = []
    counts = [0, 0]
    for j in range(a.jobs):
        tile = bytes(rnd.getrandbits(8) for _ in range(128))
        mask = [rnd.getrandbits(16) if rnd.random() < 0.8 else 0xFFFF for _ in range(16)]
        kind = 3 if rnd.random() < 0.8 else 2
        w = rnd.choice([8, 16])
        h = rnd.randint(1, 16)
        x0 = rnd.randint(0, 8) if w == 8 else 0
        y0 = rnd.randint(0, 16 - h)
        lst = rnd.randint(0, 1)
        if counts[lst] == JOBS:
            lst ^= 1
            if counts[lst] == JOBS:
                break
        ta, ma, sa = tiles + j * 128, masks + j * 32, ring + j * 128
        for i in range(32):
            mem.w32(ta + 4 * i, struct.unpack_from(">I", tile, 4 * i)[0])
        for i in range(8):
            mem.w32(ma + 4 * i, mask[2 * i] << 16 | mask[2 * i + 1])
        for i in range(32):
            mem.w32(sa + 4 * i, 0xEEEEEEEE)
        mb = b"".join(struct.pack(">I", mask[2 * i] << 16 | mask[2 * i + 1]) for i in range(8))
        words = job_words(ta, ma, sa, kind, x0, y0, w, h, tile, mb)
        if j == a.shift // 2 if a.shift >= 0 else False:
            if a.shift & 1 and kind == 3:
                mem.bad[ma] = 8
            else:
                mem.bad[ta] = 32
        at = block + (256 + 23 + lst * JOBS * JOBW + counts[lst] * JOBW) * 4
        for i, w_ in enumerate(words):
            mem.w32(at + 4 * i, w_)
        counts[lst] += 1
        ref = tex_make(kind, tile, mask, x0, y0, w, h)
        jobs.append((lst, counts[lst] - 1, kind, x0, y0, w, h, sa, ref))
    head = block + 256 * 4
    mem.w32(head, counts[0])
    mem.w32(head + 4, counts[1])

    d = DSP(mem)
    for b in range(4):
        for i in range(64):
            d.ram[b][i] = rnd.getrandbits(32)       # (what the walls' job leaves)
    d.ram[0][56] = (block & 0x07FFFFFF) >> 2
    d.ram[0][57] = (p0 & 0x07FFFFFF) >> 2
    d.prog = load_bin(a.bin)
    assert len(d.prog) == 256, len(d.prog)
    d.start(0)
    # run till it's idle (the lists done), or till end-at jobs are done, then the end flag
    done = lambda: (mem.r32(head + 12), mem.r32(head + 16))
    n = 0
    stop_at = a.end_at if a.end_at >= 0 else len(jobs)
    per_job = []
    last = d.cycles
    taken = 0
    while not d.ended and n < 20_000_000:
        d.step()
        n += 1
        if mem.r32(head + 20) != 1:
            continue                            # (not going yet: its start zeroes the counts)
        t = d.ram[0][17] + d.ram[0][18]
        if t != taken:
            per_job.append(d.cycles - last)
            last = d.cycles
            taken = t
        if (taken >= stop_at or d.ram[0][20]) and not mem.r32(head + 8):     # (or it's halted: I_HALT)
            mem.w32(head + 8, 1)
    if not d.ended:
        print("FAIL: didn't end after", n, "steps")
        return 1
    d0, d1 = done()
    if mem.r32(head + 20) != 2:
        print("FAIL: the state word's not 2 (done):", mem.r32(head + 20))
        return 1
    print(f"jobs {len(jobs)} (list0 {counts[0]}, list1 {counts[1]}), end after {stop_at}: the DSP took {d0} + {d1}, "
          f"{n} instructions")
    if a.end_at < 0 and a.shift < 0 and (d0, d1) != tuple(counts):
        print("FAIL: not all taken")
        return 1
    if a.shift >= 0:
        sj = jobs[a.shift // 2] if a.shift // 2 < len(jobs) else None
        if sj and sj[1] < (d0, d1)[sj[0]]:
            print("FAIL: the shifted read's job counted as made")
            return 1
        if sj and any(mem.r32(sj[7] + 4 * i) != 0xEEEEEEEE for i in range(32)):
            print("FAIL: the shifted read's slot written")
            return 1
        if mem.r32(head + 24) != 1:
            print("FAIL: the halt word not written for the host")
            return 1
        print(f"  the shifted read's job (list {sj[0]} job {sj[1]}) given back, its slot untouched, halt said")
    bad = 0
    cyc = {2: [], 3: []}
    for (lst, idx, kind, x0, y0, w, h, sa, ref) in jobs:
        if idx >= (d0, d1)[lst]:
            continue                            # (the host's to make)
        got = b"".join(struct.pack(">I", mem.r32(sa + 4 * i)) for i in range(32))[:len(ref)]
        if got != ref:
            bad += 1
            if bad <= 5:
                print(f"  MISMATCH list {lst} job {idx}: kind {kind} x0 {x0} y0 {y0} {w}x{h}")
                print("   ref", ref.hex())
                print("   got", got.hex())
    # (the first per-job span includes the start; the rest one job each, in the lists' order)
    order = sorted(jobs, key=lambda j: (j[0], j[1]))
    taken_jobs = [j for j in order if j[1] < (d0, d1)[j[0]]]
    for j, c in zip(taken_jobs[1:], per_job[1:]):
        cyc[j[2]].append(c)
    for kind in (3, 2):
        if cyc[kind]:
            print(f"  kind {kind}: {len(cyc[kind])} jobs, {min(cyc[kind])}..{max(cyc[kind])} cycles, "
                  f"mean {sum(cyc[kind]) // len(cyc[kind])} (DMA 8 a word)")
    print("FAIL" if bad else "OK", f"{bad} wrong")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
