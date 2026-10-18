/*
** The supertank (boss1: Power Plant's, and Security's trigger-spawned one):
** Quake 2's game/m_supertank.c, with the animations baked into SUPERTAN.MDL
** (stand, run, attack1, attack2, pain1-3, death; its mesh collapsed to the
** renderer's limits, tools/models.txt "mesh="). 1500 health. Within 160
** units, or three times in ten beyond, its chaingun (six bullets a burst,
** 6 of damage each, again while it sees you nine times in ten); else three
** rockets one after another (50 each). Hurt below half, its skin's the
** damaged one; a light hit mostly doesn't stop it. Dying, it goes down over
** 24 frames and then blows up eight times over its body before it goes
** (Quake's BossExplode, without the gibs). Left out: its walk along a path,
** its turns, its backing up, two attacks Quake never uses, its search calls.
*/
#include "game.h"

static void         supertank_run(g_ent *self);
static void         supertank_reattack1(g_ent *self);
static void         supertank_machinegun(g_ent *self);
static void         supertank_rocket(g_ent *self);
static void         supertank_tread(g_ent *self);
static void         supertank_dead(g_ent *self);
static void         supertank_explode(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define R12             { ai_run, 12, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t supertank_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      supertank_move_stand = { "stand", 0, 59, supertank_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t supertank_frames_run[] = {
    { ai_run, 12, supertank_tread }, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12, R12,
};
static mmove_t      supertank_move_run = { "run", 0, 17, supertank_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

/* "attack1" is attaka1-attaka20: the chaingun's six (looped while it's on you), then the gun down */
static const mframe_t supertank_frames_attack1[] = {
    { ai_charge, 0, supertank_machinegun }, { ai_charge, 0, supertank_machinegun }, { ai_charge, 0, supertank_machinegun },
    { ai_charge, 0, supertank_machinegun }, { ai_charge, 0, supertank_machinegun }, { ai_charge, 0, supertank_machinegun },
};
static mmove_t      supertank_move_attack1 = { "attack1", 0, 5, supertank_frames_attack1, supertank_reattack1,
                                               0, 0, 0, 0, 0, 0 };
static const mframe_t supertank_frames_end_attack1[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      supertank_move_end_attack1 = { "attack1", 6, 19, supertank_frames_end_attack1, supertank_run,
                                                   0, 0, 0, 0, 0, 0 };

/* "attack2" is attakb1-attakb27: the rockets at the 8th, 11th and 14th */
static const mframe_t supertank_frames_attack2[] = {
    C0, C0, C0, C0, C0, C0, C0, { ai_charge, 0, supertank_rocket }, M0, M0, { ai_move, 0, supertank_rocket }, M0, M0,
    { ai_move, 0, supertank_rocket }, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      supertank_move_attack2 = { "attack2", 0, 26, supertank_frames_attack2, supertank_run,
                                               0, 0, 0, 0, 0, 0 };

static const mframe_t supertank_frames_pain[] = { M0, M0, M0, M0 };
static mmove_t      supertank_move_pain1 = { "pain1", 0, 3, supertank_frames_pain, supertank_run, 0, 0, 0, 0, 0, 0 };
static mmove_t      supertank_move_pain2 = { "pain2", 0, 3, supertank_frames_pain, supertank_run, 0, 0, 0, 0, 0, 0 };
static mmove_t      supertank_move_pain3 = { "pain3", 0, 3, supertank_frames_pain, supertank_run, 0, 0, 0, 0, 0, 0 };

/* "death" is death1-death47 (30 frames, gaps in the names): Quake plays the first 24 */
static const mframe_t supertank_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      supertank_move_death = { "death", 0, 23, supertank_frames_death, supertank_dead, 0, 0, 0, 0, 0, 0 };
/* (down: the last frame held, a bang a tick over it, g_ent count's) */
static const mframe_t supertank_frames_explode[] = { { ai_move, 0, supertank_explode } };
static mmove_t      supertank_move_explode = { "death", 23, 23, supertank_frames_explode, NULL, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &supertank_move_stand, &supertank_move_run, &supertank_move_attack1, &supertank_move_end_attack1,
    &supertank_move_attack2, &supertank_move_pain1, &supertank_move_pain2, &supertank_move_pain3,
    &supertank_move_death, &supertank_move_explode,
};

static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

/* the way from start to you, unit length */
static bool         aim_at(const g_ent *self, const s32 *start, s32 *aim)
{
    s32             len;
    int             k;

    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return false;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    return true;
}

static void         supertank_stand(g_ent *self)
{
    self->move = &supertank_move_stand;
}

static void         supertank_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &supertank_move_stand : &supertank_move_run;
}

static void         supertank_tread(g_ent *self)
{
    s_play(SND_BT_TREAD, self->origin, ATTN_NORM);
}

/* a rocket from its launcher (Quake's MZ2_SUPERTANK_ROCKET_1-3 by the frame: forward, right, up) */
static void         supertank_rocket(g_ent *self)
{
    static const s32 ofs[3][3] = {
        { FIX(16), FIX(-22.5), FIX(91.2) }, { FIX(16), FIX(-33.4), FIX(86.7) }, { FIX(16), FIX(-42.8), FIX(83.3) },
    };
    int             n = self->frame - supertank_move_attack2.first;
    s32             start[3], aim[3];

    if (!self->enemy)
        return;
    project(self, ofs[n <= 7 ? 0 : n <= 10 ? 1 : 2], start);
    if (!aim_at(self, start, aim))
        return;
    fx_rocket(self, start, aim, 50, 50);
    g_muzzle_flash(start, 13, 10, 4);
    s_play(SND_BT_ROCKET, start, ATTN_NORM);
}

/* a bullet a frame from its chaingun (Quake's MZ2_SUPERTANK_MACHINEGUN_1: the same spot each), 6 of damage */
static void         supertank_machinegun(g_ent *self)
{
    static const s32 ofs[3] = { FIX(30), FIX(30), FIX(88.5) };
    s32             start[3], aim[3];

    if (!self->enemy)
        return;
    project(self, ofs, start);
    if (!aim_at(self, start, aim))
        return;
    g_fire_hitscan(self, start, aim, 6, 300, 500, 1);
    g_muzzle_flash(start, 13, 11, 5);
    s_play(SND_BT_MG, start, ATTN_NORM);
}

static void         supertank_attack(g_ent *self)
{
    s32             v[3];
    int             k;

    if (!self->enemy)
        return;
    for (k = 0; k < 3; ++k)
        v[k] = self->enemy->origin[k] - self->origin[k];
    if (vlen(v) <= FIX(160) || frandom() < FIX(0.3))
        self->move = &supertank_move_attack1;
    else
        self->move = &supertank_move_attack2;   /* (rockets more often from a distance) */
}

static void         supertank_reattack1(g_ent *self)
{
    if (self->enemy && visible(self, self->enemy) && frandom() < FIX(0.9))
        self->move = &supertank_move_attack1;
    else
        self->move = &supertank_move_end_attack1;
}

static void         supertank_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    if (damage <= 25 && frandom() < FIX(0.2))
        return;                             /* (Quake's: a light hit seldom stops it) */
    self->pain_debounce = level.time + FIX(3);
    if (damage <= 10)
    {
        s_play(SND_BT_PAIN1, self->origin, ATTN_NORM);
        self->move = &supertank_move_pain1;
    }
    else if (damage <= 25)
    {
        s_play(SND_BT_PAIN3, self->origin, ATTN_NORM);
        self->move = &supertank_move_pain2;
    }
    else
    {
        s_play(SND_BT_PAIN2, self->origin, ATTN_NORM);
        self->move = &supertank_move_pain3;
    }
}

/* down: a bang a tick at eight spots about it, and on the eighth it's gone (Quake's BossExplode,
   without the gibs) */
static void         supertank_explode(g_ent *self)
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

static void         supertank_dead(g_ent *self)
{
    self->mins[0] = self->mins[1] = -FIX(60);
    self->maxs[0] = self->maxs[1] = FIX(60);
    self->maxs[2] = FIX(72);
    self->count = 0;
    self->move = &supertank_move_explode;
}

static void         supertank_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->takedamage = false;               /* (Quake's: it can't be gibbed on its way down) */
    s_play(SND_BT_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &supertank_move_death;
}

void                SP_monster_supertank(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(64);
    self->mins[2] = 0;
    self->maxs[0] = self->maxs[1] = FIX(64);
    self->maxs[2] = FIX(112);
    self->skinnum = 0;
    self->health = 1500;
    self->gib_health = -500;
    self->stand = supertank_stand;
    self->run = supertank_run;
    self->attack = supertank_attack;
    self->pain = supertank_pain;
    self->die = supertank_die;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_supertank;
#endif
