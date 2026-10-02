/*
** The game's frame: Quake 2's G_RunFrame at 10 a second, the entities, and
** what they share: traces that see monsters and the player as boxes (as
** SV_Trace does), damage, and the weapons.
**
** Entity 0 is the player; its position comes from pmove each frame.
*/
#include "game.h"

g_level             level;
g_ent               *g_edicts;              /* low work RAM if there's room, else the cart (g_init) */
u8                  *g_shorts;
int                 g_nfull, g_nshort, g_nitems, g_ntrigs;
g_spot              *g_item_spots;
g_area              *g_trig_areas;
g_ent               *g_player;              /* g_edicts[0] (g_init) */
int                 g_dropped;              /* (the level's entities with no room: full ones past MAX_FULL) */
g_ent               goal_marker;

extern g_ent        *sound_entity;
extern int          sound_entity_framenum;

/* ---- small maths ---- */

s32                 frandom(void)
{
    return (s32)(rng() & 0xFFFF);
}

s32                 crandom(void)
{
    return (s32)(rng() & 0x1FFFF) - FIX(1);
}

int                 vectoyaw(const s32 *v)
{
    if (!v[0] && !v[1])
        return 0;
    return fatan2(v[1], v[0]) & 0xFFFF;
}

s32                 vlen(const s32 *v)
{
    /* in units x 32 while the squares fit (a side under ~1180 units), else units x 4: a level
       spans up to 5200 units, and past 1024 a quarter of a unit is plenty */
    int             sh = iabs(v[0]) < FIX(1024) && iabs(v[1]) < FIX(1024) && iabs(v[2]) < FIX(1024) ? 11 : 14;
    u32             x = (u32)iabs(v[0] >> sh), y = (u32)iabs(v[1] >> sh), z = (u32)iabs(v[2] >> sh);

    return (s32)(isqrt(x * x + y * y + z * z) << sh);
}

/* ---- traces: the world and movers (pm_trace's), then boxes for the entities ---- */

static const q_plane box_planes[6] = {
    { { FIX(1), 0, 0 }, 0, 0, 0, 0 }, { { -FIX(1), 0, 0 }, 0, 0, 1, 0 },
    { { 0, FIX(1), 0 }, 0, 1, 0, 0 }, { { 0, -FIX(1), 0 }, 0, 1, 2, 0 },
    { { 0, 0, FIX(1) }, 0, 2, 0, 0 }, { { 0, 0, -FIX(1) }, 0, 2, 4, 0 },
};

static s32          frac_div(s32 n, s32 d)
{
    if (d < 0)
    {
        n = -n;
        d = -d;
    }
    if (d == 0 || n >= 2 * d)
        return n >= 0 ? FIX(2) : -FIX(2);
    if (n <= -2 * d)
        return -FIX(2);
    return fdiv(n, d);
}

/* a box (mins/maxs) swept from start to end against a still box: closer than t->fraction? */
static bool         box_sweep(const s32 *start, const s32 *end, const s32 *mins, const s32 *maxs, const s32 *bmin,
                              const s32 *bmax, q_trace *t)
{
    s32             enter = -FIX(2), leave = FIX(2);
    int             k, plane = -1;
    bool            inside = true;

    for (k = 0; k < 3; ++k)
    {
        s32 lo = bmin[k] - maxs[k], hi = bmax[k] - mins[k], s = start[k], d = end[k] - s, t0, t1;

        if (s <= lo || s >= hi)
            inside = false;
        if (d == 0)
        {
            if (s <= lo || s >= hi)
                return false;
            continue;
        }
        t0 = frac_div(lo - s, d);
        t1 = frac_div(hi - s, d);
        if (d > 0)
        {
            if (t0 > enter)
            {
                enter = t0;
                plane = k * 2 + 1;          /* hit its low side: the normal points back along -axis */
            }
            if (t1 < leave)
                leave = t1;
        }
        else
        {
            if (t1 > enter)
            {
                enter = t1;
                plane = k * 2;
            }
            if (t0 < leave)
                leave = t0;
        }
    }
    if (inside)
    {
        t->startsolid = true;
        return false;
    }
    if (enter >= leave || enter < -FIX(0.001) || enter >= t->fraction || plane < 0)
        return false;
    t->fraction = imax(enter - 64, 0);      /* a hair short */
    t->plane = &box_planes[plane];
    t->contents = CONTENTS_MONSTER;
    return true;
}

/* The solid entities' boxes, grown by SOLID_MARGIN, in HWRAM: a trace that
   can hit monsters looks through all of them, and the edicts are in LWRAM
   (a couple of misses each). Rebuilt once a frame; a monster's own box is
   refreshed after its tick; the margin covers the rest (the player's move
   in a frame, a lift's carry). A box that passes is tested on the edict. */
#define SOLID_MARGIN    FIX(32)

typedef struct { s32 lo[3], hi[3]; g_ent *e; } g_solidbox;
static g_solidbox   g_solids[MAX_FULL];      /* (only full ones are solid) */
static u8           g_solid_slot[MAX_FULL];     /* an edict's box, or 0xFF */
static int          g_nsolids;

static void         solid_box(g_solidbox *b, const g_ent *e)
{
    int             k;

    for (k = 0; k < 3; ++k)
    {
        b->lo[k] = e->origin[k] + e->mins[k] - SOLID_MARGIN;
        b->hi[k] = e->origin[k] + e->maxs[k] + SOLID_MARGIN;
    }
}

static void         g_solids_update(void)
{
    int             i;

    g_nsolids = 0;
    for (i = 0; i < g_nfull; ++i)
    {
        g_ent       *e = &g_edicts[i];

        g_solid_slot[i] = 0xFF;
        if (e->kind == EK_FREE || !e->solid)
            continue;
        g_solid_slot[i] = (u8)g_nsolids;
        g_solids[g_nsolids].e = e;
        solid_box(&g_solids[g_nsolids++], e);
    }
}

int                 g_ntraces;
u32                 g_trace_ticks;
#ifdef FIGHT_BENCH
g_trace_site        g_trace_sites[16];      /* (the fight benchmark: where the traces come from) */
#endif

q_trace             g_trace(const s32 *start, const s32 *mins, const s32 *maxs, const s32 *end, const g_ent *pass,
                            int mask, g_ent **hit)
{
    u32             t0 = frt_read(), dt;
    q_trace         t = trace_world(start, mins, maxs, end, mask);

    ++g_ntraces;
    dt = (frt_read() - t0) & 0xFFFF;
    g_trace_ticks += dt;
#ifdef FIGHT_BENCH
    {
        u32         at = (u32)__builtin_return_address(0);
        int         j;

        for (j = 0; j < 15 && g_trace_sites[j].at && g_trace_sites[j].at != at; ++j)
            ;
        g_trace_sites[j].at = at;
        ++g_trace_sites[j].n;
        g_trace_sites[j].us += frt_to_us(dt);
    }
#endif
    int             i, k;
    s32             lo[3], hi[3];

#ifdef FIGHT_BENCH
    extern u32      tr_ticks[5];
    u32             tt0 = frt_read();
#endif

    if (hit)
        *hit = NULL;
    if (!(mask & CONTENTS_MONSTER) || t.allsolid)
        return t;
    for (k = 0; k < 3; ++k)
    {
        lo[k] = imin(start[k], end[k]) + mins[k];
        hi[k] = imax(start[k], end[k]) + maxs[k];
    }
    for (i = 0; i < g_nsolids; ++i)
    {
        const g_solidbox *sb = &g_solids[i];
        g_ent   *e = sb->e;
        s32     bmin[3], bmax[3];

        if (sb->hi[0] < lo[0] || sb->lo[0] > hi[0] || sb->hi[1] < lo[1] || sb->lo[1] > hi[1]
            || sb->hi[2] < lo[2] || sb->lo[2] > hi[2])
            continue;
        if (e == pass || e->kind == EK_FREE || !e->solid)
            continue;
        if (e->kind == EK_PLAYER ? pl.noclip || e->dead : e->dead && !(mask & CONTENTS_DEADMONSTER))
            continue;                       /* (a body: only shots) */
        for (k = 0; k < 3; ++k)
        {
            bmin[k] = e->origin[k] + e->mins[k];
            bmax[k] = e->origin[k] + e->maxs[k];
            if (bmax[k] < lo[k] || bmin[k] > hi[k])
                break;
        }
        if (k < 3)
            continue;
        if (box_sweep(start, end, mins, maxs, bmin, bmax, &t))
        {
            t.ent = 0;
            if (hit)
                *hit = e;
        }
    }
    for (k = 0; k < 3; ++k)
        t.endpos[k] = t.fraction == FIX(1) ? end[k] : start[k] + fmul(t.fraction, end[k] - start[k]);
#ifdef FIGHT_BENCH
    tr_ticks[3] += (frt_read() - tt0) & 0xFFFF;
#endif
    return t;
}

/* ---- damage ---- */

s32                 player_flash;           /* the red flash when you're hit, 0..255 */

int                 kills, total_monsters;
bool                level_complete;

/* Quake's ThrowGib and ThrowHead: the monster in pieces (each one's, as its m_*.c had them), each
   from somewhere in its box at VelocityForDamage's speed, and the body gone */
static __attribute__((cold)) void g_gib(g_ent *self, int damage)
{
    static const s8 sets[][6] = {
        /* meat, bone, chest, metal, gear, head */
        { 3, 0, 1, 0, 0, 1 },               /* the soldiers */
        { 4, 2, 0, 0, 0, 1 },               /* the infantry, the gunner, the berserker */
        { 1, 0, 1, 4, 1, 0 },               /* the tank (the gear its head) */
    };
    static const u8 mdl[6] = { MDL_GIB_MEAT, MDL_GIB_BONE, MDL_GIB_CHEST, MDL_GIB_METAL, MDL_GIB_GEAR, MDL_GIB_HEAD };
    const s8        *set = sets[self->mdl == &models[MDL_SOLDIER] ? 0 : self->mdl == &models[MDL_TANK] ? 2 : 1];
    s32             scale = damage < 50 ? FIX(0.7) : FIX(1.2);
    int             g, n, k;

    s_play(SND_GIB, self->origin, ATTN_NORM);
    for (g = 0; g < 6; ++g)
        for (n = 0; n < set[g]; ++n)
        {
            s32 p[3], v[3];

            for (k = 0; k < 3; ++k)
                p[k] = self->origin[k] + self->mins[k] + fmul(self->maxs[k] - self->mins[k], frandom());
            if (g == 5)
            {
                p[0] = self->origin[0];         /* (the head: from where its head was) */
                p[1] = self->origin[1];
                p[2] = self->origin[2] + self->maxs[2] - FIX(8);
            }
            v[0] = fmul(fmul(crandom(), FIX(100)), scale);
            v[1] = fmul(fmul(crandom(), FIX(100)), scale);
            v[2] = fmul(FIX(200) + fmul(frandom(), FIX(100)), scale);
            if (models[mdl[g]].loaded)
                fx_gib(mdl[g], p, v);
        }
    self->dead = true;
    self->solid = false;
    self->takedamage = false;
    self->move = NULL;
    self->kind = EK_FREE;                   /* (its render entity goes: g_render_ents) */
}

void                g_damage(g_ent *targ, g_ent *attacker, int damage, const s32 *point)
{
    if (!targ || targ->kind == EK_FREE || damage <= 0 || targ->inactive)
        return;
    if (targ->dead)
    {
        /* a body: enough more and it's in pieces (Quake's: a corpse takes damage) */
        if (targ->kind == EK_MONSTER && targ->gib_health < 0)
        {
            targ->health -= damage;
            if (targ->health <= targ->gib_health)
                g_gib(targ, damage);
        }
        return;
    }
    if (targ->kind == EK_PLAYER)
    {
        static s32 pain_time;               /* (a pain sound at most every 0.7 s, as Quake) */

        damage = g_armor_absorb(damage);
        targ->health -= damage;
        player_flash = imin(player_flash + damage * 12, 200);
        if (targ->health <= 0)
        {
            targ->dead = true;
            s_play(SND_PLAYER_DEATH, NULL, ATTN_NONE);
        }
        else if (damage > 0 && (level.time >= pain_time || level.time < pain_time - FIX(1)))
        {
            pain_time = level.time + FIX(0.7);
            s_play(targ->health < 25 ? SND_PLAYER_PAIN25 : targ->health < 50 ? SND_PLAYER_PAIN50
                   : targ->health < 75 ? SND_PLAYER_PAIN75 : SND_PLAYER_PAIN100, NULL, ATTN_NONE);
        }
        return;
    }
    if (!targ->takedamage)
        return;
    targ->health -= damage;
    if (targ->health <= 0)
    {
        if (targ->kind == EK_MONSTER)
        {
            ++kills;
            G_UseTargets(targ, attacker);   /* monster_death_use */
            if (targ->health <= targ->gib_health)
            {
                g_gib(targ, damage);        /* (in pieces, not a death) */
                return;
            }
        }
        targ->die(targ, attacker, damage, point);
        return;
    }
    /* M_ReactToDamage: turn on whoever did it */
    if (targ->kind == EK_MONSTER && attacker && attacker->kind == EK_PLAYER && targ->enemy != attacker)
    {
        targ->enemy = attacker;
        FoundTarget(targ);
    }
    if (targ->pain)
        targ->pain(targ, attacker, damage);
}

/* the game entity that is a brush model (a func_explosive), if any */
__attribute__((cold)) g_ent *g_ent_for_model(int model)    /* (on a hit on a brush model) */
{
    int             i;

    for (i = 1; i < g_nfull; ++i)
        if (g_edicts[i].kind != EK_FREE && g_edicts[i].model == model && g_edicts[i].takedamage)
            return &g_edicts[i];
    return NULL;
}

/* Bullets and pellets, spread up to hspread across and vspread up at 8192.
   Quake traces each through the world; here the world gets one trace (down
   the middle), and each pellet only its own test against the boxes (the
   player, monsters) closer than that. A long trace costs ~0.5 ms here. */
void                g_fire_hitscan(g_ent *self, const s32 *start, const s32 *dir, int damage, int hspread, int vspread,
                                   int count)
{
    static const s32 zero[3] = { 0, 0, 0 };
    s32             right[3], end[3];
    int             n, k, i;
    q_trace         mid;

    right[0] = dir[1];
    right[1] = -dir[0];
    right[2] = 0;
    for (k = 0; k < 3; ++k)
        end[k] = start[k] + dir[k] * 8192;
    mid = trace_world(start, zero, zero, end, MASK_SHOT & ~CONTENTS_MONSTER);
    for (n = 0; n < count; ++n)
    {
        s32     r = crandom() * hspread, u = crandom() * vspread;   /* units at 8192 away */
        s32     pend[3];
        q_trace t;
        g_ent   *hit = NULL;

        for (k = 0; k < 3; ++k)
            pend[k] = start[k] + fmul(dir[k] * 8192 + fmul(right[k], r) + (k == 2 ? u : 0), mid.fraction);
        memset(&t, 0, sizeof(t));
        t.fraction = FIX(1);
        for (i = 0; i < g_nfull; ++i)
        {
            g_ent   *e = &g_edicts[i];
            s32     bmin[3], bmax[3];

            if (e == self || e->kind == EK_FREE || !e->solid || (e->kind == EK_PLAYER && e->dead))
                continue;
            for (k = 0; k < 3; ++k)
            {
                bmin[k] = e->origin[k] + e->mins[k];
                bmax[k] = e->origin[k] + e->maxs[k];
            }
            if (box_sweep(start, pend, zero, zero, bmin, bmax, &t))
                hit = e;
        }
        if (hit)
        {
            for (k = 0; k < 3; ++k)
                pend[k] = start[k] + fmul(t.fraction, pend[k] - start[k]);
            g_damage(hit, self, damage, pend);
        }
        else
        {
            if (mid.ent)
                g_damage(g_ent_for_model(mid.ent), self, damage, pend);  /* a func_explosive */
            if (mid.fraction < FIX(1) && (n & 1))
                fx_spark(pend);             /* every other one: enough to see */
            if (mid.fraction < FIX(1) && n == 0 && !(rng() & 3))
                s_play(SND_RICOCHET1 + (int)(rng() % 3), pend, ATTN_NORM);     /* now and then, as Quake */
        }
    }
}

void                g_muzzle_flash(const s32 *p, u8 r, u8 g, u8 b)
{
    fx_flash(p, FIX(160), FIX(0.1), r, g, b);
}

void                g_fire_blaster(g_ent *self, const s32 *start, const s32 *dir, int damage, s32 speed)
{
    fx_bolt(self, start, dir, damage, speed);
}

/* ---- the frame ---- */

/* a short one, from after the items and triggers (whose places g_render_ents and the touches keep) */
__attribute__((cold)) g_ent *g_spawn(void)
{
    int             j;

    for (j = g_nitems + g_ntrigs; j < g_nshort; ++j)
    {
        g_ent       *e = G_SHORT_ENT(j);

        if (e->kind == EK_FREE)
        {
            memset(e, 0, G_SHORT);
            return e;
        }
    }
    return NULL;
}

g_ent               *g_ent_at(int n)
{
    return n < g_nfull ? &g_edicts[n] : G_SHORT_ENT(n - g_nfull);
}

/* one gone (an item: its spot too, so g_render_ents and the touches look at it no more) */
void                g_free(g_ent *e)
{
    u32             o = (u32)((u8 *)e - g_shorts);

    e->kind = EK_FREE;
    if ((u8 *)e >= g_shorts && o < (u32)g_nitems * G_SHORT)
        g_item_spots[o / G_SHORT].leaf = -1;
}

/* ---- func_explosive: a brush model that blows up when shot or used ---- */

static __attribute__((cold)) void         explosive_explode(g_ent *self)
{
    const q_model   *m = &lv.models[self->model];
    s32             p[3];
    int             k;

    for (k = 0; k < 3; ++k)
        p[k] = (m->mins[k] >> 1) + (m->maxs[k] >> 1);
    mover_hide(self->model);
    self->kind = EK_FREE;
    g_explosion(p, self, self->activator, self->dmg, FIX(self->dmg + 40), NULL);
    G_UseTargets(self, self->activator);
}

static __attribute__((cold)) void         explosive_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    self->takedamage = false;
    self->activator = attacker;
    explosive_explode(self);
}

static __attribute__((cold)) void         explosive_use(g_ent *self, g_ent *other, g_ent *activator)
{
    self->activator = activator;
    explosive_explode(self);
}

/* ---- monsters that wait to be triggered, or are woken by it ---- */

static __attribute__((cold)) void         monster_use(g_ent *self, g_ent *other, g_ent *activator)
{
    if (self->enemy || self->dead || !activator || activator->kind != EK_PLAYER)
        return;
    self->enemy = activator;
    FoundTarget(self);
}

static __attribute__((cold)) void         monster_triggered_spawn(g_ent *self, g_ent *other, g_ent *activator)
{
    int             k;

    self->inactive = false;
    self->solid = true;
    self->use = monster_use;
    M_droptofloor(self);
    self->leaf = (s16)level_leaf(self->origin);     /* (where it dropped to: g_render_ents' PVS test) */
    for (k = 0; k < 3; ++k)
        self->old_origin[k] = self->origin[k];
    if (activator && activator->kind == EK_PLAYER)
    {
        self->enemy = activator;
        FoundTarget(self);
    }
}

/* ---- the level's entities ---- */

/* what a class makes: nothing (the movers', what isn't done, what this level has no model
   for), a full entity, or a short one: an item, a trigger (a volume the player touches), the rest */
enum { G_NONE, G_FULL, G_ITEM, G_TRIG, G_POINT, G_POOLS };

static __attribute__((cold)) int          g_pool(int c)
{
    if (c >= C_MONSTER_SOLDIER_LIGHT && c <= C_MONSTER_TANK)
        return (c == C_MONSTER_INFANTRY && !models[MDL_INFANTRY].loaded)
               || (c == C_MONSTER_GUNNER && !models[MDL_GUNNER].loaded)
               || (c == C_MONSTER_BERSERK && !models[MDL_BERSERK].loaded)
               || (c == C_MONSTER_TANK && !models[MDL_TANK].loaded) ? G_NONE : G_FULL;
    if (c == C_MISC_EXPLOBOX)
        return models[MDL_BARREL].loaded ? G_FULL : G_NONE;
    if (c == C_FUNC_EXPLOSIVE)
        return G_FULL;
    if (c >= C_ITEM_HEALTH_SMALL && c <= C_KEY_BLUE_KEY)
        return g_item_spawns(c) ? G_ITEM : G_NONE;
    if (c == C_TRIGGER_ONCE || c == C_TRIGGER_MULTIPLE)
        return G_TRIG;
    if (c >= C_TRIGGER_RELAY && c <= C_TARGET_CHANGELEVEL && c != C_TRIGGER_KEY)
        return G_POINT;
    return G_NONE;
}

#define G_STANDINS      (12)                /* short ones kept for G_UseTargets' delays */
#define G_LW_KEEP       (5 * 1024)          /* low work RAM kept for what comes after (r_wall_level's table) */
#define G_CART_ROOM     (160 * 1024)        /* the cart's room to spare: the DSP walls' buffers and two guns' */

static u8           *g_slot_of;             /* each drawable's render entity + 1, 0 none (g_render_ents) */

/* the level's entities of each kind at a skill (-1: all) */
static __attribute__((cold)) void         g_count(int *n, int skill)
{
    const q_erec    *r = (const q_erec *)lv.erecs;
    int             i;

    memset(n, 0, G_POOLS * sizeof(int));
    for (i = 0; i < lv.nerecs; ++i, ++r)
        if (skill < 0 || !(r->spawnflags & (0x100 << skill)))
            ++n[g_pool(r->cls)];
}

/* the short ones, then the items' and triggers' places and the drawables' render entities, in one */
static __attribute__((cold)) u32          g_short_bytes(const int *n)
{
    return (u32)(n[G_ITEM] + n[G_TRIG] + n[G_POINT] + G_STANDINS) * G_SHORT + (u32)n[G_ITEM] * sizeof(g_spot)
           + (u32)n[G_TRIG] * sizeof(g_area) + (u32)(g_nfull + n[G_ITEM]);
}

/* room: low work RAM if it has it, else the cart if it has plenty (Comm Center's decides the
   DSP walls and the guns' slots: r_wall_level, view_level_init); NULL: neither */
static __attribute__((cold)) void         *g_room(u32 bytes, bool cart)
{
    u32             hw, lw, ca;

    level_free(&hw, &lw, &ca);
    if (lw >= bytes + G_LW_KEEP)
        return level_alloc_low(bytes);
    return cart && ca >= bytes + G_CART_ROOM ? cart_alloc(bytes) : NULL;
}

/* The monsters only some levels have: their code on the CD (build.sh build_overlays), read
   onto the cart by the levels that have them, after the models (a model loaded with no code:
   not loaded after all, its monsters left out), and run from there: the cart's misses cost
   about what low work RAM's do, and low work RAM's wanted for the entities' tables, read far
   more often. The file: "Q2OV", the base it was linked at, the image's size, how many words to
   move by where it lands; the image (its spawn's address first); those words' offsets */
static void         (*ovl_spawn[MDL_COUNT])(g_ent *self);

__attribute__((cold)) void g_overlays_load(void)
{
    static const struct { int mdl; const char *file; } ovl[] = {
        { MDL_GUNNER, "GUNNER.OVL" }, { MDL_BERSERK, "BERSERK.OVL" }, { MDL_TANK, "TANK.OVL" },
    };
    unsigned        i;

    for (i = 0; i < sizeof(ovl) / sizeof(ovl[0]); ++i)
    {
        int         m = ovl[i].mdl;
        const u32   *h, *rel;
        u32         lba, size, base, n, k;
        u8          *img;

        ovl_spawn[m] = NULL;
        if (!models[m].loaded)
            continue;
        h = cd_find(ovl[i].file, &lba, &size) ? (const u32 *)cart_load(ovl[i].file) : NULL;
        if (!h || memcmp(h, "Q2OV", 4))
        {
            models[m].loaded = false;
            continue;
        }
        base = h[1];
        size = h[2];
        n = h[3];
        img = (u8 *)(h + 4);                /* (where it landed on the cart: moved in place) */
        rel = (const u32 *)(img + size);
        for (k = 0; k < n; ++k)
            *(u32 *)(img + rel[k]) += (u32)img - base;
        ovl_spawn[m] = *(void (**)(g_ent *))img;
    }
}

/* A new level: room for everything it has at any skill (g_skill -1 spawns it all) where there's
   room for it, else for this skill's: the full ones, then the short ones. Each time: the
   entities at this skill (any past the room: g_dropped), the items first among the short
   ones, then the triggers, then the rest */
__attribute__((cold)) void g_init(void)
{
    const q_erec    *r;
    int             i, k, at[G_POOLS], end[G_POOLS];

    if (!g_edicts)
    {
        int         all[G_POOLS], now[G_POOLS];
        const int   *n = all;
        u8          *b = NULL;

        g_count(all, -1);
        g_count(now, g_skill);
        /* (LWRAM for every skill, LWRAM for this one, the cart for every skill, for this one: the
           cart's slower, and has room now: section 50) */
        for (k = 0; k < 4 && !g_edicts; ++k)
        {
            g_nfull = imin(1 + (k & 1 ? now : all)[G_FULL], MAX_FULL);
            g_edicts = g_room((u32)g_nfull * sizeof(g_ent), k >= 2);
        }
        if (!g_edicts)
            g_edicts = (g_ent *)cart_alloc((u32)g_nfull * sizeof(g_ent));
        for (k = 0; k < 4 && !b; ++k)
            b = g_room(g_short_bytes(n = k & 1 ? now : all), k >= 2);
        if (!b)
            b = cart_alloc(g_short_bytes(n));
        g_nitems = n[G_ITEM];
        g_ntrigs = n[G_TRIG];
        g_nshort = g_nitems + g_ntrigs + n[G_POINT] + G_STANDINS;
        g_shorts = b;
        g_item_spots = (g_spot *)(b + (u32)g_nshort * G_SHORT);
        g_trig_areas = (g_area *)(g_item_spots + g_nitems);
        g_slot_of = (u8 *)(g_trig_areas + g_ntrigs);
    }
    g_player = &g_edicts[0];
    memset(g_edicts, 0, (u32)g_nfull * sizeof(g_ent));
    memset(g_shorts, 0, (u32)g_nshort * G_SHORT);
    for (i = 0; i < g_nitems; ++i)
        g_item_spots[i].leaf = -1;
    memset(g_trig_areas, 0, (u32)g_ntrigs * sizeof(g_area));
    memset(g_slot_of, 0, (u32)(g_nfull + g_nitems));
    for (i = 0; i < GAME_ENTS; ++i)
    {
        ents[i].live = false;
        ents[i].g_owner = 0;
    }
    memset(&level, 0, sizeof(level));
    g_dropped = 0;
    ai_reset();
    for (i = 0; i < MON_GROUPS; ++i)
        level.mon_acc[i] = FRAMETIME * i / MON_GROUPS;
    kills = total_monsters = found_secrets = total_secrets = found_goals = total_goals = 0;
    level_complete = false;
    g_client_init();
    g_player->kind = EK_PLAYER;
    g_player->solid = true;
    g_player->health = g_player->max_health = 100;
    g_player->viewheight = FIX(22);
    for (k = 0; k < 3; ++k)
    {
        g_player->mins[k] = p_mins[k];
        g_player->maxs[k] = p_maxs[k];
    }
    at[G_FULL] = 1;
    end[G_FULL] = g_nfull;
    at[G_ITEM] = 0;
    end[G_ITEM] = at[G_TRIG] = g_nitems;
    end[G_TRIG] = at[G_POINT] = g_nitems + g_ntrigs;
    end[G_POINT] = g_nshort;
    for (i = 0, r = (const q_erec *)lv.erecs; i < lv.nerecs; ++i, ++r)
    {
        g_ent   *e;
        int     c = r->cls, pool = g_pool(c), n;

        if (pool == G_NONE)
            continue;                       /* (movers.c has the movers) */
        if (g_skill >= 0 && r->spawnflags & (0x100 << g_skill))
            continue;                       /* not at this skill (Quake's SPAWNFLAG_NOT_EASY, _MEDIUM, _HARD) */
#ifdef OLD_SET
        {
            /* (OPT=-DOLD_SET: only what the old table of 64 had room for, the first in the level's
               order: the same fight as before, to time) */
            int live = 0, j;

            for (j = 1; j < g_nfull + g_nshort; ++j)
                live += g_ent_at(j)->kind != EK_FREE;
            if (live >= 63)
                break;
        }
#endif
        if (at[pool] == end[pool])
        {
            ++g_dropped;                    /* (no room: only full ones past MAX_FULL) */
            continue;
        }
        n = at[pool]++;
        e = pool == G_FULL ? &g_edicts[n] : G_SHORT_ENT(n);
        e->cls = c;
        for (k = 0; k < 3; ++k)
            e->origin[k] = r->origin[k];
        e->yaw = r->angle;
        if (pool == G_FULL)
        {
            for (k = 0; k < 3; ++k)
                e->old_origin[k] = r->origin[k];
            e->old_yaw = r->angle;
        }
        e->spawnflags = r->spawnflags;
        e->targetname = r->targetname;
        e->target = r->target;
        e->killtarget = r->killtarget;
        e->message = r->message ? lv.strings + r->message : NULL;
        e->delay = r->delay == UNSET ? 0 : r->delay;
        e->wait = r->wait == UNSET ? 0 : r->wait;
        e->random = r->random == UNSET ? 0 : r->random;
        e->dmg = r->dmg;
        e->count = r->count;
        e->model = r->model;
        e->kind = EK_POINT;
        if (c >= C_MONSTER_SOLDIER_LIGHT && c <= C_MONSTER_TANK)
        {
            if (c != C_MONSTER_SOLDIER_LIGHT && c != C_MONSTER_SOLDIER && c != C_MONSTER_SOLDIER_SS)
            {
                int m = c == C_MONSTER_INFANTRY ? MDL_INFANTRY : c == C_MONSTER_GUNNER ? MDL_GUNNER
                        : c == C_MONSTER_BERSERK ? MDL_BERSERK : MDL_TANK;

                if (!models[m].loaded)
                {
                    e->kind = EK_FREE;      /* (an optional one with no room on the cart: tools/models.txt) */
                    continue;
                }
                e->mdl = &models[m];
                if (c == C_MONSTER_INFANTRY)
                    SP_monster_infantry(e);
                else
                    ovl_spawn[m](e);        /* (its code loaded with the level: g_overlays_load) */
            }
            else
            {
                e->mdl = &models[MDL_SOLDIER];
                SP_monster_x_soldier(e, c == C_MONSTER_SOLDIER_LIGHT ? 0 : c == C_MONSTER_SOLDIER ? 2 : 4);
            }
            e->takedamage = true;
            ++total_monsters;
            for (k = 0; k < 3; ++k)
                e->old_origin[k] = e->origin[k];    /* (where its spawn dropped it to: no blend from above) */
            if (e->spawnflags & 2)
            {
                /* not there until something triggers it */
                e->inactive = true;
                e->solid = false;
                e->use = monster_triggered_spawn;
            }
            else
                e->use = monster_use;
            e->leaf = (s16)level_leaf(e->origin);
            continue;
        }
        if (c == C_FUNC_EXPLOSIVE)
        {
            e->health = r->health ? r->health : 100;
            e->takedamage = r->targetname == 0 || r->health != 0;
            e->die = explosive_die;
            e->use = explosive_use;
            continue;
        }
        if (pool == G_ITEM ? g_spawn_item(e, r) : g_spawn_point(e, r))
        {
            if (pool == G_ITEM)
            {
                g_spot *sp = &g_item_spots[n];

                for (k = 0; k < 3; ++k)
                    sp->pos[k] = (s16)(e->origin[k] >> 16);
                sp->leaf = (s16)level_leaf(e->origin);
            }
            else if (pool == G_TRIG)
            {
                g_area *a = &g_trig_areas[n - g_nitems];

                for (k = 0; k < 3; ++k)
                {
                    a->lo[k] = lv.models[r->model].mins[k];
                    a->hi[k] = lv.models[r->model].maxs[k];
                }
            }
            else if (e->kind == EK_OBJECT)
                e->leaf = (s16)level_leaf(e->origin);
            continue;
        }
        e->kind = EK_FREE;                  /* not something we do yet */
    }
}

void                g_player_noise(void)
{
    sound_entity = g_player;
    sound_entity_framenum = level.framenum;
}

static void         g_tick(void)
{
    int             i;

    level.time += FRAMETIME;
    ++level.framenum;
    level.sight_client = g_player->dead ? NULL : g_player;
    g_touch_triggers();
    g_touch_items();
    for (i = 1; i < g_nfull + g_nshort; ++i)
    {
        g_ent *e;

        if (i == g_nfull)
            i += g_nitems;                  /* (items don't think) */
        e = g_ent_at(i);
        /* all but the active monsters: its think, when it's time */
        if ((e->kind != EK_MONSTER || e->inactive) && e->kind != EK_FREE && e->think && e->nextthink
            && level.time >= e->nextthink)
        {
            e->nextthink = 0;
            e->think(e);
        }
    }
}

/* a group's monsters' tick, in their own time */
static void         g_tick_monsters(int g)
{
    s32             now = level.time;
    int             i, k;

    level.time = level.mon_time[g] += FRAMETIME;
    for (i = 1 + (MON_GROUPS - 1 + g) % MON_GROUPS; i < g_nfull; i += MON_GROUPS)
    {
        g_ent *e = &g_edicts[i];

        if (e->kind != EK_MONSTER || e->inactive)
            continue;
        for (k = 0; k < 3; ++k)
            e->old_origin[k] = e->origin[k];
        e->old_yaw = e->yaw;
        e->old_frame = e->frame;
        if (!e->move)
            continue;                       /* dead and done */
        e->flags &= ~FL_STEPPED;
        M_MoveFrame(e);
        /* ground checks only when it's in the air, or moved other than by a step that found the
           floor (Quake's: the step's trace is its groundentity; a box trace saved a tick) */
        if (!e->on_ground || (!(e->flags & FL_STEPPED) && (e->origin[0] != e->old_origin[0]
            || e->origin[1] != e->old_origin[1] || e->origin[2] != e->old_origin[2])))
            monster_physics(e);
        if (e->origin[0] != e->old_origin[0] || e->origin[1] != e->old_origin[1] || e->origin[2] != e->old_origin[2])
            e->leaf = (s16)level_leaf(e->origin);   /* (g_render_ents': is it in view) */
        if (g_solid_slot[i] != 0xFF)
            solid_box(&g_solids[g_solid_slot[i]], e);
    }
    level.time = now;
}

void                g_frame(s32 dt)
{
    int             n = 0, k, g;

    /* the player, from pmove */
    for (k = 0; k < 3; ++k)
        g_player->origin[k] = pl.origin[k];
    if (player_flash)
        player_flash = imax(player_flash - fmul(dt, FIX(400)), 0);
    g_solids_update();
    level.acc += dt;
    while (level.acc >= FRAMETIME && n++ < 3)
    {
        level.acc -= FRAMETIME;
        s_lag(level.acc);                   /* (its moment: the rest of the frame's time ago) */
        g_tick();
    }
    if (level.acc >= FRAMETIME)
        level.acc = 0;                      /* far behind: let it go */
    for (g = 0; g < MON_GROUPS; ++g)
    {
        level.mon_acc[g] += dt;
        for (n = 0; level.mon_acc[g] >= FRAMETIME && n < 3; ++n)
        {
            level.mon_acc[g] -= FRAMETIME;
            s_lag(level.mon_acc[g]);
            g_tick_monsters(g);
        }
        while (level.mon_acc[g] >= FRAMETIME)
            level.mon_acc[g] -= FRAMETIME;  /* (far behind: those ticks go, the group keeps its place) */
    }
    s_lag(-1);
}

/* The renderer's first GAME_ENTS entities are the monsters, barrels and items in view. Each gets
   one when it's in a leaf of the PVS marked, or near the camera, and keeps it until it's been
   neither a while (or another in the PVS needs it). Only those in the PVS are filled in and
   drawn; the rest keep their light. The PVS marked is the last frame's: on a frame the camera's
   gone into another cluster, g_render_late fills in the rest, once the camera's moved (what's
   new in its PVS is near, or could be seen from the cluster it left). The ones that can be
   drawn are numbered: the full ones by their place, then the items (g_nfull + j) */
#define SLOT_LINGER     (60)                /* frames not wanted before it lets its render entity go */
#define SLOT_NEAR       (1024)              /* units, each way */

static int          slot_hint;

static void         slot_drop(int s)
{
    q_entity        *r = &ents[s];

    g_slot_of[r->g_owner - 1] = 0;
    r->g_owner = 0;
    r->live = false;
}

/* a render entity for drawable d: a free one, else the one longest not wanted, at least idle
   frames (none: -1) */
static int          slot_take(int d, int idle)
{
    int             s = 0, k, best = -1;

    --idle;
    for (k = 0; k < GAME_ENTS; ++k)
    {
        s = (slot_hint + k) & (GAME_ENTS - 1);
        if (!ents[s].g_owner)
            break;
        if (ents[s].g_idle > idle)
        {
            idle = ents[s].g_idle;
            best = s;
        }
    }
    if (k == GAME_ENTS)
    {
        if (best < 0)
        {
#ifdef SLOT_CHECK
            extern u32 slot_full;

            ++slot_full;
#endif
            return -1;                      /* (all of them wanted: it waits) */
        }
        s = best;
        slot_drop(s);
    }
    slot_hint = s + 1;
    ents[s].g_owner = (u16)(d + 1);
    ents[s].g_idle = 0;
    ents[s].g_leaf = -1;                    /* (new to it: its leaf and light found afresh) */
    ents[s].live = false;
    g_slot_of[d] = (u8)(s + 1);
    return s;
}

/* its render entity from it, blended between the last tick and this one (f: 0..1 of a tick).
   Once it's drawn, an item only turns (from g_yaw0) and a barrel stays as it is: only a
   monster's read again */
static void         fill_ent(q_entity *r, const g_ent *e, s32 f, int spin)
{
    int             k;

    if (!r->live)
    {
        r->mdl = e->mdl;
        r->pitch = 0;
    }
    else if (e->kind != EK_MONSTER)
    {
        if (e->kind == EK_ITEM)
            r->yaw = (r->g_yaw0 + spin) & 0xFFFF;
        return;
    }
    if (e->kind == EK_MONSTER)
    {
        for (k = 0; k < 3; ++k)
        {
            s32 o = e->old_origin[k] + fmul(e->origin[k] - e->old_origin[k], f);

            if (o != r->origin[k])
            {
                r->origin[k] = o;
                r->g_moved = true;
            }
        }
        r->yaw = (e->old_yaw + fmul((s16)((e->yaw - e->old_yaw) & 0xFFFF), f)) & 0xFFFF;
        r->skin = e->skinnum;
        r->oldframe = e->old_frame;
        r->frame = e->frame;
        r->lerp = e->old_frame == e->frame ? 0 : f;
    }
    else
    {
        for (k = 0; k < 3; ++k)
            if (e->origin[k] != r->origin[k])
            {
                r->origin[k] = e->origin[k];
                r->g_moved = true;
            }
        r->g_yaw0 = e->yaw;
        r->yaw = e->kind == EK_ITEM ? (e->yaw + spin) & 0xFFFF : e->yaw;
        r->skin = 0;
        r->frame = r->oldframe = 0;
        r->lerp = 0;
    }
    r->live = true;
}

/* drawable d's render entity: want 2 in the PVS (filled in), 1 only near (kept, its idle 1: one
   in the PVS may take it when there's no other), 0 neither */
static void         render_ent(g_ent *e, int d, int want, s32 f, int spin)
{
    int             s = g_slot_of[d] - 1;

    if (!((e->kind == EK_MONSTER || e->kind == EK_ITEM || e->kind == EK_OBJECT) && !e->inactive))
    {                                       /* (its model: there, or it wouldn't have come: g_pool) */
        if (s >= 0)
            slot_drop(s);
        if (d >= g_nfull && e->kind != EK_ITEM)
            g_item_spots[d - g_nfull].leaf = -1;    /* (taken: not looked at again) */
        return;
    }
    if (want)
    {
        if (s < 0 && (s = slot_take(d, want == 2 ? 1 : 2)) < 0)
            return;
        ents[s].g_idle = (u16)(2 - want);
    }
    else if (s < 0)
        return;
    else if (++ents[s].g_idle > SLOT_LINGER)
    {
        slot_drop(s);
        return;
    }
    if (want == 2)
        fill_ent(&ents[s], e, f, spin);
    else
        ents[s].live = false;               /* (kept, not drawn: unless g_render_late fills it in) */
}

static inline bool  near_cam(const int *c, int x, int y, int z)
{
    return iabs(x - c[0]) < SLOT_NEAR && iabs(y - c[1]) < SLOT_NEAR && iabs(z - c[2]) < SLOT_NEAR;
}

static int          item_spin(void)
{
    return (int)fmul(level.time + level.acc, 0x4700);  /* items: 100 degrees a second */
}

void                g_render_ents(void)
{
    int             i, c[3], spin = item_spin();

    for (i = 0; i < 3; ++i)
        c[i] = cam.pos[i] >> 16;
    for (i = 1; i < g_nfull; ++i)
    {
        g_ent       *e = &g_edicts[i];

        if (e->kind == EK_FREE && !g_slot_of[i])
            continue;
        render_ent(e, i, r_leaf_in_pvs(e->leaf) ? 2 : near_cam(c, e->origin[0] >> 16, e->origin[1] >> 16, e->origin[2] >> 16),
                   level.mon_acc[i % MON_GROUPS] * 10, spin);
    }
    for (i = 0; i < g_nitems; ++i)
    {
        const g_spot *sp = &g_item_spots[i];
        int         d = g_nfull + i, s = g_slot_of[d] - 1;
        int         want = sp->leaf < 0 ? 0 : r_leaf_in_pvs(sp->leaf) ? 2 : near_cam(c, sp->pos[0], sp->pos[1], sp->pos[2]);

        if (s >= 0 && sp->leaf >= 0 && (want != 2 || ents[s].live))
        {
            /* still there (its spot's leaf goes when it's taken) and drawn or kept: not read (render_ent's
               the same) */
            q_entity *r = &ents[s];

            if (want == 2)
            {
                r->yaw = (r->g_yaw0 + spin) & 0xFFFF;
                r->g_idle = 0;
            }
            else if (want || ++r->g_idle <= SLOT_LINGER)
            {
                r->live = false;
                if (want)
                    r->g_idle = 1;
            }
            else
                slot_drop(s);
        }
        else if (want || s >= 0)            /* (the rest: not read) */
            render_ent(G_SHORT_ENT(i), d, want, 0, spin);
    }
}

/* (once the camera's moved) pvs: the PVS marked is the camera's; if not, every one kept */
void                g_render_late(bool pvs)
{
    int             s, spin;

    if (pvs)
        return;
    spin = item_spin();
    for (s = 0; s < GAME_ENTS; ++s)
    {
        q_entity    *r = &ents[s];
        int         d = r->g_owner - 1;

        if (d >= 0 && !r->live)
            fill_ent(r, g_ent_at(d), d < g_nfull ? level.mon_acc[d % MON_GROUPS] * 10 : 0, spin);
    }
}

#ifdef SLOT_CHECK
/* (OPT=-DSLOT_CHECK: after the walk, with this frame's PVS marked) the models in it with no render
   entity this frame: drawn a frame late, if they could be seen at all; and the most held at once */
u32                 slot_frames, slot_missed, slot_peak, slot_moved, slot_full;

void                g_slot_check(void)
{
    static int      was = -2;
    int             i, k, held = 0, c = lv.leafs[level_leaf(cam.pos)].cluster;
    bool            moved = c != was;       /* (into another cluster: g_render_ents had the last one's PVS) */

    for (i = 1; i < g_nfull + g_nitems; ++i)
    {
        g_ent       *e = g_ent_at(i);

        if (!((e->kind == EK_MONSTER || e->kind == EK_ITEM || e->kind == EK_OBJECT) && !e->inactive && e->mdl
              && e->mdl->loaded))
            continue;
        k = *(volatile u8 *)UNCACHED(&g_slot_of[i]);    /* (the slave's, maybe) */
        if (k)
            ++held;
        if ((!k || !((volatile q_entity *)UNCACHED(&ents[k - 1]))->live) && r_leaf_in_pvs(level_leaf(e->origin)))
        {
            /* (and on the screen: a sphere of 64 units against the view's sides, as ents_light_dyn) */
            s32 d0 = e->origin[0] - cam.pos[0], d1 = e->origin[1] - cam.pos[1], d2 = e->origin[2] - cam.pos[2];
            s32 vx = fmul(d0, cam.right[0]) + fmul(d1, cam.right[1]) + fmul(d2, cam.right[2]);
            s32 vy = fmul(d0, cam.up[0]) + fmul(d1, cam.up[1]) + fmul(d2, cam.up[2]);
            s32 vz = fmul(d0, cam.fwd[0]) + fmul(d1, cam.fwd[1]) + fmul(d2, cam.fwd[2]);

            if (!(vz < -FIX(64) || iabs(vx) - vz > FIX(91) || iabs(vy) - fmul(vz, FIX(0.7)) > FIX(79)))
                ++*(moved ? &slot_moved : &slot_missed);
        }
    }
    was = c;
    ++slot_frames;
    slot_peak = (u32)imax((int)slot_peak, held);
}
#endif

/* (debugging) the entity drawn as render entity s */
__attribute__((cold)) g_ent *g_drawn(int s)
{
    int             d = ents[s].g_owner - 1;

    return s < GAME_ENTS && d >= 0 ? g_ent_at(d) : NULL;     /* (the numbers are g_ent_at's: the items lead the short ones) */
}
