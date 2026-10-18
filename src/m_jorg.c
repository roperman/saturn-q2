/*
** Jorg (boss2's map, the first half of the last fight): Quake 2's
** game/m_boss31.c, with the animations baked into JORG.MDL (stand, run,
** attack1, attack2, pain1-3, death; its mesh collapsed to the renderer's
** limits, tools/models.txt "mesh="). 3000 health. Three times in four
** both machineguns on you (six bursts of two, 6 of damage a bullet, again
** while it sees you nine times in ten), else a BFG ball (50 of damage,
** 200 units about). A light hit mostly doesn't stop it. Dying it goes down
** over 50 frames, blows up eight times over its body (Quake's BossExplode,
** without the gibs), its body stays, and the Makron jumps out of it at you
** (Quake's MakronToss: the level's spare entity, m_makron.c; without one,
** Jorg's own, its body gone). The Makron rides Jorg through the fight: the
** spare entity draws MAKRON.MDL at Jorg's spot and frame (its first eight
** animations are Jorg's, baked the same: the frame numbers match). Left
** out: its walk along a path, its search calls, its gun's loop.
*/
#include "game.h"

static void         jorg_run(g_ent *self);
static void         jorg_attack1(g_ent *self);
static void         jorg_reattack1(g_ent *self);
static void         jorg_firebullet(g_ent *self);
static void         jorg_bfg(g_ent *self);
static void         jorg_idle(g_ent *self);
static void         jorg_step_left(g_ent *self);
static void         jorg_step_right(g_ent *self);
static void         jorg_dead(g_ent *self);
static void         jorg_explode(g_ent *self);
static void         rider_follow(g_ent *self);

/* (every frame's AI followed by the rider's update: it sits where Jorg is, at Jorg's frame) */
static void         jorg_ai_stand(g_ent *self, s32 dist) { ai_stand(self, dist); rider_follow(self); }
static void         jorg_ai_run(g_ent *self, s32 dist) { ai_run(self, dist); rider_follow(self); }
static void         jorg_ai_charge(g_ent *self, s32 dist) { ai_charge(self, dist); rider_follow(self); }
static void         jorg_ai_move(g_ent *self, s32 dist) { ai_move(self, dist); rider_follow(self); }

#define S0              { jorg_ai_stand, 0, NULL }
#define SD(d, f)        { jorg_ai_stand, d, f }
#define M0              { jorg_ai_move, 0, NULL }
#define MD(d, f)        { jorg_ai_move, d, f }
#define C0              { jorg_ai_charge, 0, NULL }
static const mframe_t jorg_frames_stand[] = {
    { jorg_ai_stand, 0, jorg_idle }, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    SD(19, NULL), SD(11, jorg_step_left), SD(6, NULL), SD(9, jorg_step_right), SD(-2, NULL),
    SD(-17, jorg_step_left), SD(-12, NULL), SD(-14, jorg_step_right),
};
static mmove_t      jorg_move_stand = { "stand", 0, 50, jorg_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t jorg_frames_run[] = {
    { jorg_ai_run, 17, jorg_step_left }, { jorg_ai_run, 0, NULL }, { jorg_ai_run, 0, NULL }, { jorg_ai_run, 0, NULL },
    { jorg_ai_run, 12, NULL }, { jorg_ai_run, 8, NULL }, { jorg_ai_run, 10, NULL }, { jorg_ai_run, 33, jorg_step_right },
    { jorg_ai_run, 0, NULL }, { jorg_ai_run, 0, NULL }, { jorg_ai_run, 0, NULL }, { jorg_ai_run, 9, NULL },
    { jorg_ai_run, 9, NULL }, { jorg_ai_run, 9, NULL },
};
static mmove_t      jorg_move_run = { "run", 0, 13, jorg_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

/* "attack1" is attak101-attak118: the guns up (1-8), the bursts (9-14, looped), the guns down */
static const mframe_t jorg_frames_start_attack1[] = { C0, C0, C0, C0, C0, C0, C0, C0 };
static mmove_t      jorg_move_start_attack1 = { "attack1", 0, 7, jorg_frames_start_attack1, jorg_attack1,
                                                0, 0, 0, 0, 0, 0 };
static const mframe_t jorg_frames_attack1[] = {
    { jorg_ai_charge, 0, jorg_firebullet }, { jorg_ai_charge, 0, jorg_firebullet }, { jorg_ai_charge, 0, jorg_firebullet },
    { jorg_ai_charge, 0, jorg_firebullet }, { jorg_ai_charge, 0, jorg_firebullet }, { jorg_ai_charge, 0, jorg_firebullet },
};
static mmove_t      jorg_move_attack1 = { "attack1", 8, 13, jorg_frames_attack1, jorg_reattack1, 0, 0, 0, 0, 0, 0 };
static const mframe_t jorg_frames_end_attack1[] = { M0, M0, M0, M0 };
static mmove_t      jorg_move_end_attack1 = { "attack1", 14, 17, jorg_frames_end_attack1, jorg_run, 0, 0, 0, 0, 0, 0 };

/* "attack2" is attak201-attak213: the BFG at the 8th */
static const mframe_t jorg_frames_attack2[] = {
    C0, C0, C0, C0, C0, C0, C0, { jorg_ai_charge, 0, jorg_bfg }, M0, M0, M0, M0, M0,
};
static mmove_t      jorg_move_attack2 = { "attack2", 0, 12, jorg_frames_attack2, jorg_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t jorg_frames_pain_short[] = { M0, M0, M0 };
static mmove_t      jorg_move_pain1 = { "pain1", 0, 2, jorg_frames_pain_short, jorg_run, 0, 0, 0, 0, 0, 0 };
static mmove_t      jorg_move_pain2 = { "pain2", 0, 2, jorg_frames_pain_short, jorg_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t jorg_frames_pain3[] = {
    MD(-28, NULL), MD(-6, NULL), MD(-3, jorg_step_left), MD(-9, NULL), MD(0, jorg_step_right), MD(-7, NULL),
    MD(1, NULL), MD(-11, NULL), MD(-4, NULL), M0, M0, M0, M0, MD(10, NULL), MD(11, NULL), MD(10, NULL),
    MD(3, NULL), MD(10, NULL), MD(7, jorg_step_left), MD(17, NULL), MD(0, jorg_step_right), M0, M0, M0, M0,
};
static mmove_t      jorg_move_pain3 = { "pain3", 0, 24, jorg_frames_pain3, jorg_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t jorg_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      jorg_move_death = { "death", 0, 49, jorg_frames_death, jorg_dead, 0, 0, 0, 0, 0, 0 };
/* (down: the last frame held, a bang a tick over it, g_ent count's; on the eighth the Makron's out) */
static const mframe_t jorg_frames_explode[] = { { ai_move, 0, jorg_explode } };
static mmove_t      jorg_move_explode = { "death", 49, 49, jorg_frames_explode, NULL, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &jorg_move_stand, &jorg_move_run, &jorg_move_start_attack1, &jorg_move_attack1, &jorg_move_end_attack1,
    &jorg_move_attack2, &jorg_move_pain1, &jorg_move_pain2, &jorg_move_pain3, &jorg_move_death, &jorg_move_explode,
};

/* the rider: the level's spare entity, found at Jorg's spawn (NULL: none, or no Makron model) */
static g_ent        *rider;

static void         rider_follow(g_ent *self)
{
    g_ent           *r = rider;
    int             k;

    if (!r || r->cls != C_MONSTER_MAKRON || r->kind != EK_MONSTER || r->move)
        return;                             /* (none; or the Makron himself now) */
    for (k = 0; k < 3; ++k)
    {
        r->origin[k] = self->origin[k];
        r->old_origin[k] = self->old_origin[k];
    }
    r->yaw = self->yaw;
    r->old_yaw = self->old_yaw;
    r->frame = self->frame;
    r->old_frame = self->old_frame;
    r->afirst = self->afirst;
    r->aend = self->aend;
    r->lbase = self->lbase;
    r->astride = self->astride;
    r->old_mframe = self->old_mframe;
    r->skinnum = self->skinnum;
    r->leaf = self->leaf;
}

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

static void         jorg_idle(g_ent *self)
{
    s_play(SND_JO_IDLE, self->origin, ATTN_NORM);
}

static void         jorg_step_left(g_ent *self)
{
    s_play(SND_JO_STEP1, self->origin, ATTN_NORM);
}

static void         jorg_step_right(g_ent *self)
{
    s_play(SND_JO_STEP2, self->origin, ATTN_NORM);
}

static void         jorg_stand(g_ent *self)
{
    self->move = &jorg_move_stand;
}

static void         jorg_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &jorg_move_stand : &jorg_move_run;
}

/* a bullet from each gun (Quake's MZ2_JORG_MACHINEGUN_L1 and R1), 6 of damage, at where you'll
   be in a fifth of a second */
static void         jorg_firebullet(g_ent *self)
{
    static const s32 ofs[2][3] = { { FIX(78.5), FIX(-47.1), FIX(96) }, { FIX(78.5), FIX(46.7), FIX(96) } };
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
    s_play(SND_JO_MG, self->origin, ATTN_NORM);
}

/* a BFG ball from over its head (Quake's MZ2_JORG_BFG_1): 50 of damage, 200 units about */
static void         jorg_bfg(g_ent *self)
{
    static const s32 ofs[3] = { FIX(6.3), FIX(-9), FIX(111.2) };
    s32             start[3], aim[3];

    if (!self->enemy)
        return;
    project(self, ofs, start);
    if (!aim_at(self, start, 0, aim))
        return;
    fx_bfg(self, start, aim, 50, 200);
    g_muzzle_flash(start, 4, 13, 4);
    s_play(SND_JO_ATTACK2, self->origin, ATTN_NORM);
}

static void         jorg_attack(g_ent *self)
{
    if (frandom() <= FIX(0.75))
    {
        s_play(SND_JO_ATTACK1, self->origin, ATTN_NORM);
        self->move = &jorg_move_start_attack1;
    }
    else
    {
        s_play(SND_JO_ATTACK2, self->origin, ATTN_NORM);
        self->move = &jorg_move_attack2;
    }
}

static void         jorg_attack1(g_ent *self)
{
    self->move = &jorg_move_attack1;
}

static void         jorg_reattack1(g_ent *self)
{
    if (self->enemy && visible(self, self->enemy) && frandom() < FIX(0.9))
        self->move = &jorg_move_attack1;
    else
        self->move = &jorg_move_end_attack1;
}

static void         jorg_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    if (damage <= 40 && frandom() <= FIX(0.6))
        return;                             /* (Quake's: a light hit mostly doesn't stop it) */
    self->pain_debounce = level.time + FIX(3);
    if (damage <= 50)
    {
        s_play(SND_JO_PAIN1, self->origin, ATTN_NORM);
        self->move = &jorg_move_pain1;
    }
    else if (damage <= 100)
    {
        s_play(SND_JO_PAIN2, self->origin, ATTN_NORM);
        self->move = &jorg_move_pain2;
    }
    else if (frandom() <= FIX(0.3))
    {
        s_play(SND_JO_PAIN3, self->origin, ATTN_NORM);
        self->move = &jorg_move_pain3;
    }
}

/* the Makron out of the wreck at you (Quake's MakronSpawn: a jump, 400 at you and 200 up) */
static void         makron_out(g_ent *e)
{
    s32             v[3], len;
    int             k;

    e->cls = C_MONSTER_MAKRON;
    g_monster_revive(e);
    if (e->kind != EK_MONSTER)
        return;                             /* (no Makron: the model's not here) */
    if (e->sight)
        e->sight(e);                        /* (its popup) */
    for (k = 0; k < 3; ++k)
        v[k] = g_player->origin[k] - e->origin[k];
    e->yaw = vectoyaw(v);
    len = vlen(v);
    if (len > FIX(1))
        for (k = 0; k < 3; ++k)
            e->velocity[k] = fmul(fdiv(v[k], len), FIX(400));
    e->velocity[2] = FIX(200);
    e->on_ground = false;
}

/* down: a bang a tick at eight spots about it; on the eighth its body stays, and the Makron's out */
static void         jorg_explode(g_ent *self)
{
    static const s8 spot[8][2] = {
        { -24, -24 }, { 24, 24 }, { 24, -24 }, { -24, 24 }, { -48, -48 }, { 48, 48 }, { -48, 48 }, { 48, -48 },
    };
    s32             p[3];
    int             n = self->count;

    if (n >= 8)
    {
        self->move = NULL;                  /* (a body, as Quake's: it stays) */
        if (rider && rider->cls == C_MONSTER_MAKRON && rider->kind == EK_MONSTER && !rider->move)
            makron_out(rider);
        else
        {
            self->solid = false;
            makron_out(self);               /* (no spare entity: out of Jorg's own, the body gone) */
        }
        rider = NULL;
        return;
    }
    self->count = n + 1;
    p[0] = self->origin[0] + FIX(spot[n][0]);
    p[1] = self->origin[1] + FIX(spot[n][1]);
    p[2] = self->origin[2] + FIX(24 + (rng() & 15));
    s_play(SND_EXPLOSION, p, ATTN_NORM);
    fx_explosion(p);
}

static void         jorg_dead(g_ent *self)
{
    self->mins[0] = self->mins[1] = -FIX(60);
    self->maxs[0] = self->maxs[1] = FIX(60);
    self->maxs[2] = FIX(72);
    self->solid = false;
    self->count = 0;
    self->move = &jorg_move_explode;
}

static void         jorg_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->takedamage = false;               /* (Quake's: it can't be gibbed on its way down) */
    s_play(SND_JO_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &jorg_move_death;
}

void                SP_monster_jorg(g_ent *self)
{
    int             i;

    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(80);
    self->mins[2] = 0;
    self->maxs[0] = self->maxs[1] = FIX(80);
    self->maxs[2] = FIX(140);
    self->skinnum = 0;
    self->health = 3000;
    self->gib_health = -2000;
    self->stand = jorg_stand;
    self->run = jorg_run;
    self->attack = jorg_attack;
    self->pain = jorg_pain;
    self->die = jorg_die;
    monster_start(self);
    /* the rider: the level's spare entity (g_main.c g_count: one a Jorg), the Makron's model on it */
    rider = NULL;
    if (models[MDL_MAKRON].loaded)
        for (i = g_nfull - 1; i > 0; --i)   /* (the last: the spawn's loop fills from the first, and is still at Jorg) */
        {
            g_ent   *e = &g_edicts[i];

            if (e != self && e->kind == EK_FREE && e->cls == 0)
            {
                memset(e, 0, sizeof(*e));
                e->kind = EK_MONSTER;
                e->cls = C_MONSTER_MAKRON;
                e->mdl = &models[MDL_MAKRON];
                e->health = 1;
                e->viewheight = FIX(25);
                for (i = 0; i < 3; ++i)
                {
                    e->mins[i] = self->mins[i];
                    e->maxs[i] = self->maxs[i];
                }
                rider = e;
                rider_follow(self);
                break;
            }
        }
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_jorg;
#endif
