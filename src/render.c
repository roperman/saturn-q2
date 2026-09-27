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
#define MAX_SLOTS       (4096)
#define MAX_VIS         (4096)
#define CLAMP_XY        (2000)

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
/* (src/grid.s builds these with ROTCL: that order) */
#define OC_NEAR         (16)
#define OC_LEFT         (1)
#define OC_RIGHT        (2)
#define OC_TOP          (4)
#define OC_BOTTOM       (8)

typedef struct { s32 x, y, z; } v3;
/* a grid vertex: view space, the frustum planes it's outside, and (once
   something needs it) its screen position, x << 16 | y */
typedef struct { s32 x, y, z; u32 xy; union { struct { u8 oc, done; }; u16 ocd; }; u16 pad; } gv;

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
static s32          grid_k[8];              /* NEAR_Z, FOCAL, CX, CY, SCREEN_W, SCREEN_H, ky, CLAMP_XY */
static bool         r_grid_asm;             /* it passed its test (grid_selftest) */
static void         grid_selftest(void);

/* each CPU's own: its writer, scratch, statistics and half of the texture cache */
typedef struct
{
    vdp_writer      *w;
    int             bucket, first, last;    /* the part of the face list it draws */
    bool            fifo;                   /* commands appended (drawn in order) rather than pushed (reversed) */
    gv              grid[2 * MAX_ROW];      /* a face's grid, or two rows of a big one's */
    s32             gk[20];                 /* grid_k, and grid_face_asm's */
    grid_args       ga;
    v3              dut, dvt;               /* the face's axes, one stored texel apart (view space) */
    u8              dmask;                  /* the dynamic lights reaching this face */
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
static int          nvis, vis_cells;

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

/* Model vertices on the SCU DSP (engine/xformp.dsp): per CPU, the block
   headers it reads and the view-space results it writes. One DSP, two CPUs:
   a lock, taken with TAS.B (an atomic read-modify-write on the bus). */
#define DSP_MAXBLK      (32)                /* two frames of up to 256 vertices */
static u32          dsp_stream[2][DSP_MAXBLK * 13] __attribute__((aligned(16)));
static s32          dsp_out[2][DSP_MAXBLK * 48] __attribute__((aligned(16)));
static u8           dsp_lock_byte;
bool                r_use_dsp, r_dsp_ok;

static inline void  dsp_acquire(void)
{
    volatile u8     *p = (volatile u8 *)UNCACHED(&dsp_lock_byte);
    int             got;

    do
        __asm__ volatile ("tas.b @%1\n\tmovt %0" : "=r" (got) : "r" (p) : "t", "memory");
    while (!got);
}

static inline void  dsp_release(void)
{
    *(volatile u8 *)UNCACHED(&dsp_lock_byte) = 0;
}

/* the DSP self-test's findings (shown by main.c) */
s32                 dsp_test[8];

/* identity plus (100, 200, 300) on vertices (k, 2k, 3k): from work RAM, then from the cart */
static void         dsp_selftest(void)
{
    static u32      in[16] __attribute__((aligned(16)));
    u32             *cart_in = (u32 *)(0x02400000 + 0x3F0000);     /* near the cart's end */
    int             k, pass;

    for (k = 0; k < 16; ++k)
        in[k] = (u32)(k << 24 | (2 * k) << 16 | (3 * k) << 8);
    for (k = 0; k < 16; ++k)
        cart_in[k] = in[k];
    for (pass = 0; pass < 2; ++pass)
    {
        u32         *st = dsp_stream[0];
        const s32   *out = (const s32 *)UNCACHED(dsp_out[0]);
        u32         t;

        memset(dsp_out[0], 0xEE, 48 * 4);
        st[0] = FIX(100); st[1] = FIX(1); st[2] = 0; st[3] = 0;
        st[4] = FIX(200); st[5] = 0; st[6] = FIX(1); st[7] = 0;
        st[8] = FIX(300); st[9] = 0; st[10] = 0; st[11] = FIX(1);
        st[12] = ((u32)(pass ? cart_in : in) & 0x07FFFFFF) >> 2;
        dsp_blocks(dsp_stream[0], dsp_out[0], 1);
        for (t = 0; t < 2000000 && dsp_busy(); ++t)
            ;
        dsp_test[pass * 4 + 0] = dsp_busy() ? -1 : 1;                /* finished? */
        dsp_test[pass * 4 + 1] = out[5] >> 16;                       /* x of vertex 5: 105 */
        dsp_test[pass * 4 + 2] = out[16 + 5] >> 16;                  /* y: 210 */
        dsp_test[pass * 4 + 3] = out[32 + 5] >> 16;                  /* z: 315 */
        if (dsp_busy())
            dsp_init_packed();              /* stuck: reload (stops it) */
    }
    r_dsp_ok = dsp_test[0] == 1 && dsp_test[1] == 105 && dsp_test[2] == 210 && dsp_test[3] == 315
            && dsp_test[4] == 1 && dsp_test[5] == 105 && dsp_test[6] == 210 && dsp_test[7] == 315;
    /* It works, but used like this (the CPU waiting for it) it's slower than the
       CPU doing it: on only when asked (OPT=-DDSP_MODELS), until it runs alongside */
#ifdef DSP_MODELS
    r_use_dsp = r_dsp_ok;
#else
    r_use_dsp = false;
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
static bool         cull_box(const s16 *mins, const s16 *maxs)
{
    int             i;

    for (i = 0; i < 4; ++i)
    {
        const s32   *n = fr_n[i];
        u8          s = fr_sign[i];
        s32         d = n[0] * (s & 1 ? mins[0] : maxs[0])
                      + n[1] * (s & 2 ? mins[1] : maxs[1])
                      + n[2] * (s & 4 ? mins[2] : maxs[2]);

        if (d < fr_d[i])
            return true;
    }
    return false;
}

void                render_init(void)
{
    int             i, c;
    u32             base = vdp_tex_mark(), free = vdp_tex_free();
    u8              *vram = (u8 *)VDP1_VRAM;

    kx = CX * 65536 / FOCAL;
    ky = CY * 65536 / FOCAL;
    grid_k[0] = NEAR_Z; grid_k[1] = FOCAL; grid_k[2] = CX; grid_k[3] = CY;
    grid_k[4] = SCREEN_W; grid_k[5] = SCREEN_H; grid_k[6] = ky; grid_k[7] = CLAMP_XY;
    for (i = 0; i < 8; ++i)
        ctx[0].gk[i] = ctx[1].gk[i] = grid_k[i];
    grid_selftest();
    dsp_init_packed();
    dsp_selftest();
    rcp_n = 65536 / lv.N;
    wscale = 65536 / (lv.N * lv.N);
    for (i = 1; i < 64; ++i)
        rcp[i] = 65536 / i;
    face_vis = level_alloc((u32)(lv.nfaces + 7) >> 3);
    leaf_spr = level_alloc((u32)lv.nleafs * 2);
    memset(leaf_spr, 0xFF, (u32)lv.nleafs * 2);
    leaf_ent = level_alloc((u32)lv.nleafs * 2);
    memset(leaf_ent, 0xFF, (u32)lv.nleafs * 2);
    node_vis = level_alloc((u32)lv.nnodes * 2);
    leaf_vis = level_alloc((u32)lv.nleafs * 2);
    node_parent = level_alloc((u32)lv.nnodes * 2);
    leaf_parent = level_alloc((u32)lv.nleafs * 2);
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
static __attribute__((noinline)) s32 tex_load(r_ctx *x, int t)
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
        if (slot_tex[s] == 0xFFFF || (u16)(frame - slot_frame[s]) >= 2)
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
        memcpy((u8 *)VDP1_VRAM + slot_vram + (u32)s * slot_bytes, lv.texdata + tx->ofs, (u32)tx->w * tx->h / 2);
    }
    else
    {
        /* a model's: which one, which skin, which polygon's */
        int m;

        for (m = 0; m < nmodels_loaded; ++m)
        {
            const q_mdl *md = &models[m];
            int         k = t - md->tex_id0;

            if (md->loaded && k >= 0 && k < md->nskins * md->ntex)
            {
                const q_mtex *mt = &md->tex[k % md->ntex];

                memcpy((u8 *)VDP1_VRAM + slot_vram + (u32)s * slot_bytes,
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

        if (c < 0 || !(pvs[c >> 3] & (1 << (c & 7))))
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

/* which of the view frustum's planes a point is outside (no divide) */
static inline u8    view_oc(s32 x, s32 y, s32 z)
{
    s32             tx = fmul(z, kx), ty = fmul(z, ky);
    u8              oc = 0;

    if (z < NEAR_Z)
        oc |= OC_NEAR;
    if (x < -tx)
        oc |= OC_LEFT;
    else if (x > tx)
        oc |= OC_RIGHT;
    if (y > ty)
        oc |= OC_TOP;
    else if (y < -ty)
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
static u16          dlight_add(u8 mask, const gv *v, u16 c)
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
        f = ((dl[i].r2 - d2) * dl[i].inv) >> 8;     /* 0..65536 */
        r += (dl[i].r * f) >> 16;
        g += (dl[i].g * f) >> 16;
        b += (dl[i].b * f) >> 16;
    }
    return (u16)(0x8000 | (imin(b, 31) << 10) | (imin(g, 31) << 5) | imin(r, 31));
}

/* one cell's command: corners xy in the cell's order A (u0, v0), B (u1, v0), C, D */
static __attribute__((noinline)) void cell_emit(r_ctx *x, const q_cell *cell, int ty0, int th,
                                                const u32 *xy, const u16 *light, int stride, gv **g)
{
    int             t = cell->tex & 0x7FFF, s = x->tex_slot[t], tw;
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
    lut = cell->tex & CELL_FULL ? ((const q_cell_full *)cell)->lut : (u32)(tl & 0x7FFF);
    /* the command goes out in 32-bit stores */
    if (!(d = cmd_alloc(x, &link)))
        return;
    la = light[0];
    lb = light[1];
    lc = light[stride + 1];
    ld = light[stride];
    if (x->dmask)
    {
        la = dlight_add(x->dmask, g[0], la);
        lb = dlight_add(x->dmask, g[1], lb);
        lc = dlight_add(x->dmask, g[2], lc);
        ld = dlight_add(x->dmask, g[3], ld);
    }
    d[0] = 0x10020000u | link;              /* jump assign, distorted sprite */
    d[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16 | ((lut_vram + lut * 32) >> 3);
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

/* A cropped cell, or one crossing the near plane: its corners on screen (and
   maybe fewer rows of its texture). g: the grid vertices round it, which are
   cl..cr, ct..cb of its tile (in stored texels: the face's edge columns and
   rows are narrower); exact: the cell's all of that. false: not drawn */
static __attribute__((noinline)) bool cell_corners(r_ctx *x, const q_cell *cell, gv **g, u32 *xy, int *ty0, int *th,
                                                   int cl, int cr, int ct, int cb, bool exact)
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
    if (exact)
        for (k = 0; k < 4; ++k)
        {
            q[k].x = g[k]->x; q[k].y = g[k]->y; q[k].z = g[k]->z;
        }
    else
    {
        const v3 *du = &x->dut, *dv = &x->dvt;
        const gv *o = g[0];

        q[0].x = o->x + du->x * u0 + dv->x * v0; q[0].y = o->y + du->y * u0 + dv->y * v0; q[0].z = o->z + du->z * u0 + dv->z * v0;
        q[1].x = o->x + du->x * u1 + dv->x * v0; q[1].y = o->y + du->y * u1 + dv->y * v0; q[1].z = o->z + du->z * u1 + dv->z * v0;
        q[2].x = o->x + du->x * u1 + dv->x * v1; q[2].y = o->y + du->y * u1 + dv->y * v1; q[2].z = o->z + du->z * u1 + dv->z * v1;
        q[3].x = o->x + du->x * u0 + dv->x * v1; q[3].y = o->y + du->y * u0 + dv->y * v1; q[3].z = o->z + du->z * u0 + dv->z * v1;
    }
    if (r_debug == 1)
        return false;
    if (q[0].z < NEAR_Z || q[1].z < NEAR_Z || q[2].z < NEAR_Z || q[3].z < NEAR_Z)
    {
        const q_tex *tx = &lv.textures[cell->tex & 0x7FFF];

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
    else if (px > tx)
        oc |= OC_RIGHT;
    if (py > ty)
        oc |= OC_TOP;
    else if (py < -ty)
        oc |= OC_BOTTOM;
    g->x = px; g->y = py; g->z = pz;
    if (front)
        g->xy = screen_xy(px, py, divu_result());
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
                if (c[i].x != as[i].x || c[i].y != as[i].y || c[i].z != as[i].z || c[i].oc != as[i].oc
                    || (!(c[i].oc & OC_NEAR) && c[i].xy != as[i].xy))
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
            if (c[i].x != as[i].x || c[i].y != as[i].y || c[i].z != as[i].z || c[i].oc != as[i].oc
                || (!(c[i].oc & OC_NEAR) && c[i].xy != as[i].xy))
                ++grid_bad;
    }
    r_grid_asm = grid_bad == 0;
}

static __attribute__((noinline)) void draw_face(r_ctx *x, int fi, int model)
{
    const q_face    *f = &lv.faces[fi];
    v3              o, du, dv, rowp, e0, e1, f0, f1;
    s32             d[3], dtx, dty;
    gv              *top = x->grid, *bot = x->grid + MAX_ROW, *tmp;
    const q_cell    *cell;
    const u16       *light;
    int             i, j, nu = f->nu, nv = f->nv, N = lv.N, stride = nu + 1;
    int             eu0 = f->eu0, eu1 = f->eu1, ev0 = f->ev0, ev1 = (int)(f->firstlight >> 24), ct, cb, k;
    bool            whole;
    u32             size_full;

    int             count0 = x->w->count;
#ifdef R_PROFILE
    u32             pt = frt_read(), pt2;
#endif

    if (f->flags & (FF_SKY | FF_NODRAW) || nu + 1 > MAX_ROW)
        return;
    ++x->st.faces;
    d[0] = f->origin[0] + mover_ofs[model][0] - cam.pos[0];
    d[1] = f->origin[1] + mover_ofs[model][1] - cam.pos[1];
    d[2] = f->origin[2] + mover_ofs[model][2] - cam.pos[2];
    to_view(d, &o);
    to_view(f->du, &du);
    to_view(f->dv, &dv);
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
    size_full = (u32)(((N >> 3) << 8) | N);
    /* the steps into and out of the face's edge columns and rows (a whole tile's if it isn't cropped) */
    k = (nu == 1 ? eu1 : N) - eu0;
    e0.x = x->dut.x * k; e0.y = x->dut.y * k; e0.z = x->dut.z * k;
    e1.x = x->dut.x * eu1; e1.y = x->dut.y * eu1; e1.z = x->dut.z * eu1;
    k = (nv == 1 ? ev1 : N) - ev0;
    f0.x = x->dvt.x * k; f0.y = x->dvt.y * k; f0.z = x->dvt.z * k;
    f1.x = x->dvt.x * ev1; f1.y = x->dvt.y * ev1; f1.z = x->dvt.z * ev1;

    PROF((pt2 = frt_read(), x->st.p_setup += (pt2 - pt) & 0xFFFF, pt = pt2));
    /* the grid: the whole face at once if it fits (the assembly), or a row at a time */
    whole = r_grid_asm && (nu + 1) * (nv + 1) <= 2 * MAX_ROW;
    if (whole)
    {
        s32 *gk = x->gk;

        x->ga.p = o; x->ga.e0 = e0; x->ga.d = du; x->ga.e1 = e1;
        gk[GK_ROWS] = nv;
        gk[GK_F0] = f0.x; gk[GK_F0 + 1] = f0.y; gk[GK_F0 + 2] = f0.z;
        gk[GK_F0 + 3] = dv.x; gk[GK_F0 + 4] = dv.y; gk[GK_F0 + 5] = dv.z;
        gk[GK_F0 + 6] = f1.x; gk[GK_F0 + 7] = f1.y; gk[GK_F0 + 8] = f1.z;
        grid_face_asm(top, nu + 1, &x->ga, gk);
        bot = top + stride;
    }
    else if (r_grid_asm)
    {
        x->ga.p = o; x->ga.e0 = e0; x->ga.d = du; x->ga.e1 = e1;
        grid_row_asm(top, nu + 1, &x->ga, grid_k);
    }
    else
        grid_row(top, nu + 1, &o, &du, &e0, &e1, dtx, dty);
    rowp = o;
    cell = &lv.cells[f->firstcell];
    light = &lv.lights[f->firstlight & 0xFFFFFF];
    for (j = 0; j < nv; ++j, light += stride)
    {
        const v3 *step = j == 0 ? &f0 : j == nv - 1 ? &f1 : &dv;

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
        PROF((x->st.gverts += nu + 1, x->st.seen += nu));
        PROF((pt2 = frt_read(), x->st.p_grid += (pt2 - pt) & 0xFFFF, pt = pt2));
        for (i = 0; i < nu; ++i, ++cell)
        {
            u32     xy[4];
            int     ty0, th, tex = cell->tex, cl, cr;
            gv      *g[4];
            u8      oa, oo;
            bool    exact;

            if (tex == CELL_EMPTY)
                continue;
            g[0] = &top[i]; g[1] = &top[i + 1]; g[2] = &bot[i + 1]; g[3] = &bot[i];
            oa = g[0]->oc & g[1]->oc & g[2]->oc & g[3]->oc;
            if (oa)
            {
                PROF(++x->st.culled);       /* the whole cell's outside a plane: so is any crop of it */
                continue;
            }
            oo = g[0]->oc | g[1]->oc | g[2]->oc | g[3]->oc;
            if (tex & CELL_FULL && !(oo & OC_NEAR) && !x->dmask)
            {
                /* the common case, all here: a whole tile, in front, no dynamic light */
                int         t = tex & 0x7FFF, s = x->tex_slot[t];
                s32         vram;
                u32         *dw, link;
                const u16   *l = light + i;

                if (r_debug == 1)
                    continue;
                if (s == 0xFFFF)
                {
                    if ((vram = tex_load(x, t)) < 0)
                        continue;
                }
                else
                {
                    slot_frame[s] = frame;
                    vram = (s32)(slot_vram + (u32)s * slot_bytes);
                }
                if (!(dw = cmd_alloc(x, &link)))
                    continue;
                PROF(++x->st.nfast);
                dw[0] = 0x10020000u | link;
                dw[1] = (u32)(PMOD_ECD | PMOD_LUT4 | PMOD_GOURAUD) << 16
                      | ((lut_vram + (u32)((const q_cell_full *)cell)->lut * 32) >> 3);
                dw[2] = ((u32)vram >> 3) << 16 | size_full;
                dw[3] = gv_xy(x, g[0]);
                dw[4] = gv_xy(x, g[1]);
                dw[5] = gv_xy(x, g[2]);
                dw[6] = gv_xy(x, g[3]);
                dw[7] = (u32)vdp_gouraud_fast(x->w, (u32)l[0] << 16 | l[1], (u32)l[stride + 1] << 16 | l[stride]) << 16;
                continue;
            }
            PROF(++x->st.nslow);
            if (tex & CELL_FULL)
            {
                ty0 = 0;
                th = N;
            }
            else
            {
                ty0 = cell->ty0;
                th = cell->th;
            }
            /* exact: the crop's the grid cell (the face's edge is the grid's) */
            cl = i == 0 ? eu0 : 0;
            cr = i == nu - 1 ? eu1 : N;
            exact = cell->u0 == cl && cell->u1 == cr && cell->v0 == ct && cell->v1 == cb;
            if (exact && !(oo & OC_NEAR))
            {
                PROF(++x->st.nexact);
                xy[0] = gv_xy(x, g[0]);
                xy[1] = gv_xy(x, g[1]);
                xy[2] = gv_xy(x, g[2]);
                xy[3] = gv_xy(x, g[3]);
            }
            else
            {
#ifdef R_PROFILE
                u32 ps = frt_read();
                bool ok = cell_corners(x, cell, g, xy, &ty0, &th, cl, cr, ct, cb, exact);

                x->st.p_corners += (frt_read() - ps) & 0xFFFF;
                if (!ok)
                    continue;
#else
                if (!cell_corners(x, cell, g, xy, &ty0, &th, cl, cr, ct, cb, exact))
                    continue;
#endif
            }
#ifdef R_PROFILE
            {
                u32 ps = frt_read();

                cell_emit(x, cell, ty0, th, xy, light + i, stride, g);
                x->st.p_slow += (frt_read() - ps) & 0xFFFF;
            }
#else
            cell_emit(x, cell, ty0, th, xy, light + i, stride, g);
#endif
        }
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
        if (face_vis[*m >> 3] & (1 << (*m & 7)))
            return true;
    return !leaf->nummark;
}

static inline void  list_face(int i, int model)
{
    if (nvis < MAX_VIS)
    {
        const q_face *f = &lv.faces[i];

        vis_model[nvis] = (u8)model;
        vis_faces[nvis++] = (u16)i;
        vis_cells += f->nu * f->nv;
        SHARE->published = nvis;
    }
}

/* a brush model's own BSP, front to back: every face towards the camera.
   cp: the camera relative to the model (where it's moved to) */
static void         walk_model(int n, int model, const s32 *cp)
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
        if (cull_box(mn, mx))
            return;
    }
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
        d = cp[pl->type] - pl->dist;
    else
        d = fmul(cp[0], pl->n[0]) + fmul(cp[1], pl->n[1]) + fmul(cp[2], pl->n[2]) - pl->dist;
    side = d < 0;
    walk_model(node->child[side], model, cp);
    for (i = node->firstface; i < node->firstface + node->numfaces; ++i)
        if (!(lv.faces[i].flags & FF_BACK) == !side)
            list_face(i, model);
    walk_model(node->child[!side], model, cp);
}

static void         walk(int n)
{
    const q_node    *node;
    const q_plane   *pl;
    s32             d;
    int             side, i;

    if (n < 0)
    {
        const q_leaf *leaf = &lv.leafs[-(n + 1)];
        const u16    *m;

        if (leaf_vis[-(n + 1)] != visframe || cull_box(leaf->mins, leaf->maxs))
            return;
        (void)m;
        if (leaf_ent[-(n + 1)] >= 0 && !leaf_seen(leaf))
            ;                               /* none of the leaf can be seen: nor can what's in it */
        else for (i = leaf_ent[-(n + 1)]; i >= 0; i = ent_next[i])
            if (nvis < MAX_VIS)
            {
                vis_model[nvis] = ENTITY;
                vis_faces[nvis++] = (u16)i;
                vis_cells += 80;            /* about what one costs, in cells */
                SHARE->published = nvis;
            }
        for (i = leaf_spr[-(n + 1)]; i >= 0; i = spr_next[i])
            if (nvis < MAX_VIS)
            {
                vis_model[nvis] = SPRITE;
                vis_faces[nvis++] = (u16)i;
                vis_cells += 2;
                SHARE->published = nvis;
            }
        for (i = leaf_model[-(n + 1)]; i >= 0; i = model_next[i])
        {
            s32 cp[3];

            if (!mover_live(i))
                continue;
            cp[0] = cam.pos[0] - mover_ofs[i][0];
            cp[1] = cam.pos[1] - mover_ofs[i][1];
            cp[2] = cam.pos[2] - mover_ofs[i][2];
            walk_model(lv.models[i].headnode, i, cp);
        }
        return;
    }
    node = &lv.nodes[n];
    if (node_vis[n] != visframe || cull_box(node->mins, node->maxs))
        return;
    pl = &lv.planes[node->plane];
    if (pl->type < 3)
        d = cam.pos[pl->type] - pl->dist;
    else
        d = fmul(cam.pos[0], pl->n[0]) + fmul(cam.pos[1], pl->n[1]) + fmul(cam.pos[2], pl->n[2]) - pl->dist;
    side = d < 0;
    walk(node->child[side]);
    for (i = node->firstface; i < node->firstface + node->numfaces; ++i)
        if ((face_vis[i >> 3] & (1 << (i & 7))) && !(lv.faces[i].flags & FF_BACK) == !side)
            list_face(i, 0);
    walk(node->child[!side]);
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

/* ---- the sky: a VDP2 layer behind everything, showing where VDP1 drew nothing ---- */

static int          sky_h, sky_horizon = -9999;
static u16          sky_above, sky_below, sky_zenith;
static volatile int sky_line;               /* the horizon the back colour table was last built for */

static void         sky_vblank(void)
{
    volatile u16    *tab = (volatile u16 *)(VDP2_VRAM + 0x7F000);
    int             y, top = sky_horizon - sky_h / 2;

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
    s32             c = fcos(cam.pitch);

    if (!lv.sky)
        return;
    /* the horizon's screen line: straight ahead at infinity */
    sky_horizon = CY - (s32)(((s64)FOCAL * fsin(cam.pitch)) / (c ? c : 1));
    sky_prepare(cam.yaw, sky_horizon + sky_h / 2, FOCAL);
}

/* A model: its two frames blended, turned by its yaw and placed, into view
   space and onto the screen; lit by its leaf's light and Quake's shading by
   normal; back faces dropped; its polygons sorted by depth among themselves
   (32 buckets over its own depth range) and put in nearest first, so the
   bucket's last-in-first-drawn order paints them back to front. */
static __attribute__((noinline)) void draw_model(r_ctx *x, int ei)
{
    const q_entity  *e = &ents[ei];
    const q_mdl     *m = e->mdl;
    const s32       *ax[3];
    s32             M[3][3], T[3], d[3], c = fcos(e->yaw), sn = fsin(e->yaw), zmin = 0x7FFFFFFF, zmax = -0x7FFFFFFF;
    const u8        *f0 = m->frames + (u32)e->oldframe * m->frame_bytes, *f1 = m->frames + (u32)e->frame * m->frame_bytes;
    const s32       *h0 = (const s32 *)f0, *h1 = (const s32 *)f1;
    const u8        *v0 = f0 + 24, *v1 = f1 + 24, *vn = e->lerp < FIX(0.5) ? v0 : v1;
    s32             lerp = e->lerp, inv, A0[3][3], A1[3][3], C0[3], C1[3];
    const u16       *gt;
    int             i, k, b, bi, nv = imin(m->nverts, MAX_MVERTS), np = imin(m->npolys, MAX_MPOLYS);
    vdp_writer      *w = x->w;
    u32             t0 = frt_read();

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
    if (T[2] < -FIX(48) || T[0] - FIX(48) > fmul(T[2] + FIX(48), kx) || T[0] + FIX(48) < -fmul(T[2] + FIX(48), kx)
        || T[1] - FIX(48) > fmul(T[2] + FIX(48), ky) || T[1] + FIX(48) < -fmul(T[2] + FIX(48), ky))
        return;
    ++x->st.models;
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
    if (r_use_dsp && nv <= DSP_MAXBLK * 8)
    {
        int         cpu = x == &ctx[1], nb = (nv + 15) >> 4, nf = lerp ? 2 : 1, f, bl;
        u32         *st = dsp_stream[cpu];
        const s32   *out = (const s32 *)UNCACHED(dsp_out[cpu]);

        for (f = 0; f < nf; ++f)
        {
            s32 (*A)[3] = f ? A1 : A0, *C = f ? C1 : C0;
            u32 va = ((u32)(f ? v1 : v0) & 0x07FFFFFF) >> 2;

            for (bl = 0; bl < nb; ++bl, st += 13)
            {
                st[0] = (u32)C[0]; st[1] = (u32)A[0][0]; st[2] = (u32)A[0][1]; st[3] = (u32)A[0][2];
                st[4] = (u32)C[1]; st[5] = (u32)A[1][0]; st[6] = (u32)A[1][1]; st[7] = (u32)A[1][2];
                st[8] = (u32)C[2]; st[9] = (u32)A[2][0]; st[10] = (u32)A[2][1]; st[11] = (u32)A[2][2];
                st[12] = va + (u32)bl * 16;
            }
        }
        dsp_acquire();
        dsp_blocks(dsp_stream[cpu], dsp_out[cpu], nb * nf);
        dsp_wait();
        dsp_release();
        for (i = 0; i < nv; ++i)
        {
            const s32   *o = out + (i >> 4) * 48 + (i & 15);
            s32         vx = o[0], vy = o[16], vz = o[32];
            u8          oc = 0;

            if (lerp)
            {
                const s32 *o1 = o + nb * 48;

                vx += fmul(o1[0] - vx, lerp);
                vy += fmul(o1[16] - vy, lerp);
                vz += fmul(o1[32] - vz, lerp);
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
    }
    else for (i = 0; i < nv; ++i)
    {
        s32 p[3], vx, vy, vz;
        u8  oc = 0;

        const u8 *q0 = v0 + i * 4, *q1 = v1 + i * 4;

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
    for (i = 0; i < np; ++i)
    {
        const q_mpoly   *p = &m->polys[i];
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
    x->st.t_mpolys += (frt_read() - t0) & 0xFFFF;
    /* out: nearest first when pushing, farthest first when appending */
    for (bi = 0; bi < MBUCKETS; ++bi)
        for (b = x->fifo ? MBUCKETS - 1 - bi : bi, i = x->mhead[b]; i >= 0; i = x->mnext[i])
        {
            const q_mpoly   *p = &m->polys[i];
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
    x->st.t_models += (frt_read() - t0) & 0xFFFF;
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

static void         part_begin(r_ctx *x)
{
    memset(&x->st, 0, sizeof(x->st));
    x->full = false;
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
        l = level_leaf(ents[i].origin);
        ent_leaf[i] = l;
        ent_next[i] = leaf_ent[l];
        leaf_ent[l] = (s16)i;
    }
    /* the slave starts on the list as the walk fills it */
    ctx[0].w = w0;
    ctx[1].w = w1;
    ctx[0].bucket = 0;
    ctx[1].bucket = 1;
    ctx[1].fifo = false;
    nvis = vis_cells = 0;
    SHARE->published = 0;
    SHARE->lo = 0;
    SHARE->hi = 0x7FFFFFFF;
    SHARE->walk_done = 0;
    if (r_two_cpus)
        signal_slave();
    walk(0);
    for (i = 0; i < nents; ++i)
        if (ent_leaf[i] >= 0)
            leaf_ent[ent_leaf[i]] = -1;
    for (i = 0; i < r_nsprites && i < MAX_SPRITES; ++i)
        leaf_spr[spr_leaf[i]] = -1;
    rs.nodes = (int)frt_to_us((frt_read() - t0) & 0xFFFF);  /* the walk's time */

    draw_master();
    if (r_two_cpus)
        wait_signal();
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
        rs.t_models += frt_to_us(s->t_models);
        rs.t_mlight += frt_to_us(s->t_mlight);
        rs.t_mverts += frt_to_us(s->t_mverts);
        rs.t_mpolys += frt_to_us(s->t_mpolys);
        rs.nfast += s->nfast;
        rs.nslow += s->nslow;
        rs.nexact += s->nexact;
        rs.p_setup += frt_to_us(s->p_setup);
        rs.p_grid += frt_to_us(s->p_grid);
        rs.p_cells += frt_to_us(s->p_cells);
        rs.p_slow += frt_to_us(s->p_slow);
        rs.p_corners += frt_to_us(s->p_corners);
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
