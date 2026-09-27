/*
** What things cost on this SH-2 (in the emulator we measure with): cycles
** per operation, each timed over thousands in hand-written loops (the FRT
** ticks every 128 cycles). Run once at boot; the stats overlay shows them.
*/
#include "q2.h"

s32                 cyc[12];                /* cycles x 10 per operation, as named in cyc_names */
const char          *cyc_names[12] = { "LOOP", "ST HW", "ST LW", "LD HIT", "LD UNC", "MISS HW", "MISS LW",
                                       "MISS CART", "DIVU", "DMULS", "ST CART", "ST VRAM1" };

#define ITERS           (512)               /* 8 operations each: 4096 */

static u32          buf[16] __attribute__((aligned(16)));

/* the loop: 8 of op, ITERS times (op sees the address as %0, the value as %2) */
#define LOOP8(op)                                                                                   \
    __asm__ volatile("mov   %1,r2\n"                                                               \
                     "1:\n" op op op op op op op op                                                  \
                     "dt    r2\n"                                                                    \
                     "bf    1b\n"                                                                    \
                     : "+r"(a) : "r"(ITERS), "r"(b) : "r0", "r1", "r2", "r3", "macl", "mach", "memory", "t")

static u32          timed_ticks(int kind)
{
    const u8        *a;
    u32             b = 0, t;

    switch (kind)
    {
    case 1:     /* stores to HWRAM (the value it has: harmless) */
        a = (const u8 *)buf;
        b = *(volatile const u32 *)a;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n");
        break;
    case 2:     /* to LWRAM (the same, over level data) */
        a = (const u8 *)0x00280000;
        b = *(volatile const u32 *)a;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n");
        break;
    case 3:     /* loads that hit */
        a = (const u8 *)buf;
        t = frt_read();
        LOOP8("mov.l @%0,r0\n");
        break;
    case 4:     /* loads through the uncached mirror */
        a = (const u8 *)UNCACHED(buf);
        t = frt_read();
        LOOP8("mov.l @%0,r0\n");
        break;
    case 5:     /* misses: a new line each (64 KB of HWRAM, the program's own) */
    case 6:     /* LWRAM */
    case 7:     /* the cart */
        a = kind == 5 ? (const u8 *)0x06004000 : kind == 6 ? (const u8 *)0x00200000 : (const u8 *)0x02400000;
        cache_purge();
        t = frt_read();
        LOOP8("mov.l @%0,r0\n add #16,%0\n");
        break;
    case 8:     /* a divide: start it, read the result */
        a = (const u8 *)0xFFFFFF00;
        b = 1000;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n mov #0,r0\n mov.l r0,@(16,%0)\n mov.l %2,@(20,%0)\n mov.l @(20,%0),r0\n");
        break;
    case 9:     /* a 32x32 -> 64 multiply and the top half */
        a = (const u8 *)buf;
        b = 12345;
        t = frt_read();
        LOOP8("dmuls.l %2,%2\n sts mach,r0\n");
        break;
    case 10:    /* stores to the cart (its last word: free) */
        a = (const u8 *)(0x02400000 + 0x3FFFF0);
        b = *(volatile const u32 *)a;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n");
        break;
    case 11:    /* to VDP1's VRAM (its last word) */
        a = (const u8 *)(0x25C7FFF0);
        b = *(volatile const u32 *)a;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n");
        break;
    default:    /* the bare loop */
        a = (const u8 *)buf;
        t = frt_read();
        LOOP8("");
        break;
    }
    return (frt_read() - t) & 0xFFFF;
}

void                cycles_measure(void)
{
    extern u32      render_bench_grid(void);
    u32             base = timed_ticks(0);
    int             k;

    for (k = 0; k < 12; ++k)
    {
        u32 t = timed_ticks(k);

        /* less the loop */
        if (k)
            t -= base;
        cyc[k] = (s32)(t * FRT_DIV * 10 / (ITERS * 8));
    }
    cyc[0] = (s32)(render_bench_grid() * FRT_DIV * 10 / (512 * 8));    /* a grid point, in place of the loop */
}
