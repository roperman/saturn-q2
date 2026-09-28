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
#define MAX_MVERTS      (256)
#define MAX_MPOLYS      (320)
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
    vdp1_cmd        *cmd;
    u32             link, prev;
    u32             *gst;
    u32             grda;
    const q_cell    *cell0;                 /* the deferred cells' numbers count from here */
    /* small crops: their grid cells' texels (the face's edge columns and rows are narrower) */
    u32             cl0, cr1, ct0, cb1;     /* the first column's left, the last's right; the rows' */
    u32             rows0, N;               /* rows at the start (the first's rows == this) */
    const s32       *rcp;
    const u16       *slot_lut;
    const u8        *slot_w;
    u16             def[2 * MAX_ROW];
}                   cell_args;
_Static_assert(__builtin_offsetof(cell_args, cell0) == 96 && __builtin_offsetof(cell_args, cl0) == 100
               && __builtin_offsetof(cell_args, slot_w) == 132, "src/cells.s: C_ROW, C_CROP");
void                cells_asm(cell_args *a);
bool                r_cells_asm = true;
bool                r_nosplit;              /* (for comparing: whole tiles near the camera in one piece) */
bool                r_dl_verts = true;      /* dynamic lights added once a grid point (not at each cell's corners) */
#ifdef DL_CHECK
u32                 dl_checks, dl_diffs;    /* (OPT=-DDL_CHECK: the corners' sums as well, compared) */
#endif
#ifndef LOD_Z
#define LOD_Z           (384)               /* a face wholly beyond this uses its coarse grid (OPT=-DLOD_Z=...) */
#endif
s32                 r_lod_z = FIX(LOD_Z);
#ifndef MODEL_FAR
#define MODEL_FAR       (400)               /* a model beyond this (units) uses its coarse mesh */
#endif
int                 r_model_far = MODEL_FAR;
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
    v3              fo;                     /* the face's first grid point (its steps: ga, gk) */
    int             fnu, fnv;
    v3              dut, dvt;               /* the face's axes, one stored texel apart (view space) */
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
    bool            full;
    u16             *tex_slot;              /* per texture: its slot or 0xFFFF */
    const void      *up_src[UPQ];           /* textures to copy into VRAM at the frame's end (tex_upload) */
    u32             up_dst[UPQ];            /* VRAM offset << 8 | longs */
    int             nup;
}                   r_ctx;

r_stats             rs;
int                 r_debug;                /* profiling: 1 = stop after culling, 2 = after projecting, 3 = after the texture */
bool                r_two_cpus;
q_cam               cam;

static r_ctx        ctx[2];
static u16          frame = 1, visframe;
static u16          *node_vis, *leaf_vis;
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
static struct { s32 x, y, z, r2, inv; u8 r, g, b, pad; } dl[MAX_DLIGHTS];
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
static u16          slot_lut[MAX_SLOTS];    /* a world texture's record (q_tex's lut, w) while it has the slot: */
static u8           slot_w[MAX_SLOTS];      /* the records are on the cart, 75 cycles a miss */
static s32          kx, ky;                 /* CX / FOCAL, CY / FOCAL (16.16): the frustum's slopes */
static s32          rcp[64];                /* 65536 / n */
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
#define DSPM_BLOCKS     (64)                /* 16 vertices each */
static u32          dspm_stream[DSPM_MODELS * 24] __attribute__((aligned(16)));
static s32          dspm_out[DSPM_BLOCKS * 48] __attribute__((aligned(16)));
static u32          dspm_count __attribute__((aligned(16)));
static s16          ent_dsp[MAX_ENTITIES], ent_blk[MAX_ENTITIES];  /* its place in the DSP's list (or -1), its first block */
bool                r_use_dsp, r_dsp_ok;
static s32          *axis_view;             /* the axes in view space: du and its frame, dv and its (draw_face) */


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

void                render_init(void)
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
    memcpy(vram + base, lv.luts, (u32)lv.nluts * 32);
    base += (u32)lv.nluts * 32;
    free -= (u32)lv.nluts * 32;
    ntex_all = lv.ntextures;
    for (c = 0; c < nmodels_loaded; ++c)
    {
        q_mdl   *md = &models[c];
        u32     n = (u32)(md->nskins * md->ntex), nl = (u32)(md->nskins * md->nluts);

        if (!md->loaded)
            continue;
        md->tex_id0 = ntex_all;
        md->lut0 = (int)((base - lut_vram) / 32);
        ntex_all += (int)n;
        memcpy(vram + base, md->luts, nl * 32);
        base += nl * 32;
        free -= nl * 32;
    }
    /* the rest: tile-sized slots, half each */
    slot_bytes = (u32)(lv.N * lv.N / 2);
    slot_vram = base;
    i = (int)(free / slot_bytes);
    if (i > MAX_SLOTS)
        i = MAX_SLOTS;
    vdp_tex_release(base + (u32)i * slot_bytes);
    for (c = 0; c < 2; ++c)
    {
        r_ctx *x = &ctx[c];

        x->slot0 = c * (i / 2);
        x->nslots = i / 2;
        x->hand = 0;
        x->tex_slot = level_alloc((u32)ntex_all * 2);
        memset(x->tex_slot, 0xFF, (u32)ntex_all * 2);
    }
    for (c = 0; c < MAX_SLOTS; ++c)
        slot_tex[c] = 0xFFFF;
}

/* a texture into this CPU's part of the cache: -1 if no slot is free */
/* A texture into its slot: queued for the SCU's DMA at the frame's end (the
   cart to VDP1's VRAM on the SCU's own buses, not the CPUs'; a CPU's store
   to VRAM is 111 cycles), or copied now if the queue's full */
static void         tex_upload(r_ctx *x, u32 vram, const void *src, u32 bytes)
{
    if (r_dma_uploads && x->nup < UPQ && !(((u32)src | vram | bytes) & 3))
    {
        x->up_src[x->nup] = src;
        x->up_dst[x->nup++] = vram << 8 | bytes >> 2;
    }
    else
        memcpy((u8 *)VDP1_VRAM + vram, src, bytes);
}

s32                 tex_load(r_ctx *x, int t);      /* (src/mdraw.s calls it) */

__attribute__((noinline)) s32 tex_load(r_ctx *x, int t)
{
    int             s = 0, n;
    const q_tex     *tx;

    if (x->full)
        return -1;
    for (n = 0; n < x->nslots; ++n)
    {
        s = x->slot0 + x->hand;
        if (++x->hand == x->nslots)
            x->hand = 0;
        if (slot_tex[s] == 0xFFFF || (u16)(frame - slot_frame[s]) >= 3)     /* (two frames may be in flight) */
            break;
    }
    if (n == x->nslots)
    {
        x->full = true;
        ++x->st.nocache;
        return -1;
    }
    if (slot_tex[s] != 0xFFFF)
        x->tex_slot[slot_tex[s]] = 0xFFFF;
    slot_tex[s] = (u16)t;
    x->tex_slot[t] = (u16)s;
    slot_frame[s] = frame;
    if (t < lv.ntextures)
    {
        tx = &lv.textures[t];
        slot_lut[s] = tx->lut;
        slot_w[s] = (u8)tx->w;
        tex_upload(x, slot_vram + (u32)s * slot_bytes, lv.texdata + tx->ofs, (u32)tx->w * tx->h / 2);
    }
    else
    {
        /* a model's: which one, which skin, which polygon's */
        int m;

        ++x->st.muploads;
        for (m = 0; m < nmodels_loaded; ++m)
        {
            const q_mdl *md = &models[m];
            int         k = t - md->tex_id0;

            if (md->loaded && k >= 0 && k < md->nskins * md->ntex)
            {
                const q_mtex *mt = &md->tex[k % md->ntex];

                tex_upload(x, slot_vram + (u32)s * slot_bytes,
                           md->texdata + (u32)(k / md->ntex) * md->per_skin + mt->ofs, (u32)mt->w * mt->h / 2);
                break;
            }
        }
    }
    ++x->st.uploads;
    return (s32)(slot_vram + (u32)s * slot_bytes);
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

/* is a leaf in the camera's PVS? (for the AI: can't see what isn't) */
bool                r_leaf_in_pvs(int leaf)
{
    return leaf >= 0 && leaf_vis[leaf] == visframe;
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

    if (i >= WRITER_CMDS)
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
    d[1] = (u32)CELL_PMOD << 16 | ((lut_vram + lut * 32) >> 3);
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

/* the four corners of cell (i, j): A B C D */
static __attribute__((noinline)) void cell_pos(const r_ctx *x, int i, int j, v3 *P)
{
    const v3        *f = (const v3 *)&x->gk[GK_F0];     /* f0, dv, f1 */
    v3              a0, a1, b0, b1;

    grid_step(&a0, &x->ga.e0, &x->ga.d, &x->ga.e1, x->fnu, i);
    grid_step(&a1, &x->ga.e0, &x->ga.d, &x->ga.e1, x->fnu, i + 1);
    grid_step(&b0, &f[0], &f[1], &f[2], x->fnv, j);
    grid_step(&b1, &f[0], &f[1], &f[2], x->fnv, j + 1);
    P[0].x = x->fo.x + a0.x + b0.x; P[0].y = x->fo.y + a0.y + b0.y; P[0].z = x->fo.z + a0.z + b0.z;
    P[1].x = x->fo.x + a1.x + b0.x; P[1].y = x->fo.y + a1.y + b0.y; P[1].z = x->fo.z + a1.z + b0.z;
    P[2].x = x->fo.x + a1.x + b1.x; P[2].y = x->fo.y + a1.y + b1.y; P[2].z = x->fo.z + a1.z + b1.z;
    P[3].x = x->fo.x + a0.x + b1.x; P[3].y = x->fo.y + a0.y + b1.y; P[3].z = x->fo.z + a0.z + b1.z;
}

/* Row j of the face's lights with the dynamic lights added: at the grid
   points cell_pos finds (the same steps, summed the same way), so each
   corner comes out as cell_emit's did, once a point rather than once a
   corner (up to four times a point). Out to out[0..fnu] */
static __attribute__((noinline)) void dl_row(const r_ctx *x, u16 *out, const u16 *raw, int j)
{
    const v3        *f = (const v3 *)&x->gk[GK_F0];
    v3              a, b, V;
    int             i, n = x->fnu;

    grid_step(&b, &f[0], &f[1], &f[2], x->fnv, j);
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
        V.x = x->fo.x + a.x + b.x; V.y = x->fo.y + a.y + b.y; V.z = x->fo.z + a.z + b.z;
        out[i] = dlight_add(x->dmask, &V, raw[i]);
    }
}

#ifdef DL_CHECK
/* (the old way, each cell's corners, against rows j and j + 1 of the lit lights) */
static void         dl_check(const r_ctx *x, const u16 *lit, const u16 *raw, int stride, int j)
{
    int             i;
    v3              P[4];

    for (i = 0; i < x->fnu; ++i)
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
    d[1] = (u32)CELL_PMOD << 16 | ((lut_vram + sc->lut * 32) >> 3);
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
        const v3 *du = &x->dut, *dv = &x->dvt;
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
static __attribute__((noinline)) void grid_row(gv *row, int n, const v3 *p, const v3 *du, const v3 *e0, const v3 *e1, s32 dtx, s32 dty)
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

/* src/cells.s: what it needs for this face (and this frame's texture cache) */
static void         cells_face(r_ctx *x, int stride, const q_cell *cell0)
{
    cell_args       *a = &x->ca;

    a->cell0 = cell0;
    a->tex_slot = x->tex_slot;
    a->slot_frame = slot_frame;
    a->frame = frame;
    a->sb8 = slot_bytes >> 3;
    a->base8 = slot_vram >> 3;
    a->lut8 = lut_vram >> 3;
    a->pmod = (u32)CELL_PMOD << 16;
    a->ctrl = 0x10020000u;                  /* jump assign, distorted sprite */
    a->fifo = x->fifo;
    a->stride2 = (u32)stride * 2;
    a->fast = CELL_FULL | CELL_EXACT;
    a->near24 = (u32)(OC_NEAR | OC_FAR) << 24;
    a->N = (u32)lv.N;
    a->rcp = rcp;
    a->slot_lut = slot_lut;
    a->slot_w = slot_w;
}

/* rows of cells in assembly (a whole face's, or one), the writer's lists
   passed in and brought up to date after: returns how many it left for the C
   (their numbers, from a->cell0, in x->ca.def) */
static int          cells_run(r_ctx *x, const gv *top, const gv *bot, const q_cell *cell, const u16 *light, int n,
                              int rows)
{
    cell_args       *a = &x->ca;
    vdp_writer      *w = x->w;
    int             b = x->bucket, c0 = w->count, made;

    a->n = (u32)n;
    a->rows = (u32)rows;
    a->defp = a->def;
    a->top = top;
    a->bot = bot;
    a->cell = cell;
    a->light = light;
    a->cmd = &w->cmds[c0];
    a->link = w->link_base + (u32)c0 * (sizeof(vdp1_cmd) >> 3);
    if (x->fifo)
        a->prev = w->tail[b] >= 0 ? (u32)&w->cmds[w->tail[b]] : 0;
    else
        a->prev = w->head[b] >= 0 ? (u32)(w->link_base + w->head[b] * (sizeof(vdp1_cmd) >> 3)) : 0;
    a->gst = w->gst + w->gcount * 2;
    a->grda = (w->gbase >> 3) + (u32)w->gcount;
    cells_asm(a);
    made = (int)(a->cmd - &w->cmds[c0]);
    if (made)
    {
        if (x->fifo)
        {
            if (w->head[b] < 0)
                w->head[b] = (s16)c0;
            w->tail[b] = (s16)(c0 + made - 1);
        }
        else
        {
            if (w->head[b] < 0)
                w->tail[b] = (s16)c0;
            w->head[b] = (s16)(c0 + made - 1);
        }
        w->count = c0 + made;
        w->gcount += made;
    }
    return (int)(a->defp - a->def);
}

/* The assembly row against the C one (the reference): rows across the view,
   off its edges, and into the near plane. Any difference and the C one's used. */
int                 grid_bad;

static void         grid_selftest(void)
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

/* One cell, the C way: what the assembly leaves (near the camera, cropped
   inside its grid cell, its texture to load), and every cell when there's a
   dynamic light near (or the assembly's off). top, bot: its grid rows;
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
        dw[1] = (u32)CELL_PMOD << 16
              | ((lut_vram + (u32)((const q_cell_fast *)cell)->lut * 32) >> 3);
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


/* A face's grid and its cells (draw_face's setup chose which grid: the face's,
   or its coarse one): its own function, so that the setup's choosing doesn't
   cost the compiler registers in this, the part that counts */
typedef struct
{
    v3              o, du, dv;              /* the first point, a cell along u and v: view space */
    int             nu, nv, eu0, eu1, ev0, ev1;
    const q_cell    *cells;
    const u16       *light;
}                   face_grid;

static __attribute__((noinline)) void draw_grid(r_ctx *x, const face_grid *gp, const q_face *f, int model)
{
    v3              o = gp->o, du = gp->du, dv = gp->dv, rowp, e0, e1, f0, f1;
    s32             dtx, dty;
    gv              *top = x->grid, *bot = x->grid + MAX_ROW, *tmp;
    const u16       *light = gp->light;
    int             i, j, nu = gp->nu, nv = gp->nv, N = lv.N, stride = nu + 1;
    int             eu0 = gp->eu0, eu1 = gp->eu1, ev0 = gp->ev0, ev1 = gp->ev1, ct, cb, k;
    bool            whole, fast_ok;
    const q_cell    *row_cells = gp->cells;
    vdp_writer      *w = x->w;
    int             count0 = x->w->count;
    u16             lit[2 * MAX_ROW];       /* the lights with the dynamic lights added: the whole face's, or two rows */
#ifdef R_PROFILE
    u32             pt = frt_read(), pt2;
    int             gc0 = x->w->gcount;
#endif

    /* the dynamic lights close enough to its plane (and in front of it) */
    x->dmask = 0;
    for (i = 0; i < r_ndlights; ++i)
    {
        const q_plane   *pl = &lv.planes[f->plane];
        const s32       *lp = r_dlights[i].pos, *o = mover_ofs[model];
        s32             dist = fmul(lp[0] - o[0], pl->n[0]) + fmul(lp[1] - o[1], pl->n[1])
                             + fmul(lp[2] - o[2], pl->n[2]) - pl->dist;

        if (f->flags & FF_BACK)
            dist = -dist;
        if (dist > -FIX(8) && dist < r_dlights[i].radius)
            x->dmask |= (u8)(1 << i);
    }
    /* one stored texel along each axis: times 1/N (a variable shift would be a library call) */
    x->dut.x = fmul(du.x, rcp_n); x->dut.y = fmul(du.y, rcp_n); x->dut.z = fmul(du.z, rcp_n);
    x->dvt.x = fmul(dv.x, rcp_n); x->dvt.y = fmul(dv.y, rcp_n); x->dvt.z = fmul(dv.z, rcp_n);
    dtx = fmul(du.z, kx);
    dty = fmul(du.z, ky);
    /* the steps into and out of the face's edge columns and rows (a whole tile's if it isn't cropped) */
    k = (nu == 1 ? eu1 : N) - eu0;
    e0.x = x->dut.x * k; e0.y = x->dut.y * k; e0.z = x->dut.z * k;
    e1.x = x->dut.x * eu1; e1.y = x->dut.y * eu1; e1.z = x->dut.z * eu1;
    k = (nv == 1 ? ev1 : N) - ev0;
    f0.x = x->dvt.x * k; f0.y = x->dvt.y * k; f0.z = x->dvt.z * k;
    f1.x = x->dvt.x * ev1; f1.y = x->dvt.y * ev1; f1.z = x->dvt.z * ev1;

    /* the grid's steps, where the assembly (and grid_pos) find them */
    {
        s32 *gk = x->gk;

        x->ga.e0 = e0; x->ga.d = du; x->ga.e1 = e1;
        gk[GK_F0] = f0.x; gk[GK_F0 + 1] = f0.y; gk[GK_F0 + 2] = f0.z;
        gk[GK_F0 + 3] = dv.x; gk[GK_F0 + 4] = dv.y; gk[GK_F0 + 5] = dv.z;
        gk[GK_F0 + 6] = f1.x; gk[GK_F0 + 7] = f1.y; gk[GK_F0 + 8] = f1.z;
        x->fo = o;
        x->fnu = nu;
        x->fnv = nv;
    }
    PROF((pt2 = frt_read(), x->st.p_setup += (pt2 - pt) & 0xFFFF, pt = pt2));
    /* the grid: the whole face at once if it fits (the assembly), or a row at a time */
    whole = r_grid_asm && (nu + 1) * (nv + 1) <= 2 * MAX_ROW;
    if (whole)
    {
        x->ga.p = o;
        x->gk[GK_ROWS] = nv;
        grid_face_asm(top, nu + 1, &x->ga, x->gk);
        bot = top + stride;
    }
    else if (r_grid_asm)
    {
        x->ga.p = o;
        grid_row_asm(top, nu + 1, &x->ga, grid_k);
    }
    else
        grid_row(top, nu + 1, &o, &du, &e0, &e1, dtx, dty);
    rowp = o;
    PROF((x->st.gverts += (nu + 1) * (nv + 1), x->st.seen += nu * nv));
    PROF((pt2 = frt_read(), x->st.p_grid += (pt2 - pt) & 0xFFFF, pt = pt2));
    /* dynamic lights: added to the lights at each grid point, then drawn as any other face */
    x->lit = x->dmask && r_dl_verts;
    if (x->lit)
    {
        x->raw_light = light;
        if (whole)
        {
            for (j = 0; j <= nv; ++j)
                dl_row(x, lit + j * stride, light + j * stride, j);
#ifdef DL_CHECK
            for (j = 0; j < nv; ++j)
                dl_check(x, lit + j * stride, light + j * stride, stride, j);
#endif
            light = lit;
        }
        else
            dl_row(x, lit, light, 0);       /* (row 0; each row's next as it comes) */
    }
    fast_ok = r_cells_asm && (!x->dmask || x->lit) && !r_debug;
    if (fast_ok)
        cells_face(x, stride, row_cells);
    if (whole && fast_ok && w->count + nu * nv <= WRITER_CMDS && w->gcount + nu * nv <= w->gmax)
    {
        /* the common cells of the whole face in assembly, then the C for what it left */
        int n, li;

        x->ca.cl0 = (u32)eu0; x->ca.cr1 = (u32)eu1; x->ca.ct0 = (u32)ev0; x->ca.cb1 = (u32)ev1;
        x->ca.rows0 = (u32)nv;
        n = cells_run(x, top, top + stride, row_cells, light, nu, nv);

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
        const v3    *step = j == 0 ? &f0 : j == nv - 1 ? &f1 : &dv;
        int         n = nu, li;
        const u16   *list = cell_all, *lrow = light;

        ct = j == 0 ? ev0 : 0;
        cb = j == nv - 1 ? ev1 : N;
        if (!whole)
        {
            rowp.x += step->x; rowp.y += step->y; rowp.z += step->z;
            if (r_grid_asm)
            {
                x->ga.p = rowp;
                grid_row_asm(bot, nu + 1, &x->ga, grid_k);
            }
            else
                grid_row(bot, nu + 1, &rowp, &du, &e0, &e1, dtx, dty);
        }
        if (x->lit && !whole)
        {
            /* the lit lights: rows j and j + 1 */
            dl_row(x, lit + stride, x->raw_light + (j + 1) * stride, j + 1);
#ifdef DL_CHECK
            dl_check(x, lit, x->raw_light + j * stride, stride, j);
#endif
            lrow = lit;
        }
        /* a row at a time: the common cells in assembly if there's room in the lists */
        if (fast_ok && w->count + nu <= WRITER_CMDS && w->gcount + nu <= w->gmax)
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
    x->st.cells += x->w->count - count0;    /* once a face, not a store a cell */
#ifdef R_PROFILE
    {
        /* (profile: Gouraud tables the same as the one before, and flat ones) */
        const u32   *gs = x->w->gst;
        int         k;

        for (k = gc0 + 1; k < x->w->gcount; ++k)
        {
            x->st.g_same += gs[k * 2] == gs[k * 2 - 2] && gs[k * 2 + 1] == gs[k * 2 - 1];
            x->st.g_flat += gs[k * 2] == gs[k * 2 + 1] && (gs[k * 2] >> 16) == (gs[k * 2] & 0xFFFF);
        }
    }
#endif
}


static __attribute__((noinline)) void draw_face(r_ctx *x, int fi, int model)
{
    const q_face    *f = &lv.faces[fi];
    face_grid       g;
    v3              o, du, dv;
    s32             d[3];
    const u16       *light;
    int             nu = f->nu, nv = f->nv, eu0 = f->eu0, eu1 = f->eu1, ev0 = f->ev0, ev1 = f->ev1;
    const q_cell    *row_cells;
    s32             zmin;
#ifdef R_PROFILE
    u32             pxf;
#endif

    if (f->flags & (FF_SKY | FF_NODRAW) || nu + 1 > MAX_ROW)
        return;
    ++x->st.faces;
    PROF(pxf = frt_read());
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
        {
            PROF(++x->st.faces_out);
            PROF(x->st.p_xform += (frt_read() - pxf) & 0xFFFF);
            return;
        }
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
    PROF(x->st.p_xform += (frt_read() - pxf) & 0xFFFF);
    g.o = o; g.du = du; g.dv = dv;
    g.nu = nu; g.nv = nv; g.eu0 = eu0; g.eu1 = eu1; g.ev0 = ev0; g.ev1 = ev1;
    g.cells = row_cells;
    g.light = light;
    draw_grid(x, &g, f, model);
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

/* the frame's models for the DSP: those in leaves the view can see, and not
   all off the screen, weighted for their blends; started at once */
static int          rs_mdsp;                /* (stats: models the DSP was given this frame) */

static void         models_to_dsp(void)
{
    u32             *st = dspm_stream;
    int             i, k, nm = 0, nb = 0;

    rs_mdsp = 0;
    for (i = 0; i < nents; ++i)
        ent_dsp[i] = -1;
    if (!r_use_dsp)
        return;
    dsp_wait();                             /* (last frame's list, if a model it had wasn't drawn) */
    for (i = 0; i < nents; ++i)
    {
        const q_entity  *e = &ents[i];
        const q_mdl     *m = e->mdl;
        model_xform     xf;
        s32             w1 = e->lerp, w0 = FIX(1) - w1;
        int             blocks;

        if (ent_leaf[i] < 0 || leaf_vis[ent_leaf[i]] != visframe || !m)
            continue;
        blocks = (imin(m->nverts, MAX_MVERTS) + 15) >> 4;
        if (nm == DSPM_MODELS || nb + blocks > DSPM_BLOCKS)
            break;
        if (!model_xf(e, &xf))
            continue;
        for (k = 0; k < 3; ++k, st += 7)
        {
            st[0] = (u32)(fmul(xf.C0[k], w0) + fmul(xf.C1[k], w1));
            st[1] = (u32)fmul(xf.A0[k][0], w0); st[2] = (u32)fmul(xf.A0[k][1], w0); st[3] = (u32)fmul(xf.A0[k][2], w0);
            st[4] = (u32)fmul(xf.A1[k][0], w1); st[5] = (u32)fmul(xf.A1[k][1], w1); st[6] = (u32)fmul(xf.A1[k][2], w1);
        }
        st[0] = ((u32)(m->frames + (u32)e->oldframe * m->frame_bytes + 24) & 0x07FFFFFF) >> 2;
        st[1] = ((u32)(m->frames + (u32)e->frame * m->frame_bytes + 24) & 0x07FFFFFF) >> 2;
        st[2] = (u32)blocks;
        st += 3;
        ent_dsp[i] = (s16)nm++;
        ++rs_mdsp;
        ent_blk[i] = (s16)nb;
        nb += blocks;
    }
    if (nm)
    {
        *(volatile u32 *)UNCACHED(&dspm_count) = 0;
        dsp_models(dspm_stream, dspm_out, nm, &dspm_count);
    }
}

#ifdef MODEL_CHECK
u32                 model_checks[4], model_diffs[3];   /* vertices, buckets (and quads by their other half), models' commands */
#endif
#ifdef COMPARE_MODELS
bool                r_model_ref;            /* (tools/compare.sh with COMPARE=models: UP, draw_model's old loops) */
#endif

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
}                   mpolys_args;

void                mpolys_asm(mpolys_args *a);

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
    u32             frame, svram, sbytes, lb, dw1, luts4;
    s32             gmax;
    u32             gb, fifo;
    r_ctx           *x;
    s32             cnt, head, tail, gc, drop;
    const s16       *mhead;
    s32             wcmds;
}                   mcmds_args;

void                mcmds_asm(mcmds_args *a);
_Static_assert(__builtin_offsetof(mcmds_args, luts4) == 60 && __builtin_offsetof(mcmds_args, cnt) == 80
               && __builtin_offsetof(mcmds_args, wcmds) == 104, "mdraw.s: mcmds_args");
_Static_assert(__builtin_offsetof(r_ctx, mnext) == __builtin_offsetof(r_ctx, mhead) + MBUCKETS * 2, "mdraw.s: mnext after mhead");

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
    int             i, b, nv = imin(m->nverts, MAX_MVERTS), np = imin(m->npolys, MAX_MPOLYS);
#if defined(COMPARE_MODELS) || defined(MODEL_CHECK)
    int             bi;                     /* (the C command passes) */
#endif
    const q_mpoly   *polys = m->polys;
    const u16       *vlist = NULL;          /* far: only the vertices the coarse mesh uses */
    int             nvl = nv, vi;
    vdp_writer      *w = x->w;
    u32             t0 = frt_read();

    if (ent_dsp[ei] < 0 && !model_xf(e, &xf))
        return;
    ++x->st.models;
    x->st.mcpu += ent_dsp[ei] < 0;
    {
        /* far: the mesh merged on a coarse grid (a third of the polygons), if it has one */
        s32 dx = (e->origin[0] - cam.pos[0]) >> 16, dy = (e->origin[1] - cam.pos[1]) >> 16;

        if (m->nfpolys && dx * dx + dy * dy > r_model_far * r_model_far)
        {
            polys = m->fpolys;
            np = imin(m->nfpolys, MAX_MPOLYS);
            vlist = m->fverts;
            nvl = m->nfverts;
        }
    }
    /* its light by normal: the base (ents_light), plus any dynamic lights
       near it, stronger on the side facing them */
    gt = e->gbase;
    for (i = 0; i < ndl; ++i)
    {
        const q_dlight  *l = &r_dlights[i];
        s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
        s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16, d2 = dx * dx + dy * dy + dz * dz;
        s32             f, len, mx, my, mz;
        const s16       *nrm = m->normals;
        int             n;

        if (d2 >= r * r)
            continue;
        f = ((r * r - d2) * dl[i].inv) >> 8;            /* 0..65536 at the origin */
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
    if (ent_dsp[ei] >= 0)
    {
        /* on the DSP (models_to_dsp): wait if it's not there yet, have the cache
           forget what it had where the DSP wrote, and read them, blended already */
        const s32   *out = dspm_out + ent_blk[ei] * 48;
        u32         a, end = (u32)(out + ((nv + 15) >> 4) * 48);

        u32         tw = frt_read();

        while (*(volatile u32 *)UNCACHED(&dspm_count) <= (u32)ent_dsp[ei])
            ;
        x->st.t_mwait += (frt_read() - tw) & 0xFFFF;
        for (a = (u32)out; a < end; a += 16)
            *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;      /* the cache's associative purge */
#ifdef COMPARE_MODELS
        if (r_model_ref)
        for (vi = 0; vi < nvl; ++vi)
        {
            const s32   *o;

            i = vlist ? vlist[vi] : vi;
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
        if (!vlist)
        {
            /* the whole mesh: in assembly (src/mdraw.s), then each vertex's
               light by its normal (its index is in the frame, on the cart) */
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
        else for (vi = 0; vi < nvl; ++vi)
        {
            /* the far mesh: only the vertices it uses */
            const s32   *o;

            i = vlist[vi];
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
    }
    else for (vi = 0; vi < nvl; ++vi)
    {
        s32 p[3], vx, vy, vz;
        u8  oc = 0;
        const u8 *q0, *q1;

        i = vlist ? vlist[vi] : vi;
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
            int             lut = m->lut0 + e->skin * m->nluts + (m->nluts > 1 ? p->tex : 0);
            s32             vram = tex_vram(x, m->tex_id0 + id);
            u32             *dw, link;
            u16             grda;

            if (vram < 0 || !(dw = cmd_alloc(x, &link)))
                continue;
            dw[0] = 0x10020000u | link;
            dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | ((lut_vram + (u32)lut * 32) >> 3);
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
        u32         colr0 = lut_vram + (u32)(m->lut0 + e->skin * m->nluts) * 32;
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
        a.dw1 = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | colr0 >> 3;
        a.luts4 = m->nluts > 1 ? 4 : 0;         /* (colr0 is a multiple of 32) */
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
        a.wcmds = WRITER_CMDS;
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
        u32         lb = w->link_base, colr0 = lut_vram + (u32)(m->lut0 + e->skin * m->nluts) * 32;
        bool        luts = m->nluts > 1, fifo = x->fifo;
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
                if (cnt >= WRITER_CMDS)
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
                dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | ((colr0 + (luts ? (u32)p->tex * 32 : 0)) >> 3);
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

static void         part_begin(r_ctx *x)
{
    memset(&x->st, 0, sizeof(x->st));
    x->full = false;
    x->nup = 0;
}

void                render_slave(void)
{
    r_ctx           *x = &ctx[1];
    u32             t0 = frt_read();
    int             lo = 0;

    part_begin(x);
    for (;;)
    {
        int n = SHARE->published, hi = SHARE->hi, done = SHARE->walk_done;

        if (lo < n && lo < hi)
        {
            SHARE->lo = lo + 1;             /* claim it, then draw it */
            draw_item(x, lo++, true);
        }
        else if (done && (lo >= hi || lo >= SHARE->published))
            break;
    }
    x->st.t_face = frt_to_us((frt_read() - t0) & 0xFFFF);
}

static void         draw_master(void)
{
    r_ctx           *x = &ctx[0];
    u32             t0 = frt_read();
    int             i;

    part_begin(x);
    if (!r_two_cpus)
    {
        /* on its own: all of it, pushed nearest first */
        x->fifo = false;
        for (i = 0; i < nvis; ++i)
            draw_item(x, i, false);
    }
    else
    {
        int hi = nvis;

        x->fifo = true;
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
    /* this frame's lights, into view space; the sprites, into the leaves they're in */
    ndl = imin(r_ndlights, MAX_DLIGHTS);
    for (i = 0; i < ndl; ++i)
    {
        const q_dlight  *l = &r_dlights[i];
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
    /* the models' vertices: the DSP starts on them now */
    models_to_dsp();
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
    /* (OPT=-DGAME_DURING_DRAW: the game's tick here, on the master, while the
       slave draws from the front of the list; the master then draws from the
       back until they meet, so the slave takes more of it) */
    if (r_during)
        r_during();
    draw_master();
    if (r_two_cpus)
        wait_signal();
    /* both done: the textures they queued into VRAM (the slave's queue read
       uncached: it wrote it), before VDP1 can draw them (after the swap) */
    for (i = 0; i < 2; ++i)
    {
        r_ctx       *c = i ? (r_ctx *)UNCACHED(&ctx[1]) : &ctx[0];
        int         n = c->nup, k;

        for (k = 0; k < n; ++k)
        {
            u32 d = c->up_dst[k];

            scu_dma0((void *)(VDP1_VRAM + (d >> 8)), c->up_src[k], (d & 255) * 4, true);
            while (scu_dma0_busy())
                ;
#ifdef UPLOAD_CHECK
            {
                /* (OPT=-DUPLOAD_CHECK: read back and compared) */
                extern u32 upload_checks, upload_diffs;

                ++upload_checks;
                if (memcmp((const void *)(VDP1_VRAM + (d >> 8)), c->up_src[k], (d & 255) * 4))
                    ++upload_diffs;
            }
#endif
        }
        c->nup = 0;
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
        rs.t_mfar += frt_to_us(s->t_mfar);
        rs.mfar += s->mfar;
        rs.t_mlight += frt_to_us(s->t_mlight);
        rs.t_mverts += frt_to_us(s->t_mverts);
        rs.t_mpolys += frt_to_us(s->t_mpolys);
        rs.t_mwait += frt_to_us(s->t_mwait);
        rs.t_masm += frt_to_us(s->t_masm);
        rs.t_mnorm += frt_to_us(s->t_mnorm);
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

static int          sky_h;
static u16          sky_above, sky_below, sky_zenith;
static volatile int sky_line = -9999;       /* the horizon the back colour table was last built for */
static s32          sky_ring[4][2];         /* each frame's yaw and horizon, for when it's on screen */

/* in the vblank a frame appears: its sky */
static void         sky_vblank(void)
{
    volatile u16    *tab = (volatile u16 *)(VDP2_VRAM + 0x7F000);
    const s32       *r = sky_ring[vdp_shown & 3];
    int             y, sky_horizon = (int)r[1], top = sky_horizon - sky_h / 2;

    sky_prepare((int)r[0], sky_horizon + sky_h / 2, FOCAL);
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

void                render_sky_init(void)
{
    const u16       *s = lv.sky;

    if (!s)
        return;
    sky_h = s[1];
    sky_above = s[18];
    sky_below = s[19];
    sky_zenith = s[20];
    REG16(VDP2_REG + 0x0E) = 0x0300;        /* RAMCTL: banks A and B split; the sky's in B1 */
    sky_init((const u8 *)(s + 21), s[0], sky_h, s + 2);
    sky_enable(true);
    vdp_set_vblank_hook(sky_vblank);
}

void                render_sky(void)
{
    s32             c = fcos(cam.pitch), *r = sky_ring[vdp_frame_no() & 3];

    if (!lv.sky)
        return;
    /* the horizon's screen line: straight ahead at infinity (put on screen with this frame) */
    r[0] = cam.yaw;
    r[1] = CY - (s32)(((s64)FOCAL * fsin(cam.pitch)) / (c ? c : 1));
}
