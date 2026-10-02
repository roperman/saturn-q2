/*
** The gunner: Quake 2's game/m_gunner.c, with the animations baked into
** GUNNER.MDL (stand, run, pain2, pain3, death, chain, grenade). A chaingun
** burst (again and again while it can see you) or, from further off, four
** grenades. Left out to fit (Installation's the only level with room): its
** fidget, walk, run-and-shoot (Quake never uses it) and long pain (the
** middle one in its place), its idle and search sounds, and ducking (the
** port's shots don't warn monsters).
*/
#include "game.h"

static void         gunner_run(g_ent *self);
static void         gunner_dead(g_ent *self);
static void         gunner_opengun(g_ent *self);
static void         GunnerFire(g_ent *self);
static void         GunnerGrenade(g_ent *self);
static void         gunner_fire_chain(g_ent *self);
static void         gunner_refire_chain(g_ent *self);

#define S0              { ai_stand, 0, NULL }
static const mframe_t gunner_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      gunner_move_stand = { "stand", 0, 29, gunner_frames_stand, NULL, 0, 0 };

static const mframe_t gunner_frames_run[] = {
    { ai_run, 26, NULL }, { ai_run, 9, NULL }, { ai_run, 9, NULL }, { ai_run, 9, NULL },
    { ai_run, 15, NULL }, { ai_run, 10, NULL }, { ai_run, 13, NULL }, { ai_run, 6, NULL },
};
static mmove_t      gunner_move_run = { "run", 0, 7, gunner_frames_run, NULL, 0, 0 };

static const mframe_t gunner_frames_pain3[] = {
    { ai_move, -3, NULL }, { ai_move, 1, NULL }, { ai_move, 1, NULL }, { ai_move, 0, NULL }, { ai_move, 1, NULL },
};
static mmove_t      gunner_move_pain3 = { "pain3", 0, 4, gunner_frames_pain3, gunner_run, 0, 0 };

static const mframe_t gunner_frames_pain2[] = {
    { ai_move, -2, NULL }, { ai_move, 11, NULL }, { ai_move, 6, NULL }, { ai_move, 2, NULL },
    { ai_move, -1, NULL }, { ai_move, -7, NULL }, { ai_move, -2, NULL }, { ai_move, -7, NULL },
};
static mmove_t      gunner_move_pain2 = { "pain2", 0, 7, gunner_frames_pain2, gunner_run, 0, 0 };

static const mframe_t gunner_frames_death[] = {
    { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, -7, NULL }, { ai_move, -3, NULL },
    { ai_move, -5, NULL }, { ai_move, 8, NULL }, { ai_move, 6, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
static mmove_t      gunner_move_death = { "death", 0, 10, gunner_frames_death, gunner_dead, 0, 0 };

/* the chaingun: opened, fired (a shot a frame), and put away; "chain" is attak209-attak230 */
#define C0              { ai_charge, 0, NULL }
#define CF              { ai_charge, 0, GunnerFire }
static const mframe_t gunner_frames_attack_chain[] = {
    { ai_charge, 0, gunner_opengun }, C0, C0, C0, C0, C0, C0,
};
static mmove_t      gunner_move_attack_chain = { "chain", 0, 6, gunner_frames_attack_chain, gunner_fire_chain, 0, 0 };

static const mframe_t gunner_frames_fire_chain[] = { CF, CF, CF, CF, CF, CF, CF, CF };
static mmove_t      gunner_move_fire_chain = { "chain", 7, 14, gunner_frames_fire_chain, gunner_refire_chain, 0, 0 };

static const mframe_t gunner_frames_endfire_chain[] = { C0, C0, C0, C0, C0, C0, C0 };
static mmove_t      gunner_move_endfire_chain = { "chain", 15, 21, gunner_frames_endfire_chain, gunner_run, 0, 0 };

#define CG              { ai_charge, 0, GunnerGrenade }
static const mframe_t gunner_frames_attack_grenade[] = {
    C0, C0, C0, C0, CG, C0, C0, CG, C0, C0, CG, C0, C0, CG, C0, C0, C0, C0, C0, C0, C0,
};
static mmove_t      gunner_move_attack_grenade = { "grenade", 0, 20, gunner_frames_attack_grenade, gunner_run, 0, 0 };

static mmove_t      *all_moves[] = {
    &gunner_move_stand, &gunner_move_run, &gunner_move_pain3, &gunner_move_pain2, &gunner_move_death,
    &gunner_move_attack_chain, &gunner_move_fire_chain, &gunner_move_endfire_chain, &gunner_move_attack_grenade,
};

/* Quake's monster_flash_offset for the chaingun's eight frames (x1.15): forward, right, up */
static const s32    fire_ofs[8][3] = {
    { FIX(34.6), FIX(4.5), FIX(22.5) }, { FIX(33.5), FIX(2.9), FIX(23.8) }, { FIX(32.4), FIX(2.9), FIX(25.5) },
    { FIX(32.4), FIX(4.1), FIX(25.3) }, { FIX(30.9), FIX(2.3), FIX(26.9) }, { FIX(30.5), FIX(0.7), FIX(23.9) },
    { FIX(30.9), FIX(0.6), FIX(24.7) }, { FIX(33.4), FIX(2.8), FIX(22.4) },
};

static void         gunner_stand(g_ent *self)
{
    self->move = &gunner_move_stand;
}

static void         gunner_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &gunner_move_stand : &gunner_move_run;
}

/* where a shot starts: the gunner's origin, so far forward, right and up */
static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

static void         gunner_opengun(g_ent *self)
{
    s_play(SND_GUN_OPEN, self->origin, ATTN_IDLE);
}

/* a bullet at you, where you're heading (Quake's aim: back a fifth of a second along your way) */
static void         GunnerFire(g_ent *self)
{
    s32             start[3], aim[3], len;
    int             k, f = iclamp(self->frame - gunner_move_fire_chain.first, 0, 7);

    if (!self->enemy)
        return;
    project(self, fire_ofs[f], start);
    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] - fmul(self->enemy->velocity[k], FIX(0.2)) - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    g_fire_hitscan(self, start, aim, 3, 300, 500, 1);
    g_muzzle_flash(start, 13, 11, 5);
    s_play(SND_GUN_FIRE, start, ATTN_NORM);
}

/* a grenade from the launcher on its left, straight ahead (Quake's aim too) */
static void         GunnerGrenade(g_ent *self)
{
    static const s32 ofs[3] = { FIX(5.3), FIX(-19.3), FIX(8.4) };
    s32             start[3], aim[3];

    project(self, ofs, start);
    aim[0] = fcos(self->yaw);
    aim[1] = fsin(self->yaw);
    aim[2] = 0;
    fx_grenade(self, start, aim, 50, FIX(600));
    g_muzzle_flash(start, 13, 8, 3);
    s_play(SND_GUN_GRENADE, start, ATTN_NORM);
}

static void         gunner_attack(g_ent *self)
{
    if (range(self, self->enemy) == RANGE_MELEE || frandom() > FIX(0.5))
        self->move = &gunner_move_attack_chain;
    else
        self->move = &gunner_move_attack_grenade;
}

static void         gunner_fire_chain(g_ent *self)
{
    self->move = &gunner_move_fire_chain;
}

static void         gunner_refire_chain(g_ent *self)
{
    if (self->enemy && self->enemy->health > 0 && visible(self, self->enemy) && frandom() <= FIX(0.5))
        self->move = &gunner_move_fire_chain;
    else
        self->move = &gunner_move_endfire_chain;
}

static void         gunner_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    s_play(rng() & 1 ? SND_GUN_PAIN1 : SND_GUN_PAIN2, self->origin, ATTN_NORM);
    self->move = damage <= 10 ? &gunner_move_pain3 : &gunner_move_pain2;
}

static void         gunner_sight(g_ent *self)
{
    s_play(SND_GUN_SIGHT, self->origin, ATTN_NORM);
}

static void         gunner_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);
    self->solid = false;
    self->move = NULL;
}

static void         gunner_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_GUN_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &gunner_move_death;
}

void                SP_monster_gunner(g_ent *self)
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
    self->health = 175;
    self->gib_health = -70;
    self->stand = gunner_stand;
    self->run = gunner_run;
    self->attack = gunner_attack;
    self->pain = gunner_pain;
    self->die = gunner_die;
    self->sight = gunner_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_gunner;
#endif
