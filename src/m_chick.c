/*
** The iron maiden: Quake 2's game/m_chick.c, with the animations baked into
** CHICK.MDL (stand, run, pain1-3, death1-2, rocket, slash). From a distance
** she fires rockets, again and again while she can see you (six in ten); in
** reach she slashes (10 to 15), again while you're there. Her run starts
** with ten frames of getting going, as Quake's. Left out: her fidget, walk,
** duck, idle and search sounds. (Her gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         chick_run(g_ent *self);
static void         chick_dead(g_ent *self);
static void         chick_attack1(g_ent *self);
static void         chick_rerocket(g_ent *self);
static void         chick_slash(g_ent *self);
static void         chick_reslash(g_ent *self);
static void         Chick_PreAttack1(g_ent *self);
static void         ChickRocket(g_ent *self);
static void         ChickReload(g_ent *self);
static void         ChickSlash(g_ent *self);

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
static const mframe_t chick_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      chick_move_stand = { "stand", 0, 29, chick_frames_stand, NULL, 0, 0 };

/* "run" is walk01-walk20: getting going, then the loop */
static const mframe_t chick_frames_start_run[] = {
    { ai_run, 1, NULL }, { ai_run, 0, NULL }, { ai_run, 0, NULL }, { ai_run, -1, NULL }, { ai_run, -1, NULL },
    { ai_run, 0, NULL }, { ai_run, 1, NULL }, { ai_run, 3, NULL }, { ai_run, 6, NULL }, { ai_run, 3, NULL },
};
static mmove_t      chick_move_start_run = { "run", 0, 9, chick_frames_start_run, chick_run, 0, 0 };

static const mframe_t chick_frames_run[] = {
    { ai_run, 6, NULL }, { ai_run, 8, NULL }, { ai_run, 13, NULL }, { ai_run, 5, NULL }, { ai_run, 7, NULL },
    { ai_run, 4, NULL }, { ai_run, 11, NULL }, { ai_run, 5, NULL }, { ai_run, 9, NULL }, { ai_run, 7, NULL },
};
static mmove_t      chick_move_run = { "run", 10, 19, chick_frames_run, NULL, 0, 0 };

static const mframe_t chick_frames_pain1[] = { M0, M0, M0, M0, M0 };
static mmove_t      chick_move_pain1 = { "pain1", 0, 4, chick_frames_pain1, chick_run, 0, 0 };
static const mframe_t chick_frames_pain2[] = { M0, M0, M0, M0, M0 };
static mmove_t      chick_move_pain2 = { "pain2", 0, 4, chick_frames_pain2, chick_run, 0, 0 };
static const mframe_t chick_frames_pain3[] = {
    M0, M0, { ai_move, -6, NULL }, { ai_move, 3, NULL }, { ai_move, 11, NULL }, { ai_move, 3, NULL }, M0, M0,
    { ai_move, 4, NULL }, { ai_move, 1, NULL }, M0, { ai_move, -3, NULL }, { ai_move, -4, NULL }, { ai_move, 5, NULL },
    { ai_move, 7, NULL }, { ai_move, -2, NULL }, { ai_move, 3, NULL }, { ai_move, -5, NULL }, { ai_move, -2, NULL },
    { ai_move, -8, NULL }, { ai_move, 2, NULL },
};
static mmove_t      chick_move_pain3 = { "pain3", 0, 20, chick_frames_pain3, chick_run, 0, 0 };

static const mframe_t chick_frames_death1[] = {
    M0, M0, { ai_move, -7, NULL }, { ai_move, 4, NULL }, { ai_move, 11, NULL }, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      chick_move_death1 = { "death1", 0, 11, chick_frames_death1, chick_dead, 0, 0 };

static const mframe_t chick_frames_death2[] = {
    { ai_move, -6, NULL }, M0, { ai_move, -1, NULL }, { ai_move, -5, NULL }, M0, { ai_move, -1, NULL },
    { ai_move, -2, NULL }, { ai_move, 1, NULL }, { ai_move, 10, NULL }, { ai_move, 2, NULL }, { ai_move, 3, NULL },
    { ai_move, 1, NULL }, { ai_move, 2, NULL }, M0, { ai_move, 3, NULL }, { ai_move, 3, NULL }, { ai_move, 1, NULL },
    { ai_move, -3, NULL }, { ai_move, -5, NULL }, { ai_move, 4, NULL }, { ai_move, 15, NULL }, { ai_move, 14, NULL },
    { ai_move, 1, NULL },
};
static mmove_t      chick_move_death2 = { "death2", 0, 22, chick_frames_death2, chick_dead, 0, 0 };

/* "rocket" is attak101-attak132: the launcher raised, a rocket and a reload (again while she sees you), lowered */
static const mframe_t chick_frames_start_attack1[] = {
    { ai_charge, 0, Chick_PreAttack1 }, C0, C0, { ai_charge, 4, NULL }, C0, { ai_charge, -3, NULL }, { ai_charge, 3, NULL },
    { ai_charge, 5, NULL }, { ai_charge, 7, NULL }, C0, C0, C0, { ai_charge, 0, chick_attack1 },
};
static mmove_t      chick_move_start_attack1 = { "rocket", 0, 12, chick_frames_start_attack1, NULL, 0, 0 };

static const mframe_t chick_frames_attack1[] = {
    { ai_charge, 19, ChickRocket }, { ai_charge, -6, NULL }, { ai_charge, -5, NULL }, { ai_charge, -2, NULL },
    { ai_charge, -7, NULL }, C0, { ai_charge, 1, NULL }, { ai_charge, 10, ChickReload }, { ai_charge, 4, NULL },
    { ai_charge, 5, NULL }, { ai_charge, 6, NULL }, { ai_charge, 6, NULL }, { ai_charge, 4, NULL },
    { ai_charge, 3, chick_rerocket },
};
static mmove_t      chick_move_attack1 = { "rocket", 13, 26, chick_frames_attack1, NULL, 0, 0 };

static const mframe_t chick_frames_end_attack1[] = {
    { ai_charge, -3, NULL }, C0, { ai_charge, -6, NULL }, { ai_charge, -4, NULL }, { ai_charge, -2, NULL },
};
static mmove_t      chick_move_end_attack1 = { "rocket", 27, 31, chick_frames_end_attack1, chick_run, 0, 0 };

/* "slash" is attak201-attak216: a step in, the slash (again while you're in reach), a step back */
static const mframe_t chick_frames_start_slash[] = { { ai_charge, 1, NULL }, { ai_charge, 8, NULL }, { ai_charge, 3, NULL } };
static mmove_t      chick_move_start_slash = { "slash", 0, 2, chick_frames_start_slash, chick_slash, 0, 0 };

static const mframe_t chick_frames_slash[] = {
    { ai_charge, 1, NULL }, { ai_charge, 7, ChickSlash }, { ai_charge, -7, NULL }, { ai_charge, 1, NULL },
    { ai_charge, -1, NULL }, { ai_charge, 1, NULL }, C0, { ai_charge, 1, NULL }, { ai_charge, -2, chick_reslash },
};
static mmove_t      chick_move_slash = { "slash", 3, 11, chick_frames_slash, NULL, 0, 0 };

static const mframe_t chick_frames_end_slash[] = {
    { ai_charge, -6, NULL }, { ai_charge, -1, NULL }, { ai_charge, -6, NULL }, C0,
};
static mmove_t      chick_move_end_slash = { "slash", 12, 15, chick_frames_end_slash, chick_run, 0, 0 };

static mmove_t      *all_moves[] = {
    &chick_move_stand, &chick_move_start_run, &chick_move_run, &chick_move_pain1, &chick_move_pain2, &chick_move_pain3,
    &chick_move_death1, &chick_move_death2, &chick_move_start_attack1, &chick_move_attack1, &chick_move_end_attack1,
    &chick_move_start_slash, &chick_move_slash, &chick_move_end_slash,
};

static void         chick_stand(g_ent *self)
{
    self->move = &chick_move_stand;
}

static void         chick_run(g_ent *self)
{
    if (self->aiflags & AI_STAND_GROUND)
        self->move = &chick_move_stand;
    else
        self->move = self->move == &chick_move_start_run ? &chick_move_run : &chick_move_start_run;
}

/* where a shot starts: her origin, so far forward, right and up */
static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

static void         Chick_PreAttack1(g_ent *self)
{
    s_play(SND_CHK_PRELAUNCH, self->origin, ATTN_NORM);
}

static void         ChickReload(g_ent *self)
{
    s_play(SND_CHK_RELOAD, self->origin, ATTN_NORM);
}

/* a rocket from the launcher on her shoulder, at you (Quake's MZ2_CHICK_ROCKET_1) */
static void         ChickRocket(g_ent *self)
{
    static const s32 ofs[3] = { FIX(24.8), FIX(-9.0), FIX(39.0) };
    s32             start[3], aim[3], len;
    int             k;

    if (!self->enemy)
        return;
    project(self, ofs, start);
    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    fx_rocket(self, start, aim, 50, 50);
    g_muzzle_flash(start, 13, 8, 3);
    s_play(SND_ROCKET_FIRE, start, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static void         ChickSlash(g_ent *self)
{
    s_play(SND_CHK_SWING, self->origin, ATTN_NORM);
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
    {
        g_damage(self->enemy, self, 10 + (int)(rng() % 6), self->enemy->origin);
        s_play(SND_CHK_HIT, self->origin, ATTN_NORM);
    }
}

/* (the port's AI asks for an attack in reach too: the slash there, the rockets otherwise) */
static void         chick_attack(g_ent *self)
{
    self->move = range(self, self->enemy) == RANGE_MELEE ? &chick_move_start_slash : &chick_move_start_attack1;
}

static void         chick_attack1(g_ent *self)
{
    self->move = &chick_move_attack1;
}

static void         chick_rerocket(g_ent *self)
{
    if (self->enemy && self->enemy->health > 0 && range(self, self->enemy) > RANGE_MELEE && visible(self, self->enemy)
        && frandom() <= FIX(0.6))
        self->move = &chick_move_attack1;
    else
        self->move = &chick_move_end_attack1;
}

static void         chick_slash(g_ent *self)
{
    self->move = &chick_move_slash;
}

static void         chick_reslash(g_ent *self)
{
    if (self->enemy && self->enemy->health > 0 && range(self, self->enemy) == RANGE_MELEE && frandom() <= FIX(0.9))
        self->move = &chick_move_slash;
    else
        self->move = &chick_move_end_slash;
}

static void         chick_pain(g_ent *self, g_ent *other, int damage)
{
    u32             n;

    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    n = rng() % 3;
    s_play(n == 0 ? SND_CHK_PAIN1 : n == 1 ? SND_CHK_PAIN2 : SND_CHK_PAIN3, self->origin, ATTN_NORM);
    self->move = damage <= 10 ? &chick_move_pain1 : damage <= 25 ? &chick_move_pain2 : &chick_move_pain3;
}

static void         chick_sight(g_ent *self)
{
    s_play(SND_CHK_SIGHT, self->origin, ATTN_NORM);
}

static void         chick_dead(g_ent *self)
{
    self->maxs[2] = FIX(16);                /* (a body, her origin at her feet: shot or blown up again, it's gibbed) */
    self->move = NULL;
}

static void         chick_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->aiflags &= ~AI_HOLD_FRAME;
    if (rng() & 1)
    {
        s_play(SND_CHK_DEATH1, self->origin, ATTN_NORM);
        self->move = &chick_move_death1;
    }
    else
    {
        s_play(SND_CHK_DEATH2, self->origin, ATTN_NORM);
        self->move = &chick_move_death2;
    }
}

void                SP_monster_chick(g_ent *self)
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
    self->mins[2] = 0;                      /* (her origin's at her feet, as Quake's) */
    self->maxs[0] = self->maxs[1] = FIX(16);
    self->maxs[2] = FIX(56);
    self->skinnum = 0;
    self->health = 175;
    self->gib_health = -70;
    self->stand = chick_stand;
    self->run = chick_run;
    self->attack = chick_attack;
    self->pain = chick_pain;
    self->die = chick_die;
    self->sight = chick_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_chick;
#endif
