/*
** Fixed-point helpers: trig, square roots, a random number source.
*/
#include "q2.h"
#include "sintab.h"

s32                 fsin(int a)
{
    return sintab[(a >> 4) & 4095];
}

s32                 fcos(int a)
{
    return sintab[((a >> 4) + 1024) & 4095];
}

/* atan(r) for 0 <= r <= 1 (16.16), as a 16-bit angle: pi/4 r + 0.273 r (1 - r),
   good to about a quarter of a degree */
static int          atan01(s32 r)
{
    return (int)((8192 * (s64)r + 2847 * (((s64)r * (65536 - r)) >> 16)) >> 16);
}

int                 fatan2(s32 x, s32 z)
{
    s32             ax = iabs(x), az = iabs(z);
    int             a;

    if (ax == 0 && az == 0)
        return 0;
    a = ax <= az ? atan01(fdiv(ax, az)) : 0x4000 - atan01(fdiv(az, ax));
    if (z < 0)
        a = 0x8000 - a;
    return (x < 0 ? -a : a) & 0xFFFF;
}

u32                 isqrt(u32 v)
{
    u32             r = 0, b = 1u << 30;

    while (b > v)
        b >>= 2;
    while (b)
    {
        if (v >= r + b)
        {
            v -= r + b;
            r = (r >> 1) + b;
        }
        else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* the SH-2 divider: 16.16 a / b; the quotient must fit 32 bits */
s32                 fdiv(s32 a, s32 b)
{
    if (b == 0)
        return a < 0 ? -0x7FFFFFFF : 0x7FFFFFFF;
    divu_start(a >> 16, (u32)a << 16, b);
    return divu_result();
}

static u32          rng_state = 0x2545F491;

void                rng_seed(u32 s)
{
    rng_state = s ? s : 1;
}

u32                 rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
