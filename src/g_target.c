/*
** Triggers and targets: Quake 2's game/g_trigger.c, g_target.c, g_misc.c and
** G_UseTargets (g_utils.c), cut down.
**
** Using an entity's targets: after its delay, show its message, remove its
** killtargets, and use everything whose targetname is its target - entities
** here, and doors, lifts and buttons (movers.c) by the same names.
*/
#include "game.h"

const char          *center_msg;
s32                 center_until;
int                 found_secrets, total_secrets, found_goals, total_goals;

void                g_centerprint(const char *msg)
{
    center_msg = msg;
    center_until = level.time + FIX(3);
}

/* ---- using targets ---- */

static void         think_delay(g_ent *ent)
{
    G_UseTargets(ent, ent->activator);
    ent->kind = EK_FREE;
}

void                G_UseTargetName(int name, g_ent *other, g_ent *activator)
{
    int             i;

    if (!name)
        return;
    for (i = 0; i < g_nfull + g_nshort; ++i)
    {
        g_ent *t = g_ent_at(i);

        if (t->kind != EK_FREE && t->targetname == name && t->use && t != other)
            t->use(t, other, activator);
    }
    movers_use(name);
}

int                 g_uses;

void                G_UseTargets(g_ent *ent, g_ent *activator)
{
    int             i;

    ++g_uses;
    if (ent->delay)
    {
        /* do it later, from a stand-in (none free: now) */
        g_ent *t = g_spawn();

        if (t)
        {
            t->kind = EK_POINT;
            t->activator = activator;
            t->message = ent->message;
            t->target = ent->target;
            t->killtarget = ent->killtarget;
            t->nextthink = level.time + ent->delay;
            t->think = think_delay;
            return;
        }
    }
    if (ent->message && activator == g_player)
    {
        g_centerprint(ent->message);
        s_play(SND_TALK, NULL, ATTN_NONE);
    }
    if (ent->killtarget)
        for (i = 1; i < g_nfull + g_nshort; ++i)
        {
            g_ent *t = g_ent_at(i);

            if (t->kind != EK_FREE && t->targetname == ent->killtarget)
                g_free(t);
        }
    G_UseTargetName(ent->target, ent, activator);
}

void                g_mover_fired(int target)
{
    G_UseTargetName(target, NULL, g_player);
}

/* ---- trigger_once / trigger_multiple ---- */

static void         multi_wait(g_ent *ent)
{
    ent->nextthink = 0;
    ent->inactive = false;
}

static void         multi_trigger(g_ent *ent)
{
    if (ent->inactive)
        return;                             /* already been triggered, and waiting */
    G_UseTargets(ent, ent->activator);
    if (ent->wait > 0)
    {
        ent->inactive = true;
        ent->think = multi_wait;
        ent->nextthink = level.time + ent->wait;
    }
    else
        ent->kind = EK_FREE;                /* once */
}

static void         touch_multi(g_ent *self, g_ent *other)
{
    if (other != g_player || self->spawnflags & 2)
        return;
    self->activator = other;
    multi_trigger(self);
}

static void         use_multi(g_ent *self, g_ent *other, g_ent *activator)
{
    self->activator = activator;
    multi_trigger(self);
}

/* a trigger that waits to be triggered itself (spawnflag 4) */
static void         trigger_enable(g_ent *self, g_ent *other, g_ent *activator)
{
    self->inactive = false;
    self->use = use_multi;
}

/* the player against the triggers' boxes (g_trig_areas: only a trigger touched is read) */
void                g_touch_triggers(void)
{
    int             i, k;
    s32             plo[3], phi[3];

    if (g_player->dead || pl.noclip)
        return;
    for (k = 0; k < 3; ++k)
    {
        plo[k] = g_player->origin[k] + g_player->mins[k];
        phi[k] = g_player->origin[k] + g_player->maxs[k];
    }
    for (i = 0; i < g_ntrigs; ++i)
    {
        const g_area *a = &g_trig_areas[i];
        g_ent   *t;

        if (a->hi[0] < plo[0] || a->lo[0] > phi[0] || a->hi[1] < plo[1] || a->lo[1] > phi[1]
            || a->hi[2] < plo[2] || a->lo[2] > phi[2])
            continue;
        t = G_SHORT_ENT(g_nitems + i);
        if (t->kind == EK_TRIGGER && !t->inactive && t->touch)
            t->touch(t, g_player);
    }
}

/* ---- relays, counters, timers ---- */

static void         use_relay(g_ent *self, g_ent *other, g_ent *activator)
{
    G_UseTargets(self, activator);
}

static void         think_always(g_ent *self)
{
    G_UseTargets(self, g_player);
    self->kind = EK_FREE;
}

static void         use_counter(g_ent *self, g_ent *other, g_ent *activator)
{
    if (self->count == 0)
        return;
    if (--self->count)
    {
        if (!(self->spawnflags & 1))
            g_centerprint(self->count == 1 ? "Only 1 more to go..." : "More to go...");
        return;
    }
    if (!(self->spawnflags & 1))
        g_centerprint("Sequence completed!");
    self->activator = activator;
    G_UseTargets(self, activator);
}

static void         think_timer(g_ent *self)
{
    G_UseTargets(self, self->activator);
    self->nextthink = level.time + self->wait + fmul(crandom(), self->random);
}

static void         use_timer(g_ent *self, g_ent *other, g_ent *activator)
{
    self->activator = activator;
    if (self->nextthink)
    {
        self->nextthink = 0;                /* on: turn it off */
        return;
    }
    if (self->delay)
        self->nextthink = level.time + self->delay;
    else
        think_timer(self);
}

/* ---- targets ---- */

/* radius damage: less further out, halved for the one who caused it, not through walls */
void                T_RadiusDamage(const s32 *p, g_ent *inflictor, g_ent *attacker, int damage, g_ent *ignore, s32 radius)
{
    static const s32 zero[3] = { 0, 0, 0 };
    int             i, k;

    for (i = 0; i < g_nfull; ++i)
    {
        g_ent   *e = &g_edicts[i];             /* (only full ones take damage) */
        s32     v[3], points, len;
        q_trace t;

        if (e == ignore || !e->takedamage || e->kind == EK_FREE || e->dead || e->inactive)
            continue;
        for (k = 0; k < 3; ++k)
            v[k] = e->origin[k] + (e->mins[k] >> 1) + (e->maxs[k] >> 1) - p[k];
        len = vlen(v);
        if (len > radius + FIX(32))
            continue;
        points = FIX(damage) - (len >> 1);
        if (e == attacker)
            points >>= 1;
        if (points <= 0)
            continue;
        /* CanDamage: a clear line to its middle */
        for (k = 0; k < 3; ++k)
            v[k] += p[k];
        t = trace_world(p, zero, zero, v, MASK_SOLID_ONLY);
        if (t.fraction < FIX(0.95))
            continue;
        g_damage(e, attacker, points >> 16, v);
    }
}

void                g_explosion(const s32 *p, g_ent *inflictor, g_ent *attacker, int damage, s32 radius, g_ent *ignore)
{
    fx_explosion(p);
    if (inflictor)
        s_play(SND_EXPLOSION, p, ATTN_NORM);   /* (a rocket's or a grenade's: fx.c, its own) */
    if (damage)
        T_RadiusDamage(p, inflictor, attacker, damage, ignore, radius);
}

static void         think_explosion(g_ent *self)
{
    g_explosion(self->origin, self, self->activator, self->dmg, FIX(self->dmg + 40), NULL);
    self->nextthink = 0;
}

static void         use_explosion(g_ent *self, g_ent *other, g_ent *activator)
{
    self->activator = activator;
    if (self->delay)
    {
        self->think = think_explosion;
        self->nextthink = level.time + self->delay;
    }
    else
        think_explosion(self);
}

static void         use_splash(g_ent *self, g_ent *other, g_ent *activator)
{
    int             n;

    for (n = 0; n < 4; ++n)
    {
        s32 q[3];

        q[0] = self->origin[0] + crandom() * 8;
        q[1] = self->origin[1] + crandom() * 8;
        q[2] = self->origin[2] + crandom() * 8;
        fx_spark(q);
    }
}

static void         use_secret(g_ent *self, g_ent *other, g_ent *activator)
{
    ++found_secrets;
    s_play(SND_SECRET, NULL, ATTN_NONE);
    G_UseTargets(self, activator);
    self->kind = EK_FREE;
}

static void         use_goal(g_ent *self, g_ent *other, g_ent *activator)
{
    ++found_goals;
    G_UseTargets(self, activator);
    self->kind = EK_FREE;
}

static void         use_help(g_ent *self, g_ent *other, g_ent *activator)
{
    g_centerprint("Computer updated");
}

const char          *g_next_map;

static void         use_changelevel(g_ent *self, g_ent *other, g_ent *activator)
{
    level_complete = true;
    g_next_map = self->message;             /* (the map key: where it leads) */
}

/* ---- misc_explobox: the barrel ---- */

static void         barrel_explode(g_ent *self)
{
    s32             p[3];

    p[0] = self->origin[0];
    p[1] = self->origin[1];
    p[2] = self->origin[2] + FIX(20);
    self->kind = EK_FREE;
    g_explosion(p, self, self->activator, self->dmg, FIX(self->dmg + 40), NULL);
}

static void         barrel_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    self->takedamage = false;
    self->solid = false;
    self->dead = true;
    self->activator = attacker;
    self->think = barrel_explode;
    self->nextthink = level.time + FIX(0.2);
}

static void         barrel_pain(g_ent *self, g_ent *other, int damage)
{
}

/* ---- spawning ---- */

bool                g_spawn_point(g_ent *e, const q_erec *r)
{
    int             k;

    switch (r->cls)
    {
        case C_TRIGGER_ONCE:
        case C_TRIGGER_MULTIPLE:
            e->kind = EK_TRIGGER;           /* (its box: its brush model's, in g_trig_areas: g_init) */
            for (k = 0; k < 3; ++k)
                e->origin[k] = 0;
            e->wait = r->cls == C_TRIGGER_ONCE ? -1 : r->wait == UNSET ? FIX(0.2) : r->wait;
            e->touch = touch_multi;
            if (r->spawnflags & 4)
            {
                e->inactive = true;
                e->use = trigger_enable;
            }
            else
                e->use = use_multi;
            return true;
        case C_TRIGGER_RELAY:
            e->kind = EK_POINT;
            e->use = use_relay;
            return true;
        case C_TRIGGER_ALWAYS:
            e->kind = EK_POINT;
            if (e->delay < FIX(0.2))
                e->delay = FIX(0.2);
            e->think = think_always;
            e->nextthink = level.time + FIX(0.1);
            return true;
        case C_TRIGGER_COUNTER:
            e->kind = EK_POINT;
            e->count = r->count ? r->count : 2;
            e->use = use_counter;
            return true;
        case C_FUNC_TIMER:
            e->kind = EK_POINT;
            e->wait = r->wait == UNSET ? FIX(1) : r->wait;
            e->random = r->random == UNSET ? 0 : imin(r->random, e->wait - FRAMETIME);
            e->use = use_timer;
            e->think = think_timer;
            if (r->spawnflags & 1)
            {
                e->nextthink = level.time + FIX(1) + e->delay + e->wait + fmul(crandom(), e->random);
                e->activator = g_player;
            }
            return true;
        case C_TARGET_EXPLOSION:
            e->kind = EK_POINT;
            e->use = use_explosion;
            return true;
        case C_TARGET_SPLASH:
            e->kind = EK_POINT;
            e->use = use_splash;
            return true;
        case C_TARGET_SECRET:
            e->kind = EK_POINT;
            e->use = use_secret;
            if (!e->message)
                e->message = "You found a secret area!";
            ++total_secrets;
            return true;
        case C_TARGET_GOAL:
            e->kind = EK_POINT;
            e->use = use_goal;
            ++total_goals;
            return true;
        case C_TARGET_HELP:
            e->kind = EK_POINT;
            e->use = use_help;
            return true;
        case C_TARGET_CHANGELEVEL:
            e->kind = EK_POINT;
            e->use = use_changelevel;
            return true;
        case C_MISC_EXPLOBOX:
            if (!models[MDL_BARREL].loaded)
                return false;
            e->kind = EK_OBJECT;
            e->mdl = &models[MDL_BARREL];
            e->mins[0] = e->mins[1] = -FIX(16);
            e->mins[2] = 0;
            e->maxs[0] = e->maxs[1] = FIX(16);
            e->maxs[2] = FIX(40);
            e->solid = true;
            e->takedamage = true;
            e->health = r->health ? r->health : 10;
            e->dmg = r->dmg ? r->dmg : 150;
            e->die = barrel_die;
            e->pain = barrel_pain;
            M_droptofloor(e);
            return true;
    }
    return false;
}
