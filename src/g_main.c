/*
** The game's frame: Quake 2's G_RunFrame at 10 a second, the entities, and
** what they share: traces that see monsters and the player as boxes (as
** SV_Trace does), damage, and the weapons.
**
** Entity 0 is the player; its position comes from pmove each frame.
*/
#include "game.h"

g_level             level;
g_ent               *g_edicts;              /* low work RAM: high's for the renderer */
g_ent               *g_player;              /* g_edicts[0] (g_init) */
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
    u32             x = (u32)iabs(v[0] >> 11), y = (u32)iabs(v[1] >> 11), z = (u32)iabs(v[2] >> 11);

    return (s32)(isqrt(x * x + y * y + z * z) << 11);
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
static g_solidbox   g_solids[MAX_EDICTS];
static u8           g_solid_slot[MAX_EDICTS];   /* an edict's box, or 0xFF */
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
    for (i = 0; i < MAX_EDICTS; ++i)
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
    extern u32      tr_ticks[4];
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
        if (e->kind == EK_PLAYER && (pl.noclip || e->dead))
            continue;
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

void                g_damage(g_ent *targ, g_ent *attacker, int damage, const s32 *point)
{
    if (!targ || targ->kind == EK_FREE || targ->dead || damage <= 0 || targ->inactive)
        return;
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
g_ent               *g_ent_for_model(int model)
{
    int             i;

    for (i = 1; i < MAX_EDICTS; ++i)
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
        for (i = 0; i < MAX_EDICTS; ++i)
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

g_ent               *g_spawn(void)
{
    int             i;

    for (i = 1; i < MAX_EDICTS; ++i)
        if (g_edicts[i].kind == EK_FREE)
        {
            memset(&g_edicts[i], 0, sizeof(g_ent));
            return &g_edicts[i];
        }
    return NULL;
}

/* ---- func_explosive: a brush model that blows up when shot or used ---- */

static void         explosive_explode(g_ent *self)
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

static void         explosive_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    self->takedamage = false;
    self->activator = attacker;
    explosive_explode(self);
}

static void         explosive_use(g_ent *self, g_ent *other, g_ent *activator)
{
    self->activator = activator;
    explosive_explode(self);
}

/* ---- monsters that wait to be triggered, or are woken by it ---- */

static void         monster_use(g_ent *self, g_ent *other, g_ent *activator)
{
    if (self->enemy || self->dead || !activator || activator->kind != EK_PLAYER)
        return;
    self->enemy = activator;
    FoundTarget(self);
}

static void         monster_triggered_spawn(g_ent *self, g_ent *other, g_ent *activator)
{
    self->inactive = false;
    self->solid = true;
    self->use = monster_use;
    M_droptofloor(self);
    if (activator && activator->kind == EK_PLAYER)
    {
        self->enemy = activator;
        FoundTarget(self);
    }
}

void                g_init(void)
{
    const q_erec    *r = (const q_erec *)lv.erecs;
    int             i, k;

    if (!g_edicts)
        g_edicts = level_alloc_low(MAX_EDICTS * sizeof(g_ent));
    g_player = &g_edicts[0];
    memset(g_edicts, 0, MAX_EDICTS * sizeof(g_ent));
    memset(&level, 0, sizeof(level));
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
    for (i = 0; i < lv.nerecs; ++i, ++r)
    {
        g_ent   *e;
        int     c = r->cls;

        if (c == C_INFO_PLAYER_START || c == C_PATH_CORNER || c == C_POINT_COMBAT || c == C_TARGET_SPEAKER)
            continue;
        if (g_skill >= 0 && r->spawnflags & (0x100 << g_skill))
            continue;                       /* not at this skill (Quake's SPAWNFLAG_NOT_EASY, _MEDIUM, _HARD) */
        if ((c >= C_FUNC_DOOR && c <= C_FUNC_WATER && c != C_FUNC_EXPLOSIVE) || c == C_MISC_TELEPORTER_DEST)
            continue;                       /* movers.c has them */
        if (!(e = g_spawn()))
            break;
        e->cls = c;
        for (k = 0; k < 3; ++k)
            e->origin[k] = e->old_origin[k] = r->origin[k];
        e->yaw = e->old_yaw = r->angle;
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
        if (c >= C_MONSTER_SOLDIER_LIGHT && c <= C_MONSTER_INFANTRY)
        {
            if (c == C_MONSTER_INFANTRY)
            {
                if (!models[MDL_INFANTRY].loaded)
                {
                    e->kind = EK_FREE;
                    continue;
                }
                e->mdl = &models[MDL_INFANTRY];
                SP_monster_infantry(e);
            }
            else
            {
                e->mdl = &models[MDL_SOLDIER];
                SP_monster_x_soldier(e, c == C_MONSTER_SOLDIER_LIGHT ? 0 : c == C_MONSTER_SOLDIER ? 2 : 4);
            }
            e->takedamage = true;
            ++total_monsters;
            if (e->spawnflags & 2)
            {
                /* not there until something triggers it */
                e->inactive = true;
                e->solid = false;
                e->use = monster_triggered_spawn;
            }
            else
                e->use = monster_use;
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
        if (g_spawn_item(e, r) || g_spawn_point(e, r))
            continue;
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
    for (i = 1; i < MAX_EDICTS; ++i)
    {
        g_ent *e = &g_edicts[i];

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
    for (i = 1 + (MON_GROUPS - 1 + g) % MON_GROUPS; i < MAX_EDICTS; i += MON_GROUPS)
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
        M_MoveFrame(e);
        /* ground checks only when it's moved or is in the air (Quake's linkcount test) */
        if (!e->on_ground || e->origin[0] != e->old_origin[0] || e->origin[1] != e->old_origin[1]
            || e->origin[2] != e->old_origin[2])
            monster_physics(e);
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
            g_tick_monsters(g);
        }
        while (level.mon_acc[g] >= FRAMETIME)
            level.mon_acc[g] -= FRAMETIME;  /* (far behind: those ticks go, the group keeps its place) */
    }
}

/* monsters, items and objects into the renderer's entities, blended between the last tick and this one */
void                g_render_ents(void)
{
    s32             f;                      /* 0..1 of a tick (a monster's group's) */
    int             i, k, spin = (int)fmul(level.time + level.acc, 0x4700);    /* items: 100 degrees a second */

    for (i = 0; i < MAX_EDICTS; ++i)
    {
        g_ent       *e = &g_edicts[i];
        q_entity    *r = &ents[i];
        bool        show = (e->kind == EK_MONSTER || e->kind == EK_ITEM || e->kind == EK_OBJECT) && !e->inactive
                           && e->mdl && e->mdl->loaded;

        if (!show)
        {
            r->live = false;
            continue;
        }
        if (!r->live)
            r->g_leaf = -1;
        r->mdl = e->mdl;
        r->pitch = 0;
        if (e->kind == EK_MONSTER)
        {
            f = level.mon_acc[i % MON_GROUPS] * 10;
            for (k = 0; k < 3; ++k)
                r->origin[k] = e->old_origin[k] + fmul(e->origin[k] - e->old_origin[k], f);
            r->yaw = (e->old_yaw + fmul((s16)((e->yaw - e->old_yaw) & 0xFFFF), f)) & 0xFFFF;
            r->skin = e->skinnum;
            r->oldframe = e->old_frame;
            r->frame = e->frame;
            r->lerp = e->old_frame == e->frame ? 0 : f;
        }
        else
        {
            for (k = 0; k < 3; ++k)
                r->origin[k] = e->origin[k];
            r->yaw = e->kind == EK_ITEM ? (e->yaw + spin) & 0xFFFF : e->yaw;
            r->skin = 0;
            r->frame = r->oldframe = 0;
            r->lerp = 0;
        }
        r->live = true;
    }
    nents = MAX_EDICTS;
}
