/*
** The technician: Quake 2's game/m_float.c, with the animations baked into
** FLOATER.MDL (stand, which is its run too; blaster; claws; zap; pain1-2).
** It flies (FL_FLY, as the flyer), fires seven blaster bolts from a
** distance and, in reach, swings its claws (5 to 10) or zaps you (5 to 10).
** It explodes when it dies, as Quake's. Left out: its second stand, its
** activation and walk, its idle and search sounds.
*/
#include "game.h"

static void         floater_run(g_ent *self);
static void         floater_fire_blaster(g_ent *self);
static void         floater_wham(g_ent *self);
static void         floater_zap(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define R0              { ai_run, 13, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
#define FB              { ai_charge, 0, floater_fire_blaster }
static const mframe_t floater_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      floater_move_stand = { "stand", 0, 51, floater_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t floater_frames_run[] = {
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
};
static mmove_t      floater_move_run = { "stand", 0, 51, floater_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t floater_frames_attack1[] = { C0, C0, C0, FB, FB, FB, FB, FB, FB, FB, C0, C0, C0, C0 };
static mmove_t      floater_move_attack1 = { "blaster", 0, 13, floater_frames_attack1, floater_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t floater_frames_attack2[] = {
    C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, { ai_charge, 0, floater_wham }, C0, C0, C0, C0, C0, C0, C0, C0, C0,
    C0, C0, C0, C0,
};
static mmove_t      floater_move_attack2 = { "claws", 0, 24, floater_frames_attack2, floater_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t floater_frames_attack3[] = {
    C0, C0, C0, C0, C0, C0, C0, C0, { ai_charge, 0, floater_zap }, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0,
    C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0,
};
static mmove_t      floater_move_attack3 = { "zap", 0, 33, floater_frames_attack3, floater_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t floater_frames_pain1[] = { M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      floater_move_pain1 = { "pain1", 0, 6, floater_frames_pain1, floater_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t floater_frames_pain2[] = { M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      floater_move_pain2 = { "pain2", 0, 7, floater_frames_pain2, floater_run, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &floater_move_stand, &floater_move_run, &floater_move_attack1, &floater_move_attack2, &floater_move_attack3,
    &floater_move_pain1, &floater_move_pain2,
};

static void         floater_stand(g_ent *self)
{
    self->move = &floater_move_stand;
}

static void         floater_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &floater_move_stand : &floater_move_run;
}

/* a bolt at you from its gun (Quake's MZ2_FLOAT_BLASTER_1: forward, right, up) */
static void         floater_fire_blaster(g_ent *self)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw), start[3], aim[3], len;
    int             k;

    if (!self->enemy)
        return;
    start[0] = self->origin[0] + fmul(fx, FIX(32.5)) + fmul(fy, FIX(-0.8));
    start[1] = self->origin[1] + fmul(fy, FIX(32.5)) - fmul(fx, FIX(-0.8));
    start[2] = self->origin[2] + FIX(10);
    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    g_fire_blaster(self, start, aim, 1, FIX(1000));
    g_muzzle_flash(start, 12, 10, 3);
    s_play(SND_FLT_BLASTER, start, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static void         floater_wham(g_ent *self)
{
    s_play(SND_FLT_WHAM, self->origin, ATTN_NORM);
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
        g_damage(self->enemy, self, 5 + (int)(rng() % 6), self->enemy->origin);
}

/* the zap (Quake's hits whatever the range; here, in reach, where the attack was chosen) */
static void         floater_zap(g_ent *self)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw), p[3];

    p[0] = self->origin[0] + fmul(fx, FIX(18.5)) + fmul(fy, FIX(-0.9));
    p[1] = self->origin[1] + fmul(fy, FIX(18.5)) - fmul(fx, FIX(-0.9));
    p[2] = self->origin[2] + FIX(10);
    s_play(SND_FLT_ZAP, self->origin, ATTN_NORM);
    fx_spark(p);
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE)
        g_damage(self->enemy, self, 5 + (int)(rng() % 6), self->enemy->origin);
}

/* (the port's AI asks for an attack in reach too: the claws or the zap there, the bolts otherwise) */
static void         floater_attack(g_ent *self)
{
    if (range(self, self->enemy) == RANGE_MELEE)
        self->move = rng() & 1 ? &floater_move_attack3 : &floater_move_attack2;
    else
        self->move = &floater_move_attack1;
}

static void         floater_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    if (rng() % 3 == 0)
    {
        s_play(SND_FLT_PAIN1, self->origin, ATTN_NORM);
        self->move = &floater_move_pain1;
    }
    else
    {
        s_play(SND_FLT_PAIN2, self->origin, ATTN_NORM);
        self->move = &floater_move_pain2;
    }
}

static void         floater_sight(g_ent *self)
{
    s_play(SND_FLT_SIGHT, self->origin, ATTN_NORM);
}

/* it blows up (Quake's BecomeExplosion1): a bang, and no body */
static void         floater_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_FLT_DEATH, self->origin, ATTN_NORM);
    s_play(SND_EXPLOSION, self->origin, ATTN_NORM);
    fx_explosion(self->origin);
    self->solid = false;
    self->takedamage = false;
    self->move = NULL;
    self->kind = EK_FREE;                   /* (its render entity goes: g_render_ents) */
}

void                SP_monster_floater(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(24);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(24);
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 200;
    self->gib_health = -1000;               /* (never gibbed: it always explodes) */
    self->flags |= FL_FLY;                  /* (before monster_start: it isn't dropped to the floor) */
    self->stand = floater_stand;
    self->run = floater_run;
    self->attack = floater_attack;
    self->pain = floater_pain;
    self->die = floater_die;
    self->sight = floater_sight;
    monster_start(self);
    self->yaw_speed = ANG(10);              /* (Quake's flymonster_start: half a walker's) */
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_floater;
#endif
