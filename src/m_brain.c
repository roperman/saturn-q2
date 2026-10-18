/*
** The brains: Quake 2's game/m_brain.c, with the animations baked into
** BRAIN.MDL (stand, run, pain1-3, death1-2, attack1, attack2). It has no
** gun: it runs at you and, in reach, swings its claws twice (15 to 19) or
** opens its chest and strikes with the tentacles (10 to 14), and if those
** land it claws you straight after (as Quake's, above easy). Left out: its
** power screen, duck, idle and search sounds. (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         brain_run(g_ent *self);
static void         brain_dead(g_ent *self);
static void         brain_swing_right(g_ent *self);
static void         brain_hit_right(g_ent *self);
static void         brain_swing_left(g_ent *self);
static void         brain_hit_left(g_ent *self);
static void         brain_chest_open(g_ent *self);
static void         brain_tentacle_attack(g_ent *self);
static void         brain_chest_closed(g_ent *self);

#define SF_CLAW_NEXT    (65536)             /* (the tentacles landed: the claws next, Quake's spawnflag bit) */

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
static const mframe_t brain_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      brain_move_stand = { "stand", 0, 29, brain_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t brain_frames_run[] = {
    { ai_run, 9, NULL }, { ai_run, 2, NULL }, { ai_run, 3, NULL }, { ai_run, 3, NULL }, { ai_run, 1, NULL },
    { ai_run, 0, NULL }, { ai_run, 0, NULL }, { ai_run, 10, NULL }, { ai_run, -4, NULL }, { ai_run, -1, NULL },
    { ai_run, 2, NULL },
};
static mmove_t      brain_move_run = { "run", 0, 10, brain_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t brain_frames_pain1[] = {
    { ai_move, -6, NULL }, { ai_move, -2, NULL }, { ai_move, -6, NULL }, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    { ai_move, 2, NULL }, M0, { ai_move, 2, NULL }, { ai_move, 1, NULL }, { ai_move, 7, NULL }, M0, { ai_move, 3, NULL },
    { ai_move, -1, NULL },
};
static mmove_t      brain_move_pain1 = { "pain1", 0, 20, brain_frames_pain1, brain_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t brain_frames_pain2[] = {
    { ai_move, -2, NULL }, M0, M0, M0, M0, { ai_move, 3, NULL }, { ai_move, 1, NULL }, { ai_move, -2, NULL },
};
static mmove_t      brain_move_pain2 = { "pain2", 0, 7, brain_frames_pain2, brain_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t brain_frames_pain3[] = {
    { ai_move, -2, NULL }, { ai_move, 2, NULL }, { ai_move, 1, NULL }, { ai_move, 3, NULL }, M0, { ai_move, -4, NULL },
};
static mmove_t      brain_move_pain3 = { "pain3", 0, 5, brain_frames_pain3, brain_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t brain_frames_death1[] = {
    M0, M0, { ai_move, -2, NULL }, { ai_move, 9, NULL }, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      brain_move_death1 = { "death1", 0, 17, brain_frames_death1, brain_dead, 0, 0, 0, 0, 0, 0 };
static const mframe_t brain_frames_death2[] = { M0, M0, M0, { ai_move, 9, NULL }, M0 };
static mmove_t      brain_move_death2 = { "death2", 0, 4, brain_frames_death2, brain_dead, 0, 0, 0, 0, 0, 0 };

static const mframe_t brain_frames_attack1[] = {
    { ai_charge, 8, NULL }, { ai_charge, 3, NULL }, { ai_charge, 5, NULL }, { ai_charge, 0, NULL },
    { ai_charge, -3, brain_swing_right }, { ai_charge, 0, NULL }, { ai_charge, -5, NULL }, { ai_charge, -7, brain_hit_right },
    { ai_charge, 0, NULL }, { ai_charge, 6, brain_swing_left }, { ai_charge, 1, NULL }, { ai_charge, 2, brain_hit_left },
    { ai_charge, -3, NULL }, { ai_charge, 6, NULL }, { ai_charge, -1, NULL }, { ai_charge, -3, NULL },
    { ai_charge, 2, NULL }, { ai_charge, -11, NULL },
};
static mmove_t      brain_move_attack1 = { "attack1", 0, 17, brain_frames_attack1, brain_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t brain_frames_attack2[] = {
    { ai_charge, 5, NULL }, { ai_charge, -4, NULL }, { ai_charge, -4, NULL }, { ai_charge, -3, NULL },
    { ai_charge, 0, brain_chest_open }, { ai_charge, 0, NULL }, { ai_charge, 13, brain_tentacle_attack },
    { ai_charge, 0, NULL }, { ai_charge, 2, NULL }, { ai_charge, 0, NULL }, { ai_charge, -9, brain_chest_closed },
    { ai_charge, 0, NULL }, { ai_charge, 4, NULL }, { ai_charge, 3, NULL }, { ai_charge, 2, NULL },
    { ai_charge, -3, NULL }, { ai_charge, -6, NULL },
};
static mmove_t      brain_move_attack2 = { "attack2", 0, 16, brain_frames_attack2, brain_run, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &brain_move_stand, &brain_move_run, &brain_move_pain1, &brain_move_pain2, &brain_move_pain3, &brain_move_death1,
    &brain_move_death2, &brain_move_attack1, &brain_move_attack2,
};

static void         brain_stand(g_ent *self)
{
    self->move = &brain_move_stand;
}

static void         brain_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &brain_move_stand : &brain_move_run;
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static bool         brain_hit(g_ent *self, int damage)
{
    if (!self->enemy || range(self, self->enemy) != RANGE_MELEE || !infront(self, self->enemy))
        return false;
    g_damage(self->enemy, self, damage, self->enemy->origin);
    return true;
}

static void         brain_swing_right(g_ent *self)
{
    s_play(SND_BRN_SWING1, self->origin, ATTN_NORM);
}

static void         brain_hit_right(g_ent *self)
{
    if (brain_hit(self, 15 + (int)(rng() % 5)))
        s_play(SND_BRN_HIT, self->origin, ATTN_NORM);
}

static void         brain_swing_left(g_ent *self)
{
    s_play(SND_BRN_SWING2, self->origin, ATTN_NORM);
}

static void         brain_hit_left(g_ent *self)
{
    if (brain_hit(self, 15 + (int)(rng() % 5)))
        s_play(SND_BRN_HIT, self->origin, ATTN_NORM);
}

static void         brain_chest_open(g_ent *self)
{
    self->spawnflags &= ~SF_CLAW_NEXT;
    s_play(SND_BRN_CHEST, self->origin, ATTN_NORM);
}

static void         brain_tentacle_attack(g_ent *self)
{
    if (brain_hit(self, 10 + (int)(rng() % 5)) && g_skill > 0)
        self->spawnflags |= SF_CLAW_NEXT;
    s_play(SND_BRN_TENTACLES, self->origin, ATTN_NORM);
}

static void         brain_chest_closed(g_ent *self)
{
    if (self->spawnflags & SF_CLAW_NEXT)
    {
        self->spawnflags &= ~SF_CLAW_NEXT;
        self->move = &brain_move_attack1;
    }
}

/* (the port's AI asks for an attack in reach, and now and then further off: the brains only strike in
   reach, and otherwise keep running at you, as Quake's, which has no attack) */
static void         brain_attack(g_ent *self)
{
    if (range(self, self->enemy) != RANGE_MELEE)
        return;
    self->move = rng() & 1 ? &brain_move_attack1 : &brain_move_attack2;
}

static void         brain_pain(g_ent *self, g_ent *other, int damage)
{
    u32             n;

    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    n = rng() % 3;
    s_play(n == 1 ? SND_BRN_PAIN2 : SND_BRN_PAIN1, self->origin, ATTN_NORM);
    self->move = n == 0 ? &brain_move_pain1 : n == 1 ? &brain_move_pain2 : &brain_move_pain3;
}

static void         brain_sight(g_ent *self)
{
    s_play(SND_BRN_SIGHT, self->origin, ATTN_NORM);
}

static void         brain_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);                /* (a body: shot or blown up again, it's gibbed; nothing walks into it) */
    self->move = NULL;
}

static void         brain_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_BRN_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = rng() & 1 ? &brain_move_death1 : &brain_move_death2;
}

void                SP_monster_brain(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(16);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(16);
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 300;
    self->gib_health = -150;
    self->spawnflags &= ~SF_CLAW_NEXT;
    self->stand = brain_stand;
    self->run = brain_run;
    self->attack = brain_attack;
    self->pain = brain_pain;
    self->die = brain_die;
    self->sight = brain_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_brain;
#endif
