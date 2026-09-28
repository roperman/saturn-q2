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
    m->nluts = (int)h32[9];
    m->fpolys = (const q_mpoly *)(b + h32[10]);
    m->nfpolys = (int)h32[11];
    if (m->nfpolys)
    {
        const u16 *u = (const u16 *)(m->fpolys + m->nfpolys);   /* a count, then the vertices */

        m->nfverts = u[0];
        m->fverts = u + 1;
    }
    m->frame_bytes = 24 + (u32)m->nverts * 4;
    /* the frames are read every time it's drawn: into fast RAM if they fit */
    m->loaded = true;
    return true;
}

void                models_load_all(void)
{
    bool            need[MDL_COUNT];
    int             i;

    g_models_needed(need);
    nmodels_loaded = 0;
    for (i = 0; i < MDL_COUNT; ++i)
    {
        models[i].loaded = false;
        if (need[i] && model_load(&models[i], model_files[i]))
            nmodels_loaded = i + 1;
    }
}

/* The animated models' polygons, far mesh and texture records into HWRAM,
   in what's left once everything else has had its share (the last thing at
   start-up; 2 KB kept for later): both of draw_model's polygon passes read
   a record a polygon, and a miss on the cart is 75 cycles to HWRAM's 10.
   The frames stay on the cart: the DSP reads those */
#define HOT_KEEP        (2048)

void                models_hot(void)
{
    int             i;

    for (i = 0; i < nmodels_loaded; ++i)
    {
        q_mdl       *m = &models[i];

        if (!m->loaded || m->nframes < 2)
            continue;
        m->polys = level_hot_keep(m->polys, (u32)m->npolys * sizeof(q_mpoly), HOT_KEEP);
        m->tex = level_hot_keep(m->tex, (u32)m->ntex * sizeof(q_mtex), HOT_KEEP);
        if (m->nfpolys)
        {
            m->fpolys = level_hot_keep(m->fpolys, (u32)m->nfpolys * sizeof(q_mpoly), HOT_KEEP);
            m->fverts = level_hot_keep(m->fverts, (u32)m->nfverts * 2, HOT_KEEP);
        }
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

void                ents_light(void)
{
    int             i;
    /* only what can be drawn this frame is lit now: what's in a leaf of the PVS render_world
       walks (the one marked, if it's the camera's: not on a frame the camera's changed cluster,
       when everything is, as before). The rest keeps what it was lit for (g_litleaf, g_yaw),
       and is lit when it's next in the PVS */
    bool            pvs = r_pvs_marked(lv.leafs[level_leaf(cam.pos)].cluster);

    for (i = 0; i < nents; ++i)
    {
        q_entity    *e = &ents[i];
        int         leaf, ys;
        const u16   *ll;
        const u8    *sh;

        if (!e->live)
            continue;
        if (e->g_leaf < 0)
            e->g_litleaf = -1;              /* (new, or a new level: nothing lit yet) */
        /* its leaf: again only if it's moved (items stand still; render_world uses it too) */
        leaf = e->g_moved || e->g_leaf < 0 ? level_leaf(e->origin) : e->g_leaf;
        e->g_leaf = leaf;
        e->g_moved = false;
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
