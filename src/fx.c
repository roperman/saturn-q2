/*
** Projectiles and effects: blaster bolts, rockets and grenades (Quake 2's
** game/g_weapon.c fire_blaster, fire_rocket, fire_grenade), explosions,
** muzzle and impact flashes, sparks.
**
** Each frame the live ones become the renderer's dynamic lights (added to
** the Gouraud colours of the cell corners and model normals they reach),
** sprites (glowing blobs, drawn in depth order), and - rockets and grenades -
** models (render entities 64 on).
*/
#include "game.h"

#define MAX_PROJ        (24)
#define MAX_FLASHES     (8)
#define MAX_SPARKS      (24)
#define FX_ENT0         (64)                /* the render entities after the game's */

enum { P_BOLT, P_ROCKET, P_GRENADE };

typedef struct
{
    s32             pos[3], vel[3], life;
    g_ent           *owner;
    int             damage, radius_damage, kind, spin;
    bool            live, resting;
}                   t_proj;

typedef struct { s32 pos[3], t, dur, radius; u8 r, g, b; bool live, sprite, big; } t_flash;
typedef struct { s32 pos[3], t; bool live; } t_spark;

static t_proj       proj[MAX_PROJ];
static t_flash      flashes[MAX_FLASHES];
static t_spark      sparks[MAX_SPARKS];
static int          spark_next;
static t_flash      *last_flash;

void                fx_spark(const s32 *p)
{
    t_spark         *s = &sparks[spark_next];
    int             k;

    spark_next = (spark_next + 1) % MAX_SPARKS;
    for (k = 0; k < 3; ++k)
        s->pos[k] = p[k];
    s->t = 0;
    s->live = true;
}

void                fx_flash(const s32 *p, s32 radius, s32 dur, u8 r, u8 g, u8 b)
{
    int             i, k, oldest = 0;

    for (i = 0; i < MAX_FLASHES; ++i)
    {
        if (!flashes[i].live)
            break;
        if (flashes[i].t > flashes[oldest].t)
            oldest = i;
    }
    if (i == MAX_FLASHES)
        i = oldest;
    for (k = 0; k < 3; ++k)
        flashes[i].pos[k] = p[k];
    flashes[i].t = 0;
    flashes[i].dur = dur;
    flashes[i].radius = radius;
    flashes[i].r = r;
    flashes[i].g = g;
    flashes[i].b = b;
    flashes[i].live = true;
    flashes[i].sprite = dur > FIX(0.2);     /* impacts get a blob; muzzle flashes are just light */
    flashes[i].big = false;
    last_flash = &flashes[i];
}

/* an explosion: a big orange light fading over half a second, a growing blob, sparks */
int                 fx_explosions;

void                fx_explosion(const s32 *p)
{
    int             n;

    ++fx_explosions;
    fx_flash(p, FIX(360), FIX(0.5), 16, 11, 3);
    last_flash->big = true;
    for (n = 0; n < 6; ++n)
    {
        s32 q[3];

        q[0] = p[0] + crandom() * 24;
        q[1] = p[1] + crandom() * 24;
        q[2] = p[2] + crandom() * 24;
        fx_spark(q);
    }
}

static t_proj       *new_proj(g_ent *owner, const s32 *start, const s32 *dir, s32 speed, int kind)
{
    s32             len = vlen(dir);
    t_proj          *p = NULL;
    int             i, k;

    for (i = 0; i < MAX_PROJ; ++i)
        if (!proj[i].live)
        {
            p = &proj[i];
            break;
        }
    if (!p || len < 256)
        return NULL;
    memset(p, 0, sizeof(*p));
    for (k = 0; k < 3; ++k)
    {
        p->pos[k] = start[k];
        p->vel[k] = fmul(fdiv(dir[k], len), speed);
    }
    p->owner = owner;
    p->kind = kind;
    p->live = true;
    return p;
}

/* a bolt from owner, along dir (roughly unit length: it's normalised) */
void                fx_bolt(g_ent *owner, const s32 *start, const s32 *dir, int damage, s32 speed)
{
    t_proj          *p = new_proj(owner, start, dir, speed, P_BOLT);

    if (p)
    {
        p->life = FIX(3);
        p->damage = damage;
    }
}

void                fx_rocket(g_ent *owner, const s32 *start, const s32 *dir, int damage, int radius_damage)
{
    t_proj          *p = new_proj(owner, start, dir, FIX(650), P_ROCKET);

    if (p)
    {
        p->life = FIX(12);
        p->damage = damage;
        p->radius_damage = radius_damage;
    }
}

void                fx_grenade(g_ent *owner, const s32 *start, const s32 *dir, int damage, s32 speed)
{
    t_proj          *p = new_proj(owner, start, dir, speed, P_GRENADE);

    if (p)
    {
        /* Quake throws it up a little, and scatters it */
        p->vel[2] += FIX(200) + crandom() * 10;
        p->vel[0] += crandom() * 10;
        p->vel[1] += crandom() * 10;
        p->life = FIX(2.5);
        p->damage = damage;
        p->radius_damage = damage;
    }
}

static void         explode(t_proj *p, g_ent *direct)
{
    s_play(p->kind == P_ROCKET ? SND_EXPLOSION : SND_GRENADE_EXPLODE, p->pos, ATTN_NORM);
    if (p->kind == P_ROCKET)
    {
        if (direct)
            g_damage(direct, p->owner, p->damage, p->pos);
        g_explosion(p->pos, NULL, p->owner, p->radius_damage, FIX(p->radius_damage), direct);
    }
    else
        g_explosion(p->pos, NULL, p->owner, p->damage, FIX(p->damage + 40), NULL);
    p->live = false;
}

/* Quake's ClipVelocity with a bounce (MOVETYPE_BOUNCE: 1.5) */
static void         bounce(s32 *v, const s32 *n)
{
    s32             back = fmul(fmul(v[0], n[0]) + fmul(v[1], n[1]) + fmul(v[2], n[2]), FIX(1.5));
    int             k;

    for (k = 0; k < 3; ++k)
        v[k] -= fmul(n[k], back);
}

static void         move_proj(t_proj *p, s32 dt)
{
    static const s32 zero[3] = { 0, 0, 0 };
    s32             end[3];
    q_trace         t;
    g_ent           *hit;
    int             k;

    p->life -= dt;
    if (p->kind == P_GRENADE)
    {
        if (p->life <= 0)
        {
            explode(p, NULL);
            return;
        }
        if (p->resting)
            return;
        p->vel[2] -= fmul(FIX(800), dt);
        p->spin += (int)fmul(dt, 0x10000);
    }
    for (k = 0; k < 3; ++k)
        end[k] = p->pos[k] + fmul(p->vel[k], dt);
    t = g_trace(p->pos, zero, zero, end, p->owner, MASK_SHOT, &hit);
    if (!hit && t.ent)
        hit = g_ent_for_model(t.ent);       /* a func_explosive */
    for (k = 0; k < 3; ++k)
        p->pos[k] = t.endpos[k];
    if (t.fraction == FIX(1))
    {
        if (p->life <= 0)
            p->live = false;
        return;
    }
    switch (p->kind)
    {
        case P_BOLT:
        {
            s32 q[3];

            if (hit)
                g_damage(hit, p->owner, p->damage, t.endpos);
            /* the flash, a little off the wall so it lights it */
            for (k = 0; k < 3; ++k)
                q[k] = t.endpos[k] - fmul(p->vel[k], FIX(0.008));
            fx_flash(q, FIX(240), FIX(0.35), 13, 8, 2);
            if (!hit)
                s_play(SND_BLASTER_HIT, q, ATTN_NORM);
            if (hit == g_player)
                last_flash->sprite = false;             /* not in your face */
            p->live = false;
            break;
        }
        case P_ROCKET:
            for (k = 0; k < 3; ++k)
                p->pos[k] -= fmul(p->vel[k], FIX(0.02));   /* back off the wall a bit */
            explode(p, hit && hit->takedamage ? hit : NULL);
            break;
        case P_GRENADE:
            if (hit && hit->takedamage)
            {
                explode(p, hit);
                break;
            }
            if (t.plane)
            {
                if (vlen(p->vel) > FIX(100))
                    s_play(SND_GRENADE_BOUNCE, p->pos, ATTN_NORM);
                bounce(p->vel, t.plane->n);
                /* on a floor and slow: it stops (still ticking) */
                if (t.plane->n[2] > FIX(0.7) && vlen(p->vel) < FIX(60))
                {
                    p->resting = true;
                    p->vel[0] = p->vel[1] = p->vel[2] = 0;
                }
            }
            break;
    }
}

void                fx_update(s32 dt)
{
    int             i, k;

    for (i = 0; i < MAX_PROJ; ++i)
        if (proj[i].live)
            move_proj(&proj[i], dt);
    for (i = 0; i < MAX_FLASHES; ++i)
        if (flashes[i].live && (flashes[i].t += dt) >= flashes[i].dur)
            flashes[i].live = false;
    for (i = 0; i < MAX_SPARKS; ++i)
        if (sparks[i].live && (sparks[i].t += dt) >= FIX(0.15))
            sparks[i].live = false;

    /* the renderer's lights and sprites */
    r_ndlights = 0;
    r_nsprites = 0;
    for (i = 0; i < MAX_PROJ; ++i)
    {
        t_proj  *p = &proj[i];

        if (!p->live || p->kind == P_GRENADE)
            continue;
        if (r_ndlights < MAX_DLIGHTS)
        {
            q_dlight *l = &r_dlights[r_ndlights++];

            for (k = 0; k < 3; ++k)
                l->pos[k] = p->pos[k];
            l->radius = FIX(200);
            if (p->kind == P_BOLT)
            {
                l->r = 9; l->g = 7; l->b = 1;
            }
            else
            {
                l->r = 11; l->g = 8; l->b = 2;
            }
        }
        if (p->kind == P_BOLT && r_nsprites < MAX_SPRITES)
        {
            q_sprite *s = &r_sprites[r_nsprites++];

            for (k = 0; k < 3; ++k)
                s->pos[k] = p->pos[k];
            s->size = FIX(3);
            s->color = RGB(255, 255, 200);
            s->halo = RGB(255, 200, 40);
        }
    }
    for (i = 0; i < MAX_SPARKS; ++i)
        if (sparks[i].live && r_nsprites < MAX_SPRITES)
        {
            q_sprite *sp = &r_sprites[r_nsprites++];

            for (k = 0; k < 3; ++k)
                sp->pos[k] = sparks[i].pos[k];
            sp->size = FIX(1.5);
            sp->color = RGB(255, 230, 150);
            sp->halo = RGB(160, 120, 60);
        }
    for (i = 0; i < MAX_FLASHES; ++i)
    {
        t_flash     *f = &flashes[i];
        s32         left;

        if (!f->live)
            continue;
        left = FIX(1) - fdiv(f->t, f->dur);         /* fading out */
        if (r_ndlights < MAX_DLIGHTS)
        {
            q_dlight *l = &r_dlights[r_ndlights++];

            for (k = 0; k < 3; ++k)
                l->pos[k] = f->pos[k];
            l->radius = imax(fmul(f->radius, left), FIX(8));
            l->r = f->r; l->g = f->g; l->b = f->b;
        }
        if (r_nsprites < MAX_SPRITES && f->sprite)
        {
            q_sprite *s = &r_sprites[r_nsprites++];

            for (k = 0; k < 3; ++k)
                s->pos[k] = f->pos[k];
            s->size = f->big ? FIX(10) + fmul(FIX(26), FIX(1) - left) : FIX(4) + fmul(FIX(10), FIX(1) - left);
            s->color = f->big ? RGB(255, 220, 120) : RGB(255, 240, 160);
            s->halo = f->big ? RGB(230, 90, 10) : RGB(255, 140, 20);
        }
    }
}

/* rockets and grenades as models: render entities from FX_ENT0 */
void                fx_render(void)
{
    int             i, n = FX_ENT0;

    for (i = 0; i < MAX_PROJ && n < MAX_ENTITIES; ++i)
    {
        t_proj      *p = &proj[i];
        q_entity    *r;
        int         k;

        if (!p->live || p->kind == P_BOLT)
            continue;
        r = &ents[n];
        if (!r->live)
            r->g_leaf = -1;
        for (k = 0; k < 3; ++k)
            r->origin[k] = p->pos[k];
        if (p->kind == P_ROCKET)
        {
            s32 h[3];

            h[0] = p->vel[0];
            h[1] = p->vel[1];
            h[2] = 0;
            r->mdl = &models[MDL_ROCKET];
            r->yaw = vectoyaw(p->vel);
            r->pitch = -fatan2(p->vel[2], vlen(h)) & 0xFFFF;
        }
        else
        {
            r->mdl = &models[MDL_GRENADE];
            r->yaw = (i * 0x3000) & 0xFFFF;
            r->pitch = p->spin & 0xFFFF;
        }
        r->skin = 0;
        r->frame = r->oldframe = 0;
        r->lerp = 0;
        r->live = r->mdl->loaded;
        ++n;
    }
    for (; n < MAX_ENTITIES; ++n)
        ents[n].live = false;
    nents = MAX_ENTITIES;
}
