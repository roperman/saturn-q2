#!/usr/bin/env python3
"""Code loaded per level (build.sh build_overlays): the same code linked at two bases, base A
and base A + 0x10000; the words that differ by that are addresses in it, the loader's to move
(src/g_main.c g_overlays_load). The file, big-endian:
    "Q2OV", base A, the image's size, how many words to move
    the image (linked at A)
    the words' offsets in it
    tools/overlay.py a.bin b.bin base_a out.ovl"""
import struct
import sys

a, b = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
base = int(sys.argv[3], 0)
if len(a) != len(b) or len(a) % 4:
    sys.exit("overlay: the two links differ in size")
rel = []
for k in range(0, len(a), 4):
    wa, wb = struct.unpack_from(">I", a, k)[0], struct.unpack_from(">I", b, k)[0]
    if wa == wb:
        continue
    if (wb - wa) & 0xFFFFFFFF != 0x10000:
        sys.exit("overlay: a word at %d differs by %x" % (k, (wb - wa) & 0xFFFFFFFF))
    rel.append(k)
with open(sys.argv[4], "wb") as f:
    f.write(b"Q2OV" + struct.pack(">3I", base, len(a), len(rel)) + a + struct.pack(">%dI" % len(rel), *rel))
print("%s: %d bytes, %d to move" % (sys.argv[4], len(a), len(rel)))
