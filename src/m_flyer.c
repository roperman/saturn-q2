/*
** The flyer: Quake 2's game/m_flyer.c, with the animations baked into
** FLYER.MDL (stand, which is its run too; melee; attack; pain1-3). It flies
** (FL_FLY: g_ai.c's step leans up or down towards you and never falls),
** shoots blaster bolts in pairs from its two guns and, in reach, pops its
** blades and slashes with each wing. It explodes when it dies, as Quake's.
** Left out: its start, stop, banks, rolls and defence (Quake never plays
** them), its idle hum and search sound.
*/
#include "game.h"

static void         flyer_run(g_ent *self);
static void         flyer_loop_melee(g_ent *self);
static void         flyer_check_melee(g_ent *self);
static void         flyer_pop_blades(g_ent *self);
static void         flyer_fireleft(g_ent *self);
static void         flyer_fireright(g_ent *self);
static void         flyer_slash_left(g_ent *self);
static void         flyer_slash_right(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define R0              { ai_run, 10, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t flyer_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      flyer_move_stand = { "stand", 0, 44, flyer_frames_stand, NULL, 0, 0 };

static const mframe_t flyer_frames_run[] = {
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
};
static mmove_t      flyer_move_run = { "stand", 0, 44, flyer_frames_run, NULL, 0, 0 };

static const mframe_t flyer_frames_pain1[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      flyer_move_pain1 = { "pain1", 0, 8, flyer_frames_pain1, flyer_run, 0, 0 };
static const mframe_t flyer_frames_pain2[] = { M0, M0, M0, M0 };
static mmove_t      flyer_move_pain2 = { "pain2", 0, 3, flyer_frames_pain2, flyer_run, 0, 0 };
static const mframe_t flyer_frames_pain3[] = { M0, M0, M0, M0 };
static mmove_t      flyer_move_pain3 = { "pain3", 0, 3, flyer_frames_pain3, flyer_run, 0, 0 };

/* the guns: four bolts from each, left and right in turn, backing off as it fires */
#define FL              { ai_charge, -10, flyer_fireleft }
#define FR              { ai_charge, -10, flyer_fireright }
static const mframe_t flyer_frames_attack[] = {
    C0, C0, C0, FL, FR, FL, FR, FL, FR, FL, FR, C0, C0, C0, C0, C0, C0,
};
static mmove_t      flyer_move_attack = { "attack", 0, 16, flyer_frames_attack, flyer_run, 0, 0 };

/* the blades: out, then a loop of two slashes while you're in reach, then away */
static const mframe_t flyer_frames_start_melee[] = { { ai_charge, 0, flyer_pop_blades }, C0, C0, C0, C0, C0 };
static mmove_t      flyer_move_start_melee = { "melee", 0, 5, flyer_frames_start_melee, flyer_loop_melee, 0, 0 };
static const mframe_t flyer_frames_loop_melee[] = {
    C0, C0, { ai_charge, 0, flyer_slash_left }, C0, C0, C0, C0, { ai_charge, 0, flyer_slash_right }, C0, C0, C0, C0,
};
static mmove_t      flyer_move_loop_melee = { "melee", 6, 17, flyer_frames_loop_melee, flyer_check_melee, 0, 0 };
static const mframe_t flyer_frames_end_melee[] = { C0, C0, C0 };
static mmove_t      flyer_move_end_melee = { "melee", 18, 20, flyer_frames_end_melee, flyer_run, 0, 0 };

static mmove_t      *all_moves[] = {
    &flyer_move_stand, &flyer_move_run, &flyer_move_pain1, &flyer_move_pain2, &flyer_move_pain3, &flyer_move_attack,
    &flyer_move_start_melee, &flyer_move_loop_melee, &flyer_move_end_melee,
};

static void         flyer_stand(g_ent *self)
{
    self->move = &flyer_move_stand;
}

static void         flyer_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &flyer_move_stand : &flyer_move_run;
}

/* a bolt at you from one of its guns (Quake's monster_flash_offset: forward, right, up) */
static void         flyer_fire(g_ent *self, s32 right)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw), start[3], aim[3], len;
    int             k;

    if (!self->enemy)
        return;
    start[0] = self->origin[0] + fmul(fx, FIX(12.1)) + fmul(fy, right);
    start[1] = self->origin[1] + fmul(fy, FIX(12.1)) - fmul(fx, right);
    start[2] = self->origin[2] - FIX(14.5);
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
    s_play(SND_FLY_BLASTER, start, ATTN_NORM);
}

static void         flyer_fireleft(g_ent *self)
{
    flyer_fire(self, FIX(13.4));
}

static void         flyer_fireright(g_ent *self)
{
    flyer_fire(self, FIX(-7.4));
}

static void         flyer_pop_blades(g_ent *self)
{
    s_play(SND_FLY_BLADES, self->origin, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static void         flyer_slash(g_ent *self)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
        g_damage(self->enemy, self, 5, self->enemy->origin);
    s_play(SND_FLY_SLASH, self->origin, ATTN_NORM);
}

static void         flyer_slash_left(g_ent *self)
{
    flyer_slash(self);
}

static void         flyer_slash_right(g_ent *self)
{
    flyer_slash(self);
}

/* (the port's AI asks for an attack in reach too: the blades there, the guns otherwise) */
static void         flyer_attack(g_ent *self)
{
    self->move = range(self, self->enemy) == RANGE_MELEE ? &flyer_move_start_melee : &flyer_move_attack;
}

static void         flyer_loop_melee(g_ent *self)
{
    self->move = &flyer_move_loop_melee;
}

static void         flyer_check_melee(g_ent *self)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && frandom() <= FIX(0.8))
        self->move = &flyer_move_loop_melee;
    else
        self->move = &flyer_move_end_melee;
}

static void         flyer_pain(g_ent *self, g_ent *other, int damage)
{
    u32             n;

    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    n = rng() % 3;
    s_play(n == 1 ? SND_FLY_PAIN2 : SND_FLY_PAIN1, self->origin, ATTN_NORM);
    self->move = n == 0 ? &flyer_move_pain1 : n == 1 ? &flyer_move_pain2 : &flyer_move_pain3;
}

static void         flyer_sight(g_ent *self)
{
    s_play(SND_FLY_SIGHT, self->origin, ATTN_NORM);
}

/* it blows up (Quake's BecomeExplosion1): a bang, and no body */
static void         flyer_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_FLY_DEATH, self->origin, ATTN_NORM);
    s_play(SND_EXPLOSION, self->origin, ATTN_NORM);
    fx_explosion(self->origin);
    self->solid = false;
    self->takedamage = false;
    self->move = NULL;
    self->kind = EK_FREE;                   /* (its render entity goes: g_render_ents) */
}

void                SP_monster_flyer(g_ent *self)
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
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 50;
    self->gib_health = -1000;               /* (never gibbed: g_damage gibs at or under it; it always explodes) */
    self->flags |= FL_FLY;                  /* (before monster_start: it isn't dropped to the floor) */
    self->stand = flyer_stand;
    self->run = flyer_run;
    self->attack = flyer_attack;
    self->pain = flyer_pain;
    self->die = flyer_die;
    self->sight = flyer_sight;
    monster_start(self);
    self->yaw_speed = ANG(10);              /* (Quake's flymonster_start: half a walker's) */
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_flyer;
#endif
