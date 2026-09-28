/*
** Monster AI and movement: Quake 2's game/g_ai.c and game/m_move.c, in fixed
** point, for walking monsters against a single player. Left out: the player
** trail (a monster that loses sight goes to where it last saw you, then
** looks round), combat points, flying and swimming, coop.
*/
#include "game.h"

#define STEPSIZE        FIX(18)
#define MELEE_DISTANCE  FIX(80)
#define ANG(d)          ((int)((d) * 65536 / 360))

static bool         enemy_vis, enemy_infront;
static int          enemy_range, enemy_yaw;

/* ---- m_move.c ---- */

/* false if any part of the bottom is off an edge that isn't a staircase */
static bool         M_CheckBottom(g_ent *ent)
{
    static const s32 zero[3] = { 0, 0, 0 };
    s32             mins[3], maxs[3], start[3], stop[3], mid, bottom;
    int             x, y, k;
    q_trace         t;

    for (k = 0; k < 3; ++k)
    {
        mins[k] = ent->origin[k] + ent->mins[k];
        maxs[k] = ent->origin[k] + ent->maxs[k];
    }
    /* the quick way: all four corners of the bottom over solid */
    start[2] = mins[2] - FIX(1);
    for (x = 0; x <= 1; ++x)
        for (y = 0; y <= 1; ++y)
        {
            start[0] = x ? maxs[0] : mins[0];
            start[1] = y ? maxs[1] : mins[1];
            if (point_contents(start, 0) != CONTENTS_SOLID)
                goto realcheck;
        }
    return true;
realcheck:
    /* the middle, then each corner, no further than a step below it */
    start[2] = mins[2];
    start[0] = stop[0] = (mins[0] >> 1) + (maxs[0] >> 1);
    start[1] = stop[1] = (mins[1] >> 1) + (maxs[1] >> 1);
    stop[2] = start[2] - 2 * STEPSIZE;
    t = g_trace(start, zero, zero, stop, ent, MASK_MONSTERSOLID, NULL);
    if (t.fraction == FIX(1))
        return false;
    mid = bottom = t.endpos[2];
    for (x = 0; x <= 1; ++x)
        for (y = 0; y <= 1; ++y)
        {
            start[0] = stop[0] = x ? maxs[0] : mins[0];
            start[1] = stop[1] = y ? maxs[1] : mins[1];
            t = g_trace(start, zero, zero, stop, ent, MASK_MONSTERSOLID, NULL);
            if (t.fraction != FIX(1) && t.endpos[2] > bottom)
                bottom = t.endpos[2];
            if (t.fraction == FIX(1) || mid - t.endpos[2] > STEPSIZE)
                return false;
        }
    return true;
}

/* a walking step: up a stair or down one, not off a ledge; false (and no move) if it can't */
static bool         SV_movestep(g_ent *ent, const s32 *move)
{
    s32             oldorg[3], neworg[3], end[3], test[3];
    q_trace         t;
    g_ent           *hit;
    int             k;

    for (k = 0; k < 3; ++k)
    {
        oldorg[k] = ent->origin[k];
        neworg[k] = ent->origin[k] + move[k];
    }
    neworg[2] += STEPSIZE;
    for (k = 0; k < 3; ++k)
        end[k] = neworg[k];
    end[2] -= STEPSIZE * 2;
    t = g_trace(neworg, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID, &hit);
    if (t.allsolid)
        return false;
    if (t.startsolid)
    {
        neworg[2] -= STEPSIZE;
        t = g_trace(neworg, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID, &hit);
        if (t.allsolid || t.startsolid)
            return false;
    }
    /* don't walk into water */
    if (ent->waterlevel == 0)
    {
        test[0] = t.endpos[0];
        test[1] = t.endpos[1];
        test[2] = t.endpos[2] + ent->mins[2] + FIX(1);
        if (point_contents(test, 0) & MASK_WATER)
            return false;
    }
    if (t.fraction == FIX(1))
    {
        if (ent->flags & FL_PARTIALGROUND)
        {
            for (k = 0; k < 3; ++k)
                ent->origin[k] += move[k];
            ent->on_ground = false;
            return true;
        }
        return false;                       /* walked off an edge */
    }
    for (k = 0; k < 3; ++k)
        ent->origin[k] = t.endpos[k];
    if (!M_CheckBottom(ent))
    {
        if (ent->flags & FL_PARTIALGROUND)
            return true;                    /* the floor was mostly pulled out from under it */
        for (k = 0; k < 3; ++k)
            ent->origin[k] = oldorg[k];
        return false;
    }
    ent->flags &= ~FL_PARTIALGROUND;
    ent->on_ground = true;
    return true;
}

void                M_ChangeYaw(g_ent *ent)
{
    int             current = ent->yaw & 0xFFFF, move = (s16)((ent->ideal_yaw - current) & 0xFFFF);

    if (move > ent->yaw_speed)
        move = ent->yaw_speed;
    else if (move < -ent->yaw_speed)
        move = -ent->yaw_speed;
    ent->yaw = (current + move) & 0xFFFF;
}

/* turn to a direction, and walk it if facing it */
static bool         SV_StepDirection(g_ent *ent, int yaw, s32 dist)
{
    s32             move[3], old[3];
    int             k, delta;

    ent->ideal_yaw = yaw & 0xFFFF;
    M_ChangeYaw(ent);
    move[0] = fmul(fcos(yaw), dist);
    move[1] = fmul(fsin(yaw), dist);
    move[2] = 0;
    for (k = 0; k < 3; ++k)
        old[k] = ent->origin[k];
    if (SV_movestep(ent, move))
    {
        delta = (ent->yaw - ent->ideal_yaw) & 0xFFFF;
        if (delta > ANG(45) && delta < ANG(315))
            for (k = 0; k < 3; ++k)
                ent->origin[k] = old[k];    /* not turned far enough: don't take the step */
        return true;
    }
    return false;
}

#define DI_NODIR        (-1)

static void         SV_NewChaseDir(g_ent *actor, const s32 *goal, s32 dist)
{
    int             d[3], tdir, olddir, turnaround;
    s32             dx, dy;

    olddir = (actor->ideal_yaw & 0xFFFF) / ANG(45) * ANG(45);
    turnaround = (olddir + ANG(180)) & 0xFFFF;
    dx = goal[0] - actor->origin[0];
    dy = goal[1] - actor->origin[1];
    d[1] = dx > FIX(10) ? 0 : dx < -FIX(10) ? ANG(180) : DI_NODIR;
    d[2] = dy < -FIX(10) ? ANG(270) : dy > FIX(10) ? ANG(90) : DI_NODIR;
    /* try the direct route */
    if (d[1] != DI_NODIR && d[2] != DI_NODIR)
    {
        if (d[1] == 0)
            tdir = d[2] == ANG(90) ? ANG(45) : ANG(315);
        else
            tdir = d[2] == ANG(90) ? ANG(135) : ANG(215);
        if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
            return;
    }
    /* try the other directions */
    if ((rng() & 3 & 1) || iabs(dy) > iabs(dx))
    {
        tdir = d[1];
        d[1] = d[2];
        d[2] = tdir;
    }
    if (d[1] != DI_NODIR && d[1] != turnaround && SV_StepDirection(actor, d[1], dist))
        return;
    if (d[2] != DI_NODIR && d[2] != turnaround && SV_StepDirection(actor, d[2], dist))
        return;
    /* there's no direct path to the player, so pick another direction */
    if (olddir != DI_NODIR && SV_StepDirection(actor, olddir, dist))
        return;
    if (rng() & 1)
    {
        for (tdir = 0; tdir <= ANG(315); tdir += ANG(45))
            if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
                return;
    }
    else
    {
        for (tdir = ANG(315); tdir >= 0; tdir -= ANG(45))
            if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
                return;
    }
    if (turnaround != DI_NODIR && SV_StepDirection(actor, turnaround, dist))
        return;
    actor->ideal_yaw = olddir;              /* can't move */
    if (!M_CheckBottom(actor))
        actor->flags |= FL_PARTIALGROUND;
}

static bool         SV_CloseEnough(const g_ent *ent, const g_ent *goal, s32 dist)
{
    int             k;

    for (k = 0; k < 3; ++k)
    {
        if (goal->origin[k] + goal->mins[k] > ent->origin[k] + ent->maxs[k] + dist)
            return false;
        if (goal->origin[k] + goal->maxs[k] < ent->origin[k] + ent->mins[k] - dist)
            return false;
    }
    return true;
}

static const s32    *goal_point(const g_ent *ent)
{
    return ent->goalentity == &goal_marker ? ent->goal_pos : ent->goalentity ? ent->goalentity->origin : ent->origin;
}

static void         M_MoveToGoal(g_ent *ent, s32 dist)
{
    if (!ent->on_ground)
        return;
    if (ent->enemy && SV_CloseEnough(ent, ent->enemy, dist))
        return;
    /* bump around... */
    if ((rng() & 3) == 1 || !SV_StepDirection(ent, ent->ideal_yaw, dist))
        SV_NewChaseDir(ent, goal_point(ent), dist);
}

bool                M_walkmove(g_ent *ent, int yaw, s32 dist)
{
    s32             move[3];

    if (!ent->on_ground)
        return false;
    move[0] = fmul(fcos(yaw), dist);
    move[1] = fmul(fsin(yaw), dist);
    move[2] = 0;
    return SV_movestep(ent, move);
}

/* ---- g_ai.c ---- */

void                ai_move(g_ent *self, s32 dist)
{
    M_walkmove(self, self->yaw, dist);
}

/* standing around, looking for the player */
void                ai_stand(g_ent *self, s32 dist)
{
    if (dist)
        M_walkmove(self, self->yaw, dist);
    if (self->aiflags & AI_STAND_GROUND)
    {
        if (self->enemy)
        {
            s32 v[3];
            int k;

            for (k = 0; k < 3; ++k)
                v[k] = self->enemy->origin[k] - self->origin[k];
            self->ideal_yaw = vectoyaw(v);
            if (self->yaw != self->ideal_yaw && self->aiflags & AI_TEMP_STAND_GROUND)
            {
                self->aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
                self->run(self);
            }
            M_ChangeYaw(self);
            ai_checkattack(self, 0);
        }
        else
            FindTarget(self);
        return;
    }
    FindTarget(self);
}

/* turn towards the enemy (and step, if dist): for attacks */
void                ai_charge(g_ent *self, s32 dist)
{
    s32             v[3];
    int             k;

    if (!self->enemy)
        return;
    for (k = 0; k < 3; ++k)
        v[k] = self->enemy->origin[k] - self->origin[k];
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);
    if (dist)
        M_walkmove(self, self->yaw, dist);
}

int                 range(const g_ent *self, const g_ent *other)
{
    s32             v[3], len;
    int             k;

    for (k = 0; k < 3; ++k)
        v[k] = self->origin[k] - other->origin[k];
    len = vlen(v);
    return len < MELEE_DISTANCE ? RANGE_MELEE : len < FIX(500) ? RANGE_NEAR : len < FIX(1000) ? RANGE_MID : RANGE_FAR;
}

/* a clear line from eye to eye */
bool                visible(const g_ent *self, const g_ent *other)
{
    static const s32 zero[3] = { 0, 0, 0 };
    s32             a[3], b[3];
    q_trace         t;
    int             k;

    for (k = 0; k < 3; ++k)
    {
        a[k] = self->origin[k];
        b[k] = other->origin[k];
    }
    a[2] += self->viewheight;
    b[2] += other->viewheight;
    t = g_trace(a, zero, zero, b, self, MASK_OPAQUE, NULL);
    return t.fraction == FIX(1);
}

/* within 72 degrees of straight ahead */
bool                infront(const g_ent *self, const g_ent *other)
{
    s32             v[3], len;
    int             k;

    for (k = 0; k < 3; ++k)
        v[k] = other->origin[k] - self->origin[k];
    len = vlen(v);
    if (len < FIX(1))
        return true;
    return fmul(v[0], fcos(self->yaw)) + fmul(v[1], fsin(self->yaw)) > fmul(len, FIX(0.3));
}

static void         AttackFinished(g_ent *self, s32 t)
{
    self->attack_finished = level.time + t;
}

static void         HuntTarget(g_ent *self)
{
    s32             v[3];
    int             k;

    self->goalentity = self->enemy;
    if (self->aiflags & AI_STAND_GROUND)
        self->stand(self);
    else
        self->run(self);
    for (k = 0; k < 3; ++k)
        v[k] = self->enemy->origin[k] - self->origin[k];
    self->ideal_yaw = vectoyaw(v);
    if (!(self->aiflags & AI_STAND_GROUND))
        AttackFinished(self, FIX(1));
}

static g_ent        *sight_entity;
static int          sight_entity_framenum;
g_ent               *sound_entity;
int                 sound_entity_framenum;

void                FoundTarget(g_ent *self)
{
    int             k;

    if (self->enemy == g_player)
    {
        sight_entity = self;                /* other monsters that see this one wake up too */
        sight_entity_framenum = level.framenum;
        if (self->sight)
            self->sight(self);              /* (its "there you are") */
    }
    self->show_hostile = level.time + FIX(1);
    for (k = 0; k < 3; ++k)
        self->last_sighting[k] = self->enemy->origin[k];
    HuntTarget(self);
}

/* not attacking anything: look for the player (or a monster that's seen them, or a noise) */
bool                FindTarget(g_ent *self)
{
    g_ent           *client;
    bool            heardit = false;
    int             r;

    if (sight_entity_framenum >= level.framenum - 1 && !(self->spawnflags & 1) && sight_entity && sight_entity != self)
    {
        client = sight_entity;
        if (client->enemy == self->enemy)
            return false;
    }
    else if (sound_entity_framenum >= level.framenum - 1 && sound_entity)
    {
        client = sound_entity;
        heardit = true;
    }
    else
        client = level.sight_client;
    if (!client || client->kind == EK_FREE)
        return false;
    if (client == self->enemy)
        return true;
    /* idle: look every third tick (staggered), not every tick - long traces are dear here */
    if (!self->enemy && !heardit && (level.framenum + (int)(self - g_edicts)) % 3)
        return false;
    if (client->kind == EK_MONSTER)
    {
        if (!client->enemy || client->enemy->kind != EK_PLAYER)
            return false;
    }
    if (client->dead)
        return false;
    if (!heardit)
    {
        r = range(self, client);
        if (r == RANGE_FAR)
            return false;
        if (client == g_player && !r_leaf_in_pvs(level_leaf(self->origin)))
            return false;                   /* not in the player's PVS: can't be seen, no trace needed */
        if (!visible(self, client))
            return false;
        if (r == RANGE_NEAR)
        {
            if (client->show_hostile < level.time && !infront(self, client))
                return false;
        }
        else if (r == RANGE_MID)
        {
            if (!infront(self, client))
                return false;
        }
        self->enemy = client->kind == EK_PLAYER ? client : client->enemy;
        self->aiflags &= ~AI_SOUND_TARGET;
    }
    else
    {
        s32 v[3];
        int k;

        if (self->spawnflags & 1)
        {
            if (!visible(self, client))
                return false;
        }
        for (k = 0; k < 3; ++k)
            v[k] = client->origin[k] - self->origin[k];
        if (vlen(v) > FIX(1000))
            return false;                   /* too far to hear */
        self->ideal_yaw = vectoyaw(v);
        M_ChangeYaw(self);
        self->aiflags |= AI_SOUND_TARGET;
        self->enemy = client;
    }
    FoundTarget(self);
    if (!(self->aiflags & AI_SOUND_TARGET) && self->sight)
        self->sight(self);
    return true;
}

static bool         FacingIdeal(const g_ent *self)
{
    int             delta = (self->yaw - self->ideal_yaw) & 0xFFFF;

    return !(delta > ANG(45) && delta < ANG(315));
}

static bool         enemy_clear;            /* ai_checkattack's trace: nothing (not even a monster) in the way */

static bool         M_CheckAttack(g_ent *self)
{
    s32             chance;

    if (self->enemy->health > 0 && !enemy_clear)
        return false;                       /* something's in the way */
    if (enemy_range == RANGE_MELEE)
    {
        self->attack_state = AS_MISSILE;    /* soldiers have no melee */
        return true;
    }
    if (!self->attack || level.time < self->attack_finished || enemy_range == RANGE_FAR)
        return false;
    if (self->aiflags & AI_STAND_GROUND)
        chance = FIX(0.4);
    else if (enemy_range == RANGE_NEAR)
        chance = FIX(0.1);
    else if (enemy_range == RANGE_MID)
        chance = FIX(0.02);
    else
        return false;
    if (frandom() < chance)
    {
        self->attack_state = AS_MISSILE;
        self->attack_finished = level.time + 2 * frandom();
        return true;
    }
    return false;
}

static void         ai_run_missile(g_ent *self)
{
    self->ideal_yaw = enemy_yaw;
    M_ChangeYaw(self);
    if (FacingIdeal(self))
    {
        self->attack(self);
        self->attack_state = AS_STRAIGHT;
    }
}

/* decide whether to attack, or do something else */
bool                ai_checkattack(g_ent *self, s32 dist)
{
    s32             v[3];
    int             k;

    if (!self->enemy || self->enemy->kind == EK_FREE || self->enemy->health <= 0)
    {
        /* the enemy's dead: back to standing */
        self->enemy = NULL;
        self->goalentity = NULL;
        self->pausetime = level.time + FIX(30000);
        self->stand(self);
        return true;
    }
    self->show_hostile = level.time + FIX(1);
    /* one trace for both "can I see them" and "is anything in the way of a shot" */
    {
        static const s32 zero[3] = { 0, 0, 0 };
        s32     a[3], b[3];
        g_ent   *hit = NULL;
        q_trace t;

        for (k = 0; k < 3; ++k)
        {
            a[k] = self->origin[k];
            b[k] = self->enemy->origin[k];
        }
        a[2] += self->viewheight;
        b[2] += self->enemy->viewheight;
        t = g_trace(a, zero, zero, b, self, MASK_OPAQUE | CONTENTS_WINDOW | CONTENTS_MONSTER, &hit);
        enemy_clear = hit == self->enemy || (t.fraction == FIX(1) && !hit);
        /* seen unless something opaque is in the way: windows and other monsters don't hide you */
        enemy_vis = enemy_clear || hit || (t.contents & CONTENTS_WINDOW);
    }
    if (enemy_vis)
    {
        self->search_time = level.time + FIX(5);
        for (k = 0; k < 3; ++k)
            self->last_sighting[k] = self->enemy->origin[k];
    }
    enemy_infront = infront(self, self->enemy);
    enemy_range = range(self, self->enemy);
    for (k = 0; k < 3; ++k)
        v[k] = self->enemy->origin[k] - self->origin[k];
    enemy_yaw = vectoyaw(v);
    if (self->attack_state == AS_MISSILE)
    {
        ai_run_missile(self);
        return true;
    }
    if (!enemy_vis)
        return false;
    return M_CheckAttack(self);
}

/* the monster has an enemy it's trying to kill */
void                ai_run(g_ent *self, s32 dist)
{
    s32             v[3], d1;
    g_ent           *save;
    int             k;

    if (self->aiflags & AI_SOUND_TARGET)
    {
        for (k = 0; k < 3; ++k)
            v[k] = self->origin[k] - self->enemy->origin[k];
        if (vlen(v) < FIX(64))
        {
            self->aiflags |= AI_STAND_GROUND | AI_TEMP_STAND_GROUND;
            self->stand(self);
            return;
        }
        M_MoveToGoal(self, dist);
        if (!FindTarget(self))
            return;
    }
    if (ai_checkattack(self, dist))
        return;
    if (enemy_vis)
    {
        M_MoveToGoal(self, dist);
        self->aiflags &= ~AI_LOST_SIGHT;
        for (k = 0; k < 3; ++k)
            self->last_sighting[k] = self->enemy->origin[k];
        return;
    }
    if (self->search_time && level.time > self->search_time + FIX(20))
    {
        M_MoveToGoal(self, dist);
        self->search_time = 0;
        return;
    }
    /* lost sight: head for where it was last seen */
    save = self->goalentity;
    self->goalentity = &goal_marker;
    if (!(self->aiflags & AI_LOST_SIGHT))
    {
        self->aiflags |= AI_LOST_SIGHT | AI_PURSUIT_LAST_SEEN;
        self->aiflags &= ~(AI_PURSUE_NEXT | AI_PURSUE_TEMP);
    }
    for (k = 0; k < 3; ++k)
        v[k] = self->origin[k] - self->last_sighting[k];
    d1 = vlen(v);
    if (d1 <= dist)
    {
        self->aiflags |= AI_PURSUE_NEXT;
        dist = d1;
    }
    for (k = 0; k < 3; ++k)
        self->goal_pos[k] = self->last_sighting[k];
    for (k = 0; k < 3; ++k)
        v[k] = self->goal_pos[k] - self->origin[k];
    self->ideal_yaw = vectoyaw(v);
    M_MoveToGoal(self, dist);
    self->goalentity = save;
}

/* ---- g_monster.c ---- */

void                M_MoveFrame(g_ent *self)
{
    const mmove_t   *move = self->move;
    int             index;

    if (self->nextframe && self->nextframe >= move->first && self->nextframe <= move->last)
    {
        self->frame = self->nextframe;
        self->nextframe = 0;
    }
    else
    {
        if (self->frame == move->last && move->endfunc)
        {
            move->endfunc(self);
            move = self->move;              /* very likely changed */
            if (self->dead && self->move == NULL)
                return;
        }
        if (self->frame < move->first || self->frame > move->last)
        {
            self->aiflags &= ~AI_HOLD_FRAME;
            self->frame = move->first;
        }
        else if (!(self->aiflags & AI_HOLD_FRAME))
        {
            if (++self->frame > move->last)
                self->frame = move->first;
        }
    }
    index = self->frame - move->first;
    if (move->frames[index].ai)
        move->frames[index].ai(self, self->aiflags & AI_HOLD_FRAME ? 0 : FIX(move->frames[index].dist));
    if (move->frames[index].think)
        move->frames[index].think(self);
}

/* standing on something? (Quake's M_CheckGround) */
static void         M_CheckGround(g_ent *ent)
{
    s32             p[3];
    q_trace         t;

    if (ent->velocity[2] > FIX(100))
    {
        ent->on_ground = false;
        return;
    }
    p[0] = ent->origin[0];
    p[1] = ent->origin[1];
    p[2] = ent->origin[2] - FIX(0.25);
    t = g_trace(ent->origin, ent->mins, ent->maxs, p, ent, MASK_MONSTERSOLID, NULL);
    if (t.plane && t.plane->n[2] < FIX(0.7) && !t.startsolid)
    {
        ent->on_ground = false;
        return;
    }
    if (!t.startsolid && !t.allsolid && t.fraction < FIX(1))
    {
        ent->origin[0] = t.endpos[0];
        ent->origin[1] = t.endpos[1];
        ent->origin[2] = t.endpos[2];
        ent->on_ground = true;
        ent->velocity[2] = 0;
    }
    else if (t.fraction == FIX(1))
        ent->on_ground = false;
}

void                M_droptofloor(g_ent *ent)
{
    s32             end[3];
    q_trace         t;

    ent->origin[2] += FIX(1);
    end[0] = ent->origin[0];
    end[1] = ent->origin[1];
    end[2] = ent->origin[2] - FIX(256);
    t = g_trace(ent->origin, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID, NULL);
    if (t.fraction == FIX(1) || t.allsolid)
        return;
    ent->origin[0] = t.endpos[0];
    ent->origin[1] = t.endpos[1];
    ent->origin[2] = t.endpos[2];
    M_CheckGround(ent);
}

/* MOVETYPE_STEP: falls when it isn't on the ground */
void                monster_physics(g_ent *self)
{
    s32             end[3];
    q_trace         t;
    int             k;

    M_CheckGround(self);
    if (self->on_ground)
        return;
    self->velocity[2] -= fmul(FIX(800), FRAMETIME);
    for (k = 0; k < 3; ++k)
        end[k] = self->origin[k] + fmul(self->velocity[k], FRAMETIME);
    t = g_trace(self->origin, self->mins, self->maxs, end, self, MASK_MONSTERSOLID, NULL);
    for (k = 0; k < 3; ++k)
        self->origin[k] = t.endpos[k];
    if (t.fraction < FIX(1) && t.plane && t.plane->n[2] >= FIX(0.7))
    {
        self->on_ground = true;
        self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
    }
}

void                monster_start(g_ent *self)
{
    self->kind = EK_MONSTER;
    self->solid = true;
    self->yaw_speed = ANG(20);
    self->viewheight = FIX(25);
    self->max_health = self->health;
    self->ideal_yaw = self->yaw;
    self->old_yaw = self->yaw;
    M_droptofloor(self);
    self->stand(self);
    self->frame = self->move->first + (int)(rng() % (u32)(self->move->last - self->move->first + 1));
    self->old_frame = self->frame;
}
