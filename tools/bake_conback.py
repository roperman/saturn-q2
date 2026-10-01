#!/usr/bin/env python3
"""Quake 2's console background (pics/conback.pcx), shown behind the loading screens (src/main.c
loading_back): a VDP2 bitmap, 8-bit, Quake's palette (as Quake 2 draws its pictures).

    tools/bake_conback.py data/pak0.pak cd/CONBACK.BIN

Layout: the bitmap as VDP2 reads it, 512 x 256 bytes (read straight into its VRAM): the picture's
middle 224 rows (the screen's) at the top left; at row 240 (never shown) the palette, 256 RGB555
(big-endian). Colour 0 is transparent there; the picture doesn't use it.
"""
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2data import Pak, palette

W, H, SCREEN_H = 512, 256, 224


def main():
    pak = Pak(sys.argv[1])
    pal = palette(pak)
    img = Image.open(io.BytesIO(pak.read("pics/conback.pcx")))
    w, h = img.size
    px = list(img.getdata())
    assert 0 not in px, "conback uses colour 0 (transparent)"
    top = (h - SCREEN_H) // 2
    out = bytearray(W * H)
    for y in range(SCREEN_H):
        out[y * W:y * W + w] = bytes(px[(top + y) * w:(top + y + 1) * w])
    out[240 * W:240 * W + 512] = struct.pack(">256H", *(((c[2] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[0] >> 3)
                                                       for c in pal))
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print("%s: %d bytes" % (sys.argv[2], len(out)))


if __name__ == "__main__":
    main()
