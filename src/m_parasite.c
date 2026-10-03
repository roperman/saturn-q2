/*
** The parasite: Quake 2's game/m_parasite.c, with the animations baked into
** PARASITE.MDL (stand, run, pain1, drain, death). It runs at you and, in
** range and level enough, launches its tongue: a beam from its mouth to you
** (fx_beam, drawn as a sprite between the two) that bites, then drains for
** a second while it can still reach you. Left out: its fidgets, walk and
** "break" (Quake never plays the last), its search sound.
*/
#include "game.h"

static void         parasite_stand(g_ent *self);
static void         parasite_run(g_ent *self);
static void         parasite_start_run(g_ent *self);
static void         parasite_dead(g_ent *self);
static void         parasite_tap(g_ent *self);
static void         parasite_launch(g_ent *self);
static void         parasite_reel_in(g_ent *self);
static void         parasite_drain_attack(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define ST              { ai_stand, 0, parasite_tap }
#define C0              { ai_charge, 0, NULL }
#define DR(d)           { ai_charge, d, parasite_drain_attack }
static const mframe_t parasite_frames_stand[] = { S0, S0, ST, S0, ST, S0, S0, S0, ST, S0, ST, S0, S0, S0, ST, S0, ST };
static mmove_t      parasite_move_stand = { "stand", 0, 16, parasite_frames_stand, NULL, 0, 0 };

static const mframe_t parasite_frames_start_run[] = { { ai_run, 0, NULL }, { ai_run, 30, NULL } };
static mmove_t      parasite_move_start_run = { "run", 0, 1, parasite_frames_start_run, parasite_run, 0, 0 };
static const mframe_t parasite_frames_run[] = {
    { ai_run, 30, NULL }, { ai_run, 30, NULL }, { ai_run, 22, NULL }, { ai_run, 19, NULL }, { ai_run, 24, NULL },
    { ai_run, 28, NULL }, { ai_run, 25, NULL },
};
static mmove_t      parasite_move_run = { "run", 2, 8, parasite_frames_run, NULL, 0, 0 };

static const mframe_t parasite_frames_pain1[] = {
    { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL },
    { ai_move, 0, NULL }, { ai_move, 6, NULL }, { ai_move, 16, NULL }, { ai_move, -6, NULL }, { ai_move, -7, NULL },
    { ai_move, 0, NULL },
};
static mmove_t      parasite_move_pain1 = { "pain1", 0, 10, parasite_frames_pain1, parasite_start_run, 0, 0 };

/* the tongue: out, hits on the third frame, drains for ten, back */
static const mframe_t parasite_frames_drain[] = {
    { ai_charge, 0, parasite_launch }, C0, DR(15), DR(0), DR(0), DR(0), DR(0), DR(-2), DR(-2), DR(-3), DR(-2), DR(0),
    DR(-1), { ai_charge, 0, parasite_reel_in }, { ai_charge, -2, NULL }, { ai_charge, -2, NULL },
    { ai_charge, -3, NULL }, C0,
};
static mmove_t      parasite_move_drain = { "drain", 0, 17, parasite_frames_drain, parasite_start_run, 0, 0 };

static const mframe_t parasite_frames_death[] = {
    { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL },
    { ai_move, 0, NULL }, { ai_move, 0, NULL },
};
static mmove_t      parasite_move_death = { "death", 0, 6, parasite_frames_death, parasite_dead, 0, 0 };

static mmove_t      *all_moves[] = {
    &parasite_move_stand, &parasite_move_start_run, &parasite_move_run, &parasite_move_pain1, &parasite_move_drain,
    &parasite_move_death,
};

static void         parasite_tap(g_ent *self)
{
    s_play(SND_PAR_TAP, self->origin, ATTN_IDLE);
}

static void         parasite_launch(g_ent *self)
{
    s_play(SND_PAR_LAUNCH, self->origin, ATTN_NORM);
}

static void         parasite_reel_in(g_ent *self)
{
    s_play(SND_PAR_REELIN, self->origin, ATTN_NORM);
}

static void         parasite_stand(g_ent *self)
{
    self->move = &parasite_move_stand;
}

static void         parasite_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &parasite_move_stand : &parasite_move_run;
}

static void         parasite_start_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &parasite_move_stand : &parasite_move_start_run;
}

/* the tongue reaches: within 256 units, and not more than 30 degrees up or down */
static bool         drain_ok(const s32 *start, const s32 *end)
{
    s32             d[3], h[3];

    d[0] = start[0] - end[0];
    d[1] = start[1] - end[1];
    d[2] = start[2] - end[2];
    if (vlen(d) > FIX(256))
        return false;
    h[0] = d[0];
    h[1] = d[1];
    h[2] = 0;
    return iabs(d[2]) <= fmul(vlen(h), FIX(0.5774));
}

/* a frame of the tongue: from its mouth to you if it can reach (your middle, head or feet),
   and nothing's in the way; the bite's 5, each drain 2 (Quake's parasite_drain_attack) */
static void         parasite_drain_attack(g_ent *self)
{
    static const s32 zero[3];
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw), start[3], end[3];
    const g_ent     *e = self->enemy;
    g_ent           *hit;
    int             f = self->frame - parasite_move_drain.first, damage;

    if (!e)
        return;
    start[0] = self->origin[0] + fmul(fx, FIX(24));
    start[1] = self->origin[1] + fmul(fy, FIX(24));
    start[2] = self->origin[2] + FIX(6);
    end[0] = e->origin[0];
    end[1] = e->origin[1];
    end[2] = e->origin[2];
    if (!drain_ok(start, end))
    {
        end[2] = e->origin[2] + e->maxs[2] - FIX(8);
        if (!drain_ok(start, end))
        {
            end[2] = e->origin[2] + e->mins[2] + FIX(8);
            if (!drain_ok(start, end))
                return;
        }
    }
    end[2] = e->origin[2];
    g_trace(start, zero, zero, end, self, MASK_SHOT, &hit);
    if (hit != e)
        return;
    if (f == 2)
    {
        damage = 5;
        s_play(SND_PAR_IMPACT, end, ATTN_NORM);
    }
    else
    {
        if (f == 3)
            s_play(SND_PAR_SUCK, self->origin, ATTN_NORM);
        damage = 2;
    }
    fx_beam(start, end);
    g_damage(self->enemy, self, damage, end);
}

static void         parasite_attack(g_ent *self)
{
    self->move = &parasite_move_drain;
}

static void         parasite_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    s_play(rng() & 1 ? SND_PAR_PAIN1 : SND_PAR_PAIN2, self->origin, ATTN_NORM);
    self->move = &parasite_move_pain1;
}

static void         parasite_sight(g_ent *self)
{
    s_play(SND_PAR_SIGHT, self->origin, ATTN_NORM);
}

static void         parasite_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);                /* (a body: shot or blown up again, it's gibbed; nothing walks into it) */
    self->move = NULL;
}

static void         parasite_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_PAR_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &parasite_move_death;
}

void                SP_monster_parasite(g_ent *self)
{
    unsigned        i;

    for (i = 0; i < sizeof(all_moves) / sizeof(all_moves[0]); ++i)
    {
        mmove_t *m = all_moves[i];
        int     a = model_anim(self->mdl, m->anim);

        m->first = self->mdl->anims[a].first + m->from;
        m->last = self->mdl->anims[a].first + m->to;
    }
    self->mins[0] = self->mins[1] = -FIX(16);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(16);
    self->maxs[2] = FIX(24);
    self->skinnum = 0;
    self->health = 175;
    self->gib_health = -50;
    self->stand = parasite_stand;
    self->run = parasite_start_run;
    self->attack = parasite_attack;
    self->pain = parasite_pain;
    self->die = parasite_die;
    self->sight = parasite_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_parasite;
#endif
