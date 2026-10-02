/*
** The berserker: Quake 2's game/m_berserk.c, with the animations baked into
** BERSERK.MDL (stand, run, attack, pain1, pain2, death1, death2). It runs at
** you and swings: the spike (an upward stab, 15 to 20) or the club (5 to
** 10); it has nothing for a distance (Quake's has no attack, only melee).
** Left out to fit: its fidget and idle sound, walk and search sound (as the
** gunner's). (Its gibs: g_main.c's g_gib.)
*/
#include "game.h"

static void         berserk_run(g_ent *self);
static void         berserk_dead(g_ent *self);
static void         berserk_swing(g_ent *self);
static void         berserk_attack_spike(g_ent *self);
static void         berserk_attack_club(g_ent *self);

#define S0              { ai_stand, 0, NULL }
static const mframe_t berserk_frames_stand[] = { S0, S0, S0, S0, S0 };
static mmove_t      berserk_move_stand = { "stand", 0, 4, berserk_frames_stand, NULL, 0, 0 };

static const mframe_t berserk_frames_run[] = {
    { ai_run, 21, NULL }, { ai_run, 11, NULL }, { ai_run, 21, NULL },
    { ai_run, 25, NULL }, { ai_run, 18, NULL }, { ai_run, 19, NULL },
};
static mmove_t      berserk_move_run = { "run", 0, 5, berserk_frames_run, NULL, 0, 0 };

/* "attack" is att_c1-att_c20: the spike is its first 8, the club the next 12 */
#define C0              { ai_charge, 0, NULL }
static const mframe_t berserk_frames_attack_spike[] = {
    C0, C0, { ai_charge, 0, berserk_swing }, { ai_charge, 0, berserk_attack_spike }, C0, C0, C0, C0,
};
static mmove_t      berserk_move_attack_spike = { "attack", 0, 7, berserk_frames_attack_spike, berserk_run, 0, 0 };

static const mframe_t berserk_frames_attack_club[] = {
    C0, C0, C0, C0, { ai_charge, 0, berserk_swing }, C0, C0, C0, { ai_charge, 0, berserk_attack_club }, C0, C0, C0,
};
static mmove_t      berserk_move_attack_club = { "attack", 8, 19, berserk_frames_attack_club, berserk_run, 0, 0 };

#define M0              { ai_move, 0, NULL }
static const mframe_t berserk_frames_pain1[] = { M0, M0, M0, M0 };
static mmove_t      berserk_move_pain1 = { "pain1", 0, 3, berserk_frames_pain1, berserk_run, 0, 0 };

static const mframe_t berserk_frames_pain2[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      berserk_move_pain2 = { "pain2", 0, 19, berserk_frames_pain2, berserk_run, 0, 0 };

static const mframe_t berserk_frames_death1[] = { M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      berserk_move_death1 = { "death1", 0, 12, berserk_frames_death1, berserk_dead, 0, 0 };

static const mframe_t berserk_frames_death2[] = { M0, M0, M0, M0, M0, M0, M0, M0 };
static mmove_t      berserk_move_death2 = { "death2", 0, 7, berserk_frames_death2, berserk_dead, 0, 0 };

static mmove_t      *all_moves[] = {
    &berserk_move_stand, &berserk_move_run, &berserk_move_attack_spike, &berserk_move_attack_club,
    &berserk_move_pain1, &berserk_move_pain2, &berserk_move_death1, &berserk_move_death2,
};

static void         berserk_stand(g_ent *self)
{
    self->move = &berserk_move_stand;
}

static void         berserk_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &berserk_move_stand : &berserk_move_run;
}

static void         berserk_swing(g_ent *self)
{
    s_play(SND_BER_PUNCH, self->origin, ATTN_NORM);
}

/* Quake's fire_hit: the blow lands if you're still in reach and in front */
static void         berserk_hit(g_ent *self, int damage)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
        g_damage(self->enemy, self, damage, self->enemy->origin);
}

static void         berserk_attack_spike(g_ent *self)
{
    berserk_hit(self, 15 + (int)(rng() % 6));
}

static void         berserk_attack_club(g_ent *self)
{
    berserk_hit(self, 5 + (int)(rng() % 6));
}

/* (the port's AI asks for an attack in reach, and now and then further off: the berserker only
   swings in reach, and otherwise keeps running at you, as Quake's, which has no attack) */
static void         berserk_attack(g_ent *self)
{
    if (range(self, self->enemy) != RANGE_MELEE)
        return;
    self->move = rng() & 1 ? &berserk_move_attack_spike : &berserk_move_attack_club;
}

static void         berserk_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    s_play(SND_BER_PAIN, self->origin, ATTN_NORM);
    self->move = damage < 20 || frandom() < FIX(0.5) ? &berserk_move_pain1 : &berserk_move_pain2;
}

static void         berserk_sight(g_ent *self)
{
    s_play(SND_BER_SIGHT, self->origin, ATTN_NORM);
}

static void         berserk_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);
    self->solid = false;
    self->move = NULL;
}

static void         berserk_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_BER_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = damage >= 50 ? &berserk_move_death1 : &berserk_move_death2;
}

void                SP_monster_berserk(g_ent *self)
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
    self->health = 240;
    self->gib_health = -60;
    self->stand = berserk_stand;
    self->run = berserk_run;
    self->attack = berserk_attack;
    self->pain = berserk_pain;
    self->die = berserk_die;
    self->sight = berserk_sight;
    monster_start(self);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_berserk;
#endif
