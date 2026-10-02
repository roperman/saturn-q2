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

bool                model_parse(q_mdl *m, const u8 *b)
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
    m->frame_bytes = 24 + (u32)m->nverts * 4;
    /* the frames are read every time it's drawn: into fast RAM if they fit */
    m->loaded = true;
    return true;
}

/* An optional model (tools/models.txt: the gunner) is loaded only if the cart has room after
   it for both gun slots and the gun's kept drawing (src/view.c, render.c): Installation has,
   Comm Center hasn't (its gunners are left out, src/g_main.c) */
#define CART_AFTER      (2 * VIEW_MAX_BYTES + 16 * 1024)

static bool         room_for(const char *file)
{
    u32             lba, size;

    return cd_find(file, &lba, &size) && cart_free() >= ((size + 2047) & ~2047u) + CART_AFTER;
}

void                models_load_all(void)
{
    bool            need[MDL_COUNT];
    int             i;

    g_models_needed(need);
    nmodels_loaded = 0;
    for (i = 0; i < MDL_COUNT; ++i)
        models[i].loaded = false;
    /* (the optional ones last: the room's what's left after everything else) */
    for (i = 0; i < MDL_COUNT; ++i)
        if (need[i] && !(MDL_OPTIONAL >> i & 1) && model_load(&models[i], model_files[i]))
            nmodels_loaded = imax(nmodels_loaded, i + 1);
    for (i = 0; i < MDL_COUNT; ++i)
        if (need[i] && MDL_OPTIONAL >> i & 1 && room_for(model_files[i]) && model_load(&models[i], model_files[i]))
            nmodels_loaded = imax(nmodels_loaded, i + 1);
}

/* The animated models' polygons, far mesh and texture records into HWRAM,
   in what's left once everything else has had its share (the last thing at
   start-up; 2 KB kept for later): both of draw_model's polygon passes read
   a record a polygon, and a miss on the cart is 75 cycles to HWRAM's 10.
   The frames stay on the cart: the DSP reads those */
#define HOT_KEEP        (2048)

#ifdef LEVEL_TEST
u32                 models_cold;            /* (OPT=-DLEVEL_TEST: what didn't fit, bytes) */
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

void                models_hot(void)
{
    int             i;

#ifdef LEVEL_TEST
    models_cold = 0;
#endif
    for (i = 0; i < nmodels_loaded; ++i)
    {
        q_mdl       *m = &models[i];

        if (!m->loaded || m->nframes < 2)
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
u16                 ent_lit[LIT_POOL][162];

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

/* (a CPU that didn't make them: its cache's copies of the pool forgotten, a line at a time) */
void                ent_lit_forget(void)
{
    u32             a;

    for (a = (u32)ent_lit & ~15u; a < (u32)ent_lit + sizeof(ent_lit); a += 16)
        *(volatile u32 *)(0x40000000 | (a & 0x1FFFFFFF)) = 0;
}

#ifndef NO_LIGHT_AHEAD
void                ents_light_dyn(bool pvs)
{
    int             i, k, n = 0, nn;

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

            if (vz < -FIX(64) || iabs(vx) - vz > FIX(91) || iabs(vy) - fmul(vz, FIX(0.7)) > FIX(79))
            {
                e->g_lit = -2;
                continue;
            }
        }
        c = fcos(e->yaw);
        sn = fsin(e->yaw);
        for (k = 0; k < nlag; ++k)
        {
            const q_dlight  *l = &lag[k];
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
            for (nn = 0; nn < 162; ++nn, nrm += 3)
            {
                s32 dot = (nrm[0] * mx + nrm[1] * my + nrm[2] * mz) >> 14;      /* 2.14 */
                s32 w = (f * (5734 + (dot > 0 ? (dot * 10650) >> 14 : 0))) >> 14;   /* 0.35 + 0.65 dot */
                u16 g0 = gt[nn];
                int rr = (g0 & 31) + ((l->r * w) >> 16), gg = ((g0 >> 5) & 31) + ((l->g * w) >> 16);
                int bb = ((g0 >> 10) & 31) + ((l->b * w) >> 16);

                gt[nn] = (u16)(0x8000 | imin(bb, 31) << 10 | imin(gg, 31) << 5 | imin(rr, 31));
            }
        }
    }
}
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
