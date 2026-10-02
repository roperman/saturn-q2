/*
** The tank: Quake 2's game/m_tank.c, with the animations baked into TANK.MDL
** (stand, run, blast, rocket, chain, pain1-3, death). Three blaster bolts,
** a machinegun burst swept across you, or (from further off) three rockets;
** on hard it fires again while it can see you. Loaded with the levels that
** have one (Installation's: build.sh build_overlays). Left out to fit: its
** walk (it runs as it walks), the stomp it does on you when you're dead,
** its idle and death thud sounds. (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         tank_run(g_ent *self);
static void         tank_dead(g_ent *self);
static void         tank_footstep(g_ent *self);
static void         TankBlaster(g_ent *self);
static void         TankRocket(g_ent *self);
static void         TankMachineGun(g_ent *self);
static void         tank_reattack_blaster(g_ent *self);
static void         tank_doattack_rocket(g_ent *self);
static void         tank_refire_rocket(g_ent *self);

#define S0              { ai_stand, 0, NULL }
static const mframe_t tank_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      tank_move_stand = { "stand", 0, 29, tank_frames_stand, NULL, 0, 0 };

/* "run" is walk01-walk20: the start, then the loop */
static const mframe_t tank_frames_start_run[] = {
    { ai_run, 0, NULL }, { ai_run, 6, NULL }, { ai_run, 6, NULL }, { ai_run, 11, tank_footstep },
};
static mmove_t      tank_move_start_run = { "run", 0, 3, tank_frames_start_run, tank_run, 0, 0 };

static const mframe_t tank_frames_run[] = {
    { ai_run, 4, NULL }, { ai_run, 5, NULL }, { ai_run, 3, NULL }, { ai_run, 2, NULL },
    { ai_run, 5, NULL }, { ai_run, 5, NULL }, { ai_run, 4, NULL }, { ai_run, 4, tank_footstep },
    { ai_run, 3, NULL }, { ai_run, 5, NULL }, { ai_run, 4, NULL }, { ai_run, 5, NULL },
    { ai_run, 7, NULL }, { ai_run, 7, NULL }, { ai_run, 6, NULL }, { ai_run, 6, tank_footstep },
};
static mmove_t      tank_move_run = { "run", 4, 19, tank_frames_run, NULL, 0, 0 };

#define M0              { ai_move, 0, NULL }
static const mframe_t tank_frames_pain1[] = { M0, M0, M0, M0 };
static mmove_t      tank_move_pain1 = { "pain1", 0, 3, tank_frames_pain1, tank_run, 0, 0 };

static const mframe_t tank_frames_pain2[] = { M0, M0, M0, M0, M0 };
static mmove_t      tank_move_pain2 = { "pain2", 0, 4, tank_frames_pain2, tank_run, 0, 0 };

static const mframe_t tank_frames_pain3[] = {
    { ai_move, -7, NULL }, M0, M0, M0, { ai_move, 2, NULL }, M0, M0, { ai_move, 3, NULL },
    M0, { ai_move, 2, NULL }, M0, M0, M0, M0, M0, { ai_move, 0, tank_footstep },
};
static mmove_t      tank_move_pain3 = { "pain3", 0, 15, tank_frames_pain3, tank_run, 0, 0 };

/* "blast" is attak101-attak122: the bolts at 110, 113 and 116, again from 111, then put away */
#define C0              { ai_charge, 0, NULL }
#define CB              { ai_charge, 0, TankBlaster }
static const mframe_t tank_frames_attack_blast[] = {
    C0, C0, C0, C0, { ai_charge, -1, NULL }, { ai_charge, -2, NULL }, { ai_charge, -1, NULL }, { ai_charge, -1, NULL },
    C0, CB, C0, C0, CB, C0, C0, CB,
};
static mmove_t      tank_move_attack_blast = { "blast", 0, 15, tank_frames_attack_blast, tank_reattack_blaster, 0, 0 };

static const mframe_t tank_frames_reattack_blast[] = { C0, C0, CB, C0, C0, CB };
static mmove_t      tank_move_reattack_blast = { "blast", 10, 15, tank_frames_reattack_blast, tank_reattack_blaster, 0, 0 };

static const mframe_t tank_frames_attack_post_blast[] = {
    M0, M0, { ai_move, 2, NULL }, { ai_move, 3, NULL }, { ai_move, 2, NULL }, { ai_move, -2, tank_footstep },
};
static mmove_t      tank_move_attack_post_blast = { "blast", 16, 21, tank_frames_attack_post_blast, tank_run, 0, 0 };

/* "rocket" is attak301-attak353: raised, the rockets at 324, 327 and 330, lowered */
static const mframe_t tank_frames_attack_pre_rocket[] = {
    C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, { ai_charge, 1, NULL }, { ai_charge, 2, NULL },
    { ai_charge, 7, NULL }, { ai_charge, 7, NULL }, { ai_charge, 7, tank_footstep }, C0, C0, C0, C0,
    { ai_charge, -3, NULL },
};
static mmove_t      tank_move_attack_pre_rocket = { "rocket", 0, 20, tank_frames_attack_pre_rocket, tank_doattack_rocket,
                                                    0, 0 };

static const mframe_t tank_frames_attack_fire_rocket[] = {
    { ai_charge, -3, NULL }, C0, { ai_charge, 0, TankRocket }, C0, C0, { ai_charge, 0, TankRocket }, C0, C0,
    { ai_charge, -1, TankRocket },
};
static mmove_t      tank_move_attack_fire_rocket = { "rocket", 21, 29, tank_frames_attack_fire_rocket, tank_refire_rocket,
                                                     0, 0 };

static const mframe_t tank_frames_attack_post_rocket[] = {
    C0, { ai_charge, -1, NULL }, { ai_charge, -1, NULL }, C0, { ai_charge, 2, NULL }, { ai_charge, 3, NULL },
    { ai_charge, 4, NULL }, { ai_charge, 2, NULL }, C0, C0, C0, { ai_charge, -9, NULL }, { ai_charge, -8, NULL },
    { ai_charge, -7, NULL }, { ai_charge, -1, NULL }, { ai_charge, -1, tank_footstep }, C0, C0, C0, C0, C0, C0, C0,
};
static mmove_t      tank_move_attack_post_rocket = { "rocket", 30, 52, tank_frames_attack_post_rocket, tank_run, 0, 0 };

/* "chain" is attak401-attak429: the gun swept across you, a shot a frame from 406 to 424 */
#define CM              { NULL, 0, TankMachineGun }
static const mframe_t tank_frames_attack_chain[] = {
    C0, C0, C0, C0, C0, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, CM, C0, C0, C0, C0, C0,
};
static mmove_t      tank_move_attack_chain = { "chain", 0, 28, tank_frames_attack_chain, tank_run, 0, 0 };

static const mframe_t tank_frames_death[] = {
    { ai_move, -7, NULL }, { ai_move, -2, NULL }, { ai_move, -2, NULL }, { ai_move, 1, NULL }, { ai_move, 3, NULL },
    { ai_move, 6, NULL }, { ai_move, 1, NULL }, { ai_move, 1, NULL }, { ai_move, 2, NULL }, M0, M0, M0,
    { ai_move, -2, NULL }, M0, M0, { ai_move, -3, NULL }, M0, M0, M0, M0, M0, M0, { ai_move, -4, NULL },
    { ai_move, -6, NULL }, { ai_move, -4, NULL }, { ai_move, -5, NULL }, { ai_move, -7, NULL }, { ai_move, -15, NULL },
    { ai_move, -5, NULL }, M0, M0, M0,
};
static mmove_t      tank_move_death = { "death", 0, 31, tank_frames_death, tank_dead, 0, 0 };

static mmove_t      *all_moves[] = {
    &tank_move_stand, &tank_move_start_run, &tank_move_run, &tank_move_pain1, &tank_move_pain2, &tank_move_pain3,
    &tank_move_attack_blast, &tank_move_reattack_blast, &tank_move_attack_post_blast, &tank_move_attack_pre_rocket,
    &tank_move_attack_fire_rocket, &tank_move_attack_post_rocket, &tank_move_attack_chain, &tank_move_death,
};

/* Quake's monster_flash_offset: forward, right, up (the blaster's three, the machinegun's 19, the rockets' three) */
static const s32    blaster_ofs[3][3] = {
    { FIX(20.7), FIX(-18.5), FIX(28.7) }, { FIX(16.6), FIX(-21.5), FIX(30.1) }, { FIX(11.8), FIX(-23.9), FIX(32.1) },
};
static const s32    mg_ofs[19][3] = {
    { FIX(22.9), FIX(-0.7), FIX(25.3) }, { FIX(22.2), FIX(6.2), FIX(22.3) }, { FIX(19.4), FIX(13.1), FIX(18.6) },
    { FIX(19.4), FIX(18.8), FIX(18.6) }, { FIX(17.9), FIX(25.0), FIX(18.6) }, { FIX(14.1), FIX(30.5), FIX(20.6) },
    { FIX(9.3), FIX(35.3), FIX(22.1) }, { FIX(4.7), FIX(38.4), FIX(22.1) }, { FIX(-1.1), FIX(40.4), FIX(24.1) },
    { FIX(-6.5), FIX(41.2), FIX(24.1) }, { FIX(3.2), FIX(40.1), FIX(24.7) }, { FIX(11.7), FIX(36.7), FIX(26.0) },
    { FIX(18.9), FIX(31.3), FIX(26.0) }, { FIX(24.4), FIX(24.4), FIX(26.4) }, { FIX(27.1), FIX(17.1), FIX(27.2) },
    { FIX(28.5), FIX(9.1), FIX(28.0) }, { FIX(27.1), FIX(2.2), FIX(28.0) }, { FIX(24.9), FIX(-2.8), FIX(28.0) },
    { FIX(21.6), FIX(-7.0), FIX(26.4) },
};
static const s32    rocket_ofs[3][3] = {
    { FIX(6.2), FIX(29.1), FIX(49.1) }, { FIX(6.9), FIX(23.8), FIX(49.1) }, { FIX(8.3), FIX(17.8), FIX(49.5) },
};

static void         tank_stand(g_ent *self)
{
    self->move = &tank_move_stand;
}

static void         tank_run(g_ent *self)
{
    if (self->aiflags & AI_STAND_GROUND)
        self->move = &tank_move_stand;
    else if (self->move == &tank_move_start_run || self->move == &tank_move_run)
        self->move = &tank_move_run;
    else
        self->move = &tank_move_start_run;
}

static void         tank_footstep(g_ent *self)
{
    s_play(SND_TNK_STEP, self->origin, ATTN_NORM);
}

/* where a shot starts: the tank's origin, so far forward, right and up */
static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

/* from start to your eyes, of length 1 (false: you're at the muzzle) */
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

static void         TankBlaster(g_ent *self)
{
    int             n = self->frame - tank_move_attack_blast.first;     /* attak101 on */
    s32             start[3], aim[3];

    if (!self->enemy)
        return;
    project(self, blaster_ofs[n <= 9 ? 0 : n <= 12 ? 1 : 2], start);
    if (!aim_at(self, start, aim))
        return;
    g_fire_blaster(self, start, aim, 30, FIX(800));
    g_muzzle_flash(start, 12, 10, 3);
    s_play(SND_TNK_BLASTER, start, ATTN_NORM);
}

static void         TankRocket(g_ent *self)
{
    int             n = self->frame - tank_move_attack_pre_rocket.first;    /* attak301 on */
    s32             start[3], aim[3];

    if (!self->enemy)
        return;
    project(self, rocket_ofs[n <= 23 ? 0 : n <= 26 ? 1 : 2], start);
    if (!aim_at(self, start, aim))
        return;
    fx_rocket(self, start, aim, 50, 50);
    g_muzzle_flash(start, 13, 10, 4);
    s_play(SND_TNK_ROCKET, start, ATTN_NORM);
}

/* a bullet a frame: the gun's height at you, its way swept from 40 degrees one side to 40 the
   other (Quake's: 8 a frame, back across at 416) */
static void         TankMachineGun(g_ent *self)
{
    int             f = iclamp(self->frame - tank_move_attack_chain.first - 5, 0, 18), frame = 406 + f;
    int             yaw = self->yaw + (frame <= 415 ? -1456 * (frame - 411) : 1456 * (frame - 419));
    s32             start[3], aim[3], h = FIX(1), v = 0;

    project(self, mg_ofs[f], start);
    if (self->enemy && aim_at(self, start, aim))
    {
        v = aim[2];
        aim[2] = 0;
        h = vlen(aim);                      /* (the aim's length across) */
    }
    aim[0] = fmul(h, fcos(yaw & 0xFFFF));
    aim[1] = fmul(h, fsin(yaw & 0xFFFF));
    aim[2] = v;
    g_fire_hitscan(self, start, aim, 20, 300, 500, 1);
    g_muzzle_flash(start, 13, 11, 5);
    s_play(SND_TNK_MG, start, ATTN_NORM);
}

static void         tank_reattack_blaster(g_ent *self)
{
    if (g_skill >= 2 && self->enemy && self->enemy->health > 0 && visible(self, self->enemy) && frandom() <= FIX(0.6))
        self->move = &tank_move_reattack_blast;
    else
        self->move = &tank_move_attack_post_blast;
}

static void         tank_doattack_rocket(g_ent *self)
{
    self->move = &tank_move_attack_fire_rocket;
}

static void         tank_refire_rocket(g_ent *self)
{
    if (g_skill >= 2 && self->enemy && self->enemy->health > 0 && visible(self, self->enemy) && frandom() <= FIX(0.4))
        self->move = &tank_move_attack_fire_rocket;
    else
        self->move = &tank_move_attack_post_rocket;
}

/* near, the machinegun or the blaster; far, the rockets too */
static void         tank_attack(g_ent *self)
{
    s32             d[3], r = frandom(), range;
    int             k;

    if (self->enemy->health <= 0)
    {
        tank_run(self);                     /* (Quake's stomps on you: left out) */
        return;
    }
    for (k = 0; k < 3; ++k)
        d[k] = self->enemy->origin[k] - self->origin[k];
    range = vlen(d);
    if (range <= FIX(125))
        self->move = r < FIX(0.4) ? &tank_move_attack_chain : &tank_move_attack_blast;
    else if (range <= FIX(250))
        self->move = r < FIX(0.5) ? &tank_move_attack_chain : &tank_move_attack_blast;
    else if (r < FIX(0.33))
        self->move = &tank_move_attack_chain;
    else if (r < FIX(0.66))
    {
        self->move = &tank_move_attack_pre_rocket;
        self->pain_debounce = level.time + FIX(5);  /* (no pain for a while) */
    }
    else
        self->move = &tank_move_attack_blast;
}

static void         tank_pain(g_ent *self, g_ent *other, int damage)
{
    int             n;

    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (damage <= 10 || level.time < self->pain_debounce)
        return;
    if (damage <= 30 && frandom() > FIX(0.2))
        return;
    if (g_skill >= 2)
    {
        /* (hard: not while it's firing its rockets or its blaster) */
        n = self->frame - tank_move_attack_pre_rocket.first;
        if (n >= 0 && n <= 29)
            return;
        n = self->frame - tank_move_attack_blast.first;
        if (n >= 0 && n <= 15)
            return;
    }
    self->pain_debounce = level.time + FIX(3);
    s_play(SND_TNK_PAIN, self->origin, ATTN_NORM);
    self->move = damage <= 30 ? &tank_move_pain1 : damage <= 60 ? &tank_move_pain2 : &tank_move_pain3;
}

static void         tank_sight(g_ent *self)
{
    s_play(SND_TNK_SIGHT, self->origin, ATTN_NORM);
}

static void         tank_dead(g_ent *self)
{
    self->maxs[2] = 0;
    self->solid = false;
    self->move = NULL;
}

static void         tank_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_TNK_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &tank_move_death;
}

void                SP_monster_tank(g_ent *self)
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
    self->mins[2] = -FIX(16);
    self->maxs[0] = self->maxs[1] = FIX(32);
    self->maxs[2] = FIX(72);
    self->skinnum = 0;
    self->health = 750;
    self->gib_health = -200;
    self->stand = tank_stand;
    self->run = tank_run;
    self->attack = tank_attack;
    self->pain = tank_pain;
    self->die = tank_die;
    self->sight = tank_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_tank;
#endif
