#!/usr/bin/env python3
"""The CPUs' profiles (a build with OPT="-DFIGHT_BENCH -DSLAVE_PROF"): after the
fight, save Mednafen's state (F5), then
    [CPU=master] tools/prof.py [state.mc0] [game.elf]
reads the slave's samples (or the master's: low work RAM's top 64 KB, a u16 for
each 8 bytes of high work RAM's code, one sample each 16,384 cycles) and prints
the functions they fell in, the most first, and each one's busiest addresses."""
import glob, gzip, os, subprocess, sys

MASTER = os.environ.get("CPU") == "master"
HIST, OTHER = (0x0F0000, 0x0E7FFC) if MASTER else (0x0F8000, 0x0E7FF8)
CODE, BUCKET = 0x06004000, 8
FRAMES, CYCLES, HZ = 502, 16384, 26.8e6

state = sys.argv[1] if len(sys.argv) > 1 else max(
    glob.glob(os.path.expanduser("~/.mednafen/mcs/*.mc0")), key=os.path.getmtime)
elf = sys.argv[2] if len(sys.argv) > 2 else "game.elf"

d = gzip.open(state).read()
i = d.index(b"\x08WorkRAML") + 9
size = int.from_bytes(d[i:i + 4], "little")
raw = d[i + 4:i + 4 + size]
lw = bytearray(size)
lw[0::2], lw[1::2] = raw[1::2], raw[0::2]          # (kept as host-order u16s)

counts = [int.from_bytes(lw[HIST + 2 * k:HIST + 2 * k + 2], "big") for k in range(0x8000 // 2)]
other = int.from_bytes(lw[OTHER:OTHER + 4], "big")
syms = []
for line in subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tT":
        syms.append((int(parts[0], 16), parts[2].lstrip("_")))

def owner(a):
    lo, hi = 0, len(syms) - 1
    if not syms or a < syms[0][0]:
        return "?"
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if syms[mid][0] <= a:
            lo = mid
        else:
            hi = mid - 1
    return syms[lo][1]

total = sum(counts) + other
per = {}
for k, n in enumerate(counts):
    if n:
        f = owner(CODE + k * BUCKET)
        per.setdefault(f, []).append((n, CODE + k * BUCKET))
ms = lambda n: n * CYCLES / HZ * 1000 / FRAMES
print(f"{'master' if MASTER else 'slave'}: {total} samples ({ms(total):.1f} ms a frame); low work RAM's code {other} ({ms(other):.2f} ms)")
for f, b in sorted(per.items(), key=lambda kv: -sum(n for n, _ in kv[1]))[:int(os.environ.get("TOP", 30))]:
    n = sum(x for x, _ in b)
    hot = " ".join(f"{a:08X}:{c}" for c, a in sorted(b, reverse=True)[:4])
    print(f"{100 * n / total:5.1f}%  {ms(n):5.2f} ms  {f:28s} {hot}")
