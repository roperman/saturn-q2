/*
** Bare-metal Saturn definitions - no SGL, no Sega libraries.
** Register addresses and behaviour cross-checked against Mednafen's Saturn core.
*/
#ifndef __SAT_H__
#define __SAT_H__

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef long long           s64;
typedef unsigned long long  u64;
typedef int                 bool;
#define true    1
#define false   0
#ifndef NULL
# define NULL   ((void *)0)
#endif

#define REG8(a)     (*(volatile u8 *)(a))
#define REG16(a)    (*(volatile u16 *)(a))
#define REG32(a)    (*(volatile u32 *)(a))

#define UNCACHED(p) ((void *)(((u32)(p)) | 0x20000000))

/* ---- VDP1 ---- */
#define VDP1_VRAM   (0x25C00000)
#define VDP1_TVMR   REG16(0x25D00000)
#define VDP1_FBCR   REG16(0x25D00002)
#define VDP1_PTMR   REG16(0x25D00004)
#define VDP1_EWDR   REG16(0x25D00006)
#define VDP1_EWLR   REG16(0x25D00008)
#define VDP1_EWRR   REG16(0x25D0000A)
#define VDP1_ENDR   REG16(0x25D0000C)
#define VDP1_EDSR   REG16(0x25D00010)   /* bit1 CEF: current draw finished */

/* ---- VDP2 ---- */
#define VDP2_VRAM   (0x25E00000)
#define VDP2_REG    (0x25F80000)
#define VDP2_TVMD   REG16(0x25F80000)
#define VDP2_TVSTAT REG16(0x25F80004)   /* bit3 VBLANK, bit0 PAL */
#define VDP2_RAMCTL REG16(0x25F8000E)
#define VDP2_BGON   REG16(0x25F80020)
#define VDP2_BKTAU  REG16(0x25F800AC)
#define VDP2_BKTAL  REG16(0x25F800AE)
#define VDP2_SPCTL  REG16(0x25F800E0)
#define VDP2_PRISA  REG16(0x25F800F0)
#define VDP2_PRISB  REG16(0x25F800F2)
#define VDP2_PRISC  REG16(0x25F800F4)
#define VDP2_PRISD  REG16(0x25F800F6)

/* VDP2 registers are write-only: layer enables go through a shadow copy */
void                vdp2_bgon(u16 set, u16 clear);

/* ---- SCU ---- */
#define SCU_T0C     REG32(0x25FE0090)   /* timer 0 compare: the scanline to interrupt on */
#define SCU_T1MD    REG32(0x25FE0098)   /* bit0: timers on */
#define SCU_IMS     REG32(0x25FE00A0)   /* interrupt mask: 1 = masked */
#define SCU_IST     REG32(0x25FE00A4)
#define SCU_D0R     REG32(0x25FE0000)
#define SCU_D0W     REG32(0x25FE0004)
#define SCU_D0C     REG32(0x25FE0008)
#define SCU_D0AD    REG32(0x25FE000C)
#define SCU_D0EN    REG32(0x25FE0010)
#define SCU_D0MD    REG32(0x25FE0014)
#define SCU_DSTA    REG32(0x25FE007C)   /* bit4: level 0 DMA in progress */
#define DSP_PPAF    REG32(0x25FE0080)
#define DSP_PPD     REG32(0x25FE0084)
#define DSP_PDA     REG32(0x25FE0088)
#define DSP_PDD     REG32(0x25FE008C)

/* ---- SMPC ---- */
#define SMPC_IREG(n) REG8(0x20100001 + (n) * 2)
#define SMPC_COMREG REG8(0x2010001F)
#define SMPC_OREG(n) REG8(0x20100021 + (n) * 2)
#define SMPC_SR     REG8(0x20100061)
#define SMPC_SF     REG8(0x20100063)

/* ---- SH-2 on-chip ---- */
#define FRT_TIER    REG8(0xFFFFFE10)   /* bit7 ICIE: must stay 0, we poll ICF as the CPU doorbell */
#define FRT_FTCSR   REG8(0xFFFFFE11)   /* bit7 ICF: set by the other CPU writing 0x21000000/0x21800000 */
#define FRT_TCR     REG8(0xFFFFFE16)
#define FRT_FRCH    REG8(0xFFFFFE12)
#define FRT_FRCL    REG8(0xFFFFFE13)
#define DIVU_DVSR   REG32(0xFFFFFF00)
#define DIVU_DVDNTH REG32(0xFFFFFF10)
#define DIVU_DVDNTL REG32(0xFFFFFF14)   /* writing starts the 64/32 divide; reading waits for it */

/* 64/32 hardware divide: (hi:lo) / d. Start, do other work, then read. */
#ifdef HOST
/* the game logic built for the PC (soccer/tests): the divider in software */
static s64          host_div;
static inline void  divu_start(s32 hi, u32 lo, s32 d)
{
    host_div = (s64)(((u64)(u32)hi << 32) | lo) / d;
}

static inline s32   divu_result(void)
{
    return (s32)host_div;
}
#else
static inline void  divu_start(s32 hi, u32 lo, s32 d)
{
    DIVU_DVSR = (u32)d;
    DIVU_DVDNTH = (u32)hi;
    DIVU_DVDNTL = lo;
}

static inline s32   divu_result(void)
{
    return (s32)DIVU_DVDNTL;
}
#endif

static inline s32   fmul(s32 a, s32 b)      /* 16.16 multiply */
{
    return (s32)(((s64)a * b) >> 16);
}

/* ---- pad (jo-compatible bit layout, 1 = pressed) ---- */
enum
{
    PAD_RIGHT = 1 << 15, PAD_LEFT = 1 << 14, PAD_DOWN = 1 << 13, PAD_UP = 1 << 12,
    PAD_START = 1 << 11, PAD_A = 1 << 10, PAD_C = 1 << 9, PAD_B = 1 << 8,
    PAD_R = 1 << 7, PAD_X = 1 << 6, PAD_Y = 1 << 5, PAD_Z = 1 << 4, PAD_L = 1 << 3
};

/* ---- runtime (crt0.s / sys.c) ---- */
#ifdef HOST
# include <string.h>
#else
void                *memset(void *d, int c, u32 n);
void                *memcpy(void *d, const void *s, u32 n);
int                 memcmp(const void *a, const void *b, u32 n);
#endif
int                 fmt(char *out, const char *f, ...);     /* tiny printf: %d %u %x %s %c, -/0/width/.prec */
int                 vfmt(char *out, const char *f, __builtin_va_list ap);

#define FRT_DIV     (128u)              /* the FRT's clock divider (it wraps every 312 ms: a slow frame still measures) */
#define FRT_TICKS(us)   ((u32)(us) * 26847u / (FRT_DIV * 1000u))
void                frt_init(void);
u32                 frt_read(void);
u32                 frt_to_us(u32 ticks);
void                wait_vblank_in(void);
void                wait_vblank_out(void);
u16                 pad_read(void);                          /* call once per frame, in active display */
void                pad_request(void);                  /* ask for the pad now... */
u16                 pad_collect(void);                  /* ...and have the answer later */
void                pad_vblank(void);                   /* (the vblank-out interrupt: asks, if pad_by_vblank) */
void                pad_by_vblank(bool on);             /* the vblank asks for the pad (the SMPC's read is in the picture) */

void                scu_dma0(void *dst, const void *src, u32 bytes, bool bbus_dst);

/* ---- dual CPU ---- */
/* ---- bup.c: files in the internal backup RAM (BIOS memory manager format) ---- */
bool                bup_formatted(void);
void                bup_format(void);
int                 bup_read(const char *name, void *buf, int max);     /* bytes read, or -1 */
bool                bup_write(const char *name, const char *comment, const void *buf, int size);
bool                bup_delete(const char *name);
int                 bup_free_blocks(void);

/* ---- cd.c: files on the game disc ---- */
bool                snd_init(const char *file);        /* SCSP + sound bank (tools/gen_sound.py) */
void                snd_tick(void);
extern u32          snd_ticks;
extern int          snd_stage, snd_size;                     /* 60 Hz: from the field interrupt */
void                snd_music(int song);                /* -1 = silence */
void                snd_music_cut(int song);            /* no crossfade */
void                snd_reverb(int level);              /* 0 dry .. 7 */
int                 snd_music_playing(void);
void                snd_sfx(int id);
void                snd_sfx_at(int id, int vol, int pan);   /* vol 0-127 (times its own), pan -15 left .. 15 right */
void                snd_music_volume(int v);            /* 0-15 */
bool                cd_init(void);
bool                cd_read_sectors(u32 lba, u32 count, void *dst);
bool                cd_find(const char *name, u32 *lba, u32 *size);
int                 cd_load(const char *name, void *dst, u32 max);  /* size or -1; max: whole sectors */
bool                cd_async_start(u32 lba, u32 count, void *dst);  /* a read in the background ... */
int                 cd_async_poll(int max_sectors);                 /* ... on by a few sectors: 1 done, 0 not yet, -1 failed */
bool                cd_async_busy(void);

/* ---- lzss.c ---- */
u32                 lzss_size(const u8 *src);
u32                 lzss_decode(const u8 *src, u8 *dst);    /* returns the decoded size */
#define LWRAM       ((u8 *)0x00200000)  /* 1MB low work RAM: unpacked assets */

void                scu_timer0_start(int line, void (*isr)(void));   /* isr: an ordinary function (the BIOS dispatches) */
void                scu_vblank_in_start(void (*isr)(void));
void                scu_vblank_out_start(void (*isr)(void));
void                slave_start(void);                      /* boots the slave into slave_main() */
void                slave_main(void);                       /* provided by the program */
static inline void  signal_slave(void)  { REG16(0x21000000) = 0xFFFF; }
static inline void  signal_master(void) { REG16(0x21800000) = 0xFFFF; }
static inline void  wait_signal(void)                       /* for the *other* CPU's signal */
{
    while (!(FRT_FTCSR & 0x80))
        ;
    FRT_FTCSR = 0;
}
static inline void  cache_purge(void)
{
    REG8(0xFFFFFE92) = 0x10;
    REG8(0xFFFFFE92) = 0x01;
}
bool                scu_dma0_busy(void);

#endif
