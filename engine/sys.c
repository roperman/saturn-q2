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
/* Every level 0 transfer goes as an indirect table whose last entry copies a sequence number
   (dma_tokval, work RAM) to VDP1 VRAM's TOKEN_VRAM (the SCU bridges buses: it can't copy work
   RAM to work RAM, so a work RAM destination's token goes to VRAM too, its halves 4 apart under
   that table's add mode rather than 2). Done means the number has landed (its low half: a 16-bit
   write, never torn). The SCU's D0MV and
   D0WT flags aren't to be seen for a direct transfer on a Saturn (hwtest.c DMA FLAG DIR: never;
   TAB: 128 cycles), and a transfer programmed over one still draining came out shifted a word
   (the gun's records, section 65) or lost (the lists' pieces, SUITE 19 and 20) */
#define TOKEN_VRAM      (0x0F0)             /* (VDP1 VRAM: after the header's four commands) */
static u32          dma_seq, dma_tokval;
static u32          dma_tok_addr, dma_tok_want, dma_chain_want;     /* the last transfer's slot and number;
                                                                       the last chain's (vdp_submit's) */
static u32          dma_tab1[2][8] __attribute__((aligned(32)));    /* scu_dma0's table, a CPU each */
#define UNC(v)          (*(volatile u32 *)UNCACHED(&(v)))

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
static inline bool  tok_landed(u32 addr, u32 want)
{
    return !addr || (s16)(*(volatile u16 *)addr - (u16)want) >= 0;
}
bool                scu_dma0_busy(void)
{
    return !tok_landed(UNC(dma_tok_addr), UNC(dma_tok_want));
}
/* the last chain (vdp_submit's) landed? (the pieces after it don't count) */
bool                scu_dma0_chain_done(void)
{
    return tok_landed(VDP1_VRAM + TOKEN_VRAM + 2, UNC(dma_chain_want));
}
/* (BOOT_TRACE's watchdog) the token slot's low half, the number awaited, the last given, the chain's */
void                scu_dma0_debug(u32 *out)
{
    u32             a = UNC(dma_tok_addr);

    out[0] = a ? *(volatile u16 *)a : 0xFFFF;
    out[1] = UNC(dma_tok_want);
    out[2] = UNC(dma_seq);
    out[3] = UNC(dma_chain_want);
    out[4] = *(volatile u16 *)(VDP1_VRAM + TOKEN_VRAM + 2);
    out[5] = a;
}
void                scu_dma0_init(void)
{
    *(volatile u32 *)(VDP1_VRAM + TOKEN_VRAM) = 0;      /* (VRAM is anything at power-on) */
    *(volatile u32 *)(VDP1_VRAM + TOKEN_VRAM + 4) = 0;
}
/* the lock taken, the last transfer waited for, the next number: then the table's token entry
   (its last) and the registers, the enable last */
static void         dma_start(u32 *table, int n, u32 ad, bool chain)
{
    u32             seq, slot = VDP1_VRAM + TOKEN_VRAM + ((ad & 7) == 1 ? 2 : 4);   /* (the low half's) */

    dma_lock();
    while (scu_dma0_busy() || (SCU_DSTA & D0_ACTIVE))
        ;
    seq = UNC(dma_seq) + 1;
    UNC(dma_seq) = seq;
    UNC(dma_tokval) = seq;
    if (n)
        table[3 * n - 1] &= 0x7FFFFFFF;
    table[3 * n] = 4;
    table[3 * n + 1] = (VDP1_VRAM + TOKEN_VRAM) & 0x07FFFFFF;
    table[3 * n + 2] = ((u32)&dma_tokval & 0x07FFFFFF) | 0x80000000;
    SCU_D0EN = 0;
    SCU_D0W = (u32)table & 0x07FFFFFF;      /* (the table's address, in indirect mode) */
    SCU_D0AD = ad;
    SCU_D0MD = 0x01000007;                  /* indirect mode, start by enable bit */
    UNC(dma_tok_addr) = slot;
    UNC(dma_tok_want) = seq;
    if (chain)
        UNC(dma_chain_want) = seq;
    SCU_D0EN = 0x101;
    dma_unlock();
}
void                scu_dma0(void *dst, const void *src, u32 bytes, bool bbus_dst)
{
    u32             sp, *t;

#ifdef NO_AB_DMA
    if (bbus_dst && on_abus((u32)src))
    {
        ab_copy((u32)dst, (u32)src, bytes);
        return;
    }
#endif
    STEP(80);
    __asm__ volatile ("mov r15,%0" : "=r" (sp));
    t = dma_tab1[SP_MASTER(sp) ? 0 : 1];
    t[0] = bytes;
    t[1] = (u32)dst & 0x07FFFFFF;
    t[2] = (u32)src & 0x07FFFFFF;
    dma_start(t, 1, 0x100 | (bbus_dst ? 1 : 2), false);    /* read +4, write +2 (B-bus) or +4 */
    STEP(84);
}
/* SCU DMA's indirect mode: the n transfers in table (count, destination, source), all to the B-bus,
   one after another, not waited for; the table has room for one more (the token's) and the
   alignment its size then needs (ST-210 No. 25). The chain vdp_begin and the swap wait for */
void                scu_dma0_table(u32 *table, int n)
{
    STEP(85);
    dma_start(table, n, 0x101, true);
    STEP(89);
}
/* ...the same, not a chain the swap must wait for (CMD_RING's pieces of the lists) */
void                scu_dma0_pieces(u32 *table, int n)
{
    dma_start(table, n, 0x101, false);
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
