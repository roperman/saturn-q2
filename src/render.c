/*
** The world renderer.
**
** Quake's own visibility: the PVS row of the camera's cluster marks leaves
** (and the nodes above them); the BSP is walked front to back from the
** camera, culling nodes against the view frustum; reaching a leaf marks its
** faces; a node's marked faces that face the camera are listed between its
** front and back children. That list is in front-to-back order.
**
** Both SH-2s draw: the master the nearer part of the list, the slave the
** rest. Each puts its commands into one bucket of its own writer (master 1,
** slave 0), and VDP1 draws a bucket's commands last-in first, so the list
** comes out back to front: a correct painter's order with no sorting.
**
** A face is a grid of cells (tools/bake_map.py). Its origin and two axes go
** into view space once; grid vertices are then additions. Each cell is a
** VDP1 distorted sprite of its texture, Gouraud-shaded from the lightmap. A
** cropped cell's corners are interpolated on screen from its grid corners
** when it's small there (VDP1 maps the texture across the whole cell that
** way, so the crop lands where the full cell would have drawn it), and
** worked out in view space and projected when it's big or near the camera.
**
** Textures live in a cache in VDP1 VRAM, in fixed slots the size of a tile
** (every texture is a tile or a crop of one), half the slots for each CPU.
** A slot can be reused once the frame that last drew with it has been drawn:
** not by the list being built nor the one VDP1 is drawing now. The colour
** tables are all resident.
*/
#include "q2.h"
#include "sky.h"
#include "dsp.h"

#define CX              (SCREEN_W / 2)
#define CY              (SCREEN_H / 2)
#define FOCAL           (160)               /* 90 degrees across */
#define NEAR_Z          FIX(8)
#define MAX_ROW         (257)
#ifdef ROWS_TEST
#define WHOLE_MAX       (12)                /* (test: most faces a row at a time; face.s's too, -Wa,--defsym,ROWS_TEST=1) */
#else
#define WHOLE_MAX       (2 * MAX_ROW)       /* a face's grid points: at most, for the whole grid at once */
#endif
#define MAX_SLOTS       (3072)              /* (VRAM has room for about 2,800) */
#define MAX_VIS         (2048)              /* (busy views list about 500) */
#ifndef CLAMP_XY
#define CLAMP_XY        (2000)
#endif

/* the world's cells: 4-bit colour tables, Gouraud. OPT=-DHSS adds VDP1's
   high-speed shrink (a cell drawn smaller than its texture skips texels):
   Mednafen's timing doesn't show it, so it's untested where it would count */
#ifdef HSS
#define CELL_PMOD       (PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD | PMOD_HSS)
#else
#define CELL_PMOD       (PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD)
#endif

/* R_PROFILE: the per-cell and per-row counts and timings (the benchmark's
   breakdown). Each is a store, and on the SH-2 a store waits for the bus (the
   cache writes through, with no buffer), so they're out unless asked for */
#ifdef R_PROFILE
#define PROF(x)         (x)
#else
#define PROF(x)         ((void)0)
#endif
#define INTERP_PX       (64)                /* cropped cells smaller than this on screen are interpolated */
#define MAX_MVERTS      (336)               /* (the gunner's 329) */
#define MAX_MPOLYS      (384)               /* (the gunner's 382) */
#define MBUCKETS        (32)
#define MODEL_FRONT     (-1)                /* the sign of a front face's screen winding */                /* Gouraud steps for a model's light 0..2: sized for skin brightness */

/* outcodes */
/* (src/grid.s builds these with ROTCL: that order). OC_FAR: beyond the guard
   band (GUARD_X, GUARD_Y from the middle): not outside the view, but a cell
   there would have VDP1 going over far more than the screen (cell_split) */
#define OC_FAR          (32)
#define GUARD_X         (480)
#define GUARD_Y         (336)
#define OC_NEAR         (16)
#define OC_LEFT         (1)
#define OC_RIGHT        (2)
#define OC_TOP          (4)
#define OC_BOTTOM       (8)

typedef struct { s32 x, y, z; } v3;
/* a grid vertex: its screen position (x << 16 | y) and the frustum planes it's
   outside, 8 bytes (two to a cache line). Where it is in view space, the few
   things that need it work out again (grid_pos). */
typedef struct { u32 xy; union { struct { u8 oc, done; }; u16 ocd; }; u16 pad; } gv;

/* src/grid.s: a grid row in assembly. p: the row's first point; e0, d, e1:
   the steps (out of the first column, a whole one, into the last). k: the
   constants, grid_k. */
typedef struct { v3 p, e0, d, e1, rs; } grid_args;
void                grid_row_asm(gv *row, int n, const grid_args *a, const s32 *k);
/* and a whole face's, the rows one after another: k is then grid_k's, the
   points a row, the rows after the first, 0, and the row steps f0, dv, f1 */
void                grid_face_asm(gv *grid, int npts, grid_args *a, s32 *k);
#define GK_ROWS         (9)
#define GK_F0           (11)
static s32          grid_k[22];             /* NEAR_Z, FOCAL, CX, CY, SCREEN_W, SCREEN_H, ky, CLAMP_XY; [20] [21] the guard band */
static bool         r_grid_asm;             /* it passed its test (grid_selftest) */
static void         grid_selftest(void);

/* src/cells.s: a row of cells, the common ones (whole tiles and exact crops,
   in front, their textures in VRAM); the rest it lists for the C. The first
   words are what it reads for each cell (offsets up to 60: SH-2 addressing),
   then the row, in and out. */
typedef struct
{
    const u16       *tex_slot;
    u16             *slot_frame;
    u32             frame, sb8, base8, lut8, pmod, ctrl, fifo, stride2, fast, near24, n;
    u16             *defp;
    u32             rows;
    const gv        *top, *bot;
    const q_cell    *cell;
    const u16       *light;
    vdp_writer      *w;                     /* this CPU's list (read, and brought up to date) */
    s16             *head, *tail;           /* its bucket's ends */
    u32             spare[2];
    const q_cell    *cell0;                 /* the deferred cells' numbers count from here */
    /* small crops: their grid cells' texels (the face's edge columns and rows are narrower) */
    u32             cl0, cr1, ct0, cb1;     /* the first column's left, the last's right; the rows' */
    u32             rows0, N;               /* rows at the start (the first's rows == this) */
    const s32       *rcp;
    const u16       *slot_lut;
    const u8        *slot_w;
    u16             def[2 * MAX_ROW];
}                   cell_args;
_Static_assert(__builtin_offsetof(cell_args, top) == 60 && __builtin_offsetof(cell_args, cell0) == 96
               && __builtin_offsetof(vdp_writer, link_base) == 4 && __builtin_offsetof(vdp_writer, count) == 12
               && __builtin_offsetof(vdp_writer, gbase) == 16 && __builtin_offsetof(vdp_writer, gcount) == 20
               && __builtin_offsetof(vdp_writer, gst) == 28, "cells.s: cell_args, vdp_writer");
_Static_assert(__builtin_offsetof(cell_args, cell0) == 96 && __builtin_offsetof(cell_args, cl0) == 100
               && __builtin_offsetof(cell_args, slot_w) == 132, "src/cells.s: C_ROW, C_CROP");
void                cells_asm(cell_args *a);

/* src/face.s: a face's setup (face_setup's, bit for bit). Its results first (small
   offsets), then the frame's (face_frame); the offsets are face.s's A_ */
typedef struct
{
    v3              fo;                     /* its first grid point (view space; its steps: ga, gk) */
    int             fnu, fnv;
    v3              dut, dvt;               /* its axes, one stored texel apart */
    int             eu0, eu1, ev0, ev1;     /* its edge columns' and rows' texels */
    const q_cell    *cells;
    const u16       *light;
    s32             rt[3], up[3], fw[3];    /* the camera's axes, */
    s32             pos[3];                 /* and where it is */
    s32             ky, lod_z, rcp_n;
    u32             frame;
    const s32       *mover_ofs;
    s32             *axis_view;
    const s32       *axes;
    const q_face    *faces;
    const q_cell    *cells0, *lodcells;
    const u16       *lights0, *lodlights;
    const q_lodface *lodfaces;
    s32             N;
    gv              *grid;
    grid_args       *ga;
    s32             *gk;
    s32             prx0, prx1, pry0, pry1; /* the face's cluster's rectangle, as seen through the portals: */
    s32             prect;                  /* its sides' slopes, if it's not the whole screen */
}                   face_args;
_Static_assert(__builtin_offsetof(face_args, light) == 64 && __builtin_offsetof(face_args, rt) == 68
               && __builtin_offsetof(face_args, frame) == 128 && __builtin_offsetof(face_args, gk) == 180
               && __builtin_offsetof(face_args, prect) == 200
               && sizeof(q_face) == 32 && sizeof(q_lodface) == 16 && __builtin_offsetof(q_face, firstcell) == 24
               && __builtin_offsetof(grid_args, e1) == 36, "src/face.s: face_args");
int                 face_asm(face_args *a, int fi, int model);   /* (OPT=-DNO_FACE_ASM: face_setup, the C) */
bool                r_cells_asm = true;
bool                r_nosplit;              /* (for comparing: whole tiles near the camera in one piece) */
bool                r_dl_verts = true;
#ifdef ENTLIGHT_CHECK
u32                 el_checks, el_diffs;
#endif      /* dynamic lights added once a grid point (not at each cell's corners) */
#ifdef DL_CHECK
u32                 dl_checks, dl_diffs;    /* (OPT=-DDL_CHECK: the corners' sums as well, compared) */
#endif
s32                 r_lod_z = FIX(LOD_Z);   /* a face wholly beyond this uses its coarse grid (settings.h) */
u32                 r_clock;                /* the game's time (16.16 seconds, main.c): the water's movement */
#ifdef NO_WATER
bool                r_water = false;
#else
bool                r_water = true;         /* water moves (waves, and a ripple of light) */
#endif
int                 r_trans = TRANS_MODE;   /* translucent surfaces: 0 solid, 1 mesh, 2 half-transparent (VDP1),
                                               3 half-transparent, the water untextured (plain_water) */
int                 r_bright = BRIGHT;      /* the options' brightness: 0 as baked, to 4 (a gamma on every colour table) */

/* a 5-bit channel through each brightness's curve (1 to 4: gamma 1.15, 1.3, 1.5, 1.75) */
static const u8     gamma_tab[4][32] = {
    { 0, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31 },
    { 0, 2, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31 },
    { 0, 3, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 28, 28, 29, 30, 30, 31 },
    { 0, 4, 6, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 21, 22, 23, 23, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30, 30, 31 },
};

/* an RGB colour at the brightness chosen (its top bit kept) */
__attribute__((cold)) u16 r_gamma(u16 c)      /* (the brightness's work's all at a level's start, a gun's change or
                                                   the options: built small) */
{
    const u8        *g;

    if (!r_bright)
        return c;
    g = gamma_tab[r_bright - 1];
    return (u16)((c & 0x8000) | g[c >> 10 & 31] << 10 | g[c >> 5 & 31] << 5 | g[c & 31]);
}

/* n colours into VDP1's VRAM at vram_ofs, through the brightness */
static __attribute__((cold)) void luts_copy(u32 vram_ofs, const u16 *src, u32 n)
{
    volatile u16    *d = (volatile u16 *)(VDP1_VRAM + vram_ofs);
    u32             i;

    if (!r_bright)
        memcpy((void *)d, src, n * 2);
    else
        for (i = 0; i < n; ++i)
            d[i] = r_gamma(src[i]);
}
int                 r_model_far = MODEL_FAR;   /* a model beyond this (units) uses its coarse mesh (settings.h) */
static u16          cell_all[MAX_ROW];      /* the cells' numbers, for the C doing a whole row */
#define UPQ             (256)               /* texture uploads a CPU can queue in a frame (then it copies them itself) */
#ifdef NO_DMA_UPLOADS
bool                r_dma_uploads = false;
#else
bool                r_dma_uploads = true;
#endif

/* each CPU's own: its writer, scratch, statistics and half of the texture cache */
typedef struct
{
    vdp_writer      *w;
    int             bucket, first, last;    /* the part of the face list it draws */
    bool            fifo;                   /* commands appended (drawn in order) rather than pushed (reversed) */
    gv              grid[2 * MAX_ROW];      /* a face's grid, or two rows of a big one's */
    s32             gk[22];                 /* grid_k, and grid_face_asm's (the guard band at the end, as grid_k) */
    grid_args       ga;
    cell_args       ca;
    face_args       fa;                     /* a face's setup (its first grid point, its steps: ga, gk) */
    u32             cpmod;                  /* its cells' PMOD << 16 (translucent faces: mesh or half-transparent) */
    bool            wave;                   /* its grid points move (water: wave_at) */
    v3              wn;                     /* ...along its plane's normal (view space) */
    u8              dmask;                  /* the dynamic lights reaching this face */
    bool            lit;                    /* ...added to the face's lights at its grid points already */
    const u16       *raw_light;             /* (then: the face's own lights, for cell_split) */
    /* a model being drawn: its vertices (screen xy, view z, outcodes, Gouraud) and polygons by depth */
    u32             mxy[MAX_MVERTS];
    s32             mz[MAX_MVERTS];
    u16             mg[MAX_MVERTS];
    u8              moc[MAX_MVERTS];
    u16             gtab[162];
    s16             mhead[MBUCKETS], mnext[MAX_MPOLYS];
    r_stats         st;
    int             slot0, nslots, hand;
    bool            full;                   /* no slot free this frame, even two frames back */
    bool            tight;                  /* none three frames back: those two back, their uploads late */
    u16             *tex_slot;              /* per texture: its slot or 0xFFFF */
    const void      *up_src[UPQ];           /* textures to copy into VRAM at the frame's end (tex_upload) */
    u32             up_dst[UPQ];            /* VRAM offset << 8 | longs (| UP_LATE) */
    int             nup;
    u16             wl_rec[32];             /* the whole faces it lit this frame (the next frame's, lit ahead: r_wall_ahead) */
    int             wl_nrec;
#ifdef DSP_WALLS
    u16             *dw_rp;                 /* (the whole faces it draws, for the DSP: where the next goes, or
                                               NULL, */
    int             dw_step;                /* ...and the way: dw_note) */
#endif
}                   r_ctx;

r_stats             rs;
int                 r_debug;                /* profiling: 1 = stop after culling, 2 = after projecting, 3 = after the texture */
bool                r_two_cpus;
q_cam               cam;

static r_ctx        ctx[2];
u32                 r_full[3];              /* frames each CPU's part of the texture cache ran out; late uploads (the benchmarks) */
#ifdef TEX_WSET
u32                 r_wset[5];              /* most textures a frame, each CPU; their sums; the slots each */
#endif
static u16          frame = 1;
u16                 visframe = 1;           /* (1: leaf_vis's zeroes aren't a PVS before the first's marked) */
static u16          *node_vis;
u16                 *leaf_vis;
static u8           *face_vis;              /* the camera cluster's visible faces, a bit each */
static s16          *node_parent, *leaf_parent;
static int          view_cluster = -2;
static u16          vis_faces[MAX_VIS];
static u8           vis_model[MAX_VIS];     /* the brush model each face is part of (0 the world; SPRITE a sprite) */
#define SPRITE          (0xFF)
#define ENTITY          (0xFE)
static s16          *leaf_spr, spr_next[MAX_SPRITES];
static s16          *leaf_ent, ent_next[MAX_ENTITIES];
static int          ent_leaf[MAX_ENTITIES];
static int          spr_leaf[MAX_SPRITES];

q_dlight            r_dlights[MAX_DLIGHTS];
int                 r_ndlights;
q_sprite            r_sprites[MAX_SPRITES];
int                 r_nsprites;
/* this frame's lights in view space: position, radius squared (whole units) */
typedef struct { s32 x, y, z, r2, inv; u8 r, g, b, pad; } dl_light;
static dl_light     dl[MAX_DLIGHTS];
static const q_dlight *lsrc = r_dlights;    /* the lights the walls go by (the DSP's, or OPT=-DWALLS_AHEAD: last frame's) */
static int          ndl;
static s16          *leaf_model, *model_next;   /* brush models, listed by the leaf their centre's in */
static int          nvis;
static const u8     bitm[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };  /* (a variable shift's a library call) */
static u8           *face_back;             /* a bit a face: on its plane's back (FF_BACK), in high work RAM */

/* frustum: 4 planes through the camera, normals inwards (16.16), and the camera's distance along each */
static s32          fr_n[4][3], fr_d[4];
static u8           fr_sign[4];

/* the texture cache */
static u32          lut_vram;               /* VRAM offset of colour table 0 */
static u32          slot_vram, slot_bytes;
static u16          slot_tex[MAX_SLOTS], slot_frame[MAX_SLOTS];
#define SLOT_NONE       (0xFFFF)            /* slot_tex: none (free now) */
#define UP_LATE         (0x80000000u)       /* up_dst: into a slot two frames back: not till VDP1's done with that (r_late_uploads) */
#ifndef SPLIT_M
# define SPLIT_M        (45)                /* the master's part of the texture cache, % (the slave draws more of the list; the gun is the master's) */
#endif
static u16          slot_lut[MAX_SLOTS];    /* a world texture's record (q_tex's lut, w) while it has the slot: */
static u8           slot_w[MAX_SLOTS];      /* the records are on the cart, 75 cycles a miss */
static s32          kx, ky;                 /* CX / FOCAL, CY / FOCAL (16.16): the frustum's slopes */
static s32          rcp[64];                /* 65536 / n */

/* The gun's last drawing, kept (on the cart: DMA reads it into the list each frame it goes
   out again): most of the time it's still (up, not firing), and then its polygons land where
   they did, in the same order. Its lighting can still change (another leaf, turning), and so
   can its bob: what's kept is the polygons (their corners among the kept vertices), then the
   vertices' places before the bob, then their normals */
typedef struct { u16 tex, size; u16 v[4]; } view_kept;      /* 12 bytes */
#define VIEW_KEEP_BYTES (MAX_MPOLYS * sizeof(view_kept) + MAX_MVERTS * 2 * 5)
static u8           *view_keep;             /* (r_view_level: in HWRAM if there's room, else on the cart) */
static bool         view_keep_hot;
static int          view_nkeep, view_nkv, view_gen = 1;
static u32          view_kver;              /* (one more each time it's kept anew) */
/* ...and last frame's going out, if it was that: its commands and colour tables are still in
   the lists' staging where they were, so this frame's need only what's changed written */
static struct { const u32 *at; int gc; u32 kver, gver; s32 bob[3]; bool ok; } vz;
static struct { const q_mdl *m; int gen, f0, f1; s32 lerp; } view_key;

static int          ntex_all;               /* the level's textures, then the models' */
static u32          glow_vram, glow_lut;    /* the sprites' texture and its colours */
static s32          rcp_n, wscale;          /* 1/N (16.16); 65536 / N^2 (interpolation weights) */

/* Model vertices on the SCU DSP (engine/xformm.dsp), alongside the CPUs: at
   the start of a frame the master lists the models that may be seen (both
   frames' matrices, weighted for the blend) and starts the DSP; it works
   through them while the CPUs walk and draw, counting each one off in work
   RAM, and draw_model waits only if its own isn't done yet. Its results come
   in behind the CPU's cache, so draw_model forgets those lines first. */
#define DSPM_MODELS     (32)
#define DSPM_BLOCKS     (64)                /* 16 vertices each (a monster's 15, far 3 to 5: four near) */
#define DSPM_MORE       (96)                /* more where a level leaves HWRAM (r_dspm_level) */
#define DSPM_BIG        (100)               /* vertices: a model to the DSP before the small ones */
#define DSPM_HEAD       (25)                /* (the last word: where its blocks go; xformml.dsp's, its lighting's jobs) */
static u32          dspm_stream[DSPM_MODELS * DSPM_HEAD] __attribute__((aligned(16)));
static s32          dspm_out[DSPM_BLOCKS * 48] __attribute__((aligned(16)));
static s32          *dspm_more;             /* (blocks DSPM_BLOCKS on) */
static int          dspm_nmore;
static u32          dspm_count __attribute__((aligned(16)));
#ifdef DSP_LIGHT
/* (OPT=-DDSP_LIGHT) the models' lighting on the DSP too (xformml.dsp, each model's before
   its vertices): a job for each model and dynamic light near it, 162 weights out */
#define DSPL_JOBS       (8)
#define DSPL_N          (176)               /* the 162 normals, padded to 11 x 16 */
static u32          dspl_jobs[DSPL_JOBS * 9] __attribute__((aligned(16)));
static s32          dspl_out[DSPL_JOBS * DSPL_N] __attribute__((aligned(16)));
static s32          dspl_norm[DSPL_N * 3] __attribute__((aligned(16)));
static u32          dspl_count __attribute__((aligned(16)));
static s8           ent_lj0[MAX_ENTITIES], ent_ljn[MAX_ENTITIES];   /* each entity's first job, how many (0: the CPU) */
#endif
#ifdef DSPL_CHECK
u32                 dspl_checks, dspl_diffs;    /* (OPT="-DDSP_LIGHT -DDSPL_CHECK": its weights against the C's) */
#endif
static s16          ent_dsp[MAX_ENTITIES], ent_blk[MAX_ENTITIES];  /* its place in the DSP's list (or -1), its first block */

/* a block's results: the first DSPM_BLOCKS', then the level's more */
static inline s32   *dspm_blk(int b)
{
    return b < DSPM_BLOCKS ? dspm_out + b * 48 : dspm_more + (b - DSPM_BLOCKS) * 48;
}

/* room for a model's blocks (n): its first block, or -1 (a model's all in one or the other) */
static int          dspm_place(int n, int *nb, int *nc)
{
    int             b;

    if (*nb + n <= DSPM_BLOCKS)
    {
        b = *nb;
        *nb += n;
        return b;
    }
#ifndef DSP_LIGHT
    if (*nc + n <= dspm_nmore)
    {
        b = DSPM_BLOCKS + *nc;
        *nc += n;
        return b;
    }
#endif
    return -1;
}
bool                r_use_dsp, r_dsp_ok;
static s32          *axis_view;             /* the axes in view space: du and its frame, dv and its (draw_face) */


#ifdef PPD_TEST
u32                 ppd_ticks;
#endif
/* the DSP self-test's findings (shown by main.c) */
s32                 dsp_test[8];

/* two frames, (k, 2k, 3k) and 2 more, half each, plus (100, 200, 300): from
   work RAM, then from the cart (where the models are) */
static void         dsp_selftest(void)
{
    static u32      in[32] __attribute__((aligned(16)));
    u32             *cart_in = (u32 *)(0x02400000 + 0x3F0000);     /* near the cart's end */
    volatile u32    *count = (volatile u32 *)UNCACHED(&dspm_count);
    int             k, pass;

    for (k = 0; k < 16; ++k)
    {
        in[k] = (u32)(k << 24 | (2 * k) << 16 | (3 * k) << 8);
        in[16 + k] = (u32)((k + 2) << 24 | (2 * k + 2) << 16 | (3 * k + 2) << 8);
    }
    for (k = 0; k < 32; ++k)
        cart_in[k] = in[k];
#ifdef PPD_TEST
    {
        /* (OPT=-DPPD_TEST: what loading a DSP program costs a CPU: the models' program, 4 times) */
        extern u32 ppd_ticks;
        u32 t0 = frt_read();

        dsp_init_models();
        dsp_init_models();
        dsp_init_models();
        dsp_init_models();
        ppd_ticks = (frt_read() - t0) & 0xFFFF;
    }
#endif
    dsp_init_models();
    for (pass = 0; pass < 2; ++pass)
    {
        u32         *st = dspm_stream, *v = pass ? cart_in : in;
        const s32   *out = (const s32 *)UNCACHED(dspm_out);
        u32         t;

        memset(dspm_out, 0xEE, 48 * 4);
        *count = 0;
        for (k = 0; k < 3; ++k, st += 7)
        {
            st[0] = (u32)FIX(100 * (k + 1));
            st[1] = st[2] = st[3] = st[4] = st[5] = st[6] = 0;
            st[1 + k] = st[4 + k] = FIX(0.5);
        }
        st[0] = ((u32)v & 0x07FFFFFF) >> 2;
        st[1] = ((u32)(v + 16) & 0x07FFFFFF) >> 2;
        st[2] = 1;
#ifdef DSP_LIGHT
        st[3] = 0;                          /* (no lighting) */
#else
        st[3] = ((u32)dspm_out & 0x07FFFFFF) >> 2;
#endif
        dsp_models(dspm_stream, dspm_out, 1, &dspm_count);
        for (t = 0; t < 2000000 && dsp_busy(); ++t)
            ;
        dsp_test[pass * 4 + 0] = dsp_busy() ? -1 : (s32)*count;      /* finished, and counted? 1 */
        dsp_test[pass * 4 + 1] = out[5] >> 16;                       /* x of vertex 5: 106 */
        dsp_test[pass * 4 + 2] = out[16 + 5] >> 16;                  /* y: 211 */
        dsp_test[pass * 4 + 3] = out[32 + 5] >> 16;                  /* z: 316 */
        if (dsp_busy())
            dsp_init_models();              /* stuck: reload (stops it) */
    }
    r_dsp_ok = dsp_test[0] == 1 && dsp_test[1] == 106 && dsp_test[2] == 211 && dsp_test[3] == 316
            && dsp_test[4] == 1 && dsp_test[5] == 106 && dsp_test[6] == 211 && dsp_test[7] == 316;
    r_use_dsp = r_dsp_ok;
#ifdef NO_DSP
    r_use_dsp = false;                      /* (OPT=-DNO_DSP: the CPUs do the models' vertices) */
#endif
}

void                cam_update(void)
{
    s32             sy = fsin(cam.yaw), cy = fcos(cam.yaw);
    s32             sp = fsin(cam.pitch), cp = fcos(cam.pitch);
    s32             kf, kc, len;
    int             i;

    cam.fwd[0] = fmul(cy, cp);
    cam.fwd[1] = fmul(sy, cp);
    cam.fwd[2] = -sp;
    cam.right[0] = sy;
    cam.right[1] = -cy;
    cam.right[2] = 0;
    cam.up[0] = fmul(cy, sp);
    cam.up[1] = fmul(sy, sp);
    cam.up[2] = cp;

    /* side planes: x = +-(CX/FOCAL) z and y = +-(CY/FOCAL) z in view space, as world normals */
    len = (s32)isqrt(FOCAL * FOCAL + CX * CX);
    kf = FOCAL * 65536 / len;
    kc = CX * 65536 / len;
    for (i = 0; i < 3; ++i)
    {
        fr_n[0][i] = fmul(cam.right[i], kf) + fmul(cam.fwd[i], kc);     /* left edge */
        fr_n[1][i] = -fmul(cam.right[i], kf) + fmul(cam.fwd[i], kc);    /* right edge */
    }
    len = (s32)isqrt(FOCAL * FOCAL + CY * CY);
    kf = FOCAL * 65536 / len;
    kc = CY * 65536 / len;
    for (i = 0; i < 3; ++i)
    {
        fr_n[2][i] = -fmul(cam.up[i], kf) + fmul(cam.fwd[i], kc);       /* top */
        fr_n[3][i] = fmul(cam.up[i], kf) + fmul(cam.fwd[i], kc);        /* bottom */
    }
    for (i = 0; i < 4; ++i)
    {
        fr_d[i] = fmul(fr_n[i][0], cam.pos[0]) + fmul(fr_n[i][1], cam.pos[1]) + fmul(fr_n[i][2], cam.pos[2]);
        fr_sign[i] = (u8)((fr_n[i][0] < 0) | (fr_n[i][1] < 0) << 1 | (fr_n[i][2] < 0) << 2);
    }
}

/* a node's box entirely outside a frustum plane? (its nearest corner along the normal is behind it) */
/* Is a box outside one of the view's side planes? Only the planes in *mask
   are tried, and those the box is wholly inside come off it: nothing in the
   box can be outside them either, so a walk passes the mask down and deep in
   the tree most boxes aren't tried at all. */
static bool         cull_box(const s16 *mins, const s16 *maxs, u8 *mask)
{
    int             i;
    u8              m = *mask;

    for (i = 0; i < 4; ++i)
    {
        const s32   *n;
        u8          s;

        if (!(m & (1 << i)))
            continue;
        n = fr_n[i];
        s = fr_sign[i];
        /* the corner furthest along the plane's normal: outside, and so's all of it */
        if (n[0] * (s & 1 ? mins[0] : maxs[0]) + n[1] * (s & 2 ? mins[1] : maxs[1])
            + n[2] * (s & 4 ? mins[2] : maxs[2]) < fr_d[i])
            return true;
        /* the nearest: inside, and so's all of it */
        if (n[0] * (s & 1 ? maxs[0] : mins[0]) + n[1] * (s & 2 ? maxs[1] : mins[1])
            + n[2] * (s & 4 ? maxs[2] : mins[2]) >= fr_d[i])
            m &= (u8)~(1 << i);
    }
    *mask = m;
    return false;
}

static void         r_late_uploads(void);

__attribute__((cold)) void render_init(void)                /* (at a level's start: built small) */
{
    int             i, c;
    u32             base = vdp_tex_mark(), free = vdp_tex_free();
    u8              *vram = (u8 *)VDP1_VRAM;

    view_cluster = -2;                      /* (a new level: work out what's visible again) */
    kx = CX * 65536 / FOCAL;
    ky = CY * 65536 / FOCAL;
    for (i = 0; i < MAX_ROW; ++i)
        cell_all[i] = (u8)i;
    grid_k[0] = NEAR_Z; grid_k[1] = FOCAL; grid_k[2] = CX; grid_k[3] = CY;
    grid_k[4] = SCREEN_W; grid_k[5] = SCREEN_H; grid_k[6] = ky; grid_k[7] = CLAMP_XY;
    grid_k[20] = GUARD_X;
    grid_k[21] = GUARD_Y;
    for (i = 0; i < 22; ++i)
        ctx[0].gk[i] = ctx[1].gk[i] = grid_k[i];
    grid_selftest();
    dsp_selftest();
    rcp_n = 65536 / lv.N;
    wscale = 65536 / (lv.N * lv.N);
    for (i = 1; i < 64; ++i)
        rcp[i] = 65536 / i;
    face_back = level_alloc((u32)(lv.nfaces + 7) >> 3);
    memset(face_back, 0, (u32)(lv.nfaces + 7) >> 3);
    for (i = 0; i < lv.nfaces; ++i)
        if (lv.faces[i].flags & FF_BACK)
            face_back[i >> 3] |= bitm[i & 7];
    axis_view = level_alloc((u32)lv.naxes * 32);
    memset(axis_view, 0, (u32)lv.naxes * 32);
    face_vis = level_alloc((u32)(lv.nfaces + 7) >> 3);
    leaf_spr = level_alloc((u32)lv.nleafs * 2);
    memset(leaf_spr, 0xFF, (u32)lv.nleafs * 2);
    leaf_ent = level_alloc((u32)lv.nleafs * 2);
    memset(leaf_ent, 0xFF, (u32)lv.nleafs * 2);
    node_vis = level_alloc((u32)lv.nnodes * 2);
    leaf_vis = level_alloc((u32)lv.nleafs * 2);
    node_parent = level_alloc_low((u32)lv.nnodes * 2);     /* (only when the camera's cluster changes) */
    leaf_parent = level_alloc_low((u32)lv.nleafs * 2);
    memset(node_vis, 0, (u32)lv.nnodes * 2);
    memset(leaf_vis, 0, (u32)lv.nleafs * 2);
    /* parents, for marking the nodes above visible leaves */
    for (i = 0; i < lv.nnodes; ++i)
        node_parent[i] = -1;
    for (i = 0; i < lv.nnodes; ++i)
    {
        int k;

        for (k = 0; k < 2; ++k)
        {
            int ch = lv.nodes[i].child[k];

            if (ch >= 0)
                node_parent[ch] = (s16)i;
            else
                leaf_parent[-(ch + 1)] = (s16)i;
        }
    }
    /* brush models (doors, lifts...): each in the leaf its centre's in */
    leaf_model = level_alloc((u32)lv.nleafs * 2);
    view_key.m = NULL;
    model_next = level_alloc((u32)lv.nmodels * 2 + 2);
    for (i = 0; i < lv.nleafs; ++i)
        leaf_model[i] = -1;
    for (i = 1; i < lv.nmodels; ++i)
    {
        const q_model   *m = &lv.models[i];
        s32             p[3];
        int             k, l;

        if (lv.movers[i].kind == MV_NONE)
            continue;                       /* triggers: never drawn */
        for (k = 0; k < 3; ++k)
            p[k] = (m->mins[k] >> 1) + (m->maxs[k] >> 1);
        l = level_leaf(p);
        model_next[i] = leaf_model[l];
        leaf_model[l] = (s16)i;
    }
    /* the glow for sprites: 32x32, 4bpp, a radial ramp (index 0, round the edge, transparent) */
    {
        u8  *t = vram + base;
        u16 *lut;
        int y, xx;

        glow_vram = base;
        for (y = 0; y < 32; ++y)
            for (xx = 0; xx < 32; xx += 2)
            {
                int k2, v2[2];

                for (k2 = 0; k2 < 2; ++k2)
                {
                    int dx = xx + k2 - 16, dy = y - 16, d2 = dx * dx + dy * dy;

                    v2[k2] = d2 >= 256 ? 0 : 15 - d2 * 15 / 256;
                    if (v2[k2] < 1 && d2 < 256)
                        v2[k2] = 1;
                }
                t[y * 16 + xx / 2] = (u8)(v2[0] << 4 | v2[1]);
            }
        base += 512;
        glow_lut = base;
        lut = (u16 *)(vram + base);
        lut[0] = 0;
        for (y = 1; y < 16; ++y)
        {
            int c2 = 4 + y * 27 / 15;       /* dim at the edge, white in the middle */

            lut[y] = (u16)(0x8000 | c2 << 10 | c2 << 5 | c2);
        }
        base += 32;
        free -= 544;
    }
    /* colour tables: all of them, for good; the models' after the level's */
    lut_vram = base;
    luts_copy(base, lv.luts, (u32)lv.nluts * 16);
    base += (u32)lv.nluts * 32;
    free -= (u32)lv.nluts * 32;
    ntex_all = lv.ntextures;
    for (c = 0; c < nmodels_loaded; ++c)
    {
        q_mdl   *md = &models[c];
        u32     n = (u32)(md->nskins * md->ntex), nl = (u32)(md->lutmap ? md->nluts : md->nskins * md->nluts);

        if (!md->loaded)
            continue;
        md->tex_id0 = ntex_all;
        md->lut0 = (int)((base - lut_vram) / 32);
        ntex_all += (int)n;
        luts_copy(base, md->luts, nl * 16);
        base += nl * 32;
        free -= nl * 32;
    }
#ifdef DSP_LIGHT
    /* the DSP's normals (every model's are Quake's 162), words, padded with nothing */
    memset(dspl_norm, 0, sizeof(dspl_norm));
    for (c = 0; c < nmodels_loaded; ++c)
        if (models[c].loaded)
        {
            for (i = 0; i < 162 * 3; ++i)
                dspl_norm[i] = models[c].normals[i];
            break;
        }
#endif
    /* the guns' two slots (src/view.c): room for the biggest's textures; and one gun's colour
       tables, the one to be drawn's (r_view_luts: the other's three frames gone by then) */
    for (c = 0; c < VIEW_SLOTS; ++c)
    {
        q_mdl   *md = &models[MDL_VIEW0 + c];

        md->loaded = false;
        md->tex_id0 = ntex_all;
        md->lut0 = (int)((base - lut_vram) / 32);
        ntex_all += VIEW_MAX_TEX;
    }
    base += VIEW_MAX_TEX * 32;
    free -= VIEW_MAX_TEX * 32;
    /* the rest: tile-sized slots, in two parts, one each CPU's (SPLIT_M% the master's) */
    slot_bytes = (u32)(lv.N * lv.N / 2);
    slot_vram = base;
    i = (int)(free / slot_bytes);
    if (i > MAX_SLOTS)
        i = MAX_SLOTS;
    vdp_tex_release(base + (u32)i * slot_bytes);
    for (c = 0; c < 2; ++c)
    {
        r_ctx *x = &ctx[c];

        x->slot0 = c ? i * SPLIT_M / 100 : 0;
        x->nslots = c ? i - i * SPLIT_M / 100 : i * SPLIT_M / 100;
        x->hand = 0;
        x->tex_slot = level_alloc((u32)ntex_all * 2);
        memset(x->tex_slot, 0xFF, (u32)ntex_all * 2);
    }
    for (c = 0; c < MAX_SLOTS; ++c)
        slot_tex[c] = SLOT_NONE;
    ctx[0].nup = ctx[1].nup = 0;            /* (no late uploads from the last level's) */
    vdp_set_list_hook(r_late_uploads);
}

/* A texture into its slot: queued for the SCU's DMA at the frame's end (the
   cart to VDP1's VRAM on the SCU's own buses, not the CPUs'; a CPU's store
   to VRAM is 111 cycles), or copied now if the queue's full. Late: into a
   slot two frames back, queued till VDP1's done with that frame
   (r_late_uploads; the caller's seen there's room in the queue) */
static void         tex_upload(r_ctx *x, u32 vram, const void *src, u32 bytes, bool late)
{
    if (late || (r_dma_uploads && x->nup < UPQ && !(((u32)src | vram | bytes) & 3)))
    {
        x->up_src[x->nup] = src;
        x->up_dst[x->nup++] = vram << 8 | bytes >> 2 | (late ? UP_LATE : 0);
    }
    else
        memcpy((u8 *)VDP1_VRAM + vram, src, bytes);
}

/* a slot this CPU's part of the cache can have: unused for age frames (or never used), -1 none */
static int          slot_find(r_ctx *x, int age)
{
    int             n, s;

    for (n = 0; n < x->nslots; ++n)
    {
        s = x->slot0 + x->hand;
        if (++x->hand == x->nslots)
            x->hand = 0;
        if (slot_tex[s] == SLOT_NONE || (u16)(frame - slot_frame[s]) >= age)
            return s;
    }
    return -1;
}

s32                 tex_load(r_ctx *x, int t);      /* (src/mdraw.s calls it) */

/* A texture into this CPU's part of the cache: its VRAM offset, -1 if no slot's free. Two
   frames may be in flight, VDP1 drawing one and one waiting for its swap: a slot's reused three
   frames after it was last drawn, its upload at this frame's end. When there are none of those
   (the view turning fast), one two frames back: its upload's late, once VDP1's done with that
   frame (vdp_submit, r_late_uploads), before this one's list goes to it. */
__attribute__((noinline)) s32 tex_load(r_ctx *x, int t)
{
    int             s = -1;
    const q_tex     *tx = NULL;
    const void      *src = NULL;
    u32             bytes = 0, vram;
    bool            late = false;
    u16             lut = 0;

    if (x->full)
        return -1;
    if (t < lv.ntextures)
    {
        tx = &lv.textures[t];
        src = lv.texdata + tx->ofs;
        bytes = (u32)tx->w * tx->h / 2;
    }
    else
    {
        /* a model's: which one, which skin, which polygon's */
        int m;

        for (m = 0; m < MDL_COUNT + VIEW_SLOTS; ++m)
        {
            const q_mdl *md = &models[m];
            int         k = t - md->tex_id0;

            if (md->loaded && k >= 0 && k < md->nskins * md->ntex)
            {
                const q_mtex *mt = &md->tex[k % md->ntex];

                src = md->texdata + (u32)(k / md->ntex) * md->per_skin + mt->ofs;
                bytes = (u32)mt->w * mt->h / 2;
                /* its colour table, kept with the slot (the commands' colr: mdraw.s, draw_model):
                   the map read now, not a polygon at a time (it's on the cart) */
                lut = (u16)(md->lut0 + (md->lutmap ? md->lutmap[k]
                                        : (k / md->ntex) * md->nluts + (md->nluts > 1 ? mt->lut : 0)));
                break;
            }
        }
        if (!src)
            return -1;
    }
    if (!x->tight && (s = slot_find(x, 3)) < 0)
        x->tight = true;
    if (x->tight)
    {
        if (!r_dma_uploads || x->nup >= UPQ || (((u32)src | bytes) & 3) || (s = slot_find(x, 2)) < 0)
        {
            x->full = true;
            ++x->st.nocache;
            return -1;
        }
        late = true;
        ++x->st.late;
    }
    if (slot_tex[s] != SLOT_NONE)
        x->tex_slot[slot_tex[s]] = 0xFFFF;
    slot_tex[s] = (u16)t;
    x->tex_slot[t] = (u16)s;
    slot_frame[s] = frame;
    vram = slot_vram + (u32)s * slot_bytes;
    if (tx)
    {
        slot_lut[s] = tx->lut;
        slot_w[s] = (u8)tx->w;
    }
    else
    {
        slot_lut[s] = lut;
        ++x->st.muploads;
    }
    tex_upload(x, vram, src, bytes, late);
    ++x->st.uploads;
    return (s32)vram;
}

/* a texture's VRAM offset (bytes), uploading it if need be; -1: no slot free */
static inline s32   tex_vram(r_ctx *x, int t)
{
    int             s = x->tex_slot[t];

    if (s == 0xFFFF)
        return tex_load(x, t);
    slot_frame[s] = frame;
    return (s32)(slot_vram + (u32)s * slot_bytes);
}

/* ---- visibility ---- */

static void         mark_leaves(int cluster)
{
    const u8        *pvs;
    int             i;

    if (++visframe == 0)
    {
        memset(node_vis, 0, (u32)lv.nnodes * 2);
        memset(leaf_vis, 0, (u32)lv.nleafs * 2);
        visframe = 1;
    }
    pvs = level_pvs(cluster);
    level_facevis(cluster, face_vis);
    for (i = 0; i < lv.nleafs; ++i)
    {
        int c = lv.leafs[i].cluster, n;

        if (c < 0 || !(pvs[(u32)c >> 3] & bitm[c & 7]))
            continue;
        leaf_vis[i] = visframe;
        for (n = leaf_parent[i]; n >= 0 && node_vis[n] != visframe; n = node_parent[n])
            node_vis[n] = visframe;
    }
}

/* is the PVS that's marked (r_leaf_in_pvs's) this cluster's? (render_world marks the
   camera's when it changes cluster) */
bool                r_pvs_marked(int cluster)
{
    return cluster >= 0 && cluster == view_cluster;
}

/* ---- faces ---- */

static inline void  to_view(const s32 *w, v3 *o)
{
    o->x = fmul(w[0], cam.right[0]) + fmul(w[1], cam.right[1]) + fmul(w[2], cam.right[2]);
    o->y = fmul(w[0], cam.up[0]) + fmul(w[1], cam.up[1]) + fmul(w[2], cam.up[2]);
    o->z = fmul(w[0], cam.fwd[0]) + fmul(w[1], cam.fwd[1]) + fmul(w[2], cam.fwd[2]);
}

/* which of the view frustum's planes a point is outside (no divide; each
   plane on its own, so behind the camera it can be outside two opposite) */
static inline u8    view_oc(s32 x, s32 y, s32 z)
{
#if CX == FOCAL
    s32             tx = z, ty = fmul(z, ky);       /* (90 degrees across: the sides are x = +-z) */
#else
    s32             tx = fmul(z, kx), ty = fmul(z, ky);
#endif
    u8              oc = 0;

    if (z < NEAR_Z)
        oc |= OC_NEAR;
    if (x < -tx)
        oc |= OC_LEFT;
    if (x > tx)
        oc |= OC_RIGHT;
    if (y > ty)
        oc |= OC_TOP;
    if (y < -ty)
        oc |= OC_BOTTOM;
    return oc;
}

/* the top 32 bits of a 64-bit product: a 16.16 times a 16.16, as an integer (DMULS.L, then MACH) */
static inline s32   mul_hi(s32 a, s32 b)
{
    return (s32)(((s64)a * b) >> 32);
}

/* a screen position from the divide's result (FOCAL / z, 16.16), x << 16 | y */
static inline u32   screen_xy(s32 x, s32 y, s32 r)
{
    s32             sx = iclamp(CX + mul_hi(x, r), -CLAMP_XY, CLAMP_XY);
    s32             sy = iclamp(CY - mul_hi(y, r), -CLAMP_XY, CLAMP_XY);

    return (u32)sx << 16 | (u16)sy;
}

/* screen position of a point in front of the near plane */
static inline u32   project(r_ctx *c, s32 x, s32 y, s32 z)
{
    PROF(++c->st.proj);
    divu_start(FOCAL, 0, z);                /* FOCAL << 32 / z: FOCAL / z in 16.16 */
    return screen_xy(x, y, divu_result());
}

/* a grid vertex's screen position: the grid projects every point in front of
   the near plane as it goes (and nothing asks for one behind it) */
static inline u32   gv_xy(r_ctx *c, const gv *v)
{
    return v->xy;
}

#define XY_X(p)         ((s32)(p) >> 16)
#define XY_Y(p)         ((s32)(s16)(p))

static inline void  lerp3(v3 *o, const v3 *p, const v3 *q, s32 t)
{
    o->x = p->x + fmul(q->x - p->x, t);
    o->y = p->y + fmul(q->y - p->y, t);
    o->z = p->z + fmul(q->z - p->z, t);
}

/* how far from p to q the near plane is (16.16); p behind it, q in front */
static inline s32   near_t(const v3 *p, const v3 *q)
{
    return fdiv(NEAR_Z + 16 - p->z, q->z - p->z);
}

/* A cell crossing the near plane. q: its corners A B C D (texture order),
   rows: the texture's rows, which run from edge AB to edge DC (or from AD to
   BC for a transposed texture). If the near plane cuts off whole rows' worth
   of one end, those rows are dropped from the texture (srca/height) and the
   corners moved to match: exact. Otherwise the corners behind are pulled
   onto the plane along an edge, squashing the texture a little.
   ra: rows dropped from the start must be a multiple of it (srca is in 8 bytes).
   Returns false if nothing's left. */
static __attribute__((noinline)) bool near_clip(v3 *q, int *ty0, int *th, bool transposed, int ra)
{
    int             m = 0, k, i;
    /* the two cases: which corners are behind (a mask), and the two of them */
    static const s8 cases[2][2][3] = {
        { { 3, 0, 1 }, { 12, 3, 2 } },      /* rows along v: A B the first row's edge, D C the last's */
        { { 9, 0, 3 }, { 6, 1, 2 } },       /* transposed: A D first, B C last */
    };
    const s8        (*c)[3] = cases[transposed];

    for (i = 0; i < 4; ++i)
        if (q[i].z < NEAR_Z)
            m |= 1 << i;
    for (i = 0; i < 2; ++i)
    {
        if (m != c[i][0])
            continue;
        {
            int a = c[i][1], b = c[i][2];
            /* a and b are behind; the corners across the rows from them are in front */
            int oa = transposed ? (a == 0 ? 1 : a == 3 ? 2 : a == 1 ? 0 : 3) : (a == 0 ? 3 : a == 1 ? 2 : a == 3 ? 0 : 1);
            int ob = transposed ? (b == 0 ? 1 : b == 3 ? 2 : b == 1 ? 0 : 3) : (b == 0 ? 3 : b == 1 ? 2 : b == 3 ? 0 : 1);
            s32 t = imax(near_t(&q[a], &q[oa]), near_t(&q[b], &q[ob]));
            v3  na, nb;

            k = (int)(((s64)t * *th + 0xFFFF) >> 16);
            if (i == 0)
                k = (k + ra - 1) / ra * ra;
            if (k >= *th)
                return false;
            t = k * rcp[*th];
            lerp3(&na, &q[a], &q[oa], t);
            lerp3(&nb, &q[b], &q[ob], t);
            q[a] = na;
            q[b] = nb;
            if (i == 0)
                *ty0 += k;                          /* the first rows went */
            *th -= k;
            return true;
        }
    }
    /* anything else: pull each corner behind onto the plane, towards a neighbour in front */
    {
        v3  o[4];

        for (i = 0; i < 4; ++i)
        {
            int n1 = (i + 1) & 3, n3 = (i + 3) & 3, n2 = (i + 2) & 3, to;

            o[i] = q[i];
            if (!(m & (1 << i)))
                continue;
            to = !(m & (1 << n3)) ? n3 : !(m & (1 << n1)) ? n1 : n2;
            if (m & (1 << to))
                return false;
            lerp3(&o[i], &q[i], &q[to], near_t(&q[i], &q[to]));
        }
        for (i = 0; i < 4; ++i)
            q[i] = o[i];
    }
    return true;
}

/* A command slot in this CPU's bucket. Pushed (x->fifo false): the newest is
   drawn first, so things go in nearest first. Appended: drawn in order, so
   farthest first. *link: the LINK to write into the new command. */
static inline u32   *cmd_alloc(r_ctx *x, u32 *link)
{
    vdp_writer      *w = x->w;
    int             i = w->count, b = x->bucket;

    if (i >= w->cmax)
    {
        ++x->st.dropped;
        return NULL;
    }
    *link = 0;
    if (x->fifo)
    {
        if (w->head[b] < 0)
            w->head[b] = (s16)i;
        else
            w->cmds[w->tail[b]].link = (u16)(w->link_base + i * (sizeof(vdp1_cmd) >> 3));
        w->tail[b] = (s16)i;
    }
    else
    {
        if (w->head[b] < 0)
            w->tail[b] = (s16)i;
        else
            *link = (u16)(w->link_base + w->head[b] * (sizeof(vdp1_cmd) >> 3));
        w->head[b] = (s16)i;
    }
    w->count = i + 1;
    return (u32 *)&w->cmds[i];
}

/* a corner's Gouraud colour with the dynamic lights near it added (Quake's falloff, squared) */
static u16          dlight_add(u8 mask, const v3 *v, u16 c)
{
    int             r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, i;

    for (i = 0; mask; ++i, mask >>= 1)
    {
        s32 dx, dy, dz, d2, f;

        if (!(mask & 1))
            continue;
        dx = (v->x - dl[i].x) >> 16;
        dy = (v->y - dl[i].y) >> 16;
        dz = (v->z - dl[i].z) >> 16;
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= dl[i].r2)
            continue;
        f = (s32)((u32)((dl[i].r2 - d2) * dl[i].inv) >> 8);    /* 0..65536 (positive: an unsigned shift, not a library call) */
        r += (dl[i].r * f) >> 16;
        g += (dl[i].g * f) >> 16;
        b += (dl[i].b * f) >> 16;
    }
    return (u16)(0x8000 | (imin(b, 31) << 10) | (imin(g, 31) << 5) | imin(r, 31));
}

/* one cell's command: corners xy in the cell's order A (u0, v0), B (u1, v0), C, D */
static void         cell_pos(const r_ctx *x, int i, int j, v3 *P);

static __attribute__((noinline)) void cell_emit(r_ctx *x, const q_cell *cell, int ty0, int th,
                                                const u32 *xy, const u16 *light, int stride, int i, int j)
{
    int             t = cell->tex & CELL_TEX, s = x->tex_slot[t], tw;
    vdp_writer      *w = x->w;
    s32             vram;
    u32             lut, *d, link;
    u16             la, lb, lc, ld, grda, tl;

    if (r_debug == 2)
        return;
    if (s == 0xFFFF)
    {
        if ((vram = tex_load(x, t)) < 0)
            return;
        s = x->tex_slot[t];
    }
    else
    {
        slot_frame[s] = frame;
        vram = (s32)(slot_vram + (u32)s * slot_bytes);
    }
    if (r_debug == 3)
        return;
    tl = slot_lut[s];
    tw = slot_w[s];
    lut = cell->tex & CELL_FULL ? ((const q_cell_fast *)cell)->lut : (u32)(tl & 0x7FFF);
    /* the command goes out in 32-bit stores */
    if (!(d = cmd_alloc(x, &link)))
        return;
    la = light[0];
    lb = light[1];
    lc = light[stride + 1];
    ld = light[stride];
    if (x->dmask && !x->lit)
    {
        v3 P[4];

        cell_pos(x, i, j, P);
        la = dlight_add(x->dmask, &P[0], la);
        lb = dlight_add(x->dmask, &P[1], lb);
        lc = dlight_add(x->dmask, &P[2], lc);
        ld = dlight_add(x->dmask, &P[3], ld);
    }
    d[0] = 0x10020000u | link;              /* jump assign, distorted sprite */
    d[1] = x->cpmod | ((lut_vram + lut * 32) >> 3);
    d[2] = ((vram + (u32)ty0 * (u32)(tw >> 1)) >> 3) << 16 | (u32)(((tw >> 3) << 8) | th);
    d[3] = xy[0];
    d[5] = xy[2];
    if (tl & TEX_TRANSPOSED)
    {
        /* the texture's rows run down the cell: A (u0, v0), B (u0, v1), C (u1, v1), D (u1, v0) */
        d[4] = xy[3];
        d[6] = xy[1];
        grda = vdp_gouraud_fast(w, (u32)la << 16 | ld, (u32)lc << 16 | lb);
    }
    else
    {
        d[4] = xy[1];
        d[6] = xy[3];
        grda = vdp_gouraud_fast(w, (u32)la << 16 | lb, (u32)lc << 16 | ld);
    }
    d[7] = (u32)grda << 16;
}

/* where grid point (i, j) of the face being drawn is in view space: its grid
   steps (the edge column and row, whole ones between) from the first point.
   The grid adds the same steps, so it's the same point exactly */
static void         grid_step(v3 *o, const v3 *e0, const v3 *d, const v3 *e1, int n, int i)
{
    if (i == 0)
        o->x = o->y = o->z = 0;
    else if (n == 1 || i < n)
    {
        o->x = e0->x + d->x * (i - 1); o->y = e0->y + d->y * (i - 1); o->z = e0->z + d->z * (i - 1);
    }
    else
    {
        o->x = e0->x + d->x * (n - 2) + e1->x; o->y = e0->y + d->y * (n - 2) + e1->y;
        o->z = e0->z + d->z * (n - 2) + e1->z;
    }
}

/* Water (FF_WARP faces) moves: its grid points rise and fall along its plane's
   normal, and a ripple of light crosses it. Both go by where a point is in the
   world, so faces meeting agree at their edges, and fade out between WAVE_NEAR
   and WAVE_FAR, so far water (and its edges with near water) is as it was. */
#define WAVE_NEAR       FIX(192)
#define WAVE_FAR        FIX(384)
#define WAVE_FADE       (341)               /* 1 / (WAVE_FAR - WAVE_NEAR), 16.16 */
#define WAVE_H          FIX(1.25)           /* half the waves' height each way (two waves added) */
#define RIPPLE          (3)                 /* the light's, each way (of Gouraud's 31) */

/* at view-space point V: how far it moves along the water's normal (16.16), and
   the ripple (light steps) */
static s32          wave_at(const v3 *V, int *ripple)
{
    s32             fade, wx, wy, s;

    *ripple = 0;
    if (V->z >= WAVE_FAR)
        return 0;
    fade = V->z <= WAVE_NEAR ? FIX(1) : fmul(WAVE_FAR - V->z, WAVE_FADE);
    /* where it is in the world (the view's axes turned back) */
    wx = cam.pos[0] + fmul(V->x, cam.right[0]) + fmul(V->y, cam.up[0]) + fmul(V->z, cam.fwd[0]);
    wy = cam.pos[1] + fmul(V->x, cam.right[1]) + fmul(V->y, cam.up[1]) + fmul(V->z, cam.fwd[1]);
    /* two waves 128 units long across each other, and the ripple 64 units long, faster */
    s = fsin((wx >> 7) + (int)fmul((s32)r_clock, 9000)) + fsin((wy >> 7) + (int)fmul((s32)r_clock, 7000));
    *ripple = (int)((fmul(fsin(((wx + wy) >> 6) + (int)fmul((s32)r_clock, 21000)), fade) * RIPPLE + 32768) >> 16);
    return fmul(fmul(s, WAVE_H), fade);
}

static void         wave_move(const r_ctx *x, v3 *V)
{
    int             r;
    s32             h = wave_at(V, &r);

    V->x += fmul(x->wn.x, h);
    V->y += fmul(x->wn.y, h);
    V->z += fmul(x->wn.z, h);
}

/* the four corners of cell (i, j): A B C D */
static __attribute__((noinline)) void cell_pos(const r_ctx *x, int i, int j, v3 *P)
{
    const v3        *f = (const v3 *)&x->gk[GK_F0];     /* f0, dv, f1 */
    v3              a0, a1, b0, b1;

    grid_step(&a0, &x->ga.e0, &x->ga.d, &x->ga.e1, x->fa.fnu, i);
    grid_step(&a1, &x->ga.e0, &x->ga.d, &x->ga.e1, x->fa.fnu, i + 1);
    grid_step(&b0, &f[0], &f[1], &f[2], x->fa.fnv, j);
    grid_step(&b1, &f[0], &f[1], &f[2], x->fa.fnv, j + 1);
    P[0].x = x->fa.fo.x + a0.x + b0.x; P[0].y = x->fa.fo.y + a0.y + b0.y; P[0].z = x->fa.fo.z + a0.z + b0.z;
    P[1].x = x->fa.fo.x + a1.x + b0.x; P[1].y = x->fa.fo.y + a1.y + b0.y; P[1].z = x->fa.fo.z + a1.z + b0.z;
    P[2].x = x->fa.fo.x + a1.x + b1.x; P[2].y = x->fa.fo.y + a1.y + b1.y; P[2].z = x->fa.fo.z + a1.z + b1.z;
    P[3].x = x->fa.fo.x + a0.x + b1.x; P[3].y = x->fa.fo.y + a0.y + b1.y; P[3].z = x->fa.fo.z + a0.z + b1.z;
    if (x->wave)
    {
        int k;

        for (k = 0; k < 4; ++k)
            wave_move(x, &P[k]);
    }
}

/* Row j of the face's lights with the dynamic lights added: at the grid
   points cell_pos finds (the same steps, summed the same way), so each
   corner comes out as cell_emit's did, once a point rather than once a
   corner (up to four times a point). Out to out[0..fnu] */
static __attribute__((noinline)) void dl_row(const r_ctx *x, u16 *out, const u16 *raw, int j)
{
    const v3        *f = (const v3 *)&x->gk[GK_F0];
    v3              a, b, V;
    int             i, n = x->fa.fnu;

    grid_step(&b, &f[0], &f[1], &f[2], x->fa.fnv, j);
    a.x = a.y = a.z = 0;
    for (i = 0; i <= n; ++i)
    {
        if (i == 1)
            a = x->ga.e0;
        else if (i > 1)
        {
            const v3 *st = i < n ? &x->ga.d : &x->ga.e1;       /* (grid_step's e0 + d (i - 1), + e1 at the end) */

            a.x += st->x; a.y += st->y; a.z += st->z;
        }
        V.x = x->fa.fo.x + a.x + b.x; V.y = x->fa.fo.y + a.y + b.y; V.z = x->fa.fo.z + a.z + b.z;
        out[i] = dlight_add(x->dmask, &V, raw[i]);
    }
}

/* The dynamic lights at all of a whole face's grid points (dl_row's, a face at a time): the
   point's sums (r g b, 10 bits each: 31 and 8 lights of 31 fit) packed in a word, each light
   in reach of the face (face_dlights) over its points, clamped and made Gouraud colours at the
   end. The same sums as dlight_add's, bit for bit: the same point (its steps added in another
   order), each light's share truncated the same way, the clamp after them all. raw and out
   may be the same. (OPT=-DNO_DL_ASM: the C it was, a row at a time, each row's lights tested
   against its box and a row none reaches copied) */
/* how far (whole units squared) a light at l is from the box lo .. hi (16.16), each way a unit
   less than it is: the sums' points round their offsets down, so this is never more than any
   point's d2 in the box (a light it says is out of reach is out of reach of all of them) */
static s32          dl_boxd2(s32 lx, s32 ly, s32 lz, const v3 *lo, const v3 *hi)
{
    s32             d, d2 = 0;

#define DL_AXIS(c, l) \
    d = l < lo->c ? ((lo->c - l) >> 16) - 1 : l > hi->c ? ((l - hi->c) >> 16) - 1 : 0; \
    if (d > 0) \
        d2 += d * d;
    DL_AXIS(x, lx)
    DL_AXIS(y, ly)
    DL_AXIS(z, lz)
#undef DL_AXIS
    return d2;
}

/* the box of points p (n of them) into lo, hi */
static void         dl_box(const v3 *p, int n, v3 *lo, v3 *hi)
{
    int             k;

    *lo = *hi = p[0];
    for (k = 1; k < n; ++k)
    {
        if (p[k].x < lo->x) lo->x = p[k].x; else if (p[k].x > hi->x) hi->x = p[k].x;
        if (p[k].y < lo->y) lo->y = p[k].y; else if (p[k].y > hi->y) hi->y = p[k].y;
        if (p[k].z < lo->z) lo->z = p[k].z; else if (p[k].z > hi->z) hi->z = p[k].z;
    }
}

#ifdef NO_DL_ASM
static __attribute__((noinline)) void dl_face(r_ctx *x, u16 *out, const u16 *raw, const dl_light *L)
{
    const face_args *a = &x->fa;
    const v3        *fs = (const v3 *)&x->gk[GK_F0], *e0 = &x->ga.e0, *d = &x->ga.d, *e1 = &x->ga.e1;
    int             nu = a->fnu, nv = a->fnv, i, j, li;
    unsigned        mask = x->dmask;
    u32             acc[MAX_ROW];
    v3              across, b;              /* a whole row; the row's start (grid_step's, stepped) */

    grid_step(&across, e0, d, e1, nu, nu);
    b.x = b.y = b.z = 0;
    for (j = 0; j <= nv; ++j, raw += nu + 1, out += nu + 1)
    {
        s32         r0x, r0y, r0z, r1x, r1y, r1z;
        v3          lo, hi;
        unsigned    rm = 0, m;

        if (j == 1)
            b = fs[0];
        else if (j > 1)
        {
            const v3 *st = j < nv ? &fs[1] : &fs[2];

            b.x += st->x; b.y += st->y; b.z += st->z;
        }
        /* the lights in reach of the row (its points are on the line between its ends) */
        r0x = a->fo.x + b.x; r0y = a->fo.y + b.y; r0z = a->fo.z + b.z;
        r1x = r0x + across.x; r1y = r0y + across.y; r1z = r0z + across.z;
        if (r1x < r0x) { lo.x = r1x; hi.x = r0x; } else { lo.x = r0x; hi.x = r1x; }
        if (r1y < r0y) { lo.y = r1y; hi.y = r0y; } else { lo.y = r0y; hi.y = r1y; }
        if (r1z < r0z) { lo.z = r1z; hi.z = r0z; } else { lo.z = r0z; hi.z = r1z; }
        for (li = 0, m = mask; m; ++li, m >>= 1)
            if (m & 1 && dl_boxd2(L[li].x, L[li].y, L[li].z, &lo, &hi) < L[li].r2)
                rm |= 1u << li;
        if (!rm)
        {
            for (i = 0; i <= nu; ++i)
                out[i] = (u16)(raw[i] | 0x8000);        /* (as unpacked and packed again) */
            continue;
        }
        for (i = 0; i <= nu; ++i)
        {
            u32 c = raw[i];

            acc[i] = (c & 31) | (c >> 5 & 31) << 10 | (c >> 10 & 31) << 20;
        }
        for (li = 0; rm; ++li, rm >>= 1)
        {
            s32         r2, inv, px, py, pz;
            u32         lr, lg, lb;

            if (!(rm & 1))
                continue;
            r2 = L[li].r2;
            inv = L[li].inv;
            lr = L[li].r;
            lg = L[li].g;
            lb = L[li].b;
            px = r0x - L[li].x;
            py = r0y - L[li].y;
            pz = r0z - L[li].z;
            for (i = 0; i <= nu; ++i)
            {
                s32 dx, dy, dz, d2;

                if (i == 1)
                {
                    px += e0->x; py += e0->y; pz += e0->z;
                }
                else if (i > 1)
                {
                    const v3 *st = i < nu ? d : e1;

                    px += st->x; py += st->y; pz += st->z;
                }
                dx = px >> 16;
                dy = py >> 16;
                dz = pz >> 16;
                d2 = dx * dx + dy * dy + dz * dz;
#ifdef FIGHT_BENCH
                ++x->st.n_dlpts;
#endif
                if (d2 < r2)
                {
                    u32 f = (u32)((r2 - d2) * inv) >> 8;

                    acc[i] += ((lr * f) >> 16) | ((lg * f) >> 16) << 10 | ((lb * f) >> 16) << 20;
                }
            }
        }
        for (i = 0; i <= nu; ++i)
        {
            u32 c = acc[i];

            out[i] = (u16)(0x8000 | imin((int)(c >> 20), 31) << 10 | imin((int)(c >> 10 & 1023), 31) << 5
                           | imin((int)(c & 1023), 31));
        }
    }
}
#else
/* (the same, a face at a time: the whole face's sums unpacked, each light in reach of any of it
   over all its points (src/dlight.s dl_rows: its rows' starts stepped there too), packed at the
   end; no row's reach tested: in a fight a lit face is ~16 points, and those tests, calls and
   setups a row cost more than the points they'd skip) */
typedef struct { s32 r2, inv; u32 lr, lg, lb; s32 p[3], e0[3], d[3], e1[3], f0[3], dv[3], f1[3]; s32 nu, nv, j; } dl_pts;
void                dl_rows(u32 *acc, dl_pts *q);

static __attribute__((noinline)) void dl_face(r_ctx *x, u16 *out, const u16 *raw, const dl_light *L)
{
    const face_args *a = &x->fa;
    const s32       *fs = &x->gk[GK_F0];
    int             np = (a->fnu + 1) * (a->fnv + 1), i, li;
    unsigned        m;
    u32             acc[WHOLE_MAX];
    dl_pts          q;

    for (i = 0; i < np; ++i)
    {
        u32 c = raw[i];

        acc[i] = (c & 31) | (c >> 5 & 31) << 10 | (c >> 10 & 31) << 20;
    }
    q.e0[0] = x->ga.e0.x; q.e0[1] = x->ga.e0.y; q.e0[2] = x->ga.e0.z;
    q.d[0] = x->ga.d.x; q.d[1] = x->ga.d.y; q.d[2] = x->ga.d.z;
    q.e1[0] = x->ga.e1.x; q.e1[1] = x->ga.e1.y; q.e1[2] = x->ga.e1.z;
    for (i = 0; i < 3; ++i)
    {
        q.f0[i] = fs[i];
        q.dv[i] = fs[3 + i];
        q.f1[i] = fs[6 + i];
    }
    q.nu = a->fnu;
    q.nv = a->fnv;
    for (li = 0, m = x->dmask; m; ++li, m >>= 1)
    {
        if (!(m & 1))
            continue;
        q.r2 = L[li].r2;
        q.inv = L[li].inv;
        q.lr = L[li].r;
        q.lg = L[li].g;
        q.lb = L[li].b;
        q.p[0] = a->fo.x - L[li].x;
        q.p[1] = a->fo.y - L[li].y;
        q.p[2] = a->fo.z - L[li].z;
        q.j = 0;
        dl_rows(acc, &q);
#ifdef FIGHT_BENCH
        x->st.n_dlpts += np;
#endif
    }
    for (i = 0; i < np; ++i)
    {
        u32 c = acc[i];

        out[i] = (u16)(0x8000 | imin((int)(c >> 20), 31) << 10 | imin((int)(c >> 10 & 1023), 31) << 5
                       | imin((int)(c & 1023), 31));
    }
}
#endif


/* ---- the walls' dynamic lights, a frame behind (a test, OPT=-DWALLS_AHEAD: off) ----

   Each CPU notes the whole faces it lit as it drew them (wl_record). When the slave's done its
   drawing, while the master finishes the frame, it lights them again for the next frame
   (r_wall_ahead): with this frame's lights, which the next frame's walls go by (a frame behind:
   render_world's dl, lsrc), from the faces' grids in the world (a light's reach is the same from
   anywhere: only the rounding's not the view space's), into one of two buffers (the master may
   still be drawing from the other), a face at a time, until the next frame's first job comes.
   The next frame a face found there (wl_find) isn't lit again; the rest are lit as they're
   drawn, as before. It works (a shade out here and there, the world's rounding), but the slave's
   free time at a frame's end (~1.2 ms, the lists' DMA aside) lights ~3 of a fight frame's 20:
   0.4 ms less while drawing, nothing off the frame (OVERNIGHT.md section 30) */
#define WL_N            (24)                /* faces a buffer */
#define WL_WORDS        (4096)              /* their lights (u16s), at most: what low work RAM has room for */
typedef struct { u16 n, frame; u16 face[WL_N]; u16 *lit[WL_N]; u16 buf[WL_WORDS]; } wl_buf;
static wl_buf       *wl_b[2];               /* (in low work RAM, if there's room) */
static u32          wl_words;               /* ...the room for lights each has */
#ifdef FIGHT_BENCH
u32                 wl_stop[3];             /* (why the slave stopped: the next frame, no room, all done) */
#endif

/* ---- the walls' dynamic lights on the DSP (engine/walls0.dsp, walls1.dsp, walls2.dsp: on unless
   OPT=-DNO_DSP_WALLS, src/q2.h) ----

   Three programs, one after another after the models' job: walls0.dsp picks, of the whole faces
   the CPUs drew last frame, those this frame's lights may light (face_dlights' tests, from the
   cart's copies of their records and planes) and which lights may; walls1.dsp sets out each
   one's numbers (its record's, its axes'); walls2.dsp lights it and writes its lit lights to the
   cart. dw_filter and dw_model are what they work out, in C (OPT=-DWALLS_TEST checks them
   against it at a level's start) */
#if defined(DSP_WALLS) || defined(WALLS_TEST)
#include "q2models.h"
static int          wl_face_setup(r_ctx *x, int fi, const dl_light *L, const q_dlight *ls, int nl);
#define DW_FACES        (60)                /* faces a frame, at most (walls0.dsp's MAXF) */
#define DW_BLKW         (2400)              /* their blocks' words (walls1.dsp's MAXBW: one's 75 at most) */
#define DW_OUTW         (640)               /* their lit lights' words (walls1.dsp's MAXOW: one's 31 at most) */
#define DW_LIGHTS       (3)
#define DW_REC          (256)               /* the faces a CPU notes a frame, at most (dw_rec: 2 a word) */
#define DW_HASH         (128)               /* (dw_find's table) */
static const u8     *dw_prog;               /* the programs (cd/WALLS.BIN), on the cart */
static u32          *dw_blocks, *dw_out[2], *dw_rec[2], *dw_acc[2]; /* (on the cart) */
static s32          *dw_lt, *dw_lt0;        /* walls2.dsp's lights (the first again last); walls0.dsp's, its
                                               constants (on the cart) */
static u32          dw_fb;                  /* the cart's faces >> 2 (a record's address: + 8 its index) */

/* room in walls2.dsp's RAM for f: 12 points a row, 24 rows, 62 in all */
static __attribute__((unused)) bool dw_fits(const q_face *f)
{
    return f->nu < 12 && f->nv < 24 && (f->nu + 1) * (f->nv + 1) <= 62;
}

/* the lights for the programs: walls2.dsp's (-x -y -z (16.16, the world), r2, -(inv << 8), r g b)
   and walls0.dsp's (x y z, radius, x y z + reach, reach - x y z: reach the radius + 2 units) */
static __attribute__((noinline)) void dw_lights(const q_dlight *ls, int nl)
{
    int             k, c;

    for (k = 0; k < nl; ++k)
    {
        const q_dlight *l = &ls[k];
        s32         rad = l->radius >> 16, r2 = imax(rad * rad, 4), reach = l->radius + FIX(2), *o = &dw_lt[k * 8],
                    *o0 = &dw_lt0[k * 10];

        for (c = 0; c < 3; ++c)
        {
            o[c] = -l->pos[c];
            o0[c] = l->pos[c];
            o0[4 + c] = l->pos[c] + reach;
            o0[7 + c] = reach - l->pos[c];
        }
        o[3] = r2;
        o[4] = -(((1 << 24) / r2) << 8);
        o[5] = l->r;
        o[6] = l->g;
        o[7] = l->b;
        o0[3] = l->radius;
    }
    for (c = 0; c < 8; ++c)
        dw_lt[24 + c] = dw_lt[c];
    dw_lt0[30] = FIX(8);
    dw_lt0[31] = 5;
    dw_lt0[32] = 6;
}

/* what walls0.dsp works out for face fi, lights lt0 (nl): the lights that may light it */
static __attribute__((unused, cold)) unsigned dw_filter(int fi, const s32 *lt0, int nl)
{
    const q_face    *f = &lv.faces[fi];
    const q_plane   *pl = &lv.planes[f->plane];
    const s32       *ax = &lv.axes[f->axes * 6];
    s32             s = f->flags & FF_BACK ? -1 : 1, lo[3], hi[3];
    unsigned        mp = 0, m = 0;
    int             l, c;

    for (l = 0; l < nl; ++l)
    {
        const s32   *L = &lt0[l * 10];
        s32         nd = (s32)(((s64)pl->n[0] * L[0] + (s64)pl->n[1] * L[1] + (s64)pl->n[2] * L[2]) >> 16);
        s32         md = (pl->dist - nd) * s;      /* -d, d its distance from the plane, the face's way */

        if (L[3] + md > 0 && FIX(8) - md > 0)
            mp |= 1u << l;
    }
    if (!mp)
        return 0;
    for (c = 0; c < 3; ++c)
    {
        s32 A = ax[c] * f->nu, B = ax[3 + c] * f->nv, M = imax(A, 0) + imax(B, 0);

        lo[c] = f->origin[c] + A + B - M;
        hi[c] = f->origin[c] + M;
    }
    for (l = 0; l < nl; ++l)
    {
        const s32   *L = &lt0[l * 10];

        if (!(mp & 1u << l))
            continue;
        for (c = 0; c < 3; ++c)
            if (L[4 + c] - lo[c] < 0 || L[7 + c] + hi[c] < 0)
                break;
        if (c == 3)
            m |= 1u << l;
    }
    return m;
}

/* what walls1.dsp and walls2.dsp work out for face fi, lights lt (those of mask): its lit lights */
static __attribute__((unused, cold)) void dw_model(int fi, const s32 *lt, unsigned mask, u16 *out)
{
    const q_face    *f = &lv.faces[fi];
    const s32       *ax = &lv.axes[f->axes * 6];
    const u16       *raw = &lv.lights[f->firstlight & 0xFFFFFF];
    int             nu = f->nu, nv = f->nv, N = lv.N, rcp = 65536 / N, i, j, c, l, k = 0;
    s32             dut[3], dvt[3], A[12][3];

    for (c = 0; c < 3; ++c)
    {
        dut[c] = (s32)(((s64)ax[c] * rcp) >> 16);
        dvt[c] = (s32)(((s64)ax[3 + c] * rcp) >> 16);
    }
    for (i = 0; i <= nu; ++i)
    {
        s32 a = i == 0 ? 0 : i == nu ? (nu - 1) * N - f->eu0 + f->eu1 : i * N - f->eu0;

        for (c = 0; c < 3; ++c)
            A[i][c] = a * dut[c];
    }
    for (j = 0; j <= nv; ++j)
    {
        s32 b = j == 0 ? 0 : j == nv ? (nv - 1) * N - f->ev0 + f->ev1 : j * N - f->ev0, R[3];

        for (c = 0; c < 3; ++c)
            R[c] = f->origin[c] + b * dvt[c];
        for (i = 0; i <= nu; ++i, ++k)
        {
            u32 v = raw[k] & 0x7FFF;
            s32 s[3], P[3];

            s[0] = (s32)(v & 31);
            s[1] = (s32)(v >> 5 & 31);
            s[2] = (s32)(v >> 10 & 31);
            for (c = 0; c < 3; ++c)
                P[c] = R[c] + A[i][c];
            for (l = 0; l < DW_LIGHTS; ++l)
            {
                const s32   *L = &lt[l * 8];
                s32         d2 = 0, d, fw;

                if (!(mask & 1u << l))
                    continue;
                for (c = 0; c < 3; ++c)
                {
                    d = (s32)(((s64)P[c] + L[c]) >> 16);
                    d2 += d * d;
                }
                if (d2 - L[3] >= 0)
                    continue;
                fw = (s32)(((s64)(d2 - L[3]) * L[4]) >> 16);
                for (c = 0; c < 3; ++c)
                    s[c] += (s32)(((s64)fw * L[5 + c]) >> 16);
            }
            out[k] = (u16)(0x8000 + imin(s[0], 31) + 32 * imin(s[1], 31) + 1024 * imin(s[2], 31));
        }
    }
}

/* the programs' numbers: the list (n entries), nl lights (dw_lights'), lit lights to out */
static dsp_walls_p  dw_p;                   /* (the level's, dw_level; each job's changed, dw_params) */

/* (a level's start) the programs' numbers that stay the level's */
static __attribute__((cold)) void dw_level(void)
{
    memset(&dw_p, 0, sizeof(dw_p));
    dw_p.blocks = ((u32)dw_blocks & 0x07FFFFFF) >> 2;
    dw_p.lights = ((u32)dw_lt & 0x07FFFFFF) >> 2;
    dw_p.axes = ((u32)lv.axes_cart & 0x07FFFFFF) >> 2;
    dw_p.raw = ((u32)lv.lights_cart & 0x07FFFFFF) >> 2;
    dw_p.n = (u32)lv.N;
    dw_p.rcp = (u32)(65536 / lv.N);
    dw_p.progf = (((u32)dw_prog + 3072) & 0x07FFFFFF) >> 2;
    dw_p.prog2 = (((u32)dw_prog + 1024) & 0x07FFFFFF) >> 2;
    dw_p.planes = ((u32)lv.planes_cart & 0x07FFFFFF) >> 2;
    dw_p.prog1 = ((u32)dw_prog & 0x07FFFFFF) >> 2;
    dw_p.c7fff = 0x7FFF;
    dw_p.prog0 = (((u32)dw_prog + 2048) & 0x07FFFFFF) >> 2;
    dw_p.faces_cart = dw_fb;
    dw_p.lights0 = ((u32)dw_lt0 & 0x07FFFFFF) >> 2;
}

/* (the DSP stopped) the programs' numbers: the list (n faces' indices from the u16 first), nl lights
   (dw_lights'), lit lights to out, the faces picked to acc */
static void         dw_params(const u16 *first, int n, int nl, const u32 *out, const u32 *acc)
{
    const u32       *list = (const u32 *)((u32)first & ~3u);

    dw_p.half = ((u32)first & 2) != 0;
    if (dw_p.half)
        dsp_walls_word(*(const volatile u32 *)UNCACHED(list++));    /* (that word walls0.dsp has already) */
    dw_p.faces = dw_p.list_n = (u32)n;
    dw_p.out = ((u32)out & 0x07FFFFFF) >> 2;
    dw_p.light_words = nl == 3 ? 32 : (u32)nl * 8;
    dw_p.lights1 = (u32)nl - 1;
    dw_p.list = ((u32)list & 0x07FFFFFF) >> 2;
    dw_p.acc = ((u32)acc & 0x07FFFFFF) >> 2;
    dsp_walls_params(&dw_p);
}

#ifdef DSP_WALLS
/* the walls' dynamic lights on the DSP, a frame behind: each CPU notes the
   whole faces it draws (the record's address, in dw_rec: the master's up from its middle, the
   slave's down, so they're one list); the next frame, after the models' job, the DSP picks
   those that frame's lights may light and lights them (dw_frame: the master, as it starts the
   models' job), for the frame after, whose walls go by those lights (lsrc: a frame behind, as
   the models'); a face lit then that it did is drawn with its (dw_find), the rest by dl_face as
   before */
static u32          dw_count;               /* (frames: which half of dw_rec, dw_out, dw_acc) */
static bool         dw_ran;                 /* (the last frame's job: it lit into dw_last, listed them in dw_lacc) */
static const u32    *dw_last, *dw_lacc, *dw_cur;    /* ...; this frame's lit lights */
static bool         dw_on;                  /* (this frame's faces noted) */
static u32          dw_quiet;               /* (frames since the last with lights) */
#define DW_LINGER       (60)                /* (faces noted for this many frames after the last light) */
static u32          *dw_hash;               /* this frame's faces the DSP lit: stamp << 24 | index << 10 | where
                                               its lit lights are (words), by the index's low bits (the next after,
                                               if taken); DW_HASH, in low work RAM */
static u32          dw_stamp;               /* (this frame's: 1-255, the table cleared as it goes round) */
#ifdef FIGHT_BENCH
static bool         dw_have;                /* (this frame has the DSP's) */
#endif
u32                 dw_stat[6];             /* (FIGHT_BENCH: faces the DSP lit, frames it ran, faces it was
                                               given, us in dw_frame, frames it was still busy, us waited) */

/* (models_to_dsp, the DSP idle) the last job's faces for this frame (dw_find's), then this
   frame's job, for the next frame: the faces the CPUs drew last frame, with this frame's lights,
   after the models' job (dsp_models starts it, the parameters set) */
static bool         dw_frame(void)
{
    int             p = (int)(dw_count & 1), n0, n1, k, nl = r_ndlights;
    u32             a;

    if (!dw_prog)
        return false;
    if (++dw_stamp > 255)
    {
        dw_stamp = 1;
        memset(dw_hash, 0, DW_HASH * 4);
    }
#ifdef FIGHT_BENCH
    dw_have = dw_ran;
#endif
    if (dw_ran)
    {
        /* its list (walls1.dsp's words: index << 16 | where its lit lights are), through the cache (its
           lines forgotten first: the DSP wrote them) */
        const u32   *acc = dw_lacc;
        int         n;

        for (a = (u32)acc; a < (u32)(acc + 1 + DW_FACES); a += 16)
            *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
        n = imin((int)acc[0], DW_FACES);
        for (k = 1; k <= n; ++k)
        {
            u32 w = acc[k], fi = w >> 16;
            int h = (int)fi & (DW_HASH - 1);

            while (dw_hash[h] >> 24 == dw_stamp)
                h = (h + 1) & (DW_HASH - 1);
            dw_hash[h] = dw_stamp << 24 | fi << 10 | (w & 0x3FF);
        }
        dw_cur = dw_last;
        dw_stat[0] += (u32)n;
        dw_ran = false;
    }
    ++dw_count;
    /* this frame's faces noted while there are lights, and for a while after (a fight's next light's
       first frame, then, has the DSP's: ~0.35 ms of each CPU a busy frame, so not when it's quiet) */
    if (nl)
        dw_quiet = 0;
    else if (dw_quiet < 255)
        ++dw_quiet;
#ifdef DW_ALWAYS
    dw_on = r_dl_verts;                     /* (a test: every frame's faces noted, lights or not) */
#else
    dw_on = r_dl_verts && dw_quiet < DW_LINGER;
#endif
    if (!nl || nl > DW_LIGHTS)
        return false;
    n1 = ((const r_ctx *)UNCACHED(&ctx[1]))->wl_nrec;      /* (the slave's still adding, maybe: those so far) */
    n0 = ctx[0].wl_nrec;
    if (!(n0 + n1))
        return false;
#ifdef DW_NOJOB
    return false;                           /* (a test: the faces noted, no job) */
#endif
    dw_lights(r_dlights, nl);
    dw_params((const u16 *)dw_rec[p] + DW_REC - n1, n0 + n1, nl, dw_out[p], dw_acc[p]);
    dw_last = dw_out[p];
    dw_lacc = dw_acc[p];
    dw_ran = true;
    dw_stat[2] += (u32)(n0 + n1);
    ++dw_stat[1];
    return true;
}

/* (each CPU, as it draws) whole face f, drawn with light (its own grid's or the coarse one's), for the
   DSP next frame if walls2.dsp has room for it and it's its own grid: its index (dw_rp's, its CPU's
   way); the count after (the master may be reading it). Not inlined: face_cells has registers enough
   to keep without it */
static __attribute__((noinline)) void dw_note(r_ctx *x, const q_face *f, const u16 *light)
{
    int             n = x->wl_nrec;

    if (f->flags & FF_WARP || !dw_fits(f) || n >= DW_REC
        || (f->flags & FF_LOD && light != &lv.lights[f->firstlight & 0xFFFFFF]))
        return;
    *x->dw_rp = (u16)(((u32)f - (u32)lv.faces) >> 5);
    x->dw_rp += x->dw_step;
    __asm__ volatile ("" : : : "memory");
    x->wl_nrec = n + 1;
}

/* face fi's lit lights (np of them) as the DSP worked them out for this frame, or NULL; its
   lines forgotten first (the DSP wrote them behind the cache) */
static const u16    *dw_find(int fi, int np)
{
    int             h = fi & (DW_HASH - 1);
    u32             e;

#ifdef DW_IGNORE
    return NULL;                            /* (a test: the DSP's work, its results not used) */
#endif
    if (!dw_hash)
        return NULL;
    while ((e = dw_hash[h]) >> 24 == dw_stamp)
    {
        if ((int)(e >> 10 & 0x3FFF) == fi)
        {
            const u16   *o = (const u16 *)(dw_cur + (e & 0x3FF));
            u32         a;

            for (a = (u32)o & ~15u; a < (u32)(o + np); a += 16)
                *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
            return o;
        }
        h = (h + 1) & (DW_HASH - 1);
    }
    return NULL;
}
#endif

#ifdef WALLS_TEST
u32                 wt_res[18];             /* (faces, points, faces different, points different, us, done; points
                                               lit, points not dl_face's (in the world), most a colour's out; faces
                                               whose lights walls0.dsp picked differently; the first point
                                               different: its face, point, the DSP's, dw_model's; the list's
                                               entries, us with the lights out of reach) */

/* (OPT=-DWALLS_TEST, at a level's start) three lights by where you start, the faces near them
   the programs have room for: the lights walls0.dsp picks against dw_filter's, the programs' lit
   lights against dw_model's, and their time */
static __attribute__((cold)) void walls_test(void)
{
    static const s8 at[3][3] = { { 0, 0, 40 }, { 120, 0, 30 }, { 0, -120, 50 } };
    static const u8 col[3][3] = { { 13, 11, 5 }, { 5, 13, 11 }, { 31, 20, 0 } };
    const u16       *dout = (const u16 *)UNCACHED(dw_out[0]);
    const u32       *acc = (const u32 *)UNCACHED(dw_acc[0]);
    u16             *list = (u16 *)UNCACHED(dw_rec[0]);
    u16             ref[64];
    q_dlight        ql[3];
    int             fi, n = 0, k, i, j, na, nf = 0;
    u32             t0, t;

    for (k = 0; k < 3; ++k)
    {
        for (i = 0; i < 3; ++i)
            ql[k].pos[i] = lv.start[i] + FIX(at[k][i]);
        ql[k].radius = FIX(160 + 60 * k);
        ql[k].r = col[k][0];
        ql[k].g = col[k][1];
        ql[k].b = col[k][2];
    }
    dw_lights(ql, 3);
    /* the faces near the start (the list: all of them, whichever the lights may light) */
    for (fi = 0; fi < lv.nfaces && n < 2 * DW_REC; ++fi)
    {
        const q_face *f = &lv.faces[fi];

        if (!dw_fits(f) || f->flags & FF_WARP || iabs((f->origin[0] - lv.start[0]) >> 16) > 400
            || iabs((f->origin[1] - lv.start[1]) >> 16) > 400 || iabs((f->origin[2] - lv.start[2]) >> 16) > 400)
            continue;
        list[n++] = (u16)fi;
        nf += dw_filter(fi, dw_lt0, 3) != 0;
    }
    if (!n)
        return;
    memset((void *)UNCACHED(dw_out[0]), 0xEE, DW_OUTW * 4);
    dsp_wait();
    dsp_init_models();
    dw_params(list, n, 3, dw_out[0], dw_acc[0]);
    t0 = frt_read();
    dsp_walls_start();
    for (t = 0; t < 4000000 && dsp_busy(); ++t)
        ;
    wt_res[4] = frt_to_us((frt_read() - t0) & 0xFFFF);
    wt_res[5] = !dsp_busy();
    if (dsp_busy())
        dsp_init_models();                  /* (stuck: stopped) */
    na = imin((int)acc[0], DW_FACES);
    wt_res[0] = (u32)na;
    /* the faces listed (walls1.dsp's words: index << 16 | where its lit lights are), against
       dw_filter's picks in the list's order, each lit as dw_model does with dw_filter's lights */
    for (i = 0, k = 0; i < n && k < na; ++i)
    {
        int         f2 = list[i];
        unsigned    mk = dw_filter(f2, dw_lt0, 3);

        if (!mk)
            continue;
        if ((int)(acc[1 + k] >> 16) != f2)
        {
            ++wt_res[9];                    /* (walls0.dsp picked differently) */
            break;
        }
        {
            const q_face *f = &lv.faces[f2];
            int         np = (f->nu + 1) * (f->nv + 1), bad = 0;

            dout = (const u16 *)UNCACHED(dw_out[0]) + 2 * (acc[1 + k] & 0xFFFF);
            dw_model(f2, dw_lt, mk, ref);
            for (j = 0; j < np; ++j)
                if (dout[j] != ref[j] && !bad++ && !wt_res[2])
                {
                    wt_res[10] = (u32)f2;
                    wt_res[11] = (u32)j | mk << 8 | (u32)f->nu << 12 | (u32)f->nv << 16;
                    wt_res[12] = (u32)dout[j] << 16 | ref[j];
                    wt_res[13] = (u32)lv.lights[(f->firstlight & 0xFFFFFF) + (u32)j] << 16
                                 | ((const u16 *)UNCACHED(lv.lights_cart))[(f->firstlight & 0xFFFFFF) + (u32)j];
                }
            {
                /* (and against dl_face itself, in the world, as walls-ahead set it up: how near) */
                dl_light    L[3];
                u16         dlf[WHOLE_MAX];
                const u16   *raw = &lv.lights[f->firstlight & 0xFFFFFF];
                int         c, l;

                for (l = 0; l < 3; ++l)
                {
                    const s32 *o = &dw_lt[l * 8];

                    L[l].x = -o[0];
                    L[l].y = -o[1];
                    L[l].z = -o[2];
                    L[l].r2 = o[3];
                    L[l].inv = (1 << 24) / o[3];
                    L[l].r = (u8)o[5];
                    L[l].g = (u8)o[6];
                    L[l].b = (u8)o[7];
                }
                if (wl_face_setup(&ctx[0], f2, L, ql, 3))
                    dl_face(&ctx[0], dlf, raw, L);
                else
                    for (j = 0; j < np; ++j)
                        dlf[j] = (u16)(raw[j] | 0x8000);
                for (j = 0; j < np; ++j)
                {
                    wt_res[6] += ref[j] != (u16)(raw[j] | 0x8000);
                    wt_res[7] += ref[j] != dlf[j];
                    for (c = 0; c < 15; c += 5)
                        wt_res[8] = (u32)imax((int)wt_res[8], iabs((ref[j] >> c & 31) - (dlf[j] >> c & 31)));
                }
            }
            wt_res[1] += (u32)np;
            wt_res[2] += bad != 0;
            wt_res[3] += (u32)bad;
        }
        ++k;
    }
    wt_res[9] += (u32)(nf != na && na < DW_FACES);  /* (and if it listed a different number, room aside) */
    wt_res[14] = (u32)n;
    /* (again with the lights out of reach: walls0.dsp alone) */
    for (k = 0; k < 3; ++k)
        ql[k].pos[2] += FIX(3000);
    dw_lights(ql, 3);
    dsp_init_models();
    dw_params(list, n, 3, dw_out[0], dw_acc[0]);
    t0 = frt_read();
    dsp_walls_start();
    for (t = 0; t < 4000000 && dsp_busy(); ++t)
        ;
    wt_res[15] = frt_to_us((frt_read() - t0) & 0xFFFF);
    if (dsp_busy())
        dsp_init_models();
}
#endif
#endif

/* a new level (main.c, with the portals'): the buffers */
__attribute__((cold)) void r_wall_level(void)       /* (a level's start: built small) */
{
    u32             hw, lw, ca, view;
    int             k;

    wl_b[0] = wl_b[1] = NULL;
#if defined(DSP_WALLS) || defined(WALLS_TEST)
    /* the programs and their buffers on the cart, if there's room with the gun's kept drawing and
       as many slots as it would have anyway (after this: view_level_init; demo3's cart has room for
       one, with or without these) */
    dw_prog = NULL;
    level_free(&hw, &lw, &ca);
    view = VIEW_KEEP_BYTES + (ca >= 2 * VIEW_MAX_BYTES + VIEW_KEEP_BYTES ? 2 : 1) * VIEW_MAX_BYTES;
#ifdef WALLS_TEST
    memset(wt_res, 0, sizeof(wt_res));
    wt_res[16] = ca >> 10;
    wt_res[17] = lw >> 10;
#endif
    if (r_use_dsp && lw >= DW_HASH * 4 + 4096
        && ca >= 4096 + (DW_BLKW + 2 * DW_OUTW + 2 * DW_REC + 2 * (1 + 2 * DW_FACES) + 32 + 36) * 4 + 4096 + view)
    {
        dw_prog = cart_load("WALLS.BIN");
        dw_blocks = (u32 *)cart_alloc(DW_BLKW * 4);
        for (k = 0; k < 2; ++k)
        {
            dw_out[k] = (u32 *)cart_alloc(DW_OUTW * 4);
            dw_rec[k] = (u32 *)cart_alloc(DW_REC * 4);
            dw_acc[k] = (u32 *)cart_alloc((1 + 2 * DW_FACES) * 4);
        }
        dw_lt = (s32 *)cart_alloc(32 * 4);
        dw_lt0 = (s32 *)cart_alloc(36 * 4);
        dw_fb = ((u32)lv.faces_cart & 0x07FFFFFF) >> 2;
        dw_level();
#ifdef DSP_WALLS
        dw_hash = level_alloc_low(DW_HASH * 4);
        memset(dw_hash, 0, DW_HASH * 4);
        dw_stamp = 0;
#endif
    }
#ifdef DSP_WALLS
    else
        dw_hash = NULL;
    dw_ran = dw_on = false;
    dw_quiet = 255;
#endif
#ifdef WALLS_TEST
    if (dw_prog)
        walls_test();
#endif
#endif
#ifdef WALLS_AHEAD
    level_free(&hw, &lw, &ca);
    lw = lw > 4096 ? (lw - 4096) / 2 : 0;   /* (some kept) */
    if (lw < sizeof(wl_buf) - 2 * WL_WORDS + 2 * 256)
        return;                             /* (not room for a few faces) */
    wl_words = imin(WL_WORDS, (int)(lw - (sizeof(wl_buf) - 2 * WL_WORDS)) / 2);
    for (k = 0; k < 2; ++k)
    {
        wl_b[k] = level_alloc_low(sizeof(wl_buf) - 2 * (WL_WORDS - wl_words));
        wl_b[k]->n = 0;
        wl_b[k]->frame = 0;
    }
#else
    (void)hw; (void)lw; (void)ca; (void)k; (void)view;
#endif
}

static inline __attribute__((unused)) void wl_record(r_ctx *x, int fi)
{
    if (x->wl_nrec < (int)(sizeof(x->wl_rec) / 2))
        x->wl_rec[x->wl_nrec++] = (u16)fi;
}

static __attribute__((unused)) const u16 *wl_find(int fi)
{
    const wl_buf    *w = wl_b[frame & 1];
    int             k;

    if (!w || w->frame != frame)
        return NULL;
    for (k = 0; k < w->n; ++k)
        if (w->face[k] == fi)
            return w->lit[k];
    return NULL;
}

/* (the master, before it draws: its cache's copies of this frame's buffer forgotten) */
void                r_wall_forget(void)
{
    const wl_buf    *w = wl_b[frame & 1];
    u32             a;

    if (w)
        for (a = (u32)w & ~15u; a < (u32)w + sizeof(wl_buf); a += 16)
            *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
}

/* face fi's grid in the world (face_setup's, from the world's axes), into x's fa, ga and gk, and
   the lights of L (nl, from ls) in reach of it (face_dlights') into x->dmask; its points, 0 if
   none's in reach */
static int          wl_face_setup(r_ctx *x, int fi, const dl_light *L, const q_dlight *ls, int nl)
{
    const q_face    *f = &lv.faces[fi];
    const q_plane   *pl = &lv.planes[f->plane];
    const s32       *ax = &lv.axes[f->axes * 6];
    face_args       *a = &x->fa;
    s32             *gk = x->gk;
    int             nu = f->nu, nv = f->nv, N = lv.N, k, li;
    unsigned        m = 0;
    v3              du, dv, dut, dvt, e0, e1, f0, f1, across, down, corner[4], lo, hi;

    for (li = 0; li < nl; ++li)
    {
        s32 dist = fmul(ls[li].pos[0], pl->n[0]) + fmul(ls[li].pos[1], pl->n[1]) + fmul(ls[li].pos[2], pl->n[2]) - pl->dist;

        if (f->flags & FF_BACK)
            dist = -dist;
        if (dist > -FIX(8) && dist < ls[li].radius)
            m |= 1u << li;
    }
    if (!m)
        return 0;
    du.x = ax[0]; du.y = ax[1]; du.z = ax[2];
    dv.x = ax[3]; dv.y = ax[4]; dv.z = ax[5];
    dut.x = fmul(du.x, rcp_n); dut.y = fmul(du.y, rcp_n); dut.z = fmul(du.z, rcp_n);
    dvt.x = fmul(dv.x, rcp_n); dvt.y = fmul(dv.y, rcp_n); dvt.z = fmul(dv.z, rcp_n);
    k = (nu == 1 ? f->eu1 : N) - f->eu0;
    e0.x = dut.x * k; e0.y = dut.y * k; e0.z = dut.z * k;
    e1.x = dut.x * f->eu1; e1.y = dut.y * f->eu1; e1.z = dut.z * f->eu1;
    k = (nv == 1 ? f->ev1 : N) - f->ev0;
    f0.x = dvt.x * k; f0.y = dvt.y * k; f0.z = dvt.z * k;
    f1.x = dvt.x * f->ev1; f1.y = dvt.y * f->ev1; f1.z = dvt.z * f->ev1;
    x->ga.e0 = e0; x->ga.d = du; x->ga.e1 = e1;
    gk[GK_F0] = f0.x; gk[GK_F0 + 1] = f0.y; gk[GK_F0 + 2] = f0.z;
    gk[GK_F0 + 3] = dv.x; gk[GK_F0 + 4] = dv.y; gk[GK_F0 + 5] = dv.z;
    gk[GK_F0 + 6] = f1.x; gk[GK_F0 + 7] = f1.y; gk[GK_F0 + 8] = f1.z;
    a->fo.x = f->origin[0]; a->fo.y = f->origin[1]; a->fo.z = f->origin[2];
    a->fnu = nu;
    a->fnv = nv;
    /* the box of its corners: lights out of reach of all of it dropped */
    grid_step(&across, &e0, &du, &e1, nu, nu);
    grid_step(&down, &f0, &dv, &f1, nv, nv);
    corner[0] = a->fo;
    corner[1].x = a->fo.x + across.x; corner[1].y = a->fo.y + across.y; corner[1].z = a->fo.z + across.z;
    corner[2].x = a->fo.x + down.x; corner[2].y = a->fo.y + down.y; corner[2].z = a->fo.z + down.z;
    corner[3].x = corner[1].x + down.x; corner[3].y = corner[1].y + down.y; corner[3].z = corner[1].z + down.z;
    dl_box(corner, 4, &lo, &hi);
    for (li = 0; li < nl; ++li)
        if (m & 1u << li && dl_boxd2(L[li].x, L[li].y, L[li].z, &lo, &hi) >= L[li].r2)
            m &= ~(1u << li);
    x->dmask = (u8)m;
    return m ? (nu + 1) * (nv + 1) : 0;
}

/* (the slave, its drawing done) the next frame's buffer: the faces lit this frame (its own first:
   the nearest), with this frame's lights, until the next frame's first job comes */
void                r_wall_ahead(void)
{
    int             b = (frame + 1) & 1, nl = imin(r_ndlights, MAX_DLIGHTS), own, nm, i, n = 0;
    wl_buf          *w = wl_b[b];
    r_ctx           *x = &ctx[1];
    const r_ctx     *mc = (const r_ctx *)UNCACHED(&ctx[0]);
    dl_light        L[MAX_DLIGHTS];
    u16             *out;

    if (!w)
        return;
    w->n = 0;
    w->frame = (u16)(frame + 1);
    if (!nl)
        return;
    for (i = 0; i < nl; ++i)
    {
        const q_dlight  *l = &r_dlights[i];
        s32             rad = l->radius >> 16;

        L[i].x = l->pos[0];
        L[i].y = l->pos[1];
        L[i].z = l->pos[2];
        L[i].r2 = rad * rad;
        L[i].inv = (1 << 24) / imax(L[i].r2, 1);
        L[i].r = l->r;
        L[i].g = l->g;
        L[i].b = l->b;
    }
    own = x->wl_nrec;
    nm = mc->wl_nrec;
    out = w->buf;
    for (i = 0; i < own + nm && n < WL_N; ++i)
    {
        int fi = i < own ? x->wl_rec[i] : mc->wl_rec[i - own], np;

        if (FRT_FTCSR & 0x80)
        {
#ifdef FIGHT_BENCH
            ++wl_stop[0];
#endif
            break;                          /* (the next frame's first job: its signal's left for it) */
        }
        if (!(np = wl_face_setup(x, fi, L, r_dlights, nl)))
            continue;
        if (out + np > w->buf + wl_words)
        {
#ifdef FIGHT_BENCH
            ++wl_stop[1];
#endif
            break;
        }
        dl_face(x, out, &lv.lights[lv.faces[fi].firstlight & 0xFFFFFF], L);
        w->face[n] = (u16)fi;
        w->lit[n] = out;
        w->n = (u16)++n;                    /* (last: it's there) */
        out += (np + 1) & ~1;
    }
#ifdef FIGHT_BENCH
    wl_stop[2] += i == own + nm;
#endif
}

#ifdef DLF_CHECK
u32                 dlf_checks, dlf_diffs;  /* (OPT=-DDLF_CHECK: dl_face against dl_row, every lit face) */
#endif


#ifdef DL_CHECK
/* (the old way, each cell's corners, against rows j and j + 1 of the lit lights) */
static void         dl_check(const r_ctx *x, const u16 *lit, const u16 *raw, int stride, int j)
{
    int             i;
    v3              P[4];

    for (i = 0; i < x->fa.fnu; ++i)
    {
        cell_pos(x, i, j, P);
        dl_checks += 4;
        dl_diffs += dlight_add(x->dmask, &P[0], raw[i]) != lit[i];
        dl_diffs += dlight_add(x->dmask, &P[1], raw[i + 1]) != lit[i + 1];
        dl_diffs += dlight_add(x->dmask, &P[2], raw[stride + i + 1]) != lit[stride + i + 1];
        dl_diffs += dlight_add(x->dmask, &P[3], raw[stride + i]) != lit[stride + i];
    }
}
#endif

/* A whole tile too near the camera, or reaching too far off the screen, to
   draw in one piece: VDP1 goes over all of a polygon, on the screen or off
   (one such cell can cost more than the rest of the frame). Its quarters
   instead (tile t quartered is texture lv.quart0 + t, the four one under
   another), and those in halves and quarters by rows (SRCA steps of two), the
   pieces outside the view dropped. The cell's a parallelogram in view space:
   A, and U and V along its sides (u, v in texels, 0..N). */
typedef struct
{
    v3              A, U, V;
    u16             l[4];                   /* the corners' lights: A, B (u = N), C, D (v = N) */
    u32             srca, lut;              /* the quartered tile's, in 8s */
}                   split_cell;

/* a colour between two (RGB 5:5:5), t of n along: multiplies and a >> 16
   (a divide would be a library call, and it's 36 of these a piece) */
static u16          col_lerp(u16 a, u16 b, int t, int n)
{
    s32             w = t * rcp[n];         /* 16.16 */
    int             ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int             r = ar + (((s32)((b & 31) - ar) * w) >> 16);
    int             g = ag + (((s32)(((b >> 5) & 31) - ag) * w) >> 16);
    int             bl = ab + (((s32)(((b >> 10) & 31) - ab) * w) >> 16);

    return (u16)(0x8000 | bl << 10 | g << 5 | r);
}

static void         split_piece(r_ctx *x, const split_cell *sc, int ua, int ub, int va, int vb)
{
    int             N = lv.N, h = N / 2, k, qv = va >= h, ty0, th;
    int             us[4] = { ua, ub, ub, ua }, vs[4] = { va, va, vb, vb };
    v3              q[4];
    u8              oa = 0xFF, oo = 0;
    u32             xy[4], *d, link;
    u16             l[4], grda;
    s32             vram;

    for (k = 0; k < 4; ++k)
    {
        s32 fu = us[k] * rcp[N], fv = vs[k] * rcp[N];
        u8  oc;

        q[k].x = sc->A.x + fmul(sc->U.x, fu) + fmul(sc->V.x, fv);
        q[k].y = sc->A.y + fmul(sc->U.y, fu) + fmul(sc->V.y, fv);
        q[k].z = sc->A.z + fmul(sc->U.z, fu) + fmul(sc->V.z, fv);
        oc = view_oc(q[k].x, q[k].y, q[k].z);
        oa &= oc;
        oo |= oc;
    }
    if (oa)
        return;                             /* outside one plane: none of it's seen */
    if (!(oo & OC_NEAR))
    {
        for (k = 0; k < 4; ++k)
        {
            xy[k] = project(x, q[k].x, q[k].y, q[k].z);
            if (iabs(XY_X(xy[k]) - CX) > GUARD_X || iabs(XY_Y(xy[k]) - CY) > GUARD_Y)
                oo |= OC_FAR;
        }
    }
    if (oo & (OC_NEAR | OC_FAR) && vb - va > 2)
    {
        /* still too near or too big: in two by rows */
        int m = (va + vb) >> 1;

        split_piece(x, sc, ua, ub, va, m);
        split_piece(x, sc, ua, ub, m, vb);
        return;
    }
    ty0 = va - qv * h;
    th = vb - va;
    if (oo & OC_NEAR)
    {
        if (!near_clip(q, &ty0, &th, false, 16 / h))           /* rows in 8 bytes */
            return;
        for (k = 0; k < 4; ++k)
            xy[k] = project(x, q[k].x, q[k].y, q[k].z);
    }
    /* its lights: between the cell's corners' */
    for (k = 0; k < 4; ++k)
    {
        u16 top = col_lerp(sc->l[0], sc->l[1], us[k], N), bot = col_lerp(sc->l[3], sc->l[2], us[k], N);

        l[k] = col_lerp(top, bot, vs[k], N);
        if (x->dmask)
        {
            l[k] = dlight_add(x->dmask, &q[k], l[k]);
        }
    }
    vram = tex_vram(x, lv.quart0 + (int)sc->lut);
    if (vram < 0 || !(d = cmd_alloc(x, &link)))
        return;
    PROF(++x->st.pieces);
    d[0] = 0x10020000u | link;
    d[1] = x->cpmod | ((lut_vram + sc->lut * 32) >> 3);
    /* quarter (qv, qu), from its row ty0: h / 2 bytes a row */
    d[2] = (((u32)vram >> 3) + (u32)((qv * 2 + (ua >= h)) * h + ty0) * (u32)h / 16) << 16
         | (u32)(((h >> 3) << 8) | th);
    d[3] = xy[0];
    d[4] = xy[1];
    d[5] = xy[2];
    d[6] = xy[3];
    grda = vdp_gouraud_fast(x->w, (u32)l[0] << 16 | l[1], (u32)l[2] << 16 | l[3]);
    d[7] = (u32)grda << 16;
}

static __attribute__((noinline)) void cell_split(r_ctx *x, const q_cell_fast *cell, const v3 *P, const u16 *light,
                                                 int stride)
{
    split_cell      sc;
    int             h = lv.N / 2;

    sc.A = P[0];
    sc.U.x = P[1].x - P[0].x; sc.U.y = P[1].y - P[0].y; sc.U.z = P[1].z - P[0].z;
    sc.V.x = P[3].x - P[0].x; sc.V.y = P[3].y - P[0].y; sc.V.z = P[3].z - P[0].z;
    sc.l[0] = light[0];
    sc.l[1] = light[1];
    sc.l[2] = light[stride + 1];
    sc.l[3] = light[stride];
    sc.lut = cell->lut;
    split_piece(x, &sc, 0, h, 0, h);
    split_piece(x, &sc, h, 2 * h, 0, h);
    split_piece(x, &sc, 0, h, h, 2 * h);
    split_piece(x, &sc, h, 2 * h, h, 2 * h);
}

/* A cropped cell, or one crossing the near plane: its corners on screen (and
   maybe fewer rows of its texture). g: the grid vertices round it, which are
   cl..cr, ct..cb of its tile (in stored texels: the face's edge columns and
   rows are narrower); exact: the cell's all of that. false: not drawn */
static __attribute__((noinline)) bool cell_corners(r_ctx *x, const q_cell *cell, gv **g, v3 *P, int i, int j, u32 *xy,
                                                   int *ty0, int *th, int cl, int cr, int ct, int cb, bool exact)
{
    v3              q[4];
    int             k;
    s32             u0 = cell->u0 - cl, u1 = cell->u1 - cl, v0 = cell->v0 - ct, v1 = cell->v1 - ct;

    if (!((g[0]->oc | g[1]->oc | g[2]->oc | g[3]->oc) & OC_NEAR))
    {
        /* all in front: small on screen, interpolate the crop's corners */
        u32 p0 = gv_xy(x, g[0]), p1 = gv_xy(x, g[1]), p2 = gv_xy(x, g[2]), p3 = gv_xy(x, g[3]);
        s32 x0 = XY_X(p0), y0 = XY_Y(p0), x1 = XY_X(p1), y1 = XY_Y(p1);
        s32 x2 = XY_X(p2), y2 = XY_Y(p2), x3 = XY_X(p3), y3 = XY_Y(p3);

        if (iabs(x2 - x0) < INTERP_PX && iabs(x1 - x3) < INTERP_PX && iabs(y2 - y0) < INTERP_PX && iabs(y1 - y3) < INTERP_PX)
        {
            /* along the top and bottom edges to the crop's columns, then down them to its rows:
               fractions in 16.16 (times a reciprocal) and >> 16 (the SH-2 has no barrel shifter:
               other signed shifts are library calls); the grid cell's own edges come out exact */
            s32 w = cr - cl, h = cb - ct;
            s32 fa0 = u0 * rcp[w], fa1 = cell->u1 == cr ? 65536 : u1 * rcp[w];
            s32 fb0 = v0 * rcp[h], fb1 = cell->v1 == cb ? 65536 : v1 * rcp[h];
            s32 tx0 = x0 + (((x1 - x0) * fa0) >> 16), ty0_ = y0 + (((y1 - y0) * fa0) >> 16);
            s32 tx1 = x0 + (((x1 - x0) * fa1) >> 16), ty1 = y0 + (((y1 - y0) * fa1) >> 16);
            s32 bx0 = x3 + (((x2 - x3) * fa0) >> 16), by0 = y3 + (((y2 - y3) * fa0) >> 16);
            s32 bx1 = x3 + (((x2 - x3) * fa1) >> 16), by1 = y3 + (((y2 - y3) * fa1) >> 16);

            if (r_debug == 1)
                return false;
            PROF(++x->st.ns_small);
#define CORNER(n, TX, TY, BX, BY, F) \
            xy[n] = (u32)((TX) + ((((BX) - (TX)) * (F)) >> 16)) << 16 | (u16)((TY) + ((((BY) - (TY)) * (F)) >> 16));
            CORNER(0, tx0, ty0_, bx0, by0, fb0)
            CORNER(1, tx1, ty1, bx1, by1, fb0)
            CORNER(2, tx1, ty1, bx1, by1, fb1)
            CORNER(3, tx0, ty0_, bx0, by0, fb1)
#undef CORNER
            return true;
        }
    }
    /* big or near: in view space (the cell as a whole isn't outside the view: VDP1 clips the rest) */
    cell_pos(x, i, j, P);
    if (exact)
        for (k = 0; k < 4; ++k)
        {
            q[k] = P[k];
        }
    else
    {
        const v3 *du = &x->fa.dut, *dv = &x->fa.dvt;
        const v3 *o = &P[0];

        q[0].x = o->x + du->x * u0 + dv->x * v0; q[0].y = o->y + du->y * u0 + dv->y * v0; q[0].z = o->z + du->z * u0 + dv->z * v0;
        q[1].x = o->x + du->x * u1 + dv->x * v0; q[1].y = o->y + du->y * u1 + dv->y * v0; q[1].z = o->z + du->z * u1 + dv->z * v0;
        q[2].x = o->x + du->x * u1 + dv->x * v1; q[2].y = o->y + du->y * u1 + dv->y * v1; q[2].z = o->z + du->z * u1 + dv->z * v1;
        q[3].x = o->x + du->x * u0 + dv->x * v1; q[3].y = o->y + du->y * u0 + dv->y * v1; q[3].z = o->z + du->z * u0 + dv->z * v1;
    }
    if (r_debug == 1)
        return false;
    if (q[0].z < NEAR_Z || q[1].z < NEAR_Z || q[2].z < NEAR_Z || q[3].z < NEAR_Z)
    {
        const q_tex *tx = &lv.textures[cell->tex & CELL_TEX];

        PROF(++x->st.near);
        if (!near_clip(q, ty0, th, (tx->lut & TEX_TRANSPOSED) != 0, tx->w < 16 ? 2 : 1))
            return false;
    }
    for (k = 0; k < 4; ++k)
        xy[k] = project(x, q[k].x, q[k].y, q[k].z);
    return true;
}

/* a grid point: view space, which planes it's outside (x = +-tx, y = +-ty at
   its depth), and if it's in front of the near plane, where it is on screen.
   The divide's started first: the SH-2's divider works on its own, so its 39
   cycles pass while the stores go out. (Scalars, not a pointer to the running
   point: its address taken would keep it on the stack.) */
static inline void  grid_point(gv *g, s32 px, s32 py, s32 pz, s32 tx, s32 ty)
{
    u8              oc = 0;
    bool            front = pz >= NEAR_Z;

    if (front)
        divu_start(FOCAL, 0, pz);
    else
        oc = OC_NEAR;
    if (px < -tx)
        oc |= OC_LEFT;
    if (px > tx)
        oc |= OC_RIGHT;
    if (py > ty)
        oc |= OC_TOP;
    if (py < -ty)
        oc |= OC_BOTTOM;
    if (front)
    {
        s32 r = divu_result(), sx = mul_hi(px, r), sy = -mul_hi(py, r);

        if (oc && (sx > GUARD_X || sx < -GUARD_X || sy > GUARD_Y || sy < -GUARD_Y))
            oc |= OC_FAR;
        g->xy = screen_xy(px, py, r);
    }
    g->ocd = (u16)(oc << 8 | front);        /* oc, and done if projected: one store (big-endian) */
}

/* a grid row of n points: view space by additions from its first point, and
   which planes each is outside - the frustum's limits (x = +-kx z, y = +-ky z)
   are linear along the row too, so no multiplies either. The first and last
   steps are the face's edge columns (e0, e1: short if the face ends inside
   the tile); for n = 2, e0 is the one step. */
static __attribute__((noinline, cold)) void grid_row(gv *row, int n, const v3 *p, const v3 *du, const v3 *e0, const v3 *e1, s32 dtx, s32 dty)
{
    s32             px = p->x, py = p->y, pz = p->z, dx = du->x, dy = du->y, dz = du->z, tx, ty;
    int             i;

    grid_point(row, px, py, pz, fmul(pz, kx), fmul(pz, ky));
    px += e0->x; py += e0->y; pz += e0->z;
    tx = fmul(pz, kx);
    ty = fmul(pz, ky);
    if (n == 2)
    {
        grid_point(row + 1, px, py, pz, tx, ty);
        return;
    }
    for (i = n - 3, ++row; i > 0; --i, ++row)
    {
        grid_point(row, px, py, pz, tx, ty);
        px += dx; py += dy; pz += dz;
        tx += dtx;
        ty += dty;
    }
    grid_point(row, px, py, pz, tx, ty);
    px += e1->x; py += e1->y; pz += e1->z;
    grid_point(row + 1, px, py, pz, fmul(pz, kx), fmul(pz, ky));
}

/* src/cells.s: what it needs for this face */
static void         cells_face(r_ctx *x, int stride, const q_cell *cell0)
{
    cell_args       *a = &x->ca;

    a->cell0 = cell0;
    a->stride2 = (u32)stride * 2;
}

/* face.s: what's the same all frame */
static void         face_frame(r_ctx *x)
{
    face_args       *a = &x->fa;
    int             k;

    for (k = 0; k < 3; ++k)
    {
        a->rt[k] = cam.right[k];
        a->up[k] = cam.up[k];
        a->fw[k] = cam.fwd[k];
        a->pos[k] = cam.pos[k];
    }
    a->ky = ky;
    a->lod_z = r_lod_z;
    a->rcp_n = rcp_n;
    a->frame = frame;
    a->mover_ofs = &mover_ofs[0][0];
    a->axis_view = axis_view;
    a->axes = lv.axes;
    a->faces = lv.faces;
    a->cells0 = lv.cells;
    a->lodcells = lv.lodcells;
    a->lights0 = lv.lights;
    a->lodlights = lv.lodlights;
    a->lodfaces = lv.lodfaces;
    a->N = lv.N;
    a->grid = x->grid;
    a->ga = &x->ga;
    a->gk = x->gk;
}

/* ...and what's the same all frame (once x->fifo's set) */
static void         cells_frame(r_ctx *x)
{
    cell_args       *a = &x->ca;

    a->tex_slot = x->tex_slot;
    a->slot_frame = slot_frame;
    a->frame = frame;
    a->sb8 = slot_bytes >> 3;
    a->base8 = slot_vram >> 3;
    a->lut8 = lut_vram >> 3;
    a->pmod = x->cpmod = (u32)CELL_PMOD << 16;
    a->ctrl = 0x10020000u;                  /* jump assign, distorted sprite */
    a->fifo = x->fifo;
    a->w = x->w;
    a->head = &x->w->head[x->bucket];
    a->tail = &x->w->tail[x->bucket];
    a->fast = CELL_FULL | CELL_EXACT;
    a->near24 = (u32)(OC_NEAR | OC_FAR) << 24;
    a->N = (u32)lv.N;
    a->rcp = rcp;
    a->slot_lut = slot_lut;
    a->slot_w = slot_w;
}

/* rows of cells in assembly (a whole face's, or one), the writer's list
   brought up to date by it: returns how many it left for the C (their
   numbers, from a->cell0, in x->ca.def) */
static int          cells_run(r_ctx *x, const gv *top, const gv *bot, const q_cell *cell, const u16 *light, int n,
                              int rows)
{
    cell_args       *a = &x->ca;

    a->n = (u32)n;
    a->rows = (u32)rows;
    a->defp = a->def;
    a->top = top;
    a->bot = bot;
    a->cell = cell;
    a->light = light;
#ifdef R_PROFILE
    {
        u32 pc = frt_read();
        int c0 = x->w->count;

        cells_asm(a);
        x->st.p_casm += (frt_read() - pc) & 0xFFFF;
        ++x->st.n_calls;
        x->st.n_casm += x->w->count - c0;
    }
#else
    cells_asm(a);
#endif
    return (int)(a->defp - a->def);
}

/* The assembly row against the C one (the reference): rows across the view,
   off its edges, and into the near plane. Any difference and the C one's used. */
int                 grid_bad;

static __attribute__((cold)) void grid_selftest(void)
{
    static const s16 rows[][6] = {          /* the first point, the step (world units) */
        { -301, -203, 401, 61, 31, 5 },
        { -51, 23, 30, -9, 3, -4 },         /* into the near plane */
        { 903, -907, 101, -151, 149, 3 },   /* far off the screen: clamped */
        { -17, 13, 250, 3, -2, -30 },
        { 37, -405, 60, 1, 97, -7 },        /* below and above */
    };
    gv              *c = ctx[0].grid, *as = ctx[0].grid + MAX_ROW;
    grid_args       *a = &ctx[0].ga;
    int             r, i, n = 9;

    grid_bad = 0;
    for (r = 0; r < (int)(sizeof(rows) / sizeof(rows[0])); ++r)
    {
        a->p.x = FIX(rows[r][0]); a->p.y = FIX(rows[r][1]); a->p.z = FIX(rows[r][2]);
        a->d.x = FIX(rows[r][3]); a->d.y = FIX(rows[r][4]); a->d.z = FIX(rows[r][5]);
        a->e0.x = a->d.x >> 1; a->e0.y = a->d.y >> 1; a->e0.z = a->d.z >> 1;
        a->e1.x = a->d.x - (a->d.x >> 2); a->e1.y = a->d.y - (a->d.y >> 2); a->e1.z = a->d.z - (a->d.z >> 2);
        for (n = 2; n <= 9; n += 7)
        {
            grid_row(c, n, &a->p, &a->d, &a->e0, &a->e1, fmul(a->d.z, kx), fmul(a->d.z, ky));
            grid_row_asm(as, n, a, grid_k);
            for (i = 0; i < n; ++i)
                if (c[i].oc != as[i].oc || (!(c[i].oc & OC_NEAR) && c[i].xy != as[i].xy))
                    ++grid_bad;
        }
    }
    /* and a whole face (3 x 3 cells, 4 x 4 points), against the C a row at a time */
    {
        v3  f0 = { FIX(3), FIX(-7), FIX(-2) }, dv = { FIX(5), FIX(-11), FIX(-3) }, f1 = { FIX(2), FIX(-4), FIX(-1) };
        v3  rp = { FIX(-120), FIX(90), FIX(260) };
        int j, np = 4, nr = 4;
        s32 *gk = ctx[0].gk;

        a->p = rp;
        a->d.x = FIX(23); a->d.y = FIX(2); a->d.z = FIX(4);
        a->e0.x = FIX(9); a->e0.y = FIX(1); a->e0.z = FIX(1);
        a->e1.x = FIX(17); a->e1.y = FIX(1); a->e1.z = FIX(3);
        for (j = 0; j < nr; ++j)
        {
            const v3 *st = j == 0 ? &f0 : j == nr - 2 ? &f1 : &dv;

            grid_row(c + j * np, np, &rp, &a->d, &a->e0, &a->e1, fmul(a->d.z, kx), fmul(a->d.z, ky));
            rp.x += st->x; rp.y += st->y; rp.z += st->z;
        }
        gk[GK_ROWS] = nr - 1;
        gk[GK_F0] = f0.x; gk[GK_F0 + 1] = f0.y; gk[GK_F0 + 2] = f0.z;
        gk[GK_F0 + 3] = dv.x; gk[GK_F0 + 4] = dv.y; gk[GK_F0 + 5] = dv.z;
        gk[GK_F0 + 6] = f1.x; gk[GK_F0 + 7] = f1.y; gk[GK_F0 + 8] = f1.z;
        grid_face_asm(as, np, a, gk);
        for (i = 0; i < np * nr; ++i)
            if (c[i].oc != as[i].oc || (!(c[i].oc & OC_NEAR) && c[i].xy != as[i].xy))
                ++grid_bad;
    }
    r_grid_asm = grid_bad == 0;
}

/* One cell, the C way: what the assembly leaves (near the camera, a big
   crop, its texture to load), and every cell when the assembly's off. top, bot: its grid rows;
   light: the top row's lights; cl..cr, ct..cb: its grid cell in its tile. */
static __attribute__((noinline)) void cell_c(r_ctx *x, const q_cell *cell, gv *top, gv *bot, const u16 *light,
                                             int stride, int i, int j, int cl, int cr, int ct, int cb)
{
    u32             xy[4];
    int             ty0, th, tex = cell->tex, N = lv.N;
    u32             size_full = (u32)(((N >> 3) << 8) | N);
    gv              *g[4];
    v3              P[4];                   /* its corners in view space, when they're needed */
    u8              oa, oo;
    bool            exact;

    if (tex == CELL_EMPTY)
        return;
    g[0] = &top[i]; g[1] = &top[i + 1]; g[2] = &bot[i + 1]; g[3] = &bot[i];
    oa = g[0]->oc & g[1]->oc & g[2]->oc & g[3]->oc & ~OC_FAR;
    if (oa)
    {
        PROF(++x->st.culled);       /* the whole cell's outside a plane: so is any crop of it */
        return;
    }
    oo = g[0]->oc | g[1]->oc | g[2]->oc | g[3]->oc;
    /* (where its corners are in view space: only what needs them works them out) */
    if (tex & CELL_FULL && oo & (OC_NEAR | OC_FAR) && !r_nosplit)
    {
        /* a whole tile too near, or too big on the screen: in pieces */
        cell_pos(x, i, j, P);
        cell_split(x, (const q_cell_fast *)cell, P, (x->lit ? x->raw_light + j * stride : light) + i, stride);
        return;
    }
    if (tex & CELL_FULL && !(oo & (OC_NEAR | OC_FAR)) && (!x->dmask || x->lit))
    {
        /* the common case, all here: a whole tile, in front, no dynamic light */
        int         t = tex & CELL_TEX, s = x->tex_slot[t];
        s32         vram;
        u32         *dw, link;
        const u16   *l = light + i;

        if (r_debug == 1)
            return;
        if (s == 0xFFFF)
        {
            if ((vram = tex_load(x, t)) < 0)
                return;
        }
        else
        {
            slot_frame[s] = frame;
            vram = (s32)(slot_vram + (u32)s * slot_bytes);
        }
        if (!(dw = cmd_alloc(x, &link)))
            return;
        PROF(++x->st.nfast);
        dw[0] = 0x10020000u | link;
        dw[1] = x->cpmod | ((lut_vram + (u32)((const q_cell_fast *)cell)->lut * 32) >> 3);
        dw[2] = ((u32)vram >> 3) << 16 | size_full;
        dw[3] = gv_xy(x, g[0]);
        dw[4] = gv_xy(x, g[1]);
        dw[5] = gv_xy(x, g[2]);
        dw[6] = gv_xy(x, g[3]);
        dw[7] = (u32)vdp_gouraud_fast(x->w, (u32)l[0] << 16 | l[1], (u32)l[stride + 1] << 16 | l[stride]) << 16;
        return;
    }
    PROF(++x->st.nslow);
    PROF(x->st.ns_dl += (tex & CELL_FULL) != 0);    /* (whole tiles here only for a dynamic light) */
    PROF(x->st.ns_crop += !(tex & CELL_FULL));
    PROF(x->st.ns_exact += (tex & (CELL_FULL | CELL_EXACT)) == CELL_EXACT);
    if (tex & (CELL_FULL | CELL_EXACT))
    {
        ty0 = ((const q_cell_fast *)cell)->ty0;
        th = ((const q_cell_fast *)cell)->th;
    }
    else
    {
        ty0 = cell->ty0;
        th = cell->th;
    }
    /* exact: the crop's the grid cell (the face's edge is the grid's; the baker says) */
    exact = (tex & (CELL_FULL | CELL_EXACT)) != 0;
    if (exact && !(oo & OC_NEAR))
    {
        xy[0] = gv_xy(x, g[0]);
        xy[1] = gv_xy(x, g[1]);
        xy[2] = gv_xy(x, g[2]);
        xy[3] = gv_xy(x, g[3]);
    }
    else
    {
#ifdef R_PROFILE
        u32 ps = frt_read();
        bool ok = cell_corners(x, cell, g, P, i, j, xy, &ty0, &th, cl, cr, ct, cb, exact);

        x->st.p_corners += (frt_read() - ps) & 0xFFFF;
        if (!ok)
            return;
#else
        if (!cell_corners(x, cell, g, P, i, j, xy, &ty0, &th, cl, cr, ct, cb, exact))
            return;
#endif
    }
#ifdef R_PROFILE
    {
        u32 ps = frt_read();

        cell_emit(x, cell, ty0, th, xy, light + i, stride, i, j);
        x->st.p_slow += (frt_read() - ps) & 0xFFFF;
    }
#else
    cell_emit(x, cell, ty0, th, xy, light + i, stride, i, j);
#endif
}


/* A face's grid and its cells. First its setup (face_asm in src/face.s;
   face_setup here, the same bit for bit, for OPT=-DNO_FACE_ASM and
   -DFACE_CHECK): which grid (the face's, or its coarse one when it's far),
   whether it's in view, its steps (x->fa, x->ga, x->gk) and, if it fits,
   the whole grid at once; then its cells (face_cells: for a big face, its
   grid and cells a row at a time). */

/* the dynamic lights close enough to its plane (and in front of it) */
static void         face_dlights(r_ctx *x, const q_face *f, int model)
{
    int             i;
    unsigned        m = 0;

    for (i = 0; i < ndl; ++i)
    {
        const q_plane   *pl = &lv.planes[f->plane];
        const s32       *lp = lsrc[i].pos, *o = mover_ofs[model];
        s32             dist = fmul(lp[0] - o[0], pl->n[0]) + fmul(lp[1] - o[1], pl->n[1])
                             + fmul(lp[2] - o[2], pl->n[2]) - pl->dist;

        if (f->flags & FF_BACK)
            dist = -dist;
        if (dist > -FIX(8) && dist < lsrc[i].radius)
            m |= 1u << i;
    }
#ifndef NO_DL_REACH
    if (m)
    {
        /* near its plane, but near the face? The box of its grid's corners (dl_face's), each
           light out of reach of all of it dropped: most faces the plane test passes (a floor,
           a light a little above it) are too far along it, and would take the lit way for
           nothing */
        const face_args *a = &x->fa;
        const v3        *fs = (const v3 *)&x->gk[GK_F0];
        v3              across, down, corner[4], lo, hi;

        grid_step(&across, &x->ga.e0, &x->ga.d, &x->ga.e1, a->fnu, a->fnu);
        grid_step(&down, &fs[0], &fs[1], &fs[2], a->fnv, a->fnv);
        corner[0] = a->fo;
        corner[1].x = a->fo.x + across.x; corner[1].y = a->fo.y + across.y; corner[1].z = a->fo.z + across.z;
        corner[2].x = a->fo.x + down.x; corner[2].y = a->fo.y + down.y; corner[2].z = a->fo.z + down.z;
        corner[3].x = corner[1].x + down.x; corner[3].y = corner[1].y + down.y; corner[3].z = corner[1].z + down.z;
        dl_box(corner, 4, &lo, &hi);
        for (i = 0; i < ndl; ++i)
            if (m & 1u << i && dl_boxd2(dl[i].x, dl[i].y, dl[i].z, &lo, &hi) >= dl[i].r2)
                m &= ~(1u << i);
    }
#endif
    x->dmask = (u8)m;
}

#if defined(NO_FACE_ASM) || defined(FACE_CHECK)
/* which of the sides of the face's cluster's rectangle (portal_face's slopes) a point's outside */
static inline u8    rect_oc(const face_args *a, s32 x, s32 y, s32 z)
{
    return (u8)((x < fmul(z, a->prx0)) | (x > fmul(z, a->prx1)) << 1 | (y > fmul(z, a->pry1)) << 2
                | (y < fmul(z, a->pry0)) << 3);
}

/* 0: nothing to draw (the sky, say); 1: out of view; 2: its steps and the whole grid
   done; 3: its steps done, the grid too big to do at once (or the assembly grid's off) */
static __attribute__((noinline)) int face_setup(r_ctx *x, const q_face *f, int model)
{
    face_args       *a = &x->fa;
    v3              o, du, dv, e0, e1, f0, f1;
    s32             d[3], zmin, *gk = x->gk;
    const u16       *light;
    int             nu = f->nu, nv = f->nv, eu0 = f->eu0, eu1 = f->eu1, ev0 = f->ev0, ev1 = f->ev1, k, N = lv.N;
    const q_cell    *row_cells;

    if (f->flags & (FF_SKY | FF_NODRAW) || nu + 1 > MAX_ROW)
        return 0;
    d[0] = f->origin[0] + mover_ofs[model][0] - cam.pos[0];
    d[1] = f->origin[1] + mover_ofs[model][1] - cam.pos[1];
    d[2] = f->origin[2] + mover_ofs[model][2] - cam.pos[2];
    to_view(d, &o);
    {
        /* the grid's axes in view space, once a frame for each of the level's pairs (both
           CPUs share them: a vector and its frame fill a cache line, so a line that says
           this frame is this frame's) */
        s32 *c = axis_view + f->axes * 8;

        if (c[3] == (s32)frame && c[7] == (s32)frame)
        {
            du.x = c[0]; du.y = c[1]; du.z = c[2];
            dv.x = c[4]; dv.y = c[5]; dv.z = c[6];
        }
        else
        {
            to_view(&lv.axes[f->axes * 6], &du);
            to_view(&lv.axes[f->axes * 6 + 3], &dv);
            c[0] = du.x; c[1] = du.y; c[2] = du.z; c[3] = (s32)frame;
            c[4] = dv.x; c[5] = dv.y; c[6] = dv.z; c[7] = (s32)frame;
        }
    }
    /* all of it outside one of the view's planes? Its grid's a parallelogram (o, and nu
       steps along du and nv along dv: a little more than the face), so four corners
       answer it, and a face out here needn't have its grid worked out at all */
    {
        s32 ux = du.x * nu, uy = du.y * nu, uz = du.z * nu, vx = dv.x * nv, vy = dv.y * nv, vz = dv.z * nv;

        if (view_oc(o.x, o.y, o.z) & view_oc(o.x + ux, o.y + uy, o.z + uz) & view_oc(o.x + vx, o.y + vy, o.z + vz)
            & view_oc(o.x + ux + vx, o.y + uy + vy, o.z + uz + vz))
            return 1;
        if (a->prect && (rect_oc(a, o.x, o.y, o.z) & rect_oc(a, o.x + ux, o.y + uy, o.z + uz)
                         & rect_oc(a, o.x + ux + vx, o.y + uy + vy, o.z + uz + vz) & rect_oc(a, o.x + vx, o.y + vy, o.z + vz)))
            return 1;                       /* (outside its cluster's rectangle: the portals') */
        zmin = imin(imin(o.z, o.z + uz), imin(o.z + vz, o.z + uz + vz));
        PROF(x->st.cells_all += nu * nv);
    }
    /* far: its coarse grid, if it has one (a quarter of the cells, the textures at half the
       resolution: as mipmapping would, and less shimmer) */
    row_cells = &lv.cells[f->firstcell];
    light = &lv.lights[f->firstlight & 0xFFFFFF];
    if (f->flags & FF_LOD && zmin > r_lod_z)
    {
        const q_lodface *lf = &lv.lodfaces[f->lodhi << 8 | f->firstlight >> 24];
        s32             ou = (s32)lf->offu * (65536 / 32), ov = (s32)lf->offv * (65536 / 32);  /* in the face's cells */

        o.x -= fmul(du.x, ou) + fmul(dv.x, ov);
        o.y -= fmul(du.y, ou) + fmul(dv.y, ov);
        o.z -= fmul(du.z, ou) + fmul(dv.z, ov);
        du.x += du.x; du.y += du.y; du.z += du.z;
        dv.x += dv.x; dv.y += dv.y; dv.z += dv.z;
        nu = lf->nu;
        nv = lf->nv;
        eu0 = lf->eu0; eu1 = lf->eu1; ev0 = lf->ev0; ev1 = lf->ev1;
        row_cells = &lv.lodcells[lf->firstcell];
        light = &lv.lodlights[lf->firstlight];
        PROF(x->st.cells_384 += nu * nv);
    }
    /* one stored texel along each axis: times 1/N (a variable shift would be a library call) */
    a->dut.x = fmul(du.x, rcp_n); a->dut.y = fmul(du.y, rcp_n); a->dut.z = fmul(du.z, rcp_n);
    a->dvt.x = fmul(dv.x, rcp_n); a->dvt.y = fmul(dv.y, rcp_n); a->dvt.z = fmul(dv.z, rcp_n);
    /* the steps into and out of the face's edge columns and rows (a whole tile's if it isn't cropped) */
    k = (nu == 1 ? eu1 : N) - eu0;
    e0.x = a->dut.x * k; e0.y = a->dut.y * k; e0.z = a->dut.z * k;
    e1.x = a->dut.x * eu1; e1.y = a->dut.y * eu1; e1.z = a->dut.z * eu1;
    k = (nv == 1 ? ev1 : N) - ev0;
    f0.x = a->dvt.x * k; f0.y = a->dvt.y * k; f0.z = a->dvt.z * k;
    f1.x = a->dvt.x * ev1; f1.y = a->dvt.y * ev1; f1.z = a->dvt.z * ev1;
    /* the grid's steps, where the assembly (and grid_pos) find them */
    x->ga.p = o; x->ga.e0 = e0; x->ga.d = du; x->ga.e1 = e1;
    gk[GK_F0] = f0.x; gk[GK_F0 + 1] = f0.y; gk[GK_F0 + 2] = f0.z;
    gk[GK_F0 + 3] = dv.x; gk[GK_F0 + 4] = dv.y; gk[GK_F0 + 5] = dv.z;
    gk[GK_F0 + 6] = f1.x; gk[GK_F0 + 7] = f1.y; gk[GK_F0 + 8] = f1.z;
    a->fo = o;
    a->fnu = nu;
    a->fnv = nv;
    a->eu0 = eu0; a->eu1 = eu1; a->ev0 = ev0; a->ev1 = ev1;
    a->cells = row_cells;
    a->light = light;
    /* the grid: the whole face at once if it fits (the assembly), or a row at a time */
    if (!r_grid_asm || (nu + 1) * (nv + 1) > WHOLE_MAX)
        return 3;
    gk[GK_ROWS] = nv;
    grid_face_asm(x->grid, nu + 1, &x->ga, gk);
    return 2;
}

#endif

#ifdef R_PROFILE
/* (profile: Gouraud tables the same as the one before, and flat ones) */
static void         prof_gouraud(r_ctx *x, int gc0)
{
    const u32       *gs = x->w->gst;
    int             k;

    for (k = gc0 + 1; k < x->w->gcount; ++k)
    {
        x->st.g_same += gs[k * 2] == gs[k * 2 - 2] && gs[k * 2 + 1] == gs[k * 2 - 1];
        x->st.g_flat += gs[k * 2] == gs[k * 2 + 1] && (gs[k * 2] >> 16) == (gs[k * 2] & 0xFFFF);
    }
}
#endif

/* a Gouraud colour, each channel r steps brighter (or darker) */
static u16          light_add(u16 l, int r)
{
    int             cr = iclamp((l & 31) + r, 0, 31), cg = iclamp((l >> 5 & 31) + r, 0, 31);
    int             cb = iclamp((l >> 10 & 31) + r, 0, 31);

    return (u16)(0x8000 | cb << 10 | cg << 5 | cr);
}

/* the nearest z of the face's grid: its first point's, and the far edges' where they come nearer */
static s32          face_zmin(const r_ctx *x)
{
    const face_args *a = &x->fa;
    const s32       *gk = x->gk;
    s32             uz = x->ga.e0.z, vz = gk[GK_F0 + 2];

    if (a->fnu > 1)
        uz += x->ga.d.z * (a->fnu - 2) + x->ga.e1.z;
    if (a->fnv > 1)
        vz += gk[GK_F0 + 5] * (a->fnv - 2) + gk[GK_F0 + 8];
    return a->fo.z + (uz < 0 ? uz : 0) + (vz < 0 ? vz : 0);
}

/* water, a whole face near enough: its grid points moved by the waves (and projected again;
   those that don't move are as the grid had them) and its lights rippled, into lit */
static __attribute__((noinline)) void water_grid(r_ctx *x, u16 *lit, const u16 *raw)
{
    const face_args *a = &x->fa;
    const v3        *fs = (const v3 *)&x->gk[GK_F0];
    gv              *g = x->grid;
    int             i, j, nu = a->fnu, nv = a->fnv;

    for (j = 0; j <= nv; ++j)
    {
        v3  b, c = { 0, 0, 0 };

        grid_step(&b, &fs[0], &fs[1], &fs[2], nv, j);
        for (i = 0; i <= nu; ++i, ++g, ++lit, ++raw)
        {
            v3  V;
            int r;
            s32 h;

            if (i == 1)
                c = x->ga.e0;
            else if (i > 1)
            {
                const v3 *st = i < nu ? &x->ga.d : &x->ga.e1;      /* (the grid's steps, as dl_row's) */

                c.x += st->x; c.y += st->y; c.z += st->z;
            }
            V.x = a->fo.x + c.x + b.x; V.y = a->fo.y + c.y + b.y; V.z = a->fo.z + c.z + b.z;
            h = wave_at(&V, &r);
            if (h)
            {
                V.x += fmul(x->wn.x, h);
                V.y += fmul(x->wn.y, h);
                V.z += fmul(x->wn.z, h);
                grid_point(g, V.x, V.y, V.z, fmul(V.z, kx), fmul(V.z, ky));
            }
            *lit = light_add(*raw, r);
        }
    }
}

/* (the options' "blend, plain water") a translucent water face's cells, written from command
   first on: half-transparent Gouraud polygons, not textured ones, in its texture's colour (its first
   cell's colour table's, averaged, at the brightness chosen): VDP1 has no texels to read for them,
   only the screen's pixels it blends with. Their corners and Gouraud tables are as written */
static __attribute__((noinline)) void plain_water(vdp_writer *w, int first)
{
    const u16       *lut;
    u32             r = 0, g = 0, b = 0, n, i;
    u16             colour;
    int             k;

    if (first >= w->count)
        return;
    n = ((u32)w->cmds[first].colr * 8 - lut_vram) / 32;    /* (its colour table: the first cell's) */
    if (n >= (u32)lv.nluts)
        return;
    lut = &lv.luts[n * 16];
    for (i = 1; i < 16; ++i)                /* (0: the transparent pixels') */
    {
        r += lut[i] & 31;
        g += lut[i] >> 5 & 31;
        b += lut[i] >> 10 & 31;
    }
    colour = r_gamma((u16)(0x8000 | (b / 15) << 10 | (g / 15) << 5 | r / 15));
    for (k = first; k < w->count; ++k)
    {
        vdp1_cmd    *c = &w->cmds[k];

        c->ctrl = (u16)((c->ctrl & ~0xF) | VDP1_POLYGON);
        c->pmod = PMOD_RGB | PMOD_GOURAUD | PMOD_HALF_TRANS;
        c->colr = colour;
    }
}

/* a face's cells, its grid done if whole; if not (a big face), its grid and cells a row at
   a time */
static __attribute__((noinline)) void face_cells(r_ctx *x, const q_face *f, int model, bool whole)
{
    const face_args *a = &x->fa;
    const s32       *gk = x->gk;
    v3              rowp, du, e0, e1;
    s32             dtx, dty;
    gv              *top = x->grid, *bot = x->grid + MAX_ROW, *tmp;
    const u16       *light = a->light;
    const q_cell    *row_cells = a->cells;
    int             i, j, nu = a->fnu, nv = a->fnv, N = lv.N, stride = nu + 1;
    int             eu0 = a->eu0, eu1 = a->eu1, ev0 = a->ev0, ev1 = a->ev1, ct, cb;
    bool            fast_ok;
    vdp_writer      *w = x->w;
    int             count0 = w->count;
    u16             lit[2 * MAX_ROW];       /* the lights with the dynamic lights added: the whole face's, or two rows */
#ifdef R_PROFILE
    u32             pt = frt_read(), pt2;
    int             gc0 = w->gcount;
#endif

    if (whole)
        bot = top + stride;
    else
    {
        /* its first row */
        rowp = a->fo; du = x->ga.d; e0 = x->ga.e0; e1 = x->ga.e1;
        dtx = fmul(du.z, kx);
        dty = fmul(du.z, ky);
        if (r_grid_asm)
        {
            x->ga.p = rowp;
            grid_row_asm(top, nu + 1, &x->ga, grid_k);
        }
        else
            grid_row(top, nu + 1, &rowp, &du, &e0, &e1, dtx, dty);
        PROF((pt2 = frt_read(), x->st.p_grid += (pt2 - pt) & 0xFFFF, pt = pt2));
    }
    /* dynamic lights: added to the lights at each grid point, then drawn as any other face;
       water near enough moves, and its lights ripple */
    face_dlights(x, f, model);
#ifdef FIGHT_BENCH
    x->st.n_dlfaces += x->dmask != 0;       /* (not timed: two timer reads a face cost more than the test) */
#if defined(DSP_WALLS) && !defined(DW_CHECK)
    x->st.dw_why[0] += x->dmask && r_dl_verts && !whole;
#endif
#endif
#ifdef DSP_WALLS
    if (whole && x->dw_rp && !model)
        dw_note(x, f, light);               /* (for the DSP next frame, lit or not) */
#endif
    x->wave = whole && f->flags & FF_WARP && r_water && face_zmin(x) < WAVE_FAR;
    x->lit = (x->dmask && r_dl_verts) || x->wave;
    if (x->lit)
    {
        const u16   *wl_hit = NULL;

        x->raw_light = light;
        if (whole)
        {
            if (x->wave)
            {
                to_view(lv.planes[f->plane].n, &x->wn);
                water_grid(x, lit, light);
            }
            if (x->dmask && r_dl_verts)
            {
#ifdef FIGHT_BENCH
                u32 tdl = frt_read();
#endif
#ifdef DLF_CHECK
                {
                    u16 ref[2 * MAX_ROW];
                    int np = stride * (nv + 1), k;

                    for (j = 0; j <= nv; ++j)
                        dl_row(x, ref + j * stride, (x->wave ? lit : light) + j * stride, j);
                    dl_face(x, lit, x->wave ? lit : light, dl);
                    ++dlf_checks;
                    for (k = 0; k < np; ++k)
                        if (ref[k] != lit[k])
                        {
                            ++dlf_diffs;
                            break;
                        }
                }
#elif defined(OLD_DL)
                for (j = 0; j <= nv; ++j)
                    dl_row(x, lit + j * stride, (x->wave ? lit : light) + j * stride, j);
#elif defined(NO_DL_SUMS)
                memcpy(lit, x->wave ? lit : light, (u32)(stride * (nv + 1)) * 2);     /* (test: the sums' cost) */
#else
#ifdef WALLS_AHEAD
                if (!x->wave && !model && a->light == &lv.lights[f->firstlight & 0xFFFFFF])
                {
                    wl_record(x, (int)(f - lv.faces));      /* (for the next frame's) */
                    wl_hit = wl_find((int)(f - lv.faces));  /* (lit already, a frame behind: r_wall_ahead) */
                }
#elif defined(DSP_WALLS)
                if (!x->wave && !model && a->light == &lv.lights[f->firstlight & 0xFFFFFF] && dw_fits(f))
                {
                    wl_hit = dw_find((int)(f - lv.faces), stride * (nv + 1));     /* (lit by the DSP?) */
#if defined(FIGHT_BENCH) && !defined(DW_CHECK)
                    if (!wl_hit)
                        ++x->st.dw_why[dw_have ? 3 : 2];
#endif
                }
#endif
                if (!wl_hit)
                    dl_face(x, lit, x->wave ? lit : light, dl);
#ifdef DW_CHECK
                else
                {
                    /* (OPT=-DDW_CHECK: the DSP's against dl_face's, here, in view space: points, different,
                       most a colour's out) */
                    int np = stride * (nv + 1), k, c;

                    dl_face(x, lit, light, dl);
                    x->st.dw_why[0] += np;
                    for (k = 0; k < np; ++k)
                        if (lit[k] != wl_hit[k])
                        {
                            ++x->st.dw_why[1];
                            for (c = 0; c < 15; c += 5)
                                x->st.dw_why[2] = imax(x->st.dw_why[2], iabs((lit[k] >> c & 31) - (wl_hit[k] >> c & 31)));
                        }
                }
#endif
#endif
#ifdef FS_STATS
                if (!x->wave && !model && a->light == &lv.lights[f->firstlight & 0xFFFFFF])
                {
                    int np = stride * (nv + 1), nl = __builtin_popcount(x->dmask);

                    ++x->st.fs[np <= 16 ? 0 : np <= 32 ? 1 : np <= 48 ? 2 : np <= 64 ? 3 : 4];
                    x->st.fs[5] += stride > 12;
                    x->st.fs[6] += stride > 16;
                    ++x->st.fs[nl <= 1 ? 7 : nl == 2 ? 8 : 9];
                }
#endif
#ifdef FIGHT_BENCH
                x->st.t_dlsum += (frt_read() - tdl) & 0xFFFF;
                x->st.n_wlhit += wl_hit != NULL;
#endif
            }
#ifdef DL_CHECK
            for (j = 0; j < nv; ++j)
                dl_check(x, lit + j * stride, light + j * stride, stride, j);
#endif
            light = wl_hit ? wl_hit : lit;
        }
        else
            dl_row(x, lit, light, 0);       /* (row 0; each row's next as it comes) */
    }
    /* translucent (glass, water): VDP1's mesh or half-transparency (the options' choice) */
    if (f->flags & (FF_TRANS33 | FF_TRANS66) && r_trans)
        x->cpmod = x->ca.pmod = (u32)(CELL_PMOD | (r_trans == 1 ? PMOD_MESH : PMOD_HALF_TRANS)) << 16;
    fast_ok = r_cells_asm && (!x->dmask || x->lit) && !r_debug;
    if (fast_ok)
        cells_face(x, stride, row_cells);
    if (whole && fast_ok && w->count + nu * nv <= w->cmax && w->gcount + nu * nv <= w->gmax)
    {
        /* the common cells of the whole face in assembly, then the C for what it left */
        int n, li;

        x->ca.cl0 = (u32)eu0; x->ca.cr1 = (u32)eu1; x->ca.ct0 = (u32)ev0; x->ca.cb1 = (u32)ev1;
        x->ca.rows0 = (u32)nv;
        PROF((pt2 = frt_read(), x->st.p_cpre += (pt2 - pt) & 0xFFFF));
        n = cells_run(x, top, bot, row_cells, light, nu, nv);

        PROF((x->st.nexact += n));          /* (profile: X counts the cells the assembly left) */
        for (li = 0; li < n; ++li)
        {
            int c = x->ca.def[li], jj = c / nu;

            i = c - jj * nu;
            cell_c(x, row_cells + c, top + jj * stride, top + (jj + 1) * stride, light + jj * stride, stride, i, jj,
                   i == 0 ? eu0 : 0, i == nu - 1 ? eu1 : N, jj == 0 ? ev0 : 0, jj == nv - 1 ? ev1 : N);
        }
        PROF((pt2 = frt_read(), x->st.p_cells += (pt2 - pt) & 0xFFFF, pt = pt2));
    }
    else for (j = 0; j < nv; ++j, light += stride, row_cells += nu)
    {
        int         n = nu, li;
        const u16   *list = cell_all, *lrow = light;

        ct = j == 0 ? ev0 : 0;
        cb = j == nv - 1 ? ev1 : N;
        PROF((++x->st.n_rows, x->st.n_rfaces += j == 0));
        if (!whole)
        {
            /* the next row: f0 after the first, f1 before the last, dv between */
            const s32 *step = gk + (j == 0 ? GK_F0 : j == nv - 1 ? GK_F0 + 6 : GK_F0 + 3);

            rowp.x += step[0]; rowp.y += step[1]; rowp.z += step[2];
            if (r_grid_asm)
            {
                x->ga.p = rowp;
                grid_row_asm(bot, nu + 1, &x->ga, grid_k);
            }
            else
                grid_row(bot, nu + 1, &rowp, &du, &e0, &e1, dtx, dty);
            if (x->lit)
            {
                /* the lit lights: rows j and j + 1 */
                dl_row(x, lit + stride, x->raw_light + (j + 1) * stride, j + 1);
#ifdef DL_CHECK
                dl_check(x, lit, x->raw_light + j * stride, stride, j);
#endif
                lrow = lit;
            }
        }
        /* a row at a time: the common cells in assembly if there's room in the lists */
        if (fast_ok && w->count + nu <= w->cmax && w->gcount + nu <= w->gmax)
        {
            x->ca.cell0 = row_cells;
            x->ca.cl0 = (u32)eu0; x->ca.cr1 = (u32)eu1; x->ca.ct0 = (u32)ct; x->ca.cb1 = (u32)cb;
            x->ca.rows0 = 1;
            n = cells_run(x, top, bot, row_cells, lrow, nu, 1);
            list = x->ca.def;
            PROF((x->st.nexact += n));
        }
        for (li = 0; li < n; ++li)
        {
            i = list[li];
            cell_c(x, row_cells + i, top, bot, lrow, stride, i, j, i == 0 ? eu0 : 0, i == nu - 1 ? eu1 : N, ct, cb);
        }
        if (x->lit && !whole)
            memcpy(lit, lit + stride, (u32)stride * 2);     /* (row j + 1 is the next's first) */
        PROF((pt2 = frt_read(), x->st.p_cells += (pt2 - pt) & 0xFFFF, pt = pt2));
        if (whole)
        {
            top = bot;
            bot += stride;
        }
        else
        {
            tmp = top; top = bot; bot = tmp;
        }
    }
    x->st.cells += w->count - count0;       /* once a face, not a store a cell */
    if (r_trans == 3 && f->flags & FF_WARP && f->flags & (FF_TRANS33 | FF_TRANS66))
        plain_water(w, count0);
    x->cpmod = x->ca.pmod = (u32)CELL_PMOD << 16;
#ifdef R_PROFILE
    prof_gouraud(x, gc0);
#endif
}

#ifdef FACE_CHECK
/* (OPT=-DFACE_CHECK: face_asm's results against face_setup's, every face; the axes'
   cache emptied first for every other, so both work them out) */
u32                 face_checks, face_diffs, face_rows3, face_lods;   /* (and how many a row at a time, far) */

static int          face_check(r_ctx *x, const q_face *f, int fi, int model)
{
    face_args       fa;
    grid_args       ga;
    s32             gk[GK_F0 + 9], *c = axis_view + f->axes * 8;
    u32             sum = 0, sum2 = 0;
    int             r, r2, k, n = 0;

    if (fi & 1)
        c[3] = c[7] = -1;
    r = face_asm(&x->fa, fi, model);
    fa = x->fa;
    ga = x->ga;
    memcpy(gk, x->gk, sizeof(gk));
    if (r == 2)
        for (n = (fa.fnu + 1) * (fa.fnv + 1), k = 0; k < n; ++k)
            sum = sum * 31 + x->grid[k].xy + x->grid[k].ocd;
    if (fi & 1)
        c[3] = c[7] = -1;
    r2 = face_setup(x, f, model);
    ++face_checks;
    face_rows3 += r2 == 3;
    face_lods += r2 >= 2 && x->fa.cells >= lv.lodcells && lv.lodcells;
    for (k = 0; k < n; ++k)
        sum2 = sum2 * 31 + x->grid[k].xy + x->grid[k].ocd;
    if (r != r2 || (r >= 2 && (memcmp(&fa, &x->fa, __builtin_offsetof(face_args, rt))
                               || memcmp(&ga.e0, &x->ga.e0, 3 * sizeof(v3)) || memcmp(gk + GK_F0, x->gk + GK_F0, 9 * 4)))
        || (r == 2 && (memcmp(&ga.p, &x->ga.p, sizeof(v3)) || gk[GK_ROWS] != x->gk[GK_ROWS] || sum != sum2)))
        ++face_diffs;
    return r2;
}
#endif


#ifndef PORTALS
void                r_portals_level(void)
{
}
#else
/* ---- portals ----

   The level's clusters and the openings between them (tools/bake_map.py cluster_portals: a box
   round each pair's portals). Each frame the view flows out from the camera's cluster through
   them, a screen rectangle a cluster reached (what of it could be seen through the openings on
   the way: each portal's box's rectangle, conservative, cut down by where it's seen from, and
   what reaches a cluster by more than one way added together). A face whose cluster wasn't
   reached isn't drawn; one whose was is tested against its cluster's rectangle as it's set up
   (face_asm: the four corners of its grid against the rectangle's sides, as against the
   screen's). A face in more than one cluster, or none (a brush model's), isn't tested. The
   walk, the models and the AI's sight still go by the PVS. (OPT=-DPORTALS with PORTALS=1 ./build.sh:
   a test, off: section 29 of OVERNIGHT.md. The flow costs far more than the drawing it saves) */
#define PORTAL_SLOTS    (255)               /* clusters a frame can reach (more: no culling that frame) */
#define RECT_FULL       ((u32)((SCREEN_W / 2) << 8 | SCREEN_H))  /* x0/2 y0 x1/2 y1, a byte each */

static bool         r_portals;              /* the level has them, and room for the rest */
static bool         portals_on;             /* this frame's flow's in */
static u16          *face_clu;              /* each face's cluster (0xFFFF: none, or more than one) */
static u8           *clu_slot;              /* each cluster's slot this frame (0: not reached) */
static u32          *slot_rect;             /* each slot's rectangle, [1 .. PORTAL_SLOTS] */
static u16          *slot_clu;              /* ... its cluster */
static u16          *slot_work;             /* the flow's slots to go on from (a ring: first in, first out) */
static u8           portal_pvs[256];        /* the camera's cluster's PVS (the flow goes only where it could see) */
static int          portal_pvs_of = -1;
static u8           *slot_queued;
static int          nslots;

/* room: high work RAM if it has it, else low, else the cart (NULL: none) */
static void         *portal_alloc(u32 bytes, bool high)
{
    u32             hw, lw, ca;

    level_free(&hw, &lw, &ca);
    if (high && hw >= bytes + 16)
        return level_alloc(bytes);
    if (lw >= bytes + 16)
        return level_alloc_low(bytes);
    return ca >= bytes + 2048 ? cart_alloc(bytes) : NULL;
}

/* a new level (main.c, once the models and the traces have had their HWRAM): the faces'
   clusters (from the leaves' lists of them), the flow's state */
void                r_portals_level(void)
{
    u32             slots = PORTAL_SLOTS + 1;
    u8              *st;
    int             i, k;

    r_portals = false;
    nslots = 0;
    portal_pvs_of = -1;
    if (!lv.nportals || lv.nclusters <= 0)
        return;
    st = portal_alloc((u32)lv.nclusters + slots * (4 + 2 + 2 + 1) + 16, true);
    face_clu = portal_alloc((u32)lv.nfaces * 2, false);
    if (!st || !face_clu)
        return;
    slot_rect = (u32 *)(((u32)st + 3) & ~3u);
    slot_clu = (u16 *)(slot_rect + slots);
    slot_work = slot_clu + slots;
    slot_queued = (u8 *)(slot_work + slots);
    clu_slot = slot_queued + slots;
    memset(clu_slot, 0, (u32)lv.nclusters);
    memset(slot_queued, 0, slots);
    for (i = 0; i < lv.nfaces; ++i)
        face_clu[i] = 0xFFFE;
    for (i = 0; i < lv.nleafs; ++i)
    {
        const q_leaf *l = &lv.leafs[i];

        if (l->cluster < 0)
            continue;
        for (k = 0; k < l->nummark; ++k)
        {
            int f = lv.marks[l->firstmark + k];

            if (f >= lv.models[0].numfaces)
                continue;                   /* (a brush model's: in its own tree's leaves, cluster 0 or not) */
            face_clu[f] = face_clu[f] == 0xFFFE || face_clu[f] == (u16)l->cluster ? (u16)l->cluster : 0xFFFF;
        }
    }
    for (i = 0; i < lv.nfaces; ++i)
        if (face_clu[i] == 0xFFFE)
            face_clu[i] = 0xFFFF;
    r_portals = true;
}

/* screen x of view-space x at depth z (z > 0), rounded down (up) */
static int          portal_sx(s32 x, s32 z, bool up)
{
    s32             r = iclamp(fdiv(x, z), -FIX(2), FIX(2)) * FOCAL;

    return CX + (up ? -((-r) >> 16) : r >> 16);
}

/* the sides of rectangle r (x0/2 y0 x1/2 y1) as slopes (16.16: x / z at the left and right, y / z
   at the top and bottom; 65536 / 160 = 2048 / 5) */
typedef struct { s32 l, r, t, b; } rect_slopes;

static void         rect_slopes_of(u32 r, rect_slopes *k)
{
    k->l = (((s32)(r >> 24) * 2 - CX) << 11) / 5;
    k->r = (((s32)(r >> 8 & 255) * 2 - CX) << 11) / 5;
    k->t = ((CY - (s32)(r >> 16 & 255)) << 11) / 5;
    k->b = ((CY - (s32)(r & 255)) << 11) / 5;
}

/* a portal's box on the screen, within rectangle r (its sides k): a rectangle round it (its
   view-space extents, each way from the depths that make it widest), 0 if none of it can be seen
   there, RECT_FULL if it's across the near plane. A box wholly off one of r's sides (each a
   plane through the eye) is found with no divides */
static u32          portal_rect(const q_portal *p, const rect_slopes *rk)
{
    s32             c[3], e[3], vx, vy, vz, ex, ey, ez, z0, z1, x0, x1, y0, y1;
    int             k, sx0, sx1, sy0, sy1;

    for (k = 0; k < 3; ++k)
    {
        c[k] = ((s32)(p->lo[k] + p->hi[k]) << 15) - cam.pos[k];
        e[k] = (s32)(p->hi[k] - p->lo[k]) << 15;
    }
    vz = fmul(c[0], cam.fwd[0]) + fmul(c[1], cam.fwd[1]) + fmul(c[2], cam.fwd[2]);
    ez = fmul(e[0], iabs(cam.fwd[0])) + fmul(e[1], iabs(cam.fwd[1])) + fmul(e[2], iabs(cam.fwd[2])) + 3;
    z1 = vz + ez;
    if (z1 <= 0)
        return 0;                           /* behind the camera */
    z0 = vz - ez;
    if (z0 < NEAR_Z)
        return RECT_FULL;                   /* across the near plane (or you're in it) */
    vx = fmul(c[0], cam.right[0]) + fmul(c[1], cam.right[1]) + fmul(c[2], cam.right[2]);
    ex = fmul(e[0], iabs(cam.right[0])) + fmul(e[1], iabs(cam.right[1])) + fmul(e[2], iabs(cam.right[2])) + 3;
    vy = fmul(c[0], cam.up[0]) + fmul(c[1], cam.up[1]) + fmul(c[2], cam.up[2]);
    ey = fmul(e[0], iabs(cam.up[0])) + fmul(e[1], iabs(cam.up[1])) + fmul(e[2], iabs(cam.up[2])) + 3;
    x0 = vx - ex;
    x1 = vx + ex;
    y0 = vy - ey;
    y1 = vy + ey;
    sx0 = imax(portal_sx(x0, x0 < 0 ? z0 : z1, false) - 1, 0);
    sx1 = imin(portal_sx(x1, x1 < 0 ? z1 : z0, true) + 1, SCREEN_W);
    sy0 = imax(CX + CY - portal_sx(y1, y1 < 0 ? z1 : z0, true) - 1, 0);    /* (CY - F y / z) */
    sy1 = imin(CX + CY - portal_sx(y0, y0 < 0 ? z0 : z1, false) + 1, SCREEN_H);
    if (sx0 >= sx1 || sy0 >= sy1)
        return 0;
    return (u32)(sx0 >> 1) << 24 | (u32)sy0 << 16 | (u32)((sx1 + 1) >> 1) << 8 | (u32)sy1;
}

static inline u32   umin(u32 a, u32 b)
{
    return a < b ? a : b;
}

static inline u32   umax(u32 a, u32 b)
{
    return a > b ? a : b;
}

static inline u32   rect_and(u32 a, u32 b)
{
    u32             x0 = umax(a >> 24, b >> 24), y0 = umax(a >> 16 & 255, b >> 16 & 255);
    u32             x1 = umin(a >> 8 & 255, b >> 8 & 255), y1 = umin(a & 255, b & 255);

    return x0 < x1 && y0 < y1 ? x0 << 24 | y0 << 16 | x1 << 8 | y1 : 0;
}

static inline u32   rect_or(u32 a, u32 b)
{
    return umin(a >> 24, b >> 24) << 24 | umin(a >> 16 & 255, b >> 16 & 255) << 16
           | umax(a >> 8 & 255, b >> 8 & 255) << 8 | umax(a & 255, b & 255);
}

static inline bool  rect_in(u32 a, u32 b)  /* a within b */
{
    return a >> 24 >= b >> 24 && (a >> 16 & 255) >= (b >> 16 & 255) && (a >> 8 & 255) <= (b >> 8 & 255)
           && (a & 255) <= (b & 255);
}

/* the flow from the camera's cluster: false if it reached more than there are slots. First in,
   first out (breadth first: a cluster's rectangle tends to be whole before it's gone on from:
   half the going round again of last in, first out); only into clusters in the camera's PVS
   (anything seen is on a line of sight, and every cluster that crosses is in it) */
static bool         portals_flow(int cluster)
{
    const u16       *first = lv.cportals, *list = lv.cportals + lv.nclusters + 1;
    int             i, head = 0, tail = 0;
    bool            prune = lv.nclusters <= (int)sizeof(portal_pvs) * 8;

    if (prune && portal_pvs_of != cluster)
    {
        memcpy(portal_pvs, level_pvs(cluster), (u32)(lv.nclusters + 7) >> 3);
        portal_pvs_of = cluster;
    }
    for (i = 1; i <= nslots; ++i)
        clu_slot[slot_clu[i]] = 0;          /* (last frame's) */
    nslots = 1;
    slot_clu[1] = (u16)cluster;
    slot_rect[1] = RECT_FULL;
    clu_slot[cluster] = 1;
    slot_work[tail++] = 1;
    slot_queued[1] = 1;
    while (head != tail)
    {
        int         s = slot_work[head], u = slot_clu[s], k;
        u32         r = slot_rect[s];
        rect_slopes rk;

        head = head == PORTAL_SLOTS ? 0 : head + 1;
        rect_slopes_of(r, &rk);

        slot_queued[s] = 0;
        for (k = first[u]; k < first[u + 1]; ++k)
        {
            int             o = list[2 * k], os = clu_slot[o];
            u32             pr;

#ifdef FIGHT_BENCH
            ++rs.n_ptest;
#endif
            if (prune && !(portal_pvs[(u32)o >> 3] & bitm[o & 7]))
                continue;                   /* (it can't see there) */
            if (os && rect_in(r, slot_rect[os]))
                continue;                   /* (it's seen at least that much already) */
#ifdef FIGHT_BENCH
            ++rs.n_proj;
#endif
            if (!(pr = portal_rect(&lv.portals[list[2 * k + 1]], &rk)) || !(pr = rect_and(pr, r)))
                continue;
            if (!os)
            {
                if (nslots == PORTAL_SLOTS)
                    return false;
                os = ++nslots;
                slot_clu[os] = (u16)o;
                slot_rect[os] = pr;
                clu_slot[o] = (u8)os;
            }
            else
            {
                u32 nr = rect_or(slot_rect[os], pr);

                if (nr == slot_rect[os])
                    continue;
                slot_rect[os] = nr;
            }
            if (!slot_queued[os])
            {
                slot_queued[os] = 1;
                slot_work[tail] = (u16)os;
                tail = tail == PORTAL_SLOTS ? 0 : tail + 1;
            }
        }
    }
    return true;
}

/* a face of the world's: false if its cluster wasn't reached; else its cluster's rectangle
   for face_asm (prect 0: the whole screen, or not known) */
static inline bool  portal_face(face_args *fa, int fi)
{
    u32             c = face_clu[fi], r;
    int             s;

    fa->prect = 0;
    if (c == 0xFFFF)
        return true;
    if (!(s = clu_slot[c]))
        return false;
    if ((r = slot_rect[s]) == RECT_FULL)
        return true;
    {
        rect_slopes k;

        rect_slopes_of(r, &k);
        fa->prx0 = k.l;
        fa->prx1 = k.r;
        fa->pry1 = k.t;
        fa->pry0 = k.b;
    }
    fa->prect = 1;
    return true;
}
#endif

static __attribute__((noinline)) void draw_face(r_ctx *x, int fi, int model)
{
    const q_face    *f = &lv.faces[fi];
    int             r;
#ifdef R_PROFILE
    u32             pxf = frt_read();
#endif

    x->fa.prect = 0;
#ifdef PORTALS
    if (portals_on && !model && !portal_face(&x->fa, fi))
    {
        ++x->st.portal_out;
        return;                             /* (not through the portals) */
    }
#endif

#if defined(FACE_CHECK)
    r = face_check(x, f, fi, model);        /* (the C's results kept) */
#elif defined(NO_FACE_ASM)
    r = face_setup(x, f, model);
#else
    r = face_asm(&x->fa, fi, model);
#endif
    PROF(x->st.p_xform += (frt_read() - pxf) & 0xFFFF);
    if (r == 0)
        return;
    ++x->st.faces;
    if (r == 1)
    {
        PROF(++x->st.faces_out);
        return;
    }
    PROF((x->st.gverts += (x->fa.fnu + 1) * (x->fa.fnv + 1), x->st.seen += x->fa.fnu * x->fa.fnv));
    face_cells(x, f, model, r == 2);
}

/* src/mdraw.s: a model's whole mesh from the DSP into screen space (offsets fixed there) */
typedef struct
{
    const s32       *out;                   /* the DSP's results: blocks of 16 x, 16 y, 16 z */
    u32             *mxy;
    s32             *mz;
    u8              *moc;
    s32             n, zmin, zmax;          /* (zmin and zmax in and out) */
}                   mverts_args;

void                mverts_asm(mverts_args *a);

/* ...and its polygons facing the camera into depth buckets */
typedef struct
{
    const q_mpoly   *polys;
    s32             n;
    const u8        *moc;
    const u32       *mxy;
    const s32       *mz;
    s16             *mhead;                 /* (mnext right after it) */
    s32             zmin, inv;
    s32             sign;                   /* 0: MODEL_FRONT; -1: the other way (the gun) */
    s16             *nearl;                 /* those across the near plane (NULL: dropped) */
    s32             nnear;                  /* ...how many, in and out */
}                   mpolys_args;

void                mpolys_asm(mpolys_args *a);
_Static_assert(__builtin_offsetof(mpolys_args, sign) == 32 && __builtin_offsetof(mpolys_args, nnear) == 40,
               "mdraw.s: mpolys_args");

/* ...and those into commands; the offsets are mdraw.s's M_ */
typedef struct
{
    const q_mpoly   *polys;
    const q_mtex    *tex;
    const s16       *mnext;
    const u32       *mxy;
    const u16       *mg;
    const u16       *tslot;
    u16             *sframe;
    u32             *cmds, *gst;
    s32             tid0;
    u32             frame, svram, sbytes, lb, dw1;
    const u16       *slut;                  /* slot -> its colour table (slot_lut) */
    s32             gmax;
    u32             gb, fifo;
    r_ctx           *x;
    s32             cnt, head, tail, gc, drop;
    const s16       *mhead;
    s32             wcmds;
}                   mcmds_args;

void                mcmds_asm(mcmds_args *a);
_Static_assert(__builtin_offsetof(mcmds_args, slut) == 60 && __builtin_offsetof(mcmds_args, cnt) == 80
               && __builtin_offsetof(mcmds_args, wcmds) == 104, "mdraw.s: mcmds_args");
_Static_assert(__builtin_offsetof(r_ctx, mnext) == __builtin_offsetof(r_ctx, mhead) + MBUCKETS * 2, "mdraw.s: mnext after mhead");

/* ---- the gun in your hands (src/view.c) ---- */

#define VIEW_NEAR       FIX(4)              /* (Quake 2's near plane for its guns: they're close) */
#define VIEW_FRONT      (1)                 /* the guns' triangles wind the other way from the monsters':
                                               MODEL_FRONT draws their insides (seen side by side with a
                                               z-buffered render on the PC; a soldier the same way matches) */

/* src/mdraw.s: the gun's vertices from its two frames into screen space. The frames are in
   the eye's space (Quake's x forward, y left, z up), so into the view's (x right = -y,
   y up = z, z forward = x) as they are (offsets fixed in mdraw.s: V_) */
typedef struct
{
    s32             st[12];                 /* per view axis (z x y): frame 0's scale, translation, frame 1's */
    s32             lerp;
    s32             zmin, zmax;             /* in and out */
    const u8        *pa, *pb;               /* the two frames' vertices (x y z normal) */
    u32             *mxy;
    s32             *mz;
    u8              *moc, *vn;              /* (vn: each one's normal, the nearer frame's) */
    s32             n, nofs;                /* (nofs: the normal's byte from pa: 3, or frame 1's) */
}                   vverts_args;

void                vverts_asm(vverts_args *a);
_Static_assert(__builtin_offsetof(vverts_args, zmin) == 52 && __builtin_offsetof(vverts_args, pa) == 60
               && __builtin_offsetof(vverts_args, nofs) == 88, "mdraw.s: vverts_args");

static void         view_blend(vverts_args *a, const u8 *f0, const u8 *f1, s32 lerp, const u8 *pa, const u8 *pb)
{
    const s32       *h0 = (const s32 *)f0, *h1 = (const s32 *)f1;
    int             k;

    for (k = 0; k < 3; ++k)                 /* (z from Quake's x, x from its y, y from its z) */
    {
        a->st[k * 4 + 0] = h0[k];
        a->st[k * 4 + 1] = h0[3 + k];
        a->st[k * 4 + 2] = h1[k];
        a->st[k * 4 + 3] = h1[3 + k];
    }
    a->lerp = lerp;
    a->pa = pa;
    a->pb = pb;
}

/* a vertex of the gun, blended, in view space (as vverts_asm has it) */
static void         view_vertex(const vverts_args *a, int i, v3 *o)
{
    const u8        *p = a->pa + i * 4, *q = a->pb + i * 4;
    s32             c[3];
    int             k;

    for (k = 0; k < 3; ++k)
    {
        const s32   *st = &a->st[k * 4];
        s32         u = st[0] * p[k] + st[1], v = st[2] * q[k] + st[3];

        c[k] = u + fmul(v - u, a->lerp);
    }
    o->z = c[0];
    o->x = -c[1];
    o->y = c[2];
}

/* a polygon's corners on the screen (before the bob: ux the vertices' places then). One
   behind the near plane is pulled along an edge to a corner in front, to where the edge
   crosses the plane: the texture squeezes a little at the screen's edge, rather than a hole
   there. false: all of it behind */
static bool         view_poly_xy(r_ctx *x, const vverts_args *va, const q_mpoly *p, u32 *xy, const u32 *ux)
{
    int             k;

    for (k = 0; k < 4; ++k)
    {
        int vi = p->v[k], f = -1, j;
        v3  a, b;
        s32 t;

        if (!(x->moc[vi] & OC_NEAR))
        {
            xy[k] = ux[vi];
            continue;
        }
        for (j = 1; j < 4 && f < 0; ++j)
        {
            int o = p->v[(k + (j == 1 ? 1 : j == 2 ? 3 : 2)) & 3];     /* the neighbours, then across */

            if (!(x->moc[o] & OC_NEAR))
                f = o;
        }
        if (f < 0)
            return false;
        view_vertex(va, vi, &a);
        view_vertex(va, f, &b);
        t = fdiv(VIEW_NEAR - a.z, b.z - a.z);
        xy[k] = project(x, a.x + fmul(b.x - a.x, t), a.y + fmul(b.y - a.y, t), VIEW_NEAR);
    }
    return true;
}

/* The gun's bob (view.c view_bob: pitch down, yaw left, roll right side down) on the screen:
   the gun turns about the eye, so each point's direction turns, and a point's place on the
   screen is its direction's. The rotation's rows (2.14) take (X right, Y up, FOCAL) from the
   centre to the turned direction, and that's projected again (exact: a divide a point) */
typedef struct { s32 m[9]; bool on; } view_turn;

static void         view_turn_make(view_turn *t)
{
    /* Quake 2 adds the gun's angles to the view's: the gun's axes are AngleVectors of (the
       view's pitch + the gun's, its yaw, its roll) (the view's yaw left out: it turns both
       alike), taken into the view's own axes (pitch vp): x = -w1, y = svp w0 + cvp w2,
       z = cvp w0 - svp w2 for a Quake vector w */
    int             ap = (int)(((view_bob[0] * 10430) >> 16) & 0xFFFF);  /* (radians 16.16 -> 65536 a turn) */
    int             ay = (int)(((view_bob[1] * 10430) >> 16) & 0xFFFF);
    int             ar = (int)(((view_bob[2] * 10430) >> 16) & 0xFFFF);
    int             vp = cam.pitch & 0xFFFF;
    s32             sp = fsin((vp + ap) & 0xFFFF), cp = fcos((vp + ap) & 0xFFFF), sy = fsin(ay), cy = fcos(ay);
    s32             sr = fsin(ar), cr = fcos(ar), svp = fsin(vp), cvp = fcos(vp);
    s32             ax[3][3];               /* the gun's right, up, forward (Quake's coordinates) */
    int             k;

    t->on = ap || ay || ar;
    ax[0][0] = -fmul(fmul(sr, sp), cy) + fmul(cr, sy);
    ax[0][1] = -fmul(fmul(sr, sp), sy) - fmul(cr, cy);
    ax[0][2] = -fmul(sr, cp);
    ax[1][0] = fmul(fmul(cr, sp), cy) + fmul(sr, sy);
    ax[1][1] = fmul(fmul(cr, sp), sy) - fmul(sr, cy);
    ax[1][2] = fmul(cr, cp);
    ax[2][0] = fmul(cp, cy);
    ax[2][1] = fmul(cp, sy);
    ax[2][2] = -sp;
    for (k = 0; k < 3; ++k)
    {
        /* column k: the axis in the view's; the forward one's times FOCAL (the direction's z) */
        s32 x = -ax[k][1], y = fmul(svp, ax[k][0]) + fmul(cvp, ax[k][2]), z = fmul(cvp, ax[k][0]) - fmul(svp, ax[k][2]);

        t->m[k] = (x >> 2) * (k == 2 ? FOCAL : 1);
        t->m[3 + k] = (y >> 2) * (k == 2 ? FOCAL : 1);
        t->m[6 + k] = (z >> 2) * (k == 2 ? FOCAL : 1);
    }
}

/* the bob on n places (in and out may be the same): src/mdraw.s vturn_asm, each one's divide
   running while the one before is finished */
void                vturn_asm(const s32 *m, const u32 *in, u32 *out, int n);
#define view_turn_n(t, in, out, n)  vturn_asm((t)->m, (in), (out), (n))

#if defined(VIEW_CHECK) && VIEW_CHECK < 3
/* (the C it was: the checks' reference) */
static __attribute__((noinline)) void view_turn_c(const view_turn *t, const u32 *in, u32 *out, int n)
{
    s32             X, Y, nz, Xn = 0, Yn = 0, nzn = 0, nx, ny, r = 0, sx, sy;
    int             i;

    if (n <= 0)
        return;
    X = XY_X(in[0]) - CX;
    Y = CY - XY_Y(in[0]);
    nz = t->m[6] * X + t->m[7] * Y + t->m[8];
    if (nz >= 4096)
        divu_start(FOCAL, 0, nz);           /* FOCAL << 32 / nz (both in 2.14 screen units: they go) */
    for (i = 0; i < n; ++i)
    {
        nx = t->m[0] * X + t->m[1] * Y + t->m[2];
        ny = t->m[3] * X + t->m[4] * Y + t->m[5];
        if (nz >= 4096)
            r = divu_result();
        if (i + 1 < n)
        {
            u32 q = in[i + 1];

            Xn = XY_X(q) - CX;
            Yn = CY - XY_Y(q);
            nzn = t->m[6] * Xn + t->m[7] * Yn + t->m[8];
            if (nzn >= 4096)
                divu_start(FOCAL, 0, nzn);
        }
        if (nz < 4096)
        {
            /* (turned nearly square to the view, or behind: to the clamp, its way) */
            sx = nx > 0 ? CLAMP_XY : nx < 0 ? -CLAMP_XY : CX;
            sy = ny > 0 ? -CLAMP_XY : ny < 0 ? CLAMP_XY : CY;
        }
        else
        {
            sx = iclamp(CX + mul_hi(nx, r), -CLAMP_XY, CLAMP_XY);
            sy = iclamp(CY - mul_hi(ny, r), -CLAMP_XY, CLAMP_XY);
        }
        out[i] = (u32)sx << 16 | (u16)sy;
        X = Xn;
        Y = Yn;
        nz = nzn;
    }
}
#endif

#ifdef FIGHT_BENCH
u32                 view_ph[4];             /* (the fight benchmark: the gun's vertices, sort, commands, kept; us) */
#define VPH(k)      (view_ph[k] += frt_to_us((frt_read() - tph) & 0xFFFF), tph = frt_read())
#else
#define VPH(k)      ((void)0)
#endif

#ifdef VIEW_CHECK
/* (OPT=-DVIEW_CHECK: all of draw_viewmodel again in C, and compared: the vertices, then the
   commands and colours) */
u32                 view_checks[7];         /* frames, commands, differing; (=3) vertices, moved, most, across */
#define OVL_CHECK       (200)
static u32          *chk_cmds, *chk_gcs;    /* (LWRAM, r_view_level) */
static u32          *chk_kept, *chk_kgc;    /* (VIEW_CHECK=2: what was kept, gone out) */
static u32          *chk_ux;                /* (the C's places before the bob) */
#if VIEW_CHECK == 2
static struct { const q_mdl *m; int gen, f0, f1; s32 lerp; } ck_key;
static int          ck_nkeep, ck_nkv;
#endif

#if VIEW_CHECK < 3
static __attribute__((noinline)) void draw_viewmodel_ref(r_ctx *x, const q_mdl *m, const vverts_args *va,
                                                         const u8 *f0, const u8 *f1, s32 lerp, const u16 *gt)
{
    int             i, b, nv = imin(m->nverts, MAX_MVERTS), np = imin(m->npolys, VIEW_MAX_POLYS);
    s32             zmin = 0x7FFFFFFF, zmax = -0x7FFFFFFF, inv;
    const u8        *fn = lerp < FIX(0.5) ? f0 : f1;
    vdp_writer      *w = x->w;
    const q_mpoly   *polys = m->polys;
    u32             colr0;
    view_turn       tn;

    for (i = 0; i < nv; ++i)
    {
        v3  v;
        u8  ni = fn[24 + i * 4 + 3];

        view_vertex(va, i, &v);
        x->mz[i] = v.z;
        x->mg[i] = ni < 162 ? ni : 0;
        if (v.z < VIEW_NEAR)
            x->moc[i] = OC_NEAR;
        else
        {
            x->mxy[i] = project(x, v.x, v.y, v.z);
            x->moc[i] = 0;
            if (v.z < zmin)
                zmin = v.z;
            if (v.z > zmax)
                zmax = v.z;
        }
    }
    view_turn_make(&tn);
    memcpy(chk_ux, x->mxy, (u32)nv * 4);
    if (tn.on)
        view_turn_c(&tn, chk_ux, x->mxy, nv);
    if (zmax < zmin)
        return;
    inv = (s32)(((u32)(MBUCKETS - 1) << 16) / (u32)imax((zmax - zmin) >> 16, 1));
    for (b = 0; b < MBUCKETS; ++b)
        x->mhead[b] = -1;
    for (i = 0; i < np; ++i)
    {
        const q_mpoly   *p = &polys[i];
        int             a = p->v[0], bb = p->v[1], c = p->v[2], d = p->v[3];
        u32             xy[4];
        s32             cross, z;

        if (x->moc[a] & x->moc[bb] & x->moc[c] & x->moc[d])
            continue;
        if ((x->moc[a] | x->moc[bb] | x->moc[c] | x->moc[d]) & OC_NEAR)
        {
            if (!view_poly_xy(x, va, p, xy, chk_ux))
                continue;
            if (tn.on)
                view_turn_c(&tn, xy, xy, 4);
        }
        else
        {
            xy[0] = x->mxy[a];
            xy[1] = x->mxy[bb];
            xy[2] = x->mxy[c];
            xy[3] = x->mxy[d];
        }
        cross = (XY_X(xy[1]) - XY_X(xy[0])) * (XY_Y(xy[2]) - XY_Y(xy[0]))
              - (XY_Y(xy[1]) - XY_Y(xy[0])) * (XY_X(xy[2]) - XY_X(xy[0]));
        if (cross == 0 && !(p->flags & 1))
            cross = (XY_X(xy[2]) - XY_X(xy[0])) * (XY_Y(xy[3]) - XY_Y(xy[0]))
                  - (XY_Y(xy[2]) - XY_Y(xy[0])) * (XY_X(xy[3]) - XY_X(xy[0]));
        if (cross * VIEW_FRONT <= 0)
            continue;
        z = (x->mz[a] >> 1) + (x->mz[c] >> 1);
        b = z <= zmin ? 0 : iclamp((((z - zmin) >> 16) * inv) >> 16, 0, MBUCKETS - 1);
        x->mnext[i] = x->mhead[b];
        x->mhead[b] = (s16)i;
    }
    colr0 = lut_vram + (u32)m->lut0 * 32;
    for (b = MBUCKETS - 1; b >= 0; --b)
        for (i = x->mhead[b]; i >= 0; i = x->mnext[i])
        {
            const q_mpoly   *p = &polys[i];
            const q_mtex    *mt = &m->tex[p->tex];
            int             t = m->tex_id0 + p->tex, s = x->tex_slot[t];
            s32             vram;
            u32             *dw, xy[4];

            if (s != 0xFFFF)
            {
                slot_frame[s] = frame;
                vram = (s32)(slot_vram + (u32)s * slot_bytes);
            }
            else if ((vram = tex_load(x, t)) < 0)
                continue;
            if ((x->moc[p->v[0]] | x->moc[p->v[1]] | x->moc[p->v[2]] | x->moc[p->v[3]]) & OC_NEAR)
            {
                if (!view_poly_xy(x, va, p, xy, chk_ux))
                    continue;
                if (tn.on)
                    view_turn_c(&tn, xy, xy, 4);
            }
            else
            {
                xy[0] = x->mxy[p->v[0]];
                xy[1] = x->mxy[p->v[1]];
                xy[2] = x->mxy[p->v[2]];
                xy[3] = x->mxy[p->v[3]];
            }
            if (!(dw = (u32 *)vdp_overlay()))
                return;
            dw[0] = (u32)(0x1000 | VDP1_DISTORTED) << 16;
            dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | ((colr0 + (m->nluts > 1 ? (u32)mt->lut * 32 : 0)) >> 3);
            dw[2] = ((u32)vram >> 3) << 16 | (u32)(((mt->w >> 3) << 8) | mt->h);
            dw[3] = xy[0];
            dw[4] = xy[1];
            dw[5] = xy[2];
            dw[6] = xy[3];
            dw[7] = (u32)vdp_gouraud_fast(w, (u32)gt[x->mg[p->v[0]]] << 16 | gt[x->mg[p->v[1]]],
                                          (u32)gt[x->mg[p->v[2]]] << 16 | gt[x->mg[p->v[3]]]) << 16;
        }
}
#endif
#endif

/* The gun's polygons cut at the near plane get corners of their own: vertices MAX_MVERTS and
   up, in the r_ctx arrays after mxy and mg (mz, then moc and gtab: done with by then, as
   draw_model reads them), and records after its own (the files have VIEW_SUBS spare) */
_Static_assert(__builtin_offsetof(r_ctx, mz) == __builtin_offsetof(r_ctx, mxy) + MAX_MVERTS * 4
               && __builtin_offsetof(r_ctx, moc) == __builtin_offsetof(r_ctx, mg) + MAX_MVERTS * 2
               && __builtin_offsetof(r_ctx, gtab) == __builtin_offsetof(r_ctx, moc) + MAX_MVERTS
               && MAX_MVERTS * 2 + 162 * 2 >= VIEW_SUBS * 4 * 2 && MAX_MVERTS >= VIEW_SUBS * 4
               && VIEW_MAX_POLYS + VIEW_SUBS <= MAX_MPOLYS, "draw_viewmodel: room for the cut polygons");

/* The gun's records (its polygons, the spare ones, its textures') and the two frames it's
   between are read off the cart by DMA while the walk and the game's tick run, into the
   master's own command list: it's free until the master draws its faces, after the gun.
   (Read in place, the cart's misses were most of the gun's time, the other CPU drawing.) */
static struct
{
    const q_mdl     *m;
    int             f0, f1;
    u8              *base;                  /* the records, then the frames */
    const u8        *fsrc;                  /* the first frame's vertices (and the next frame, if that's the other) */
    u32             rbytes, fbytes;
    u8              stage;                  /* 0 nothing; 1 the records on their way; 2 the frames too;
                                               3 what was kept (the gun as it was) */
}                   vf;

static u8           *view_scratch(vdp_writer *w0, u32 bytes)
{
    u8              *p = (u8 *)(((u32)(w0->cmds + w0->count) + 15) & ~15u);

    return p + bytes <= (u8 *)(w0->cmds + w0->cmax) ? p : NULL;
}

static u32          view_rbytes(const q_mdl *m)
{
    return ((u32)((const u8 *)(m->tex + m->ntex) - (const u8 *)m->polys) + 15) & ~15u;
}

/* (render_world, before the walk) the gun's to be drawn, and not as it was last frame: its records on their way */
static void         view_fetch_start(vdp_writer *w0)
{
    int             f0i, f1i;
    s32             lerp;
    const q_mdl     *m = view_frame(&f0i, &f1i, &lerp);

    vf.stage = 0;
    if (!m)
        return;
    if (f1i == f0i)
        lerp = 0;
    vf.m = m;
    if (view_key.m == m && view_key.gen == view_gen && view_key.f0 == f0i && view_key.f1 == f1i
        && view_key.lerp == lerp)
    {
        /* it'll go out as kept: that's what's read */
        vf.rbytes = ((u32)view_nkeep * sizeof(view_kept) + (u32)view_nkv * 5 + 3) & ~3u;
        if (!view_keep_hot && view_nkeep && (vf.base = view_scratch(w0, vf.rbytes)))
        {
            scu_dma0(vf.base, view_keep, vf.rbytes, false);
            vf.stage = 3;
        }
        return;
    }
    vf.rbytes = view_rbytes(m);
    vf.fsrc = m->frames + (u32)f0i * m->frame_bytes + 24;
    vf.fbytes = (f1i == f0i + 1 ? m->frame_bytes : 0) + (u32)m->nverts * 4;
    if (!(vf.base = view_scratch(w0, vf.rbytes + vf.fbytes)))
        return;
    vf.f0 = f0i;
    vf.f1 = f1i;
    scu_dma0(vf.base, m->polys, vf.rbytes, false);
    vf.stage = 1;
}

/* (after the walk) ...and its frames, once the records are in */
static void         view_fetch_more(void)
{
    if (vf.stage == 1 && !scu_dma0_busy())
    {
        scu_dma0(vf.base + vf.rbytes, vf.fsrc, vf.fbytes, false);
        vf.stage = 2;
    }
}

/* The gun's vertices on the DSP, as the monsters' are (models_to_dsp: after theirs, in the
   blocks they leave), if it's to be drawn other than as kept: in view space doubled
   (mverts_asm's near plane is 8, the gun's 4). Only the blend's rounding differs from
   vverts_asm's, which it's otherwise (the DSP's is the monsters': each frame's scale times
   its share of the blend, a vertex a pixel or two off at most); OPT=-DVIEW_EXACT keeps to
   vverts_asm. */
static struct { const q_mdl *m; int f0, f1; s32 lerp; int dsp, blk; } vd = { NULL, 0, 0, 0, -1, 0 };

static int          view_to_dsp(u32 *st, int nm, int nb)
{
    int             f0i, f1i, blocks;
    s32             lerp, w0, w1;
    const q_mdl     *m = view_frame(&f0i, &f1i, &lerp);
    const s32       *h0, *h1;

    vd.dsp = -1;
#if defined(VIEW_EXACT) || (defined(VIEW_CHECK) && VIEW_CHECK < 3)
    (void)st; (void)nm; (void)nb; (void)m; (void)h0; (void)h1; (void)w0; (void)w1; (void)blocks;
    return 0;
#else
    if (!m)
        return 0;
    if (f1i == f0i)
        lerp = 0;
    if (view_key.m == m && view_key.gen == view_gen && view_key.f0 == f0i && view_key.f1 == f1i
        && view_key.lerp == lerp)
        return 0;                           /* (it'll go out as kept) */
    blocks = (imin(m->nverts, MAX_MVERTS) + 15) >> 4;
    if (nm == DSPM_MODELS || nb + blocks > DSPM_BLOCKS)
        return 0;
    h0 = (const s32 *)(m->frames + (u32)f0i * m->frame_bytes);
    h1 = (const s32 *)(m->frames + (u32)f1i * m->frame_bytes);
    w1 = lerp;
    w0 = FIX(1) - lerp;
    /* rows (t, frame 0's bytes', frame 1's): view x = -(Quake's y), y = z, z = x; doubled */
    st[0] = (u32)(-2 * (fmul(h0[4], w0) + fmul(h1[4], w1)));
    st[1] = 0; st[2] = (u32)(-2 * fmul(h0[1], w0)); st[3] = 0;
    st[4] = 0; st[5] = (u32)(-2 * fmul(h1[1], w1)); st[6] = 0;
    st[7] = (u32)(2 * (fmul(h0[5], w0) + fmul(h1[5], w1)));
    st[8] = 0; st[9] = 0; st[10] = (u32)(2 * fmul(h0[2], w0));
    st[11] = 0; st[12] = 0; st[13] = (u32)(2 * fmul(h1[2], w1));
    st[14] = (u32)(2 * (fmul(h0[3], w0) + fmul(h1[3], w1)));
    st[15] = (u32)(2 * fmul(h0[0], w0)); st[16] = 0; st[17] = 0;
    st[18] = (u32)(2 * fmul(h1[0], w1)); st[19] = 0; st[20] = 0;
    st[21] = ((u32)(h0 + 6) & 0x07FFFFFF) >> 2;
    st[22] = ((u32)(h1 + 6) & 0x07FFFFFF) >> 2;
    st[23] = (u32)blocks;
#ifdef DSP_LIGHT
    st[24] = 0;                             /* (lit as kept) */
#else
    st[24] = ((u32)dspm_blk(nb) & 0x07FFFFFF) >> 2;
#endif
    vd.m = m;
    vd.f0 = f0i;
    vd.f1 = f1i;
    vd.lerp = lerp;
    vd.dsp = nm;
    vd.blk = nb;
    return blocks;
#endif
}

/* the cache's copies of what DMA has written over (its associative purge, a line at a time) */
static void         cache_forget(const void *p, u32 bytes)
{
    u32             a = (u32)p & ~15u, end = (u32)p + bytes;

    for (; a < end; a += 16)
        *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
}

/* a new level (view.c): room for the gun's last drawing: in HWRAM if the level's left room
   (read straight from there), else on the cart (read into the list by SCU DMA, which halts
   both CPUs while it writes their RAM: 0.34 ms of a fight's frame, before the slave can start) */
void                r_view_level(void)
{
    u32             hw, lw, ca;

    level_free(&hw, &lw, &ca);
    view_keep_hot = hw >= VIEW_KEEP_BYTES;  /* (nothing takes HWRAM after this) */
    view_keep = view_keep_hot ? level_alloc(VIEW_KEEP_BYTES) : cart_alloc(VIEW_KEEP_BYTES);
    view_key.m = NULL;
    vf.stage = 0;
#ifdef VIEW_CHECK
    chk_cmds = level_alloc_low(OVL_CHECK * 32);
    chk_gcs = level_alloc_low(OVL_CHECK * 8);
    chk_kept = level_alloc_low(OVL_CHECK * 32);
    chk_kgc = level_alloc_low(OVL_CHECK * 8);
    chk_ux = level_alloc_low(MAX_MVERTS * 4);
#endif
}

/* a new level, the gun's slots had (main.c, after view_level_init: nothing takes HWRAM after
   them): more of the models' DSP blocks in what it's left (Installation's: one monster near
   more; Comm Center's: many; demo3's: a far one) */
__attribute__((cold)) void r_dspm_level(void)
{
    u32             hw, lw, ca;

    level_free(&hw, &lw, &ca);
    dspm_nmore = imin(DSPM_MORE, (int)(hw / 192));
    dspm_more = dspm_nmore ? (s32 *)level_alloc((u32)dspm_nmore * 192) : NULL;
}

/* the gun, over the world (VDP1's overlay list, drawn after it): its polygons facing you,
   farthest first, lit by the light where you are and the way you face (Quake's shading).
   In src/mdraw.s as the monsters are (the vertices, the sort, the commands); the polygons
   across the near plane cut here and put in with the rest */
static __attribute__((noinline)) void draw_viewmodel(r_ctx *x, int leaf)
{
    int             f0i, f1i, i, b, k, nv, np, nnear, ns, ys = (int)((u32)cam.yaw >> 12) & 15;
    s32             lerp, zmin, zmax, inv;
    const q_mdl     *m = view_frame(&f0i, &f1i, &lerp);
    const u8        *f0, *f1;
    static u16      gt[162];
    static const q_mdl *gt_m;
    static int      gt_leaf = -1, gt_ys = -1;
    static u32      gt_ver;
    vdp_writer      *w = x->w;
    u32             colr0;
    vverts_args     va;
    q_mpoly         *polys;                                 /* (its records, fetched: view_fetch_start) */
    const q_mtex    *tex;
    int             nsubs;
    u8              vn[MAX_MVERTS];                         /* each vertex's normal */
    s16             nearl[VIEW_MAX_POLYS];                  /* the polygons across the near plane */
    u32             sxy[VIEW_SUBS * 4];                     /* ...those drawn: their corners */
    u16             sg[VIEW_SUBS * 4], si[VIEW_SUBS];       /* ...colours, and which polygon each was */
    u32             uxy[MAX_MVERTS], su[VIEW_SUBS * 4];     /* (the bob on: the places before it, to keep) */
    view_turn       tn;
#ifdef FIGHT_BENCH
    u32             tph = frt_read();
#endif
#ifdef VIEW_CHECK
#if VIEW_CHECK == 2
    int             kept_n = -1;                            /* (the kept commands) */
#endif
    u32             cmxy[MAX_MVERTS];                       /* (the vertices, as vverts_asm had them) */
    s32             cmz[MAX_MVERTS];
    u8              cmoc[MAX_MVERTS];
#endif

    bool            vz_ok = vz.ok;

    vz.ok = false;                          /* (unless it goes out as kept, below) */
#if defined(VIEW_CHECK) && VIEW_CHECK != 2
    (void)vz_ok;
#endif
    if (vf.stage)
        while (scu_dma0_busy())
            ;                               /* (in any case: the list's to be written over) */
    if (!m || leaf < 0)
        return;
    nv = imin(m->nverts, MAX_MVERTS);
    np = imin(m->npolys, VIEW_MAX_POLYS);
    if (m != gt_m || leaf != gt_leaf || ys != gt_ys)
    {
        model_shade(gt, m->shade + ys * 162, &lv.leaflight[leaf * 4]);
        gt_m = m;
        gt_leaf = leaf;
        gt_ys = ys;
        ++gt_ver;
    }
    f0 = m->frames + (u32)f0i * m->frame_bytes;
    f1 = m->frames + (u32)f1i * m->frame_bytes;
    if (f1 == f0)
        lerp = 0;
#if !defined(VIEW_CHECK) || VIEW_CHECK == 2
    if (view_key.m == m && view_key.gen == view_gen && view_key.f0 == f0i && view_key.f1 == f1i
        && view_key.lerp == lerp)
    {
        /* as last frame: out from what was kept (read into the list by DMA, if it was expected),
           turned by the bob as it is now */
#ifdef VIEW_CHECK
        int             kept_gc = w->gcount;
#endif
        const u8        *kb = view_keep;
        const view_kept *kp;
        const u32       *kxy;
        const u8        *kn;
        u32             txy[MAX_MVERTS * 2], *dw, dw1;
        int             n = 0, room, luts32 = m->nluts > 1 ? 32 : 0;
        view_turn       tn;

        if (vf.stage == 3 && vf.m == m)
        {
            cache_forget(vf.base, vf.rbytes);
            kb = vf.base;
        }
        vf.stage = 0;
        kp = (const view_kept *)kb;
        kxy = (const u32 *)(kb + view_nkeep * sizeof(view_kept));
        kn = (const u8 *)(kxy + view_nkv);
        view_turn_make(&tn);
        colr0 = lut_vram + (u32)m->lut0 * 32;
        dw1 = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | colr0 >> 3;
        dw = (u32 *)vdp_overlay_block(&room);
        {
            /* (what every polygon needs, in locals: the stores through dw could be anything's).
               In place (last frame's went out the same, from the same place): a command a kept
               polygon, one skipped if its texture can't be had; what's there is rewritten only
               where it's changed (the colour tables' place alternates with the lists) */
            const u16   *tslot = x->tex_slot;
            u32         *gst = w->gst, svram = slot_vram, sbytes = slot_bytes, gb = w->gbase >> 3;
            int         tid0 = m->tex_id0, gc = w->gcount, luts4 = luts32 >> 3, k;
            u16         fr = frame;
            s32         bob[3];
            bool        in_place, nxy, ng, whole = true;

            for (k = 0; k < 3; ++k)
                bob[k] = tn.on ? view_bob[k] : 0;
            n = imin(view_nkeep, imin(room, w->gmax - gc));
            in_place = vz_ok && vz.at == dw && vz.gc == gc && vz.kver == view_kver && n == view_nkeep;
            nxy = !in_place || bob[0] != vz.bob[0] || bob[1] != vz.bob[1] || bob[2] != vz.bob[2];
            ng = !in_place || vz.gver != gt_ver;
            if (nxy && tn.on)
            {
                view_turn_n(&tn, kxy, txy, view_nkv);
                kxy = txy;
            }
            vz.at = dw;
            vz.gc = gc;
            for (i = 0; i < n; ++i, ++kp, dw += 8, ++gc)
            {
                int     t = tid0 + kp->tex, sl = tslot[t];
                u32     vram;

                if (sl != 0xFFFF)
                {
                    slot_frame[sl] = fr;
                    vram = svram + (u32)sl * sbytes;
                }
                else
                {
                    s32 v = tex_load(x, t);

                    if (v < 0)
                    {
                        dw[0] = (u32)(0x5000 | VDP1_DISTORTED) << 16;   /* (skipped: all written next frame) */
                        whole = false;
                        continue;
                    }
                    vram = (u32)v;
                }
                if (!in_place)
                {
                    dw[0] = (u32)(0x1000 | VDP1_DISTORTED) << 16;
                    dw[1] = dw1 + (u32)(kp->tex * luts4);     /* (a gun's table is its texture's: --keeplut) */
                }
                if (!in_place || dw[2] >> 16 != vram >> 3)
                    dw[2] = (vram >> 3) << 16 | kp->size;
                if (nxy)
                {
                    dw[3] = kxy[kp->v[0]];
                    dw[4] = kxy[kp->v[1]];
                    dw[5] = kxy[kp->v[2]];
                    dw[6] = kxy[kp->v[3]];
                }
                if (ng)
                {
                    gst[gc * 2] = (u32)gt[kn[kp->v[0]]] << 16 | gt[kn[kp->v[1]]];
                    gst[gc * 2 + 1] = (u32)gt[kn[kp->v[2]]] << 16 | gt[kn[kp->v[3]]];
                }
                dw[7] = (gb + (u32)gc) << 16;
            }
            vz.kver = view_kver;
            vz.gver = gt_ver;
            for (k = 0; k < 3; ++k)
                vz.bob[k] = bob[k];
            vz.ok = whole && n == view_nkeep;
            w->gcount = gc;
        }
        vdp_overlay_add(n);
        x->st.mpolys += n;
#if defined(VIEW_CHECK)
        /* (OPT=-DVIEW_CHECK=2: kept, then drawn in full as well: the same?) */
        kept_n = imin(n, OVL_CHECK);
        memcpy(chk_kept, dw - n * 8, (u32)kept_n * 32);
        vdp_overlay_add(-n);
        w->gcount = kept_gc;
        memcpy(chk_kgc, w->gst + kept_gc * 2, (u32)kept_n * 8);
#else
        VPH(3);
        return;
#endif
    }
#endif
#if VIEW_CHECK == 2
    if (kept_n >= 0)
    {
        /* (the kept drawing stays, as it went out: the next frame's goes out in place over it) */
        ck_key.m = view_key.m;
        ck_key.gen = view_key.gen;
        ck_key.f0 = view_key.f0;
        ck_key.f1 = view_key.f1;
        ck_key.lerp = view_key.lerp;
        ck_nkeep = view_nkeep;
        ck_nkv = view_nkv;
    }
#endif
    view_key.m = NULL;
    view_nkeep = 0;
    {
        /* the records and frames, fetched (view_fetch_start); or not, copied now (it was to be
           kept, but the game's tick fired) */
        u32         rb = view_rbytes(m);
        const u8    *pa = f0 + 24, *pb = f1 + 24;
        u8          *base;

        if ((vf.stage == 1 || vf.stage == 2) && vf.m == m)
        {
            base = vf.base;
            cache_forget(base, rb + (vf.stage == 2 ? vf.fbytes : 0));
            if (vf.stage == 2 && vf.f0 == f0i && vf.f1 == f1i)
            {
                pa = base + rb;
                pb = f1i == f0i ? pa : f1i == f0i + 1 ? pa + m->frame_bytes : pb;
            }
        }
        else if ((base = view_scratch(w, rb)))
            memcpy(base, m->polys, rb);
        else
            return;
        vf.stage = 0;
        polys = (q_mpoly *)base;
        tex = (const q_mtex *)(base + ((const u8 *)m->tex - (const u8 *)m->polys));
        nsubs = imin(VIEW_SUBS, (int)(((const u8 *)m->tex - (const u8 *)m->polys) / 12) - np);
        /* the vertices (src/mdraw.s), then each one's colour by its normal */
        view_blend(&va, f0, f1, lerp, pa, pb);
    }
    va.zmin = 0x7FFFFFFF;
    va.zmax = -0x7FFFFFFF;
    va.mxy = x->mxy;
    va.mz = x->mz;
    va.moc = x->moc;
    va.vn = vn;
    va.n = nv;
    va.nofs = lerp < FIX(0.5) ? 3 : 3 + (s32)(va.pb - va.pa);
    if (vd.dsp >= 0 && vd.m == m && vd.f0 == f0i && vd.f1 == f1i && vd.lerp == lerp)
    {
        /* the DSP's (view_to_dsp): wait for them if they're not there, forget what the cache
           had there, and onto the screen as a monster's; the normals from the frames */
        const s32   *out = dspm_blk(vd.blk);
        const u8    *pn = va.pa + va.nofs;
        mverts_args ma;

        while (*(volatile u32 *)UNCACHED(&dspm_count) <= (u32)vd.dsp)
            ;
        cache_forget(out, (u32)((nv + 15) >> 4) * 192);
        ma.out = out;
        ma.mxy = x->mxy;
        ma.mz = x->mz;
        ma.moc = x->moc;
        ma.n = nv;
        ma.zmin = 0x7FFFFFFF;
        ma.zmax = -0x7FFFFFFF;
        mverts_asm(&ma);
        zmin = ma.zmin;
        zmax = ma.zmax;
        for (i = 0; i < nv; ++i)
        {
            u32 n = pn[i * 4];

            n = n < 162 ? n : 0;
            vn[i] = (u8)n;
            x->mg[i] = gt[n];
        }
#if defined(VIEW_CHECK) && VIEW_CHECK == 3
        {
            /* (OPT=-DVIEW_CHECK=3: vverts_asm's as well: how far off are the DSP's?) */
            u32 dxy[MAX_MVERTS];
            u8  doc[MAX_MVERTS];

            memcpy(dxy, x->mxy, (u32)nv * 4);
            memcpy(doc, x->moc, (u32)nv);
            vverts_asm(&va);
            for (i = 0; i < nv; ++i)
            {
                ++view_checks[3];
                if ((doc[i] ^ x->moc[i]) & OC_NEAR)
                    ++view_checks[6];       /* (either side of the near plane) */
                else if (!(doc[i] & OC_NEAR) && dxy[i] != x->mxy[i])
                {
                    ++view_checks[4];
                    view_checks[5] = (u32)imax((s32)view_checks[5], imax(iabs(XY_X(dxy[i]) - XY_X(x->mxy[i])),
                                                                        iabs(XY_Y(dxy[i]) - XY_Y(x->mxy[i]))));
                }
            }
            mverts_asm(&ma);                /* (the DSP's drawn) */
        }
#endif
    }
    else
    {
        vverts_asm(&va);
        zmin = va.zmin;
        zmax = va.zmax;
        for (i = 0; i < nv; ++i)
        {
            u32 n = vn[i] < 162 ? vn[i] : 0;

            vn[i] = (u8)n;
            x->mg[i] = gt[n];
        }
    }
    vd.dsp = -1;
    /* the bob; and nothing dropped for being off a side of the screen: what's kept goes out
       again as the bob changes (VDP1 clips) */
    view_turn_make(&tn);
    if (tn.on)
    {
        memcpy(uxy, x->mxy, (u32)nv * 4);
        view_turn_n(&tn, uxy, x->mxy, nv);  /* (those behind the near plane too: not used) */
    }
    for (i = 0; i < nv; ++i)
        if (!(x->moc[i] & OC_NEAR))
            x->moc[i] = 0;
#ifdef VIEW_CHECK
    memcpy(cmxy, x->mxy, (u32)nv * 4);
    memcpy(cmz, x->mz, (u32)nv * 4);
    memcpy(cmoc, x->moc, (u32)nv);
#endif
    VPH(0);
    if (zmax < zmin)
        return;
    inv = (s32)(((u32)(MBUCKETS - 1) << 16) / (u32)imax((zmax - zmin) >> 16, 1));
    for (b = 0; b < MBUCKETS; ++b)
        x->mhead[b] = -1;
    /* facing you (the other way round from the monsters'), by depth; those across the near
       plane listed */
    {
        mpolys_args a;

        a.polys = polys;
        a.n = np;
        a.moc = x->moc;
        a.mxy = x->mxy;
        a.mz = x->mz;
        a.mhead = x->mhead;
        a.zmin = zmin;
        a.inv = inv;
        a.sign = -1;
        a.nearl = nearl;
        a.nnear = 0;
        mpolys_asm(&a);
        nnear = a.nnear;
    }
    /* those across the near plane: cut, tested, and into their buckets where they'd be in
       polygon order (each bucket's list runs by polygon, highest first), with corners and
       records of their own */
    for (k = ns = 0; k < nnear && ns < nsubs; ++k)
    {
        const q_mpoly   *p = &polys[nearl[k]];
        q_mpoly         *q = &polys[np + ns];
        int             j, e, prev, pi = nearl[k];
        u32             *xy = &sxy[ns * 4];
        s32             cross, z;

        if (!view_poly_xy(x, &va, p, xy, tn.on ? uxy : x->mxy))
            continue;
        if (tn.on)
        {
            memcpy(&su[ns * 4], xy, 16);
            view_turn_n(&tn, xy, xy, 4);
        }
        cross = (XY_X(xy[1]) - XY_X(xy[0])) * (XY_Y(xy[2]) - XY_Y(xy[0]))
              - (XY_Y(xy[1]) - XY_Y(xy[0])) * (XY_X(xy[2]) - XY_X(xy[0]));
        if (cross == 0 && !(p->flags & 1))
            cross = (XY_X(xy[2]) - XY_X(xy[0])) * (XY_Y(xy[3]) - XY_Y(xy[0]))
                  - (XY_Y(xy[2]) - XY_Y(xy[0])) * (XY_X(xy[3]) - XY_X(xy[0]));
        if (cross * VIEW_FRONT <= 0)
            continue;
        z = (x->mz[p->v[0]] >> 1) + (x->mz[p->v[2]] >> 1);
        b = z <= zmin ? 0 : iclamp((((z - zmin) >> 16) * inv) >> 16, 0, MBUCKETS - 1);
        for (j = 0; j < 4; ++j)
        {
            sg[ns * 4 + j] = x->mg[p->v[j]];
            q->v[j] = (u16)(MAX_MVERTS + ns * 4 + j);
        }
        q->tex = p->tex;
        q->flags = p->flags;
        si[ns] = (u16)pi;
        e = np + ns;
        for (prev = -1, j = x->mhead[b]; j >= 0 && (j < np ? j : si[j - np]) > pi; j = x->mnext[j])
            prev = j;
        x->mnext[e] = (s16)j;
        if (prev < 0)
            x->mhead[b] = (s16)e;
        else
            x->mnext[prev] = (s16)e;
        ++ns;
    }
    if (ns)
    {
        /* their corners: past mxy's and mg's own (mz, moc and gtab are done with) */
        u32 *cxy = (u32 *)((u8 *)x + __builtin_offsetof(r_ctx, mz));
        u16 *cg = (u16 *)((u8 *)x + __builtin_offsetof(r_ctx, moc));

        memcpy(cxy, sxy, (u32)ns * 16);
        memcpy(cg, sg, (u32)ns * 8);
    }
    VPH(1);
    /* out, farthest first (appended: the overlay list is drawn in order) */
    colr0 = lut_vram + (u32)m->lut0 * 32;
    {
        mcmds_args  a;
        int         room;

        a.polys = polys;
        a.tex = tex;
        a.mnext = x->mnext;
        a.mxy = x->mxy;
        a.mg = x->mg;
        a.tslot = x->tex_slot;
        a.sframe = slot_frame;
        a.cmds = (u32 *)vdp_overlay_block(&room);
        a.gst = w->gst;
        a.tid0 = m->tex_id0;
        a.frame = frame;
        a.svram = slot_vram;
        a.sbytes = slot_bytes;
        a.lb = 0;                           /* (the overlay's links are made at the submit) */
        a.dw1 = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | lut_vram >> 3;
        a.slut = slot_lut;
        a.gmax = w->gmax;
        a.gb = w->gbase >> 3;
        a.fifo = true;
        a.x = x;
        a.cnt = 0;
        a.head = -1;
        a.tail = -1;
        a.gc = w->gcount;
        a.drop = 0;
        a.mhead = x->mhead;
        a.wcmds = room;
        mcmds_asm(&a);
#if defined(VIEW_CHECK) && VIEW_CHECK < 3
        {
            /* (the C again from the same start: the same commands and colours?) */
            u32         *cmds = chk_cmds, *gcs = chk_gcs, *cw = a.cmds;
            int         n = imin(a.cnt, OVL_CHECK), ng = imin(a.gc - w->gcount, OVL_CHECK), rn, d = 0, gc0 = w->gcount;

            vverts_args rva = va;
            s16         lists[MBUCKETS + MAX_MPOLYS];   /* (the C sorts its own: the kept drawing's made from these) */

            memcpy(lists, x->mhead, sizeof(lists));
            memcpy(cmds, cw, (u32)n * 32);
            memcpy(gcs, w->gst + gc0 * 2, (u32)ng * 8);
            rva.pa = f0 + 24;               /* (the C reads the cart's: the DMA's copy checked too) */
            rva.pb = f1 + 24;
            draw_viewmodel_ref(x, m, &rva, f0, f1, lerp, gt);
            for (i = 0; i < nv; ++i)
                d += x->mz[i] != cmz[i] || x->moc[i] != cmoc[i] || (!(cmoc[i] & OC_NEAR) && x->mxy[i] != cmxy[i]);
            vdp_overlay_block(&rn);
            rn = room - rn;                 /* (the C's commands) */
            ++view_checks[0];
            view_checks[1] += (u32)n;
            for (k = 0; k < n && k < rn; ++k)
                d += (cw[k * 8] >> 16) != (cmds[k * 8] >> 16) || memcmp(&cw[k * 8 + 1], &cmds[k * 8 + 1], 28)
                     || memcmp(&w->gst[(gc0 + k) * 2], &gcs[k * 2], 8);
            view_checks[2] += (u32)d + (u32)iabs(rn - n) + (u32)(w->gcount - gc0 != ng);
            vdp_overlay_add(-rn);
            w->gcount = gc0;
            memcpy(x->mhead, lists, sizeof(lists));
            memcpy(cw, cmds, (u32)n * 32);
            memcpy(w->gst + gc0 * 2, gcs, (u32)ng * 8);
        }
#if VIEW_CHECK == 2
        if (kept_n >= 0)
        {
            /* (the kept drawing went out as this does? Each polygon found by its texture (the
               gun's are one a polygon): its corners and colours the same. As the bob turns it,
               a polygon edge-on can face the other way: those counted apart) */
            int fn = imin(a.cnt, OVL_CHECK), found = 0, j;

            view_checks[1] += (u32)kept_n;
            for (k = 0; k < kept_n; ++k)
            {
                for (j = 0; j < fn && (a.cmds[j * 8 + 2] >> 16) != (chk_kept[k * 8 + 2] >> 16); ++j)
                    ;
                if (j == fn)
                    continue;
                ++found;
                view_checks[2] += memcmp(&a.cmds[j * 8 + 1], &chk_kept[k * 8 + 1], 24)
                                  || memcmp(&w->gst[(w->gcount + j) * 2], &chk_kgc[k * 2], 8);
            }
            view_checks[3] += (u32)(kept_n - found + fn - found);
            /* (the kept drawing goes out, not this: put back where it was) */
            memcpy(a.cmds, chk_kept, (u32)kept_n * 32);
            memcpy(w->gst + w->gcount * 2, chk_kgc, (u32)kept_n * 8);
            a.cnt = kept_n;
            a.gc = w->gcount + kept_n;
        }
#endif
#endif
        vdp_overlay_add(a.cnt);
        w->gcount = a.gc;
        x->st.mpolys += a.cnt;
        x->st.dropped += a.drop;
    }
#if VIEW_CHECK == 2
    if (kept_n >= 0)
    {
        view_key.m = ck_key.m;
        view_key.gen = ck_key.gen;
        view_key.f0 = ck_key.f0;
        view_key.f1 = ck_key.f1;
        view_key.lerp = ck_key.lerp;
        view_nkeep = ck_nkeep;
        view_nkv = ck_nkv;
        VPH(2);
        return;
    }
#endif
    /* a still frame (the gun at rest): kept, to go out again as it is next frame. Written to
       the cart (once, as it comes to rest): the polygons, their corners renumbered among the
       vertices they use; those vertices' places (before the bob) and normals */
    if (f1 == f0)
    {
        view_kept   *kp = (view_kept *)view_keep;
        const u32   *ux = tn.on ? uxy : x->mxy, *us = tn.on ? su : sxy;
        u32         fxy[MAX_MVERTS * 2];
        u8          fnn[MAX_MVERTS * 2];
        u16         map[MAX_MVERTS];
        int         nk = 0;

        memset(map, 0xFF, sizeof(map));
        for (b = MBUCKETS - 1; b >= 0; --b)
            for (i = x->mhead[b]; i >= 0 && view_nkeep < MAX_MPOLYS; i = x->mnext[i], ++kp, ++view_nkeep)
            {
                const q_mpoly   *p = &polys[i];
                const q_mtex    *mt = &tex[p->tex];
                int             j;

                kp->tex = p->tex;
                kp->size = (u16)(((mt->w >> 3) << 8) | mt->h);
                for (j = 0; j < 4; ++j)
                    if (i < np)
                    {
                        int v = p->v[j];

                        if (map[v] == 0xFFFF)
                        {
                            map[v] = (u16)nk;
                            fxy[nk] = ux[v];
                            fnn[nk++] = vn[v];
                        }
                        kp->v[j] = map[v];
                    }
                    else
                    {
                        fxy[nk] = us[(i - np) * 4 + j];         /* (a cut one's corner: its own) */
                        fnn[nk] = vn[polys[si[i - np]].v[j]];
                        kp->v[j] = (u16)nk++;
                    }
            }
        view_nkv = nk;
        ++view_kver;
        memcpy(view_keep + view_nkeep * sizeof(view_kept), fxy, (u32)nk * 4);
        memcpy(view_keep + view_nkeep * sizeof(view_kept) + nk * 4, fnn, (u32)nk);
        view_key.m = m;
        view_key.gen = view_gen;
        view_key.f0 = f0i;
        view_key.f1 = f1i;
        view_key.lerp = lerp;
    }
    VPH(2);
}

/* a slot's new gun (src/view.c): its textures' old copies forgotten (only the master draws
   the gun, so only its part of the cache has them). The slot's last gun hasn't been drawn for
   a while (view.c sees to it): nothing in flight uses them */
void                r_view_slot(int slot)
{
    const q_mdl     *md = &models[MDL_VIEW0 + slot];
    r_ctx           *x = &ctx[0];
    int             t;

    ++view_gen;                             /* (what draw_viewmodel kept is another gun's) */
    for (t = md->tex_id0; t < md->tex_id0 + VIEW_MAX_TEX; ++t)
    {
        int s = x->tex_slot[t];

        if (s != 0xFFFF)
        {
            slot_tex[s] = SLOT_NONE;
            x->tex_slot[t] = 0xFFFF;
        }
    }
}

/* the gun to be drawn next (src/view.c): its colour tables into VRAM, where the guns share
   one set. The last gun drawn from there is gone from the frames in flight (view.c sees to it) */
__attribute__((cold)) void r_view_luts(int slot)
{
    const q_mdl     *md = &models[MDL_VIEW0 + slot];
    u32             n = (u32)(md->nskins * md->nluts) * 16, k, i, m;
    u16             *buf = (u16 *)ctx[0].grid;  /* (brighter: through here, a piece at a time; the master's
                                                   not drawing now) */

    if (!md->loaded)
        return;
    while (scu_dma0_busy())
        ;                                   /* (the gun's fetch may be under way: not cut short) */
    if (!r_bright)
        scu_dma0((void *)(VDP1_VRAM + lut_vram + (u32)md->lut0 * 32), md->luts, n * 2, true);
    else
        for (k = 0; k < n; k += m)
        {
            m = n - k < sizeof(ctx[0].grid) / 2 ? n - k : sizeof(ctx[0].grid) / 2;
            for (i = 0; i < m; ++i)
                buf[i] = r_gamma(md->luts[k + i]);
            scu_dma0((void *)(VDP1_VRAM + lut_vram + (u32)md->lut0 * 32 + k * 2), buf, m * 2, true);
            while (scu_dma0_busy())
                ;
        }
    while (scu_dma0_busy())
        ;
}

static void         sky_colours(void);

/* the options' brightness: every colour table again (the level's, the models', the gun's,
   the sky's, the status bar's) */
__attribute__((cold)) void r_set_bright(int b)
{
    int             c;

    r_bright = b;
    luts_copy(lut_vram, lv.luts, (u32)lv.nluts * 16);
    for (c = 0; c < nmodels_loaded; ++c)
    {
        const q_mdl *md = &models[c];

        if (md->loaded)
            luts_copy(lut_vram + (u32)md->lut0 * 32, md->luts,
                      (u32)(md->lutmap ? md->nluts : md->nskins * md->nluts) * 16);
    }
    view_relut();
    sky_colours();
    hud_palette();
}

/* Sharing the list. The slave starts as soon as the walk starts, taking
   faces from the front (nearest) as they're listed, pushed into bucket 1 (so
   drawn back to front). When its walk is done the master takes them from the
   back (farthest), appended into bucket 0 (drawn first, in order). They meet
   in the middle: each claims an index after checking the other hasn't passed
   it, so at worst the one face where they meet is drawn by both, which
   painter's order doesn't mind. These are read uncached by the other CPU. */
typedef struct
{
    volatile int    published;              /* listed so far (the walk) */
    volatile int    lo;                     /* the slave's next */
    volatile int    hi;                     /* the master's last taken (from the back) */
    volatile int    walk_done;
}                   t_share;

static t_share      share;
#define SHARE       ((t_share *)UNCACHED(&share))

/* ---- the BSP walk: the visible faces, front to back ---- */

/* can any of a leaf's faces be seen from the camera's cluster (facevis)? */
static bool         leaf_seen(const q_leaf *leaf)
{
    const u16       *m = &lv.marks[leaf->firstmark];
    int             i;

    for (i = leaf->nummark; i; --i, ++m)
        if (face_vis[*m >> 3] & bitm[*m & 7])
            return true;
    return !leaf->nummark;
}

static inline void  list_face(int i, int model)
{
    if (nvis < MAX_VIS)
    {
        vis_model[nvis] = (u8)model;
        vis_faces[nvis++] = (u16)i;
        SHARE->published = nvis;
    }
}

/* a brush model's own BSP, front to back: every face towards the camera.
   cp: the camera relative to the model (where it's moved to) */
static void         walk_model(int n, int model, const s32 *cp, u8 mask)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             d;
    int             side, i;

    if (n < 0)
        return;
    node = &lv.nodes[n];
    {
        s16 mn[3], mx[3];

        for (i = 0; i < 3; ++i)
        {
            s16 o = (s16)(mover_ofs[model][i] >> 16);

            mn[i] = (s16)(node->mins[i] + o - 1);
            mx[i] = (s16)(node->maxs[i] + o + 1);
        }
        if (mask && cull_box(mn, mx, &mask))
            return;
    }
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
        d = cp[pl->type] - pl->dist;
    else
        d = fmul(cp[0], pl->n[0]) + fmul(cp[1], pl->n[1]) + fmul(cp[2], pl->n[2]) - pl->dist;
    side = d < 0;
    walk_model(node->child[side], model, cp, mask);
    for (i = node->firstface; i < node->firstface + node->numfaces; ++i)
        if (!(face_back[(u32)i >> 3] & bitm[i & 7]) == !side)
            list_face(i, model);
    walk_model(node->child[!side], model, cp, mask);
}

#ifdef R_PROFILE
int                 wk_nodes, wk_leaves, wk_ftests, wk_models;
#endif

/* is a node or leaf in the camera's cluster's PVS? (asked before walking into
   it: a call's eight registers saved and restored for nothing, otherwise) */
static inline bool  in_pvs(int n)
{
    return n >= 0 ? node_vis[n] == visframe : leaf_vis[-(n + 1)] == visframe;
}

/* what a leaf in view holds besides its faces: entities (unless none of the
   leaf can be seen), sprites, brush models (walked on their own). The walk in
   assembly calls this for a leaf with any. */
void                walk_leaf_extra(int l, int mask)
{
    int             i;

    if (leaf_ent[l] >= 0 && !leaf_seen(&lv.leafs[l]))
        ;                                   /* none of the leaf can be seen: nor can what's in it */
    else for (i = leaf_ent[l]; i >= 0; i = ent_next[i])
        if (nvis < MAX_VIS)
        {
            vis_model[nvis] = ENTITY;
            vis_faces[nvis++] = (u16)i;
            SHARE->published = nvis;
        }
    for (i = leaf_spr[l]; i >= 0; i = spr_next[i])
        if (nvis < MAX_VIS)
        {
            vis_model[nvis] = SPRITE;
            vis_faces[nvis++] = (u16)i;
            SHARE->published = nvis;
        }
    for (i = leaf_model[l]; i >= 0; i = model_next[i])
    {
        s32 cp[3];

        if (!mover_live(i))
            continue;
        cp[0] = cam.pos[0] - mover_ofs[i][0];
        cp[1] = cam.pos[1] - mover_ofs[i][1];
        cp[2] = cam.pos[2] - mover_ofs[i][2];
        PROF(++wk_models);
        walk_model(lv.models[i].headnode, i, cp, (u8)mask);
    }
}

/* src/walk.s: the walk in assembly. Its context (the layout it reads) */
typedef struct
{
    const q_node    *nodes;
    const q_plane   *planes;
    const q_leaf    *leafs;
    const u16       *node_vis, *leaf_vis;
    u32             frame;
    const u8        *face_vis, *face_back;
    s32             cam[3];
    const u8        *bitm;
    u16             *list;
    u8              *list_model;
    int             *nlist;
    volatile int    *published;
    struct { u8 far[3], near[3], pad[2]; s32 n[3], d; } fr[4];     /* the box's corners: offsets from its mins */
    const s16       *leaf_ent, *leaf_spr, *leaf_model;
    void            (*extra)(int, int);
}                   walk_ctx;
void                walk_asm(int n, int mask, const walk_ctx *w);
static walk_ctx     wctx;
bool                r_walk_asm = true;
#ifdef WALK_CHECK
int                 walk_diff, walk_len, walk_clen, walk_first, walk_what[4], walk_total, walk_frames;
#endif

static void         walk_setup(void)
{
    int             i, k;

    wctx.nodes = lv.nodes;
    wctx.planes = lv.planes;
    wctx.leafs = lv.leafs;
    wctx.node_vis = node_vis;
    wctx.leaf_vis = leaf_vis;
    wctx.frame = visframe;
    wctx.face_vis = face_vis;
    wctx.face_back = face_back;
    wctx.cam[0] = cam.pos[0]; wctx.cam[1] = cam.pos[1]; wctx.cam[2] = cam.pos[2];
    wctx.bitm = bitm;
    wctx.list = vis_faces;
    wctx.list_model = vis_model;
    wctx.nlist = &nvis;
    wctx.published = &SHARE->published;
    for (i = 0; i < 4; ++i)
    {
        for (k = 0; k < 3; ++k)
        {
            u8 neg = fr_n[i][k] < 0;

            wctx.fr[i].far[k] = (u8)(2 * k + (neg ? 0 : 6));    /* mins, or maxs 6 bytes on */
            wctx.fr[i].near[k] = (u8)(2 * k + (neg ? 6 : 0));
            wctx.fr[i].n[k] = fr_n[i][k];
        }
        wctx.fr[i].d = fr_d[i];
    }
    wctx.leaf_ent = leaf_ent;
    wctx.leaf_spr = leaf_spr;
    wctx.leaf_model = leaf_model;
    wctx.extra = walk_leaf_extra;
}

static void         walk(int n, u8 mask)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             d;
    int             side, i;

    if (n < 0)
    {
        PROF(++wk_leaves);
        const q_leaf *leaf = &lv.leafs[-(n + 1)];
        const u16    *m;

        if (leaf_vis[-(n + 1)] != visframe || (mask && cull_box(leaf->mins, leaf->maxs, &mask)))
            return;
        (void)m;
        walk_leaf_extra(-(n + 1), mask);
        return;
    }
    node = &lv.nodes[n];
    PROF(++wk_nodes);
    if (node_vis[n] != visframe || (mask && cull_box(node->mins, node->maxs, &mask)))
        return;
    PROF(wk_ftests += node->numfaces);
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
        d = cam.pos[pl->type] - pl->dist;
    else
        d = fmul(cam.pos[0], pl->n[0]) + fmul(cam.pos[1], pl->n[1]) + fmul(cam.pos[2], pl->n[2]) - pl->dist;
    side = d < 0;
    if (in_pvs(node->child[side]))
        walk(node->child[side], mask);
    for (i = node->firstface; i < node->firstface + node->numfaces; ++i)
    {
        /* visible from here (facevis) and facing this way: two bits in high work RAM (the
           faces themselves are in low: a miss each) */
        u32 k = (u32)i >> 3, b = bitm[i & 7];

        if (face_vis[k] & b && !(face_back[k] & b) == !side)
            list_face(i, 0);
    }
    if (in_pvs(node->child[!side]))
        walk(node->child[!side], mask);
}

/* a glowing blob: a half-transparent halo round a solid core */
static __attribute__((noinline)) void draw_sprite(r_ctx *x, int si)
{
    const q_sprite  *sp = &r_sprites[si];
    vdp_writer      *w = x->w;
    s32             d[3], r, cx, cy, s;
    v3              v;
    int             k;

    (void)w;
    d[0] = sp->pos[0] - cam.pos[0];
    d[1] = sp->pos[1] - cam.pos[1];
    d[2] = sp->pos[2] - cam.pos[2];
    to_view(d, &v);
    if (v.z < NEAR_Z * 2)
        return;
    divu_start(FOCAL, 0, v.z);
    r = divu_result();
    cx = CX + (fmul(v.x, r) >> 16);
    cy = CY - (fmul(v.y, r) >> 16);
    s = imax(fmul(sp->size, r) >> 16, 1);
    if (cx + 2 * s < 0 || cx - 2 * s >= SCREEN_W || cy + 2 * s < 0 || cy - 2 * s >= SCREEN_H || s > 400)
        return;
    for (k = 0; k < 2; ++k)
    {
        /* a round glow, tinted by Gouraud (the texture's white; 16 leaves it), half-transparent:
           the core goes on top of the halo */
        bool        halo = x->fifo ? k == 0 : k == 1;
        u32         link, *cw = cmd_alloc(x, &link);
        vdp1_cmd    *c = (vdp1_cmd *)cw;
        s32         e = halo ? s * 2 : s;
        u16         col = halo ? sp->halo : sp->color, g;

        if (!c)
            return;
        g = (u16)(0x8000 | (col & 0x7FFF));     /* the colour's 5 bits a channel as the offset: 31 brightens, 0 darkens */
        c->ctrl = 0x1000 | VDP1_DISTORTED;
        c->link = (u16)link;
        c->pmod = (u16)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD | PMOD_HALF_TRANS);
        c->colr = (u16)(glow_lut >> 3);
        c->srca = (u16)(glow_vram >> 3);
        c->size = (4 << 8) | 32;
        c->xa = (s16)(cx - e); c->ya = (s16)(cy - e);
        c->xb = (s16)(cx + e); c->yb = (s16)(cy - e);
        c->xc = (s16)(cx + e); c->yc = (s16)(cy + e);
        c->xd = (s16)(cx - e); c->yd = (s16)(cy + e);
        c->grda = vdp_gouraud_w(x->w, g, g, g, g);
        ++x->st.cells;
    }
}

/* A model: its two frames blended, turned by its yaw and placed, into view
   space and onto the screen; lit by its leaf's light and Quake's shading by
   normal; back faces dropped; its polygons sorted by depth among themselves
   (32 buckets over its own depth range) and put in nearest first, so the
   bucket's last-in-first-drawn order paints them back to front. */
/* a model's two frames into view space: view = A b + C for each (b its packed
   bytes). false: all of it's off the screen */
typedef struct { s32 A0[3][3], A1[3][3], C0[3], C1[3]; } model_xform;

static bool         model_xf(const q_entity *e, model_xform *o)
{
    const q_mdl     *m = e->mdl;
    const s32       *ax[3];
    s32             M[3][3], T[3], d[3], c = fcos(e->yaw), sn = fsin(e->yaw);
    const s32       *h0 = (const s32 *)(m->frames + (u32)e->oldframe * m->frame_bytes);
    const s32       *h1 = (const s32 *)(m->frames + (u32)e->frame * m->frame_bytes);
    s32             (*A0)[3] = o->A0, (*A1)[3] = o->A1, *C0 = o->C0, *C1 = o->C1;
    int             i, k;

    ax[0] = cam.right;
    ax[1] = cam.up;
    ax[2] = cam.fwd;
    d[0] = e->origin[0] - cam.pos[0];
    d[1] = e->origin[1] - cam.pos[1];
    d[2] = e->origin[2] - cam.pos[2];
    if (e->pitch)
    {
        /* turned by yaw, then tipped by pitch (nose down positive, as Quake) */
        s32 cp = fcos(e->pitch), sp = fsin(e->pitch), col[3][3];

        col[0][0] = fmul(c, cp); col[0][1] = fmul(sn, cp); col[0][2] = -sp;
        col[1][0] = -sn;         col[1][1] = c;            col[1][2] = 0;
        col[2][0] = fmul(c, sp); col[2][1] = fmul(sn, sp); col[2][2] = cp;
        for (k = 0; k < 3; ++k)
            for (i = 0; i < 3; ++i)
                M[k][i] = fmul(ax[k][0], col[i][0]) + fmul(ax[k][1], col[i][1]) + fmul(ax[k][2], col[i][2]);
    }
    for (k = 0; k < 3; ++k)
    {
        if (!e->pitch)
        {
            M[k][0] = fmul(ax[k][0], c) + fmul(ax[k][1], sn);
            M[k][1] = fmul(ax[k][1], c) - fmul(ax[k][0], sn);
            M[k][2] = ax[k][2];
        }
        T[k] = fmul(ax[k][0], d[0]) + fmul(ax[k][1], d[1]) + fmul(ax[k][2], d[2]);
    }
    /* per frame: view = A b + C, with A = M * the frame's scale, C = M * its translation + T */
    for (k = 0; k < 3; ++k)
    {
        for (i = 0; i < 3; ++i)
        {
            A0[k][i] = fmul(M[k][i], h0[i]);
            A1[k][i] = fmul(M[k][i], h1[i]);
        }
        C0[k] = fmul(M[k][0], h0[3]) + fmul(M[k][1], h0[4]) + fmul(M[k][2], h0[5]) + T[k];
        C1[k] = fmul(M[k][0], h1[3]) + fmul(M[k][1], h1[4]) + fmul(M[k][2], h1[5]) + T[k];
    }
    /* all of it (within 48 units of its origin) off screen? */
    return !(T[2] < -FIX(48) || T[0] - FIX(48) > fmul(T[2] + FIX(48), kx) || T[0] + FIX(48) < -fmul(T[2] + FIX(48), kx)
             || T[1] - FIX(48) > fmul(T[2] + FIX(48), ky) || T[1] + FIX(48) < -fmul(T[2] + FIX(48), ky));
}

/* far away: the coarse mesh (its polygons, and only the first nfverts vertices: models_to_dsp
   and draw_model both ask) */
static inline bool  model_far(const q_entity *e)
{
    s32             dx = (e->origin[0] - cam.pos[0]) >> 16, dy = (e->origin[1] - cam.pos[1]) >> 16;

    return e->mdl->nfpolys && dx * dx + dy * dy > r_model_far * r_model_far;
}

/* the frame's models for the DSP: those in leaves the view can see, and not
   all off the screen, weighted for their blends; started at once */
static int          rs_mdsp;                /* (stats: models the DSP was given this frame) */

static void         models_to_dsp(void)
{
    u32             *st = dspm_stream;
    int             i, k, nm = 0, nb = 0, nc = 0;
#ifdef DSP_WALLS
    bool            walls;
#endif
#ifdef DSP_LIGHT
    int             nlj = 0;
#endif
    s16             order[MAX_ENTITIES];
    int             no = 0, oi;
#ifdef DSP_NEAR
    u32             key[MAX_ENTITIES];
#endif

    rs_mdsp = 0;
    for (i = 0; i < nents; ++i)
        ent_dsp[i] = -1;
    if (!r_use_dsp)
        return;
#if defined(DSP_WALLS) && defined(FIGHT_BENCH)
    if (dsp_busy())
    {
        u32 t = frt_read();                 /* (the walls' job still going: how often, how long) */

        dsp_wait();
        ++dw_stat[4];
        dw_stat[5] += frt_to_us((frt_read() - t) & 0xFFFF);
    }
#endif
    dsp_wait();                             /* (last frame's list, if a model it had wasn't drawn) */
#ifdef DSP_WALLS
    {
#ifdef FIGHT_BENCH
        u32 t = frt_read();
#endif
        walls = dw_frame();                 /* (the walls' job: after the models', or alone) */
#ifdef FIGHT_BENCH
        dw_stat[3] += frt_to_us((frt_read() - t) & 0xFFFF);
#endif
    }
#endif
#ifdef DSP_NEAR
    /* (OPT=-DDSP_NEAR) nearest first: the slave draws from the front, so it wants
       those first */
    for (i = 0; i < nents; ++i)
    {
        s32         dx, dy, dz;
        u32         d2;

        if (ent_leaf[i] < 0 || leaf_vis[ent_leaf[i]] != visframe || !ents[i].mdl)
            continue;
        dx = (ents[i].origin[0] - cam.pos[0]) >> 16;
        dy = (ents[i].origin[1] - cam.pos[1]) >> 16;
        dz = (ents[i].origin[2] - cam.pos[2]) >> 16;
        d2 = (u32)(dx * dx + dy * dy + dz * dz);
        for (k = no++; k > 0 && key[k - 1] > d2; --k)
        {
            key[k] = key[k - 1];
            order[k] = order[k - 1];
        }
        key[k] = d2;
        order[k] = (s16)i;
    }
#else
    /* the big ones first (the monsters: the CPU's way is dear for them), then the small; only
       those that may be on the screen (a sphere of 64 units against the view's sides, as
       ents_light_dyn's: one drawn after all is the CPU's) */
    for (k = 0; k < 2; ++k)
        for (i = 0; i < nents; ++i)
        {
            const q_entity  *e = &ents[i];
            s32             d0, d1, d2, vx, vy, vz;

            if (ent_leaf[i] < 0 || leaf_vis[ent_leaf[i]] != visframe || !e->mdl || !k != (e->mdl->nverts >= DSPM_BIG))
                continue;
            d0 = e->origin[0] - cam.pos[0];
            d1 = e->origin[1] - cam.pos[1];
            d2 = e->origin[2] - cam.pos[2];
            vx = fmul(d0, cam.right[0]) + fmul(d1, cam.right[1]) + fmul(d2, cam.right[2]);
            vy = fmul(d0, cam.up[0]) + fmul(d1, cam.up[1]) + fmul(d2, cam.up[2]);
            vz = fmul(d0, cam.fwd[0]) + fmul(d1, cam.fwd[1]) + fmul(d2, cam.fwd[2]);
            if (vz < -FIX(64) || iabs(vx) - vz > FIX(91) || iabs(vy) - fmul(vz, FIX(0.7)) > FIX(79))
                continue;
            order[no++] = (s16)i;
        }
#endif
    for (oi = 0; oi < no; ++oi)
    {
        const q_entity  *e = &ents[i = order[oi]];
        const q_mdl     *m = e->mdl;
        model_xform     xf;
        s32             w1 = e->lerp, w0 = FIX(1) - w1;
        int             blocks, b, nb0 = nb, nc0 = nc;

        if (ent_leaf[i] < 0 || leaf_vis[ent_leaf[i]] != visframe || !m)
            continue;
        blocks = (imin(model_far(e) ? m->nfverts : m->nverts, MAX_MVERTS) + 15) >> 4;
        if (nm == DSPM_MODELS)
            break;
        if ((b = dspm_place(blocks, &nb, &nc)) < 0)
            continue;                       /* (no room: the CPU's; a smaller one may fit) */
        if (!model_xf(e, &xf))
        {
            nb = nb0;                       /* (all off the screen: its room back) */
            nc = nc0;
            continue;
        }
        for (k = 0; k < 3; ++k, st += 7)
        {
            st[0] = (u32)(fmul(xf.C0[k], w0) + fmul(xf.C1[k], w1));
            st[1] = (u32)fmul(xf.A0[k][0], w0); st[2] = (u32)fmul(xf.A0[k][1], w0); st[3] = (u32)fmul(xf.A0[k][2], w0);
            st[4] = (u32)fmul(xf.A1[k][0], w1); st[5] = (u32)fmul(xf.A1[k][1], w1); st[6] = (u32)fmul(xf.A1[k][2], w1);
        }
        st[0] = ((u32)(m->frames + (u32)e->oldframe * m->frame_bytes + 24) & 0x07FFFFFF) >> 2;
        st[1] = ((u32)(m->frames + (u32)e->frame * m->frame_bytes + 24) & 0x07FFFFFF) >> 2;
        st[2] = (u32)blocks;
#ifndef DSP_LIGHT
        st[3] = ((u32)dspm_blk(b) & 0x07FFFFFF) >> 2;
#endif
        st += DSPM_HEAD - 21;
        ent_dsp[i] = (s16)nm++;
        ++rs_mdsp;
        ent_blk[i] = (s16)b;
#ifdef DSP_LIGHT
        {
            /* its lighting: a job for each dynamic light near it (draw_model's test and
               direction), all of them or none (then the CPU does it) */
            s32 c = fcos(e->yaw), sn = fsin(e->yaw);
            int li, near = 0;

            ent_ljn[i] = 0;
            st[-1] = 0;                     /* (the header's last word: its jobs) */
            for (li = 0; li < ndl; ++li)
            {
                const q_dlight  *l = &r_dlights[li];
                s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
                s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16;

                near += dx * dx + dy * dy + dz * dz < r * r;
            }
            if (near && nlj + near <= DSPL_JOBS)
            {
                ent_lj0[i] = (s8)nlj;
                ent_ljn[i] = (s8)near;
                st[-1] = (u32)near;
                for (li = 0; li < ndl; ++li)
                {
                    const q_dlight  *l = &r_dlights[li];
                    s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
                    s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16;
                    s32             d2 = dx * dx + dy * dy + dz * dz, f, len;
                    u32             *jb = dspl_jobs + nlj * 9;

                    if (d2 >= r * r)
                        continue;
                    f = ((r * r - d2) * dl[li].inv) >> 8;
                    len = (s32)isqrt((u32)d2) + 1;
                    jb[0] = ((u32)(dspl_out + nlj * DSPL_N) & 0x07FFFFFF) >> 2;
                    jb[1] = 0;
                    jb[2] = (u32)(((dx * c + dy * sn) >> 2) / len * 4);
                    jb[3] = (u32)(((dy * c - dx * sn) >> 2) / len * 4);
                    jb[4] = (u32)((dz << 14) / len * 4);
                    jb[5] = 42600;
                    jb[6] = (u32)(f * 4);
                    jb[7] = (u32)(f * 4 * 5734);
                    jb[8] = (u32)((f * 5734) >> 14);
                    ++nlj;
                }
            }
        }
#endif
    }
    if ((k = view_to_dsp(st, nm, nb)) > 0)
    {
        /* (the gun in your hands, after them) */
        st += DSPM_HEAD;
        ++nm;
        nb += k;
    }
    if (nm)
    {
        *(volatile u32 *)UNCACHED(&dspm_count) = 0;
#ifdef DSP_LIGHT
        *(volatile u32 *)UNCACHED(&dspl_count) = 0;
        dsp_models_lit(dspm_stream, dspm_out, nm, &dspm_count, dspl_jobs, nlj, dspl_norm, &dspl_count);
#else
        dsp_models(dspm_stream, dspm_out, nm, &dspm_count);
#endif
    }
#ifdef DSP_WALLS
    else if (walls)
        dsp_walls_start();                  /* (no models: the walls alone) */
#endif
}

#ifdef SHADE_CHECK
u32                 shade_checks, shade_diffs;
#endif
#ifdef MF_PROF
u32                 mf_t[4], mf_v[4], mf_m[4];
#endif
#ifdef MODEL_CHECK
u32                 model_checks[4], model_diffs[3];   /* vertices, buckets (and quads by their other half), models' commands */
#endif
#ifdef COMPARE_MODELS
bool                r_model_ref;            /* (tools/compare.sh with COMPARE=models: UP, draw_model's old loops) */
#endif


static __attribute__((noinline)) void draw_model(r_ctx *x, int ei)
{
    const q_entity  *e = &ents[ei];
    const q_mdl     *m = e->mdl;
    s32             c = fcos(e->yaw), sn = fsin(e->yaw), zmin = 0x7FFFFFFF, zmax = -0x7FFFFFFF;
    const u8        *f0 = m->frames + (u32)e->oldframe * m->frame_bytes, *f1 = m->frames + (u32)e->frame * m->frame_bytes;
    const u8        *v0 = f0 + 24, *v1 = f1 + 24, *vn = e->lerp < FIX(0.5) ? v0 : v1;
    s32             lerp = e->lerp, inv;
    model_xform     xf;
    s32             (*A0)[3] = xf.A0, (*A1)[3] = xf.A1, *C0 = xf.C0, *C1 = xf.C1;
    const u16       *gt;
    const q_dlight  *msrc = r_dlights;      /* (the lights it goes by) */
    int             nml = ndl;
    int             i, b, nv = imin(m->nverts, MAX_MVERTS), np = imin(m->npolys, MAX_MPOLYS);
#if defined(COMPARE_MODELS) || defined(MODEL_CHECK)
    int             bi;                     /* (the C command passes) */
#endif
    const q_mpoly   *polys = m->polys;
    int             vi;
    vdp_writer      *w = x->w;
    u32             t0 = frt_read();
#ifdef SHADE_CHECK
    {
        /* (OPT=-DSHADE_CHECK: its light as this CPU reads it, and made again: the same? The slave
           makes it after the master's gone on: model.c ents_shade) */
        extern u32 shade_checks, shade_diffs;
        u16 again[162];
        int k;

        if (e->g_litleaf >= 0)
        {
            model_shade(again, m->shade + e->g_yaw * 162, &lv.leaflight[e->g_litleaf * 4]);
            ++shade_checks;
            for (k = 0; k < 162; ++k)
                if (again[k] != e->gbase[k])
                {
                    ++shade_diffs;
                    break;
                }
        }
    }
#endif
#ifdef ENTLIGHT_CHECK
    {
        /* (OPT=-DENTLIGHT_CHECK: its light made for the leaf it's in, turned as it is) */
        extern u32 el_checks, el_diffs;

        ++el_checks;
        el_diffs += e->g_litleaf != e->g_leaf || e->g_yaw != (int)(((u32)e->yaw >> 12) & 15);
    }
#endif

    if (ent_dsp[ei] < 0 && !model_xf(e, &xf))
        return;
    ++x->st.models;
    x->st.mcpu += ent_dsp[ei] < 0;
    if (model_far(e))
    {
        /* far: the mesh merged on a coarse grid (a third of the polygons), its vertices the
           first so many (tools/bake_md2.py) */
        polys = m->fpolys;
        np = imin(m->nfpolys, MAX_MPOLYS);
        nv = imin(m->nfverts, MAX_MVERTS);
    }
    /* its light by normal: the base (ents_light), plus any dynamic lights
       near it, stronger on the side facing them */
    gt = e->gbase;
#ifndef NO_LIGHT_AHEAD
    msrc = lights_lagged(&nml);             /* (the models go by last frame's lights: model.c) */
    nml = imin(nml, MAX_DLIGHTS);
    {
        int gl = ((volatile q_entity *)UNCACHED(e))->g_lit;     /* (the slave's, after the master's read e) */

        if (gl >= 0)
            gt = ent_lit[gl];               /* (lit a frame behind, as the frame started: model.c) */
        i = gl == -2 ? 0 : ndl;
    }
    for (; i < nml; ++i)
#else
    for (i = 0; i < nml; ++i)
#endif
    {
        const q_dlight  *l = &msrc[i];
        s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
        s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16, d2 = dx * dx + dy * dy + dz * dz;
        s32             f, len, mx, my, mz;
        const s16       *nrm = m->normals;
        int             n;

        if (d2 >= r * r)
            continue;
#ifdef DSP_LIGHT
        if (ent_dsp[ei] >= 0 && ent_ljn[ei] > 0)
        {
            /* the weights from the DSP (this model's jobs are this light and those after, in
               order): wait if it's not there yet, and read them past the cache */
            const s32   *wt = (const s32 *)UNCACHED(dspl_out + ent_lj0[ei] * DSPL_N);
            int         n;

            while (*(volatile u32 *)UNCACHED(&dspl_count) <= (u32)ent_lj0[ei])
                ;
            if (gt == e->gbase)
            {
                memcpy(x->gtab, e->gbase, sizeof(x->gtab));
                gt = x->gtab;
            }
#ifdef DSPL_CHECK
            {
                s32 fc = ((r * r - d2) * dl[i].inv) >> 8, lc = (s32)isqrt((u32)d2) + 1;
                s32 cx = ((dx * c + dy * sn) >> 2) / lc, cy = ((dy * c - dx * sn) >> 2) / lc, cz = (dz << 14) / lc;
                const s16 *nr = m->normals;

                for (n = 0; n < 162; ++n, nr += 3)
                {
                    s32 dot = (nr[0] * cx + nr[1] * cy + nr[2] * cz) >> 14;

                    ++dspl_checks;
                    dspl_diffs += wt[n] != (fc * (5734 + (dot > 0 ? (dot * 10650) >> 14 : 0))) >> 14;
                }
            }
#endif
            for (n = 0; n < 162; ++n)
            {
                s32 w = wt[n];
                u16 g0 = x->gtab[n];
                int rr = (g0 & 31) + ((l->r * w) >> 16), gg = ((g0 >> 5) & 31) + ((l->g * w) >> 16);
                int bb = ((g0 >> 10) & 31) + ((l->b * w) >> 16);

                x->gtab[n] = (u16)(0x8000 | imin(bb, 31) << 10 | imin(gg, 31) << 5 | imin(rr, 31));
            }
            ++ent_lj0[ei];                  /* (the next light's job) */
            continue;
        }
#endif
        f = ((r * r - d2) * ((1 << 24) / imax(r * r, 1))) >> 8;     /* 0..65536 at the origin */
        len = (s32)isqrt((u32)d2) + 1;
        /* the direction to the light, in the model's space (turned back by its yaw), 2.14 */
        mx = ((dx * c + dy * sn) >> 2) / len;
        my = ((dy * c - dx * sn) >> 2) / len;
        mz = (dz << 14) / len;
        if (gt == e->gbase)
        {
            memcpy(x->gtab, e->gbase, sizeof(x->gtab));
            gt = x->gtab;
        }
        for (n = 0; n < 162; ++n, nrm += 3)
        {
            s32 dot = (nrm[0] * mx + nrm[1] * my + nrm[2] * mz) >> 14;      /* 2.14 */
            s32 w = (f * (5734 + (dot > 0 ? (dot * 10650) >> 14 : 0))) >> 14;   /* 0.35 + 0.65 dot */
            u16 g0 = x->gtab[n];
            int rr = (g0 & 31) + ((l->r * w) >> 16), gg = ((g0 >> 5) & 31) + ((l->g * w) >> 16);
            int bb = ((g0 >> 10) & 31) + ((l->b * w) >> 16);

            x->gtab[n] = (u16)(0x8000 | imin(bb, 31) << 10 | imin(gg, 31) << 5 | imin(rr, 31));
        }
    }
    /* the vertices: on the DSP (both frames, 16 at a time, straight into view space), the CPU just
       blends and projects; or all on the CPU */
    x->st.t_mlight += (frt_read() - t0) & 0xFFFF;
#ifdef MF_PROF
    u32 mf0 = frt_read();
#endif
    if (ent_dsp[ei] >= 0)
    {
        /* on the DSP (models_to_dsp): wait if it's not there yet, have the cache
           forget what it had where the DSP wrote, and read them, blended already */
        const s32   *out = dspm_blk(ent_blk[ei]);
        u32         a, end = (u32)(out + ((nv + 15) >> 4) * 48);

        u32         tw = frt_read();

        while (*(volatile u32 *)UNCACHED(&dspm_count) <= (u32)ent_dsp[ei])
            ;
        x->st.t_mwait += (frt_read() - tw) & 0xFFFF;
        for (a = (u32)out; a < end; a += 16)
            *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;      /* the cache's associative purge */
#ifdef COMPARE_MODELS
        if (r_model_ref)
        for (vi = 0; vi < nv; ++vi)
        {
            const s32   *o;

            i = vi;
            o = out + (i >> 4) * 48 + (i & 15);
            s32         vx = o[0], vy = o[16], vz = o[32];
            u8          oc = 0;

            x->mz[i] = vz;
            x->mg[i] = gt[vn[i * 4 + 3] < 162 ? vn[i * 4 + 3] : 0];
            if (vz < NEAR_Z)
                oc = OC_NEAR;
            else
            {
                u32 xy = project(x, vx, vy, vz);
                s32 sx = XY_X(xy), sy = XY_Y(xy);

                x->mxy[i] = xy;
                if (sx < 0) oc |= OC_LEFT;
                else if (sx >= SCREEN_W) oc |= OC_RIGHT;
                if (sy < 0) oc |= OC_TOP;
                else if (sy >= SCREEN_H) oc |= OC_BOTTOM;
                if (vz < zmin) zmin = vz;
                if (vz > zmax) zmax = vz;
            }
            x->moc[i] = oc;
        }
        else
#endif
        {
            /* in assembly (src/mdraw.s), then each vertex's light by its normal (its index is
               in the frame, on the cart) */
            mverts_args a;

            a.out = out;
            a.mxy = x->mxy;
            a.mz = x->mz;
            a.moc = x->moc;
            a.n = nv;
            a.zmin = zmin;
            a.zmax = zmax;
#ifdef FIGHT_BENCH
            u32 ta = frt_read();
#endif
            mverts_asm(&a);
#ifdef FIGHT_BENCH
            x->st.t_masm += (frt_read() - ta) & 0xFFFF;
            ta = frt_read();
#endif
#ifdef MODEL_CHECK
            {
                /* (OPT=-DMODEL_CHECK: the C again, compared) */
                s32 cmin = zmin, cmax = zmax;
                int k;

                for (k = 0; k < nv; ++k)
                {
                    const s32   *o = out + (k >> 4) * 48 + (k & 15);
                    s32         vx = o[0], vy = o[16], vz = o[32];
                    u8          oc = 0;
                    u32         xy = 0;

                    if (vz >= NEAR_Z)
                    {
                        xy = project(x, vx, vy, vz);
                        if (XY_X(xy) < 0) oc |= OC_LEFT;
                        else if (XY_X(xy) >= SCREEN_W) oc |= OC_RIGHT;
                        if (XY_Y(xy) < 0) oc |= OC_TOP;
                        else if (XY_Y(xy) >= SCREEN_H) oc |= OC_BOTTOM;
                        if (vz < cmin) cmin = vz;
                        if (vz > cmax) cmax = vz;
                    }
                    else
                        oc = OC_NEAR;
                    ++model_checks[0];
                    if (x->mz[k] != vz || x->moc[k] != oc || (vz >= NEAR_Z && x->mxy[k] != xy))
                        ++model_diffs[0];
                }
                if (cmin != a.zmin || cmax != a.zmax)
                    ++model_diffs[0];
            }
#endif
            zmin = a.zmin;
            zmax = a.zmax;
            PROF(x->st.proj += nv);
            for (i = 0; i < nv; ++i)
            {
                int k = vn[i * 4 + 3];

                x->mg[i] = gt[k < 162 ? k : 0];
            }
#ifdef FIGHT_BENCH
            x->st.t_mnorm += (frt_read() - ta) & 0xFFFF;
#endif
        }
    }
    else for (vi = 0; vi < nv; ++vi)
    {
        s32 p[3], vx, vy, vz;
        u8  oc = 0;
        const u8 *q0, *q1;

        i = vi;
        q0 = v0 + i * 4;
        q1 = v1 + i * 4;

        /* each frame straight into view space (bytes times the folded matrix: 32-bit multiplies), then blended */
        vx = A0[0][0] * q0[0] + A0[0][1] * q0[1] + A0[0][2] * q0[2] + C0[0];
        vy = A0[1][0] * q0[0] + A0[1][1] * q0[1] + A0[1][2] * q0[2] + C0[1];
        vz = A0[2][0] * q0[0] + A0[2][1] * q0[1] + A0[2][2] * q0[2] + C0[2];
        if (lerp)
        {
            p[0] = A1[0][0] * q1[0] + A1[0][1] * q1[1] + A1[0][2] * q1[2] + C1[0];
            p[1] = A1[1][0] * q1[0] + A1[1][1] * q1[1] + A1[1][2] * q1[2] + C1[1];
            p[2] = A1[2][0] * q1[0] + A1[2][1] * q1[1] + A1[2][2] * q1[2] + C1[2];
            vx += fmul(p[0] - vx, lerp);
            vy += fmul(p[1] - vy, lerp);
            vz += fmul(p[2] - vz, lerp);
        }
        x->mz[i] = vz;
        x->mg[i] = gt[vn[i * 4 + 3] < 162 ? vn[i * 4 + 3] : 0];
        if (vz < NEAR_Z)
            oc = OC_NEAR;
        else
        {
            u32 xy = project(x, vx, vy, vz);
            s32 sx = XY_X(xy), sy = XY_Y(xy);

            x->mxy[i] = xy;
            if (sx < 0) oc |= OC_LEFT;
            else if (sx >= SCREEN_W) oc |= OC_RIGHT;
            if (sy < 0) oc |= OC_TOP;
            else if (sy >= SCREEN_H) oc |= OC_BOTTOM;
            if (vz < zmin) zmin = vz;
            if (vz > zmax) zmax = vz;
        }
        x->moc[i] = oc;
    }
    x->st.t_mverts += (frt_read() - t0) & 0xFFFF;
#ifdef MF_PROF
    {
        /* (OPT="-DFIGHT_BENCH -DMF_PROF": the vertices' time by way: the DSP's whole mesh, its far
           mesh, the CPU's whole, its far) */
        extern u32 mf_t[4], mf_v[4], mf_m[4];
        int k = (ent_dsp[ei] < 0) * 2 + (polys != m->polys);

        mf_t[k] += (frt_read() - mf0) & 0xFFFF;
        mf_v[k] += (u32)nv;
        ++mf_m[k];
    }
#endif
    /* the polygons facing us, by depth */
    for (b = 0; b < MBUCKETS; ++b)
        x->mhead[b] = -1;
    inv = ((MBUCKETS - 1) << 16) / imax(((zmax - zmin) >> 16) + 1, 1);
#ifdef COMPARE_MODELS
    if (!r_model_ref)
#endif
    {
        mpolys_args a;

        a.polys = polys;
        a.n = np;
        a.moc = x->moc;
        a.mxy = x->mxy;
        a.mz = x->mz;
        a.mhead = x->mhead;
        a.zmin = zmin;
        a.inv = inv;
        a.sign = 0;
        a.nearl = NULL;
        a.nnear = 0;
        mpolys_asm(&a);
#ifdef MODEL_CHECK
        {
            s16 head[MBUCKETS], nxt[MAX_MPOLYS];
            int k, j, q;

            memcpy(head, x->mhead, sizeof(head));
            memcpy(nxt, x->mnext, sizeof(nxt));
            for (k = 0; k < MBUCKETS; ++k)
                x->mhead[k] = -1;
            for (i = 0; i < np; ++i)
            {
                const q_mpoly   *p = &polys[i];
                int             a0 = p->v[0], b0 = p->v[1], c0 = p->v[2], d0 = p->v[3];
                s32             cross, z;
                u32             pa, pb, pc;

                if (x->moc[a0] & x->moc[b0] & x->moc[c0] & x->moc[d0])
                    continue;
                if ((x->moc[a0] | x->moc[b0] | x->moc[c0] | x->moc[d0]) & OC_NEAR)
                    continue;
                pa = x->mxy[a0];
                pb = x->mxy[b0];
                pc = x->mxy[c0];
                cross = (XY_X(pb) - XY_X(pa)) * (XY_Y(pc) - XY_Y(pa)) - (XY_Y(pb) - XY_Y(pa)) * (XY_X(pc) - XY_X(pa));
                if (cross == 0 && !(p->flags & 1))
                {
                    u32 pd = x->mxy[d0];

                    ++model_checks[2];
                    cross = (XY_X(pc) - XY_X(pa)) * (XY_Y(pd) - XY_Y(pa)) - (XY_Y(pc) - XY_Y(pa)) * (XY_X(pd) - XY_X(pa));
                }
                if (cross * MODEL_FRONT <= 0)
                    continue;
                z = (x->mz[a0] >> 1) + (x->mz[c0] >> 1);
                b = iclamp((((z - zmin) >> 16) * inv) >> 16, 0, MBUCKETS - 1);
                x->mnext[i] = x->mhead[b];
                x->mhead[b] = (s16)i;
            }
            for (k = 0; k < MBUCKETS; ++k)
            {
                ++model_checks[1];
                for (j = head[k], q = x->mhead[k]; j >= 0 && q >= 0 && j == q; j = nxt[j], q = x->mnext[q])
                    ;
                if (j != q)
                    ++model_diffs[1];
            }
        }
#endif
    }
#ifdef COMPARE_MODELS
    else
    for (i = 0; i < np; ++i)
    {
        const q_mpoly   *p = &polys[i];
        int             a = p->v[0], bb = p->v[1], cc = p->v[2], dd = p->v[3];
        s32             cross, z;
        u32             pa, pb, pc;

        if (x->moc[a] & x->moc[bb] & x->moc[cc] & x->moc[dd])
            continue;
        if ((x->moc[a] | x->moc[bb] | x->moc[cc] | x->moc[dd]) & OC_NEAR)
            continue;
        pa = x->mxy[a];
        pb = x->mxy[bb];
        pc = x->mxy[cc];
        cross = (XY_X(pb) - XY_X(pa)) * (XY_Y(pc) - XY_Y(pa)) - (XY_Y(pb) - XY_Y(pa)) * (XY_X(pc) - XY_X(pa));
        if (cross == 0 && !(p->flags & 1))
        {
            u32 pd = x->mxy[dd];

            cross = (XY_X(pc) - XY_X(pa)) * (XY_Y(pd) - XY_Y(pa)) - (XY_Y(pc) - XY_Y(pa)) * (XY_X(pd) - XY_X(pa));
        }
        if (cross * MODEL_FRONT <= 0)
            continue;
        z = (x->mz[a] >> 1) + (x->mz[cc] >> 1);
        b = iclamp((((z - zmin) >> 16) * inv) >> 16, 0, MBUCKETS - 1);
        x->mnext[i] = x->mhead[b];
        x->mhead[b] = (s16)i;
    }
#endif
    x->st.t_mpolys += (frt_read() - t0) & 0xFFFF;
    /* out: nearest first when pushing, farthest first when appending */
#ifdef COMPARE_MODELS
    if (r_model_ref)
    for (bi = 0; bi < MBUCKETS; ++bi)
        for (b = x->fifo ? MBUCKETS - 1 - bi : bi, i = x->mhead[b]; i >= 0; i = x->mnext[i])
        {
            const q_mpoly   *p = &polys[i];
            const q_mtex    *mt = &m->tex[p->tex];
            int             id = e->skin * m->ntex + p->tex;
            s32             vram = tex_vram(x, m->tex_id0 + id);
            u32             *dw, link;
            u16             grda;

            if (vram < 0 || !(dw = cmd_alloc(x, &link)))
                continue;
            dw[0] = 0x10020000u | link;
            dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16
                    | ((lut_vram + (u32)slot_lut[x->tex_slot[m->tex_id0 + id]] * 32) >> 3);
            dw[2] = ((u32)vram >> 3) << 16 | (u32)(((mt->w >> 3) << 8) | mt->h);
            dw[3] = x->mxy[p->v[0]];
            dw[4] = x->mxy[p->v[1]];
            dw[5] = x->mxy[p->v[2]];
            dw[6] = x->mxy[p->v[3]];
            grda = vdp_gouraud_fast(w, (u32)x->mg[p->v[0]] << 16 | x->mg[p->v[1]],
                                    (u32)x->mg[p->v[2]] << 16 | x->mg[p->v[3]]);
            dw[7] = (u32)grda << 16;
            ++x->st.mpolys;
        }
    else
#endif
    {
        /* in assembly (src/mdraw.s) */
        mcmds_args  a;
#ifdef MODEL_CHECK
        s32         cnt0 = w->count, head0 = w->head[x->bucket], tail0 = w->tail[x->bucket], gc0 = w->gcount;
        u16         link0 = x->fifo && head0 >= 0 ? w->cmds[tail0].link : 0;
        u32         h_asm = 0, h_c = 0;
        int         k;
#endif

        a.polys = polys;
        a.tex = m->tex;
        a.mnext = x->mnext;
        a.mxy = x->mxy;
        a.mg = x->mg;
        a.tslot = x->tex_slot;
        a.sframe = slot_frame;
        a.cmds = (u32 *)w->cmds;
        a.gst = w->gst;
        a.tid0 = m->tex_id0 + e->skin * m->ntex;
        a.frame = frame;
        a.svram = slot_vram;
        a.sbytes = slot_bytes;
        a.lb = w->link_base;
        a.dw1 = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | lut_vram >> 3;
        a.slut = slot_lut;                      /* (each slot's table: tex_load) */
        a.gmax = w->gmax;
        a.gb = w->gbase >> 3;
        a.fifo = x->fifo;
        a.x = x;
        a.cnt = w->count;
        a.head = w->head[x->bucket];
        a.tail = w->tail[x->bucket];
        a.gc = w->gcount;
        a.drop = 0;
        a.mhead = x->mhead;
        a.wcmds = w->cmax;
        mcmds_asm(&a);
        x->st.mpolys += a.cnt - w->count;
        x->st.dropped += a.drop;
        w->count = a.cnt;
        w->head[x->bucket] = (s16)a.head;
        w->tail[x->bucket] = (s16)a.tail;
        w->gcount = a.gc;
#ifdef MODEL_CHECK
        /* (the C again from the same start, compared by a hash of all it wrote) */
        for (k = cnt0 * 8; k < a.cnt * 8; ++k)
            h_asm = h_asm * 31 + ((u32 *)w->cmds)[k];
        for (k = gc0 * 2; k < a.gc * 2; ++k)
            h_asm = h_asm * 31 + w->gst[k];
        h_asm = h_asm * 31 + (u32)(a.head ^ a.tail << 16) + (x->fifo && head0 >= 0 ? w->cmds[tail0].link : 0);
        x->st.mpolys -= a.cnt - cnt0;
        x->st.dropped -= a.drop;
        w->count = cnt0;
        w->head[x->bucket] = (s16)head0;
        w->tail[x->bucket] = (s16)tail0;
        w->gcount = gc0;
        if (x->fifo && head0 >= 0)
            w->cmds[tail0].link = link0;
    {
        /* (the C: draw_model's command pass before mdraw.s) */
        u32         *cmds = (u32 *)w->cmds, *gst = w->gst, gb = w->gbase >> 3;
        int         bk = x->bucket, cnt = w->count, head = w->head[bk], tail = w->tail[bk], gc = w->gcount;
        int         gmax = w->gmax, tid0 = m->tex_id0 + e->skin * m->ntex, n = 0;
        u32         lb = w->link_base;
        bool        fifo = x->fifo;
        const u16   *tslot = x->tex_slot, *mg = x->mg;
        const u32   *mxy = x->mxy;
        const s16   *mnext = x->mnext;
        const q_mtex *tex = m->tex;

        for (bi = 0; bi < MBUCKETS; ++bi)
            for (b = fifo ? MBUCKETS - 1 - bi : bi, i = x->mhead[b]; i >= 0; i = mnext[i])
            {
                const q_mpoly   *p = &polys[i];
                const q_mtex    *mt = &tex[p->tex];
                int             t = tid0 + p->tex, sl = tslot[t], gi;
                s32             vram;
                u32             *dw, link = 0;

                if (sl != 0xFFFF)
                {
                    slot_frame[sl] = frame;
                    vram = (s32)(slot_vram + (u32)sl * slot_bytes);
                }
                else if ((vram = tex_load(x, t)) < 0)
                    continue;
                else
                    sl = tslot[t];
                if (cnt >= w->cmax)
                {
                    ++x->st.dropped;
                    continue;
                }
                if (fifo)
                {
                    if (head < 0)
                        head = cnt;
                    else
                        ((u16 *)&cmds[tail * 8])[1] = (u16)(lb + (u32)cnt * 4);
                    tail = cnt;
                }
                else
                {
                    if (head < 0)
                        tail = cnt;
                    else
                        link = (u16)(lb + (u32)head * 4);
                    head = cnt;
                }
                dw = &cmds[cnt++ * 8];
                dw[0] = 0x10020000u | link;
                dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | ((lut_vram + (u32)slot_lut[sl] * 32) >> 3);
                dw[2] = ((u32)vram >> 3) << 16 | (u32)(((mt->w >> 3) << 8) | mt->h);
                dw[3] = mxy[p->v[0]];
                dw[4] = mxy[p->v[1]];
                dw[5] = mxy[p->v[2]];
                dw[6] = mxy[p->v[3]];
                gi = gc;
                if (gi >= gmax)
                    gi = gmax - 1;
                else
                    gc = gi + 1;
                gst[gi * 2] = (u32)mg[p->v[0]] << 16 | mg[p->v[1]];
                gst[gi * 2 + 1] = (u32)mg[p->v[2]] << 16 | mg[p->v[3]];
                dw[7] = (gb + (u32)gi) << 16;
                ++n;
            }
        w->count = cnt;
        w->head[bk] = (s16)head;
        w->tail[bk] = (s16)tail;
        w->gcount = gc;
        x->st.mpolys += n;
        for (i = cnt0 * 8; i < cnt * 8; ++i)
            h_c = h_c * 31 + cmds[i];
        for (i = gc0 * 2; i < gc * 2; ++i)
            h_c = h_c * 31 + gst[i];
        h_c = h_c * 31 + (u32)(head ^ tail << 16) + (fifo && head0 >= 0 ? w->cmds[tail0].link : 0);
        ++model_checks[3];
        if (h_c != h_asm)
            ++model_diffs[2];
    }
#endif
    }
    x->st.t_models += (frt_read() - t0) & 0xFFFF;
#ifdef R_PROFILE
    {
        /* (profile: how much of it went on models beyond 400 units, and how many) */
        s32 dx = (e->origin[0] - cam.pos[0]) >> 16, dy = (e->origin[1] - cam.pos[1]) >> 16;

        if (dx * dx + dy * dy > 400 * 400)
        {
            x->st.t_mfar += (frt_read() - t0) & 0xFFFF;
            ++x->st.mfar;
        }
    }
#endif
}


static void         draw_item(r_ctx *x, int i, bool uncached)
{
    int             f = uncached ? ((volatile u16 *)UNCACHED(vis_faces))[i] : vis_faces[i];
    int             m = uncached ? ((volatile u8 *)UNCACHED(vis_model))[i] : vis_model[i];

    if (m == SPRITE)
        draw_sprite(x, f);
    else if (m == ENTITY)
        draw_model(x, f);
    else
        draw_face(x, f, m);
}

#ifdef UPLOAD_CHECK
u32                 upload_checks, upload_diffs;
#endif

/* a queued texture into VRAM (tex_upload) */
static void         upload_one(const void *src, u32 d)
{
    u32             vram = (d & ~UP_LATE) >> 8, bytes = (d & 255) * 4;

#ifndef UPLOAD_CHECK
    if (vdp_dma_queue(vram, src, bytes))
        return;                             /* (with the lists, not waited for: vdp_submit) */
#endif
    scu_dma0((void *)(VDP1_VRAM + vram), src, bytes, true);
    while (scu_dma0_busy())
        ;
#ifdef UPLOAD_CHECK
    {
        /* (OPT=-DUPLOAD_CHECK: read back and compared) */
        extern u32 upload_checks, upload_diffs;

        ++upload_checks;
        if (memcmp((const void *)(VDP1_VRAM + vram), src, bytes))
            ++upload_diffs;
    }
#endif
}

/* (vdp_submit, once VDP1's done with the frame before last) the late uploads: into slots
   that frame used */
static void         r_late_uploads(void)
{
    int             i, k;

    for (i = 0; i < 2; ++i)
    {
        r_ctx       *c = i ? (r_ctx *)UNCACHED(&ctx[1]) : &ctx[0];
        int         n = c->nup;

        for (k = 0; k < n; ++k)
            upload_one(c->up_src[k], c->up_dst[k]);
        c->nup = 0;
    }
}

static void         part_begin(r_ctx *x)
{
    memset(&x->st, 0, sizeof(x->st));
    x->wl_nrec = 0;
#ifdef DSP_WALLS
    /* (this frame's half of dw_rec: the master's up from its middle, the slave's down) */
    x->dw_step = x == &ctx[0] ? 1 : -1;
    x->dw_rp = dw_on ? (u16 *)dw_rec[dw_count & 1] + DW_REC - (x != &ctx[0]) : NULL;
#endif
    x->full = false;
    x->tight = false;
    x->nup = 0;
}

#ifdef FIGHT_BENCH
u32                 sl_first;               /* (the slave: ticks it waited for its first item) */
#endif

void                render_slave(void)
{
    r_ctx           *x = &ctx[1];
    u32             t0 = frt_read();
    int             lo = 0;
#ifdef FIGHT_BENCH
    bool            first = true;
#endif

    part_begin(x);
    cells_frame(x);
    face_frame(x);
    for (;;)
    {
        int n = SHARE->published, hi = SHARE->hi, done = SHARE->walk_done;

        if (lo < n && lo < hi)
        {
#ifdef FIGHT_BENCH
            if (first)
            {
                first = false;
                sl_first = (frt_read() - t0) & 0xFFFF;
            }
#endif
            SHARE->lo = lo + 1;             /* claim it, then draw it */
            draw_item(x, lo++, true);
        }
        else if (done && (lo >= hi || lo >= SHARE->published))
            break;
    }
    x->st.t_face = frt_to_us((frt_read() - t0) & 0xFFFF);
}

static int          view_leaf;              /* (the camera's: the gun's light) */

static void         draw_master(void)
{
    r_ctx           *x = &ctx[0];
    u32             t0 = frt_read();
    int             i;

    if (r_lit_wait)
        r_lit_wait();                       /* (the models' lights: the slave's, a frame behind) */
    part_begin(x);
    {
        u32 tv = frt_read();

        draw_viewmodel(x, view_leaf);       /* (first, so the slave takes more of the list) */
        x->st.t_view += (frt_read() - tv) & 0xFFFF;
    }
    if (!r_two_cpus)
    {
        /* on its own: all of it, pushed nearest first */
#ifdef TEST_FIFO
        x->fifo = true;                     /* (test: appended, as the master's with two; the wrong order) */
#else
        x->fifo = false;
#endif
        cells_frame(x);
        face_frame(x);
        for (i = 0; i < nvis; ++i)
            draw_item(x, i, false);
    }
    else
    {
        int hi = nvis;

        x->fifo = true;
        cells_frame(x);
        face_frame(x);
        SHARE->hi = hi;
        SHARE->walk_done = 1;
        while (hi - 1 >= SHARE->lo)
        {
            SHARE->hi = --hi;               /* claim it, then draw it */
            draw_item(x, hi, false);
        }
    }
    x->st.t_face = frt_to_us((frt_read() - t0) & 0xFFFF);
}

#ifdef OCC_COUNT
/* (OPT=-DOCC_COUNT: how much a coverage mask would cull, counted after the
   frame's drawing. The list is front to back: each big solid rectangular
   face marks the screen tiles (16 x 8) it covers wholly; a face whose
   screen box is all marked tiles is behind them) */
static u32          occ_rows[SCREEN_H / 8];

/* the quad's x extent at scanline y (false: it misses it) */
static bool         quad_span(const s32 *qx, const s32 *qy, s32 y, s32 *l, s32 *r)
{
    int             k, n = 0;

    *l = 0x7FFFFFFF;
    *r = -0x7FFFFFFF;
    for (k = 0; k < 4; ++k)
    {
        s32 x0 = qx[k], y0 = qy[k], x1 = qx[(k + 1) & 3], y1 = qy[(k + 1) & 3], xx;

        if ((y < y0 && y < y1) || (y > y0 && y > y1))
            continue;
        xx = y1 == y0 ? (x0 < x1 ? x0 : x1) : x0 + (s32)(((s64)(y - y0) * (x1 - x0)) / (y1 - y0));
        if (y1 == y0)
        {
            if (x0 < *l) *l = x0;
            if (x1 < *l) *l = x1;
            if (x0 > *r) *r = x0;
            if (x1 > *r) *r = x1;
        }
        if (xx < *l) *l = xx;
        if (xx > *r) *r = xx;
        ++n;
    }
    return n > 0;
}

static void         occ_count(void)
{
    int             k;

    memset(occ_rows, 0, sizeof(occ_rows));
    for (k = 0; k < nvis; ++k)
    {
        int             m = vis_model[k], nu, nv, c, r, r0, r1, t0, t1, W, H, N = lv.N;
        const q_face    *f;
        s32             d[3], qx[4], qy[4], minx, maxx, miny, maxy;
        v3              o, du, dv, P[4];
        bool            hidden = true, solid;

        if (m == SPRITE || m == ENTITY)
            continue;
        f = &lv.faces[vis_faces[k]];
        if (f->flags & (FF_SKY | FF_NODRAW))
            continue;
        nu = f->nu;
        nv = f->nv;
        d[0] = f->origin[0] + mover_ofs[m][0] - cam.pos[0];
        d[1] = f->origin[1] + mover_ofs[m][1] - cam.pos[1];
        d[2] = f->origin[2] + mover_ofs[m][2] - cam.pos[2];
        to_view(d, &o);
        to_view(&lv.axes[f->axes * 6], &du);
        to_view(&lv.axes[f->axes * 6 + 3], &dv);
        /* the face's own rectangle: its grid's edge columns and rows are narrower */
        W = nu == 1 ? f->eu1 - f->eu0 : (N - f->eu0) + (nu - 2) * N + f->eu1;
        H = nv == 1 ? f->ev1 - f->ev0 : (N - f->ev0) + (nv - 2) * N + f->ev1;
        {
            s32 fu = W * rcp_n, fv = H * rcp_n;     /* in cells, 16.16 */
            s32 ux = fmul(du.x, fu), uy = fmul(du.y, fu), uz = fmul(du.z, fu);
            s32 vx = fmul(dv.x, fv), vy = fmul(dv.y, fv), vz = fmul(dv.z, fv);

            P[0] = o;
            P[1].x = o.x + ux; P[1].y = o.y + uy; P[1].z = o.z + uz;
            P[2].x = o.x + ux + vx; P[2].y = o.y + uy + vy; P[2].z = o.z + uz + vz;
            P[3].x = o.x + vx; P[3].y = o.y + vy; P[3].z = o.z + vz;
        }
        for (c = 0; c < 4; ++c)
            if (P[c].z < NEAR_Z)
                break;
        if (c < 4)
            continue;                       /* (across the near plane: neither) */
        minx = miny = 0x7FFFFFFF;
        maxx = maxy = -0x7FFFFFFF;
        for (c = 0; c < 4; ++c)
        {
            s32 rz = fdiv(FOCAL << 16, P[c].z);

            qx[c] = CX + (s32)(((s64)P[c].x * rz) >> 32);
            qy[c] = CY - (s32)(((s64)P[c].y * rz) >> 32);
            minx = imin(minx, qx[c]); maxx = imax(maxx, qx[c]);
            miny = imin(miny, qy[c]); maxy = imax(maxy, qy[c]);
        }
        if (maxx < 0 || minx >= SCREEN_W || maxy < 0 || miny >= SCREEN_H)
            continue;
        t0 = imax(minx, 0) >> 4;
        t1 = imin(maxx, SCREEN_W - 1) >> 4;
        r0 = imax(miny, 0) >> 3;
        r1 = imin(maxy, SCREEN_H - 1) >> 3;
        {
            u32 mask = (t1 >= 31 ? 0xFFFFFFFF : (2u << t1) - 1) & ~((1u << t0) - 1);

            for (r = r0; r <= r1; ++r)
                if ((occ_rows[r] & mask) != mask)
                    hidden = false;
        }
        if (hidden)
        {
            ++rs.occ_faces;
            rs.occ_cells += nu * nv;
            continue;
        }
        /* an occluder: the world's, opaque, and wholly its rectangle (no cropped cells) */
        solid = m == 0 && !(f->flags & (FF_TRANS33 | FF_TRANS66 | FF_WARP)) && maxx - minx >= 32 && maxy - miny >= 16;
        for (c = 0; solid && c < nu * nv; ++c)
        {
            int t = lv.cells[f->firstcell + c].tex;

            if (t == CELL_EMPTY || !(t & (CELL_FULL | CELL_EXACT)))
                solid = false;
        }
        if (!solid)
            continue;
        ++rs.occ_occluders;
        for (r = r0; r <= r1; ++r)
        {
            s32 l0, rr0, l1, rr1, a, b;

            if (!quad_span(qx, qy, r * 8, &l0, &rr0) || !quad_span(qx, qy, r * 8 + 7, &l1, &rr1))
                continue;
            a = imax(l0, l1);
            b = imin(rr0, rr1);
            a = (a + 15) >> 4;              /* whole tiles within [a, b] */
            b = (b + 1) >> 4;               /* (tiles < b) */
            if (a < 0) a = 0;
            if (b > SCREEN_W / 16) b = SCREEN_W / 16;
            if (b > a)
                occ_rows[r] |= ((b >= 32 ? 0xFFFFFFFF : (1u << b) - 1)) & ~((1u << a) - 1);
        }
    }
}
#endif

void                (*r_during)(void);
void                (*r_pre_wait)(void);
void                (*r_lit_wait)(void);

void                render_world(vdp_writer *w0, vdp_writer *w1)
{
    u32             t0 = frt_read();
    int             leaf, cluster, i;

    memset(&rs, 0, sizeof(rs));
    if (++frame == 0)
    {
            frame = 1;
    }
    leaf = level_leaf(cam.pos);
    cluster = lv.leafs[leaf].cluster;
    rs.leaf = leaf;
    rs.cluster = cluster;
    if (cluster != view_cluster)
    {
        view_cluster = cluster;
        mark_leaves(cluster);
    }
    {
        /* what can be seen through the portals */
#ifdef FIGHT_BENCH
        u32 tp = frt_read();
#endif

#ifdef PORTALS
        portals_on = r_portals && cluster >= 0 && portals_flow(cluster);
#endif
#ifdef FIGHT_BENCH
        rs.t_flow = frt_to_us((frt_read() - tp) & 0xFFFF);
#ifdef PORTALS
        rs.n_reach = portals_on ? nslots : 0;
#endif
#endif
    }
    /* this frame's lights, into view space; the sprites, into the leaves they're in */
#ifdef WALLS_AHEAD
    lsrc = lights_lagged(&ndl);             /* (the walls go by last frame's: r_wall_ahead's) */
    ndl = imin(ndl, MAX_DLIGHTS);
#else
    lsrc = r_dlights;
    ndl = imin(r_ndlights, MAX_DLIGHTS);
#ifdef DSP_WALLS
    if (dw_prog)
    {
        lsrc = lights_lagged(&ndl);         /* (a level the DSP lights the walls on: last frame's, the DSP's) */
        ndl = imin(ndl, MAX_DLIGHTS);
    }
#endif
#endif
    for (i = 0; i < ndl; ++i)
    {
        const q_dlight  *l = &lsrc[i];
        s32             d[3], rad = l->radius >> 16;
        v3              v;

        d[0] = l->pos[0] - cam.pos[0];
        d[1] = l->pos[1] - cam.pos[1];
        d[2] = l->pos[2] - cam.pos[2];
        to_view(d, &v);
        dl[i].x = v.x;
        dl[i].y = v.y;
        dl[i].z = v.z;
        dl[i].r2 = rad * rad;
        dl[i].inv = (1 << 24) / imax(dl[i].r2, 1);
        dl[i].r = l->r;
        dl[i].g = l->g;
        dl[i].b = l->b;
    }
    for (i = 0; i < r_nsprites && i < MAX_SPRITES; ++i)
    {
        int l = level_leaf(r_sprites[i].pos);

        spr_leaf[i] = l;
        spr_next[i] = leaf_spr[l];
        leaf_spr[l] = (s16)i;
    }
    /* (the entities' list and light may be the slave's: done by now, as a rule) */
    if (r_pre_wait)
        r_pre_wait();
    for (i = 0; i < nents; ++i)
    {
        int l;

        ent_leaf[i] = -1;
        if (!ents[i].live)
            continue;
        l = ents[i].g_leaf >= 0 && !ents[i].g_moved ? ents[i].g_leaf : level_leaf(ents[i].origin);  /* (ents_light's) */
        ent_leaf[i] = l;
        ent_next[i] = leaf_ent[l];
        leaf_ent[l] = (s16)i;
    }
    PROF(rs.us_pre = frt_to_us((frt_read() - t0) & 0xFFFF));
    /* the models' vertices: the DSP starts on them now; the gun's records on their way */
    models_to_dsp();
    view_fetch_start(w0);
    PROF(rs.us_mdsp = frt_to_us((frt_read() - t0) & 0xFFFF) - rs.us_pre);
    /* the slave starts on the list as the walk fills it */
    ctx[0].w = w0;
    ctx[1].w = w1;
    ctx[0].bucket = 0;
    ctx[1].bucket = 1;
    ctx[1].fifo = false;
    nvis = 0;
    SHARE->published = 0;
    SHARE->lo = 0;
    SHARE->hi = 0x7FFFFFFF;
    SHARE->walk_done = 0;
#ifdef FIGHT_BENCH
    rs.us_rwpre = frt_to_us((frt_read() - t0) & 0xFFFF);
#endif
    if (r_two_cpus)
        signal_slave();
#ifdef R_PROFILE
    {
        u32 tw = frt_read();
#endif
    if (r_walk_asm)
    {
        walk_setup();
        walk_asm(0, 15, &wctx);             /* all four side planes to try */
    }
    else
        walk(0, 15);
#ifdef R_PROFILE
        rs.us_tree = frt_to_us((frt_read() - tw) & 0xFFFF);
    }
#endif
#ifdef WALK_CHECK
    {
        /* (OPT="-DWALK_CHECK -DONE_CPU") the C walk again: the same list? */
        static u16  *a_faces;
        static u8   *a_model;
        int         na = nvis, k;

        if (!a_faces)
        {
            a_faces = level_alloc_low(MAX_VIS * 2);
            a_model = level_alloc_low(MAX_VIS);
        }

        memcpy(a_faces, vis_faces, (u32)na * 2);
        memcpy(a_model, vis_model, (u32)na);
        nvis = 0;
        walk(0, 15);
        walk_diff = 0;
        walk_first = -1;
        for (k = 0; k < na && k < nvis; ++k)
            if (a_faces[k] != vis_faces[k] || a_model[k] != vis_model[k])
            {
                if (walk_first < 0)
                {
                    walk_first = k;
                    walk_what[0] = a_faces[k]; walk_what[1] = a_model[k];
                    walk_what[2] = vis_faces[k]; walk_what[3] = vis_model[k];
                }
                ++walk_diff;
            }
        walk_len = na;
        walk_clen = nvis;
        walk_total += walk_diff + (na != nvis);
        ++walk_frames;
    }
#endif
    for (i = 0; i < nents; ++i)
        if (ent_leaf[i] >= 0)
            leaf_ent[ent_leaf[i]] = -1;
    for (i = 0; i < r_nsprites && i < MAX_SPRITES; ++i)
        leaf_spr[spr_leaf[i]] = -1;
    rs.nodes = (int)frt_to_us((frt_read() - t0) & 0xFFFF);  /* the walk's time */
    view_fetch_more();
    /* ("faster fights", on unless OPT=-DNO_GAME_DURING_DRAW: the game's tick here,
       on the master, while the slave draws from the front of the list; the master
       then draws from the back until they meet, so the slave takes more of it) */
    if (r_during)
        r_during();
    view_leaf = leaf;
    draw_master();
    if (r_two_cpus)
        wait_signal();
    /* both done: the textures they queued into VRAM (the slave's queue read
       uncached: it wrote it), before VDP1 can draw them (after the swap); the
       late ones kept, at the front, for r_late_uploads */
    for (i = 0; i < 2; ++i)
    {
        r_ctx       *c = i ? (r_ctx *)UNCACHED(&ctx[1]) : &ctx[0];
        int         n = c->nup, k, nl = 0;

        for (k = 0; k < n; ++k)
        {
            if (c->up_dst[k] & UP_LATE)
            {
                c->up_src[nl] = c->up_src[k];
                c->up_dst[nl++] = c->up_dst[k];
            }
            else
                upload_one(c->up_src[k], c->up_dst[k]);
        }
        c->nup = nl;
    }
#ifdef OCC_COUNT
    occ_count();
#endif
    /* the totals (the slave's through the cache's back door: it wrote them) */
    for (i = 0; i < 2; ++i)
    {
        const r_stats *s = i ? (const r_stats *)UNCACHED(&ctx[1].st) : &ctx[0].st;

        rs.faces += s->faces;
        rs.cells += s->cells;
        rs.culled += s->culled;
        rs.near += s->near;
        rs.uploads += s->uploads;
        rs.nocache += s->nocache;
        rs.late += s->late;
        r_full[i] += s->nocache > 0;
        r_full[2] += (u32)s->late;
#ifdef TEX_WSET
        {
            /* (OPT=-DTEX_WSET: the textures each CPU's part of the cache held for this frame) */
            const u16   *sf = (const u16 *)UNCACHED(slot_frame);
            int         k, nw = 0;

            for (k = ctx[i].slot0; k < ctx[i].slot0 + ctx[i].nslots; ++k)
                nw += sf[k] == frame;
            r_wset[i] = (u32)imax((s32)r_wset[i], nw);
            r_wset[2 + i] += (u32)nw;
            r_wset[4] = (u32)ctx[0].nslots;
        }
#endif
        rs.dropped += s->dropped;
        rs.proj += s->proj;
        rs.gverts += s->gverts;
        rs.seen += s->seen;
        rs.models += s->models;
        rs.mpolys += s->mpolys;
        rs.muploads += s->muploads;
        rs.mcpu += s->mcpu;
        rs.mdsp = rs_mdsp;
        rs.t_models += frt_to_us(s->t_models);
        rs.t_view += frt_to_us(s->t_view);
        rs.t_mfar += frt_to_us(s->t_mfar);
        rs.mfar += s->mfar;
        rs.t_mlight += frt_to_us(s->t_mlight);
        rs.t_mverts += frt_to_us(s->t_mverts);
        rs.t_mpolys += frt_to_us(s->t_mpolys);
        rs.t_mwait += frt_to_us(s->t_mwait);
        rs.t_masm += frt_to_us(s->t_masm);
        rs.t_mnorm += frt_to_us(s->t_mnorm);
        rs.t_dltest += frt_to_us(s->t_dltest);
        rs.t_dlsum += frt_to_us(s->t_dlsum);
        rs.n_dlfaces += s->n_dlfaces;
        rs.n_dlpts += s->n_dlpts;
#ifdef FS_STATS
        {
            int k;

            for (k = 0; k < 10; ++k)
                rs.fs[k] += s->fs[k];
        }
#endif
        rs.n_wlhit += s->n_wlhit;
#ifdef DSP_WALLS
        {
            int k;

            for (k = 0; k < 4; ++k)
#ifdef DW_CHECK
                rs.dw_why[k] = k == 2 ? imax(rs.dw_why[k], s->dw_why[k]) : rs.dw_why[k] + s->dw_why[k];
#else
                rs.dw_why[k] += s->dw_why[k];
#endif
        }
#endif
        rs.portal_out += s->portal_out;
        rs.nfast += s->nfast;
        rs.nslow += s->nslow;
        rs.ns_dl += s->ns_dl;
        rs.ns_crop += s->ns_crop;
        rs.ns_exact += s->ns_exact;
        rs.g_same += s->g_same;
        rs.g_flat += s->g_flat;
        rs.ns_small += s->ns_small;
        rs.nexact += s->nexact;
        rs.pieces += s->pieces;
        rs.faces_out += s->faces_out;
        rs.cells_all += s->cells_all;
        rs.cells_384 += s->cells_384;
        rs.cells_512 += s->cells_512;
        rs.p_setup += frt_to_us(s->p_setup);
        rs.p_grid += frt_to_us(s->p_grid);
        rs.p_cells += frt_to_us(s->p_cells);
        rs.p_slow += frt_to_us(s->p_slow);
        rs.p_corners += frt_to_us(s->p_corners);
        rs.p_xform += frt_to_us(s->p_xform);
        rs.p_cpre += frt_to_us(s->p_cpre);
        rs.p_casm += frt_to_us(s->p_casm);
        rs.n_casm += s->n_casm;
        rs.n_calls += s->n_calls;
        rs.n_rows += s->n_rows;
        rs.n_rfaces += s->n_rfaces;
    }
    rs.t_face = ctx[0].st.t_face;
    rs.t_grid = ((const r_stats *)UNCACHED(&ctx[1].st))->t_face;
    rs.us_walk = frt_to_us((frt_read() - t0) & 0xFFFF);
}

/* src/cycles.c: one grid row of 8 points, 512 times (FRT ticks) */
u32                 render_bench_grid(void)
{
    static const v3 p = { FIX(-40), FIX(-30), FIX(200) }, du = { FIX(10), FIX(1), FIX(2) };
    u32             t = frt_read();
    int             i;

    ctx[0].ga.p = p; ctx[0].ga.e0 = du; ctx[0].ga.d = du; ctx[0].ga.e1 = du;
    for (i = 0; i < 512; ++i)
        if (r_grid_asm)
            grid_row_asm(ctx[0].grid, 8, &ctx[0].ga, grid_k);
        else
            grid_row(ctx[0].grid, 8, &p, &du, &du, &du, fmul(du.z, kx), fmul(du.z, ky));
    return (frt_read() - t) & 0xFFFF;
}

/* ---- the sky: a VDP2 layer behind everything, showing where VDP1 drew nothing ---- */

static int          sky_h, sky_up;          /* (the sky's rows, those above the horizon) */
static bool         sky_ok;                 /* (the level has one: its file read into VDP2) */
static u16          sky_above, sky_below, sky_zenith;
static volatile int sky_line = -9999;       /* the horizon the back colour table was last built for */
static s32          sky_ring[4][2];         /* each frame's yaw and horizon, for when it's on screen */

/* in the vblank a frame appears: its sky */
static void         sky_vblank(void)
{
    volatile u16    *tab = (volatile u16 *)(VDP2_VRAM + 0x7F000);
    const s32       *r = sky_ring[vdp_shown & 3];
    int             y, sky_horizon = (int)r[1], top = sky_horizon - sky_up;

    sky_prepare((int)r[0], sky_horizon + sky_h - sky_up, FOCAL);
    sky_commit();
    if (sky_line == sky_horizon)
        return;
    /* above the strip: from its top edge's colour to the zenith's, over about
       60 degrees; below it: its bottom edge's colour */
    for (y = 0; y < SCREEN_H; ++y)
    {
        if (y >= top + 2)
            tab[y] = sky_below;
        else
        {
            int t = iclamp((top + 2 - y) * 256 / (FOCAL * 2), 0, 256), k;
            u16 c = 0x8000;

            for (k = 0; k < 15; k += 5)
            {
                int a = (sky_above >> k) & 31, b = (sky_zenith >> k) & 31;

                c |= (u16)((a + ((b - a) * t >> 8)) << k);
            }
            tab[y] = c;
        }
    }
    sky_line = sky_horizon;
}

/* the sky's colours at the brightness chosen: its palettes, and the gradient's above and below it */
static __attribute__((cold)) void sky_colours(void)
{
    if (sky_ok)
        sky_set_colours(r_gamma, &sky_above, &sky_below, &sky_zenith);
}

/* the level's sky (tools/bake_sky.py: map's name, .SKY), from the disc straight into VDP2 */
__attribute__((cold)) void render_sky_init(const char *map)
{
    char            file[16];
    int             i;

    for (i = 0; map[i] && map[i] != '.' && i < 11; ++i)
        file[i] = map[i];
    memcpy(file + i, ".SKY", 5);
    REG16(VDP2_REG + 0x0E) = 0x0300;        /* RAMCTL: banks A and B split (the sky's in B0 and B1) */
    sky_ok = sky_load(file);
    if (!sky_ok)
    {
        sky_enable(false);
        return;
    }
    sky_h = sky_rows();
    sky_up = sky_rows_above();
    sky_line = -9999;
    sky_colours();
    sky_enable(true);
    vdp_set_vblank_hook(sky_vblank);
}

void                render_sky(void)
{
    s32             c = fcos(cam.pitch), *r = sky_ring[vdp_frame_no() & 3];

    if (!sky_ok)
        return;
    /* the horizon's screen line: straight ahead at infinity (put on screen with this frame) */
    r[0] = cam.yaw;
    r[1] = CY - (s32)(((s64)FOCAL * fsin(cam.pitch)) / (c ? c : 1));
}
