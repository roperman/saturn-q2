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
   the CPUs get on with other things. pad_read does both, waiting. */
void                pad_request(void)
{
    while (SMPC_SF & 1)
        ;
    SMPC_SF = 1;
    SMPC_IREG(0) = 0x00;        /* no SMPC status */
    SMPC_IREG(1) = 0x0A;        /* peripheral data, 15-byte mode, no time optimisation */
    SMPC_IREG(2) = 0xF0;
    SMPC_COMREG = 0x10;         /* INTBACK */
}

u16                 pad_collect(void)
{
    u16             buttons = 0;

    while (SMPC_SF & 1)
        ;
    /* port 1 direct: OREG0 = 0xF1 (1 device), OREG1 = 0x02 (digital pad) */
    if (SMPC_OREG(0) == 0xF1 && (SMPC_OREG(1) >> 4) == 0x0)
        buttons = (u16)~((SMPC_OREG(2) << 8) | SMPC_OREG(3));
    return (u16)(buttons & 0xFFF8);
}

u16                 pad_read(void)
{
    pad_request();
    return pad_collect();
}

/* ---- SCU DMA level 0, CPU-triggered ---- */

void                scu_dma0(void *dst, const void *src, u32 bytes, bool bbus_dst)
{
    SCU_D0EN = 0;
    SCU_D0R = (u32)src & 0x07FFFFFF;
    SCU_D0W = (u32)dst & 0x07FFFFFF;
    SCU_D0C = bytes;
    SCU_D0AD = 0x100 | (bbus_dst ? 1 : 2);  /* read +4, write +2 (B-bus) or +4 */
    SCU_D0MD = 0x00000007;                  /* direct mode, start by enable bit */
    SCU_D0EN = 0x101;
}

bool                scu_dma0_busy(void)
{
    return (SCU_DSTA & 0x10) != 0;
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

void                slave_start(void)
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
