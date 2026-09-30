/*
** VDP1/VDP2 without SGL - see vdp.h for the frame model.
**
** VDP1 VRAM layout (computed from VDP_MAX_CMDS)
**   0x00000  header: system clip, user clip, local coords, jump -> active list
**   0x00100  command list A        (MAX_CMDS * 32 bytes)
**            command list B
**            font, 4bpp            (128 chars * 32 bytes)
**            text colour tables    (16 * 32 bytes)
**            Gouraud tables A / B  (GOURAUD_MAX * 8 bytes each)
**            textures              (vdp_tex_upload, bump allocated to 0x7FFFF)
*/
#include <stdarg.h>
#include "vdp.h"
#include "font8x8_basic.h"

#define HDR_JUMP        (3)
#define LIST_BYTES      ((u32)MAX_CMDS * sizeof(vdp1_cmd))
#define LIST_A          (0x00100)
#define LIST_B          ((LIST_A + LIST_BYTES + 0xFF) & ~0xFFu)
#define FONT_VRAM       ((LIST_B + LIST_BYTES + 0xFF) & ~0xFFu)
#define LUT_VRAM        (FONT_VRAM + 128 * 32)
#define MAX_TEXT_COLORS (32)            /* kept from frame to frame: a list in flight may use any */
#ifndef VDP_GOURAUD_MAX
# define VDP_GOURAUD_MAX (1024)
#endif
#define GOURAUD_MAX     VDP_GOURAUD_MAX     /* per list; half each for the master and slave writers */
#define GOURAUD_A       (LUT_VRAM + MAX_TEXT_COLORS * 32)
#define GOURAUD_B       (GOURAUD_A + GOURAUD_MAX * 8)
#define TEX_VRAM        (GOURAUD_B + GOURAUD_MAX * 8)
#define VRAM_END        (0x80000)

#define CMD_USERCLIP    (0x0008)
#define CMD_SYSCLIP     (0x0009)
#define CMD_LOCAL       (0x000A)
#define JP_ASSIGN       (0x1000)
#define JP_SKIP_ASSIGN  (0x5000)
#define CMD_END         (0x8000)

#define OVL_FIRST       (1 + WRITER_CMDS + WRITER1_CMDS)
#define OVL_MAX         (MAX_CMDS - OVL_FIRST - 1)

static vdp1_cmd     staging[MAX_CMDS] __attribute__((aligned(16)));
static vdp_writer   writers[2];
static int          overlay_count;
static int          list;               /* 0 = A, 1 = B: the list being built */
static bool         drawing;            /* VDP1 has a list in flight */
static u16          lut_color[MAX_TEXT_COLORS];
static int          lut_count;
static u32          tex_next = TEX_VRAM;
static s16          clear_rect[4] = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
static u32          min_frame_ticks;    /* frame pacing, FRT ticks (0 = as fast as possible) */
static u32          frame_ticks = FRT_TICKS(16683u);            /* one NTSC field, FRT ticks */
#define ERASE_MARGIN    FRT_TICKS(1500u)                    /* 1.5ms safety before the vblank */
static u32          last_swap;
static bool         hw_erase;           /* VDP1 erases the displayed buffer in the spare vblank */
static bool         no_hw_erase;
static void         (*vblank_hook)(void);
u32                 vdp_us_dma, vdp_us_wait;
int                 vdp_peak[5];
u32                 late_frames;        /* frames that had to clear with a polygon */

/* Erase decision, made in the timer-0 interrupt a few lines before the
   vblank that follows each swap (see vdp_submit):
     ERASE_ARMED     a swap just happened; decide at the end of this field
     ERASE_REQUESTED VDP1 had finished: erase requested for the next field
     ERASE_DECLINED  VDP1 was still drawing: the next list clears itself */
enum { ERASE_IDLE = 0, ERASE_ARMED, ERASE_REQUESTED, ERASE_DECLINED };
static volatile int erase_state;
#define ERASE_LINE      (208)           /* NTSC: 224 visible lines, ~1ms before vblank */

static void         (*field_hook)(void);
static void         (*list_hook)(void);

/* Pipelined frames (vdp_set_pipelined): vdp_submit sends the list and returns
   at once, and the CPUs start the next frame. The timer-0 interrupt near the
   end of each field swaps to the waiting list once VDP1 has finished the one
   before (FBCR=3 then takes effect at that vblank), and the vblank interrupt
   runs the vblank hook for the picture now on screen. So three frames can be
   in hand at once: VDP1 drawing one, one waiting for its swap, one being
   built, and what a frame uses in VRAM (its textures) must stay put for two
   frames after it. The next submit waits only if the last list hasn't been
   swapped to yet (its slot is the one VDP1 may still be reading). */
#define SWAP_LINE       (216)           /* 8 lines before the vblank */
static bool         pipelined;
static volatile int queued = -1;        /* the list waiting for its swap, or -1 */
static volatile u32 queued_frame;
static volatile bool hook_due;
static u32          frames_sent;
volatile u32        vdp_shown;          /* the frame (vdp_frame_no's count) whose picture is on screen */
volatile u32        vdp_swap_fields[8]; /* pictures that stayed on screen 1, 2, ... 7+ fields (pipelined) */
static int          fields;

/* The interrupts' C runs as an ordinary function under the BIOS dispatcher;
   these wrappers keep everything the interrupted code may be using that C
   could change: r0-r7, PR, MACH/MACL (the renderer's MAC sums) and GBR
   (grid.s reaches the divider through it) */
#define ISR_WRAP(wrap, fn) \
    __asm__("        .text\n        .align  2\n_" #wrap ":\n" \
            "        sts.l   pr,@-r15\n        sts.l   mach,@-r15\n        sts.l   macl,@-r15\n" \
            "        stc.l   gbr,@-r15\n        mov.l   r0,@-r15\n        mov.l   r1,@-r15\n" \
            "        mov.l   r2,@-r15\n        mov.l   r3,@-r15\n        mov.l   r4,@-r15\n" \
            "        mov.l   r5,@-r15\n        mov.l   r6,@-r15\n        mov.l   r7,@-r15\n" \
            "        mov.l   1f,r0\n        jsr     @r0\n        nop\n" \
            "        mov.l   @r15+,r7\n        mov.l   @r15+,r6\n        mov.l   @r15+,r5\n" \
            "        mov.l   @r15+,r4\n        mov.l   @r15+,r3\n        mov.l   @r15+,r2\n" \
            "        mov.l   @r15+,r1\n        mov.l   @r15+,r0\n        ldc.l   @r15+,gbr\n" \
            "        lds.l   @r15+,macl\n        lds.l   @r15+,mach\n        lds.l   @r15+,pr\n" \
            "        rts\n        nop\n        .align  2\n1:      .long   _" #fn "\n")

void                swap_isr(void);
void                vblank_isr(void);
void                vblank_out_isr(void);
extern void         swap_isr_w(void);
extern void         vblank_isr_w(void);
extern void         vblank_out_isr_w(void);
ISR_WRAP(swap_isr_w, swap_isr);
ISR_WRAP(vblank_isr_w, vblank_isr);
ISR_WRAP(vblank_out_isr_w, vblank_out_isr);

void                swap_isr(void)
{
    ++fields;
    if (queued >= 0 && (VDP1_EDSR & 2) && scu_dma0_chain_done())
    {
        ++vdp_swap_fields[fields < 7 ? fields : 7];
        fields = 0;
        ((volatile u16 *)(VDP1_VRAM + HDR_JUMP * sizeof(vdp1_cmd)))[1] = (u16)((queued ? LIST_B : LIST_A) >> 3);
        VDP1_FBCR = 3;
        vdp_shown = queued_frame - 1;   /* VDP1 starts on that one; the one before it appears */
        hook_due = true;
        queued = -1;
    }
    if (field_hook)
        field_hook();
}

void                vblank_isr(void)
{
    if (hook_due)
    {
        hook_due = false;
        if (vblank_hook)
            vblank_hook();
    }
}

/* the pad (sys.c): the SMPC reads at the top of the picture */
void                vblank_out_isr(void)
{
    pad_vblank();
}

void                vdp_set_pipelined(bool on)
{
    if (hw_erase)
        return;                         /* (the erase's timing needs the swap where it is) */
    pipelined = on;
    if (on)
    {
        scu_timer0_start(SWAP_LINE, swap_isr_w);
        scu_vblank_in_start(vblank_isr_w);
        scu_vblank_out_start(vblank_out_isr_w);
    }
}

bool                vdp_get_pipelined(void)
{
    return pipelined;
}

u32                 vdp_frame_no(void)
{
    return frames_sent;
}

/* called by the BIOS interrupt dispatcher: an ordinary function */
static void         erase_isr(void)
{
    if (erase_state == ERASE_ARMED)
    {
        if (VDP1_EDSR & 2)
        {
            VDP1_FBCR = 2;
            erase_state = ERASE_REQUESTED;
        }
        else
            erase_state = ERASE_DECLINED;
    }
    if (field_hook)
        field_hook();                   /* 60 Hz work: the music sequencer */
}

static u32          list_base(int which)
{
    return which ? LIST_B : LIST_A;
}

static u16          link_to(int index)
{
    return (u16)((list_base(list) + (u32)index * sizeof(vdp1_cmd)) >> 3);
}

static void         upload_font(void)
{
    u8              *dst = (u8 *)(VDP1_VRAM + FONT_VRAM);
    int             c, row, x;

    for (c = 0; c < 128; ++c)
        for (row = 0; row < 8; ++row)
        {
            u8 bits = (u8)font8x8_basic[c][row];

            for (x = 0; x < 8; x += 2)
            {
                /* font8x8: bit 0 = leftmost pixel; 4bpp: high nibble = left pixel */
                u8 l = (bits >> x) & 1, r = (bits >> (x + 1)) & 1;

                dst[c * 32 + row * 4 + x / 2] = (u8)((l << 4) | r);
            }
        }
}

void                vdp_init(u16 back_color)
{
    volatile u16    *hdr = (volatile u16 *)VDP1_VRAM;
    int             i;

    /* VDP2: display off, clear registers, back screen colour, sprite layer on */
    VDP2_TVMD = 0;
    for (i = 2; i < 0x120; i += 2)
        REG16(VDP2_REG + i) = 0;
    REG16(VDP2_VRAM + 0x7FFFE) = back_color;
    VDP2_BKTAU = 0x0003;                /* back screen: last word of VRAM (bank B1) */
    VDP2_BKTAL = 0xFFFF;
    VDP2_SPCTL = 0x0020;                /* sprite type 0, RGB/palette mixed */
    VDP2_PRISA = VDP2_PRISB = VDP2_PRISC = VDP2_PRISD = 0x0606;

    /* VDP1: 16bpp, manual frame change, auto-start drawing after each change */
    VDP1_TVMR = 0;
    VDP1_FBCR = 0;
    VDP1_PTMR = 0;
    VDP1_EWDR = 0;
    VDP1_EWLR = 0;
    VDP1_EWRR = (u16)(((SCREEN_W >> 3) << 9) | (SCREEN_H - 1));

    /* header commands */
    for (i = 0; i < 4 * 16; ++i)
        hdr[i] = 0;
    hdr[0 * 16 + 0] = CMD_SYSCLIP;
    hdr[0 * 16 + 10] = SCREEN_W - 1;    /* xc */
    hdr[0 * 16 + 11] = SCREEN_H - 1;    /* yc */
    hdr[1 * 16 + 0] = CMD_USERCLIP;
    hdr[1 * 16 + 10] = SCREEN_W - 1;
    hdr[1 * 16 + 11] = SCREEN_H - 1;
    hdr[2 * 16 + 0] = CMD_LOCAL;        /* origin top-left */
    hdr[HDR_JUMP * 16 + 0] = JP_SKIP_ASSIGN;
    hdr[HDR_JUMP * 16 + 1] = (u16)(LIST_A >> 3);
    /* an empty list A so the first frame has something valid to run */
    ((volatile u16 *)(VDP1_VRAM + LIST_A))[0] = CMD_END;

    upload_font();
    VDP1_PTMR = 2;
    VDP1_FBCR = 3;
    VDP2_TVMD = 0x8000;                 /* display on, 320x224 */
    list = 1;
    drawing = true;
}

bool                vdp_tex_upload(vdp_tex *t, const u16 *pixels, int w, int h)
{
    u32             bytes = (u32)(w * h * 2);

    if (tex_next + bytes > VRAM_END || (w & 7))
        return false;
    memcpy((void *)(VDP1_VRAM + tex_next), pixels, bytes);
    t->srca = (u16)(tex_next >> 3);
    t->size = (u16)(((w >> 3) << 8) | h);
    t->w = (u16)w;
    t->h = (u16)h;
    t->colr = 0;
    t->pmode = PMOD_RGB;
    tex_next = (tex_next + bytes + 31) & ~31u;
    return true;
}

/* 4bpp texture + its 16-entry colour lookup table (index 0 = transparent) */
bool                vdp_tex_upload_lut(vdp_tex *t, const u8 *pixels, const u16 *lut, int w, int h)
{
    u32             bytes = (u32)(w * h / 2);

    if (tex_next + 32 + bytes > VRAM_END || (w & 7))
        return false;
    memcpy((void *)(VDP1_VRAM + tex_next), lut, 32);
    t->colr = (u16)(tex_next >> 3);
    tex_next += 32;
    memcpy((void *)(VDP1_VRAM + tex_next), pixels, bytes);
    t->srca = (u16)(tex_next >> 3);
    t->size = (u16)(((w >> 3) << 8) | h);
    t->w = (u16)w;
    t->h = (u16)h;
    t->pmode = PMOD_LUT4;
    tex_next = (tex_next + bytes + 31) & ~31u;
    return true;
}

/* 4bpp texture drawn with colour tables of its own (uploaded apart): no table here */
bool                vdp_tex_upload_4bpp(vdp_tex *t, const u8 *pixels, int w, int h)
{
    u32             bytes = (u32)(w * h / 2);

    if (tex_next + bytes > VRAM_END || (w & 7))
        return false;
    memcpy((void *)(VDP1_VRAM + tex_next), pixels, bytes);
    t->colr = 0;
    t->srca = (u16)(tex_next >> 3);
    t->size = (u16)(((w >> 3) << 8) | h);
    t->w = (u16)w;
    t->h = (u16)h;
    t->pmode = PMOD_LUT4;
    tex_next = (tex_next + bytes + 31) & ~31u;
    return true;
}

u32                 vdp_tex_free(void)
{
    return VRAM_END - tex_next;
}

u32                 vdp_tex_mark(void)
{
    return tex_next;
}

void                vdp_tex_release(u32 mark)
{
    tex_next = mark;
}

/* Gouraud tables: the master's half is shared by writer 0 and the overlays
   (both built on the master), the slave's writer has the other half. Both
   are staged in work RAM (writing VDP1 VRAM from a CPU stalls while VDP1
   draws) and go over by DMA with the list. */
static u32          gstage[GOURAUD_MAX * 2] __attribute__((aligned(16)));

u16                 vdp_gouraud_w(vdp_writer *w, u16 c0, u16 c1, u16 c2, u16 c3)
{
    return vdp_gouraud_fast(w, ((u32)c0 << 16) | c1, ((u32)c2 << 16) | c3);
}

u16                 vdp_gouraud(u16 c0, u16 c1, u16 c2, u16 c3)
{
    return vdp_gouraud_fast(&writers[0], ((u32)c0 << 16) | c1, ((u32)c2 << 16) | c3);
}

void                vdp_begin(void)
{
    int             i, w;
    vdp1_cmd        *c;

    while (!scu_dma0_chain_done())
        ;                               /* (the last lists still going out: vdp_submit's) */
    for (w = 0; w < 2; ++w)
    {
        vdp_writer *wr = &writers[w];

        wr->first = w ? 1 + WRITER_CMDS : 1;
        wr->cmax = w ? WRITER1_CMDS : WRITER_CMDS;
        wr->cmds = &staging[wr->first];
        wr->link_base = link_to(wr->first);
        wr->count = 0;
        for (i = 0; i < ZBUCKETS; ++i)
            wr->head[i] = -1;
        wr->gbase = (list ? GOURAUD_B : GOURAUD_A) + (u32)(w * GOURAUD_MAX / 2) * 8;
        wr->gcount = 0;
        wr->gmax = GOURAUD_MAX / 2;
        wr->gst = &gstage[w * GOURAUD_MAX];
    }
    overlay_count = 0;
    /* slot 0: clear (part of) the draw buffer to transparent - VDP2's back
       screen shows through. It costs VDP1 fill time, so scenes that cover the
       screen can shrink it with vdp_set_clear(). */
    c = &staging[0];
    memset(c, 0, sizeof(*c));
    c->ctrl = hw_erase ? JP_SKIP_ASSIGN : (JP_ASSIGN | VDP1_POLYGON);
    c->pmod = 0x00C0;
    c->colr = 0x0000;
    c->xa = clear_rect[0]; c->ya = clear_rect[1];
    c->xb = clear_rect[2]; c->yb = clear_rect[1];
    c->xc = clear_rect[2]; c->yc = clear_rect[3];
    c->xd = clear_rect[0]; c->yd = clear_rect[3];
}

void                vdp_set_min_frame(int vblanks)
{
    /* ask for the swap no earlier than (vblanks - 0.5) frames after the last
       one, so it lands on the vblank after that: 2 -> 30 fps NTSC, 25 fps PAL */
    u32             us = vblanks > 1 ? (u32)(vblanks * 2 - 1) * 8333u : 0;  /* (n - 0.5) NTSC frames */

    min_frame_ticks = FRT_TICKS(us);
    /* With 2+ vblanks per frame the one in between is free for a VDP1 manual
       erase: it wipes the displayed buffer line by line as it scans out, so it
       comes back clean for drawing. No clear polygon needed. */
    hw_erase = vblanks > 1 && !no_hw_erase;
    if (hw_erase)
        scu_timer0_start(ERASE_LINE, erase_isr);
}

/* the manual erase's timing was worked out for NTSC; this turns it off and
   every list clears its buffer with a polygon instead */
void                vdp_set_hw_erase(bool on)
{
    no_hw_erase = !on;
}

static u16          bgon_shadow;

void                vdp2_bgon(u16 set, u16 clear)
{
    bgon_shadow = (u16)((bgon_shadow | set) & ~clear);
    VDP2_BGON = bgon_shadow;
}

void                vdp_set_field_hook(void (*fn)(void))
{
    field_hook = fn;
}

void                vdp_set_list_hook(void (*fn)(void))
{
    list_hook = fn;
}

void                vdp_set_vblank_hook(void (*fn)(void))
{
    vblank_hook = fn;
}

void                vdp_set_clear(int x0, int y0, int x1, int y1)
{
    clear_rect[0] = (s16)x0;
    clear_rect[1] = (s16)y0;
    clear_rect[2] = (s16)x1;
    clear_rect[3] = (s16)y1;
}

vdp_writer          *vdp_get_writer(int which)
{
    return &writers[which];
}

vdp1_cmd            *vdp_overlay(void)
{
    vdp1_cmd        *c;

    if (overlay_count >= OVL_MAX)
        return NULL;
    c = &staging[OVL_FIRST + overlay_count++];
    c->ctrl = JP_ASSIGN;
    return c;
}

/* the overlay's next free commands, to be written in order (their links are made at the
   submit); vdp_overlay_add says how many were */
vdp1_cmd            *vdp_overlay_block(int *room)
{
    *room = OVL_MAX - overlay_count;
    return &staging[OVL_FIRST + overlay_count];
}

void                vdp_overlay_add(int n)
{
    overlay_count += n;
}

void                vdp_rect(int x0, int y0, int x1, int y1, u16 color, bool half_transparent)
{
    vdp1_cmd        *c = vdp_overlay();

    if (!c)
        return;
    c->ctrl |= VDP1_POLYGON;
    c->pmod = (u16)(PMOD_ECD | PMOD_SPD | (half_transparent ? PMOD_HALF_TRANS : 0));
    c->colr = color;
    c->xa = (s16)x0; c->ya = (s16)y0;
    c->xb = (s16)x1; c->yb = (s16)y0;
    c->xc = (s16)x1; c->yc = (s16)y1;
    c->xd = (s16)x0; c->yd = (s16)y1;
}

/* opaque rectangle, colour shaded from the top edge (g_top) to the bottom
   (g_bot): Gouraud values as vdp_gouraud_rgb(), 16 = unchanged */
void                vdp_rect_shaded(int x0, int y0, int x1, int y1, u16 color, u16 g_top, u16 g_bot)
{
    vdp1_cmd        *c = vdp_overlay();

    if (!c)
        return;
    c->ctrl |= VDP1_POLYGON;
    c->pmod = (u16)(PMOD_ECD | PMOD_SPD | PMOD_GOURAUD);
    c->colr = color;
    c->grda = vdp_gouraud(g_top, g_top, g_bot, g_bot);
    c->xa = (s16)x0; c->ya = (s16)y0;
    c->xb = (s16)x1; c->yb = (s16)y0;
    c->xc = (s16)x1; c->yc = (s16)y1;
    c->xd = (s16)x0; c->yd = (s16)y1;
}

void                vdp_frame(int x0, int y0, int x1, int y1, u16 color)
{
    vdp1_cmd        *c = vdp_overlay();

    if (!c)
        return;
    c->ctrl |= VDP1_POLYLINE;
    c->pmod = (u16)(PMOD_ECD | PMOD_SPD);
    c->colr = color;
    c->xa = (s16)x0; c->ya = (s16)y0;
    c->xb = (s16)x1; c->yb = (s16)y0;
    c->xc = (s16)x1; c->yc = (s16)y1;
    c->xd = (s16)x0; c->yd = (s16)y1;
}

void                vdp_sprite(const vdp_tex *t, int x, int y, bool half_transparent)
{
    vdp1_cmd        *c = vdp_overlay();

    if (!c)
        return;
    c->ctrl |= VDP1_SPRITE;
    c->pmod = (u16)(PMOD_ECD | t->pmode | (half_transparent ? PMOD_HALF_TRANS : 0));
    c->colr = t->colr;
    c->srca = t->srca;
    c->size = t->size;
    c->xa = (s16)x;
    c->ya = (s16)y;
}

static u16          text_lut(u16 color)
{
    int             i;
    volatile u16    *lut;

    for (i = 0; i < lut_count; ++i)
        if (lut_color[i] == color)
            return (u16)((LUT_VRAM + i * 32) >> 3);
    if (lut_count >= MAX_TEXT_COLORS)
        return (u16)(LUT_VRAM >> 3);
    lut = (volatile u16 *)(VDP1_VRAM + LUT_VRAM + lut_count * 32);
    lut[0] = 0;
    lut[1] = color;
    lut_color[lut_count] = color;
    return (u16)((LUT_VRAM + lut_count++ * 32) >> 3);
}

void                vdp_text(int x, int y, u16 color, const char *s)
{
    u16             lut = text_lut(color);
    vdp1_cmd        *c;

    for (; *s; ++s, x += 8)
    {
        if (*s == ' ')
            continue;
        if ((c = vdp_overlay()) == NULL)
            return;
        c->ctrl |= VDP1_SPRITE;
        c->pmod = PMOD_ECD | PMOD_LUT4;
        c->colr = lut;
        c->srca = (u16)((FONT_VRAM + (*s & 0x7F) * 32) >> 3);
        c->size = 0x0108;               /* 8x8 */
        c->xa = (s16)x;
        c->ya = (s16)y;
    }
}

/* for text drawn in the scene (road signs): a colour's table, a character's glyph (8x8, 4bpp) */
u16                 vdp_text_colr(u16 color)
{
    return text_lut(color);
}

u16                 vdp_glyph(int ch)
{
    return (u16)((FONT_VRAM + (ch & 0x7F) * 32) >> 3);
}

void                vdp_printf(int x, int y, u16 color, const char *f, ...)
{
    char            buf[128];
    va_list         ap;

    va_start(ap, f);
    vfmt(buf, f, ap);
    va_end(ap);
    vdp_text(x, y, color, buf);
}

#define VDP2_CLOFEN REG16(0x25F80110)
#define VDP2_CLOFSL REG16(0x25F80112)
#define VDP2_COAR   REG16(0x25F80114)
#define VDP2_COAG   REG16(0x25F80116)
#define VDP2_COAB   REG16(0x25F80118)

void                vdp_color_offset(int r, int g, int b)
{
    VDP2_COAR = (u16)(r & 0x1FF);
    VDP2_COAG = (u16)(g & 0x1FF);
    VDP2_COAB = (u16)(b & 0x1FF);
    VDP2_CLOFSL = 0;                    /* offset A for every layer */
    VDP2_CLOFEN = 0x0040;               /* enable on the sprite layer */
}

void                vdp_color_offset_off(void)
{
    VDP2_CLOFEN = 0;
}

void                vdp_color_offset_all(int r, int g, int b)
{
    VDP2_COAR = (u16)(r & 0x1FF);
    VDP2_COAG = (u16)(g & 0x1FF);
    VDP2_COAB = (u16)(b & 0x1FF);
    VDP2_CLOFSL = 0;
    VDP2_CLOFEN = 0x007F;               /* NBG0-3, RBG0, back screen, sprites */
}

int                 vdp_cmd_count(void)
{
    return 1 + writers[0].count + writers[1].count + overlay_count + 1;
}

/* (pipelined) the frame's transfers into VRAM, for SCU DMA's indirect mode (count, destination,
   source): its textures (vdp_dma_queue), then its lists */
#define DMA_TAB         (64)
static u32          dma_tab[3 * DMA_TAB] __attribute__((aligned(16)));
static int          dma_n;

static void         dma_add(u32 dst, const void *src, u32 bytes)
{
    u32             *e = &dma_tab[3 * dma_n++];

    e[0] = bytes;
    e[1] = dst & 0x07FFFFFF;
    e[2] = (u32)src & 0x07FFFFFF;
}

/* a transfer into VDP1's VRAM for the frame being built (from the cart or high work RAM, a
   source that stays put): sent with its lists (vdp_submit), not waited for, before the swap to
   that frame. false: no room (or not pipelined), send it now */
bool                vdp_dma_queue(u32 vram, const void *src, u32 bytes)
{
    if (!pipelined || dma_n >= DMA_TAB - 5)
        return false;                   /* (the lists' five kept room for) */
    dma_add(VDP1_VRAM + vram, src, bytes);
    return true;
}

static void         tab_range(int first, int n)
{
    if (n > 0)
        dma_add(VDP1_VRAM + list_base(list) + (u32)first * sizeof(vdp1_cmd), &staging[first],
                (u32)n * sizeof(vdp1_cmd));
}

static void         dma_range(int first, int n)
{
    if (n <= 0)
        return;
    scu_dma0((void *)(VDP1_VRAM + list_base(list) + (u32)first * sizeof(vdp1_cmd)), &staging[first],
             (u32)n * sizeof(vdp1_cmd), true);
    while (scu_dma0_busy())
        ;
}

int                 vdp_submit(void)
{
    int             b, i, w, waited = 0;
    vdp1_cmd        *prev = &staging[0];
    u32             t;

    /* the slave writer's state was written by the other CPU: read it uncached */
    writers[1] = *(vdp_writer *)UNCACHED(&writers[1]);

    /* chain: clear -> buckets far..near (master's then slave's) -> overlays -> END */
    for (b = 0; b < ZBUCKETS; ++b)
        for (w = 0; w < 2; ++w)
            if (writers[w].head[b] >= 0)
            {
                prev->link = (u16)(writers[w].link_base + writers[w].head[b] * (sizeof(vdp1_cmd) >> 3));
                prev = &writers[w].cmds[writers[w].tail[b]];
            }
    for (i = 0; i < overlay_count; ++i)
    {
        prev->link = link_to(OVL_FIRST + i);
        prev = &staging[OVL_FIRST + i];
    }
    staging[OVL_FIRST + overlay_count].ctrl = CMD_END;
    prev->link = link_to(OVL_FIRST + overlay_count);
    vdp_peak[0] = writers[0].count > vdp_peak[0] ? writers[0].count : vdp_peak[0];
    vdp_peak[1] = writers[1].count > vdp_peak[1] ? writers[1].count : vdp_peak[1];
    vdp_peak[2] = overlay_count > vdp_peak[2] ? overlay_count : vdp_peak[2];
    vdp_peak[3] = writers[0].gcount > vdp_peak[3] ? writers[0].gcount : vdp_peak[3];
    vdp_peak[4] = writers[1].gcount > vdp_peak[4] ? writers[1].gcount : vdp_peak[4];

    if (pipelined)
    {
        /* the last list swapped to (VDP1 was done with this slot's then) */
        t = frt_read();
        while (queued >= 0)
            if (VDP2_TVSTAT & 8)
            {
                ++waited;
                wait_vblank_out();
            }
        vdp_us_wait = frt_to_us((frt_read() - t) & 0xFFFF);
    }
    /* this list slot was last drawn two frames ago, so it's free to overwrite (and
       VRAM that frame used: list_hook's) */
    t = frt_read();
    if (list_hook)
        list_hook();
    if (pipelined)
    {
        /* the textures queued (list_hook's too), the lists and their Gouraud colours into VRAM
           in one go, not waited for (0.9 ms of a fight's frame): the swap isn't made until it's
           done, nor anything else sent by DMA, and vdp_begin waits for it before the lists (or
           this table) are written again */
        tab_range(0, 1 + writers[0].count);
        tab_range(writers[1].first, writers[1].count);
        tab_range(OVL_FIRST, overlay_count + 1);
        for (w = 0; w < 2; ++w)
            if (writers[w].gcount)
                dma_add(VDP1_VRAM + writers[w].gbase, writers[w].gst, (u32)writers[w].gcount * 8);
        dma_tab[3 * dma_n - 1] |= 0x80000000;
        scu_dma0_table(dma_tab);
        dma_n = 0;
        vdp_us_dma = frt_to_us((frt_read() - t) & 0xFFFF);
        queued_frame = frames_sent++;
        queued = list;                  /* (last: the interrupt may take it from here) */
        list ^= 1;
        return waited;
    }
    dma_range(0, 1 + writers[0].count);
    dma_range(writers[1].first, writers[1].count);
    dma_range(OVL_FIRST, overlay_count + 1);
    for (w = 0; w < 2; ++w)
        if (writers[w].gcount)
        {
            scu_dma0((void *)(VDP1_VRAM + writers[w].gbase), writers[w].gst, (u32)writers[w].gcount * 8, true);
            while (scu_dma0_busy())
                ;
        }
    vdp_us_dma = frt_to_us((frt_read() - t) & 0xFFFF);
    t = frt_read();

    /* wait for VDP1 to finish the frame in flight, then swap at the next vblank */
    if (drawing)
        while (!(VDP1_EDSR & 2))
        {
            if (VDP2_TVSTAT & 8)
            {
                ++waited;
                wait_vblank_out();
            }
        }
    vdp_us_wait = frt_to_us((frt_read() - t) & 0xFFFF);
    if (hw_erase)
    {
        /* The manual erase wipes the displayed buffer while that buffer is on
           screen for the last time, so it has to be requested before the
           vblank that starts its last showing - and only if the frame being
           drawn will be ready for the swap after it. Requested blindly, a late
           VDP1 leaves the wiped buffer on screen for a frame (walls vanish,
           only the VDP2 floor shows). The timer-0 interrupt makes that call at
           the last moment; if it declined, or we missed it, the list we just
           sent clears its own buffer with a polygon instead. By now that
           field's decision is made (pacing keeps us past it). */
        volatile vdp1_cmd *clr = (volatile vdp1_cmd *)(VDP1_VRAM + list_base(list));

        while (erase_state == ERASE_ARMED && ((frt_read() - last_swap) & 0xFFFF) < frame_ticks + ERASE_MARGIN)
            ;               /* the interrupt decides at line 208 of this field */
        if (erase_state == ERASE_REQUESTED)
            clr->ctrl = JP_SKIP_ASSIGN;
        else
        {
            clr->ctrl = JP_ASSIGN | VDP1_POLYGON;
            clr->xa = 0; clr->ya = 0;
            clr->xb = SCREEN_W - 1; clr->yb = 0;
            clr->xc = SCREEN_W - 1; clr->yc = SCREEN_H - 1;
            clr->xd = 0; clr->yd = SCREEN_H - 1;
            ++late_frames;
        }
    }
    while (((frt_read() - last_swap) & 0xFFFF) < min_frame_ticks)
        ;
    ((volatile u16 *)(VDP1_VRAM + HDR_JUMP * sizeof(vdp1_cmd)))[1] = (u16)(list_base(list) >> 3);
    VDP1_FBCR = 3;
    wait_vblank_in();
    last_swap = frt_read();
    vdp_shown = frames_sent++ - 1;
    if (vblank_hook)
        vblank_hook();                  /* e.g. VDP2 tables matching the frame now shown */
    erase_state = hw_erase ? ERASE_ARMED : ERASE_IDLE;
    drawing = true;
    list ^= 1;
    return waited;
}
