/*
** Bare-metal runtime: memory helpers, tiny formatter, timer, vblank, pad, SCU DMA.
*/
#include <stdarg.h>
#include "sat.h"

void                *memset(void *d, int c, u32 n)
{
    u8              *p = d;

    while (n--)
        *p++ = (u8)c;
    return d;
}

void                *memcpy(void *d, const void *s, u32 n)
{
    u8              *p = d;
    const u8        *q = s;

    if ((((u32)p | (u32)q | n) & 3) == 0)
    {
        u32 *pw = d;
        const u32 *qw = s;

        for (n >>= 2; n; --n)
            *pw++ = *qw++;
        return d;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

/* %d %u %x %s %c; flags '-' and '0', width, and .precision for %s */
int                 memcmp(const void *a, const void *b, u32 n)
{
    const u8        *p = a, *q = b;

    for (; n; --n, ++p, ++q)
        if (*p != *q)
            return *p - *q;
    return 0;
}

int                 vfmt(char *out, const char *f, va_list ap)
{
    char            *o = out, tmp[12];
    int             width, prec, zero, left, i, n;
    u32             v;
    bool            neg;

    for (; *f; ++f)
    {
        if (*f != '%')
        {
            *o++ = *f;
            continue;
        }
        ++f;
        left = zero = 0;
        for (;; ++f)
        {
            if (*f == '-')
                left = 1;
            else if (*f == '0')
                zero = 1;
            else
                break;
        }
        width = 0;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        prec = -1;
        if (*f == '.')
        {
            prec = 0;
            ++f;
            while (*f >= '0' && *f <= '9')
                prec = prec * 10 + (*f++ - '0');
        }
        neg = false;
        n = 0;
        switch (*f)
        {
            case 'd':
            {
                s32 sv = va_arg(ap, s32);

                if (sv < 0)
                {
                    neg = true;
                    v = (u32)-sv;
                }
                else
                    v = (u32)sv;
                do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
                break;
            }
            case 'u':
                v = va_arg(ap, u32);
                do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
                break;
            case 'x':
            case 'X':
                v = va_arg(ap, u32);
                do { tmp[n++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
                break;
            case 's':
            {
                const char *s = va_arg(ap, const char *);
                int len = 0;

                while (s[len] && (prec < 0 || len < prec))
                    ++len;
                if (!left)
                    for (i = len; i < width; ++i)
                        *o++ = ' ';
                for (i = 0; i < len; ++i)
                    *o++ = s[i];
                if (left)
                    for (i = len; i < width; ++i)
                        *o++ = ' ';
                continue;
            }
            case 'c':
                *o++ = (char)va_arg(ap, int);
                continue;
            default:
                *o++ = *f;
                continue;
        }
        if (neg)
            tmp[n++] = '-';
        if (!left)
            for (i = n; i < width; ++i)
                *o++ = zero ? '0' : ' ';
        for (i = n; i > 0; --i)
            *o++ = tmp[i - 1];
        if (left)
            for (i = n; i < width; ++i)
                *o++ = ' ';
    }
    *o = 0;
    return (int)(o - out);
}

int                 fmt(char *out, const char *f, ...)
{
    va_list         ap;
    int             n;

    va_start(ap, f);
    n = vfmt(out, f, ap);
    va_end(ap);
    return n;
}

/* ---- FRT: clock/128 (FRT_DIV) ---- */

void                frt_init(void)
{
    FRT_TCR = (u8)((FRT_TCR & ~3) | 2);
    /* The BIOS leaves the input-capture interrupt enabled. We use input
       capture as the doorbell between the two SH-2s and poll it, so once any
       interrupt is unmasked it would fire for every signal - and the BIOS
       handler doesn't clear ICF, so it fires forever. */
    FRT_TIER = 0x01;
}

u32                 frt_read(void)
{
    u32             h = FRT_FRCH;       /* reading H latches L */

    return (h << 8) | FRT_FRCL;
}

/* ticks of clock/128 -> microseconds; 320-wide modes run the SH-2 at ~26.8 MHz */
u32                 frt_to_us(u32 ticks)
{
    return (ticks * (FRT_DIV * 1000u / 64u)) / (26847u / 64u);
}

void                wait_vblank_in(void)
{
    while (VDP2_TVSTAT & 8)
        ;
    while (!(VDP2_TVSTAT & 8))
        ;
}

void                wait_vblank_out(void)
{
    while (!(VDP2_TVSTAT & 8))
        ;
    while (VDP2_TVSTAT & 8)
        ;
}

/* ---- SMPC INTBACK: peripheral data only, both ports in one batch ---- */

/* Pad 1. The SMPC takes milliseconds over an INTBACK (5.7 in Mednafen), so
   a game loop asks at the end of a frame (pad_request, just after the swap)
   and picks the answer up at the start of the next (pad_collect), while
   the CPUs get on with other things. pad_read does both, waiting.

   The SMPC calls a read off if a vblank starts during it (Mednafen too), and
   leaves its last answer in place. With the swap by interrupt (vdp.c) that
   was every read: the swap's at line 216, a frame that waited for it asked
   right then, 8 lines before the vblank, and once the frames fell into step
   with it the pad stood still for good (what was held stayed held, nothing
   else came through: the game looked hung). So with the interrupts on
   (pad_by_vblank), the end of each vblank asks (at its start is no good in
   Mednafen: a read asked for before the SMPC's seen the vblank is called off
   at once) and the SMPC reads at the top of the picture; pad_collect takes
   the last answer it finished (the one before, if a read's under way). */
static bool         by_vblank;
static u16          pad_last;

static void         intback(void)
{
    SMPC_SF = 1;
    SMPC_IREG(0) = 0x00;        /* no SMPC status */
    SMPC_IREG(1) = 0x0A;        /* peripheral data, 15-byte mode, no time optimisation */
    SMPC_IREG(2) = 0xF0;
    SMPC_COMREG = 0x10;         /* INTBACK */
}

void                pad_request(void)
{
    if (by_vblank)
        return;
    while (SMPC_SF & 1)
        ;
    intback();
}

/* (the vblank-out interrupt, vdp.c) */
void                pad_vblank(void)
{
    if (by_vblank && !(SMPC_SF & 1))
        intback();
}

/* on: the vblank-out interrupt's running (vdp_set_pipelined) and nothing else asks the SMPC for
   anything (snd_bank holds it off). What it was */
bool                pad_by_vblank(bool on)
{
    bool            was = by_vblank;

    by_vblank = on;
    while (SMPC_SF & 1)
        ;                               /* (one asked for already: its answer in) */
    return was;
}

u16                 pad_collect(void)
{
    u16             buttons = 0;

    if (by_vblank)
    {
        if (SMPC_SF & 1)
            return pad_last;
    }
    else
        while (SMPC_SF & 1)
            ;
    /* port 1 direct: OREG0 = 0xF1 (1 device), OREG1 = 0x02 (digital pad) */
    if (SMPC_OREG(0) == 0xF1 && (SMPC_OREG(1) >> 4) == 0x0)
        buttons = (u16)~((SMPC_OREG(2) << 8) | SMPC_OREG(3));
    pad_last = (u16)(buttons & 0xFFF8);
    return pad_last;
}

u16                 pad_read(void)
{
    pad_request();
    return pad_collect();
}

/* ---- SCU DMA level 0, CPU-triggered ---- */

/* level 0 under way: moving, on standby (waiting for the bus: behind the DSP's DMA, say, or the
   CPU's writes to VDP1/VDP2) or held back by a higher level (ST-097 DSTA: it's stopped only when
   D0MV, D0WT and D0BK are all 0). Its registers mustn't be written until it's stopped: on hardware
   that hangs the machine (ST-210 No. 23). Mednafen never shows the standby, so the move flag
   alone passed there */
#define D0_ACTIVE       (0x00010030)

/* a chain of transfers started by scu_dma0_table (vdp_submit's lists) and not yet seen
   finished: nothing else starts until it has */
volatile bool       scu_dma0_chain;
#define CHAIN           (*(volatile bool *)UNCACHED(&scu_dma0_chain))   /* (seen the same by both CPUs) */

#ifdef BOOT_TRACE
void                (*step_hook)(int step);
#endif

#ifdef NO_AB_DMA
/* (OPT=-DNO_AB_DMA, a test) nothing sent by SCU DMA from the A-bus (the cart) to the B-bus (VRAM):
   while such a transfer runs neither CPU may touch either bus (ST-210 No. 08: SDRAM's refresh stops
   and it may hang), and both do. The CPU copies it instead, through the uncached addresses */
bool                on_abus(u32 a)
{
    a &= 0x07FFFFFF;
    return a >= 0x02000000 && a < 0x05900000;
}

void                ab_copy(u32 dst, u32 src, u32 bytes)
{
    dst = (dst & 0x07FFFFFF) | 0x20000000;
    src = (src & 0x07FFFFFF) | 0x20000000;
    if (!((dst | src | bytes) & 3))
    {
        volatile u32    *d = (volatile u32 *)dst;
        const volatile u32 *s = (const volatile u32 *)src;

        for (; bytes; bytes -= 4)
            *d++ = *s++;
    }
    else
    {
        volatile u16    *d = (volatile u16 *)dst;
        const volatile u16 *s = (const volatile u16 *)src;

        for (; bytes >= 2; bytes -= 2)
            *d++ = *s++;
    }
}
#endif

/* Both CPUs start level 0 transfers (the slave its uploads, and with CMD_RING its lists' pieces):
   one at a time, under a lock taken with tas.b (an uncached read-modify-write), held from the wait
   for the level to be idle until the new transfer shows as active (the flag isn't up the cycle
   after the enable). Writing a level's registers while it's active hangs the Saturn (ST-210) */
static u8           dma_lock_byte;
static inline void  dma_lock(void)
{
    volatile u8     *l = (volatile u8 *)UNCACHED(&dma_lock_byte);
    u32             got;

    do
        __asm__ volatile ("tas.b @%1\n\tmovt %0" : "=r" (got) : "r" (l) : "t", "memory");
    while (!got);
}
static inline void  dma_unlock(void)
{
    *(volatile u8 *)UNCACHED(&dma_lock_byte) = 0;
}
static inline void  dma_started(void)
{
    int             i;

    for (i = 0; i < 64 && !(SCU_DSTA & D0_ACTIVE); ++i)
        ;
}
void                scu_dma0(void *dst, const void *src, u32 bytes, bool bbus_dst)
{
#ifdef NO_AB_DMA
    if (bbus_dst && on_abus((u32)src))
    {
        ab_copy((u32)dst, (u32)src, bytes);
        return;
    }
#endif
    STEP(80);
    dma_lock();
    while (SCU_DSTA & D0_ACTIVE)
        ;                                   /* (a chain under way: not cut short) */
    STEP(81);
    CHAIN = false;
    SCU_D0EN = 0;
    STEP(82);
    SCU_D0R = (u32)src & 0x07FFFFFF;
    SCU_D0W = (u32)dst & 0x07FFFFFF;
    SCU_D0C = bytes;
    SCU_D0AD = 0x100 | (bbus_dst ? 1 : 2);  /* read +4, write +2 (B-bus) or +4 */
    SCU_D0MD = 0x00000007;                  /* direct mode, start by enable bit */
    STEP(83);
    SCU_D0EN = 0x101;
    dma_started();
    dma_unlock();
    STEP(84);
}

bool                scu_dma0_busy(void)
{
    return (SCU_DSTA & D0_ACTIVE) != 0;
}

/* SCU DMA's indirect mode: the transfers in table (count, destination, source; the last
   source's top bit set), all to the B-bus, one after another; not waited for */
static void         dma_table_start(const u32 *table, bool chain);
void                scu_dma0_table(const u32 *table)
{
    dma_table_start(table, true);
}

/* ...the same, not a chain vdp_begin or the swap must wait for (CMD_RING's pieces of the lists) */
void                scu_dma0_pieces(const u32 *table)
{
    dma_table_start(table, false);
}

static void         dma_table_start(const u32 *table, bool chain)
{
    STEP(85);
    dma_lock();
    while (SCU_DSTA & D0_ACTIVE)
        ;
    STEP(86);
    CHAIN = chain;
    SCU_D0EN = 0;
    STEP(87);
    SCU_D0W = (u32)table & 0x07FFFFFF;      /* (the table's address, in indirect mode) */
    SCU_D0AD = 0x101;                       /* the table read +4, writes +2 (B-bus) */
    SCU_D0MD = 0x01000007;                  /* indirect mode, start by enable bit */
    STEP(88);
    SCU_D0EN = 0x101;
    dma_started();
    dma_unlock();
    STEP(89);
}

/* the chain finished? (seen once, it's forgotten) */
bool                scu_dma0_chain_done(void)
{
    if (CHAIN && !(SCU_DSTA & D0_ACTIVE))
        CHAIN = false;
    return !CHAIN;
}

/* ---- slave SH-2 ---- */

/* SCU timer 0: an interrupt at a given scanline of every field (vector 0x43,
   level 12). Installed the way libyaul does it (scu/scu_timer.c): through the
   BIOS, whose dispatcher calls `isr` as an ordinary function (no RTE) and
   keeps IMS and its own copy of the mask (0x06000348) in step. Writing the
   vector table and IMS directly got the timer masked again by the next
   BIOS-handled interrupt - the erase and the music never ran. */
void                scu_timer0_start(int line, void (*isr)(void))
{
    void            (*ihr_set)(u32, void (*)(void)) = *(void (**)(u32, void (*)(void)))0x06000300;
    void            (*mask_chg)(u32, u32) = *(void (**)(u32, u32))0x06000344;

    mask_chg(0xFFFFFFFF, 1u << 3);      /* mask timer 0 while we set it up */
    SCU_T0C = (u32)line;
    SCU_T1MD = 1;
    ihr_set(0x43, isr);
    mask_chg(~(1u << 3), 0);            /* and unmask it */
    __asm__ volatile ("ldc %0, sr" : : "r" (0xB0));  /* accept levels 12-15 */
}

/* the same for VBLANK-IN (vector 0x40, level 15) */
void                scu_vblank_in_start(void (*isr)(void))
{
    void            (*ihr_set)(u32, void (*)(void)) = *(void (**)(u32, void (*)(void)))0x06000300;
    void            (*mask_chg)(u32, u32) = *(void (**)(u32, u32))0x06000344;

    mask_chg(0xFFFFFFFF, 1u << 0);
    ihr_set(0x40, isr);
    mask_chg(~(1u << 0), 0);
    __asm__ volatile ("ldc %0, sr" : : "r" (0xB0));
}

/* ...and VBLANK-OUT (vector 0x41, level 14) */
void                scu_vblank_out_start(void (*isr)(void))
{
    void            (*ihr_set)(u32, void (*)(void)) = *(void (**)(u32, void (*)(void)))0x06000300;
    void            (*mask_chg)(u32, u32) = *(void (**)(u32, u32))0x06000344;

    mask_chg(0xFFFFFFFF, 1u << 1);
    ihr_set(0x41, isr);
    mask_chg(~(1u << 1), 0);
    __asm__ volatile ("ldc %0, sr" : : "r" (0xB0));
}

extern void         slave_entry(void);

static void         smpc_command(u8 cmd)
{
    while (SMPC_SF & 1)
        ;
    SMPC_SF = 1;
    SMPC_COMREG = cmd;
    while (SMPC_SF & 1)
        ;
}

__attribute__((cold)) void                slave_start(void)
{
    /* BIOS SYS_SETSINT: sets a vector in the *slave's* table; 0x94 is where it starts */
    void            (*set_sint)(u32, void *) = *(void (**)(u32, void *))0x06000310;
    volatile int    i;

    smpc_command(0x03);                 /* SSHOFF */
    for (i = 0; i < 1000; ++i)
        ;
    set_sint(0x94, (void *)slave_entry);
    FRT_FTCSR = 0;
    smpc_command(0x02);                 /* SSHON */
}

/* programs that don't use the slave CPU get an idle one */
__attribute__((weak)) void slave_main(void)
{
    for (;;)
        ;
}
