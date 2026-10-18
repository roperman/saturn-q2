/*
** The barracuda shark (bunk1, fact1, hangar1, train): Quake 2's
** game/m_flipper.c, with the animations baked into FLIPPER.MDL (stand,
** run, pain1-2, attack, death). It swims (FL_SWIM: g_ai.c SV_flystep in
** the water, never out of it), 50 health, and bites you within reach (5 of
** damage, twice a lunge). Its horizontal swim is its stand (Quake's one
** frame of it) and the start of its run, the vertical its run. Left out: its
** search and idle calls, and its tail animation (Quake never plays it).
*/
#include "game.h"

static void         flipper_start_run(g_ent *self);
static void         flipper_run_vert(g_ent *self);
static void         flipper_run_loop(g_ent *self);
static void         flipper_preattack(g_ent *self);
static void         flipper_bite(g_ent *self);
static void         flipper_dead(g_ent *self);

#define M0              { ai_move, 0, NULL }
#define C0              { ai_charge, 0, NULL }
#define R8              { ai_run, 8, NULL }
#define R24             { ai_run, 24, NULL }
static const mframe_t flipper_frames_stand[] = { { ai_stand, 0, NULL } };
static mmove_t      flipper_move_stand = { "stand", 0, 0, flipper_frames_stand, NULL, 0, 0, 0, 0, 0, 0 };

/* its run: the horizontal swim's first five frames, the vertical's first six, then the vertical
   swim looped (Quake's start_run, run_start, run_loop) */
static const mframe_t flipper_frames_start_run[] = { R8, R8, R8, R8, R8 };
static mmove_t      flipper_move_start_run = { "stand", 0, 4, flipper_frames_start_run, flipper_run_vert,
                                               0, 0, 0, 0, 0, 0 };
static const mframe_t flipper_frames_run_start[] = { R8, R8, R8, R8, R8, R8 };
static mmove_t      flipper_move_run_start = { "run", 0, 5, flipper_frames_run_start, flipper_run_loop,
                                               0, 0, 0, 0, 0, 0 };
static const mframe_t flipper_frames_run_loop[] = {
    R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24, R24,
    R24, R24,
};
static mmove_t      flipper_move_run_loop = { "run", 5, 28, flipper_frames_run_loop, NULL, 0, 0, 0, 0, 0, 0 };

static const mframe_t flipper_frames_pain[] = { M0, M0, M0, M0, M0 };
static mmove_t      flipper_move_pain1 = { "pain2", 0, 4, flipper_frames_pain, flipper_start_run, 0, 0, 0, 0, 0, 0 };
static mmove_t      flipper_move_pain2 = { "pain1", 0, 4, flipper_frames_pain, flipper_start_run, 0, 0, 0, 0, 0, 0 };

/* "attack" is flpbit01-flpbit20: the lunge, its bites on the 14th and the 19th */
static const mframe_t flipper_frames_attack[] = {
    { ai_charge, 0, flipper_preattack }, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, C0, { ai_charge, 0, flipper_bite },
    C0, C0, C0, C0, { ai_charge, 0, flipper_bite }, C0,
};
static mmove_t      flipper_move_attack = { "attack", 0, 19, flipper_frames_attack, flipper_start_run, 0, 0, 0, 0, 0, 0 };

static const mframe_t flipper_frames_death[] = {
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
    M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0, M0,
};
static mmove_t      flipper_move_death = { "death", 0, 55, flipper_frames_death, flipper_dead, 0, 0, 0, 0, 0, 0 };

static mmove_t      *all_moves[] = {
    &flipper_move_stand, &flipper_move_start_run, &flipper_move_run_start, &flipper_move_run_loop,
    &flipper_move_pain1, &flipper_move_pain2, &flipper_move_attack, &flipper_move_death,
};

static void         flipper_stand(g_ent *self)
{
    self->move = &flipper_move_stand;
}

static void         flipper_start_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &flipper_move_stand : &flipper_move_start_run;
}

static void         flipper_run_vert(g_ent *self)
{
    self->move = &flipper_move_run_start;
}

static void         flipper_run_loop(g_ent *self)
{
    self->move = &flipper_move_run_loop;
}

static void         flipper_preattack(g_ent *self)
{
    s_play(SND_FLP_CHOMP, self->origin, ATTN_NORM);
}

/* Quake's fire_hit: the bite lands if you're still in reach and in front */
static void         flipper_bite(g_ent *self)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
        g_damage(self->enemy, self, 5, self->enemy->origin);
}

/* (the port's AI asks for an attack in reach, and now and then further off: the shark only bites
   in reach, and otherwise keeps after you, as Quake's, which has no other attack) */
static void         flipper_attack(g_ent *self)
{
    if (!self->enemy || range(self, self->enemy) != RANGE_MELEE)
        return;
    self->move = &flipper_move_attack;
}

static void         flipper_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    if (rng() & 1)
    {
        s_play(SND_FLP_PAIN1, self->origin, ATTN_NORM);
        self->move = &flipper_move_pain1;
    }
    else
    {
        s_play(SND_FLP_PAIN2, self->origin, ATTN_NORM);
        self->move = &flipper_move_pain2;
    }
}

static void         flipper_sight(g_ent *self)
{
    s_play(SND_FLP_SIGHT, self->origin, ATTN_NORM);
}

static void         flipper_dead(g_ent *self)
{
    self->mins[2] = -FIX(24);
    self->maxs[2] = -FIX(8);
    self->move = NULL;                      /* (a body, where it was) */
}

static void         flipper_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    s_play(SND_FLP_DEATH, self->origin, ATTN_NORM);
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = &flipper_move_death;
}

void                SP_monster_flipper(g_ent *self)
{
    M_ResolveMoves(self, all_moves, sizeof(all_moves) / sizeof(all_moves[0]));
    self->mins[0] = self->mins[1] = -FIX(16);
    self->mins[2] = 0;
    self->maxs[0] = self->maxs[1] = FIX(16);
    self->maxs[2] = FIX(32);
    self->skinnum = 0;
    self->health = 50;
    self->gib_health = -30;
    self->stand = flipper_stand;
    self->run = flipper_start_run;
    self->attack = flipper_attack;
    self->pain = flipper_pain;
    self->die = flipper_die;
    self->sight = flipper_sight;
    self->flags |= FL_SWIM;                 /* (before monster_start: it isn't dropped to the floor) */
    monster_start(self);
    self->yaw_speed = ANG(10);              /* (Quake's swimmonster_start: half a walker's) */
    self->viewheight = FIX(10);
}

#ifdef OVERLAY
/* (loaded per level: build.sh build_overlays) the loader finds its spawn here, first */
__attribute__((section(".ovlhead"), used)) void (*const ovl_spawn)(g_ent *self) = SP_monster_flipper;
#endif
