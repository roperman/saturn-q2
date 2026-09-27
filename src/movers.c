/*
** Doors, lifts and buttons: the brush models that move (Quake 2's
** game/g_func.c, cut down). Each goes between its rest position and its far
** end (offset = move * frac, frac 0..1), waits there, and comes back.
**   doors    open when the player comes near (their team's box, 60 bigger
**            across), or when a button targets them; close after "wait"
**            (-1: stay open); go back up if they close on the player
**   lifts    rest at the bottom; up when the player stands on one, down 3
**            seconds after they get off
**   buttons  press in when touched, open the doors they target
** Trains, rotating things and shootable buttons don't move yet.
*/
#include "q2.h"

enum { M_REST, M_GOING, M_AT_END, M_RETURNING };

typedef struct
{
    s32             frac, step;             /* where it is (0..1), and speed / distance */
    s32             timer;
    int             state;
}                   m_state;

s32                 (*mover_ofs)[3];
static m_state      *ms;
u8                  *mover_gone;            /* blown up (func_explosive): not drawn, not solid */

static s32          len_units(const s32 *v)
{
    u32 x = (u32)iabs(v[0] >> 16), y = (u32)iabs(v[1] >> 16), z = (u32)iabs(v[2] >> 16);

    return (s32)isqrt(x * x + y * y + z * z);
}

static s32          rest_frac(const q_mover *mv)
{
    return mv->flags & MF_START_OPEN ? FIX(1) : 0;
}

static void         place(int m)
{
    const q_mover   *mv = &lv.movers[m];
    int             k;

    for (k = 0; k < 3; ++k)
        mover_ofs[m][k] = fmul(mv->move[k], ms[m].frac);
}

void                movers_init(void)
{
    int             m;

    mover_ofs = level_alloc((u32)lv.nmodels * 12);
    if (!mover_gone)
        mover_gone = level_alloc((u32)lv.nmodels);
    memset(mover_gone, 0, (u32)lv.nmodels);
    ms = level_alloc((u32)lv.nmodels * sizeof(m_state));
    for (m = 0; m < lv.nmodels; ++m)
    {
        const q_mover   *mv = &lv.movers[m];
        s32             d = len_units(mv->move);

        ms[m].frac = rest_frac(mv);
        ms[m].step = d ? mv->speed / d : 0;     /* frac a second (16.16) */
        ms[m].state = M_REST;
        ms[m].timer = 0;
        place(m);
    }
}

/* the player's box overlaps this box? */
static bool         player_in(const s32 *mins, const s32 *maxs)
{
    int             k;

    for (k = 0; k < 3; ++k)
        if (pl.origin[k] + p_maxs[k] < mins[k] || pl.origin[k] + p_mins[k] > maxs[k])
            return false;
    return true;
}

/* set a mover (and its team) going */
static void         activate(int m)
{
    int             i = m;

    do
    {
        if (ms[i].state == M_REST || ms[i].state == M_RETURNING)
            ms[i].state = M_GOING;
        else if (ms[i].state == M_AT_END && lv.movers[i].wait >= 0)
            ms[i].timer = lv.movers[i].wait;    /* already open: stay a while longer */
        i = lv.movers[i].team_next;
    } while (i >= 0 && i != m);
}

/* everything called name: doors open, lifts go (from triggers, buttons, relays) */
void                movers_use(int name)
{
    int             m;

    if (!name)
        return;
    for (m = 1; m < lv.nmodels; ++m)
        if (lv.movers[m].targetname == name && lv.movers[m].kind >= MV_DOOR && !mover_gone[m])
            activate(m);
}

/* a button's pressed: the game uses its targets (doors, relays, spawners...) */
static void         fire_targets(int target)
{
    g_mover_fired(target);
}

void                mover_hide(int model)
{
    if (model > 0 && model < lv.nmodels)
        mover_gone[model] = 1;
}

bool                mover_live(int model)
{
    return lv.movers[model].kind != MV_NONE && !mover_gone[model];
}

/* would the player be inside this mover at offset o? */
static bool         blocks_player(int m, const s32 *o)
{
    s32             p[3];
    int             k;
    q_trace         t;

    if (pl.noclip)
        return false;
    for (k = 0; k < 3; ++k)
        p[k] = pl.origin[k] - o[k];
    t = trace_box(p, p, p_mins, p_maxs, lv.models[m].headnode, MASK_PLAYERSOLID);
    return t.startsolid;
}

void                movers_update(s32 dt)
{
    int             m, k;

    for (m = 1; m < lv.nmodels; ++m)
    {
        const q_mover   *mv = &lv.movers[m];
        m_state         *s = &ms[m];
        s32             rest = rest_frac(mv), far = FIX(1) - rest, target, nf, before[3], delta[3];
        bool            triggered;

        if (mv->kind < MV_DOOR)
            continue;
        triggered = !pl.noclip && (mv->flags & MF_PROXIMITY || mv->kind == MV_PLAT) && player_in(mv->tmin, mv->tmax);
        switch (s->state)
        {
            case M_REST:
                if (triggered)
                    activate(m);
                break;
            case M_AT_END:
                if (mv->kind == MV_PLAT && triggered)
                    s->timer = FIX(1);          /* still on it: stay up */
                if (mv->wait < 0)
                    break;
                s->timer -= dt;
                if (s->timer <= 0)
                    s->state = M_RETURNING;
                break;
            case M_RETURNING:
                if (triggered && mv->kind == MV_DOOR)
                    activate(m);                /* someone's there: back open */
                break;
        }
        if (s->state != M_GOING && s->state != M_RETURNING)
            continue;
        target = s->state == M_GOING ? far : rest;
        nf = s->frac + (target > s->frac ? fmul(s->step, dt) : -fmul(s->step, dt));
        if ((target > s->frac && nf >= target) || (target < s->frac && nf <= target) || s->step == 0)
            nf = target;
        for (k = 0; k < 3; ++k)
        {
            before[k] = mover_ofs[m][k];
            delta[k] = fmul(mv->move[k], nf) - before[k];
        }
        /* the player's on it: carry them; in its way: a door goes back */
        if (pl.ground_ent == m && !pl.noclip)
        {
            s32 np[3];
            q_trace t;

            for (k = 0; k < 3; ++k)
                np[k] = pl.origin[k] + delta[k];
            t = trace_box(np, np, p_mins, p_maxs, 0, MASK_PLAYERSOLID);
            if (!t.startsolid)
                for (k = 0; k < 3; ++k)
                    pl.origin[k] = np[k];
        }
        {
            s32 was = s->frac;

            s->frac = nf;
            place(m);
            if (pl.ground_ent != m && blocks_player(m, mover_ofs[m]) && mv->kind == MV_DOOR && s->state == M_RETURNING)
            {
                /* closing on the player: stay put and open again */
                s->frac = was;
                place(m);
                activate(m);
                continue;
            }
        }
        if (pl.ground_ent != m && blocks_player(m, mover_ofs[m]))
        {
            /* a lift coming up under the player: lift them */
            for (k = 0; k < 3; ++k)
                pl.origin[k] += delta[k];
        }
        if (nf == target)
        {
            if (s->state == M_GOING)
            {
                s->state = M_AT_END;
                s->timer = mv->wait;
                if (mv->kind == MV_BUTTON)
                    fire_targets(mv->target);
            }
            else
                s->state = M_REST;
        }
    }
}
