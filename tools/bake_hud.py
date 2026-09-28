#!/usr/bin/env python3
"""Quake 2's status bar pictures for the Saturn (src/hud.c): 8-bit, Quake's palette.

    tools/bake_hud.py data/pak0.pak cd/HUD.BIN

The palette goes into VDP2 colour RAM and the pictures are VDP1 sprites in
256-colour bank mode, so they come out in Quake's exact colours. Colour 0 is
transparent there; Quake uses 255, so the two are swapped.
Layout: "Q2HD", u16 count, u16 pad, palette[256] (RGB555), then per picture
name[12], u16 w (a multiple of 8), u16 h, u32 offset; then the pixels.
"""
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2data import Pak, palette

PICS = ["num_%d" % i for i in range(10)] + ["num_minus"] + ["anum_%d" % i for i in range(10)] + ["anum_minus"] + \
       ["i_health", "i_jacketarmor", "i_combatarmor", "i_bodyarmor", "a_shells", "a_bullets", "a_grenades",
        "a_rockets", "a_blaster", "p_quad", "i_help"] + \
       ["m_main_game", "m_main_game_sel", "m_main_options", "m_main_options_sel", "m_main_plaque", "m_main_logo",
        "pause"] + ["m_cursor%d" % i for i in range(0, 15, 2)]     # (the menus: every other cursor frame, for VRAM)


def main():
    pak = Pak(sys.argv[1])
    pal = palette(pak)
    table, data = [], bytearray()
    for name in PICS:
        img = Image.open(io.BytesIO(pak.read("pics/%s.pcx" % name)))
        w, h = img.size
        pw = (w + 7) & ~7
        px = list(img.getdata())
        out = bytearray()
        for y in range(h):
            row = [255 if v == 0 else 0 if v == 255 else v for v in px[y * w:(y + 1) * w]]
            out += bytes(row + [0] * (pw - w))
        table.append((name, pw, h, len(data)))
        data += out
        while len(data) & 7:
            data.append(0)
    cols = []
    for i in range(256):
        c = pal[0] if i == 255 else pal[i]
        cols.append(((c[2] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[0] >> 3))
    hdr = b"Q2HD" + struct.pack(">2H", len(table), 0) + struct.pack(">256H", *cols)
    tab = b"".join(struct.pack(">12s2HI", n.encode(), w, h, o) for n, w, h, o in table)
    with open(sys.argv[2], "wb") as f:
        f.write(hdr + tab + bytes(data))
    print("%s: %d pictures, %d bytes" % (sys.argv[2], len(table), len(data)))


if __name__ == "__main__":
    main()
