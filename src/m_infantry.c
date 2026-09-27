/*
** The infantry: Quake 2's game/m_infantry.c, with the animations baked into
** INFANTRY.MDL (stand, run, pain1, pain2, death1, death2, attack1, attack2).
** A machinegun burst (it holds the firing frame for 1-2 seconds) and, up
** close, a punch. One of its deaths sprays bullets as it goes down.
*/
#include "game.h"

static void         infantry_run(g_ent *self);
static void         infantry_dead(g_ent *self);
static void         infantry_cock_gun(g_ent *self);
static void         infantry_fire(g_ent *self);
static void         infantry_smack(g_ent *self);
static void         InfantryMachineGun(g_ent *self);

#define S0              { ai_stand, 0, NULL }
static const mframe_t infantry_frames_stand[] = {
    S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0, S0,
};
static mmove_t      infantry_move_stand = { "stand", 0, 21, infantry_frames_stand, NULL, 0, 0 };

static const mframe_t infantry_frames_run[] = {
    { ai_run, 10, NULL }, { ai_run, 20, NULL }, { ai_run, 5, NULL }, { ai_run, 7, NULL },
    { ai_run, 30, NULL }, { ai_run, 35, NULL }, { ai_run, 2, NULL }, { ai_run, 6, NULL },
};
static mmove_t      infantry_move_run = { "run", 0, 7, infantry_frames_run, NULL, 0, 0 };

static const mframe_t infantry_frames_pain1[] = {
    { ai_move, -3, NULL }, { ai_move, -2, NULL }, { ai_move, -1, NULL }, { ai_move, -2, NULL }, { ai_move, -1, NULL },
    { ai_move, 1, NULL }, { ai_move, -1, NULL }, { ai_move, 1, NULL }, { ai_move, 6, NULL }, { ai_move, 2, NULL },
};
static mmove_t      infantry_move_pain1 = { "pain1", 0, 9, infantry_frames_pain1, infantry_run, 0, 0 };

static const mframe_t infantry_frames_pain2[] = {
    { ai_move, -3, NULL }, { ai_move, -3, NULL }, { ai_move, 0, NULL }, { ai_move, -1, NULL }, { ai_move, -2, NULL },
    { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 2, NULL }, { ai_move, 5, NULL }, { ai_move, 2, NULL },
};
static mmove_t      infantry_move_pain2 = { "pain2", 0, 9, infantry_frames_pain2, infantry_run, 0, 0 };

static const mframe_t infantry_frames_death1[] = {
    { ai_move, -4, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, -1, NULL }, { ai_move, -4, NULL },
    { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, 0, NULL }, { ai_move, -1, NULL }, { ai_move, 3, NULL },
    { ai_move, 1, NULL }, { ai_move, 1, NULL }, { ai_move, -2, NULL }, { ai_move, 2, NULL }, { ai_move, 2, NULL },
    { ai_move, 9, NULL }, { ai_move, 9, NULL }, { ai_move, 5, NULL }, { ai_move, -3, NULL }, { ai_move, -3, NULL },
};
static mmove_t      infantry_move_death1 = { "death1", 0, 19, infantry_frames_death1, infantry_dead, 0, 0 };

/* off with his head: he sprays the room as he goes */
#define MG              InfantryMachineGun
static const mframe_t infantry_frames_death2[] = {
    { ai_move, 0, NULL }, { ai_move, 1, NULL }, { ai_move, 5, NULL }, { ai_move, -1, NULL }, { ai_move, 0, NULL },
    { ai_move, 1, NULL }, { ai_move, 1, NULL }, { ai_move, 4, NULL }, { ai_move, 3, NULL }, { ai_move, 0, NULL },
    { ai_move, -2, MG }, { ai_move, -2, MG }, { ai_move, -3, MG }, { ai_move, -1, MG }, { ai_move, -2, MG },
    { ai_move, 0, MG }, { ai_move, 2, MG }, { ai_move, 2, MG }, { ai_move, 3, MG }, { ai_move, -10, MG },
    { ai_move, -7, MG }, { ai_move, -8, MG }, { ai_move, -6, NULL }, { ai_move, 4, NULL }, { ai_move, 0, NULL },
};
static mmove_t      infantry_move_death2 = { "death2", 0, 24, infantry_frames_death2, infantry_dead, 0, 0 };

static const mframe_t infantry_frames_attack1[] = {
    { ai_charge, 4, NULL }, { ai_charge, -1, NULL }, { ai_charge, -1, NULL }, { ai_charge, 0, infantry_cock_gun },
    { ai_charge, -1, NULL }, { ai_charge, 1, NULL }, { ai_charge, 1, NULL }, { ai_charge, 2, NULL },
    { ai_charge, -2, NULL }, { ai_charge, -3, NULL }, { ai_charge, 1, infantry_fire }, { ai_charge, 5, NULL },
    { ai_charge, -1, NULL }, { ai_charge, -2, NULL }, { ai_charge, -3, NULL },
};
static mmove_t      infantry_move_attack1 = { "attack1", 0, 14, infantry_frames_attack1, infantry_run, 0, 0 };

static const mframe_t infantry_frames_attack2[] = {
    { ai_charge, 3, NULL }, { ai_charge, 6, NULL }, { ai_charge, 0, NULL }, { ai_charge, 8, NULL },
    { ai_charge, 5, NULL }, { ai_charge, 8, infantry_smack }, { ai_charge, 6, NULL }, { ai_charge, 3, NULL },
};
static mmove_t      infantry_move_attack2 = { "attack2", 0, 7, infantry_frames_attack2, infantry_run, 0, 0 };

static mmove_t      *all_moves[] = {
    &infantry_move_stand, &infantry_move_run, &infantry_move_pain1, &infantry_move_pain2, &infantry_move_death1,
    &infantry_move_death2, &infantry_move_attack1, &infantry_move_attack2,
};

static void         infantry_stand(g_ent *self)
{
    self->move = &infantry_move_stand;
}

static void         infantry_run(g_ent *self)
{
    self->move = self->aiflags & AI_STAND_GROUND ? &infantry_move_stand : &infantry_move_run;
}

static void         InfantryMachineGun(g_ent *self)
{
    s32             start[3], fwd[3], right[2], aim[3], len;
    int             k;

    fwd[0] = fcos(self->yaw);
    fwd[1] = fsin(self->yaw);
    fwd[2] = 0;
    right[0] = fwd[1];
    right[1] = -fwd[0];
    start[0] = self->origin[0] + fmul(fwd[0], FIX(26.6)) + fmul(right[0], FIX(7.1));
    start[1] = self->origin[1] + fmul(fwd[1], FIX(26.6)) + fmul(right[1], FIX(7.1));
    start[2] = self->origin[2] + FIX(13.1);
    if (self->move == &infantry_move_attack1 && self->enemy)
    {
        /* at you */
        for (k = 0; k < 3; ++k)
            aim[k] = self->enemy->origin[k] - start[k];
        aim[2] += self->enemy->viewheight;
        len = vlen(aim);
        if (len < FIX(1))
            return;
        for (k = 0; k < 3; ++k)
            aim[k] = fdiv(aim[k], len);
    }
    else
    {
        /* dying: wherever he's facing, sweeping round */
        int a = self->yaw + (int)(crandom() >> 3);

        aim[0] = fcos(a);
        aim[1] = fsin(a);
        aim[2] = crandom() >> 2;
    }
    g_fire_hitscan(self, start, aim, 3, 300, 500, 1);
    g_muzzle_flash(start, 13, 11, 5);
}

static void         infantry_cock_gun(g_ent *self)
{
    self->pausetime = level.time + (int)((rng() & 15) + 10) * FRAMETIME;
}

static void         infantry_fire(g_ent *self)
{
    InfantryMachineGun(self);
    if (level.time >= self->pausetime)
        self->aiflags &= ~AI_HOLD_FRAME;
    else
        self->aiflags |= AI_HOLD_FRAME;
}

/* the punch: Quake's fire_hit, at arm's length */
static void         infantry_smack(g_ent *self)
{
    if (self->enemy && range(self, self->enemy) == RANGE_MELEE && infront(self, self->enemy))
        g_damage(self->enemy, self, 5 + (int)(rng() % 5), self->enemy->origin);
}

static void         infantry_attack(g_ent *self)
{
    self->move = range(self, self->enemy) == RANGE_MELEE ? &infantry_move_attack2 : &infantry_move_attack1;
}

static void         infantry_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum = 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    self->move = rng() & 1 ? &infantry_move_pain1 : &infantry_move_pain2;
}

static void         infantry_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);
    self->solid = false;
    self->move = NULL;
}

static void         infantry_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->aiflags &= ~AI_HOLD_FRAME;
    self->move = rng() & 1 ? &infantry_move_death1 : &infantry_move_death2;
}

void                SP_monster_infantry(g_ent *self)
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
    self->health = 100;
    self->gib_health = -40;
    self->stand = infantry_stand;
    self->run = infantry_run;
    self->attack = infantry_attack;
    self->pain = infantry_pain;
    self->die = infantry_die;
    self->sight = NULL;
    monster_start(self);
}
