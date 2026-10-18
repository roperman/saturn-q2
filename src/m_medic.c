/*
** The medic: Quake 2's game/m_medic.c, with the animations baked into
** MEDIC.MDL (stand, run, pain1-2, death, attack). It shoots a blaster at
** you from a distance (two bolts, then most times twelve more from its
** hyperblaster), and when a monster lies dead in sight it goes to it
** instead and raises it with its cable (the body spawned afresh where it
** lies, after you at once: g_main.c g_monster_revive). Left out: its walk,
** duck, idle and search sounds. (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         medic_run(g_ent *self);
static void         medic_dead(g_ent *self);
static void         medic_continue(g_ent *self);
static void         medic_fire_blaster(g_ent *self);
static void         medic_hook_launch(g_ent *self);
static void         medic_cable_attack(g_ent *self);
static void         medic_hook_retract(g_ent *self);
static void         medic_ai_run(g_ent *self, s32 dist);

#define S0              { ai_stand, 0, NULL }
#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
#define FB              { ai_charge, 0, medic_fire_blaster }
#define CA              { ai_move, 0, medic_cable_attack }
static const mframe_t medic_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      medic_move_stand = { "stand", 0, 89, medic_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t medic_frames_run[] = {
    { medic_ai_run, 18, NULL }, { medic_ai_run, 22, NULL }, { medic_ai_run, 25, NULL },
    { medic_ai_run, 23, NULL }, { medic_ai_run, 24, NULL }, { medic_ai_run, 35, NULL },
};
static mmove_t      medic_move_run = { "run", 0, 5, medic_frames_run, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t medic_frames_pain1[] = { M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      medic_move_pain1 = { "pain1", 0, 7, medic_frames_pain1, medic_run, 0, 0, 0, 0, 0, 0 };
static const mframe_t medic_frames_pain2[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      medic_move_pain2 = { "pain2", 0, 14, medic_frames_pain2, medic_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t medic_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      medic_move_death = { "death", 0, 29, medic_frames_death, medic_dead, 0, 0, 0, 0, 0, 0 };

/* "attack" is attack1-attack60: the blaster (1-14), the hyperblaster (15-30), the cable (33-60) */
static const mframe_t medic_frames_attack_blaster[] = {
    C0, { ai_charge, 5, NULL }, { ai_charge, 5, NULL }, { ai_charge, 3, NULL }, { ai_charge, 2, NULL }, C0, C0, C0,
    FB, C0, C0, FB, C0, C0,
};
static mmove_t      medic_move_attack_blaster = { "attack", 0, 13, medic_frames_attack_blaster, medic_continue, 0, 0, 0, 0, 0, 0 };

static const mframe_t medic_frames_attack_hyper[] = { C0, C0, C0, C0, FB, FB, FB, FB, FB, FB, FB, FB, FB, FB, FB, FB };
static mmove_t      medic_move_attack_hyper = { "attack", 14, 29, medic_frames_attack_hyper, medic_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t medic_frames_attack_cable[] = {
    { ai_move, 2, NULL }, { ai_move, 3, NULL }, { ai_move, 5, NULL }, { ai_move, 4, NULL }, { ai_charge, 5, NULL },
    { ai_charge, 5, NULL }, { ai_charge, 6, NULL }, { ai_charge, 4, NULL }, C0, { ai_move, 0, medic_hook_launch },
    CA, CA, CA, CA, CA, CA, CA, CA, CA, { ai_move, -15, medic_hook_retract },
    { ai_move, -2, NULL }, { ai_move, -1, NULL }, { ai_move, -3, NULL }, { ai_move, -2, NULL }, M0, { ai_move, 1, NULL },
    { ai_move, 1, NULL }, { ai_move, 1, NULL },
};
static mmove_t      medic_move_attack_cable = { "attack", 32, 59, medic_frames_attack_cable, medic_run, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &medic_move_stand, &medic_move_run, &medic_move_pain1, &medic_move_pain2, &medic_move_death,
    &medic_move_attack_blaster, &medic_move_attack_hyper, &medic_move_attack_cable,
};

/* where a shot (or the cable) starts: the medic's origin, so far forward, right and up */
static void         project(const g_ent *self, const s32 *ofs, s32 *start)
{
    s32             fx = fcos(self->yaw), fy = fsin(self->yaw);

    start[0] = self->origin[0] + fmul(fx, ofs[0]) + fmul(fy, ofs[1]);
    start[1] = self->origin[1] + fmul(fy, ofs[0]) - fmul(fx, ofs[1]);
    start[2] = self->origin[2] + ofs[2];
}

static void         medic_stand(g_ent *self)
{
    self->move = &medic_move_stand;
}

/* a monster lying dead in sight, within 1024 units, that no other medic's going to: Quake's
   medic_FindDeadMonster (a body: its move's over and it's not been blown to pieces) */
static g_ent        *medic_find_dead(g_ent *self)
{
    int             i, k;

    for (i = 1; i < g_nfull; ++i)
    {
        g_ent   *e = &g_edicts[i];
        s32     v[3];

        if (e == self || e->kind != EK_MONSTER || !e->dead || e->move || !e->takedamage || e->inactive)
            continue;
        if (e->healer && e->healer != self)
            continue;
        if (e->attack_finished > level.time)
            continue;                       /* (a medic gave it up lately: off a ledge, say) */
        for (k = 0; k < 3; ++k)
            v[k] = e->origin[k] - self->origin[k];
        if (vlen(v) > FIX(1024) || !visible(self, e))
            continue;
        return e;
    }
    return NULL;
}

/* its body's gone (blown up, or another medic was first): after you again */
static void         medic_give_up(g_ent *self)
{
    if (self->enemy && self->enemy->healer == self)
        self->enemy->healer = NULL;
    self->aiflags &= ~AI_MEDIC;
    self->enemy = g_player;
    self->lost_n = 0;
    self->lost_best = 0;
}

static void         medic_run(g_ent *self)
{
    if (!(self->aiflags & AI_MEDIC))
    {
        g_ent   *body = medic_find_dead(self);

        if (body)
        {
            body->healer = self;
            self->enemy = body;
            self->aiflags |= AI_MEDIC;
            self->lost_n = 0;               /* (its way to the body: no nearer in 3 s, it gives the body up) */
            self->lost_best = 0;
        }
    }
    self->move = self->aiflags & AI_STAND_GROUND && !(self->aiflags & AI_MEDIC) ? &medic_move_stand : &medic_move_run;
}

/* a run's frame: after you as any monster, or, with a body to raise, straight to it, and the
   cable from 200 units */
static void         medic_ai_run(g_ent *self, s32 dist)
{
    g_ent           *body = self->enemy;
    s32             v[3];

    if (!(self->aiflags & AI_MEDIC))
    {
        ai_run(self, dist);
        return;
    }
    if (!body || body->kind != EK_MONSTER || !body->dead || body->move || body->healer != self)
    {
        medic_give_up(self);
        ai_run(self, dist);
        return;
    }
    v[0] = body->origin[0] - self->origin[0];
    v[1] = body->origin[1] - self->origin[1];
    v[2] = 0;
    if (vlen(v) > FIX(1024))
    {
        medic_give_up(self);
        ai_run(self, dist);
        return;
    }
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);
    if (vlen(v) <= FIX(200) && visible(self, body))
    {
        self->move = &medic_move_attack_cable;
        return;
    }
    /* no nearer to it in 3 s (a ledge between, say: a monster won't drop off one): the body's
       left alone for 30 s by every medic, and it's after you */
    if (!self->lost_best || vlen(v) < self->lost_best - FIX(8))
    {
        self->lost_best = vlen(v);
        self->lost_n = 0;
    }
    else if (++self->lost_n >= 30)
    {
        body->attack_finished = level.time + FIX(30);
        medic_give_up(self);
        ai_run(self, dist);
        return;
    }
    self->goalentity = body;
    M_MoveToGoal(self, dist);               /* (round what's in the way, as after you) */
}

/* a bolt at you (Quake's MZ2_MEDIC_BLASTER_1: forward, right, up), 2 of damage */
static void         medic_fire_blaster(g_ent *self)
{
    static const s32 ofs[3] = { FIX(12.1), FIX(5.4), FIX(16.5) };
    s32             start[3], aim[3], len;
    int             k;

    if (self->enemy != g_player)
        return;                             /* (its enemy's a body: not a shot) */
    project(self, ofs, start);
    for (k = 0; k < 3; ++k)
        aim[k] = self->enemy->origin[k] - start[k];
    aim[2] += self->enemy->viewheight;
    len = vlen(aim);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    g_fire_blaster(self, start, aim, 2, FIX(1000));
    g_muzzle_flash(start, 12, 10, 3);
    s_play(SND_MED_BLASTER, start, ATTN_NORM);
}

/* the blaster's two shots over: the hyperblaster's twelve, most times, if it still sees you */
static void         medic_continue(g_ent *self)
{
    if (self->enemy && visible(self, self->enemy) && frandom() <= FIX(0.95))
        self->move = &medic_move_attack_hyper;
    else
        medic_run(self);
}

/* (the port's AI asks for an attack in reach too: Quake's medic has no melee, so the blaster there as well) */
static void         medic_attack(g_ent *self)
{
    self->move = self->aiflags & AI_MEDIC ? &medic_move_attack_cable : &medic_move_attack_blaster;
}

static void         medic_hook_launch(g_ent *self)
{
    s_play(SND_MED_HOOK_LAUNCH, self->origin, ATTN_NORM);
}

/* the cable's frames (attack43-51): on the body from the medic's hand (Quake's offsets a frame),
   within 256 units and in the clear; it bites on the first, heals, and on the eighth the body's
   up (g_monster_revive) */
static void         medic_cable_attack(g_ent *self)
{
    static const s32 ofs[9][3] = {
        { FIX(45.0), FIX(-9.2), FIX(15.5) }, { FIX(48.4), FIX(-9.7), FIX(15.2) }, { FIX(47.8), FIX(-9.8), FIX(15.8) },
        { FIX(47.3), FIX(-9.3), FIX(14.3) }, { FIX(45.4), FIX(-10.1), FIX(13.1) }, { FIX(41.9), FIX(-12.7), FIX(12.0) },
        { FIX(37.8), FIX(-15.8), FIX(11.2) }, { FIX(34.3), FIX(-18.4), FIX(10.7) }, { FIX(32.7), FIX(-19.5), FIX(10.4) },
    };
    static const s32 zero[3] = { 0, 0, 0 };
    g_ent           *body = self->enemy, *hit = NULL;
    s32             start[3], v[3];
    q_trace         t;
    int             f = iclamp(self->frame - medic_move_attack_cable.first - 10, 0, 8), k;

    if (!(self->aiflags & AI_MEDIC) || !body || body->kind != EK_MONSTER || !body->dead || body->move
        || body->healer != self)
        return;
    project(self, ofs[f], start);
    for (k = 0; k < 3; ++k)
        v[k] = body->origin[k] - start[k];
    if (vlen(v) > FIX(256))
        return;
    t = g_trace(start, zero, zero, body->origin, self, MASK_SHOT, &hit);
    if (t.fraction != FIX(1) && hit != body)
        return;
    if (f == 0)
        s_play(SND_MED_HOOK_HIT, self->origin, ATTN_NORM);
    else if (f == 1)
        s_play(SND_MED_HOOK_HEAL, self->origin, ATTN_NORM);
    else if (f == 7)
    {
        medic_give_up(self);                /* (after you again once the cable's in) */
        g_monster_revive(body);
        return;
    }
    fx_beam(start, body->origin);
}

static void         medic_hook_retract(g_ent *self)
{
    s_play(SND_MED_HOOK_RETRACT, self->origin, ATTN_NORM);
    if (self->aiflags & AI_MEDIC)
        medic_give_up(self);                /* (it never bit: whatever's the matter, after you) */
}

static void         medic_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    if (self->aiflags & AI_MEDIC)
        medic_give_up(self);                /* (Quake's: hurt, it drops the body for you) */
    if (rng() & 1)
    {
        s_play(SND_MED_PAIN1, self->origin, ATTN_NORM);
        self->move = &medic_move_pain1;
    }
    else
    {
        s_play(SND_MED_PAIN2, self->origin, ATTN_NORM);
        self->move = &medic_move_pain2;
    }
}

static void         medic_sight(g_ent *self)
{
    s_play(SND_MED_SIGHT, self->origin, ATTN_NORM);
}

static void         medic_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);                /* (a body: shot or blown up again, it's gibbed; nothing walks into it) */
    self->move = NULL;
}

static void         medic_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    if (self->aiflags & AI_MEDIC)
        medic_give_up(self);
    self->dead = true;
    s_play(SND_MED_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &medic_move_death;
}

void                SP_monster_medic(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(24);
    self->mins[2] = -FIX(24);
    self->maxs[0] = self->maxs[1] = FIX(24);
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 300;
    self->gib_health = -130;
    self->stand = medic_stand;
    self->run = medic_run;
    self->attack = medic_attack;
    self->pain = medic_pain;
    self->die = medic_die;
    self->sight = medic_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_medic;
#endif
