/*
** VDP1/VDP2 without SGL.
**
** Frame model (VDP1 manual frame change, 2 command lists):
**   - The CPUs build list N+1 in work RAM while VDP1 draws list N.
**   - vdp_submit(): SCU-DMA the list to its VRAM slot, wait for VDP1 to finish
**     list N, point the jump at VRAM 0 to the new list, request a swap (FBCR=3).
**     At the next vblank VDP1 swaps buffers and starts drawing list N+1 on its own.
**
** Depth sort: polygons go into Z buckets as they're built; each command's LINK
** field chains it to the next one (jump-assign), so sorting never moves any
** command data - only 16-bit LINK fields are written.
**
** Two writers (one per SH-2) fill separate parts of the list with their own
** buckets, so the CPUs never contend; vdp_submit() interleaves them bucket by
** bucket. Overlays (HUD, text) are drawn after everything, in call order.
**
** Sizes are compile-time: -DVDP_MAX_CMDS=n -DVDP_WRITER_CMDS=n (-DVDP_WRITER1_CMDS=n) -DVDP_GOURAUD_MAX=n
*/
#ifndef __VDP_H__
#define __VDP_H__

#include "sat.h"

typedef struct
{
    u16             ctrl, link, pmod, colr, srca, size;
    s16             xa, ya, xb, yb, xc, yc, xd, yd;
    u16             grda, dummy;
}                   vdp1_cmd;

#define SCREEN_W        (320)
#define SCREEN_H        (224)
#ifndef VDP_MAX_CMDS
# define VDP_MAX_CMDS   (3000)
#endif
#ifndef VDP_WRITER_CMDS
# define VDP_WRITER_CMDS (1400)
#endif
#ifndef VDP_WRITER1_CMDS
# define VDP_WRITER1_CMDS VDP_WRITER_CMDS   /* (the second writer's, if it's to differ) */
#endif
#ifndef ZBUCKETS
# define ZBUCKETS       (512)           /* (the game builds with 2: a bucket a CPU, the lists in painter's order) */
#endif
#define MAX_CMDS        VDP_MAX_CMDS
#define WRITER_CMDS     VDP_WRITER_CMDS     /* the first writer's room (the master's) */
#define WRITER1_CMDS    VDP_WRITER1_CMDS    /* the second's (the slave's) */

#define RGB(r, g, b)    ((u16)(0x8000 | (((b) >> 3) << 10) | (((g) >> 3) << 5) | ((r) >> 3)))

/* command types / flags for ctrl and pmod */
#define VDP1_SPRITE     (0x0000)
#define VDP1_DISTORTED  (0x0002)
#define VDP1_POLYGON    (0x0004)
#define VDP1_POLYLINE   (0x0005)
#define PMOD_ECD        (0x0080)        /* end codes off */
#define PMOD_SPD        (0x0040)        /* draw transparent pixels */
#define PMOD_MESH       (0x0100)
#define PMOD_RGB        (5 << 3)
#define PMOD_LUT4       (1 << 3)
#define PMOD_HALF_TRANS (0x0003)
#define PMOD_GOURAUD    (0x0004)
#define PMOD_HSS        (0x1000)        /* high-speed shrink: skips texels when drawing smaller than the texture */

typedef struct
{
    vdp1_cmd        *cmds;              /* first slot of this writer's region */
    u16             link_base;          /* VRAM LINK value of cmds[0] */
    int             first, count;       /* region start index in the list, commands used */
    u32             gbase;              /* this writer's Gouraud tables (VRAM offset), this frame */
    int             gcount, gmax;
    u32             *gst;               /* ...staged in work RAM; vdp_submit DMAs them over with the list */
    s16             head[ZBUCKETS], tail[ZBUCKETS];
    int             cmax;               /* commands it has room for */
    int             gover;              /* tables asked for past gmax this frame (they got the last one's) */
#ifdef CMD_RING
    vdp1_cmd        *win;               /* its window of RING_CMDS commands in work RAM: cmds[base] is win[0] */
    int             base, sent;         /* the index at the window's front; the first index not yet sent to VRAM */
    u32             *gwin;              /* ...and of RING_GOUR Gouraud tables: gst[gbasen] is gwin[0] */
    int             gbasen, gsent;
    int             last_off;           /* where in the window the piece last sent began (-1: none): a restart
                                           waits for it only if it's reading where the next run would write */
#endif
}                   vdp_writer;
#ifdef CMD_RING
/* (OPT=-DCMD_RING) the lists sent to VRAM in pieces as they're made, from a window a writer, not
   from a copy of the whole list (89 KB of high work RAM for 2,802 commands, and 22 KB of Gouraud
   tables). vdp_run goes before each run of commands (a row's cells, a model, a sprite) with the
   most it may make: it sends what's waiting once RING_CHUNK are, and starts the window over
   when the run wouldn't fit. A run must fit the window on its own */
# define RING_CMDS      (640)
# define RING_GOUR      (640)
# define RING_CHUNK     (96)
void                vdp_run(vdp_writer *w, int n);
#else
# define vdp_run(w, n)  ((void)0)
#endif

/* a texture in VDP1 VRAM */
typedef struct
{
    u16             srca, size;         /* ready for the command fields */
    u16             colr, pmode;        /* LUT address (4bpp) and colour mode bits for PMOD */
    u16             w, h;
}                   vdp_tex;

void                vdp_init(u16 back_color);
void                vdp_begin(void);                    /* start a new list */
void                vdp_set_clear(int x0, int y0, int x1, int y1);  /* area cleared each frame */
void                vdp_set_min_frame(int vblanks);     /* frame pacing: 1 = free running, 2 = 30 fps */
void                vdp_set_hw_erase(bool on);          /* before vdp_set_min_frame: off = clear with a polygon */
void                vdp_set_vblank_hook(void (*fn)(void));  /* runs in the vblank a new frame appears */
vdp_writer          *vdp_get_writer(int which);         /* 0 master, 1 slave; reset by vdp_begin */
int                 vdp_submit(void);                   /* returns vblanks spent waiting for VDP1 */
int                 vdp_cmd_count(void);

/* textures: 16bpp RGB (0x0000 = transparent), width a multiple of 8 */
bool                vdp_tex_upload(vdp_tex *t, const u16 *pixels, int w, int h);
bool                vdp_tex_upload_lut(vdp_tex *t, const u8 *pixels, const u16 *lut, int w, int h);
bool                vdp_tex_upload_4bpp(vdp_tex *t, const u8 *pixels, int w, int h);   /* no colour table */
u32                 vdp_tex_free(void);                 /* bytes of texture VRAM left */
u32                 vdp_tex_mark(void);                 /* texture memory high-water mark... */
void                vdp_tex_release(u32 mark);          /* ...free everything uploaded after it */

/* Gouraud: 4 corner colours (16 = neutral per channel, 5 bits each) -> GRDA.
   Tables are double buffered with the command lists. */
u16                 vdp_gouraud(u16 c0, u16 c1, u16 c2, u16 c3);        /* master CPU */
u16                 vdp_gouraud_w(vdp_writer *w, u16 c0, u16 c1, u16 c2, u16 c3);  /* either CPU: the writer's own tables */
static inline u16   vdp_gouraud_rgb(int r, int g, int b)
{
    return (u16)(0x8000 | (b << 10) | (g << 5) | r);
}

/* the same from packed pairs (c0 << 16 | c1, c2 << 16 | c3): two stores to
   work RAM, no VDP1 VRAM access (the tables go over by DMA in vdp_submit) */
static inline u16   vdp_gouraud_fast(vdp_writer *w, u32 c01, u32 c23)
{
    int             i = w->gcount;

    if (i >= w->gmax)
    {
        i = w->gmax - 1;                /* (out of tables: the wrong shading, counted) */
        ++w->gover;
    }
    else
        w->gcount = i + 1;
    w->gst[i * 2] = c01;
    w->gst[i * 2 + 1] = c23;
    return (u16)((w->gbase >> 3) + (u32)i);
}

/* overlays: drawn after all sorted polygons, in call order */
vdp1_cmd            *vdp_overlay(void);
vdp1_cmd            *vdp_overlay_block(int *room);         /* the next free ones, written in order... */
void                vdp_overlay_add(int n);                 /* ...and how many were */
void                vdp_rect(int x0, int y0, int x1, int y1, u16 color, bool half_transparent);
void                vdp_rect_shaded(int x0, int y0, int x1, int y1, u16 color, u16 g_top, u16 g_bot);
void                vdp_frame(int x0, int y0, int x1, int y1, u16 color);
void                vdp_sprite(const vdp_tex *t, int x, int y, bool half_transparent);
void                vdp_text(int x, int y, u16 color, const char *s);
void                vdp_printf(int x, int y, u16 color, const char *f, ...);
extern void         (*vdp_stall_hook)(const u32 *r);   /* (a test: vdp_submit stuck waiting for the swap) */
u16                 vdp_text_colr(u16 color);           /* text in the scene: colr for a glyph sprite */
u16                 vdp_glyph(int ch);                  /* ...and its srca (8x8, 4bpp; size 0x0108) */

/* VDP2 colour offset on the sprite layer, -255..255 per channel */
void                vdp_color_offset(int r, int g, int b);
void                vdp_color_offset_off(void);
void                vdp_color_offset_all(int r, int g, int b);   /* every layer: fades */

extern u32          vdp_us_dma, vdp_us_wait;            /* last submit: list DMA, waiting for VDP1 */
extern int          vdp_peak[5];                        /* most commands sent: master's, slave's, overlay; Gouraud tables: master's, slave's */
extern u32          vdp_gover;                          /* Gouraud tables wanted past a writer's share (drawn with the last one's) */
extern u32          vdp_overdraw;                       /* (OPT=-DOVERDRAW_PROF) pixels the last list asked for */
void                vdp_set_pipelined(bool on);         /* submit returns at once, the swap's by interrupt (vdp.c) */
bool                vdp_dma_queue(u32 vram, const void *src, u32 bytes);    /* into VRAM with the lists (vdp_submit) */
void                vdp_set_list_hook(void (*fn)(void));    /* in vdp_submit once VDP1's done with the frame before last (VRAM it used is free) */
bool                vdp_get_pipelined(void);            /* the swap by interrupt (and the vblank interrupt with it) */
u32                 vdp_frame_no(void);                 /* frames submitted so far: the one being built */
extern volatile u32 vdp_shown;                          /* in the vblank hook: the frame now on screen */
extern volatile u32 vdp_swap_fields[8];                 /* how long pictures stayed up: 1, 2, ... 7+ fields */
extern volatile u32 vdp_draw_ticks, vdp_draw_late;      /* VDP1's time on the last frame it finished (FRT ticks); ends seen a field late */
void                vdp_set_field_hook(void (*fn)(void));   /* every field, in the timer interrupt */
void                vdp_debug_state(int *queued, int *fields);  /* (a test's watchdog) */
extern u32          late_frames;                        /* VDP1 ran late: polygon clear instead of erase */

/* a sorted polygon slot in bucket z (0 = far); caller fills pmod/colr/coords.
   ctrl is set to jump-assign | polygon; OR in a different type if needed. */
static inline vdp1_cmd  *vdp_poly(vdp_writer *w, int z)
{
    int             i = w->count;
    vdp1_cmd        *c;

    if (i >= w->cmax)
        return NULL;
    c = &w->cmds[i];
    c->ctrl = 0x1004;
    if (w->head[z] < 0)
        w->tail[z] = (s16)i;
    else
        c->link = (u16)(w->link_base + w->head[z] * (sizeof(vdp1_cmd) >> 3));
    w->head[z] = (s16)i;
    w->count = i + 1;
    return c;
}

#endif
