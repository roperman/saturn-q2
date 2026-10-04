/*
** (OPT=-DHW_TEST, for a real Saturn: Mednafen's timings proved 1.5-1.7x optimistic, OVERNIGHT.md
** 67) The hardware timing suite: what each kind of memory access, the multiplier and the
** divider, the SCU's DMA and the DSP's DMA cost, and what they cost one another when they run at
** once. Run once at boot (main.c), while everything is quiet: no level loaded (its memory is free
** to test in), the slave waiting, VDP1, the DMA and the DSP idle. HW_BENCH's pages show it
** (ht_page), with the CPUs' profiles of the fight (ht_prof_page, OPT=-DSLAVE_PROF).
**
** A CPU timing: an operation 4,096 times in a hand-written loop (8 a pass), timed by the FRT (a
** tick every 128 cycles), less the bare loop: cycles x 10 an operation. A DMA's: 32 KB (the SCU's)
** or 4,096 words (the DSP's), cycles x 10 a 32-bit word. The CPU, the SCU and the DSP share the
** buses: the last ones time a CPU's misses with the DSP's DMA, the SCU's or the other CPU's
** misses going on.
*/
#include "q2.h"
#include "dsp.h"

#ifdef HW_TEST
#define DSP_PROG_SECTION __attribute__((section(".lwrodata")))
#include "hwtest.h"                         /* (obj/gen: engine/hwtest.dsp, assembled) */

#define ITERS           (512)               /* 8 operations each: 4096 */
#define NT              (52)
#define PER_PAGE        (15)
#define HT_PAGES        ((NT + PER_PAGE - 1) / PER_PAGE)   /* (4: main.c's HB_PAGES counts them) */
#define LWB             ((u8 *)0x00240000)  /* low work RAM, cart, VDP1 VRAM, VDP2 VRAM, sound RAM: */
#define CARTB           ((u8 *)0x02500000)  /* unused at boot */
#define VDP1B           ((u8 *)0x25C60000)
#define VDP2B           ((u8 *)0x25E60000)
#define SNDB            ((u8 *)0x25A70000)
#define UNC(v)          (*(volatile u32 *)UNCACHED(&(v)))

s32                 ht_res[NT];             /* cycles x 10 (a CPU's: an operation; a DMA's: a word) */
static const char   ht_names[NT][14] __attribute__((section(".lwrodata"))) = {
    "LOOP", "HIT", "MISS HW", "MISS LW", "MISS CART", "UNC HW", "UNC LW", "UNC CART", "UNC CART16",
    "RD VDP1", "RD VDP2", "RD SOUND", "ST HW", "ST LW", "ST CART", "ST CART16", "ST VDP1", "ST VDP1 16",
    "ST VDP2", "ST SOUND16",
    "MUL STS", "MUL 2 STS", "DMULS STS", "DMULS 3 STS", "MULSW STS", "DIV WAIT", "DIV 20 READ",
    "DMA HW-VDP1", "DMA CART-HW", "DMA CART-VDP1", "DMA VDP1-HW",
    "DSP RD HW", "DSP RD CART", "DSP WR HW", "DSP WR VDP1", "DSP RD VDP1",
    "MISS HW+DSPRH", "MISS HW+DSPWH", "MISSCRT+DSPRC", "MISS HW+DSPRC", "MISS HW+DMA",
    "HIT+DMA", "MISS HW+SLVH", "MISSCRT+SLVC", "MISS HW+SLVC",
    "MISSH+DSPWH16", "MISSH+DSPWH8", "MISSH+DSPRH16", "MISSC+DSPRC16", "MISSH+SLVLW", "MISSL+SLVLW", "MISSL+SLVH",
};
static u8           *hwb;                   /* high work RAM: 64 KB to read, 64 KB a DMA's destination,
                                               128 KB the background's (a DMA's source, the DSP's, the slave's) */
u32                 ht_slave_req, ht_slave_busy;    /* (the slave's job: 1 misses in high work RAM, 2 the cart) */
static u8           *ht_slave_area[4];   /* (the slave's job's: 1 high work RAM, 2 the cart, 3 low) */

/* the loop: 8 of op, ITERS times (op sees the address as %0, the value as %2) */
#define LOOP8(op)                                                                                   \
    __asm__ volatile("mov   %1,r2\n"                                                               \
                     "1:\n" op op op op op op op op                                                  \
                     "dt    r2\n"                                                                    \
                     "bf    1b\n"                                                                    \
                     : "+r"(a) : "r"(ITERS), "r"(b) : "r0", "r1", "r2", "r3", "macl", "mach", "memory", "t")
#define N20             "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"

/* a CPU timing (FRT ticks for the 4,096) */
static u32          ht_cpu(int k)
{
    const u8        *a = hwb;
    u32             b = 0, t;

    switch (k)
    {
    case 1:
        b = *(volatile const u32 *)a;       /* (its line in the cache) */
        t = frt_read();
        LOOP8("mov.l @%0,r0\n");
        break;
    case 2: case 3: case 4:                 /* misses: a new line each */
    case 36: case 37: case 38: case 39: case 40: case 42: case 43: case 44:
    case 45: case 46: case 47: case 48: case 49: case 50: case 51:
        a = k == 4 || k == 38 || k == 43 || k == 48 ? CARTB : k == 3 || k == 50 || k == 51 ? LWB : hwb;
        cache_purge();
        t = frt_read();
        LOOP8("mov.l @%0,r0\n add #16,%0\n");
        break;
    case 41:                                /* hits, a DMA going */
        b = *(volatile const u32 *)a;
        t = frt_read();
        LOOP8("mov.l @%0,r0\n");
        break;
    case 5: case 6: case 7: case 9: case 10: case 11:
        a = k == 5 ? (const u8 *)UNCACHED(hwb) : k == 6 ? (const u8 *)UNCACHED(LWB) : k == 7 ? (const u8 *)UNCACHED(CARTB)
            : k == 9 ? VDP1B : k == 10 ? VDP2B : SNDB;
        t = frt_read();
        LOOP8("mov.l @%0,r0\n");
        break;
    case 8:
        a = (const u8 *)UNCACHED(CARTB);
        t = frt_read();
        LOOP8("mov.w @%0,r0\n");
        break;
    case 12: case 13: case 14: case 16: case 18:    /* stores (the value there: harmless) */
        a = k == 12 ? hwb : k == 13 ? LWB : k == 14 ? CARTB : k == 16 ? VDP1B : VDP2B;
        b = *(volatile const u32 *)UNCACHED(a);
        t = frt_read();
        LOOP8("mov.l %2,@%0\n");
        break;
    case 15: case 17: case 19:
        a = k == 15 ? CARTB : k == 17 ? VDP1B : SNDB;
        b = *(volatile const u16 *)UNCACHED(a);
        t = frt_read();
        LOOP8("mov.w %2,@%0\n");
        break;
    case 20:
        b = 12345;
        t = frt_read();
        LOOP8("mul.l %2,%2\n sts macl,r0\n");
        break;
    case 21:
        b = 12345;
        t = frt_read();
        LOOP8("mul.l %2,%2\n nop\n nop\n sts macl,r0\n");
        break;
    case 22:
        b = 12345;
        t = frt_read();
        LOOP8("dmuls.l %2,%2\n sts mach,r0\n");
        break;
    case 23:
        b = 12345;
        t = frt_read();
        LOOP8("dmuls.l %2,%2\n nop\n nop\n nop\n sts mach,r0\n");
        break;
    case 24:
        b = 1234;
        t = frt_read();
        LOOP8("muls.w %2,%2\n sts macl,r0\n");
        break;
    case 25:                                /* a divide: start it, read the result */
        a = (const u8 *)0xFFFFFF00;
        b = 1000;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n mov #0,r0\n mov.l r0,@(16,%0)\n mov.l %2,@(20,%0)\n mov.l @(20,%0),r0\n");
        break;
    case 26:                                /* ...20 instructions between */
        a = (const u8 *)0xFFFFFF00;
        b = 1000;
        t = frt_read();
        LOOP8("mov.l %2,@%0\n mov #0,r0\n mov.l r0,@(16,%0)\n mov.l %2,@(20,%0)\n" N20 "mov.l @(20,%0),r0\n");
        break;
    default:                                /* the bare loop */
        t = frt_read();
        LOOP8("");
        break;
    }
    return (frt_read() - t) & 0xFFFF;
}

/* the DSP: one of engine/hwtest.dsp's from pc, at addr, n DMAs of 64 words (the counted ones) */
static void         ht_dsp(int pc, const void *addr, u32 n)
{
    DSP_PPAF = 0;
    DSP_PDA = 0;
    DSP_PDD = ((u32)addr & 0x07FFFFFF) >> 2;
    DSP_PDD = n;
    DSP_PPAF = (1u << 16) | (1u << 15) | (u32)pc;
}

static void         ht_dsp_stop(void)
{
    DSP_PPAF = 0;
    while (SCU_DSTA & 3)
        ;                                   /* (its DMA finished) */
}

/* the slave's job (slave_main, when ht_slave_req's set): misses over 64 KB, again and again, till
   it's cleared */
void                ht_slave_run(void)
{
    u32             kind = UNC(ht_slave_req);
    const u8        *base = (const u8 *)*(u8 *volatile *)UNCACHED(&ht_slave_area[kind]), *p;
    u32             sum = 0;

    UNC(ht_slave_busy) = 1;
    while (UNC(ht_slave_req))
        for (p = base; p < base + 65536; p += 16)
            sum += *(volatile const u32 *)p;
    UNC(ht_slave_busy) = 0;
    (void)sum;
}

static void         slave_go(u32 kind)
{
    UNC(ht_slave_req) = kind;
    signal_slave();
    while (!UNC(ht_slave_busy))
        ;
}

static void         slave_stop(void)
{
    UNC(ht_slave_req) = 0;
    while (UNC(ht_slave_busy))
        ;
}

/* a timing with something else going on: the DSP's, the SCU's or the slave's */
static u32          ht_under(int k)
{
    u32             t;

    switch (k)
    {
    case 36:
        ht_dsp(HWTEST_PROG_RDLOOP, hwb + 131072, 0);
        break;
    case 37:
        ht_dsp(HWTEST_PROG_WRLOOP, hwb + 131072, 0);
        break;
    case 38:
    case 39:
        ht_dsp(HWTEST_PROG_RDLOOP, CARTB + 0x100000, 0);
        break;
    case 40:
    case 41:
        scu_dma0(VDP1B, hwb + 131072, 131072, true);
        break;
    case 42:
        slave_go(1);
        break;
    case 43:
    case 44:
        slave_go(2);
        break;
    case 45:                                /* (shorter bursts: does a miss wait out the DSP's burst?) */
        ht_dsp(HWTEST_PROG_WRLOOP16, hwb + 131072, 0);
        break;
    case 46:
        ht_dsp(HWTEST_PROG_WRLOOP8, hwb + 131072, 0);
        break;
    case 47:
        ht_dsp(HWTEST_PROG_RDLOOP16, hwb + 131072, 0);
        break;
    case 48:
        ht_dsp(HWTEST_PROG_RDLOOP16, CARTB + 0x100000, 0);
        break;
    case 49:                                /* (low work RAM: the faces, cells and lights are there) */
    case 50:
        slave_go(3);
        break;
    case 51:
        slave_go(1);
        break;
    }
    t = ht_cpu(k);
    if ((k >= 36 && k <= 39) || (k >= 45 && k <= 48))
        ht_dsp_stop();
    else if (k <= 41)
        while (scu_dma0_busy())
            ;
    else
        slave_stop();
    return t;
}

void                ht_run(void)
{
    u32             base, t;
    int             k;

    extern u8       _bss_end[];             /* (no level yet: high work RAM free from .bss's end) */

    hwb = (u8 *)(((u32)_bss_end + 1023) & ~1023u);
    ht_slave_area[1] = hwb + 131072;
    ht_slave_area[2] = CARTB + 0x100000;
    ht_slave_area[3] = (u8 *)0x00280000;    /* (low work RAM, past what the master reads) */
    cart_timing();                          /* (the cart on: level.c's cart_init does it at a level's load) */
    base = ht_cpu(0);
    for (k = 0; k < NT; ++k)
    {
        if (k <= 26)
        {
            t = ht_cpu(k);
            ht_res[k] = (s32)((k ? t - base : t) * FRT_DIV * 10 / (ITERS * 8));
        }
        else if (k <= 30)
        {
            /* the SCU's DMA, 32 KB */
            static const u8 dsts[4] = { 0, 1, 0, 1 };   /* VDP1, high work RAM */
            const void *src = k == 27 ? (const void *)hwb : k == 30 ? (const void *)VDP1B : (const void *)CARTB;

            t = frt_read();
            scu_dma0(dsts[k - 27] ? (void *)(hwb + 65536) : (void *)VDP1B, src, 32768, !dsts[k - 27]);
            while (scu_dma0_busy())
                ;
            t = (frt_read() - t) & 0xFFFF;
            ht_res[k] = (s32)(t * FRT_DIV * 10 / 8192);
        }
        else if (k <= 35)
        {
            /* the DSP's DMA, 64 x 64 words */
            static const u8 pcs[5] = { HWTEST_PROG_RDN, HWTEST_PROG_RDN, HWTEST_PROG_WRN, HWTEST_PROG_WRBN,
                                       HWTEST_PROG_RDN };
            const void *at = k == 31 ? (const void *)hwb : k == 32 ? (const void *)CARTB : k == 33 ? (const void *)(hwb + 65536)
                             : (const void *)VDP1B;

            if (k == 31)
            {
                int i;

                DSP_PPAF = 0;
                DSP_PPAF = 1u << 15;
                for (i = 0; i < HWTEST_PROG_LEN; ++i)
                    DSP_PPD = hwtest_prog[i];
            }
            t = frt_read();
            ht_dsp(pcs[k - 31], at, 64);
            {
                int r;

                for (r = 0; r < 1000 && !(DSP_PPAF & (1u << 16)); ++r)
                    ;                       /* (on a Saturn the busy flag isn't up at once: wait for it) */
            }
            while (DSP_PPAF & (1u << 16))
                ;
            t = (frt_read() - t) & 0xFFFF;
            ht_res[k] = (s32)(t * FRT_DIV * 10 / 4096);
        }
        else
        {
            t = ht_under(k);
            ht_res[k] = (s32)((t - base) * FRT_DIV * 10 / (ITERS * 8));
        }
    }
    DSP_PPAF = 0;
    dsp_init_models();                      /* (its program back) */
}

/* a page of the results: 15 a page, from y */
void                ht_page(int page, int y)
{
    u16             w = RGB(255, 255, 255), hd = RGB(255, 220, 120);
    int             k;

    vdp_printf(8, y, hd, "TIMINGS %d/%d: CYCLES (DMA: A WORD)", page + 1, HT_PAGES);
    y += 12;
    for (k = page * PER_PAGE; k < page * PER_PAGE + PER_PAGE && k < NT; ++k, y += 10)
        vdp_printf(8, y, w, "%-13s %4d.%d", ht_names[k], ht_res[k] / 10, (ht_res[k] < 0 ? -ht_res[k] : ht_res[k]) % 10);
}

#endif

#ifdef SLAVE_PROF
/* (OPT="-DHW_BENCH -DSLAVE_PROF") a CPU's profile of the fight on the screen: its samples (main.c's
   prof_isr: one each 16,384 cycles, by 8 bytes of high work RAM's code) summed by function, by
   cd/SYMS.BIN's names (tools/mksyms.py), the most first: ms a frame and the share. And its hot spots:
   the top functions' busiest 8 bytes, as offsets into each (an instruction's samples land on the
   one after it, or on the one a stall holds up: tools/mksyms.py's listing finds them) */
#define PROF_TOP        (12)
#define HOT_FUNCS       (6)
#define HOT_SPOTS       (4)
#define SYM_ADDR(i)     (*(const u32 *)(syms + 4 + (i) * 24))
static u8           *syms;
static int          nsyms;
static u32          *fsum;

/* each function's samples (fsum), the total, and the n busiest */
static int          prof_rank(const volatile u16 *h, u32 *total, int *top, int n)
{
    int             i, k, j;

    if (!syms)
    {
        int m;

        syms = cart_alloc(32768);
        m = cd_load("SYMS.BIN", syms, 32768);
        nsyms = m > 4 ? (int)*(const u32 *)syms : 0;
        fsum = (u32 *)cart_alloc((u32)(nsyms + 1) * 4);
    }
    for (i = 0; i < nsyms; ++i)
    {
        u32 lo = SYM_ADDR(i), hi = i + 1 < nsyms ? SYM_ADDR(i + 1) : 0x06024000, sum = 0, b;

        for (b = (lo - 0x06004000) >> 3; b < (hi - 0x06004000) >> 3 && b < 16384; ++b)
            sum += h[b];
        fsum[i] = sum;
        *total += sum;
    }
    for (k = 0; k < n; ++k)
    {
        top[k] = -1;
        for (i = 0; i < nsyms; ++i)
        {
            for (j = 0; j < k && top[j] != i; ++j)
                ;
            if (j == k && (top[k] < 0 || fsum[i] > fsum[top[k]]))
                top[k] = i;
        }
    }
    return nsyms;
}

static void         sym_name(int i, char *name)
{
    int             k;

    for (k = 0; k < 15 && syms[8 + i * 24 + k]; ++k)
        name[k] = (char)(syms[8 + i * 24 + k] >= 'a' && syms[8 + i * 24 + k] <= 'z' ? syms[8 + i * 24 + k] - 32
                         : syms[8 + i * 24 + k]);
    name[k] = 0;
}

void                ht_prof_page(int cpu, u32 frames, int y)
{
    const volatile u16 *h = (const volatile u16 *)(cpu ? 0x202F0000 : 0x202F8000);     /* (master's, slave's) */
    u32             other = *(volatile u32 *)(cpu ? 0x202E7FFC : 0x202E7FF8), total = other;
    u16             w = RGB(255, 255, 255), hd = RGB(255, 220, 120);
    int             top[PROF_TOP], k;
    char            name[16];

    prof_rank(h, &total, top, PROF_TOP);
    if (!frames)
        frames = 1;
    vdp_printf(8, y, hd, "%s: %d SAMPLES, ELSEWHERE %d", cpu ? "MASTER" : "SLAVE", total, other);
    y += 12;
    if (!nsyms)
    {
        vdp_text(8, y, w, "NO SYMS.BIN");
        return;
    }
    for (k = 0; k < PROF_TOP && top[k] >= 0; ++k, y += 10)
    {
        u32 s = fsum[top[k]], v = s * 16384u / 2685u * 10 / frames;     /* (26.85 MHz: the fight's 0.1 ms; a frame's 0.01) */

        sym_name(top[k], name);
        vdp_printf(8, y, w, "%-15s %2d.%d MS %2d%%", name, v / 100, v / 10 % 10, total ? s * 100 / total : 0);
    }
}

void                ht_hot_page(int cpu, int y)
{
    const volatile u16 *h = (const volatile u16 *)(cpu ? 0x202F0000 : 0x202F8000);
    u32             total = 0;
    u16             w = RGB(255, 255, 255), hd = RGB(255, 220, 120), g = RGB(160, 255, 160);
    int             top[HOT_FUNCS], k;
    char            name[16];

    prof_rank(h, &total, top, HOT_FUNCS);
    vdp_printf(8, y, hd, "%s HOT SPOTS: OFFSET (HEX) SAMPLES", cpu ? "MASTER" : "SLAVE");
    y += 12;
    for (k = 0; k < HOT_FUNCS && top[k] >= 0; ++k)
    {
        u32 lo = SYM_ADDR(top[k]), hi = top[k] + 1 < nsyms ? SYM_ADDR(top[k] + 1) : 0x06024000;
        u32 b0 = (lo - 0x06004000) >> 3, b1 = (hi - 0x06004000) >> 3, b;
        int best[HOT_SPOTS], j, i;
        char line[48], *o = line;

        sym_name(top[k], name);
        vdp_printf(8, y, g, "%s %d AT %X", name, fsum[top[k]], lo);
        y += 9;
        for (j = 0; j < HOT_SPOTS; ++j)
        {
            best[j] = -1;
            for (b = b0; b < b1 && b < 16384; ++b)
            {
                for (i = 0; i < j && best[i] != (int)b; ++i)
                    ;
                if (i == j && h[b] && (best[j] < 0 || h[b] > h[best[j]]))
                    best[j] = (int)b;
            }
            if (best[j] >= 0)
                o += fmt(o, "%X:%d ", (u32)best[j] * 8 + 0x06004000 - lo, h[best[j]]);
        }
        *o = 0;
        vdp_text(8, y, w, line);
        y += 11;
    }
}
#endif
