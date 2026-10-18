/*
** The mutant: Quake 2's game/m_mutant.c, with the animations baked into
** MUTANT.MDL (stand, run, attack, pain1-3, death1-2). It runs at you and
** claws (10 to 14, twice, and again while you're in reach), and from more
** than 100 units away now and then leaps at you (600 units a second forward,
** 250 up: monster_physics flies it until it lands), the leap hurting you for
** 40 to 50 if it comes down on you. Left out: its walk, idle scratching,
** idle and search sounds. (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         mutant_run(g_ent *self);
static void         mutant_dead(g_ent *self);
static void         mutant_step(g_ent *self);
static void         mutant_hit_left(g_ent *self);
static void         mutant_hit_right(g_ent *self);
static void         mutant_check_refire(g_ent *self);
static void         mutant_jump_takeoff(g_ent *self);
static void         mutant_check_landing(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t mutant_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      mutant_move_stand = { "stand", 0, 50, mutant_frames_stand, NULL, 0, 0 };

static const mframe_t mutant_frames_run[] = {
    { ai_run, 40, NULL }, { ai_run, 40, mutant_step }, { ai_run, 24, NULL },
    { ai_run, 5, mutant_step }, { ai_run, 17, NULL }, { ai_run, 10, NULL },
};
static mmove_t      mutant_move_run = { "run", 0, 5, mutant_frames_run, NULL, 0, 0 };

/* "attack" is attack01-attack15: the leap, then the claws */
static const mframe_t mutant_frames_jump[] = {
    C0, { ai_charge, 17, NULL }, { ai_charge, 15, mutant_jump_takeoff }, { ai_charge, 15, NULL },
    { ai_charge, 15, mutant_check_landing }, C0, { ai_charge, 3, NULL }, C0,
};
static mmove_t      mutant_move_jump = { "attack", 0, 7, mutant_frames_jump, mutant_run, 0, 0 };

static const mframe_t mutant_frames_attack[] = {
    C0, C0, { ai_charge, 0, mutant_hit_left }, C0, C0, { ai_charge, 0, mutant_hit_right }, { ai_charge, 0, mutant_check_refire },
};
static mmove_t      mutant_move_attack = { "attack", 8, 14, mutant_frames_attack, mutant_run, 0, 0 };

static const mframe_t mutant_frames_pain1[] = {
    { ai_move, 4, NULL }, { ai_move, -3, NULL }, { ai_move, -8, NULL }, { ai_move, 2, NULL }, { ai_move, 5, NULL },
};
static mmove_t      mutant_move_pain1 = { "pain1", 0, 4, mutant_frames_pain1, mutant_run, 0, 0 };
static const mframe_t mutant_frames_pain2[] = {
    { ai_move, -24, NULL }, { ai_move, 11, NULL }, { ai_move, 5, NULL }, { ai_move, -2, NULL }, { ai_move, 6, NULL },
    { ai_move, 4, NULL },
};
static mmove_t      mutant_move_pain2 = { "pain2", 0, 5, mutant_frames_pain2, mutant_run, 0, 0 };
static const mframe_t mutant_frames_pain3[] = {
    { ai_move, -22, NULL }, { ai_move, 3, NULL }, { ai_move, 3, NULL }, { ai_move, 2, NULL }, { ai_move, 1, NULL },
    { ai_move, 1, NULL }, { ai_move, 6, NULL }, { ai_move, 3, NULL }, { ai_move, 2, NULL }, M0, { ai_move, 1, NULL },
};
static mmove_t      mutant_move_pain3 = { "pain3", 0, 10, mutant_frames_pain3, mutant_run, 0, 0 };

static const mframe_t mutant_frames_death1[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      mutant_move_death1 = { "death1", 0, 8, mutant_frames_death1, mutant_dead, 0, 0 };
static const mframe_t mutant_frames_death2[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      mutant_move_death2 = { "death2", 0, 9, mutant_frames_death2, mutant_dead, 0, 0 };

static mmove_t      *all_moves[] = {
    &mutant_move_stand, &mutant_move_run, &mutant_move_jump, &mutant_move_attack, &mutant_move_pain1,
    &mutant_move_pain2, &mutant_move_pain3, &mutant_move_death1, &mutant_move_death2,
};

static void         mutant_stand(g_ent *self)
{
    self->move = &mutant_move_stand;
}

static void         mutant_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &mutant_move_stand : &mutant_move_run;
}

static void         mutant_step(g_ent *self)
{
    s_play(SND_MUT_STEP, self->origin, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static bool         mutant_hit(g_ent *self)
{
    if (!self->enemy || range(self, self->enemy) != RANGE_MELEE || !infront(self, self->enemy))
        return false;
    g_damage(self->enemy, self, 10 + (int)(rng() % 5), self->enemy->origin);
    return true;
}

static void         mutant_hit_left(g_ent *self)
{
    s_play(mutant_hit(self) ? SND_MUT_HIT1 : SND_MUT_SWING, self->origin, ATTN_NORM);
}

static void         mutant_hit_right(g_ent *self)
{
    s_play(mutant_hit(self) ? SND_MUT_HIT2 : SND_MUT_SWING, self->origin, ATTN_NORM);
}

static void         mutant_check_refire(g_ent *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return;
    if ((g_skill >= 3 && frandom() < FIX(0.5)) || range(self, self->enemy) == RANGE_MELEE)
        self->nextframe = mutant_move_attack.first;
}

/* the leap: forward and up, and monster_physics has it from here (dmg: whether it's hurt you yet) */
static void         mutant_jump_takeoff(g_ent *self)
{
    s_play(SND_MUT_SIGHT, self->origin, ATTN_NORM);
    self->origin[2] += FIX(1);
    self->velocity[0] = fmul(fcos(self->yaw), FIX(600));
    self->velocity[1] = fmul(fsin(self->yaw), FIX(600));
    self->velocity[2] = FIX(250);
    self->on_ground = false;
    self->attack_finished = level.time + FIX(3);
    self->dmg = 0;
}

/* in the air and on you: the blow (Quake's mutant_jump_touch, as it lands on you) */
static void         mutant_jump_hit(g_ent *self)
{
    s32             v[3];

    if (self->dmg || !self->enemy || self->enemy->health <= 0)
        return;
    v[0] = self->enemy->origin[0] - self->origin[0];
    v[1] = self->enemy->origin[1] - self->origin[1];
    v[2] = 0;
    if (vlen(v) < FIX(56) && iabs(self->enemy->origin[2] - self->origin[2]) < FIX(80))
    {
        g_damage(self->enemy, self, 40 + (int)(rng() % 11), self->enemy->origin);
        self->dmg = 1;
    }
}

static void         mutant_check_landing(g_ent *self)
{
    mutant_jump_hit(self);
    if (self->on_ground)
    {
        s_play(SND_MUT_THUD, self->origin, ATTN_NORM);
        self->attack_finished = 0;
        return;
    }
    /* still in the air: this frame again, or after three seconds the leap over */
    self->nextframe = mutant_move_jump.first + (level.time > self->attack_finished ? 1 : 4);
}

/* (the port's AI asks for an attack in reach, and now and then further off: the claws in reach; from
   further, Quake's mutant_check_jump: not from well above or below you, not within 100 units) */
static void         mutant_attack(g_ent *self)
{
    const g_ent     *e = self->enemy;
    s32             v[3], h;

    if (range(self, e) == RANGE_MELEE)
    {
        self->move = &mutant_move_attack;
        return;
    }
    h = e->maxs[2] - e->mins[2];
    if (self->origin[2] + self->mins[2] > e->origin[2] + e->mins[2] + fmul(FIX(0.75), h))
        return;
    if (self->origin[2] + self->maxs[2] < e->origin[2] + e->mins[2] + fmul(FIX(0.25), h))
        return;
    v[0] = self->origin[0] - e->origin[0];
    v[1] = self->origin[1] - e->origin[1];
    v[2] = 0;
    if (vlen(v) < FIX(100) || !self->on_ground)
        return;
    self->move = &mutant_move_jump;
}

static void         mutant_pain(g_ent *self, g_ent *other, int damage)
{
    u32             n;

    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    n = rng() % 3;
    s_play(n == 1 ? SND_MUT_PAIN2 : SND_MUT_PAIN1, self->origin, ATTN_NORM);
    self->move = n == 0 ? &mutant_move_pain1 : n == 1 ? &mutant_move_pain2 : &mutant_move_pain3;
}

static void         mutant_sight(g_ent *self)
{
    s_play(SND_MUT_SIGHT, self->origin, ATTN_NORM);
}

static void         mutant_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);                /* (a body: shot or blown up again, it's gibbed; nothing walks into it) */
    self->move = NULL;
}

static void         mutant_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_MUT_DEATH, self->origin, ATTN_NORM);
    self->skinnum = 1;
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = rng() & 1 ? &mutant_move_death1 : &mutant_move_death2;
}

void                SP_monster_mutant(g_ent *self)
{
    unsigned        i;

    for (i = 0; i < sizeof(all_moves) / sizeof(all_moves[0]); ++i)
    {
        mmove_t *m = all_moves[i];
        int     a = model_anim(self->mdl, m->anim);

        m->first = self->mdl->anims[a].first + m->from;
        m->last = self->mdl->anims[a].first + m->to;
    }
    self->mins[0] = self->mins[1] = -FIX(32);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(32);
    self->maxs[2] = FIX(48);
    self->skinnum = 0;
    self->health = 300;
    self->gib_health = -120;
    self->stand = mutant_stand;
    self->run = mutant_run;
    self->attack = mutant_attack;
    self->pain = mutant_pain;
    self->die = mutant_die;
    self->sight = mutant_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_mutant;
#endif
