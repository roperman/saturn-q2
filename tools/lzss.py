#!/usr/bin/env python3
"""LZSS for the asset pack: tiny to decode on the SH-2 (engine/lzss.c).

Stream: a flag byte, then 8 items, LSB first. Flag bit 1 = a literal byte;
0 = a match: two bytes, big endian, offset (1..4096) in the top 12 bits
minus one, length (3..18) in the low 4 bits minus three.
Header: "LZ" u16 0, u32 decoded size (big endian).
"""
import struct
import sys

WINDOW = 4096
MIN_MATCH, MAX_MATCH = 3, 18


def compress(data):
    out = bytearray(b"LZ\0\0" + struct.pack(">I", len(data)))
    heads = {}              # 3-byte key -> recent positions (newest last)
    i, n = 0, len(data)
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_off = 0, 0
            if i + MIN_MATCH <= n:
                key = data[i:i + 3]
                for p in reversed(heads.get(key, ())):
                    if i - p > WINDOW:
                        break
                    l = 3
                    while l < MAX_MATCH and i + l < n and data[p + l] == data[i + l]:
                        l += 1
                    if l > best_len:
                        best_len, best_off = l, i - p
                        if l == MAX_MATCH:
                            break
            if best_len >= MIN_MATCH:
                out += struct.pack(">H", ((best_off - 1) << 4) | (best_len - MIN_MATCH))
                step = best_len
            else:
                flags |= 1 << bit
                out.append(data[i])
                step = 1
            for k in range(i, min(i + step, n - 2)):
                lst = heads.setdefault(data[k:k + 3], [])
                lst.append(k)
                if len(lst) > 48:
                    del lst[:16]
            i += step
        out[flag_pos] = flags
    return bytes(out)


def decompress(buf):
    size = struct.unpack(">I", buf[4:8])[0]
    out = bytearray()
    i = 8
    while len(out) < size:
        flags = buf[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (1 << bit):
                out.append(buf[i])
                i += 1
            else:
                v = (buf[i] << 8) | buf[i + 1]
                i += 2
                off, l = (v >> 4) + 1, (v & 15) + MIN_MATCH
                for _ in range(l):
                    out.append(out[-off])
    return bytes(out)


if __name__ == "__main__":
    src = open(sys.argv[1], "rb").read()
    packed = compress(src)
    assert decompress(packed) == src
    open(sys.argv[2], "wb").write(packed)
    print(f"{sys.argv[1]}: {len(src)} -> {len(packed)} bytes ({100 * len(packed) // len(src)}%)")
