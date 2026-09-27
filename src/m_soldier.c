/*
** The soldier (light, standard and SS): Quake 2's game/m_soldier.c, with the
** animations baked into SOLDIER.MDL (stand1, run, attack1, pain1, pain2,
** death1). Light soldiers fire a blaster, standard ones a shotgun, SS a
** machinegun burst.
*/
#include "game.h"

static void         soldier_stand(g_ent *self);
static void         soldier_run(g_ent *self);
static void         soldier_dead(g_ent *self);
static void         soldier_fire1(g_ent *self);
static void         soldier_attack1_refire1(g_ent *self);
static void         soldier_attack1_refire2(g_ent *self);

#define STAND30         { ai_stand, 0, NULL }
static const mframe_t soldier_frames_stand1[] = {
    STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30,
    STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30,
    STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30, STAND30,
};
static mmove_t      soldier_move_stand1 = { "stand", 0, 29, soldier_frames_stand1, soldier_stand, 0, 0 };

static const mframe_t soldier_frames_start_run[] = { { ai_run, 7, NULL }, { ai_run, 5, NULL } };
static mmove_t      soldier_move_start_run = { "run", 0, 1, soldier_frames_start_run, soldier_run, 0, 0 };

static const mframe_t soldier_frames_run[] = {
    { ai_run, 10, NULL }, { ai_run, 11, NULL }, { ai_run, 11, NULL },
    { ai_run, 16, NULL }, { ai_run, 10, NULL }, { ai_run, 15, NULL },
};
static mmove_t      soldier_move_run = { "run", 2, 7, soldier_frames_run, NULL, 0, 0 };

static const mframe_t soldier_frames_pain1[] = {
    { ai_move, -3, NULL }, { ai_move, 4, NULL }, { ai_move, 1, NULL }, { ai_move, 1, NULL }, { ai_move, 0, NULL },
};
static mmove_t      soldier_move_pain1 = { "pain1", 0, 4, soldier_frames_pain1, soldier_run, 0, 0 };

static const mframe_t soldier_frames_pain2[] = {
    { ai_move, -13, NULL }, { ai_move, -1, NULL }, { ai_move, 2, NULL }, { ai_move, 4, NULL },
    { ai_move, 2, NULL }, { ai_move, 3, NULL }, { ai_move, 2, NULL },
};
static mmove_t      soldier_move_pain2 = { "pain2", 0, 6, soldier_frames_pain2, soldier_run, 0, 0 };

static const mframe_t soldier_frames_attack1[] = {
    { ai_charge, 0, NULL }, { ai_charge, 0, NULL }, { ai_charge, 0, soldier_fire1 },
    { ai_charge, 0, NULL }, { ai_charge, 0, NULL }, { ai_charge, 0, soldier_attack1_refire1 },
    { ai_charge, 0, NULL }, { ai_charge, 0, NULL }, { ai_charge, 0, soldier_attack1_refire2 },
    { ai_charge, 0, NULL }, { ai_charge, 0, NULL }, { ai_charge, 0, NULL },
};
static mmove_t      soldier_move_attack1 = { "attack", 0, 11, soldier_frames_attack1, soldier_run, 0, 0 };

#define DEATH0          { ai_move, 0, NULL }
static const mframe_t soldier_frames_death1[] = {
    { ai_move, 0, NULL }, { ai_move, -10, NULL }, { ai_move, -10, NULL }, { ai_move, -10, NULL }, { ai_move, -5, NULL },
    DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0,
    DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0,
    DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0, DEATH0,
};
static mmove_t      soldier_move_death1 = { "death", 0, 35, soldier_frames_death1, soldier_dead, 0, 0 };

static mmove_t      *all_moves[] = {
    &soldier_move_stand1, &soldier_move_start_run, &soldier_move_run, &soldier_move_pain1, &soldier_move_pain2,
    &soldier_move_attack1, &soldier_move_death1,
};

static void         set_move(g_ent *self, const mmove_t *m)
{
    self->move = m;
}

static void         soldier_stand(g_ent *self)
{
    set_move(self, &soldier_move_stand1);
}

static void         soldier_run(g_ent *self)
{
    if (self->aiflags & AI_STAND_GROUND)
    {
        set_move(self, &soldier_move_stand1);
        return;
    }
    set_move(self, self->move == &soldier_move_start_run ? &soldier_move_run : &soldier_move_start_run);
}

/* where the gun is (Quake's monster_flash_offset for the soldier, x1.2), and where to aim */
static void         soldier_fire(g_ent *self)
{
    s32             start[3], aim[3], fwd[2], right[2], end[3], len;
    int             k;

    if (!self->enemy)
        return;
    fwd[0] = fcos(self->yaw);
    fwd[1] = fsin(self->yaw);
    right[0] = fwd[1];
    right[1] = -fwd[0];
    start[0] = self->origin[0] + fmul(fwd[0], FIX(12.7)) + fmul(right[0], FIX(9.2));
    start[1] = self->origin[1] + fmul(fwd[1], FIX(12.7)) + fmul(right[1], FIX(9.2));
    start[2] = self->origin[2] + FIX(9.4);
    for (k = 0; k < 3; ++k)
        end[k] = self->enemy->origin[k];
    end[2] += self->enemy->viewheight;
    for (k = 0; k < 3; ++k)
        aim[k] = end[k] - start[k];
    len = vlen(aim);
    if (len < FIX(1))
        return;
    for (k = 0; k < 3; ++k)
        aim[k] = fdiv(aim[k], len);
    if (self->skinnum <= 1)
    {
        /* not quite straight at you: Quake spreads it up to 1000 across and 500 up at 8192 */
        s32 r = fmul(crandom(), FIX(0.122)), u = fmul(crandom(), FIX(0.061));

        aim[0] += fmul(aim[1], r);
        aim[1] -= fmul(aim[0], r);
        aim[2] += u;
        g_fire_blaster(self, start, aim, 5, FIX(600));
        g_muzzle_flash(start, 12, 10, 3);
    }
    else if (self->skinnum <= 3)
    {
        g_fire_hitscan(self, start, aim, 2, 1000, 500, 12);
        g_muzzle_flash(start, 13, 10, 4);
    }
    else
    {
        /* SS: a burst, holding this frame for a few ticks */
        if (!(self->aiflags & AI_HOLD_FRAME))
            self->pausetime = level.time + (3 + (int)(rng() % 8)) * FRAMETIME;
        g_fire_hitscan(self, start, aim, 2, 300, 500, 1);
        g_muzzle_flash(start, 13, 11, 5);
        if (level.time >= self->pausetime)
            self->aiflags &= ~AI_HOLD_FRAME;
        else
            self->aiflags |= AI_HOLD_FRAME;
    }
}

static void         soldier_fire1(g_ent *self)
{
    soldier_fire(self);
}

static void         soldier_attack1_refire1(g_ent *self)
{
    if (self->skinnum > 1 || !self->enemy || self->enemy->health <= 0)
        return;
    self->nextframe = range(self, self->enemy) == RANGE_MELEE ? soldier_move_attack1.first + 1
                                                               : soldier_move_attack1.first + 9;
}

static void         soldier_attack1_refire2(g_ent *self)
{
    if (self->skinnum < 2 || !self->enemy || self->enemy->health <= 0)
        return;
    if (range(self, self->enemy) == RANGE_MELEE)
        self->nextframe = soldier_move_attack1.first + 1;
}

static void         soldier_attack(g_ent *self)
{
    set_move(self, &soldier_move_attack1);
}

static void         soldier_pain(g_ent *self, g_ent *other, int damage)
{
    if (self->health < self->max_health / 2)
        self->skinnum |= 1;
    if (level.time < self->pain_debounce)
        return;
    self->pain_debounce = level.time + FIX(3);
    set_move(self, frandom() < FIX(0.5) ? &soldier_move_pain1 : &soldier_move_pain2);
}

static void         soldier_dead(g_ent *self)
{
    self->maxs[2] = -FIX(8);
    self->solid = false;
    self->move = NULL;                      /* no more thinking */
}

static void         soldier_die(g_ent *self, g_ent *attacker, int damage, const s32 *point)
{
    if (self->dead)
        return;
    self->dead = true;
    self->skinnum |= 1;
    self->aiflags &= ~AI_HOLD_FRAME;
    set_move(self, &soldier_move_death1);
}

void                SP_monster_x_soldier(g_ent *self, int skin)
{
    unsigned        i;

    /* the moves' model frames, from the model's animations */
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
    self->skinnum = skin;
    self->health = skin == 0 ? 20 : skin == 2 ? 30 : 40;
    self->gib_health = -30;
    self->stand = soldier_stand;
    self->run = soldier_run;
    self->attack = soldier_attack;
    self->pain = soldier_pain;
    self->die = soldier_die;
    self->sight = NULL;
    monster_start(self);
}
