/*
** LZSS decoder for packs made by tools/lzss.py (format described there).
*/
#include "sat.h"

u32                 lzss_size(const u8 *src)
{
    return ((u32)src[4] << 24) | ((u32)src[5] << 16) | ((u32)src[6] << 8) | src[7];
}

u32                 lzss_decode(const u8 *src, u8 *dst)
{
    u32             size = lzss_size(src), n = 0;
    const u8        *s = src + 8;
    int             bit;
    u8              flags;

    while (n < size)
    {
        flags = *s++;
        for (bit = 0; bit < 8 && n < size; ++bit, flags >>= 1)
        {
            if (flags & 1)
                dst[n++] = *s++;
            else
            {
                u32 v = ((u32)s[0] << 8) | s[1];
                u32 off = (v >> 4) + 1, len = (v & 15) + 3;
                const u8 *from = dst + n - off;

                s += 2;
                while (len-- && n < size)
                    dst[n++] = *from++;
            }
        }
    }
    return size;
}
