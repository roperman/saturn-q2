#!/usr/bin/env python3
"""The disc image as BIN/CUE with whole 2,352-byte sectors, for optical drive emulators (SAROO
and the like) and burning: build.sh's game.iso holds only each sector's 2,048 bytes of data.

    tools/mkbin.py game.iso out_dir [name]

Writes out_dir/name.bin and out_dir/name.cue (name: "Quake II" if not given). Each sector is
CD-ROM Mode 1 (ECMA-130): the sync pattern, its address (minutes, seconds, frames from 00:02:00)
and mode, the 2,048 bytes, the EDC (a CRC of all that), 8 zero bytes, and the Reed-Solomon P and
Q parity, computed as Neill Corlett's ECM does.
"""
import os
import struct
import sys

# GF(2^8) (x^8 + x^4 + x^3 + x^2 + 1) for the parity, and the EDC's CRC table
ECC_F = [0] * 256
ECC_B = [0] * 256
EDC = [0] * 256
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF
    ECC_F[i] = j
    ECC_B[i ^ j] = i
    e = i
    for _ in range(8):
        e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    EDC[i] = e


def edc(data):
    e = 0
    for b in data:
        e = (e >> 8) ^ EDC[(e ^ b) & 0xFF]
    return e


def ecc_block(src, major_count, minor_count, major_mult, minor_inc):
    """the parity of src (the sector from its header on): major_count pairs of bytes"""
    size = major_count * minor_count
    out = bytearray(2 * major_count)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        out[major] = a
        out[major + major_count] = a ^ b
    return out


def bcd(n):
    return (n // 10) << 4 | n % 10


def sector(lba, data):
    s = bytearray(2352)
    s[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
    f = lba + 150
    s[12:16] = bytes((bcd(f // 4500), bcd(f // 75 % 60), bcd(f % 75), 1))
    s[16:2064] = data
    struct.pack_into("<I", s, 0x810, edc(s[0:0x810]))
    s[0x81C:0x8C8] = ecc_block(s[12:], 86, 24, 2, 86)        # P
    s[0x8C8:0x930] = ecc_block(s[12:], 52, 43, 86, 88)       # Q (over P too)
    return s


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.split("\n\n")[1])
    iso, out = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) == 4 else "Quake II"
    data = open(iso, "rb").read()
    if len(data) % 2048:
        data += bytes(2048 - len(data) % 2048)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name + ".bin"), "wb") as f:
        for lba in range(len(data) // 2048):
            f.write(sector(lba, data[lba * 2048:(lba + 1) * 2048]))
    with open(os.path.join(out, name + ".cue"), "w", newline="\r\n") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n' % name)
    print("%s: %d sectors" % (os.path.join(out, name + ".bin"), len(data) // 2048))


if __name__ == "__main__":
    main()
