/*
** The gladiator: Quake 2's game/m_gladiator.c, with the animations baked into
** GLADIATR.MDL (stand, run, melee, attack, pain, painup, death). Its railgun
** is aimed at where you are as it starts to charge, so the half second
** before the shot is the sidestep Quake gives you; the shot's a hitscan for
** 50, drawn as a beam for a moment (fx_beam, the parasite's). In reach it
** swings the cleaver (20 to 24) twice. Left out: its walk, idle and search
** sounds. (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         gladiator_run(g_ent *self);
static void         gladiator_dead(g_ent *self);
static void         gladiator_cleaver_swing(g_ent *self);
static void         GladiatorMelee(g_ent *self);
static void         GladiatorGun(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t gladiator_frames_stand[] = { S0, S0, S0, S0, S0, S0, S0 };
static mmove_t      gladiator_move_stand = { "stand", 0, 6, gladiator_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_run[] = {
    { ai_run, 23, NULL }, { ai_run, 14, NULL }, { ai_run, 14, NULL },
    { ai_run, 21, NULL }, { ai_run, 12, NULL }, { ai_run, 13, NULL },
};
static mmove_t      gladiator_move_run = { "run", 0, 5, gladiator_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_attack_melee[] = {
    C0, C0, C0, C0, { ai_charge, 0, gladiator_cleaver_swing }, C0, { ai_charge, 0, GladiatorMelee }, C0, C0, C0,
    { ai_charge, 0, gladiator_cleaver_swing }, C0, C0, { ai_charge, 0, GladiatorMelee }, C0, C0, C0,
};
static mmove_t      gladiator_move_attack_melee = { "melee", 0, 16, gladiator_frames_attack_melee, gladiator_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_attack_gun[] = { C0, C0, C0, { ai_charge, 0, GladiatorGun }, C0, C0, C0, C0, C0 };
static mmove_t      gladiator_move_attack_gun = { "attack", 0, 8, gladiator_frames_attack_gun, gladiator_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_pain[] = { M0, M0, M0, M0, M0, M0 };
static mmove_t      gladiator_move_pain = { "pain", 0, 5, gladiator_frames_pain, gladiator_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_pain_air[] = { M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      gladiator_move_pain_air = { "painup", 0, 6, gladiator_frames_pain_air, gladiator_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t gladiator_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      gladiator_move_death = { "death", 0, 21, gladiator_frames_death, gladiator_dead, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &gladiator_move_stand, &gladiator_move_run, &gladiator_move_attack_melee, &gladiator_move_attack_gun,
    &gladiator_move_pain, &gladiator_move_pain_air, &gladiator_move_death,
};

static void         gladiator_stand(g_ent *self)
{
    self->move = &gladiator_move_stand;
}

static void         gladiator_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &gladiator_move_stand : &gladiator_move_run;
}

static void         gladiator_cleaver_swing(g_ent *self)
{
    s_play(SND_GLD_SWING, self->origin, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static void         GladiatorMelee(g_ent *self)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
    {
        g_damage(self->enemy, self, 20 + (int)(rng() % 5), self->enemy->origin);
        s_play(SND_GLD_HIT, self->origin, ATTN_NORM);
    }
    else
        s_play(SND_GLD_MISS, self->origin, ATTN_NORM);
}

/* where a shot starts: the gladiator's origin, so far forward, right and up */
static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

/* the rail: at where you were when it started charging (Quake's pos1), a hitscan for 50, drawn */
static void         GladiatorGun(g_ent *self)
{
    static const s32 ofs[3] = { FIX(30.0), FIX(18.0), FIX(28.0) };     /* (Quake's MZ2_GLADIATOR_RAILGUN_1) */
    static const s32 zero[3] = { 0, 0, 0 };
    s32             start[3], dir[3], end[3], len;
    q_trace         t;
    g_ent           *hit;
    int             k;

    project(self, ofs, start);
    for (k = 0; k < 3; ++k)
        dir[k] = self->aim[k] - start[k];
    len = vlen(dir);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
    {
        dir[k] = fdiv(dir[k], len);
        end[k] = start[k] + fmul(dir[k], FIX(4096));
    }
    t = g_trace(start, zero, zero, end, self, MASK_SHOT, &hit);
    if (hit && hit->takedamage)
        g_damage(hit, self, 50, t.endpos);
    fx_beam(start, t.endpos);
    g_muzzle_flash(start, 6, 8, 15);
}

/* (the port's AI asks for an attack in reach, and now and then further off: the cleaver in reach, the
   rail from beyond a small safe zone, as Quake's) */
static void         gladiator_attack(g_ent *self)
{
    s32             v[3];
    int             k;

    if (range(self, self->enemy) == RANGE_MELEE)
    {
        self->move = &gladiator_move_attack_melee;
        return;
    }
    for (k = 0; k < 3; ++k)
        v[k] = self->origin[k] - self->enemy->origin[k];
    if (vlen(v) <= MELEE_DISTANCE + FIX(32))
        return;
    s_play(SND_GLD_GUN, self->origin, ATTN_NORM);
    for (k = 0; k < 3; ++k)
        self->aim[k] = self->enemy->origin[k];
    self->aim[2] += self->enemy->viewheight;
    self->move = &gladiator_move_attack_gun;
}

static void         gladiator_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
    {
        if (self->velocity[2] > FIX(100) && self->move == &gladiator_move_pain)
            self->move = &gladiator_move_pain_air;
        return;
    }
    self->pain_debounce = level.time + FIX(3);
    s_play(rng() & 1 ? SND_GLD_PAIN1 : SND_GLD_PAIN2, self->origin, ATTN_NORM);
    self->move = self->velocity[2] > FIX(100) ? &gladiator_move_pain_air : &gladiator_move_pain;
}

static void         gladiator_sight(g_ent *self)
{
    s_play(SND_GLD_SIGHT, self->origin, ATTN_NORM);
}

static void         gladiator_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);                /* (a body: shot or blown up again, it's gibbed; nothing walks into it) */
    self->move = NULL;
}

static void         gladiator_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_GLD_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &gladiator_move_death;
}

void                SP_monster_gladiator(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(32);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(32);
    self->maxs[2] = FIX(64);
    self->skinnum = 0;
    self->health = 400;
    self->gib_health = -175;
    self->stand = gladiator_stand;
    self->run = gladiator_run;
    self->attack = gladiator_attack;
    self->pain = gladiator_pain;
    self->die = gladiator_die;
    self->sight = gladiator_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_gladiator;
#endif
