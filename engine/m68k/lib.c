/*
** The 68000 has no 32-bit multiply or divide, and the cross-compiler's
** libgcc is built for the 68020 - so here are the helpers gcc calls for
** 32-bit '*', '/' and '%'. Shift-and-add, nothing clever: the driver
** only needs them a few times per song.
*/
typedef unsigned long   u32;
typedef long            s32;

u32                     __mulsi3(u32 a, u32 b)
{
    u32                 r = 0;

    while (b)
    {
        if (b & 1)
            r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

static u32              udivmod(u32 n, u32 d, u32 *rem)
{
    u32                 q = 0, bit = 1;

    if (d == 0)
    {
        *rem = n;
        return 0xFFFFFFFF;
    }
    while (d < n && !(d & 0x80000000))
    {
        d <<= 1;
        bit <<= 1;
    }
    while (bit)
    {
        if (n >= d)
        {
            n -= d;
            q |= bit;
        }
        d >>= 1;
        bit >>= 1;
    }
    *rem = n;
    return q;
}

u32                     __udivsi3(u32 n, u32 d) { u32 r; return udivmod(n, d, &r); }
u32                     __umodsi3(u32 n, u32 d) { u32 r; udivmod(n, d, &r); return r; }

s32                     __divsi3(s32 n, s32 d)
{
    u32                 r, q = udivmod(n < 0 ? -n : n, d < 0 ? -d : d, &r);

    return ((n < 0) != (d < 0)) ? -(s32)q : (s32)q;
}

s32                     __modsi3(s32 n, s32 d)
{
    u32                 r;

    udivmod(n < 0 ? -n : n, d < 0 ? -d : d, &r);
    return n < 0 ? -(s32)r : (s32)r;
}
