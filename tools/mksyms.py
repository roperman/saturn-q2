#!/usr/bin/env python3
"""The code's symbols as a file on the disc (cd/SYMS.BIN), for the profile OPT="-DHW_BENCH
-DSLAVE_PROF" shows on a real Saturn's screen: which function each sample fell in.

    tools/mksyms.py game.elf cd/SYMS.BIN

The functions (and assembly labels) in high work RAM's code, sorted by address: a big-endian
count, then each one's address and its name (the C's, without the leading _), 20 bytes, NUL-padded.
"""
import struct
import sys

LO, HI = 0x06004000, 0x06024000         # (high work RAM's code: the profiler's 128 KB)
NAME = 20


def symbols(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
        sys.exit("not a 32-bit big-endian ELF: " + path)
    shoff, = struct.unpack_from(">I", d, 32)
    shentsize, shnum = struct.unpack_from(">HH", d, 46)
    secs = [struct.unpack_from(">IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] != 2:                   # SHT_SYMTAB
            continue
        strtab = secs[s[6]]             # (its sh_link)
        so, ss = strtab[4], strtab[5]
        for k in range(s[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", d, s[4] + 16 * k)
            kind = info & 15
            if kind not in (0, 2) or not LO <= value < HI:     # NOTYPE (assembly labels), FUNC
                continue
            end = d.index(b"\0", so + name)
            n = d[so + name:end].decode("ascii", "replace")
            if not n or n.startswith(".") or n.startswith("L") and n[1:2].isdigit():
                continue
            yield value, n[1:] if n.startswith("_") else n, kind == 2, info >> 4 == 1
    return


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    best = {}
    for addr, name, func, glob in symbols(sys.argv[1]):
        rank = (func, glob)                         # (a function's name over a label's at the same place)
        if addr not in best or rank > best[addr][0]:
            best[addr] = (rank, name)
    out = bytearray(struct.pack(">I", len(best)))
    for addr in sorted(best):
        out += struct.pack(">I", addr) + best[addr][1].encode("ascii", "replace")[:NAME - 1].ljust(NAME, b"\0")
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()
