/*
** The icarus: Quake 2's game/m_hover.c, with the animations baked into
** HOVER.MDL (stand, run, pain1-3, death, attack). It flies (FL_FLY, as the
** flyer) and fires blaster bolts in pairs, again while it can see you (six
** in ten). Dying, it plays its fall and then blows up (Quake's drops to the
** floor first). Left out: its stop, take-off, land, backward and walk,
** its idle hum and search sounds.
*/
#include "game.h"

static void         hover_run(g_ent *self);
static void         hover_dead(g_ent *self);
static void         hover_attack(g_ent *self);
static void         hover_reattack(g_ent *self);
static void         hover_fire_blaster(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define R0              { ai_run, 10, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t hover_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      hover_move_stand = { "stand", 0, 29, hover_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t hover_frames_run[] = {
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
    R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0, R0,
};
static mmove_t      hover_move_run = { "run", 0, 34, hover_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t hover_frames_pain1[] = {
    M0, M0, { ai_move, 2, NULL }, { ai_move, -8, NULL }, { ai_move, -4, NULL }, { ai_move, -6, NULL },
    { ai_move, -4, NULL }, { ai_move, -3, NULL }, { ai_move, 1, NULL }, M0, M0, M0, { ai_move, 3, NULL },
    { ai_move, 1, NULL }, M0, { ai_move, 2, NULL }, { ai_move, 3, NULL }, { ai_move, 2, NULL }, { ai_move, 7, NULL },
    { ai_move, 1, NULL }, M0, M0, { ai_move, 2, NULL }, M0, M0, { ai_move, 5, NULL }, { ai_move, 3, NULL },
    { ai_move, 4, NULL },
};
static mmove_t      hover_move_pain1 = { "pain1", 0, 27, hover_frames_pain1, hover_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t hover_frames_pain2[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      hover_move_pain2 = { "pain2", 0, 11, hover_frames_pain2, hover_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t hover_frames_pain3[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      hover_move_pain3 = { "pain3", 0, 8, hover_frames_pain3, hover_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t hover_frames_death1[] = {
    M0, M0, M0, M0, M0, M0, { ai_move, -10, NULL }, { ai_move, 3, NULL }, { ai_move, 5, NULL }, { ai_move, 4, NULL },
    { ai_move, 7, NULL },
};
static mmove_t      hover_move_death1 = { "death", 0, 10, hover_frames_death1, hover_dead, 0, 0, 0, 0, 0, 0 };

/* "attack" is attak101-attak108: the guns up, two bolts backing off (again while it sees you), the guns down */
static const mframe_t hover_frames_start_attack[] = { { ai_charge, 1, NULL }, { ai_charge, 1, NULL }, { ai_charge, 1, NULL } };
static mmove_t      hover_move_start_attack = { "attack", 0, 2, hover_frames_start_attack, hover_attack, 0, 0, 0, 0, 0, 0 };
static const mframe_t hover_frames_attack1[] = {
    { ai_charge, -10, hover_fire_blaster }, { ai_charge, -10, hover_fire_blaster }, { ai_charge, 0, hover_reattack },
};
static mmove_t      hover_move_attack1 = { "attack", 3, 5, hover_frames_attack1, NULL, 0, 0, 0, 0, 0, 0 };
static const mframe_t hover_frames_end_attack[] = { { ai_charge, 1, NULL }, { ai_charge, 1, NULL } };
static mmove_t      hover_move_end_attack = { "attack", 6, 7, hover_frames_end_attack, hover_run, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &hover_move_stand, &hover_move_run, &hover_move_pain1, &hover_move_pain2, &hover_move_pain3, &hover_move_death1,
    &hover_move_start_attack, &hover_move_attack1, &hover_move_end_attack,
};

static void         hover_stand(g_ent *self)
{
    self->move = &hover_move_stand;
}

static void         hover_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &hover_move_stand : &hover_move_run;
}

/* a bolt at you from its gun (Quake's MZ2_HOVER_BLASTER_1: forward, right, up) */
static void         hover_fire_blaster(g_ent *self)
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
    s_play(SND_HOV_BLASTER, start, ATTN_NORM);
}

/* (the port's AI asks for an attack in reach too: Quake's icarus has no melee, so the guns there as well) */
static void         hover_start_attack(g_ent *self)
{
    self->move = &hover_move_start_attack;
}

static void         hover_attack(g_ent *self)
{
    self->move = &hover_move_attack1;
}

static void         hover_reattack(g_ent *self)
{
    if (self->enemy && self->enemy->health > 0 && visible(self, self->enemy) && frandom() <= FIX(0.6))
        self->move = &hover_move_attack1;
    else
        self->move = &hover_move_end_attack;
}

static void         hover_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    if (damage <= 25)
    {
        if (rng() & 1)
        {
            s_play(SND_HOV_PAIN1, self->origin, ATTN_NORM);
            self->move = &hover_move_pain3;
        }
        else
        {
            s_play(SND_HOV_PAIN2, self->origin, ATTN_NORM);
            self->move = &hover_move_pain2;
        }
    }
    else
    {
        s_play(SND_HOV_PAIN1, self->origin, ATTN_NORM);
        self->move = &hover_move_pain1;
    }
}

static void         hover_sight(g_ent *self)
{
    s_play(SND_HOV_SIGHT, self->origin, ATTN_NORM);
}

/* its fall done: it blows up (Quake's BecomeExplosion1): a bang, and no body */
static void         hover_dead(g_ent *self)
{
    s_play(SND_EXPLOSION, self->origin, ATTN_NORM);
    fx_explosion(self->origin);
    self->solid = false;
    self->takedamage = false;
    self->move = NULL;
    self->kind = EK_FREE;                   /* (its render entity goes: g_render_ents) */
}

static void         hover_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(rng() & 1 ? SND_HOV_DEATH1 : SND_HOV_DEATH2, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &hover_move_death1;
}

void                SP_monster_hover(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(24);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(24);
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 240;
    self->gib_health = -100;
    self->flags |= FL_FLY;                  /* (before monster_start: it isn't dropped to the floor) */
    self->stand = hover_stand;
    self->run = hover_run;
    self->attack = hover_start_attack;
    self->pain = hover_pain;
    self->die = hover_die;
    self->sight = hover_sight;
    monster_start(self);
    self->yaw_speed = ANG(10);              /* (Quake's flymonster_start: half a walker's) */
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_hover;
#endif
