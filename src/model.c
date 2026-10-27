/*
** MD2 models (tools/bake_md2.py) and the things that use them.
**
** A model is loaded onto the RAM cart and used in place. Its polygon
** textures go through the renderer's texture cache like the walls'
** (render_init() gives them ids after the level's), and its colour tables
** are uploaded for good.
**
** Animation is Quake's: 10 frames a second, drawn in between (oldframe to
** frame by lerp).
*/
#include "q2.h"

q_mdl               models[MDL_COUNT + VIEW_SLOTS];
int                 nmodels_loaded;
static const char   *model_files[MDL_COUNT] = { MDL_FILES };
q_entity            ents[MAX_ENTITIES];
int                 nents;

bool                model_load(q_mdl *m, const char *file)
{
    const u8        *b = cart_load(file);

    return b && model_parse(m, b);
}

__attribute__((cold)) bool                model_parse(q_mdl *m, const u8 *b)
{
    const u16       *h16;
    const u32       *h32;

    if (memcmp(b, "Q2MD", 4))
        return false;
    h16 = (const u16 *)(b + 4);
    h32 = (const u32 *)(b + 16);
    m->nverts = h16[0];
    m->npolys = h16[1];
    m->nframes = h16[2];
    m->nanims = h16[3];
    m->nskins = h16[4];
    m->ntex = h16[5];
    m->polys = (const q_mpoly *)(b + h32[0]);
    m->tex = (const q_mtex *)(b + h32[1]);
    m->texdata = b + h32[2];
    m->luts = (const u16 *)(b + h32[3]);
    m->frames = b + h32[4];
    m->anims = (const q_manim *)(b + h32[5]);
    m->shade = b + h32[6];
    m->normals = (const s16 *)(b + h32[7]);
    m->per_skin = h32[8];
    m->nluts = (int)(h32[9] & 0x7FFFFFFF);
    m->lutmap = h32[9] & 0x80000000 ? m->luts + m->nluts * 16 : NULL;   /* (tools/bake_md2.py: after the tables) */
    m->fpolys = (const q_mpoly *)(b + h32[10]);
    m->nfpolys = (int)h32[11];
    if (m->nfpolys)
    {
        /* a count, then the vertices it uses: the first so many (tools/bake_md2.py), which is all
           draw_model takes; an older bake's aren't: no far mesh then */
        const u16 *u = (const u16 *)(m->fpolys + m->nfpolys);
        int k;

        m->nfverts = u[0];
        for (k = 0; k < m->nfverts && u[1 + k] == k; ++k)
            ;
        if (k < m->nfverts)
            m->nfpolys = 0;
    }
    m->frame_bytes = 24 + (u32)m->nverts * 4 + (((u32)m->nverts + 3) & ~3u);   /* (the normals' run after the words) */
    {
        /* how far from its origin it reaches, over all its frames (each frame's box from its
           scale and translation): the screen culls were a sphere of 48 and 64 units, which the
           tank (72 above its origin) stuck out of, and lost its top when its origin was low */
        u32 r2 = 0;
        int f, k;

        for (f = 0; f < m->nframes; ++f)
        {
            const s32 *h = (const s32 *)(m->frames + (u32)f * m->frame_bytes);
            u32 d2 = 0;

            for (k = 0; k < 3; ++k)
            {
                s32 lo = h[3 + k] >> 16, hi = (h[3 + k] + h[k] * 255) >> 16, a = imax(iabs(lo), iabs(hi)) + 1;

                d2 += (u32)(a * a);
            }
            r2 = imax(r2, d2);
        }
        m->radius = FIX(imax((int)isqrt(r2) + 1, 48));
        if (m->radius <= FIX(64))
        {
            m->cull_z = FIX(64);
            m->cull_x = FIX(91);                /* (64 against the sides' slopes, 1 and 0.7: as before) */
            m->cull_y = FIX(79);
        }
        else
        {
            m->cull_z = m->radius;
            m->cull_x = fmul(m->radius, FIX(1.4143));
            m->cull_y = fmul(m->radius, FIX(1.2207));
        }
    }
    /* the frames are read every time it's drawn: into fast RAM if they fit */
    m->loaded = true;
    return true;
}

/* An optional model (tools/models.txt: the gunner) is loaded only if the cart has room after
   it for what's taken from the cart once the models are in: both gun slots, the renderer's
   texture rings and the gun's kept drawing (r_cart_after), the level's entities if low work
   RAM hasn't the room for them (g_cart_need), the monsters' code (the overlays: a few KB each)
   and the spill of low work RAM (64 KB at a time). Installation has the room for its gunners,
   Comm Center hasn't (they're left out, src/g_main.c); the retail maps' extra monsters filled
   the cart to the old margin, and the rings after them overflowed it (OVERNIGHT.md 98) */
#define CART_AFTER      (2 * VIEW_MAX_BYTES + 32 * 1024 + 16 * 1024)

static bool         room_for(const char *file)
{
    u32             lba, size;

    return cd_find(file, &lba, &size)
           && cart_free() >= ((size + 2047) & ~2047u) + CART_AFTER + r_cart_after() + g_cart_need();
}

#ifdef LEVEL_TEST
extern u32          models_noroom;
#endif

__attribute__((cold)) void                models_load_all(void)
{
    int             need[MDL_COUNT];
    int             i, best;

    g_models_needed(need);
#ifdef KILL_OTHERS
    need[KILL_OTHERS] *= 100;               /* (OPT=-DKILL_OTHERS=MDL_x, -DMEDIC_TEST: that model first, whatever the map has more of) */
#endif
    nmodels_loaded = 0;
    for (i = 0; i < MDL_COUNT; ++i)
        models[i].loaded = false;
    /* (the optional ones last: the room's what's left after everything else; the most used of them
       first, so a map short of room loses its rarest monsters) */
    for (i = 0; i < MDL_COUNT; ++i)
        if (need[i] && !(MDL_OPTIONAL >> i & 1) && model_load(&models[i], model_files[i]))
            nmodels_loaded = imax(nmodels_loaded, i + 1);
    for (;;)
    {
        best = -1;
        for (i = 0; i < MDL_COUNT; ++i)
            if (need[i] > 0 && MDL_OPTIONAL >> i & 1 && (best < 0 || need[i] > need[best]))
                best = i;
        if (best < 0)
            break;
        need[best] = -need[best];           /* (tried) */
        if (room_for(model_files[best]) && model_load(&models[best], model_files[best]))
            nmodels_loaded = imax(nmodels_loaded, best + 1);
    }
#ifdef LEVEL_TEST
    models_noroom = 0;                      /* (the first 32 models: the monsters, the pieces, the pickups) */
    for (i = 0; i < 32 && i < MDL_COUNT; ++i)
        if (need[i] && !models[i].loaded)
            models_noroom |= 1u << i;
#endif
}

/* The animated models' polygons, far mesh and texture records into HWRAM,
   in what's left once everything else has had its share (the last thing at
   start-up; 2 KB kept for later): both of draw_model's polygon passes read
   a record a polygon, and a miss on the cart is 75 cycles to HWRAM's 10.
   The frames stay on the cart: the DSP reads those */
#define HOT_KEEP        (2048)

#ifdef LEVEL_TEST
u32                 models_cold;            /* (OPT=-DLEVEL_TEST: what didn't fit, bytes) */
u32                 models_noroom;          /* (OPT=-DLEVEL_TEST: the level's models left out, a bit each: the title's NO ROOM) */
#endif

static const void   *hot(const void *p, u32 bytes)
{
    const void      *q = level_hot_keep(p, bytes, HOT_KEEP);

#ifdef LEVEL_TEST
    if (q == p)
        models_cold += bytes;
#endif
    return q;
}

#ifndef NO_NORMS_HOT
/* the 162 normals (Quake 2's anorms, 2.14): every model carries the same table, and a light's
   weights (src/mlight.s) read all 972 bytes of it a model-light pair, from the cart at some 149
   cycles a line (SUITE 54: 15 ms a frame in the yard fight, 66 cycles a normal for a loop of
   25): one copy here, read through the cache from low work RAM. OPT=-DNO_NORMS_HOT: the cart's */
static s16          norms_hot[162 * 3] __attribute__((section(".lwdata"), aligned(16)));
#endif

__attribute__((cold)) void                models_hot(void)
{
    int             i;
#ifndef NO_NORMS_HOT
    bool            norms_set = false;
#endif

#ifdef LEVEL_TEST
    models_cold = 0;
#endif
    for (i = 0; i < nmodels_loaded; ++i)
    {
        q_mdl       *m = &models[i];

        if (!m->loaded)
            continue;
#ifndef NO_NORMS_HOT
        if (!norms_set)
        {
            memcpy(norms_hot, m->normals, sizeof(norms_hot));
            norms_set = true;
        }
        m->normals = norms_hot;
#endif
        if (m->nframes < 2)
            continue;
        m->polys = hot(m->polys, (u32)m->npolys * sizeof(q_mpoly));
        m->tex = hot(m->tex, (u32)m->ntex * sizeof(q_mtex));
        if (m->nfpolys)
            m->fpolys = hot(m->fpolys, (u32)m->nfpolys * sizeof(q_mpoly));
    }
}

int                 model_anim(const q_mdl *m, const char *name)
{
    int             i, k;

    for (i = 0; i < m->nanims; ++i)
    {
        for (k = 0; k < 12 && name[k] && m->anims[i].name[k] == name[k]; ++k)
            ;
        if (k == 12 || (!name[k] && !m->anims[i].name[k]))
            return i;
    }
    return 0;
}

/* Gouraud by normal: the leaf's light times Quake's shading for this yaw.
   VDP1 adds the Gouraud value to the texel, so a light f (1 = as drawn) is
   16 + (f - 1) * MODEL_K, sized for the skins' brightness. */
#define MODEL_K         (12)

_Static_assert(MODEL_K <= 16, "ents_light: its sums stay unsigned");

/* the 162 normals' Gouraud colours: a leaf's light (ll: r g b, 8.8) times a yaw's shading */
void                model_shade(u16 *out, const u8 *sh, const u16 *ll)
{
    u32             l0 = ll[0], l1 = ll[1], l2 = ll[2];
    int             n;

    for (n = 0; n < 162; ++n)
    {
        /* iclamp(16 + ((((l s) >> 7) - 256) MODEL_K >> 8), 0, 31) with the 16 inside the
           shift: never negative then, so unsigned shifts (a signed one is a library call,
           two a channel here) give the same, exactly (every l and s tried) */
        u32 s = sh[n];
        u32 g0 = (((l0 * s) >> 7) * MODEL_K + 256 * (16 - MODEL_K)) >> 8;
        u32 g1 = (((l1 * s) >> 7) * MODEL_K + 256 * (16 - MODEL_K)) >> 8;
        u32 g2 = (((l2 * s) >> 7) * MODEL_K + 256 * (16 - MODEL_K)) >> 8;

        g0 = g0 > 31 ? 31 : g0;
        g1 = g1 > 31 ? 31 : g1;
        g2 = g2 > 31 ? 31 : g2;
        out[n] = (u16)(0x8000 | g2 << 10 | g1 << 5 | g0);
    }
}

/* only what can be drawn this frame is lit now: what's in a leaf of the PVS render_world
   walks (the one marked, if it's the camera's: not on a frame the camera's changed cluster,
   when everything is, as before). The rest keeps what it was lit for (g_litleaf, g_yaw),
   and is lit when it's next in the PVS */
bool                ents_pvs(void)
{
    return r_pvs_marked(lv.leafs[level_leaf(cam.pos)].cluster);
}

void                ents_light(void)
{
    bool            pvs = ents_pvs();

    ents_light_pvs(pvs);
#ifndef NO_LIGHT_AHEAD
#ifdef MLIGHT_DSP
    ++lit_tag;                              /* (one CPU: the frame's tag here) */
#endif
    ents_light_dyn(pvs);
#endif
}

/* The models' dynamic lights, a frame behind (OPT=-DNO_LIGHT_AHEAD: draw_model's, as it draws):
   last frame's lights (kept by lights_lag before this frame's effects make new ones), added as
   the frame starts, with the rest of the models' light: on the slave, whose first job this is,
   while it'd be waiting for the walk anyway. Each model near one gets a table of its own from a
   small pool, its base with them added as draw_model added them (the same arithmetic); a model
   near none draws with its base, one the pool's too full for is lit by draw_model */
static q_dlight     lag[MAX_DLIGHTS];
static int          nlag;
#ifndef MLIGHT_DSP
u16                 ent_lit[LIT_POOL][162];
#endif

const q_dlight      *lights_lagged(int *n)
{
    *n = nlag;
    return lag;
}

void                lights_lag(void)
{
    memcpy(lag, r_dlights, sizeof(lag));
    nlag = r_ndlights;
}

#ifndef MLIGHT_DSP
/* (a CPU that didn't make them: its cache's copies of the pool forgotten, a line at a time) */
void                ent_lit_forget(void)
{
    u32             a;

    for (a = (u32)ent_lit & ~15u; a < (u32)ent_lit + sizeof(ent_lit); a += 16)
        *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
}
#else
static void         lit_forget(int rp);
void                ent_lit_forget(void)
{
    if (lit_vram)
        lit_forget((int)((lit_tag + 1) & 1));   /* (the master, before its models: the pool read this frame) */
}
#endif

#ifndef NO_LIGHT_AHEAD
/* (OPT=-DMLIGHT_V2) what each of the 32 steps of a normal's dot with the light adds to the
   colours, for src/mlight2.s: the step's middle dot (2.14), its weight w = 0.35 + 0.65 dot, and
   (k * w) >> 18 a colour, red and blue in the low word's 5-5-5 places, green in the high's */
void                mlight2_table(u32 *tab, const s16 *k)
{
    int             i;

    for (i = 0; i < 32; ++i)
    {
        s32 w = 5734 + (((i * 512 + 256) * 10650) >> 14);
        u32 r = (u32)((k[3] * w) >> 18), g = (u32)((k[4] * w) >> 18), b = (u32)((k[5] * w) >> 18);

        tab[i] = r | b << 10 | g << 21;
    }
}

/* a light's weights on the 162 normals: src/mlight2.s, the colours together (OPT=-DMLIGHT_V1:
   src/mlight.s, one at a time, as before; -DNO_MLIGHT_ASM: the C, which they round a unit from
   at most; -DMLIGHT_CHECK: the asm and the C both, compared) */
void                mlight(u16 *gt, const s16 *nrm, s32 mx, s32 my, s32 mz, s32 f, const q_dlight *l)
{
#if !defined(NO_MLIGHT_ASM) || defined(MLIGHT_CHECK)
    s16             k[6];
#ifndef MLIGHT_V1
    u32             tab[32];                /* (the second loop's, the default: OPT=-DMLIGHT_V1 is the first) */
#endif

    k[0] = (s16)mx; k[1] = (s16)my; k[2] = (s16)mz;
    k[3] = (s16)((l->r * f) >> 12); k[4] = (s16)((l->g * f) >> 12); k[5] = (s16)((l->b * f) >> 12);
#ifndef MLIGHT_V1
    mlight2_table(tab, k);
#define MLIGHT_ASM(g) mlight2_asm(g, nrm, k, tab)
#else
#define MLIGHT_ASM(g) mlight_asm(g, nrm, k)
#endif
#endif
#if defined(NO_MLIGHT_ASM) || defined(MLIGHT_CHECK)
    {
#ifdef MLIGHT_CHECK
        u16 ref[162];
        int nn;

        memcpy(ref, gt, sizeof(ref));
        MLIGHT_ASM(ref);
#else
        int nn;
#endif
        for (nn = 0; nn < 162; ++nn, nrm += 3)
        {
            s32 dot = (nrm[0] * mx + nrm[1] * my + nrm[2] * mz) >> 14;      /* 2.14 */
            s32 w = (f * (5734 + (dot > 0 ? (dot * 10650) >> 14 : 0))) >> 14;   /* 0.35 + 0.65 dot */
            u16 g0 = gt[nn];
            int rr = (g0 & 31) + ((l->r * w) >> 16), gg = ((g0 >> 5) & 31) + ((l->g * w) >> 16);
            int bb = ((g0 >> 10) & 31) + ((l->b * w) >> 16);

            gt[nn] = (u16)(0x8000 | imin(bb, 31) << 10 | imin(gg, 31) << 5 | imin(rr, 31));
#ifdef MLIGHT_CHECK
            ++mlight_checks;
            mlight_diffs += iabs((ref[nn] & 31) - (gt[nn] & 31)) > 1 || iabs(((ref[nn] >> 5) & 31) - ((gt[nn] >> 5) & 31)) > 1
                            || iabs(((ref[nn] >> 10) & 31) - ((gt[nn] >> 10) & 31)) > 1;
#endif
        }
    }
#else
    MLIGHT_ASM(gt);
#endif
}
#ifdef MLIGHT_CHECK
u32                 mlight_checks, mlight_diffs;
#endif

#ifndef MLIGHT_DSP
void                ents_light_dyn(bool pvs)
{
    int             i, k, n = 0;

    for (i = 0; i < GAME_ENTS; ++i)
    {
        q_entity    *e = &ents[i];
        const q_mdl *m = e->mdl;
        u16         *gt = NULL;
        s32         c, sn;

        e->g_lit = -1;
        if (!e->live || !m || (pvs && !r_leaf_in_pvs(e->g_leaf)))
            continue;
        {
            /* off the screen (a sphere of 64 units against the view's sides: the camera's final by
               now)? Then only if it's drawn after all, by draw_model */
            s32 d0 = e->origin[0] - cam.pos[0], d1 = e->origin[1] - cam.pos[1], d2 = e->origin[2] - cam.pos[2];
            s32 vx = fmul(d0, cam.right[0]) + fmul(d1, cam.right[1]) + fmul(d2, cam.right[2]);
            s32 vy = fmul(d0, cam.up[0]) + fmul(d1, cam.up[1]) + fmul(d2, cam.up[2]);
            s32 vz = fmul(d0, cam.fwd[0]) + fmul(d1, cam.fwd[1]) + fmul(d2, cam.fwd[2]);

            if (vz < -m->cull_z || iabs(vx) - vz > m->cull_x || iabs(vy) - fmul(vz, FIX(0.7)) > m->cull_y)
            {
                e->g_lit = -2;
                continue;
            }
        }
        c = fcos(e->yaw);
        sn = fsin(e->yaw);
#ifdef NO_MODEL_DLIGHTS
        nlag = 0;                           /* (OPT=-DNO_MODEL_DLIGHTS: the models unlit by the dynamic lights, a test of
                                               what their lighting costs the frame: OVERNIGHT.md 106) */
#endif
#if MODEL_LIGHTS > 0
        {
        int             sel[MODEL_LIGHTS], ns = lights_pick(lag, nlag, e->origin, sel);

        for (k = 0; k < ns; ++k)
        {
            const q_dlight  *l = &lag[sel[k]];
#else
        for (k = 0; k < nlag; ++k)
        {
            const q_dlight  *l = &lag[k];
#endif
            s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
            s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16, d2 = dx * dx + dy * dy + dz * dz;
            s32             f, len, mx, my, mz;
            const s16       *nrm = m->normals;

            if (d2 >= r * r)
                continue;
            if (!gt)
            {
                if (n == LIT_POOL)
                {
                    e->g_lit = -2;          /* (no room: draw_model's) */
                    break;
                }
                gt = ent_lit[n];
                e->g_lit = (s8)n++;
                memcpy(gt, e->gbase, sizeof(e->gbase));
            }
            f = ((r * r - d2) * ((1 << 24) / imax(r * r, 1))) >> 8;     /* 0..65536 at the origin */
            len = (s32)isqrt((u32)d2) + 1;
            /* the direction to the light, in the model's space (turned back by its yaw), 2.14 */
            mx = ((dx * c + dy * sn) >> 2) / len;
            my = ((dy * c - dx * sn) >> 2) / len;
            mz = (dz << 14) / len;
            mlight(gt, nrm, mx, my, mz, f, l);
        }
#if MODEL_LIGHTS > 0
        }
#endif
    }
}
#else
/* The models' dynamic lights worked out by the DSP a frame ahead (engine/mlit.dsp, OPT=-DMLIGHT_DSP;
   OVERNIGHT.md 107). As a frame starts the slave lists a job for each model near a light: the
   frame's lights (a frame behind the effects, as before) at the model's place and yaw this frame,
   so the table is a frame old when it's drawn, as its lights were. The DSP makes the tables after
   the frame's models, into a pool in VDP1's VRAM, and writes the frame's tag back; the next frame's
   first job here finds them and gives each model that still lives its table. A model near a light
   with no table (new, or past the pool) is lit by draw_model as it draws, as before.
   The block (lit_level, VDP1's VRAM): the program, the models' program (which it loads back), the
   head (the frame's tag, its jobs, the tag back, the normals, how many), the normals, two frames'
   jobs, two frames' tables. A job: its table >> 2, the model's base colours >> 2 (gbase: high work
   RAM, on a long), its lights, then each light's 7 words: the direction to it in the model's space
   as 0.65 of 2.14 >> 6, C = 5734 << 8, and its colour at the model, 0..124: for a normal n the
   DSP's w = C + max(n . k, 0) is 2^22 times the weight 0.35 + 0.65 cos, and c w 2^24 times what the
   light adds to a colour, which it sums unclamped and clamps at the end: the C's lighting a unit
   off at most (OPT=-DMLIGHT_DSP_CHECK: the C's same sums against the DSP's, for the HUD) */
#include "dsp.h"
#define DSP_PROG_SECTION __attribute__((section(".lwrodata")))
#include "mlit.h"
u32                 lit_vram;
u32                 lit_tag;
u32                 lit_stat[4];
u32                 lit_dbg[6];
static u32          *lit_block;             /* (VDP1's VRAM: uncached) */
typedef struct
{
    s16             ent, table;
    const q_mdl     *mdl;
    u16             owner;
#ifdef MLIGHT_DSP_CHECK
    s8              nl;
    u32             w[LIT_DSP_LIGHTS * 7];
    u16             base[162];              /* (the base as the job was listed) */
#endif
}                   lit_rec;
static lit_rec      lit_recs[2][LIT_DSP_POOL] __attribute__((section(".lwdata")));
static int          lit_nrecs[2] __attribute__((section(".lwdata")));
static u32          lit_tags[2] __attribute__((section(".lwdata")));

void                lit_level(u32 vram)
{
    static const u32 consts[14] = { 255, 31, 1u << 24, 0x3E0, 1u << 19, 0x7C00, 1u << 14, 1, 32, 1024, 65536, 0x8000,
                                    4000, 0xFFFF };
    int             i, c;

    lit_vram = vram;
    lit_block = NULL;
    lit_nrecs[0] = lit_nrecs[1] = 0;
    lit_tags[0] = lit_tags[1] = 0xFFFFFFFF;
    lit_stat[0] = lit_stat[1] = lit_stat[2] = lit_stat[3] = 0;
    if (!vram)
    {
        dsp_chain_params(NULL, NULL);
        return;
    }
    lit_block = (u32 *)(VDP1_VRAM + vram);
    for (i = 0; i < 256; ++i)
        lit_block[i] = mlit_prog[i];
    dsp_models_prog(lit_block + LB_PROG0);
    for (i = 0; i < 8; ++i)
        lit_block[LB_HEAD + i] = 0;
    lit_block[LB_HEAD + 3] = ((u32)(lit_block + LB_NORM) & 0x07FFFFFF) >> 2;
    for (c = 0; c < nmodels_loaded && !models[c].loaded; ++c)
        ;
    for (i = 0; i < 176 * 3; ++i)
        lit_block[LB_NORM + i] = i < 162 * 3 && c < nmodels_loaded ? (u32)(s32)models[c].normals[i] : 0;
    dsp_data_write(41, consts, 14);         /* (its constants: the DSP's data RAM keeps them) */
    dsp_chain_params(lit_block, lit_block + LB_PROG0);
}

#ifdef LIT_UNCACHED
static u32          lit_buf[2][LB_TABLE] __attribute__((aligned(16)));   /* (a CPU's copy of the table it's drawing with) */
const u16           *lit_table(int gl)
{
    const volatile u32 *t = lit_block + LB_POOL + gl * LB_TABLE;
    u32             sp, *b;
    int             k;

    __asm__ volatile ("mov r15,%0" : "=r" (sp));
    b = lit_buf[SP_MASTER(sp) ? 0 : 1];
    for (k = 0; k < LB_TABLE; ++k)
        b[k] = t[k];
    return (const u16 *)b;
}
#else
const u16           *lit_table(int gl)
{
    return (const u16 *)((u32)(lit_block + LB_POOL + gl * LB_TABLE) & 0x1FFFFFFF);
}
#endif

/* (the DSP wrote the pool: the cache's lines of it forgotten) */
static void         lit_forget(int rp)
{
    u32             a = (u32)(lit_block + LB_POOL + rp * LIT_DSP_POOL * LB_TABLE) & 0x1FFFFFFF;
    u32             end = a + LIT_DSP_POOL * LB_TABLE * 4;

    for (; a < end; a += 16)
        *(volatile u32 *)(0x40000000 | a) = 0;
}

#ifdef MLIGHT_DSP_CHECK
/* the DSP's table against the same sums here, where the base it had is still the model's */
static void         lit_check(const lit_rec *r, const q_entity *e, int rp)
{
    const volatile u16 *t = (const volatile u16 *)(lit_block + LB_POOL + (rp * LIT_DSP_POOL + r->table) * LB_TABLE);
    const s16       *nrm = e->mdl->normals;
    int             n, k;

    ++lit_stat[2];
    for (n = 0; n < 162; ++n, nrm += 3)
    {
        u32         g = r->base[n];
        u32         sr = (g & 31) << 24, sg = ((g >> 5) & 31) << 24, sb = ((g >> 10) & 31) << 24;

        for (k = 0; k < r->nl; ++k)
        {
            const s32 *w = (const s32 *)(r->w + k * 7);
            s32     term = nrm[0] * w[0] + nrm[1] * w[1] + nrm[2] * w[2];
            u32     ww = (u32)w[3] + (term > 0 ? (u32)term : 0);

            sr += (u32)w[4] * ww;
            sg += (u32)w[5] * ww;
            sb += (u32)w[6] * ww;
        }
        sr = imin((int)(sr >> 24), 31);
        sg = imin((int)(sg >> 24), 31);
        sb = imin((int)(sb >> 24), 31);
        if (t[n] != (0x8000 | sb << 10 | sg << 5 | sr))
        {
            ++lit_stat[3];
            lit_dbg[0] = (u32)n;
            lit_dbg[1] = t[n];
            lit_dbg[2] = 0x8000 | sb << 10 | sg << 5 | sr;
            lit_dbg[3] = (u32)r->nl | (u32)r->table << 8;
            return;
        }
    }
}
#endif

void                ents_light_dyn(bool pvs)
{
    u32             tag = *(volatile u32 *)UNCACHED(&lit_tag);
    int             rp = (int)((tag + 1) & 1), wp = (int)(tag & 1);
    int             i, k, n = 0;
    u32             *jobs = lit_block ? lit_block + LB_JOBS + wp * LIT_DSP_POOL * LB_JOBW : NULL;

    for (i = 0; i < GAME_ENTS; ++i)
        ents[i].g_lit = -1;
    /* last frame's tables, to the models they were made for */
    if (lit_block && lit_tags[rp] == tag - 1 && lit_block[LB_HEAD + 2] == tag - 1)
    {
        lit_forget(rp);
        for (k = 0; k < lit_nrecs[rp]; ++k)
        {
            const lit_rec   *r = &lit_recs[rp][k];
            q_entity        *e = &ents[r->ent];

            if (!e->live || e->mdl != r->mdl || e->g_owner != r->owner)
                continue;
            e->g_lit = (s8)(r->table + rp * LIT_DSP_POOL);
            ++lit_stat[1];
#ifdef MLIGHT_DSP_CHECK
            lit_check(r, e, rp);
#endif
        }
    }
    /* this frame's models near a light: lit by draw_model if they've no table; and next frame's jobs */
    for (i = 0; i < GAME_ENTS; ++i)
    {
        q_entity    *e = &ents[i];
        const q_mdl *m = e->mdl;
        s32         c, sn, best[LIT_DSP_LIGHTS];
        int         sel[LIT_DSP_LIGHTS], ns = 0;
        u32         *j;

        if (!e->live || !m || (pvs && !r_leaf_in_pvs(e->g_leaf)))
            continue;
        {
            /* off the screen (a sphere of 64 units against the view's sides)? Then only if it's drawn
               after all, by draw_model */
            s32 d0 = e->origin[0] - cam.pos[0], d1 = e->origin[1] - cam.pos[1], d2 = e->origin[2] - cam.pos[2];
            s32 vx = fmul(d0, cam.right[0]) + fmul(d1, cam.right[1]) + fmul(d2, cam.right[2]);
            s32 vy = fmul(d0, cam.up[0]) + fmul(d1, cam.up[1]) + fmul(d2, cam.up[2]);
            s32 vz = fmul(d0, cam.fwd[0]) + fmul(d1, cam.fwd[1]) + fmul(d2, cam.fwd[2]);

            if (vz < -m->cull_z || iabs(vx) - vz > m->cull_x || iabs(vy) - fmul(vz, FIX(0.7)) > m->cull_y)
            {
                if (e->g_lit < 0)
                    e->g_lit = -2;
                continue;
            }
        }
        /* its strongest lights (as lights_pick) */
        for (k = 0; k < nlag; ++k)
        {
            const q_dlight  *l = &lag[k];
            s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
            s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16, d2 = dx * dx + dy * dy + dz * dz;
            s32             f;
            int             q;

            if (d2 >= r * r)
                continue;
            f = ((r * r - d2) * ((1 << 24) / imax(r * r, 1))) >> 8;
            f = (f * (l->r + l->g + l->b)) >> 5;
            if (ns == LIT_DSP_LIGHTS && f <= best[ns - 1])
                continue;
            if (ns < LIT_DSP_LIGHTS)
                ++ns;
            for (q = ns - 1; q > 0 && best[q - 1] < f; --q)
            {
                best[q] = best[q - 1];
                sel[q] = sel[q - 1];
            }
            best[q] = f;
            sel[q] = k;
        }
        if (!ns)
            continue;
        if (e->g_lit < 0)
            e->g_lit = -2;                  /* (no table from last frame: draw_model's) */
        if (!jobs || n == LIT_DSP_POOL)
            continue;
        c = fcos(e->yaw);
        sn = fsin(e->yaw);
        j = jobs + n * LB_JOBW;
        j[0] = ((u32)(lit_block + LB_POOL + (wp * LIT_DSP_POOL + n) * LB_TABLE) & 0x07FFFFFF) >> 2;
#ifdef LIT_UNCACHED
        {
            const u32 *gb = (const u32 *)e->gbase;  /* (its base colours into the block: the DSP reads VRAM only) */
            u32     *bc = lit_block + LB_BASES + (wp * LIT_DSP_POOL + n) * LB_TABLE;

            for (k = 0; k < 81; ++k)
                bc[k] = gb[k];
            j[1] = ((u32)bc & 0x07FFFFFF) >> 2;
        }
#else
        j[1] = ((u32)e->gbase & 0x07FFFFFF) >> 2;
#endif
        j[2] = (u32)ns;
        for (k = 0; k < ns; ++k)
        {
            const q_dlight  *l = &lag[sel[k]];
            s32             dx = (l->pos[0] - e->origin[0]) >> 16, dy = (l->pos[1] - e->origin[1]) >> 16;
            s32             dz = (l->pos[2] - e->origin[2]) >> 16, r = l->radius >> 16, d2 = dx * dx + dy * dy + dz * dz;
            s32             f = ((r * r - d2) * ((1 << 24) / imax(r * r, 1))) >> 8;     /* 0..65536 at the origin */
            s32             len = (s32)isqrt((u32)d2) + 1;
            s32             mx = ((dx * c + dy * sn) >> 2) / len, my = ((dy * c - dx * sn) >> 2) / len;
            s32             mz = (dz << 14) / len;  /* (the direction to the light in the model's space, 2.14) */
            u32             *w = j + 3 + k * 7;

            w[0] = (u32)((mx * 10650) >> 20);
            w[1] = (u32)((my * 10650) >> 20);
            w[2] = (u32)((mz * 10650) >> 20);
            w[3] = 5734u << 8;
            w[4] = (u32)((l->r * f) >> 14);
            w[5] = (u32)((l->g * f) >> 14);
            w[6] = (u32)((l->b * f) >> 14);
        }
        {
            lit_rec *r = &lit_recs[wp][n];

            r->ent = (s16)i;
            r->table = (s16)n;
            r->mdl = m;
            r->owner = e->g_owner;
#ifdef MLIGHT_DSP_CHECK
            r->nl = (s8)ns;
            for (k = 0; k < ns * 7; ++k)
                r->w[k] = j[3 + k];
            memcpy(r->base, e->gbase, sizeof(r->base));
#endif
        }
        ++n;
    }
    if (lit_block)
    {
        lit_nrecs[wp] = n;
        lit_tags[wp] = tag;
        lit_stat[0] += (u32)n;
        lit_block[LB_HEAD + 1] = ((u32)jobs & 0x07FFFFFF) >> 2;
        lit_block[LB_HEAD + 4] = (u32)n;
        lit_block[LB_HEAD + 0] = tag;       /* (last: the DSP's go) */
    }
}
#endif
#endif

/* each one's leaf: again only if it's moved (items stand still; render_world puts them in
   their leaves by it). These three do the game's entities only, as the slave's first job runs
   alongside the master's fx_render: the effects' (from GAME_ENTS) light themselves, theirs
   wanting the dynamic lights from draw_model */
void                ents_leaf(void)
{
    int             i;

    for (i = 0; i < GAME_ENTS; ++i)
    {
        q_entity    *e = &ents[i];

        if (!e->live)
            continue;
        if (e->g_leaf < 0)
            e->g_litleaf = -1;              /* (new, or a new level: nothing lit yet) */
        e->g_leaf = e->g_moved || e->g_leaf < 0 ? level_leaf(e->origin) : e->g_leaf;
        e->g_moved = false;
    }
}

/* their light by normal, where the leaf they're in or how they're turned has changed (pvs:
   ents_pvs's answer, the slave's told it: render_world may be marking a new PVS by then). The
   slave's, after the master's gone on: before it draws a model, the master forgets its cache's
   copies of the ends of their lights (ents_shade_forget), the lines it read them in with what
   it did read */
void                ents_shade(bool pvs)
{
    int             i;

    for (i = 0; i < GAME_ENTS; ++i)
    {
        q_entity    *e = &ents[i];
        int         leaf = e->g_leaf, ys;
        const u16   *ll;
        const u8    *sh;

        if (!e->live)
            continue;
        ys = (int)((u32)e->yaw >> 12) & 15;
        if (leaf == e->g_litleaf && ys == e->g_yaw)
            continue;
        if (pvs && !r_leaf_in_pvs(leaf))
            continue;                       /* not drawn this frame */
        e->g_litleaf = (s16)leaf;
        e->g_yaw = ys;
        ll = &lv.leaflight[leaf * 4];
        sh = e->mdl->shade + ys * 162;
        model_shade(e->gbase, sh, ll);
    }
}

void                ents_shade_forget(void)
{
    int             i;

    for (i = 0; i < MAX_ENTITIES; ++i)
    {
        *(volatile u32 *)(0x40000000 | ((u32)&ents[i].gbase[0] & 0x1FFFFFF0)) = 0;
        *(volatile u32 *)(0x40000000 | ((u32)&ents[i].gbase[161] & 0x1FFFFFF0)) = 0;
    }
}

/* (pvs: ents_pvs's answer) */
void                ents_light_pvs(bool pvs)
{
    ents_leaf();
    ents_shade(pvs);
}
