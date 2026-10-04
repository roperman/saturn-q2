/*
** Quake 2 on the Saturn: the first walkthrough.
**
** Loads a baked map (tools/bake_map.py) onto the RAM cart and walks round it
** with Quake 2's movement (pmove.c) and collision (trace.c).
**   up/down        forward/back        left/right   turn
**   L/R            strafe              A            jump (swim up)
**   X/Z            look up/down        C            centre the view
**   B (held)       fire                Y            next weapon
**   START          the stats overlay   START + Y    noclip on/off
**   START + B      (debugging) warp to the next door/lift/button
**   START + A      (debugging) warp in front of the next monster (or barrel)
**   START + C      (debugging) warp to the next item
**   START + X      (debugging) god mode
**   START + Z      (debugging) all the weapons and ammo
**   START + L      (debugging) into the next trigger
**   START + R      the benchmark: fixed views, then a table of times
*/
#include "game.h"
#ifdef BOOT_TRACE
#include <stdarg.h>
#endif

#ifndef MAP_FILE
# define MAP_FILE       "DEMO1.MAP"             /* (build.sh: MAP=demo2 ./build.sh for another) */
#endif

static u16          pad_now, pad_prev;

static bool         pressed(u16 b)
{
    return (pad_now & b) && !(pad_prev & b);
}

static int          strlen_(const char *s)
{
    int             n = 0;

    while (s[n])
        ++n;
    return n;
}

/* Quake 2's console background behind the loading screens (tools/bake_conback.py, cd/CONBACK.BIN):
   a 256-colour bitmap on VDP2's NBG1, read once into its VRAM's bank A0 (which nothing else uses);
   its palette (at its row 240: Quake's) into colour RAM at 0x200, where the status bar has it too
   (but for 0 and 255, which it swaps: the picture uses neither); up with message(), down once a
   level's in. (engine/sky.c leaves NBG1's settings as they are here) */
static bool         back_ok;

static __attribute__((cold)) void         loading_back(bool on)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    const volatile u16 *pal = (const volatile u16 *)(VDP2_VRAM + 240 * 512);
    int             i;

    if (!back_ok)
        return;
    if (!on)
    {
#ifdef BOOT_TRACE
        u32 *v;

        for (v = (u32 *)VDP2_VRAM; v < (u32 *)(VDP2_VRAM + 0x20000); ++v)
            *v = 0;                         /* (the trace's layer: kept, seen through) */
#else
        vdp2_bgon(0, 0x0002);
#endif
        return;
    }
#ifdef BOOT_TRACE
    for (i = 1; i < 254; ++i)
#else
    for (i = 1; i < 255; ++i)
#endif
        cram[0x200 + i] = r_gamma(pal[i]);
    REG16(VDP2_REG + 0x28) = 0x1200;        /* CHCTLA: NBG1 a 512 x 256 bitmap, 256 colours (NBG0 as the sky has it) */
    REG16(VDP2_REG + 0x2C) = 0;             /* BMPNA */
    REG16(VDP2_REG + 0x3C) = 0;             /* MPOFN: NBG1's at VRAM 0 */
    for (i = 0; i < 8; ++i)
        REG16(VDP2_REG + 0x80 + i * 2) = (u16)(i == 4 || i == 6);  /* its scroll 0, its zoom 1 */
    REG16(VDP2_REG + 0x10) = 0x5555;        /* CYCA0L: bank A0's slots NBG1's */
    REG16(VDP2_REG + 0x12) = 0xFFFF;
    REG16(VDP2_REG + 0xE4) = 0x0020;        /* CRAOFA: NBG0's colours from 0 (the sky's), NBG1's from 0x200 */
    REG16(VDP2_REG + 0xF8) = 0x0201;        /* PRINA: NBG1 over NBG0 (the sky), under VDP1 */
    vdp2_bgon(0x0002, 0);
}

static __attribute__((cold)) void         message(const char *a, const char *b)
{
    int             i;

    loading_back(true);
    for (i = 0; i < 2; ++i)
    {
        vdp_begin();
        vdp_text(160 - (int)strlen_(a) * 4, 100, RGB(255, 200, 120), a);
        if (b)
            vdp_text(160 - (int)strlen_(b) * 4, 116, RGB(200, 200, 200), b);
        vdp_submit();
    }
}

#ifdef BOOT_TRACE
/* (OPT=-DBOOT_TRACE, for a hang on a real Saturn that the emulator doesn't show) each CPU's step,
   by number and name, drawn the moment it's reached straight into VDP2's NBG1 bitmap (the loading
   screens' layer, left on over the game and cleared to see through): unlike VDP1's frames it shows
   at once, whatever's stuck, so a hang leaves on the screen where each CPU stopped. The names, the
   frame count and this code live in low work RAM (high work RAM's full) */
#define BT_N            (96)
__attribute__((section(".lwdata"))) static char bt_names[BT_N][24] = {   /* (not const: .lwdata is writable) */
    [1] = "FRAME", [2] = "WAIT SLAVE'S MOVE", [3] = "GOT SLAVE'S MOVE", [4] = "WAIT SLAVE'S LIGHT",
    [5] = "SLAVE'S LIST STARTED", [6] = "MOVERS AND PMOVE", [7] = "VIEW SKY EFFECTS", [8] = "ENTITIES",
    [9] = "VDP BEGIN", [10] = "RENDER WORLD", [11] = "MODELS TO DSP", [12] = "WAIT DSP IDLE",
    [13] = "DSP STARTED", [14] = "WALK", [15] = "GAME STEP", [16] = "DRAW MASTER",
    [17] = "WAIT SLAVE'S DRAWING", [18] = "TEXTURE MAKER END", [19] = "STATUS BAR", [20] = "SUBMIT",
    [21] = "SUBMITTED", [22] = "WAIT MAKER STATE", [23] = "UPLOADS", [24] = "WAIT DSP MODEL",
    [25] = "WAIT DSP GUN", [26] = "WALK DONE", [27] = "MENU INPUT", [28] = "GAME STEP DONE",
    [30] = "SOUNDS", [31] = "HUD INIT", [32] = "RENDER INIT", [33] = "TRACE INIT", [34] = "MOVERS INIT",
    [35] = "G INIT", [36] = "SKY INIT", [37] = "MODELS HOT", [38] = "TRACE HOT", [39] = "PORTALS",
    [40] = "WALL LEVEL", [41] = "VIEW LEVEL", [42] = "DSPM LEVEL", [43] = "LOADED",
    [50] = "WAIT SIGNAL", [51] = "LIST JOB", [52] = "WAIT CAMERA", [53] = "SHADE AND LIGHT",
    [54] = "DRAW FRONT", [55] = "DRAWN, SIGNALLED", [56] = "NEXT MOVE", [57] = "WAIT DSP MODEL",
    [70] = "SUBMIT: SWAP WAIT", [71] = "SUBMIT: LATE UPLOADS", [72] = "UPLOAD ONE", [74] = "SUBMIT: TABLE",
    [75] = "SUBMIT: DMA TABLE", [79] = "SUBMIT: DMA STARTED",
    [80] = "DMA0: WAIT IDLE", [81] = "DMA0: IDLE", [82] = "DMA0: ENABLE OFF", [83] = "DMA0: REGS SET",
    [84] = "DMA0: STARTED", [85] = "TABLE: WAIT IDLE", [86] = "TABLE: IDLE", [87] = "TABLE: ENABLE OFF",
    [88] = "TABLE: REGS SET", [89] = "TABLE: STARTED", [90] = "GUN RECORDS DMA", [91] = "GUN FRAMES DMA",
};
__attribute__((section(".lwdata"))) u32 bt_frame = 0;
__attribute__((section(".lwdata"))) static char bt_dsp[48] = { 0 };     /* (the DSP's self-test line, kept) */
static __attribute__((cold)) void bt_fmt(char *out, const char *f, ...);
void                dsp_init_models(void);
__attribute__((section(".lwdata"))) static char bt_dma[3][48] = { { 0 } };  /* (the DSP's DMA test, kept: reads,
                                                                               writes to high work RAM, to the cart) */

/* (at boot) how the DSP's DMA steps its address on this machine: four words read into its RAM, with
   add modes 1 and 2, from high work RAM (H) and the cart (C), and four written out to high work RAM.
   Each word's low hex digit: reads should give 0123, writes 1234 then E (the memory as it was). Not
   written to the cart: the SCU's DMA mustn't (ST-210 No. 01, ST-TECH-47), and on hardware it sometimes
   never finished */
static __attribute__((cold)) void bt_dma_test(void)
{
    u32             hw[8] __attribute__((aligned(16)));
    volatile u32    *cart = (volatile u32 *)0x227FFF80, *h = (volatile u32 *)UNCACHED(hw);
    char            *o;
    int             pass, k, m, t;

    cart_timing();                          /* (the cart on, as level.c's cart_init) */
    for (pass = 0; pass < 3; ++pass)
    {
        bool        wr = pass >= 2, onc = pass & 1;
        volatile u32 *mem = onc ? cart : h;
        u32         a = ((u32)(onc ? 0x027FFF80 : (u32)hw) & 0x07FFFFFF) >> 2;

        char *line = bt_dma[pass < 2 ? 0 : pass - 1];

        if (pass != 1)
            bt_fmt(line, "%s", wr ? "DSP WR " : "DSP RD ");
        o = line + strlen_(line);
        for (m = 1; m <= 2; ++m)
        {
            u32 prog[6];

            for (k = 0; k < 8; ++k)
                mem[k] = wr ? 0xEEEEEEEE : 0xA0000000 | (u32)k;
            prog[0] = (wr ? 0x9C000000 : 0x98000000) | a;      /* MVI a, WA0 / RA0 */
            prog[1] = 0x00001C00;                               /* MOV 0, CT0 */
            prog[2] = 0xC0000000 | (u32)m << 15 | (wr ? 1u << 12 : 0) | 4;    /* DMA, 4 words, add m */
            prog[3] = 0xD0000000 | 0x68u << 19 | 3;             /* JMP T0, 3 (the DMA going) */
            prog[4] = 0;
            prog[5] = 0xF0000000;                               /* END */
            DSP_PPAF = 0;
            DSP_PPAF = 1u << 15;
            for (k = 0; k < 6; ++k)
                DSP_PPD = prog[k];
            DSP_PDA = 0;
            for (k = 0; k < 4; ++k)
                DSP_PDD = wr ? 0xB0000000 | (u32)(k + 1) : 0x55555555;
            DSP_PPAF = (1u << 16) | (1u << 15);
            for (t = 0; t < 1000000 && (DSP_PPAF & (1u << 16)); ++t)
                ;
            *o++ = onc ? 'C' : 'H';
            *o++ = (char)('0' + m);
            *o++ = t >= 1000000 ? '!' : ' ';
            if (wr)
                for (k = 0; k < 8; ++k)
                    *o++ = "0123456789ABCDEF"[mem[k] & 15];
            else
            {
                DSP_PDA = 0;
                for (k = 0; k < 4; ++k)
                    *o++ = "0123456789ABCDEF"[DSP_PDD & 15];
            }
            *o++ = ' ';
        }
        *o = 0;
    }
}

static __attribute__((cold)) void bt_fmt(char *out, const char *f, ...)
{
    va_list         ap;

    va_start(ap, f);
    vfmt(out, f, ap);
    va_end(ap);
}

static __attribute__((cold)) void bt_line(int y, char *buf);
__attribute__((cold)) void btrace(int cpu, int step);

/* (engine/ STEP(n): the DMA's and vdp_submit's steps, the master's but for the slave's own uploads) */
static __attribute__((cold)) void bt_master_step(int step)
{
    u32             sp;

    __asm__ volatile ("mov r15,%0" : "=r" (sp));
    btrace(sp >= 0x060FC000 ? 0 : 1, step);
}

/* each CPU's last 8 steps and the frame of its last, kept (uncached: the watchdog reads both CPUs').
   Only kept: drawing a line at each step (VDP1's font read through the B-bus, ~1,500 writes to
   VDP2) made the game crawl on a Saturn. The watchdog draws them when the frames stop */
__attribute__((section(".lwdata"))) static u8 bt_hist[2][8] = { { 0 } };
__attribute__((section(".lwdata"))) static u32 bt_hpos[2] = { 0 }, bt_hframe[2] = { 0 };

__attribute__((cold)) void btrace(int cpu, int step)
{
    volatile u32    *pos = (volatile u32 *)UNCACHED(&bt_hpos[cpu]);
    u32             n = *pos;

    ((volatile u8 *)UNCACHED(bt_hist[cpu]))[n & 7] = (u8)step;
    *pos = n + 1;
    ((volatile u32 *)UNCACHED(bt_hframe))[cpu] = *(volatile u32 *)UNCACHED(&bt_frame);
}

/* the two CPUs' last steps (by name) and the four before each */
static __attribute__((cold)) void bt_draw_cpus(void)
{
    char            buf[48];
    const volatile u8 *h;
    u32             n[2];
    int             cpu, last;

    for (cpu = 0; cpu < 2; ++cpu)
    {
        h = (const volatile u8 *)UNCACHED(bt_hist[cpu]);
        n[cpu] = *(volatile u32 *)UNCACHED(&bt_hpos[cpu]);
        last = h[(n[cpu] - 1) & 7];
        bt_fmt(buf, "%s F%05d %02d %s", cpu ? "SLAVE " : "MASTER", ((volatile u32 *)UNCACHED(bt_hframe))[cpu],
               last, last < BT_N ? bt_names[last] : "?");
        bt_line(196 + cpu * 10, buf);
    }
    h = (const volatile u8 *)UNCACHED(bt_hist[0]);
    {
        const volatile u8 *g = (const volatile u8 *)UNCACHED(bt_hist[1]);

        bt_fmt(buf, "BEFORE M %d %d %d %d S %d %d %d %d", h[(n[0] - 2) & 7], h[(n[0] - 3) & 7], h[(n[0] - 4) & 7],
               h[(n[0] - 5) & 7], g[(n[1] - 2) & 7], g[(n[1] - 3) & 7], g[(n[1] - 4) & 7], g[(n[1] - 5) & 7]);
    }
    bt_line(176, buf);
}

/* (every field, from the swap's timer interrupt) a watchdog: the frame count not moving for two
   seconds, the SCU's DMA status, the DSP's (running? its PC), VDP1's and the swap's on three lines,
   again every two seconds while it's stuck: whatever the master's spinning on, this still runs */
__attribute__((section(".lwdata"))) static u32 bt_wlast = 0, bt_wstuck = 0;

static __attribute__((cold)) void bt_watch(void)
{
    u32             f = *(volatile u32 *)UNCACHED(&bt_frame);
    char            buf[48];
    int             q, fl;

    if (f != bt_wlast)
    {
        bt_wlast = f;
        bt_wstuck = 0;
        return;
    }
    if (++bt_wstuck % 100)
        return;
    vdp_debug_state(&q, &fl);
    bt_fmt(buf, "STUCK %ds DSTA %08X", bt_wstuck / 50, SCU_DSTA);
    bt_line(146, buf);
    {
        extern u32 gun_dma_bad;

        bt_fmt(buf, "DSP %08X EDSR %X COPR %04X GUN %d", DSP_PPAF, (u32)VDP1_EDSR, (u32)REG16(0x25D00014),
               gun_dma_bad);
    }
    bt_line(156, buf);
    bt_fmt(buf, "TV %04X QUEUED %d FIELDS %d", (u32)VDP2_TVSTAT, q, fl);
    bt_line(166, buf);
    bt_draw_cpus();
}

/* (a CPU exception: illegal instruction 4, illegal slot 6, CPU address error 9, DMA address error 10)
   where it happened, on the trace's layer, for good: the exception's number, the CPU (by its stack),
   the PC and SR the CPU stacked, PR, and r0-r7 as they were. The stubs below save r0-r7 and PR
   under the stacked PC and SR, then call here */
__attribute__((cold)) void bt_crash(u32 vec, const u32 *sp)
{
    char            buf[48];
    const u32       *pcsr = sp + 9;         /* (the stub's pushes: r0-r7, PR; then the CPU's: PC, SR) */
    u32             s = (u32)sp;

    bt_fmt(buf, "CRASH %s V%d PC %08X SR %08X", s >= 0x060FC000 ? "MASTER" : "SLAVE", vec, pcsr[0], pcsr[1]);
    bt_line(76, buf);
    bt_fmt(buf, "PR %08X SP %08X", sp[8], s + 44);
    bt_line(86, buf);
    bt_fmt(buf, "R0 %08X %08X %08X %08X", sp[7], sp[6], sp[5], sp[4]);
    bt_line(96, buf);
    bt_fmt(buf, "R4 %08X %08X %08X %08X", sp[3], sp[2], sp[1], sp[0]);
    bt_line(106, buf);
    for (;;)
        ;
}

/* the stubs: r0-r7 and PR pushed (r7 last, so sp[0] is r7 ... sp[7] r0, sp[8] PR), then bt_crash(vec, sp) */
#define BT_STUB(n) \
    __asm__("        .text\n        .align  2\n_bt_exc" #n ":\n" \
            "        sts.l   pr,@-r15\n        mov.l   r0,@-r15\n        mov.l   r1,@-r15\n" \
            "        mov.l   r2,@-r15\n        mov.l   r3,@-r15\n        mov.l   r4,@-r15\n" \
            "        mov.l   r5,@-r15\n        mov.l   r6,@-r15\n        mov.l   r7,@-r15\n" \
            "        mov     #" #n ",r4\n        mov     r15,r5\n" \
            "        mov.l   1f,r0\n        jsr     @r0\n        nop\n" \
            "        .align  2\n1:      .long   _bt_crash\n")
BT_STUB(4);
BT_STUB(6);
BT_STUB(9);
BT_STUB(10);
extern void         bt_exc4(void), bt_exc6(void), bt_exc9(void), bt_exc10(void);

/* this CPU's exception vectors (by its VBR) to the stubs */
__attribute__((cold)) void bt_crash_handlers(void)
{
    u32             *vbr;

    __asm__ volatile ("stc vbr,%0" : "=r" (vbr));
    vbr[4] = (u32)bt_exc4;
    vbr[6] = (u32)bt_exc6;
    vbr[9] = (u32)bt_exc9;
    vbr[10] = (u32)bt_exc10;
}

/* (vdp_submit stuck waiting for the swap) VDP1's state on two lines above the trace: its end flags,
   the command it's on and the last, its mode, the SCU DMA's status, the swap interrupt's fields
   since the last swap; then the command it's on (control, link, size, the corners) */
static __attribute__((cold)) void bt_stall(const u32 *r)
{
    char            buf[48];

    bt_fmt(buf, "VDP1 E%X C%04X L%04X M%X D%X F%d", r[0], r[1], r[2], r[3], r[4], r[5]);
    bt_line(156, buf);
    bt_fmt(buf, "CMD %08X %08X %08X", r[6], r[7], r[8]);
    bt_line(166, buf);
    bt_fmt(buf, "XY %08X %08X %08X %08X", r[9], r[10], r[11], r[12]);
    bt_line(176, buf);
}

static __attribute__((cold)) void bt_line(int y, char *buf)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             i, row, k;

    cram[0x200 + 254] = 0x0000;             /* (the layer's black and white) */
    cram[0x200 + 255] = 0x7FFF;
    REG16(VDP2_REG + 0xF8) = 0x0701;        /* PRINA: NBG1 over VDP1 (sprites 6), NBG0 1 */
    vdp2_bgon(0x0002, 0);
    for (row = -1; row < 9; ++row)
    {
        volatile u16 *d = (volatile u16 *)(VDP2_VRAM + (u32)(y + row) * 512 + 4);

        for (i = 0; i < 39; ++i)
        {
            const volatile u8 *g = (const volatile u8 *)(0x25C00000 + ((u32)vdp_glyph(buf[i] ? buf[i] : ' ') << 3));
            bool end = !buf[i];

            for (k = 0; k < 4; ++k)
            {
                u8 v = row < 0 || row > 7 || end ? 0 : g[row * 4 + k];

                *d++ = (u16)((v >> 4 ? 0xFF00 : 0xFE00) | (v & 15 ? 0xFF : 0xFE));
            }
            if (end)
                buf[i + 1] = 0;
        }
    }
}
#endif

/* The first level wouldn't load: what each try got (src/level.c level_load), the file on the disc,
   two of its sectors read again alone and compared with the cart, and the cart's RAM tested, on
   the screen for good (a photo of it says where it failed) */
static __attribute__((cold, noreturn)) void load_failed(const char *file)
{
    static const char *why[5] = { "-", "PLAY REFUSED", "NO SECTORS CAME", "GET REFUSED", "NO DATA READY" };
    u32             lba = 0, size = 0, ram[3], h0 = 0, h1 = 0;
    bool            found = cd_find(file, &lba, &size);
    int             s0 = level_sector_check(file, 0, &h0), s1 = level_sector_check(file, 600, &h1), i, t;

    level_ram_test(ram);
    loading_back(true);
    for (;;)
        for (i = 0; i < 2; ++i)
        {
            int y = 24;

            vdp_begin();
            vdp_printf(16, y, RGB(255, 200, 120), "%s WON'T LOAD", file);
            y += 16;
            vdp_printf(16, y, RGB(220, 220, 220), "CART ID %02X  RAM %s", (u32)REG8(0x24FFFFFF),
                       ram[0] ? "BAD" : "OK");
            y += 10;
            if (ram[0])
            {
                vdp_printf(16, y, RGB(255, 120, 120), "AT %08X WROTE %08X READ %08X", ram[0], ram[1], ram[2]);
                y += 10;
            }
            if (found)
                vdp_printf(16, y, RGB(220, 220, 220), "FILE LBA %d SIZE %d", lba, size);
            else
                vdp_printf(16, y, RGB(255, 120, 120), "FILE NOT FOUND ON THE DISC");
            y += 14;
            for (t = 0; t < 2; ++t)
            {
                const u32 *d = level_try[t];

                if (d[1])
                    vdp_printf(16, y, RGB(220, 220, 220), "TRY %d %s: %d OF %d SECTORS", t + 1, t ? "SAFE" : "ALL",
                               d[0], d[1]);
                else
                    vdp_printf(16, y, RGB(220, 220, 220), "TRY %d %s: READ, NO CD ERROR", t + 1, t ? "SAFE" : "ALL");
                y += 10;
                vdp_printf(24, y, RGB(180, 180, 180), "%s ST %08X START %08X", why[d[2] < 5 ? d[2] : 0], d[3], d[4]);
                y += 12;
            }
            vdp_printf(16, y, RGB(220, 220, 220), "SECTOR 0: %s %08X",
                       s0 == -1 ? "SAME" : s0 == -2 ? "NOT READ" : "DIFFERS", h0);
            y += 10;
            if (s0 >= 0)
            {
                vdp_printf(24, y, RGB(180, 180, 180), "FIRST AT BYTE %d", s0);
                y += 10;
            }
            vdp_printf(16, y, RGB(220, 220, 220), "SECTOR 600: %s %08X",
                       s1 == -1 ? "SAME" : s1 == -2 ? "NOT READ" : "DIFFERS", h1);
            y += 10;
            if (s1 >= 0)
            {
                vdp_printf(24, y, RGB(180, 180, 180), "FIRST AT BYTE %d", s1);
                y += 10;
            }
            y += 8;
            vdp_printf(16, y, RGB(255, 255, 160), "(EXPECTED START 5132534C = Q2SL)");
            vdp_printf(16, y + 10, RGB(255, 255, 160), "PLEASE PHOTOGRAPH THIS SCREEN");
            vdp_submit();
        }
}

static bool         slave_ok, start_used;
static u32          at_end[13];             /* (the benchmarks) the texture cache's counts at the end: r_full, vdp_peak, r_wset */

static void         counts_reset(void)
{
    memset(r_full, 0, sizeof(r_full));
    memset(vdp_peak, 0, sizeof(vdp_peak));
#ifdef TEX_WSET
    {
        extern u32 r_wset[5];

        memset(r_wset, 0, sizeof(r_wset));
    }
#endif
}

static void         counts_at_end(void)
{
    int             k;

    at_end[0] = r_full[0];
    at_end[1] = r_full[1];
    at_end[12] = r_full[2];
    for (k = 0; k < 5; ++k)
        at_end[2 + k] = (u32)vdp_peak[k];
#ifdef TEX_WSET
    {
        extern u32 r_wset[5];

        for (k = 0; k < 5; ++k)
            at_end[7 + k] = r_wset[k];
    }
#endif
}

#ifdef LEVEL_TEST
u32                 lt_hw[4][4], lt_nhw, lt_fl[4];

static u32          lt_count(int mdl)       /* (the level's entities with that model) */
{
    int             i, n = 0;

    for (i = 1; i < g_nfull; ++i)
        n += g_edicts[i].mdl == &models[mdl];
    return (u32)n;
}
int                 r_level_flags(void), view_nslots(void);
#endif
#ifdef FIGHT_BENCH
/* (OPT=-DFIGHT_BENCH: START + R stands you in the round room, god mode on,
   wakes the monsters around it and times 20 seconds of the fight, the game
   running: frame, CPU and game time, and how long each picture stayed up) */
#define FIGHT_SKIP      (10)                    /* frames before timing starts */
static int          fight_frames = -1;          /* -1: not running */
static u32          fight_ldma, fight_us, fight_cpu, fight_game, fight_gmax, fight_n, fight_swaps[8], fight_ntr, fight_ttr;
static bool         fight_done;
static g_trace_site fight_sites[16];            /* the traces' call sites, most time first */
static u32          fight_tr[8];                /* trace.c's counts */
static u32          fight_seg[6], fight_t[7];   /* the master's frame in parts (us): input, player, game, before, world, after */
static u32          fight_dl[6];
#ifdef DSP_WALLS
static u32          fight_dw[6];                /* (render.c's dw_stat, as at the fight's last frame) */
static u32          fight_why[4];               /* (lit faces the DSP's weren't: rs.dw_why) */
#endif
#ifdef FS_STATS
u32                 fs_sum[11];
#endif
static u32          fight_po[5];                /* (the portals: the flow's us, clusters reached, portals projected, faces culled) */                /* (the world's dynamic lights: the faces' test, the sums, us; faces lit) */
static u32          fight_pre[8], fight_pref[8], fight_pt;  /* (the master before the walk, us: movers, pmove, camera+gun,
                                                   sky+effects, entities+sprites, their light, the tint; render_world to
                                                   the slave's signal. fight_pref: this frame's, added in while timing) */
#define PRE(k)          (fight_pref[k] = frt_to_us((frt_read() - fight_pt) & 0xFFFF), fight_pt = frt_read())
# define FT(k)          (fight_t[k] = frt_read())
static u32          fight_tt[5];                /* a box trace's parts, 0.1 us; a line trace's */
static u32          fight_gun;                  /* the gun in your hands (us) */
#ifdef MF_PROF
static u32          fight_mf[12];               /* (render.c's mf_t, mf_v, mf_m as the fight ended) */
#endif
static u32          fight_drop[2];              /* commands dropped (a CPU's list full), textures not drawn (its cache full) */
static u32          fight_vph[4];               /* ...its vertices, sort, commands, kept (render.c view_ph) */
static u32          fight_r[14];
#ifdef R_PROFILE
static u32          fight_p[13];                 /* the world: setup, grid, cells, slow cells (us); faces, cells, C cells */
#endif                /* the drawing: master, slave; models, their polygons; the
                                                   models' light, vertices, polygons, commands (cumulative), DSP wait */
#else
# define FT(k)          ((void)0)
# define PRE(k)         ((void)0)
#endif
static u32          us_game, bench_us[5];

/* the benchmark (START + R): fixed views, 16 frames each, the game paused. x y z (eye), yaw, pitch */
static const s32    bench_demo1[][5] = {
    { 128, -320, 46, 0x6000, 0 },           /* the start, looking down the hall */
    { -1064, 1632, -2, 0x8000, 0 },         /* by the door to the crate room */
    { -1960, 1444, -2, 0xC000, 0 },         /* the dark corridor */
    { 600, -428, -74, 0x4000, 0 },          /* the round room with the soldiers */
    { 20, -213, 46, 0x7000, 0x800 },        /* the hall, turned */
    { -1636, 1488, 142, 0x0000, 0 },        /* the sliding doors */
};
/* demo2's heavy places (its benchmark: MAP=demo2) */
static const s32    bench_demo2[][5] = {
    { 832, 2292, -210, 0x8000, 0 },         /* the start: the warehouse */
    { 935, 2506, 46, 0x6544, 0 },           /* up among the crates */
    { -103, -300, 30, 0x3D35, 0 },          /* the big room: VDP1 is behind here */
    { -88, -260, 30, 0x4354, 0 },           /* ...and more so */
    { 618, -757, -146, 0x1670, 0 },
    { 503, -1816, 46, 0x90E5, 0 },
};
#define NBENCH          (6)                 /* views in each */
static const s32    (*bench_views)[5];
#ifdef HW_BENCH
/* (OPT=-DHW_BENCH, for a real Saturn, which can't save a picture: START + R runs the views held,
   then each turned full circle (textures and all, as TURN_BENCH), then the fight (FIGHT_BENCH's),
   and shows the three on one screen till START + R runs it again) */
static bool         bench_turn;             /* (the views turning) */
static int          hb_stage;               /* 1 the views held, 2 turning, 3 the fight, 4 done */
static bool         hb_go;                  /* (the fight's start, as START + R starts it in FIGHT_BENCH) */
/* (low work RAM: a level with the walls' and the maker's buffers has ~100 bytes of high to spare) */
__attribute__((section(".lwdata"))) static u32 hb_v[2][NBENCH + 1][3] = { { { 0 } } };  /* held, turned: each
                                               view's CPU, frame (0.1 ms), walk or uploads; all */
__attribute__((section(".lwdata"))) static u32 hb_late[2] = { 0 }, hb_made[2][3] = { { 0 } };  /* late uploads;
                                               textures made by the DSP, the CPU, bad reads */
# define BENCH_FRAMES   (bench_turn ? 92 : 16)
/* (its pages: the summary, each view's; the CPUs' profiles of the fight (OPT=-DSLAVE_PROF); the
   timing suite's three (OPT=-DHW_TEST)) */
# if defined(HW_TEST)
#  define HB_PAGES      (7)
# elif defined(SLAVE_PROF)
#  define HB_PAGES      (4)
# else
#  define HB_PAGES      (2)
# endif
#elif defined(TURN_BENCH)
/* (OPT=-DTURN_BENCH: at each view a full turn in 90 frames, textures and all:
   the first column is then the texture uploads a frame, not the walk) */
# define bench_turn     (true)
# define BENCH_FRAMES   (92)
#elif defined(SKY_VIEWS)
/* (OPT=-DSKY_VIEWS: the benchmark's views the sky from demo1's yard, level and looking up, 6 s each,
   for pictures of it) */
static const s32    bench_sky[][5] = {
    { -300, 1400, -82, 0x0000, 0 }, { -300, 1400, -82, 0x4000, 0 }, { -300, 1400, -82, 0x8000, -0x0C00 },
    { -300, 1400, -82, 0xC000, -0x1800 }, { -164, 1396, -82, 0x74A6, 0 }, { -164, 1396, -82, 0x74A6, -0x1400 },
};
# define bench_turn     (false)
# define BENCH_FRAMES   (150)
#else
# define bench_turn     (false)
# define BENCH_FRAMES   (16)
#endif
static int          bench_view = -1, bench_frame;
#ifdef FAR_LINEUP
/* (OPT="-DBENCH_HOLD -DFAR_LINEUP", tools/compare.sh with COMPARE=far: each view a monster, a
   soldier and then an infantry, from 150, 200 and 300 units, from where it can be seen: with
   CMP_EXTRA="-DFAR_LINEUP -DMODEL_FAR=30000 -DFAR_B=0", its whole mesh against its coarse one) */
static void         far_lineup(int view)
{
#ifdef LINEUP_BARRELS
    /* (-DLINEUP_BARRELS: the level's barrels instead, each view the next, from 80 units at a standing
       player's eye, looking a little down) */
    static const int dist[3] = { 80, 80, 80 };
    const q_mdl     *want = &models[MDL_BARREL];
    int             i, k, nth = view;

    for (i = 1; i < g_nfull && !(g_edicts[i].mdl == want && !g_edicts[i].inactive && nth-- == 0); ++i)
        ;
#else
    static const int dist[3] = { 150, 200, 300 };
    const q_mdl     *want = &models[view < 3 ? MDL_SOLDIER : MDL_INFANTRY];
    int             i, k;

    for (i = 1; i < g_nfull && !(g_edicts[i].mdl == want && !g_edicts[i].dead && !g_edicts[i].inactive); ++i)
        ;
#endif
    for (k = 0; i < g_nfull && k < 16; ++k)
    {
        const g_ent     *e = &g_edicts[i];  /* (the game's: the renderer's are only those in sight) */
        int             a = (e->yaw + k * 4096) & 0xFFFF;
        s32             o[3], p[3];
        q_trace         t;

        o[0] = e->origin[0];
        o[1] = e->origin[1];
#ifdef LINEUP_BARRELS
        o[2] = p[2] = e->origin[2] + FIX(46);
#else
        o[2] = p[2] = e->origin[2] + FIX(8);
#endif
        p[0] = o[0] + dist[view % 3] * fcos(a);
        p[1] = o[1] + dist[view % 3] * fsin(a);
        t = trace_line(o, p, 0, CONTENTS_SOLID | CONTENTS_WINDOW);
        if (t.fraction == FIX(1) && !t.startsolid)
        {
            cam.pos[0] = p[0];
            cam.pos[1] = p[1];
            cam.pos[2] = p[2];
            cam.yaw = (a + 0x8000) & 0xFFFF;
#ifdef LINEUP_BARRELS
            cam.pitch = 0x0800;
#else
            cam.pitch = 0;
#endif
            return;
        }
    }
}
#endif
static u32          bench_acc[NBENCH][7];   /* walk, master, slave, cpu, frame (us, summed), vblanks waiting for VDP1, the lists' DMA */
static u32          bench_drop[2];          /* commands dropped (a CPU's list full), textures not drawn (its cache full) */
static u32          bench_prof[15];         /* setup, grid, cells, slow, models, nfast, nslow, faces, the models' light, verts, polys */
#ifdef R_PROFILE
static u32          bench_ax[8];            /* the faces' transforms, the C before a whole face's cells, rows a row at a time,
                                               cells made, cells_asm's time, commands and calls, faces a row at a time */
#endif
static bool         bench_done;
static bool         god;

/* The slave's first job of a frame (two CPUs, the game's tick during the drawing): the
   entities' list as the frame starts (g_render_ents: the game's done with them, its tick ran
   in the last frame's drawing), then, once the master has the camera, their light
   (ents_light). The master meanwhile moves you and the camera, the gun and the effects, so the
   walk (and the slave's drawing) starts sooner. The flags are read uncached: the CPUs'
   caches are their own */
static u32          pre_job, pre_cam, pre_done, lit_done = 1;
#define LIT_DONE        (*(volatile u32 *)UNCACHED(&lit_done))  /* (the models' dynamic lights: after PRE_DONE) */
#define PRE_JOB         (*(volatile u32 *)UNCACHED(&pre_job))
#define PRE_CAM         (*(volatile u32 *)UNCACHED(&pre_cam))   /* 1 + ents_pvs(), when the camera's moved */
#define PRE_DONE        (*(volatile u32 *)UNCACHED(&pre_done))

static bool         pre_pending;            /* (the slave has a first job not yet waited for) */

/* (draw_master, before its first model: the slave's lit them, a frame behind: model.c) */
static void         lit_wait(void)
{
    while (!LIT_DONE)
        ;
    ent_lit_forget();
    ents_shade_forget();
    r_wall_forget();
}

/* (render_world, before it puts the entities in their leaves: the slave's done with them) */
static void         pre_wait(void)
{
    while (!PRE_DONE)
        ;
    cache_purge();                              /* (the slave's list and light) */
    pre_pending = false;
}

#ifdef SLAVE_PROF
/* (OPT="-DFIGHT_BENCH -DSLAVE_PROF") where the CPUs' time goes: each one's watchdog timer
   interrupts it every 16,384 cycles, and prof_isr counts the address it was at, in 8-byte
   buckets over high work RAM's code (a u16 each, in low work RAM's top 64 KB: the slave's
   32 KB, then the master's; nothing moves in high work RAM), during the fight.
   tools/prof.py reads them from a Mednafen save state */
#define PROF_VT         ((u32 *)0x002E7C00)                 /* the slave's vector table, copied */
#define PROF_ON         (*(volatile u32 *)0x202E7FF4)       /* counting (uncached) */
#define PROF_VEC        (0x7F)
#define PROF_HANDLER(name, hist, other) \
    "        .text\n" \
    "        .align 2\n" \
    "        .global " name "\n" \
    name ":\n" \
    "        mov.l   r0,@-r15\n" \
    "        mov.l   r1,@-r15\n" \
    "        mov.l   9f,r1\n" \
    "        mov.l   @r1,r0\n" \
    "        tst     r0,r0\n" \
    "        bt      5f\n"                          /* (not counting) */ \
    "        mov.l   @(8,r15),r0\n"                 /* the PC it was at */ \
    "        mov.l   1f,r1\n" \
    "        sub     r1,r0\n" \
    "        mov.l   2f,r1\n" \
    "        cmp/hs  r1,r0\n" \
    "        bt      3f\n" \
    "        shlr2   r0\n" \
    "        shlr    r0\n" \
    "        add     r0,r0\n" \
    "        mov.l   4f,r1\n" \
    "        add     r0,r1\n" \
    "        mov.w   @r1,r0\n" \
    "        add     #1,r0\n" \
    "        bra     5f\n" \
    "        mov.w   r0,@r1\n" \
    "3:      mov.l   6f,r1\n"                       /* elsewhere (low work RAM's code, the BIOS's) */ \
    "        mov.l   @r1,r0\n" \
    "        add     #1,r0\n" \
    "        mov.l   r0,@r1\n" \
    "5:      mov.l   7f,r1\n"                       /* WTCSR: OVF read, then cleared */ \
    "        mov.b   @r1,r0\n" \
    "        mov.w   8f,r0\n" \
    "        mov.w   r0,@r1\n" \
    "        mov.l   @r15+,r1\n" \
    "        mov.l   @r15+,r0\n" \
    "        rte\n" \
    "        nop\n" \
    "        .align 2\n" \
    "1:      .long   0x06004000\n" \
    "2:      .long   0x00020000\n" \
    "4:      .long   " hist "\n" \
    "6:      .long   " other "\n" \
    "7:      .long   0xFFFFFE80\n" \
    "9:      .long   0x202E7FF4\n" \
    "8:      .word   0xA521\n"                      /* interval timer, on, clock/64 */ \
    "        .align 2\n"
__asm__ (PROF_HANDLER("_prof_isr", "0x202F8000", "0x202E7FF8")
         PROF_HANDLER("_prof_isr_m", "0x202F0000", "0x202E7FFC"));

/* this CPU's watchdog on, into its vector table */
static void         prof_start(u32 *vt, void (*isr)(void))
{
    vt[PROF_VEC] = (u32)isr;
    *(volatile u16 *)0xFFFFFEE4 = (u16)(PROF_VEC << 8);         /* VCRWDT: its vector */
    *(volatile u16 *)0xFFFFFEE2 = (u16)((*(volatile u16 *)0xFFFFFEE2 & ~0xF0) | 0xF0);   /* IPRA: level 15 */
    *(volatile u16 *)0xFFFFFE80 = 0x5A00;                       /* WTCNT = 0 */
    *(volatile u16 *)0xFFFFFE80 = 0xA521;                       /* WTCSR: interval timer, on, clock/64 */
}
#endif

/* The next frame's move, early (OPT=-DNO_PREMOVE: at its start, as before). The slave, its
   drawing done, waits for the master's (the game's tick ran before the master drew), reads the
   pad then and moves you (the doors and lifts too), while the master finishes the frame: the
   next one starts with it done, 0.9 ms or more sooner. What you press is only as much older as
   that's early (the SMPC reads a pad once a field; now and then it's the field before's). The
   master asks for it (PM_REQ) in a frame the game runs in and the slave draws in, and waits for
   it at the next one's start; the slave doesn't move you if a menu's up or the benchmark's
   views are showing (PM_DONE 1: the master does it), and gives it the pad it read (PM_PAD) for
   everything else a frame start does with it. dt: this frame's (the next's isn't known yet) */
#ifndef NO_PREMOVE
static u32          pm_req, pm_drawn, pm_done, pm_pad;
static s32          pm_dt;
static bool         pm_asked;               /* (the master: this frame's asked for) */
#define PM_REQ          (*(volatile u32 *)UNCACHED(&pm_req))
#define PM_DRAWN        (*(volatile u32 *)UNCACHED(&pm_drawn))  /* (the master's drawing done) */
#define PM_DONE         (*(volatile u32 *)UNCACHED(&pm_done))   /* 1: not moved; 2: moved */
#define PM_PAD          (*(volatile u32 *)UNCACHED(&pm_pad))
#define PM_DT           (*(volatile s32 *)UNCACHED(&pm_dt))
#ifdef FIGHT_BENCH
static u32          pm_t[3];                /* (slave: waiting for the master's drawing, moving; master: waiting, ticks) */
#endif
#endif
#ifdef FIGHT_BENCH
static u32          fight_pm[4];            /* (and frames it moved you in) */
#endif

/* The pad into a move: the camera turned, and the command pmove takes (the master at a frame's
   start; or the slave at the last one's end, premove) */
#ifdef MOVE_TEST
/* (OPT=-DMOVE_TEST: the same moves every run, whoever makes them, a fixed dt: from your first
   move in a game, stand, walk, turn, jump, back, curve, stand; where you end up shown, to
   compare builds) */
static int          mt_moves;
static s32          mt_at[5];

static u16          mt_pad(void)
{
    int             k = mt_moves++;

    if (k == 280)
    {
        mt_at[0] = pl.origin[0];
        mt_at[1] = pl.origin[1];
        mt_at[2] = pl.origin[2];
        mt_at[3] = cam.yaw;
        mt_at[4] = pl.velocity[2];
    }
    return k < 30 ? 0 : k < 90 ? PAD_UP : k < 110 ? PAD_RIGHT : k < 150 ? (u16)(PAD_UP | (k >= 120 && k < 124 ? PAD_A : 0))
         : k < 180 ? PAD_DOWN : k < 240 ? (u16)(PAD_UP | PAD_LEFT) : 0;
}
#endif

static void         move_input(u16 pad, int turn, q_usercmd *cmd)
{
#ifdef MOVE_TEST
    pad = mt_pad();
#endif
#ifdef FIGHT_BENCH
    if (fight_frames >= 0)
    {
        cam.yaw = 0x4000;                       /* (the fight: standing still, looking into the room) */
        cam.pitch = 0;
        pad &= PAD_START;
    }
#endif
    if (pad & PAD_LEFT)
        cam.yaw += turn;
    if (pad & PAD_RIGHT)
        cam.yaw -= turn;
    if (pad & PAD_X && !(pad & PAD_START))
        cam.pitch = imax(cam.pitch - turn / 2, -0x3800);
    if (pad & PAD_Z && !(pad & PAD_START))
        cam.pitch = imin(cam.pitch + turn / 2, 0x3800);
    if (pad & PAD_C && !(pad & PAD_START))
        cam.pitch = 0;
    cam.yaw &= 0xFFFF;
    cmd->yaw = cam.yaw;
    cmd->pitch = cam.pitch;
    cmd->forward = pad & PAD_UP ? FIX(300) : pad & PAD_DOWN ? -FIX(300) : 0;
    cmd->side = pad & PAD_R ? FIX(300) : pad & PAD_L ? -FIX(300) : 0;
    cmd->up = pad & PAD_A && !(pad & PAD_START) ? FIX(300) : 0;
#if defined(ENTLIGHT_CHECK) && !defined(FIGHT_BENCH)
    cmd->forward = FIX(300);                /* (walking in wide circles: new clusters, new things in view) */
    cam.yaw = (cam.yaw + 0x60) & 0xFFFF;
    cmd->yaw = cam.yaw;
#endif
#ifdef VIEW_TEST
    {
        /* (walking in circles, turning at three speeds in turn, then standing still: the
           gun's bob and lag, and none) */
        static int vw;
        static const int turns[4] = { 0, 0x60, 0x300, 0 };

        ++vw;
        cmd->forward = vw / 100 % 4 == 3 ? 0 : FIX(300);
        cam.yaw = (cam.yaw + turns[vw / 100 % 4]) & 0xFFFF;
        cmd->yaw = cam.yaw;
    }
#endif
#ifdef LADDER_CHECK
    cmd->forward = FIX(300);                /* (into the ladder, and up it) */
    cmd->up = FIX(300);
#endif
}

#ifndef NO_PREMOVE
static void         premove(void)
{
    q_usercmd       cmd;
    u16             pad;
    s32             dt = PM_DT;
#ifdef FIGHT_BENCH
    u32             t = frt_read(), t1;
#endif

    while (!PM_DRAWN)
        ;
    cache_purge();                              /* (the game's tick, the master's) */
#ifdef FIGHT_BENCH
    t1 = frt_read();
    pm_t[0] = (t1 - t) & 0xFFFF;
#endif
    if (menu_active() || bench_view >= 0)
    {
        PM_DONE = 1;
        return;
    }
    pad = pad_collect();
    PM_PAD = pad;
    if (g_player->dead || level_complete)
        pad &= PAD_START;
    move_input(pad, (int)fmul(dt, 0x6000), &cmd);
    movers_update(dt);
    pmove(&cmd, dt);
#ifdef FIGHT_BENCH
    pm_t[1] = (frt_read() - t1) & 0xFFFF;
#endif
    PM_DONE = 2;
}
#endif

#ifdef FIGHT_BENCH
static u32          sl_t, sl_dyn, sl_end, sl_endt;    /* (the slave: its dynamic model light; last frame's end to its next job's) */
u32                 fight_sl[3];
#endif

void                slave_main(void)
{
    frt_init();
    FRT_FTCSR = 0;
#ifdef SLAVE_PROF
    {
        extern void prof_isr(void);
        u32         *old;
        int         i;

        __asm__ volatile ("stc vbr,%0" : "=r" (old));
        for (i = 0; i < 128; ++i)
            PROF_VT[i] = old[i];
        __asm__ volatile ("ldc %0,vbr" : : "r" (PROF_VT));
        prof_start(PROF_VT, prof_isr);
        __asm__ volatile ("ldc %0,sr" : : "r" (0xE0));          /* level 15 through */
    }
#endif
#ifdef BOOT_TRACE
    {
        extern void bt_crash_handlers(void);

        bt_crash_handlers();                    /* (the slave's own vectors) */
    }
#endif
    signal_master();                            /* hello */
    for (;;)
    {
        BT(1, 50);
        wait_signal();
#ifdef HW_TEST
        if (*(volatile u32 *)UNCACHED(&ht_slave_req))
        {
            ht_slave_run();                     /* (the timing suite's: src/hwtest.c) */
            continue;
        }
#endif
        cache_purge();                          /* the master's list, camera and frame */
        if (PRE_JOB)
        {
            BT(1, 51);
            PRE_JOB = 0;
#ifdef FIGHT_BENCH
            sl_end = (frt_read() - sl_endt) & 0xFFFF;     /* (last frame's drawing done to this job) */
#endif
            g_render_ents();
            BT(1, 52);
            while (!PRE_CAM)
                ;
            BT(1, 53);
            cache_purge();                      /* (the camera, just moved) */
            g_render_late(PRE_CAM > 1);
            ents_leaf();
            PRE_DONE = 1;
#ifdef FIGHT_BENCH
            sl_t = frt_read();
#endif
            ents_shade(PRE_CAM > 1);            /* (the master's gone on: it waits for these only to draw) */
#ifndef NO_LIGHT_AHEAD
            ents_light_dyn(PRE_CAM > 1);        /* (the master's gone on; the walk's only starting) */
#endif
#ifdef FIGHT_BENCH
            sl_dyn = (frt_read() - sl_t) & 0xFFFF;
#endif
            LIT_DONE = 1;
            continue;
        }
        BT(1, 54);
        render_slave();
#ifdef FIGHT_BENCH
        sl_endt = frt_read();
#endif
        signal_master();
        BT(1, 55);
#ifndef NO_PREMOVE
        if (PM_REQ)
        {
            BT(1, 56);
            premove();                          /* (the next frame's move, while the master finishes) */
        }
#endif
#ifdef WALLS_AHEAD
        r_wall_ahead();                         /* (a test: the next frame's walls, while the master finishes) */
#endif
    }
}

/* debugging: next to the next door, lift or button, facing it */
static int          warp_m;

static __attribute__((cold)) void         warp_next(void)
{
    int             tries, r, a, k;

    for (tries = 0; tries < lv.nmodels; ++tries)
    {
        const q_model   *mo;
        s32             c[3];

        warp_m = warp_m + 1 < lv.nmodels ? warp_m + 1 : 1;
        if (lv.movers[warp_m].kind < MV_DOOR)
            continue;
        mo = &lv.models[warp_m];
        for (k = 0; k < 3; ++k)
            c[k] = (mo->mins[k] >> 1) + (mo->maxs[k] >> 1) + mover_ofs[warp_m][k];
        for (r = 48; r <= 160; r += 16)
            for (a = 0; a < 0x10000; a += 0x2000)
            {
                s32 p[3];
                q_trace t;

                p[0] = c[0] + fcos(a) * r;
                p[1] = c[1] + fsin(a) * r;
                p[2] = mo->mins[2] + mover_ofs[warp_m][2] + FIX(25);
                t = pm_trace(p, p);
                if (t.startsolid || lv.leafs[level_leaf(p)].cluster < 0)
                    continue;               /* in something, or outside the map */
                pmove_spawn(p);
                pl.origin[2] -= FIX(9);
                cam.yaw = (a + 0x8000) & 0xFFFF;
                return;
            }
    }
}

/* debugging: in front of the next monster, item or barrel, facing it */
static int          warp_e = -1;

static __attribute__((cold)) void         warp_ent(bool items)
{
    int             tries, a, n = g_nfull + g_nitems;  /* (the ones that can be drawn: g_ent_at's first) */

    for (tries = 0; tries < n; ++tries)
    {
        const g_ent     *e;

        warp_e = (warp_e + 1) % n;
        e = g_ent_at(warp_e);
        if (e->inactive || !e->mdl || (e->kind == EK_ITEM) != items
            || (e->kind != EK_ITEM && e->kind != EK_MONSTER && e->kind != EK_OBJECT))
            continue;
#ifdef WARP_ONLY
        if (e->mdl != &models[WARP_ONLY])
            continue;                       /* (OPT=-DWARP_ONLY=MDL_x: that model's alone) */
#endif
        for (a = 0; a < 0x10000; a += 0x2000)
        {
            int     ang = (e->yaw + a) & 0xFFFF;
            s32     p[3];
            q_trace t;

            p[0] = e->origin[0] + fcos(ang) * 100;
            p[1] = e->origin[1] + fsin(ang) * 100;
            p[2] = e->origin[2] + FIX(16);
            t = pm_trace(p, p);
            if (t.startsolid || lv.leafs[level_leaf(p)].cluster < 0)
                continue;
            pmove_spawn(p);
            cam.yaw = (ang + 0x8000) & 0xFFFF;
            return;
        }
    }
}

/* debugging: into the next trigger (which fires it) */
static int          warp_t;

static __attribute__((cold)) void         warp_trigger(void)
{
    int             n, k;

    for (n = 0; n < g_ntrigs; ++n)
    {
        g_ent   *e;
        s32     p[3];

        warp_t = (warp_t + 1) % g_ntrigs;
        e = G_SHORT_ENT(g_nitems + warp_t);
        if (e->kind != EK_TRIGGER || e->inactive)
            continue;
        for (k = 0; k < 3; ++k)
            p[k] = (g_trig_areas[warp_t].lo[k] >> 1) + (g_trig_areas[warp_t].hi[k] >> 1);
        pmove_spawn(p);
        pl.origin[2] -= FIX(9);
        g_centerprint(e->message ? e->message : "(trigger)");
        return;
    }
}

/* the player's weapon and the game's tick (run by the renderer on the master
   while the slave draws, unless "faster fights" is off or OPT=-DNO_GAME_DURING_DRAW:
   what's drawn is then the game as it was a frame before, for the monsters; the
   view is this frame's) */
static s32          game_dt;
#ifdef NO_GAME_DURING_DRAW
bool                game_during_draw;
#else
bool                game_during_draw = true;    /* START + UP switches it; the options too */
#endif

#ifdef DSP_SWAP_TEST
#include "dsp.h"
/* (OPT=-DDSP_SWAP_TEST: the DSP's program swapped twice a frame, as the walls' would be: after
   the models' job, another program (xform.dsp) with a job of its own, checked; the models'
   program back before the next frame's) */
u32                 swap_n[3];              /* (swaps and checked jobs, wrong ones, DSP still busy) */
static s32          swap_in[48] __attribute__((aligned(16)));
static s32          swap_out[48] __attribute__((aligned(16)));

static void         swap_test(void)
{
    static const s32 m[12] = { FIX(100), FIX(1), 0, 0, FIX(200), 0, FIX(1), 0, FIX(300), 0, 0, FIX(1) };
    const s32       *out = (const s32 *)UNCACHED(swap_out);
    int             k, bad = 0;

    if (dsp_busy())
    {
        ++swap_n[2];
        return;
    }
    for (k = 0; k < 16; ++k)
    {
        swap_in[3 * k] = k + (int)(swap_n[0] & 7);
        swap_in[3 * k + 1] = 2 * k;
        swap_in[3 * k + 2] = 3 * k;
    }
    memset((void *)UNCACHED(swap_out), 0xEE, sizeof(swap_out));
    dsp_init();
    dsp_transform(m, swap_in, swap_out, 16);
    dsp_wait();
    for (k = 0; k < 16; ++k)
        bad |= out[k] != FIX(100 + k + (int)(swap_n[0] & 7)) || out[16 + k] != FIX(200 + 2 * k)
               || out[32 + k] != FIX(300 + 3 * k);
    ++swap_n[0];
    swap_n[1] += (u32)bad;
}
#endif

static void         game_step(void)
{
#ifdef DSP_SWAP_TEST
    swap_test();
#endif
    u32             tg = frt_read();

    BT(0, 15);
    s_lag(level.acc);                       /* (the shot's moment: the last tick's, which allowed it) */
    g_player_fire(pad_now & PAD_B && !(pad_now & PAD_START), cam.pos, cam.yaw, cam.pitch);
    g_frame(game_dt);
    s_queue_flush();                        /* (a mover the game set going) */
    us_game = frt_to_us((frt_read() - tg) & 0xFFFF);
    BT(0, 28);
}

static char         cur_map[16] = MAP_FILE; /* the level loaded ("DEMO1.MAP") */
static u32          vram_base;              /* VRAM before the HUD's pictures: all a level's again */

static bool         same(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

/* "demo2" in place of this level, you at its start called spot (NULL, or not
   found: the usual one); keep: you as you were (health, armour, weapons, ammo),
   as from one of Quake's levels to the next. false: it's not on the disc */
static __attribute__((cold)) bool         load_level(const char *name, const char *spot, bool keep)
{
    char            file[16], at[16];
    g_client        was = client;
    int             health = g_player ? g_player->health : 100, i;
    u32             lba, size;
    const s32       *origin = lv.start;
    int             yaw = lv.start_yaw;

    for (i = 0; name[i] && name[i] != '$' && i < 10; ++i)
        file[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    memcpy(file + i, ".MAP", 5);
    if (!cd_find(file, &lba, &size))
        return false;
    for (i = 0; spot && spot[i] && i < 15; ++i)
        at[i] = spot[i];                    /* (spot's in this level's strings: gone soon) */
    at[i] = 0;
    message("QUAKE II", "LOADING");
    g_edicts = NULL;                        /* (they, and these, come out of the level's memory again) */
    g_tn_first = NULL;
    g_thinkers = NULL;
    mover_gone = NULL;
    if (!level_load(file))
        for (;;)
            message(file, "WON'T LOAD");
    memcpy(cur_map, file, sizeof(cur_map));
    s_level(file);                          /* (its sounds: its monsters') */
    bench_views = cur_map[4] == '2' ? bench_demo2 : bench_demo1;
    models_load_all();
    g_overlays_load();                      /* (the code of the monsters it has: after their models) */
    vdp_tex_release(vram_base);
    hud_init();
    render_init();
    trace_init();
    movers_init();
    g_init();
    render_sky_init(file);
    models_hot();
    level_trace_hot();                      /* (after the models: what HWRAM's left) */
    r_portals_level();                      /* (and the portals' flow: what's left of that) */
    r_wall_level();
    fx_reset();
    for (i = 0; i < MAX_ENTITIES; ++i)
    {
        ents[i].live = false;
        ents[i].g_leaf = -1;
    }
    for (i = 0; spot && i < lv.nstarts; ++i)
        if (same(lv.starts[i].name, at))
        {
            origin = lv.starts[i].origin;
            yaw = (int)(((s64)lv.starts[i].angle * 0x10000 / 360) >> 16);
        }
    if (!spot)
        yaw = lv.start_yaw;
    pmove_spawn(origin);
    cam.yaw = yaw & 0xFFFF;
    cam.pitch = 0;
    if (keep)
    {
        client = was;
        client.fire_time = client.quad_until = client.invul_until = client.pickup_flash = 0;
        g_player->health = health;
    }
    view_level_init();                      /* (the gun you hold: after the above) */
    r_dspm_level();
#ifdef LEVEL_TEST
    {
        extern u32 models_cold;

        level_free(&lt_hw[lt_nhw & 3][0], &lt_hw[lt_nhw & 3][1], &lt_hw[lt_nhw & 3][2]);      /* (what's left, each level) */
        lt_hw[lt_nhw & 3][3] = models_cold;
        lt_fl[lt_nhw & 3] = (u32)(r_level_flags() | view_nslots() << 2 | models[MDL_GUNNER].loaded << 4
                                  | models[MDL_BERSERK].loaded << 7 | lt_count(MDL_BERSERK) << 8 | models[MDL_TANK].loaded << 12
                                  | ((u32)g_edicts >> 24 == 0x02) << 5 | ((u32)g_shorts >> 24 == 0x02) << 6);
        {
            extern u32 level_back;

            lt_hw[lt_nhw & 3][3] = level_back;  /* (in place of COLD: models_cold's always been 0) */
        }
        ++lt_nhw;
    }
#endif
    loading_back(false);
    return true;
}

/* the level from the start: its movers, its monsters and items (at the skill chosen), you */
static __attribute__((cold)) void         new_game(void)
{
    movers_init();
    g_init();
    view_reset();
    pmove_spawn(lv.start);
    cam.yaw = lv.start_yaw;
    cam.pitch = 0;
#ifdef GUNNER_TEST
    {
        /* (MAP=demo2 OPT=-DGUNNER_TEST: in front of Installation's gunner, facing it, god mode) */
        static const s32 at[3] = { FIX(240), FIX(-1560), FIX(30) };      /* (its ledge) */

        pmove_spawn(at);
        cam.yaw = 0xD000;
        god = true;
    }
#endif
#ifdef EXIT_TEST
    {
        /* (OPT=-DEXIT_TEST: on demo1's lift down to its exit, facing its button) */
        static const s32 at[3] = { FIX(EXIT_TEST == 2 ? -1480 : -1768), FIX(1536), FIX(EXIT_TEST == 2 ? 200 : 128) };

        pmove_spawn(at);
        pl.noclip = EXIT_TEST == 2;
        cam.yaw = 0x8000;
        cam.pitch = EXIT_TEST == 2 ? 0x0700 : 0;
    }
#endif
#ifdef WATER_TEST
    {
        /* (OPT=-DWATER_TEST=1: flying over demo1's pool, looking down at the water; =2: in it) */
        static const s32 at[2][3] = { { FIX(384), FIX(420), FIX(-196) }, { FIX(384), FIX(700), FIX(-300) } };

        pmove_spawn(at[WATER_TEST - 1]);
        pl.noclip = true;
        cam.yaw = 0x4000;
        cam.pitch = WATER_TEST == 1 ? 0x1400 : 0;
    }
#endif
#ifdef LADDER_CHECK
    {
        /* (in front of demo1's ladder, facing it) */
        static const s32 at[3] = { FIX(388), FIX(-60), FIX(-200) };

        pmove_spawn(at);
        cam.yaw = 0x4000;
    }
#endif
}

#ifdef HW_BENCH
/* (HW_BENCH, the run done) the three on one screen (0.1 ms; uploads a frame turning), and what the
   level left */
static __attribute__((cold)) void hb_screen(int page)
{
    /* (the text is VDP1's overlay commands, ~400: two pages, A between them; the gun's hidden) */
    extern bool r_dsp_ok;
    extern u32 gun_dma_bad;
    u32         n = fight_n ? fight_n : 1, hw, lw, ca;
    int         v, t, jobs = r_dsp_jobs(), y = 12;
    u16         w = RGB(255, 255, 255), hd = RGB(255, 220, 120), g = RGB(160, 255, 160);

    vdp_printf(8, y, hd, "HW BENCH %s %s", VDP2_TVSTAT & 1 ? "PAL" : "NTSC",
               !r_use_dsp ? "NO DSP" : jobs == 0 ? "DSP MODELS" : jobs == 1 ? "MODELS+MAKER"
               : jobs == 2 ? "MODELS+WALLS" : "MODELS+WALLS+MAKER");
    y += 13;
#ifdef SLAVE_PROF
    if (page == 2 || page == 3)
    {
        ht_prof_page(page == 2, fight_n, y);    /* (the fight's: the master's, the slave's) */
        return;
    }
#endif
#ifdef HW_TEST
    if (page >= 4)
    {
        ht_page(page - 4, y);                   /* (the timings, at boot) */
        return;
    }
#endif
    if (page)
    {
        /* each view: CPU and frame (0.1 ms), held and turned; the walk (0.1 ms) held, uploads x 10 turned */
        vdp_text(8, y, hd, "  --- HELD ---   -- TURNED --");
        y += 10;
        vdp_text(8, y, hd, "V  CPU  FRM WLK |  CPU  FRM UPL");
        y += 10;
        for (v = 0; v < NBENCH; ++v, y += 10)
            vdp_printf(8, y, w, "%d %4d %4d %3d | %4d %4d %3d", v + 1, hb_v[0][v][0], hb_v[0][v][1],
                       hb_v[0][v][2] / 10, hb_v[1][v][0], hb_v[1][v][1], hb_v[1][v][2]);
        y += 4;
        vdp_text(8, y, g, "A: NEXT PAGE");
        return;
    }
    level_free(&hw, &lw, &ca);
    for (t = 0; t < 2; ++t, y += 10)
        vdp_printf(8, y, w, "%s CPU %d FRM %d %s %d", t ? "TURNED" : "HELD  ", hb_v[t][NBENCH][0],
                   hb_v[t][NBENCH][1], t ? "UPL" : "WALK", hb_v[t][NBENCH][2] / (t ? 1 : 10));
    vdp_printf(8, y, w, "LATE %d %d MADE %d %d BAD %d", hb_late[0], hb_late[1], hb_made[1][0], hb_made[1][1],
               hb_made[1][2]);
    y += 14;
    vdp_printf(8, y, w, "FIGHT %d.%d CPU %d.%d GAME %d.%d", fight_us / n / 1000, fight_us / n / 100 % 10,
               fight_cpu / n / 1000, fight_cpu / n / 100 % 10, fight_game / n / 1000, fight_game / n / 100 % 10);
    y += 10;
    vdp_printf(8, y, w, "UP 1:%d 2:%d 3:%d 4:%d DROP %d %d", fight_swaps[1], fight_swaps[2], fight_swaps[3],
               fight_swaps[4] + fight_swaps[5] + fight_swaps[6] + fight_swaps[7], fight_drop[0], fight_drop[1]);
    y += 14;
    vdp_printf(8, y, g, "HW %d LW %d CA %d SL %d", hw, lw, ca, r_tex_slots());
    y += 10;
    vdp_printf(8, y, g, "DSP %s%s GUN %d", r_dsp_ok ? "OK" : "BAD", r_use_dsp ? " ON" : " OFF", gun_dma_bad);
    y += 14;
    vdp_text(8, y, hd, "A: NEXT PAGE  START+R: AGAIN");
}
#endif

void                main(void)
{
    u32             t_last, t0, us_frame = 0, us_cpu = 0;
    int             waited = 0;
    bool            paused = false;

#ifdef JUNK_RAM
    {
        extern void junk_fill(void);

        junk_fill();                        /* (OPT=-DJUNK_RAM: memory as a Saturn's is at power-on) */
    }
#endif
    frt_init();
#ifndef NO_SLAVE
    slave_start();
    wait_signal();                              /* the slave says hello */
    slave_ok = true;
#endif
    vdp_init(RGB(0, 0, 0));
    vdp_set_hw_erase(false);
    vdp_set_min_frame(1);
#ifndef NO_PIPE
    vdp_set_pipelined(true);                    /* (OPT=-DNO_PIPE: submit waits for the swap) */
#endif
    if (cd_init())
        back_ok = cd_load("CONBACK.BIN", (void *)VDP2_VRAM, 0x20000) == 0x20000;
#ifdef HW_TEST
    message("QUAKE II", "TIMING THE HARDWARE");
    ht_run();                                   /* (OPT=-DHW_TEST: the timing suite, before a level's in memory) */
#endif
    message("QUAKE II", "LOADING DEMO1 ONTO THE RAM CART");
    bench_views = MAP_FILE[4] == '2' ? bench_demo2 : bench_demo1;      /* "DEMO2.MAP" */
#ifdef SKY_VIEWS
    bench_views = bench_sky;
#endif
    if (!level_load(MAP_FILE))
    {
        if (cart_mb < 4)
            for (;;)
                message("THIS NEEDS THE 4MB RAM CART", NULL);
        load_failed(MAP_FILE);
    }
    message("QUAKE II", level_tries ? "LOADING THE MODELS (SHORT READS)" : "LOADING THE MODELS");
    models_load_all();
    g_overlays_load();                      /* (the code of the monsters it has: after their models) */
    message("QUAKE II", "LOADING THE SOUNDS");
    BT(0, 30);
    s_init(cur_map);
    vram_base = vdp_tex_mark();
    BT(0, 31);
    hud_init();
    BT(0, 32);
    render_init();
#ifdef BOOT_TRACE
    {
        extern s32 dsp_test[8];
        extern bool r_dsp_ok;
        bt_dma_test();
        dsp_init_models();                  /* (its program back after the test's) */
        bt_fmt(bt_dsp, "DSP %s %d %d %d %d/%d %d %d %d", r_dsp_ok ? "OK" : "BAD", dsp_test[0], dsp_test[1],
               dsp_test[2], dsp_test[3], dsp_test[4], dsp_test[5], dsp_test[6], dsp_test[7]);
        bt_line(186, bt_dsp);               /* (expected: 1 106 211 316 twice: from HWRAM, from the cart) */
        vdp_stall_hook = bt_stall;
        vdp_set_field_hook(bt_watch);
        bt_crash_handlers();
        step_hook = bt_master_step;
    }
#endif
    BT(0, 33);
    trace_init();
    BT(0, 34);
    movers_init();
    BT(0, 35);
    g_init();
    BT(0, 36);
    render_sky_init(MAP_FILE);
    BT(0, 37);
    models_hot();
    BT(0, 38);
    level_trace_hot();                      /* (after the models: what HWRAM's left) */
    BT(0, 39);
    r_portals_level();                      /* (and the portals' flow: what's left of that) */
    BT(0, 40);
    r_wall_level();
    BT(0, 41);
    view_level_init();
    BT(0, 42);
    r_dspm_level();
    loading_back(false);
    BT(0, 43);
#ifdef BOOT_TRACE
    {
        char buf[48];

        int k;

        memcpy(buf, bt_dsp, sizeof(buf));
        bt_line(186, buf);                  /* (again: the layer's just been cleared) */
        for (k = 0; k < 3; ++k)
        {
            memcpy(buf, bt_dma[k], sizeof(buf));
            bt_line(116 + k * 10, buf);
        }
    }
#endif
#ifdef LEVEL_TEST
    {
        extern u32 models_cold;

        level_free(&lt_hw[0][0], &lt_hw[0][1], &lt_hw[0][2]);
        lt_hw[0][3] = models_cold;
        lt_fl[0] = (u32)(r_level_flags() | view_nslots() << 2 | models[MDL_GUNNER].loaded << 4
                         | models[MDL_BERSERK].loaded << 7 | lt_count(MDL_BERSERK) << 8 | models[MDL_TANK].loaded << 12
                         | ((u32)g_edicts >> 24 == 0x02) << 5 | ((u32)g_shorts >> 24 == 0x02) << 6);
        {
            extern u32 level_back;

            lt_hw[0][3] = level_back;
        }
        ++lt_nhw;
    }
#endif
    pmove_spawn(lv.start);
    cam.yaw = lv.start_yaw;
    cam.pitch = 0;
    /* trace costs, for the stats line */
    {
        static const s32 zero[3] = { 0, 0, 0 };
        s32 a[3], b[3];
        u32 t;
        int k, n;

        for (k = 0; k < 3; ++k)
            a[k] = b[k] = pl.origin[k];
        b[0] -= FIX(400);
        b[1] += FIX(300);
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_line(a, b, 0, CONTENTS_SOLID);
        bench_us[0] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, zero, zero, 0, CONTENTS_SOLID);
        bench_us[1] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, p_mins, p_maxs, 0, MASK_PLAYERSOLID);
        bench_us[2] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        b[0] = a[0] + FIX(10);
        b[1] = a[1];
        b[2] = a[2] - FIX(18);
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, p_mins, p_maxs, 0, MASK_PLAYERSOLID);
        bench_us[3] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        {
            q_trace tl = trace_line(a, b, 0, CONTENTS_SOLID);

            bench_us[4] = tl.fraction;
        }
    }
    cycles_measure();
    t_last = frt_read();
    pad_by_vblank(vdp_get_pipelined());       /* (the SMPC's reads in the picture: engine/sys.c) */
    pad_request();
    for (;;)
    {
        q_usercmd   cmd;
        bool        pre_slave;
#ifndef NO_PREMOVE
        bool        premoved;
#endif
#if defined(FIGHT_BENCH) || defined(MOVE_TEST)
        /* (the benchmark: the game's step two fields always, not the frame's time: then the
           fight goes the same way each run and each build, however long its frames take; its
           FRAME is still the frames' time) */
        s32         dt = (s32)(((u64)(VDP2_TVSTAT & 1 ? 40000 : 33367) << 16) / 1000000);
#else
        s32         dt = (s32)(((u64)imax(imin((s32)us_frame, 100000), 10000) << 16) / 1000000);
#endif
        int         turn = (int)fmul(dt, 0x6000);   /* 135 degrees a second */

        FT(0);
#ifdef BOOT_TRACE
        ++*(volatile u32 *)UNCACHED(&bt_frame);
#ifdef WATCH_TEST
        if (*(volatile u32 *)UNCACHED(&bt_frame) == 100)
            for (;;)
                ;                           /* (OPT="-DBOOT_TRACE -DWATCH_TEST": stuck, for the watchdog) */
#endif
#ifdef CRASH_TEST
        if (*(volatile u32 *)UNCACHED(&bt_frame) == 100)
            __asm__ volatile (".word 0xFFFF");      /* (OPT="-DBOOT_TRACE -DCRASH_TEST": an illegal instruction) */
#endif
#endif
        BT(0, 1);

#ifdef SOUND_TEST
        {
            /* (OPT=-DSOUND_TEST: the blaster hard left, then hard right, then an explosion in the middle) */
            static int sf;

            ++sf;
            if (sf == 60)
                snd_sfx_at(SND_BLASTER, 127, -15);
            if (sf == 120)
                snd_sfx_at(SND_BLASTER, 127, 15);
            if (sf == 180)
                snd_sfx_at(SND_EXPLOSION, 127, 0);
        }
#endif
#ifndef NO_PREMOVE
        premoved = false;
        if (pm_asked)
        {
            /* (the slave moving you already: done, and its pad) */
#ifdef FIGHT_BENCH
            u32 tw = frt_read();
#endif

            BT(0, 2);
            while (!PM_DONE)
                ;
            BT(0, 3);
#ifdef FIGHT_BENCH
            pm_t[2] = (frt_read() - tw) & 0xFFFF;
#endif
            cache_purge();
            premoved = PM_DONE == 2;
            s_queue_flush();                    /* (the move's sounds: the 68000's ring is the master's to post to) */
#ifdef FIGHT_BENCH
            if (fight_frames > FIGHT_SKIP)
            {
                fight_pm[0] += frt_to_us(pm_t[0]);
                fight_pm[1] += frt_to_us(pm_t[1]);
                fight_pm[2] += frt_to_us(pm_t[2]);
                fight_pm[3] += premoved;
            }
#endif
            PM_REQ = 0;
            PM_DONE = 0;
            pm_asked = false;
        }
        PM_DRAWN = 0;
        pad_prev = pad_now;
        pad_now = premoved ? (u16)PM_PAD : pad_collect();
#else
        pad_prev = pad_now;
        pad_now = pad_collect();
#endif
        pad_request();
        t0 = frt_read();
        /* a menu up: it has the presses and the game stands still (START + R still starts a benchmark) */
        paused = false;
        if (menu_active() && !(pad_now & PAD_START && pressed(PAD_R)))
        {
            menu_action a = menu_input((u16)(pad_now & ~pad_prev));

#if !defined(GUNNER_TEST) && !defined(SLOT_CHECK)
            if (a == MA_NEW_GAME && !same(cur_map, "DEMO1.MAP"))
#if defined(NEW_GAME_DEMO3) || defined(NEW_GAME_DEMO)
                /* (OPT=-DNEW_GAME_DEMO=2 or 3, with MAP= the same: a test, a new game on that level) */
#ifdef NEW_GAME_DEMO3
                load_level("demo3", NULL, false);
#else
                load_level(NEW_GAME_DEMO == 2 ? "demo2" : "demo3", NULL, false);
#endif
#else
                load_level("demo1", NULL, false);       /* a new game's from the first level */
#endif
#else
            if (0)
                ;                                       /* (the level it starts on stays: MAP=) */
#endif
            else if (a == MA_NEW_GAME || a == MA_RESTART || a == MA_TITLE)
                new_game();
            paused = menu_active();
            if (pad_now & PAD_START)
                start_used = true;              /* (letting go of a START that chose something isn't a pause) */
        }
        if (g_player->dead || level_complete)
        {
            /* dead: START to go again (from the start, with what you had); level done: the level again */
            pad_now &= PAD_START;
            if (pad_prev & PAD_START && !(pad_now & PAD_START))
            {
                if (level_complete)
                {
                    /* through the exit to the next level (its name, then the start you come in at);
                       not on the disc (the demo's victory screen): the end, back to the title */
                    const char *n = g_next_map, *spot = NULL, *d;

                    if (n)
                    {
                        for (d = n; *d && *d != '$'; ++d)
                            ;
                        if (*d == '$')
                            spot = d + 1;
                    }
                    if (!n || !load_level(n, spot, true))
                    {
                        if (!same(cur_map, "DEMO1.MAP"))
                            load_level("demo1", NULL, false);
                        else
                            new_game();
                        menu_open(MENU_MAIN);
                    }
                    g_next_map = NULL;
                }
                else
                {
                    pmove_spawn(lv.start);
                    cam.yaw = lv.start_yaw;
                    cam.pitch = 0;
                    g_player->dead = false;
                    g_player->health = 100;
                }
                start_used = true;              /* not the pause menu too */
            }
        }
        if (pad_now & PAD_START && pad_now & (PAD_A | PAD_B | PAD_C | PAD_X | PAD_Y | PAD_Z | PAD_L | PAD_R))
            start_used = true;
        if (pad_prev & PAD_START && !(pad_now & PAD_START))
        {
            if (!start_used && !menu_active())
            {
                menu_open(MENU_PAUSE);              /* on letting go, unless it was START + A/B... */
                s_play(SND_MENU_SELECT, NULL, ATTN_NONE);
                paused = true;
            }
            start_used = false;
        }
        if (!paused)
        {
            if (pressed(PAD_Y))
            {
                if (pad_now & PAD_START)
                {
                    pl.noclip = !pl.noclip;
                    start_used = true;
                }
                else
                    g_next_weapon();
            }
            if (pressed(PAD_A) && (pad_now & PAD_START) && nents)
                warp_ent(false);
            if (pressed(PAD_C) && (pad_now & PAD_START) && nents)
                warp_ent(true);
            if (pressed(PAD_X) && (pad_now & PAD_START))
                god = !god;
            if (pressed(PAD_UP) && (pad_now & PAD_START))
            {
                game_during_draw = !game_during_draw;
                g_centerprint(game_during_draw ? "Game during drawing: on" : "Game during drawing: off");
                start_used = true;
            }
#ifdef FIGHT_BENCH
#ifdef HW_BENCH
            if (hb_go && hb_stage == 3)
#else
            if (pressed(PAD_R) && (pad_now & PAD_START))
#endif
            {
                static const s32 at[3] = { FIX(600), FIX(-428), FIX(-96) };
                int         i;

#ifdef HW_BENCH
                hb_go = false;
#endif

                menu_cur = MENU_NONE;           /* (from anywhere: the level afresh, everything in it) */
                g_skill = -1;
                rng_seed(0x2545F491);           /* (and the same dice: the same fight each run) */
                new_game();
                god = true;
                pmove_spawn(at);
                for (i = 1; i < g_nfull; ++i)
                {
                    g_ent   *e = &g_edicts[i];

                    if (e->kind == EK_MONSTER && !e->inactive && e->health > 0 && !e->enemy
                        && iabs(e->origin[0] - at[0]) < FIX(900) && iabs(e->origin[1] - at[1]) < FIX(900))
                    {
                        e->enemy = g_player;
                        FoundTarget(e);
                    }
                }
                fight_frames = 0;
                fight_done = false;
#ifdef SLAVE_PROF
                {
                    static bool started;
                    extern void prof_isr_m(void);
                    u32         *vbr;

                    if (!started)
                    {
                        /* (the master's table: the BIOS's) */
                        __asm__ volatile ("stc vbr,%0" : "=r" (vbr));
                        prof_start(vbr, prof_isr_m);
                        started = true;
                    }
                    memset((void *)0x202F0000, 0, 0x10000);
                    *(volatile u32 *)0x202E7FF8 = 0;
                    *(volatile u32 *)0x202E7FFC = 0;
                    PROF_ON = 1;
                }
#endif
                counts_reset();
                fight_ldma = fight_us = fight_cpu = fight_game = fight_gmax = fight_n = fight_ntr = fight_ttr = 0;
                memset(fight_r, 0, sizeof(fight_r));
                memset(fight_seg, 0, sizeof(fight_seg));
                memset(fight_pre, 0, sizeof(fight_pre));
                memset(fight_dl, 0, sizeof(fight_dl));
                memset(fight_po, 0, sizeof(fight_po));
                memset(fight_sl, 0, sizeof(fight_sl));
                memset(fight_pm, 0, sizeof(fight_pm));
#ifdef DSP_WALLS
                memset(fight_why, 0, sizeof(fight_why));
                {
                    extern u32 dw_stat[6];

                    memset(dw_stat, 0, sizeof(dw_stat));
                }
#endif
                fight_gun = 0;
                {
                    extern u32 view_ph[4];

                    memset(view_ph, 0, sizeof(view_ph));
                }
#ifdef R_PROFILE
                memset(fight_p, 0, sizeof(fight_p));
#endif
            }
            if (fight_frames >= 0)
            {
                cam.yaw = 0x4000;                   /* standing still, looking into the room */
                cam.pitch = 0;
                pad_now &= PAD_START;
            }
#endif
#if !defined(FIGHT_BENCH) || defined(HW_BENCH)
            if (pressed(PAD_R) && (pad_now & PAD_START))
            {
#ifdef HW_BENCH
                hb_stage = 1;                   /* (the views held first) */
                bench_turn = false;
                fight_done = false;
                {
                    extern u32 mk_total[3];

                    memset(mk_total, 0, sizeof(mk_total));
                }
#endif
                menu_cur = MENU_NONE;           /* (from anywhere: the level afresh, everything in it) */
                g_skill = -1;
                new_game();
                bench_view = 0;
                bench_frame = 0;
                bench_done = false;
                memset(bench_acc, 0, sizeof(bench_acc));
#ifdef R_PROFILE
                memset(bench_ax, 0, sizeof(bench_ax));
#endif
                counts_reset();
#ifdef R_PROFILE
                {
                    extern int wk_nodes, wk_leaves, wk_ftests, wk_models;

                    wk_nodes = wk_leaves = wk_ftests = wk_models = 0;
                }
#endif
                memset(bench_prof, 0, sizeof(bench_prof));
                memset(bench_drop, 0, sizeof(bench_drop));
            }
#endif
            if (pressed(PAD_L) && (pad_now & PAD_START))
                warp_trigger();
            if (pressed(PAD_Z) && (pad_now & PAD_START))
            {
                int w;

                for (w = 0; w < W_COUNT; ++w)
                    client.have[w] = true;
                client.ammo[AMMO_SHELLS] = 100;
                client.ammo[AMMO_BULLETS] = 200;
                client.ammo[AMMO_GRENADES] = 50;
                client.ammo[AMMO_ROCKETS] = 50;
                g_centerprint("All weapons");
            }
            if (god)
                g_player->health = imax(g_player->health, 100);
            else if (pressed(PAD_B) && pad_now & PAD_START)
                warp_next();
#ifndef NO_PREMOVE
            if (!premoved)
#endif
                move_input(pad_now, turn, &cmd);  /* (premoved: the slave's done it, and moved you) */
        }
        if (bench_view >= 0)
        {
            /* the benchmark: the camera where the table says, the game paused */
            const s32 *bv;

#ifdef BENCH_HOLD
            /* (OPT=-DBENCH_HOLD: a view held, DOWN for the next, UP to switch the cells'
               assembly on and off: the two should be identical, pixel for pixel) */
            {
                if (pressed(PAD_DOWN))
                    bench_view = (bench_view + 1) % NBENCH;
#if defined(COMPARE_MODELS)
                if (pressed(PAD_UP))
                {
                    extern bool r_model_ref;

                    r_model_ref = !r_model_ref;
                }
#elif defined(COMPARE_FAR)
                if (pressed(PAD_UP))
                {
                    /* (COMPARE=far [CMP_EXTRA=-DFAR_B=n]: the monsters' coarse mesh from MODEL_FAR
                       or from n units, 200 if not said) */
#ifndef FAR_B
#define FAR_B           (200)
#endif
                    extern int r_model_far;
                    static int other = FAR_B;
                    int t = r_model_far;

                    r_model_far = other;
                    other = t;
                }
#else
                if (pressed(PAD_UP))
                {
                    extern bool r_cells_asm;

                    r_cells_asm = !r_cells_asm;
                }
#endif
                bench_frame = 0;
            }
#endif
            bv = bench_views[bench_view];
#ifdef USER_VIEWS
            {
                /* (OPT="-DBENCH_HOLD -DUSER_VIEWS=..." : views of your own, x y z of where you
                   stood (the stats screen's), yaw; the eye 22 above) */
                static const s32 uv[][5] = { USER_VIEWS };

                bv = uv[bench_view % (int)(sizeof(uv) / sizeof(uv[0]))];
            }
#endif

            cam.pos[0] = FIX(bv[0]);
            cam.pos[1] = FIX(bv[1]);
            cam.pos[2] = FIX(bv[2]);
            cam.yaw = (int)bv[3];
            if (bench_turn)
            {
                cam.yaw = (cam.yaw + bench_frame * (65536 / 90)) & 0xFFFF;
                view_on = true;             /* (the turns with the gun up: its textures in the cache too) */
            }
            cam.pitch = (int)bv[4];
#ifdef FAR_LINEUP
            far_lineup(bench_view);
#endif
            cam_update();
            render_sky();
            lights_lag();                   /* (last frame's lights, for the models') */
            fx_update(0);
            g_render_ents();
            g_render_late(ents_pvs());
            fx_render();
#ifdef DL_BENCH
            {
                /* (OPT=-DDL_BENCH: three dynamic lights in each view, as a fight's: ahead, right, left) */
                static const s8 at[3][3] = { { 96, 0, -8 }, { 64, 80, 16 }, { 64, -80, 0 } };  /* forward, right, up */
                int         k, c;

                for (k = 0; k < 3 && r_ndlights < MAX_DLIGHTS; ++k)
                {
                    q_dlight *l = &r_dlights[r_ndlights++];

                    for (c = 0; c < 3; ++c)
                        l->pos[c] = cam.pos[c] + cam.fwd[c] * at[k][0] + cam.right[c] * at[k][1] + cam.up[c] * at[k][2];
                    l->radius = FIX(200);
                    l->r = 13;
                    l->g = 11;
                    l->b = 5;
                }
            }
#endif
            ents_light();
            vdp_begin();
            r_two_cpus = slave_ok;
#ifdef ONE_CPU
            r_two_cpus = false;             /* (OPT=-DONE_CPU: the master alone, to see what sharing gains) */
#endif
            render_world(vdp_get_writer(0), vdp_get_writer(1));
            vdp_printf(8, 8, RGB(255, 255, 255), "BENCHMARK VIEW %d", bench_view + 1);
            us_cpu = frt_to_us((frt_read() - t0) & 0xFFFF);
            waited = vdp_submit();
            t0 = frt_read();
            us_frame = frt_to_us((t0 - t_last) & 0xFFFF);
            t_last = t0;
            if (bench_frame >= 2)           /* the first two: textures settling */
            {
                u32 *a = bench_acc[bench_view];

                if (bench_turn)
                    a[0] += (u32)rs.uploads * 100;
                else
                    a[0] += (u32)rs.nodes;
                a[1] += rs.t_face;
                a[2] += rs.t_grid;
                a[3] += us_cpu;
                a[4] += us_frame;
                a[5] += (u32)waited;
                a[6] += vdp_us_dma;
                bench_prof[0] += rs.p_setup;
                bench_prof[1] += rs.p_grid;
                bench_prof[2] += rs.p_cells;
                bench_prof[3] += rs.p_slow;
                bench_prof[4] += rs.t_models;
                bench_prof[6] += (u32)rs.nslow;
                bench_prof[7] += (u32)rs.faces;
                bench_prof[8] += rs.t_mlight;
                bench_prof[9] += rs.t_mverts;
                bench_prof[10] += rs.t_mpolys;
                bench_prof[11] += (u32)rs.nexact;
                bench_prof[13] += rs.t_mfar;
                bench_prof[14] += (u32)rs.mfar;
                bench_drop[0] += (u32)rs.dropped;
                bench_drop[1] += (u32)rs.nocache;
                bench_prof[12] += (u32)rs.models;
                bench_prof[5] += rs.us_tree;
#ifdef R_PROFILE
                bench_ax[0] += rs.p_xform;
                bench_ax[1] += rs.p_cpre;
                bench_ax[2] += (u32)rs.n_rows;
                bench_ax[7] += (u32)rs.n_rfaces;
                bench_ax[3] += (u32)rs.cells;
                bench_ax[4] += rs.p_casm;
                bench_ax[5] += (u32)rs.n_casm;
                bench_ax[6] += (u32)rs.n_calls;
#endif
#ifdef OCC_COUNT
                bench_prof[0] += (u32)rs.occ_faces * 100;
                bench_prof[1] += (u32)rs.occ_cells * 100;
                bench_prof[2] += (u32)rs.occ_occluders * 100;
                bench_prof[3] += (u32)rs.faces * 100;
                bench_prof[12] += (u32)rs.cells * 100;
#endif

            }
            if (++bench_frame == BENCH_FRAMES)
            {
                bench_frame = 0;
                if (++bench_view == NBENCH)
                {
                    bench_view = -1;
                    bench_done = true;
                    counts_at_end();
                    pmove_spawn(pl.origin);
#ifdef HW_BENCH
                    {
                        /* (this run of views kept; then the views turned, or the fight) */
                        extern u32 mk_total[3];
                        int     v, t = bench_turn, n = BENCH_FRAMES - 2;

                        memset(hb_v[t][NBENCH], 0, sizeof(hb_v[t][NBENCH]));
                        for (v = 0; v < NBENCH; ++v)
                        {
                            hb_v[t][v][0] = bench_acc[v][3] / (u32)n / 100;
                            hb_v[t][v][1] = bench_acc[v][4] / (u32)n / 100;
                            hb_v[t][v][2] = bench_acc[v][0] * 10 / (u32)n / 100;   /* (0.01 ms; uploads x 10) */
                            hb_v[t][NBENCH][0] += hb_v[t][v][0];
                            hb_v[t][NBENCH][1] += hb_v[t][v][1];
                            hb_v[t][NBENCH][2] += hb_v[t][v][2];
                        }
                        hb_late[t] = at_end[12];
                        memcpy(hb_made[t], mk_total, sizeof(mk_total));
                        bench_done = false;
                        if (!t)
                        {
                            bench_turn = true;
                            bench_view = 0;
                            bench_frame = 0;
                            memset(bench_acc, 0, sizeof(bench_acc));
                            memset(bench_drop, 0, sizeof(bench_drop));
                            counts_reset();
                            memset(mk_total, 0, sizeof(mk_total));
                            hb_stage = 2;
                        }
                        else
                        {
                            hb_stage = 3;
                            hb_go = true;           /* (the fight, as START + R would) */
                        }
                    }
#endif
                }
            }
            continue;
        }
#ifdef LEVEL_TEST
        /* (OPT=-DLEVEL_TEST: each level's exit in turn, 80 frames in) */
        if (!paused && !level_complete && !g_player->dead)
        {
            static int lt_frames, lt_n;
            static const char *exits[] = { "demo2$base1", "demo3$base2a", "demo2$base3b", "victory.pcx" };

            if (++lt_frames == 80 && lt_n < 4)
            {
                g_next_map = exits[lt_n++];
                level_complete = true;
                lt_frames = 0;
            }
        }
#endif
        FT(1);
#ifdef NO_PRE_SLAVE
        pre_slave = false;                      /* (OPT=-DNO_PRE_SLAVE: all of it on the master, as before) */
#else
        pre_slave = r_two_cpus && game_during_draw;
#endif
        if (pre_pending)
        {
            PRE_CAM = 1;                        /* (a frame that didn't draw: the last one's first job) */
            pre_wait();
            BT(0, 4);
            while (!LIT_DONE)
                ;
        }
        lights_lag();                           /* (last frame's lights, for the models': model.c) */
        if (pre_slave)
        {
            BT(0, 4);
            while (!LIT_DONE)
                ;                               /* (the last job all done: its "done" not taken for this one's) */
            PRE_CAM = 0;
            PRE_DONE = 0;
            LIT_DONE = 0;
            PRE_JOB = 1;
            pre_pending = true;
            signal_slave();                     /* (the entities' list, then their light) */
            BT(0, 5);
        }
        if (!paused)
        {
#ifdef FIGHT_BENCH
            fight_pt = frt_read();
#endif
#ifndef NO_PREMOVE
            if (!premoved)
#endif
            {
                BT(0, 6);
                movers_update(dt);
                PRE(0);
                pmove(&cmd, dt);
                s_queue_flush();
            }
            PRE(1);
            FT(2);
            game_dt = dt;
            if (!game_during_draw)
                game_step();
        }
        else
            FT(2);
        FT(3);
        if (paused && menu_at_title())
        {
            /* the title: turning slowly where the level starts */
            static int title_yaw;

            title_yaw = (title_yaw + (int)fmul(dt, 0x0C00)) & 0xFFFF;
            cam.pos[0] = lv.start[0];
            cam.pos[1] = lv.start[1];
            cam.pos[2] = lv.start[2] + FIX(22);
            cam.yaw = (lv.start_yaw + title_yaw) & 0xFFFF;
            cam.pitch = 0;
        }
        else
        {
            cam.pos[0] = pl.origin[0];
            cam.pos[1] = pl.origin[1];
            cam.pos[2] = pl.origin[2] + (g_player->dead ? FIX(-8) : FIX(22));   /* the eyes (dead: on the floor) */
        }
#ifdef VIEW_TEST
        {
            /* (OPT=-DVIEW_TEST: no title; every gun, the next one every 2 seconds) */
            static int vt;
            int w;

            if (menu_cur == MENU_MAIN)
            {
                menu_cur = MENU_NONE;
                paused = false;
            }
            for (w = 0; w < W_COUNT; ++w)
                client.have[w] = true;
            client.ammo[AMMO_SHELLS] = client.ammo[AMMO_BULLETS] = client.ammo[AMMO_GRENADES] = 50;
            client.ammo[AMMO_ROCKETS] = 50;
            if (++vt % 50 == 0)
                g_next_weapon();
        }
#endif
#ifdef FIGHT_BENCH
        fight_pt = frt_read();
#endif
        cam_update();
        if (pre_slave)
            PRE_CAM = 1 + ents_pvs();
        view_on = bench_view < 0 && !(paused && menu_at_title());
#ifdef HW_BENCH
        if (hb_stage == 4)
            view_on = false;                /* (the screen's text wants the overlay's commands) */
#endif
        BT(0, 7);
        view_update(paused ? 0 : dt);
        PRE(2);
        render_sky();
        fx_update(paused ? 0 : dt);
        PRE(3);
        if (!paused)
            r_clock += (u32)dt;
        BT(0, 8);
        if (!pre_slave)
        {
            g_render_ents();
            g_render_late(ents_pvs());
        }
        fx_render();
        PRE(4);
        if (!pre_slave)
            ents_light();
        PRE(5);
        /* VDP2's colour offset over everything: under water a tint, and on top a hit's red
           flash or a pickup's yellow */
        {
            extern s32 player_flash;
            static bool flashing;
            int         r = 0, g = 0, b = 0, c = lv.leafs[level_leaf(cam.pos)].contents;

            if (c & (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA))
            {
                /* Quake's blends (lava 1 .3 0 at .6, slime 0 .1 .05 at .6, water .5 .3 .2 at .4)
                   as offsets: an offset can only add, so each is the blend's pull on a
                   middling colour, softened (the Saturn's picture's darker than Quake's) */
                static const s16 tint[3][3] = { { 100, 14, -24 }, { -24, -8, -18 }, { 20, 8, 0 } };
                const s16   *t = tint[c & CONTENTS_LAVA ? 0 : c & CONTENTS_SLIME ? 1 : 2];

                r = t[0]; g = t[1]; b = t[2];
            }
            if (client.pickup_flash > 0)
                client.pickup_flash -= dt;
            if (player_flash)
            {
                r += player_flash >> 1;
                g -= player_flash >> 3;
                b -= player_flash >> 3;
            }
            else if (client.pickup_flash > 0)
            {
                int y = client.pickup_flash >> 10;      /* up to ~19 */

                r += y * 2;                             /* Quake's yellow pickup flash */
                g += y * 2;
            }
            if (r | g | b)
            {
                vdp_color_offset_all(iclamp(r, -255, 255), iclamp(g, -255, 255), iclamp(b, -255, 255));
                flashing = true;
            }
            else if (flashing)
            {
                vdp_color_offset_off();
                flashing = false;
            }
        }
        r_pre_wait = pre_slave ? pre_wait : NULL;
        r_lit_wait = pre_slave ? lit_wait : NULL;
        PRE(6);
        FT(4);
        BT(0, 9);
        vdp_begin();
        r_two_cpus = slave_ok;
#ifdef ONE_CPU
            r_two_cpus = false;             /* (OPT=-DONE_CPU: the master alone, to see what sharing gains) */
#endif
        if (game_during_draw && !paused)
            r_during = game_step;
#ifdef DSP_SWAP_TEST
        dsp_wait();
        dsp_init_models();                      /* (the models' program back) */
#endif
#ifndef NO_PREMOVE
        if (pre_slave && r_two_cpus && !paused && bench_view < 0)
        {
            PM_DT = dt;
            PM_REQ = 1;                         /* (the slave: the next frame's move, after its drawing) */
            pm_asked = true;
        }
#endif
        BT(0, 10);
        render_world(vdp_get_writer(0), vdp_get_writer(1));
        BT(0, 19);
        r_during = NULL;
#ifdef SLOT_CHECK
        {
            extern void g_slot_check(void);

            g_slot_check();
        }
#endif
#ifndef NO_PREMOVE
        PM_DRAWN = 1;
#endif
        FT(5);
        /* the status bar and messages; a menu over them */
        if (!(paused && menu_at_title()))
            hud_draw();
        if (paused)
            menu_draw();
#ifndef HW_BENCH
        if (bench_done)
        {
            u32 tot[5] = { 0, 0, 0, 0, 0 };
            int v, k, n = BENCH_FRAMES - 2;

#ifdef TURN_BENCH
            vdp_text(8, 96, RGB(255, 220, 120), "V UPLD MAST SLAV CPU FRM  WT DMA");
# ifdef UPLOAD_CHECK
            {
                extern u32 upload_checks, upload_diffs;

                vdp_printf(8, 30, RGB(255, 255, 120), "UPLOADS %d DIFF %d", upload_checks, upload_diffs);
            }
# endif
#else
            vdp_text(8, 96, RGB(255, 220, 120), "V WALK MAST SLAV CPU FRM  WT DMA");
#endif
            for (v = 0; v < NBENCH; ++v)
            {
                u32 *a = bench_acc[v];

                vdp_printf(8, 106 + v * 9, RGB(255, 255, 255), "%d %4d %4d %4d %4d %4d %3d %3d", v + 1, a[0] / n / 100,
                           a[1] / n / 100, a[2] / n / 100, a[3] / n / 100, a[4] / n / 100, (int)(a[5] * 10 / (u32)n),
                           a[6] / n / 100);
                for (k = 0; k < 5; ++k)
                    tot[k] += a[k] / n;
            }
            vdp_printf(8, 106 + NBENCH * 9, RGB(255, 220, 120), "A %4d %4d %4d %4d %4d", tot[0] / 100,
                       tot[1] / 100, tot[2] / 100, tot[3] / 100, tot[4] / 100);
            n *= NBENCH;
#ifdef FACE_CHECK
            {
                extern u32 face_checks, face_diffs, face_rows3, face_lods;

                vdp_printf(8, 70, RGB(255, 255, 120), "FACES %d DIFF %d ROWS %d FAR %d", face_checks, face_diffs,
                           face_rows3, face_lods);
            }
#endif
#ifdef R_PROFILE
            vdp_printf(8, 88, RGB(160, 255, 160), "XFORM %d CPRE %d ROWS %d RF %d CELLS %d", bench_ax[0] / n / 100,
                       bench_ax[1] / n / 100, bench_ax[2] / n, bench_ax[7] / n, bench_ax[3] / n);        /* (n: frames x views, here) */
            vdp_printf(8, 79, RGB(160, 255, 160), "CASM %d CMDS %d CALLS %d", bench_ax[4] / n / 100, bench_ax[5] / n,
                       bench_ax[6] / n);
#endif
            vdp_printf(8, 106 + (NBENCH + 1) * 9, RGB(160, 255, 160), "S%d G%d C%d K%d L%d M%d T%d",
                       bench_prof[0] / n / 100, bench_prof[1] / n / 100, bench_prof[2] / n / 100, bench_prof[12] / n / 100,
                       bench_prof[3] / n / 100, bench_prof[4] / n / 100, bench_prof[5] / n / 100);
            {
#ifdef WALK_CHECK
                {
                    extern int walk_total, walk_frames;

                    vdp_printf(8, 106 + (NBENCH + 3) * 9, RGB(255, 255, 120), "WALK DIFFS %d IN %d FRAMES", walk_total,
                               walk_frames);
                }
#endif
                vdp_printf(8, 106 + (NBENCH + 2) * 9, RGB(160, 255, 160), "MODELS %d, FAR %d: %d.%dMS DROP %d FULL %d",
                           bench_prof[12] / n, bench_prof[14] / n, bench_prof[13] / n / 1000, bench_prof[13] / n / 100 % 10,
                           bench_drop[0], bench_drop[1]);
                vdp_printf(8, 106 + (NBENCH + 3) * 9, RGB(255, 200, 160), "OUT M%d S%d OF %d LATE %d CMD %d %d %d",
                           at_end[0], at_end[1], NBENCH * BENCH_FRAMES, at_end[12], at_end[2], at_end[3], at_end[4]);
#ifdef TEX_WSET
                vdp_printf(8, 106 + (NBENCH + 4) * 9, RGB(255, 200, 160), "TEX M%d/%d S%d/%d OF %d",
                           at_end[9] / (NBENCH * BENCH_FRAMES), at_end[7], at_end[10] / (NBENCH * BENCH_FRAMES), at_end[8],
                           at_end[11]);
#endif
            }
        }
#endif
#ifdef HW_BENCH
        if (hb_stage == 4)
        {
            static int page;

            if (pressed(PAD_A))
                page = (page + 1) % HB_PAGES;
            hb_screen(page);
        }
#endif
#if defined(FIGHT_BENCH) && !defined(HW_BENCH)  /* (HW_BENCH: its own screen instead) */
        if (fight_done)
        {
            u32 n = fight_n ? fight_n : 1;
#ifdef MODEL_CHECK
            {
                extern u32 model_checks[4], model_diffs[3];

                vdp_printf(8, 30, RGB(255, 255, 120), "VERTS %d DIFF %d CMDS %d DIFF %d", model_checks[0], model_diffs[0],
                           model_checks[3], model_diffs[2]);
                vdp_printf(8, 39, RGB(255, 255, 120), "BUCKETS %d DIFF %d QUADS %d", model_checks[1], model_diffs[1],
                           model_checks[2]);
            }
#endif
#ifdef ENTLIGHT_CHECK
            {
                extern u32 el_checks, el_diffs;

                vdp_printf(8, 39, RGB(255, 255, 120), "MODELS DRAWN %d STALE LIGHT %d", el_checks, el_diffs);
            }
#endif
#ifdef DL_CHECK
            {
                extern u32 dl_checks, dl_diffs;

                vdp_printf(8, 30, RGB(255, 255, 120), "DLIGHT CORNERS %d DIFF %d", dl_checks, dl_diffs);
            }
#endif
#ifdef TRACE_CHECK
            {
                extern u32 trace_checks, trace_diffs, trace_dkind[4], trace_later;
                extern s32 trace_worst;

                vdp_printf(8, 30, RGB(255, 255, 120), "CHECKED %d DIFF %d LATER %d WORST %d", trace_checks,
                           trace_diffs, trace_later, trace_worst);
                vdp_printf(8, 39, RGB(255, 255, 120), "START %d ALL %d PLANE %d NORMAL %d", trace_dkind[0],
                           trace_dkind[1], trace_dkind[2], trace_dkind[3]);
            }
#endif

#ifdef TICK_PROF
            {
                extern u32 tp_us[8], tp_n[8], tp_worst, tp_tick_us, tp_ticks, tp_part[3];
                extern const char *tp_name[8], *tp_worst_name;
                extern int tp_worst_tr;
                int k, y = 2;

                {
                    extern u32 tp_full, tp_most, tp_thinks, tp_think_us, tp_think_worst, tp_think_cls;

                    vdp_printf(8, y, RGB(255, 255, 120), "TICK %dUS TRIG%d ITEM%d N%d FULL%d MOST%d",
                               tp_tick_us / imax(tp_ticks, 1), tp_part[0] / imax(tp_ticks, 1),
                               tp_part[1] / imax(tp_ticks, 1), tp_ticks, tp_full, tp_most);
                    y += 9;
                    vdp_printf(8, y, RGB(255, 255, 120), "THINKS %d %dUS WORST %dUS CLS %d", tp_thinks,
                               tp_think_us / imax(tp_ticks, 1), tp_think_worst, tp_think_cls);
#ifdef CHAIN_CHECK
                    {
                        extern u32 chain_checks, chain_diffs;

                        y += 9;
                        vdp_printf(8, y, RGB(255, 255, 120), "USES %d CHAIN DIFFS %d", chain_checks, chain_diffs);
                    }
#endif
                }
                y += 9;
#define NC(s, i)    ((s)[i] ? (s)[i] : ' ')
                vdp_printf(8, y, RGB(255, 255, 120), "WORST %dUS %c%c%c%c TR%d", tp_worst, NC(tp_worst_name, 0),
                           NC(tp_worst_name, 1), NC(tp_worst_name, 2), NC(tp_worst_name, 3), tp_worst_tr);
                y += 9;
                {
                    extern u32 tp_chase[8];

                    vdp_printf(8, y, RGB(255, 255, 120), "CHASE %d %d %d %d %d %d %d+ UP%d", tp_chase[1],
                               tp_chase[2], tp_chase[3], tp_chase[4], tp_chase[5], tp_chase[6], tp_chase[7], tp_chase[0]);
                    y += 9;
                }
                for (k = 0; k < 8 && tp_name[k]; ++k, y += 9)
                    vdp_printf(8, y, RGB(160, 255, 160), "%c%c%c%c N%d %dUS %dUS/F", NC(tp_name[k], 0),
                               NC(tp_name[k], 1), NC(tp_name[k], 2), NC(tp_name[k], 3), tp_n[k],
                               tp_us[k] / imax(tp_n[k], 1), tp_us[k] / n);
#undef NC
#ifdef FIGHT_TRACES
                vdp_printf(8, y, RGB(255, 200, 160), "BOX %d G%d C%d M%d E%d", fight_tr[4] / n, fight_tt[0] / 10,
                           fight_tt[1] / 10, fight_tt[2] / 10, fight_tt[3] / 10);
#endif
            }
#endif
            vdp_printf(8, 96, RGB(255, 220, 120), "FIGHT: %d FRAMES", fight_n);
#ifdef MF_PROF
            {
                const u32 *mf_t = fight_mf, *mf_v = fight_mf + 4, *mf_m = fight_mf + 8;
                int k;

                for (k = 0; k < 4; ++k)
                    vdp_printf(8 + (k & 1) * 160, 80 + (k >> 1) * 8, RGB(255, 160, 255), "%c%c%d %dV %dM",
                               k < 2 ? 'D' : 'C', k & 1 ? 'F' : 'W', frt_to_us(mf_t[k]) / n, mf_v[k] / n,
                               mf_m[k] * 10 / n);
            }
#endif
#ifdef DSP_SWAP_TEST
            vdp_printf(8, 80, RGB(255, 255, 120), "SWAPS %d BAD %d BUSY %d", swap_n[0], swap_n[1], swap_n[2]);
#endif
            /* (how many pictures were up 1, 2, 3 and 4+ fields: 30 fps on NTSC is all of them 2) */
            vdp_printf(8, 196, RGB(255, 200, 160), "CMDS MOST M%d S%d GUN %d DROP %d FULL %d", vdp_peak[0], vdp_peak[1],
                       vdp_peak[2], fight_drop[0], fight_drop[1]);
            vdp_printf(8, 187, RGB(255, 220, 120), "FIELDS UP 1:%d 2:%d 3:%d 4+:%d", fight_swaps[1], fight_swaps[2],
                       fight_swaps[3], fight_swaps[4] + fight_swaps[5] + fight_swaps[6] + fight_swaps[7]);
            vdp_printf(8, 106, RGB(255, 255, 255), "FRAME %d.%d CPU %d.%d MS", fight_us / n / 1000,
                       fight_us / n / 100 % 10, fight_cpu / n / 1000, fight_cpu / n / 100 % 10);
            vdp_printf(8, 115, RGB(255, 255, 255), "GAME %d.%d MS, MOST %d.%d", fight_game / n / 1000,
                       fight_game / n / 100 % 10, fight_gmax / 1000, fight_gmax / 100 % 10);
            vdp_printf(8, 124, RGB(255, 255, 255), "TRACES %d A FRAME, %d.%d MS", fight_ntr / n,
                       fight_ttr / n / 1000, fight_ttr / n / 100 % 10);
            vdp_printf(8, 133, RGB(255, 255, 255), "I%d P%d G%d B%d W%d A%d", fight_seg[0] / n / 100,
                       fight_seg[1] / n / 100, fight_seg[2] / n / 100, fight_seg[3] / n / 100, fight_seg[4] / n / 100,
                       fight_seg[5] / n / 100);
#define MS10(v)     (int)((v) / n / 100)
            vdp_printf(8, 142, RGB(160, 255, 160), "(0.1 MS) MASTER %d SLAVE %d", MS10(fight_r[0]), MS10(fight_r[1]));
            vdp_printf(8, 151, RGB(160, 255, 160), "MODELS %d.%d CPU %d.%d DSP %d.%d: %d", fight_r[2] * 10 / n / 10,
                       fight_r[2] * 10 / n % 10, fight_r[10] * 10 / n / 10, fight_r[10] * 10 / n % 10,
                       fight_r[11] * 10 / n / 10, fight_r[11] * 10 / n % 10, MS10(fight_r[7]));
            vdp_printf(8, 160, RGB(160, 255, 160), "L%d V%d(W%d A%d N%d) P%d C%d", MS10(fight_r[4]),
                       MS10(fight_r[5] - fight_r[4]), MS10(fight_r[8]), MS10(fight_r[12]), MS10(fight_r[13]),
                       MS10(fight_r[6] - fight_r[5]), MS10(fight_r[7] - fight_r[6]));
#ifdef R_PROFILE
            vdp_printf(8, 178, RGB(255, 200, 160), "S%d G%d C%d L%d F%d C%d X%d L%d", MS10(fight_p[0]),
                       MS10(fight_p[1]), MS10(fight_p[2]), MS10(fight_p[3]), fight_p[4] / n, fight_p[5] / n,
                       fight_p[6] / n, fight_p[7] / n);
            vdp_printf(8, 187, RGB(255, 200, 160), "CROP %d EXACT %d SMALL %d", fight_p[9] / n, fight_p[10] / n,
                       fight_p[12] / n);
#endif
            vdp_printf(8, 169, RGB(160, 255, 160), "UPLOAD %d.%d MODELS %d.%d GUN %d.%d", fight_r[9] / 1000 * 10 / n / 10,
                       fight_r[9] / 1000 * 10 / n % 10, fight_r[9] % 1000 * 10 / n / 10, fight_r[9] % 1000 * 10 / n % 10,
                       fight_gun / n / 1000, fight_gun / n / 100 % 10);
#ifdef DW_CHECK
            vdp_printf(8, 178, RGB(255, 200, 160), "DSP'S PTS %d DIFF %d MOST %d", fight_why[0], fight_why[1],
                       fight_why[2]);
#elif defined(DSP_WALLS)
            vdp_printf(8, 178, RGB(255, 200, 160), "NOT DSP'S: ROWS %d - %d NOJOB %d OUT %d", fight_why[0] * 10 / n,
                       fight_why[1] * 10 / n, fight_why[2] * 10 / n, fight_why[3] * 10 / n);
#else
            vdp_printf(8, 178, RGB(255, 200, 160), "LISTS' DMA %d US", fight_ldma / n);
#endif
#ifndef DSP_WALLS                           /* (the overlay's room: the DSP's line instead) */
            vdp_printf(8, 44, RGB(160, 220, 255), "PRE US M%d P%d V%d F%d", fight_pre[0] / n, fight_pre[1] / n,
                       fight_pre[2] / n, fight_pre[3] / n);
            vdp_printf(8, 53, RGB(160, 220, 255), "E%d L%d T%d R%d", fight_pre[4] / n, fight_pre[5] / n,
                       fight_pre[6] / n, fight_pre[7] / n);
#endif
#ifdef WALLS_AHEAD
            vdp_printf(8, 71, RGB(160, 220, 255), "DLIGHTS US SUMS %d FACES %d AHEAD %d", fight_dl[1] / n, fight_dl[2] / n,
                       fight_dl[5] / n);
#else
#ifdef FS_STATS
            {
                extern u32 fs_sum[11];

                vdp_printf(8, 71, RGB(255, 255, 120), "PT %d %d %d %d %d R %d %d", fs_sum[0], fs_sum[1], fs_sum[2],
                           fs_sum[3], fs_sum[4], fs_sum[5], fs_sum[6]);
                vdp_printf(8, 80, RGB(255, 255, 120), "L %d %d %d DL %d", fs_sum[7], fs_sum[8], fs_sum[9], fs_sum[10] * 10 / n);
            }
#else
            vdp_printf(8, 71, RGB(160, 220, 255), "WALL DL US %d FACES %d DSP %d", fight_dl[1] / n, fight_dl[2] / n,
                       fight_dl[5] / n);
#endif
#endif
            vdp_printf(8, 62, RGB(160, 220, 255), "SLAVE US: FIRST %d DYN %d END %d", fight_sl[0] / n,
                       fight_sl[1] / n, fight_sl[2] / n);
#ifdef WALLS_AHEAD
            {
                extern u32 wl_stop[3];

                vdp_printf(8, 80, RGB(160, 220, 255), "AHEAD STOPS: NEXT %d ROOM %d ALL %d", wl_stop[0], wl_stop[1],
                           wl_stop[2]);
            }
#elif defined(DSP_SWAP_TEST)
#elif defined(DSP_WALLS)
            {
                vdp_printf(8, 80, RGB(160, 220, 255), "DSP %d/%d RAN%d F%dUS BUSY%d %dUS", fight_dw[0] / n, fight_dw[2] / n,
                           fight_dw[1] * 100 / n, fight_dw[3] / n, fight_dw[4], fight_dw[5] / n);
            }
#elif !defined(DLF_CHECK)
            vdp_printf(8, 80, RGB(160, 220, 255), "EARLY MOVE W%d M%d MASTER %d %d%%", fight_pm[0] / n,
                       fight_pm[1] / n, fight_pm[2] / n, fight_pm[3] * 100 / n);
#endif

#ifdef DLF_CHECK
            {
                extern u32 dlf_checks, dlf_diffs;

                vdp_printf(8, 80, RGB(255, 255, 120), "DL FACES %d DIFF %d", dlf_checks, dlf_diffs);
            }
#endif
#ifdef DSPL_CHECK
            {
                extern u32 dspl_checks, dspl_diffs;

                vdp_printf(8, 26, RGB(255, 255, 120), "DSP LIGHT %d DIFF %d", dspl_checks, dspl_diffs);
            }
#endif
            vdp_printf(8, 205, RGB(255, 200, 160), "OUT M%d S%d OF %d LATE %d CMD %d %d %d", at_end[0], at_end[1],
                       fight_n + FIGHT_SKIP, at_end[12], at_end[2], at_end[3], at_end[4]);
#ifdef TEX_WSET
            vdp_printf(8, 214, RGB(255, 200, 160), "TEX M%d/%d S%d/%d OF %d", at_end[9] / (fight_n + FIGHT_SKIP),
                       at_end[7], at_end[10] / (fight_n + FIGHT_SKIP), at_end[8], at_end[11]);
#endif
#undef MS10
#ifdef FIGHT_TRACES
            {
                int j;

                vdp_printf(8, 20, RGB(255, 200, 160), "BOX %d: LOOKED%d L%d B%d S%d MOV%d", fight_tr[4] / n,
                           fight_tr[0] / imax(fight_tr[4], 1), fight_tr[1] / imax(fight_tr[4], 1),
                           fight_tr[2] / imax(fight_tr[4], 1), fight_tr[3] / imax(fight_tr[4], 1), fight_tr[5] / n);
#ifdef BOUNDS_CHECK
                {
                    extern u32 bounds_checks, bounds_diffs, bounds_skipped;

                    vdp_printf(8, 11, RGB(255, 255, 120), "BOUNDS %d DIFF %d SKIPPED %d", bounds_checks, bounds_diffs,
                               bounds_skipped);
                }
#endif
#ifdef LINE_CHECK
                {
                    extern u32 line_checks, line_diffs;

                    vdp_printf(8, 2, RGB(255, 255, 120), "LINES %d DIFF %d", line_checks, line_diffs);
                }
#endif
#ifdef FACE_CHECK
                {
                    extern u32 face_checks, face_diffs, face_rows3, face_lods;

                    vdp_printf(8, 2, RGB(255, 255, 120), "FACES %d DIFF %d ROWS %d FAR %d", face_checks, face_diffs,
                               face_rows3, face_lods);
                }
#endif
                vdp_printf(8, 29, RGB(255, 200, 160), "US G%d C%d M%d E%d LINES %d N%d US%d", fight_tt[0] / 10,
                           fight_tt[1] / 10, fight_tt[2] / 10, fight_tt[3] / 10, fight_tr[7] / n,
                           fight_tr[6] / imax(fight_tr[7], 1), fight_tt[4] / 10);
                for (j = 0; j < 4 && fight_sites[j].n; ++j)
                    vdp_printf(8, 38 + 9 * j, RGB(160, 255, 160), "%X %d.%d A FRAME %d.%dMS",
                               fight_sites[j].at & 0xFFFFF, fight_sites[j].n * 10 / n / 10,
                               fight_sites[j].n * 10 / n % 10, fight_sites[j].us / n / 1000,
                               fight_sites[j].us / n / 100 % 10);
            }
#endif
        }
#endif
#ifdef LEVEL_TEST
        {
            int k;

            for (k = 0; k < 3; ++k)
                vdp_printf(8, 30 + k * 9, RGB(255, 255, 120), "W%d K%d G%d N%d%d%dT%d E%d HW%d LW%d CA%d", lt_fl[k] & 1,
                           lt_fl[k] >> 1 & 1, lt_fl[k] >> 2 & 3, lt_fl[k] >> 4 & 1, lt_fl[k] >> 7 & 1, lt_fl[k] >> 8 & 15,
                           lt_fl[k] >> 12 & 1,
                           (lt_fl[k] >> 5 & 1) | (lt_fl[k] >> 6 & 1) << 1, lt_hw[k][0], lt_hw[k][1], lt_hw[k][2]);
        }
#endif
#if defined(LEVEL_TEST) && defined(FACE_CHECK)
        {
            extern u32 face_checks, face_diffs, face_rows3, face_lods;

            vdp_printf(8, 57, RGB(255, 255, 120), "FACES %d DIFF %d ROWS %d FAR %d", face_checks, face_diffs, face_rows3,
                       face_lods);
        }
#endif
        if (level_complete)
        {
            vdp_text(160 - 7 * 8, 60, RGB(255, 220, 120), "LEVEL COMPLETE");
            vdp_printf(160 - 9 * 8, 80, RGB(255, 255, 255), "KILLS   %d / %d", kills, total_monsters);
            vdp_printf(160 - 9 * 8, 92, RGB(255, 255, 255), "SECRETS %d / %d", found_secrets, total_secrets);
            if (g_next_map && g_next_map[0] == 'v')
                vdp_text(160 - 14 * 4, 116, RGB(255, 220, 120), "THE END OF THE DEMO");    /* ("victory.pcx") */
            vdp_text(160 - 20 * 4, 132, RGB(200, 200, 200), "PRESS START TO GO ON");
        }
        {
            if (opt_stats == 2)
                vdp_printf(8, 190, RGB(255, 255, 255), "LINE %d POINTBOX %d BOX %d SHORT %d", bench_us[0], bench_us[1],
                           bench_us[2], bench_us[3]);
            if (g_player->dead)
                vdp_text(160 - 11 * 8, 100, RGB(255, 80, 60), "YOU DIED - PRESS START");
        }
        if (opt_stats == 1)
        {
            /* (the options' STATISTICS FPS) the frame rate alone, over the last few frames */
            static u32 avg_us = 33333;

            avg_us = (avg_us * 7 + (u32)(us_frame ? us_frame : 1)) >> 3;
            vdp_printf(8, 8, RGB(255, 255, 255), "FPS %d.%d", 10000000 / avg_us / 10, 10000000 / avg_us % 10);
        }
        if (opt_stats == 2)
        {
            u16 c = RGB(255, 255, 255);

            {
                extern s32 cyc[12];

                /* cycles x 10 an operation (src/cycles.c) */
                vdp_printf(8, 88, c, "ST HW%d LW%d CA%d V1%d LD HIT%d UNC%d", cyc[1], cyc[2], cyc[10], cyc[11], cyc[3],
                           cyc[4]);
                vdp_printf(8, 98, c, "MISS HW%d LW%d CA%d DIV%d MUL%d", cyc[5], cyc[6], cyc[7], cyc[8], cyc[9]);
                extern int grid_bad;

                extern bool r_dsp_ok;

                vdp_printf(8, 108, c, "GRID PT%d BAD%d DSP %s%s HEAP %x", cyc[0], grid_bad, r_dsp_ok ? "OK" : "BAD",
                           r_use_dsp ? " ON" : "", level_heap());
                {
                    /* the DSP self-test's failed tries (work RAM, the cart) and the status port as the
                       last stopped; with OPT=-DDSP_SOAK the soak's hangs and wrong results (work RAM,
                       the cart; then the same watched with pauses), hangs first after a reload, the
                       status at the first */
                    extern s32 dsp_fails[2];
                    extern u32 dsp_fail_ppaf[2], dsp_fail_dsta, gun_dma_bad, mk_total[3];

                    vdp_printf(8, 118, c, "DSP TRIES %d %d %x %x DMA %x GUN %d", dsp_fails[0], dsp_fails[1],
                               dsp_fail_ppaf[0], dsp_fail_ppaf[1], dsp_fail_dsta, gun_dma_bad);
                    vdp_printf(8, 138, c, "MADE DSP %d CPU %d BAD READS %d", mk_total[0], mk_total[1], mk_total[2]);
                    {
                        extern u32 snd_state(void);
                        u32 ss = snd_state();

                        /* (the sound's start: its stage (4 done), ready, the 68000 alive, its status, the bank) */
                        vdp_printf(8, 148, c, "SND STAGE %d READY %d ALIVE %d STATUS %d BANK %dK", ss & 15, ss >> 4 & 1,
                                   ss >> 5 & 1, ss >> 8 & 15, ss >> 16);
                    }
#ifdef DSP_SOAK
                    {
                        extern s32 soak_hang[4], soak_bad[4], soak_first;
                        extern u32 soak_ppaf;

                        vdp_printf(8, 128, c, "SOAK H%d %d %d %d W%d %d %d %d F%d %x", soak_hang[0], soak_hang[1],
                                   soak_hang[2], soak_hang[3], soak_bad[0], soak_bad[1], soak_bad[2], soak_bad[3],
                                   soak_first, soak_ppaf);
                    }
#endif
                }
#ifdef WALLS_TEST
                {
                    extern u32 wt_res[18];

                    vdp_printf(8, 126, c, "WALLS F%d P%d BAD F%d P%d US%d %s", wt_res[0], wt_res[1], wt_res[2], wt_res[3],
                               wt_res[4], wt_res[5] ? "DONE" : "STUCK");
                    vdp_printf(8, 135, c, "LIT %d NOT DL_FACE'S %d BY %d PICKS %d", wt_res[6], wt_res[7], wt_res[8],
                               wt_res[9]);
                    vdp_printf(8, 144, c, "BAD %d %x N%d F%dUS CA%dK LW%dK", wt_res[10], wt_res[12], wt_res[14],
                               wt_res[15], wt_res[16], wt_res[17]);
                }
#endif
#ifdef PPD_TEST
                {
                    extern u32 ppd_ticks;

                    vdp_printf(8, 117, c, "PPD WRITE X10 %d CYCLES", ppd_ticks * 128 * 10 / (4 * 125));
                }
#endif
#ifdef WALK_CHECK
                {
                    extern int walk_diff, walk_len, walk_clen, walk_first, walk_total, walk_frames;

                    vdp_printf(8, 118, c, "WALK DIFF %d ASM %d C %d AT %d", walk_diff, walk_len, walk_clen, walk_first);
                    vdp_printf(8, 30, c, "WALK TOTAL %d IN %d FRAMES", walk_total, walk_frames);
                }
#endif

            }
            {
                extern int g_dropped;

                vdp_printf(8, 48, c, "%d %d %d %X%s%s%s W%d D%d", pl.origin[0] >> 16, pl.origin[1] >> 16,
                           pl.origin[2] >> 16, cam.yaw, pl.on_ground ? " GROUND" : "", pl.noclip ? " NOCLIP" : "",
                           god ? " GOD" : "", pl.waterlevel, g_dropped);
            }
#ifdef LADDER_CHECK
            {
                extern u32 lc_frames, lc_near, lc_ladder, lc_bad;

                vdp_printf(8, 8, c, "LADDER F%d NEAR%d ON%d BAD%d", lc_frames, lc_near, lc_ladder, lc_bad);
            }
#elif defined(BOX_CHECK)
            {
                extern u32 box_checks, box_ldiffs, box_diffs;

                vdp_printf(8, 8, c, "BOX TRACES %d LEAVES DIFF %d DIFF %d", box_checks, box_ldiffs, box_diffs);
            }
#elif defined(CLIP_CHECK)
            {
                extern u32 clip_checks, clip_diffs, clip_near, clip_near_diffs;

                vdp_printf(8, 8, c, "CLIPPED %d DIFF %d NEAR %d DIFF %d", clip_checks, clip_diffs, clip_near,
                           clip_near_diffs);
            }
#elif defined(SLOT_CHECK)
            {
                extern u32 slot_frames, slot_missed, slot_peak, slot_moved, slot_full;

                vdp_printf(8, 8, c, "SLOTS F%d MISSED %d+%d FULL %d MOST %d", slot_frames, slot_missed, slot_moved,
                           slot_full, slot_peak);
            }
#elif defined(ENTLIGHT_CHECK)
            {
                extern u32 el_checks, el_diffs;

                vdp_printf(8, 8, c, "MODELS DRAWN %d STALE LIGHT %d", el_checks, el_diffs);
            }
#elif defined(VIEW_CHECK)
            {
                extern u32 view_checks[7];

                vdp_printf(8, 8, c, "GUN CHECK F%d CMDS %d DIFF %d", view_checks[0], view_checks[1], view_checks[2]);
#if VIEW_CHECK == 3
                vdp_printf(8, 208, c, "DSP V%d MOVED %d MOST %d NEAR %d", view_checks[3], view_checks[4],
                           view_checks[5], view_checks[6]);
#elif VIEW_CHECK == 2
                vdp_printf(8, 208, c, "KEPT: TURNED EDGE-ON %d", view_checks[3]);
#endif
            }
#else
            vdp_printf(8, 8, c, "FPS %d.%d  CPU %dMS  WAIT %d", 10000000 / (us_frame ? us_frame : 1) / 10,
                       10000000 / (us_frame ? us_frame : 1) % 10, us_cpu / 1000, waited);
#endif
            vdp_printf(8, 18, c, "FACES %d CELLS %d CULL %d NEAR %d", rs.faces, rs.cells, rs.culled, rs.near);
            vdp_printf(8, 28, c, "UPLOADS %d MK%d D%d R%d %dUS+%d FULL %d DROP %d G%d", rs.uploads, rs.made, rs.made_dsp,
                       rs.made_rest, rs.made_us, rs.mk_us, rs.nocache, rs.dropped, vdp_gover);
            vdp_printf(8, 38, c, "WALK %d MASTER %d SLAVE %d OUT %d %d", rs.nodes / 1000, rs.t_face / 1000,
                       rs.t_grid / 1000, r_full[0], r_full[1]);
#ifdef TEX_WSET
            {
                extern u32 r_wset[5];

                vdp_printf(8, 150, c, "TEX MOST %d %d OF %d", r_wset[0], r_wset[1], r_wset[4]);
            }
#endif
#ifdef LINE_CHECK
            {
                extern u32 line_checks, line_diffs;

                vdp_printf(8, 160, c, "LINES %d DIFF %d", line_checks, line_diffs);
            }
#endif
#ifdef WHERE_TEST
            vdp_printf(8, 140, c, "N%X P%X L%X", (u32)lv.nodes, (u32)lv.planes, (u32)lv.leafs);
            vdp_printf(8, 149, c, "B%X S%X LB%X BB%X", (u32)lv.brushes, (u32)lv.brushsides, (u32)lv.leafbrushes,
                       (u32)lv.brushbounds);
#endif
#if defined(GUNNER_TEST) && defined(FIGHT_BENCH)
            {
                /* (the gunner's fight, since the start: the line traces, and a box trace's parts) */
                extern u32 tr_count[8], tr_ticks[5];

                vdp_printf(8, 62, c, "LN %d N%d US%d BOX %d L%d B%d S%d MV%d", tr_count[7], tr_count[6] / imax(tr_count[7], 1),
                           frt_to_us(tr_ticks[4]) / imax(tr_count[7], 1), tr_count[4], tr_count[1] / imax(tr_count[4], 1),
                           tr_count[2] / imax(tr_count[4], 1), tr_count[3] / imax(tr_count[4], 1), tr_count[5]);
                vdp_printf(8, 80, c, "CAND %d", tr_count[0] / imax(tr_count[4], 1));
                vdp_printf(8, 71, c, "BOX US G%d C%d M%d E%d", frt_to_us(tr_ticks[0]) / imax(tr_count[4], 1),
                           frt_to_us(tr_ticks[1]) / imax(tr_count[4], 1), frt_to_us(tr_ticks[2]) / imax(tr_count[4], 1),
                           frt_to_us(tr_ticks[3]) / imax(tr_count[4], 1));
            }
#endif
            vdp_printf(8, 58, c, "MODELS %d POLYS %d %dUS", rs.models, rs.mpolys, rs.t_models);
            {
                extern int g_ntraces;
                extern u32 g_trace_ticks;
                static int nt;
                static u32 tt;

                if (us_game > 100)
                {
                    nt = g_ntraces;
                    tt = frt_to_us(g_trace_ticks);
                }
                {
                    extern int fx_explosions, g_uses;

                    vdp_printf(8, 68, c, "GAME %dUS TRACES %d %dUS", us_game, nt, tt);
                    vdp_printf(8, 78, c, "USES %d EXPL %d KILLS %d/%d", g_uses, fx_explosions, kills, total_monsters);
                }
                g_ntraces = 0;
                g_trace_ticks = 0;
            }

        }
#ifdef MOVE_TEST
        vdp_printf(8, 30, RGB(255, 255, 120), "MOVES %d AT %X %X %X", mt_moves, mt_at[0], mt_at[1], mt_at[2]);
        vdp_printf(8, 39, RGB(255, 255, 120), "YAW %X VZ %X", mt_at[3], mt_at[4]);
#endif
        FT(6);
        us_cpu = frt_to_us((frt_read() - t0) & 0xFFFF);
        BT(0, 20);
        waited = vdp_submit();
        BT(0, 21);
        t0 = frt_read();
        us_frame = frt_to_us((t0 - t_last) & 0xFFFF);
        t_last = t0;
#ifdef FIGHT_BENCH
        if (fight_frames >= 0)
        {
            extern int  g_ntraces;
            extern u32  g_trace_ticks;
            int         k;

            if (fight_frames >= FIGHT_SKIP)
            {
                fight_ntr += (u32)g_ntraces;
                fight_ttr += frt_to_us(g_trace_ticks);
            }
            g_ntraces = 0;
            g_trace_ticks = 0;
            if (++fight_frames == FIGHT_SKIP)
            {
                for (k = 0; k < 8; ++k)
                    fight_swaps[k] = vdp_swap_fields[k];
                memset(g_trace_sites, 0, sizeof(g_trace_sites));
                {
                    extern u32 tr_count[8], tr_ticks[5];

                    memset(tr_count, 0, sizeof(tr_count));
                    memset(tr_ticks, 0, sizeof(tr_ticks));
                }
#ifdef MF_PROF
                {
                    extern u32 mf_t[4], mf_v[4], mf_m[4];

                    memset(mf_t, 0, sizeof(mf_t));
                    memset(mf_v, 0, sizeof(mf_v));
                    memset(mf_m, 0, sizeof(mf_m));
                }
#endif
            }
            else if (fight_frames > FIGHT_SKIP)
            {
                fight_us += us_frame;
                fight_cpu += us_cpu;
                fight_ldma += vdp_us_dma;           /* (the lists' DMA, vdp_submit's: after us_cpu) */
                fight_game += us_game;
                fight_drop[0] += (u32)rs.dropped;
                fight_drop[1] += (u32)rs.nocache;
                {
                    int k;

                    for (k = 0; k < 6; ++k)
                        fight_seg[k] += frt_to_us((fight_t[k + 1] - fight_t[k]) & 0xFFFF);
                }
                fight_r[0] += rs.t_face;
                fight_r[1] += rs.t_grid;
                fight_r[2] += (u32)rs.models;
                fight_r[3] += (u32)rs.mpolys;
                fight_r[4] += rs.t_mlight;
                fight_r[5] += rs.t_mverts;
                fight_r[6] += rs.t_mpolys;
                fight_r[7] += rs.t_models;
                fight_r[8] += rs.t_mwait;
                fight_r[9] += (u32)rs.uploads * 1000 + (u32)rs.muploads;
                fight_r[10] += (u32)rs.mcpu;
                fight_r[11] += (u32)rs.mdsp;
#ifdef R_PROFILE
                fight_p[0] += rs.p_setup;
                fight_p[1] += rs.p_grid;
                fight_p[2] += rs.p_cells;
                fight_p[3] += rs.p_slow;
                fight_p[4] += (u32)rs.faces;
                fight_p[5] += (u32)rs.cells;
                fight_p[6] += (u32)rs.nexact;
                fight_p[7] += (u32)rs.nslow;
                fight_p[8] += (u32)rs.ns_dl;
                fight_p[9] += (u32)rs.ns_crop;
                fight_p[10] += (u32)rs.ns_exact;
                fight_p[11] += (u32)rs.g_same;
                fight_p[12] += (u32)rs.ns_small;
#endif
                fight_r[12] += rs.t_masm;
                fight_gun += rs.t_view;
                {
                    extern u32 view_ph[4];

                    memcpy(fight_vph, view_ph, sizeof(fight_vph));
                }
                fight_r[13] += rs.t_mnorm;
                {
                    int k;

                    for (k = 0; k < 7; ++k)
                        fight_pre[k] += fight_pref[k];
                }
                fight_pre[7] += rs.us_rwpre;
                fight_dl[0] += rs.t_dltest;
                fight_dl[1] += rs.t_dlsum;
                fight_dl[2] += (u32)rs.n_dlfaces;
                fight_dl[3] += (u32)rs.n_dlpts;
#ifdef DSP_WALLS
                {
                    extern u32 dw_stat[6];

                    memcpy(fight_dw, dw_stat, sizeof(fight_dw));    /* (as at the fight's last frame) */
                    fight_why[0] += (u32)rs.dw_why[0];
                    fight_why[1] += (u32)rs.dw_why[1];
#ifdef DW_CHECK
                    fight_why[2] = (u32)imax((int)fight_why[2], rs.dw_why[2]);
#else
                    fight_why[2] += (u32)rs.dw_why[2];
#endif
                    fight_why[3] += (u32)rs.dw_why[3];
                }
#endif
#ifdef FS_STATS
                {
                    extern u32 fs_sum[11];
                    extern int r_ndlights;
                    int k;

                    for (k = 0; k < 10; ++k)
                        fs_sum[k] += (u32)rs.fs[k];
                    fs_sum[10] += (u32)r_ndlights;
                }
#endif
                fight_dl[5] += (u32)rs.n_wlhit;
                fight_po[0] += rs.t_flow;
                fight_po[1] += (u32)rs.n_reach;
                fight_po[2] += (u32)rs.n_proj;
                fight_po[3] += (u32)rs.portal_out;
                fight_po[4] += (u32)rs.n_ptest;
                {
                    extern u32 sl_first;

                    fight_sl[0] += frt_to_us(*(volatile u32 *)UNCACHED(&sl_first));
                    fight_sl[1] += frt_to_us(*(volatile u32 *)UNCACHED(&sl_dyn));
                    fight_sl[2] += frt_to_us(*(volatile u32 *)UNCACHED(&sl_end));
                }
                fight_gmax = imax((s32)fight_gmax, (s32)us_game);
                ++fight_n;
                if (fight_us >= 20000000)
                {
                    for (k = 0; k < 8; ++k)
                        fight_swaps[k] = vdp_swap_fields[k] - fight_swaps[k];
                    memcpy(fight_sites, g_trace_sites, sizeof(fight_sites));
                    {
                        extern u32 tr_count[8], tr_ticks[5];

                        memcpy(fight_tr, tr_count, sizeof(fight_tr));
                        for (k = 0; k < 4; ++k)
                            fight_tt[k] = frt_to_us(tr_ticks[k]) * 10 / imax(tr_count[4], 1);
                        fight_tt[4] = frt_to_us(tr_ticks[4]) * 10 / imax(tr_count[7], 1);
                    }
                    for (k = 1; k < 16; ++k)
                    {
                        g_trace_site x = fight_sites[k];
                        int          j = k;

                        for (; j > 0 && fight_sites[j - 1].us < x.us; --j)
                            fight_sites[j] = fight_sites[j - 1];
                        fight_sites[j] = x;
                    }
                    fight_frames = -1;
                    fight_done = true;
#ifdef HW_BENCH
                    hb_stage = 4;                   /* (its screen, not the fight's) */
#endif
#ifdef MF_PROF
                    {
                        extern u32 mf_t[4], mf_v[4], mf_m[4];

                        memcpy(fight_mf, mf_t, sizeof(mf_t));      /* (as the fight ended) */
                        memcpy(fight_mf + 4, mf_v, sizeof(mf_v));
                        memcpy(fight_mf + 8, mf_m, sizeof(mf_m));
                    }
#endif
#ifdef SLAVE_PROF
                    PROF_ON = 0;
#endif
                    counts_at_end();
                }
            }
        }
#endif
    }
}
