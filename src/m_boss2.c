/*
** The hornet (boss2, Big Gun's boss, also Hangar 2 and the Comm Satellite):
** Quake 2's game/m_boss2.c, with the animations baked into BOSS2.MDL (stand,
** walk, attack, pain, death; its mesh collapsed to the renderer's limits,
** tools/models.txt "mesh="). It flies (FL_FLY, as the icarus), has 2000
** health, and either rakes you with both machineguns (again while you're
** in front, seven times in ten) or, from past 125 units, fires four rockets
** at once (four in ten). Hurt below half, its skin's the damaged one. Dying,
** it plays its fall and then blows up eight times over its body before it
** goes (Quake's BossExplode; the gibs it throws left out). Left out too:
** its fidget, its walk along a path, its engine hum and its search call.
*/
#include "game.h"

static void         boss2_run(g_ent *self);
static void         boss2_attack_mg(g_ent *self);
static void         boss2_reattack_mg(g_ent *self);
static void         boss2_machinegun(g_ent *self);
static void         boss2_rocket(g_ent *self);
static void         boss2_dead(g_ent *self);
static void         boss2_explode(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define R8              { ai_run, 8, NULL }
#define M0              { ai_move, 0, NULL }
#define C1              { ai_charge, 1, NULL }
static const mframe_t boss2_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      boss2_move_stand = { "stand", 0, 20, boss2_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t boss2_frames_run[] = {
    R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8, R8,
};
static mmove_t      boss2_move_run = { "walk", 0, 19, boss2_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

/* "attack" is attack1-attack40: the guns up (1-9), the burst (10-15, looped while it's on you),
   the guns down (16-19); or the rockets (20-40, the four at 32) */
static const mframe_t boss2_frames_attack_pre_mg[] = { C1, C1, C1, C1, C1, C1, C1, C1, C1 };
static mmove_t      boss2_move_attack_pre_mg = { "attack", 0, 8, boss2_frames_attack_pre_mg, boss2_attack_mg,
                                                 0, 0, 0, 0, 0, 0 };
static const mframe_t boss2_frames_attack_mg[] = {
    { ai_charge, 1, boss2_machinegun }, { ai_charge, 1, boss2_machinegun }, { ai_charge, 1, boss2_machinegun },
    { ai_charge, 1, boss2_machinegun }, { ai_charge, 1, boss2_machinegun }, { ai_charge, 1, boss2_reattack_mg },
};
static mmove_t      boss2_move_attack_mg = { "attack", 9, 14, boss2_frames_attack_mg, NULL, 0, 0, 0, 0, 0, 0 };
static const mframe_t boss2_frames_attack_post_mg[] = { C1, C1, C1, C1 };
static mmove_t      boss2_move_attack_post_mg = { "attack", 15, 18, boss2_frames_attack_post_mg, boss2_run,
                                                  0, 0, 0, 0, 0, 0 };
static const mframe_t boss2_frames_attack_rocket[] = {
    C1, C1, C1, C1, C1, C1, C1, C1, C1, C1, C1, C1, { ai_move, -20, boss2_rocket }, C1, C1, C1, C1, C1, C1, C1, C1,
};
static mmove_t      boss2_move_attack_rocket = { "attack", 19, 39, boss2_frames_attack_rocket, boss2_run,
                                                 0, 0, 0, 0, 0, 0 };

/* "pain" is pain2-pain23: the heavy one 2-19, the light 20-23 */
static const mframe_t boss2_frames_pain_heavy[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      boss2_move_pain_heavy = { "pain", 0, 17, boss2_frames_pain_heavy, boss2_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t boss2_frames_pain_light[] = { M0, M0, M0, M0 };
static mmove_t      boss2_move_pain_light = { "pain", 18, 21, boss2_frames_pain_light, boss2_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t boss2_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      boss2_move_death = { "death", 0, 48, boss2_frames_death, boss2_dead, 0, 0, 0, 0, 0, 0 };
/* (down: the last frame held, a bang a tick over it, g_ent count's) */
static const mframe_t boss2_frames_explode[] = { { ai_move, 0, boss2_explode } };
static mmove_t      boss2_move_explode = { "death", 48, 48, boss2_frames_explode, NULL, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &boss2_move_stand, &boss2_move_run, &boss2_move_attack_pre_mg, &boss2_move_attack_mg,
    &boss2_move_attack_post_mg, &boss2_move_attack_rocket, &boss2_move_pain_heavy, &boss2_move_pain_light,
    &boss2_move_death, &boss2_move_explode,
};

static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

/* the way from start to you (a little ahead of where you're going, lead), unit length */
static bool         aim_at(const g_ent *self, const s32 *start, s32 lead, s32 *aim)
{
    s32             len;
    int             k;

    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] + fmul(self->enemy->velocity[k], lead) - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return false;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    return true;
}

static void         boss2_stand(g_ent *self)
{
    self->move = &boss2_move_stand;
}

static void         boss2_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &boss2_move_stand : &boss2_move_run;
}

/* four rockets at once from under its nose (Quake's MZ2_BOSS2_ROCKET_1-4: forward, right, up),
   50 of damage each */
static void         boss2_rocket(g_ent *self)
{
    static const s32 ofs[4][3] = {
        { FIX(22), FIX(16), FIX(10) }, { FIX(22), FIX(8), FIX(10) }, { FIX(22), FIX(-8), FIX(10) },
        { FIX(22), FIX(-16), FIX(10) },
    };
    int             n;

    if (!self->enemy)
        return;
    for (n = 0; n < 4; ++n)
    {
        s32         start[3], aim[3];

        project(self, ofs[n], start);
        if (!aim_at(self, start, 0, aim))
            return;
        fx_rocket(self, start, aim, 50, 50);
        g_muzzle_flash(start, 13, 10, 4);
    }
    s_play(SND_BH_ROCKET, self->origin, ATTN_NORM);
}

/* a bullet from each gun (Quake's MZ2_BOSS2_MACHINEGUN_L1 and R1), 6 of damage, at where
   you'll be in a fifth of a second */
static void         boss2_machinegun(g_ent *self)
{
    static const s32 ofs[2][3] = { { FIX(32), FIX(-40), FIX(70) }, { FIX(32), FIX(40), FIX(70) } };
    int             n;

    if (!self->enemy)
        return;
    for (n = 0; n < 2; ++n)
    {
        s32         start[3], aim[3];

        project(self, ofs[n], start);
        if (!aim_at(self, start, FIX(-0.2), aim))
            return;
        g_fire_hitscan(self, start, aim, 6, 300, 500, 1);
        g_muzzle_flash(start, 13, 11, 5);
    }
    s_play(SND_BH_MG, self->origin, ATTN_NORM);
}

static void         boss2_attack(g_ent *self)
{
    s32             v[3];
    int             k;

    if (!self->enemy)
        return;
    for (k = 0; k < 3; ++k)
        v[k] = self->enemy->origin[k] - self->origin[k];
    if (vlen(v) <= FIX(125) || frandom() <= FIX(0.6))
        self->move = &boss2_move_attack_pre_mg;
    else
        self->move = &boss2_move_attack_rocket;
}

static void         boss2_attack_mg(g_ent *self)
{
    self->move = &boss2_move_attack_mg;
}

static void         boss2_reattack_mg(g_ent *self)
{
    if (self->enemy && infront(self, self->enemy) && frandom() <= FIX(0.7))
        self->move = &boss2_move_attack_mg;
    else
        self->move = &boss2_move_attack_post_mg;
}

/* (Quake's: its cries at no attenuation, heard over the whole level) */
static void         boss2_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    if (damage < 10)
    {
        s_play(SND_BH_PAIN3, self->origin, ATTN_NONE);
        self->move = &boss2_move_pain_light;
    }
    else if (damage < 30)
    {
        s_play(SND_BH_PAIN1, self->origin, ATTN_NONE);
        self->move = &boss2_move_pain_light;
    }
    else
    {
        s_play(SND_BH_PAIN2, self->origin, ATTN_NONE);
        self->move = &boss2_move_pain_heavy;
    }
}

/* its fall done: a bang a tick at eight spots about it, and on the eighth it's gone
   (Quake's BossExplode, without the gibs) */
static void         boss2_explode(g_ent *self)
{
    static const s8 spot[8][2] = {
        { -24, -24 }, { 24, 24 }, { 24, -24 }, { -24, 24 }, { -48, -48 }, { 48, 48 }, { -48, 48 }, { 48, -48 },
    };
    s32             p[3];
    int             n = self->count;

    if (n >= 8)
    {
        self->solid = false;
        self->takedamage = false;
        self->move = NULL;
        self->kind = EK_FREE;               /* (its render entity goes: g_render_ents) */
        return;
    }
    self->count = n + 1;
    p[0] = self->origin[0] + FIX(spot[n][0]);
    p[1] = self->origin[1] + FIX(spot[n][1]);
    p[2] = self->origin[2] + FIX(24 + (rng() & 15));
    s_play(SND_EXPLOSION, p, ATTN_NORM);
    fx_explosion(p);
}

static void         boss2_dead(g_ent *self)
{
    self->count = 0;
    self->move = &boss2_move_explode;
}

static void         boss2_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->takedamage = false;               /* (Quake's: it can't be gibbed on its way down) */
    s_play(SND_BH_DEATH, self->origin, ATTN_NONE);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &boss2_move_death;
}

void                SP_monster_boss2(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(56);
    self->mins[2] = 0;
    self->maxs[0] = self->maxs[1] = FIX(56);
    self->maxs[2] = FIX(80);
    self->skinnum = 0;
    self->health = 2000;
    self->gib_health = -200;
    self->stand = boss2_stand;
    self->run = boss2_run;
    self->attack = boss2_attack;
    self->pain = boss2_pain;
    self->die = boss2_die;
    self->flags |= FL_FLY;                  /* (before monster_start: it isn't dropped to the floor) */
    monster_start(self);
    self->yaw_speed = ANG(10);              /* (Quake's flymonster_start: half a walker's) */
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_boss2;
#endif
