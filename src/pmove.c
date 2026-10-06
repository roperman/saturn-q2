/*
** Player movement: the core of Quake 2's qcommon/pmove.c in 16.16 fixed
** point. Ground check, friction, acceleration, gravity, jumping, and the
** step-slide move that walks up stairs; basic swimming. Not yet: ladders,
** ducking, water jumps, currents.
*/
#include "game.h"

#define STEPSIZE        FIX(18)
#define MIN_STEP_NORMAL FIX(0.7)
#define STOP_EPSILON    FIX(0.1)
#define MAX_CLIP_PLANES (5)

#define PM_STOPSPEED    FIX(100)
#define PM_MAXSPEED     FIX(300)
#define PM_ACCELERATE   (10)
#define PM_WATERACCEL   (10)
#define PM_FRICTION     (6)
#define PM_WATERFRICTION (1)
#define PM_GRAVITY      FIX(800)
#define PM_JUMP         FIX(270)

const s32           p_mins[3] = { -FIX(16), -FIX(16), -FIX(24) }, p_maxs[3] = { FIX(16), FIX(16), FIX(32) };

q_player            pl;
static s32          frametime;          /* seconds, 16.16 */

static inline s32   dot3(const s32 *a, const s32 *b)
{
    return fmul(a[0], b[0]) + fmul(a[1], b[1]) + fmul(a[2], b[2]);
}

/* 16.16 length of a vector, to 1/32 (squares fit 32 bits up to ~1000 a component) */
static s32          len3(const s32 *v)
{
    u32 x = (u32)iabs(v[0] >> 11), y = (u32)iabs(v[1] >> 11), z = (u32)iabs(v[2] >> 11);

    return (s32)(isqrt(x * x + y * y + z * z) << 11);
}

/* the brush models that can be solid (not triggers), their bounds and BSP
   in HWRAM: every trace looks at them all, and lv.models is on the cart */
typedef struct { s32 mins[3], maxs[3]; s32 headnode; int m; } t_mover;
static t_mover      *tmov;
static int          ntmov;
/* each mover's box where it is now (gone: none), side by side: a trace's loop over the movers
   reads these alone (its record, its place and whether it's gone were three lines a mover);
   made again when something's moved (movers_version) */
static s32          (*tbox)[6];
static u32          tbox_version;
extern u32          movers_version;

static void         tbox_refresh(void)
{
    int             i, k;

    if (tbox_version == movers_version)
        return;
    for (i = 0; i < ntmov; ++i)
    {
        const t_mover   *mo = &tmov[i];
        const s32       *o = mover_ofs[mo->m];

        for (k = 0; k < 3; ++k)
        {
            tbox[i][k] = mover_gone[mo->m] ? 0x7FFFFFFF : mo->mins[k] + o[k];
            tbox[i][3 + k] = mover_gone[mo->m] ? -0x7FFFFFFF : mo->maxs[k] + o[k];
        }
    }
    tbox_version = movers_version;
}

/* the ladders (brushes with CONTENTS_LADDER, 1 or 2 a level): bounds from their axial sides
   (each's the brush's back), so check_ladder can leave out its trace when you're nowhere near */
#define MAX_LADDERS     (8)
static s32          lad_lo[MAX_LADDERS][3], lad_hi[MAX_LADDERS][3];
static int          nladders;               /* -1: more than MAX_LADDERS, always trace */
#ifdef LADDER_CHECK
u32                 lc_frames, lc_near, lc_ladder, lc_bad;
#endif

__attribute__((cold)) void                trace_world_init(void)
{
    int             m, k;

    for (m = 1, ntmov = 0; m < lv.nmodels; ++m)
        ntmov += lv.movers[m].kind != MV_NONE;
    tmov = level_alloc((u32)(ntmov ? ntmov : 1) * sizeof(t_mover));
    tbox = level_alloc((u32)(ntmov ? ntmov : 1) * 24);
    tbox_version = movers_version - 1;
    for (m = 1, ntmov = 0; m < lv.nmodels; ++m)
        if (lv.movers[m].kind != MV_NONE)
        {
            t_mover *t = &tmov[ntmov++];

            for (k = 0; k < 3; ++k)
            {
                t->mins[k] = lv.models[m].mins[k];
                t->maxs[k] = lv.models[m].maxs[k];
            }
            t->headnode = lv.models[m].headnode;
            t->m = m;
        }
    nladders = 0;
    for (m = 0; m < lv.nbrushes; ++m)
    {
        const q_brush   *b = &lv.brushes[m];
        int             i;

        if (!(b->contents & CONTENTS_LADDER))
            continue;
        if (nladders == MAX_LADDERS)
        {
            nladders = -1;
            break;
        }
        for (k = 0; k < 3; ++k)
        {
            lad_lo[nladders][k] = -0x7FFFFFFF;  /* (no side on an axis: no bound on it) */
            lad_hi[nladders][k] = 0x7FFFFFFF;
        }
        for (i = 0; i < b->numsides; ++i)
        {
            const q_plane *pl = &lv.planes[lv.brushsides[b->firstside + i].plane];
            int           ty = pl->type;

            if (ty >= 3)
                continue;
            if (pl->n[ty] > 0)
                lad_hi[nladders][ty] = imin(lad_hi[nladders][ty], pl->dist);
            else
                lad_lo[nladders][ty] = imax(lad_lo[nladders][ty], -pl->dist);
        }
        ++nladders;
    }
}

/* could a box moving within lo..hi touch a ladder, or a mover (which might carry one)? */
static bool         ladder_near(const s32 *lo, const s32 *hi)
{
    int             i, k;

    if (nladders < 0)
        return true;
    for (i = 0; i < nladders; ++i)
    {
        for (k = 0; k < 3; ++k)
            if (hi[k] < lad_lo[i][k] || lo[k] > lad_hi[i][k])
                break;
        if (k == 3)
            return true;
    }
    tbox_refresh();
    for (i = 0; i < ntmov; ++i)
    {
        const s32       *b = tbox[i];

        if (b[3] >= lo[0] && b[0] <= hi[0] && b[4] >= lo[1] && b[1] <= hi[1] && b[5] >= lo[2] && b[2] <= hi[2])
            return true;
    }
    return false;
}

/* the world, then each solid brush model where it is now (Quake 2's SV_Trace) */
q_trace             trace_world(const s32 *start, const s32 *p_mins, const s32 *p_maxs, const s32 *end, int mask)
{
    bool            point = !p_mins[0] && !p_mins[1] && !p_mins[2] && !p_maxs[0] && !p_maxs[1] && !p_maxs[2];
    q_trace         t = point ? trace_line(start, end, 0, mask) : trace_box(start, end, p_mins, p_maxs, 0, mask);
    s32             lo[3], hi[3], ls[3], le[3];
    int             i, m, k;

#ifdef FIGHT_BENCH
    extern u32      tr_ticks[5];
    u32             tt0 = frt_read();
#endif

    t.ent = 0;
    for (k = 0; k < 3; ++k)
    {
        lo[k] = imin(start[k], end[k]) + p_mins[k] - FIX(1);
        hi[k] = imax(start[k], end[k]) + p_maxs[k] + FIX(1);
    }
    tbox_refresh();
    for (i = 0; i < ntmov; ++i)
    {
        const s32       *b = tbox[i];
        const t_mover   *mo;
        const s32       *o;
        q_trace         mt;

        if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2])
            continue;
        mo = &tmov[i];
        m = mo->m;
        o = mover_ofs[m];
        for (k = 0; k < 3; ++k)
        {
            ls[k] = start[k] - o[k];
            le[k] = end[k] - o[k];
        }
#ifdef FIGHT_BENCH
        {
            extern u32 tr_count[8];

            ++tr_count[5];
        }
#endif
        mt = point ? trace_line(ls, le, mo->headnode, mask) : trace_box(ls, le, p_mins, p_maxs, mo->headnode, mask);
        if (mt.allsolid || mt.startsolid || mt.fraction < t.fraction)
        {
            bool was_start = t.startsolid;

            for (k = 0; k < 3; ++k)
                mt.endpos[k] += o[k];
            mt.ent = m;
            t = mt;
            if (was_start)
                t.startsolid = true;
        }
    }
#ifdef FIGHT_BENCH
    if (!point)
        tr_ticks[2] += (frt_read() - tt0) & 0xFFFF;
#endif
    return t;
}

q_trace             pm_trace(const s32 *start, const s32 *end)
{
    return g_trace(start, p_mins, p_maxs, end, g_player, MASK_PLAYERSOLID, NULL);
}

static void         clip_velocity(s32 *v, const s32 *n, s32 overbounce)
{
    s32             backoff = fmul(dot3(v, n), overbounce);
    int             i;

    for (i = 0; i < 3; ++i)
    {
        v[i] -= fmul(n[i], backoff);
        if (v[i] > -STOP_EPSILON && v[i] < STOP_EPSILON)
            v[i] = 0;
    }
}

static bool         slide_clear;            /* (the last slide: the whole way, nothing touched) */

static void         step_slide_move_(void)
{
    s32             planes[MAX_CLIP_PLANES][3], primal[3], end[3], time_left = frametime;
    int             bump, numplanes = 0, i, j, k;
    q_trace         t;

    slide_clear = false;
    for (i = 0; i < 3; ++i)
        primal[i] = pl.velocity[i];
    for (bump = 0; bump < 4; ++bump)
    {
        for (i = 0; i < 3; ++i)
            end[i] = pl.origin[i] + fmul(time_left, pl.velocity[i]);
        t = pm_trace(pl.origin, end);
        if (t.allsolid)
        {
            pl.velocity[2] = 0;                 /* trapped */
            return;
        }
        slide_clear = bump == 0 && t.fraction == FIX(1);
        if (t.fraction > 0)
        {
            for (i = 0; i < 3; ++i)
                pl.origin[i] = t.endpos[i];
            numplanes = 0;
        }
        if (t.fraction == FIX(1) || !t.plane)
            break;
        time_left -= fmul(time_left, t.fraction);
        if (numplanes >= MAX_CLIP_PLANES)
        {
            pl.velocity[0] = pl.velocity[1] = pl.velocity[2] = 0;
            break;
        }
        for (i = 0; i < 3; ++i)
            planes[numplanes][i] = t.plane->n[i];
        ++numplanes;
        /* make the velocity run along all the planes hit */
        for (i = 0; i < numplanes; ++i)
        {
            clip_velocity(pl.velocity, planes[i], FIX(1.01));
            for (j = 0; j < numplanes; ++j)
                if (j != i && dot3(pl.velocity, planes[j]) < 0)
                    break;
            if (j == numplanes)
                break;
        }
        if (i == numplanes)
        {
            /* along the crease */
            s32 dir[3], d;

            if (numplanes != 2)
            {
                pl.velocity[0] = pl.velocity[1] = pl.velocity[2] = 0;
                break;
            }
            dir[0] = fmul(planes[0][1], planes[1][2]) - fmul(planes[0][2], planes[1][1]);
            dir[1] = fmul(planes[0][2], planes[1][0]) - fmul(planes[0][0], planes[1][2]);
            dir[2] = fmul(planes[0][0], planes[1][1]) - fmul(planes[0][1], planes[1][0]);
            d = dot3(dir, pl.velocity);
            for (k = 0; k < 3; ++k)
                pl.velocity[k] = fmul(dir[k], d);
        }
        /* against the original velocity: stop dead, no jitter in sloped corners */
        if (dot3(pl.velocity, primal) <= 0)
        {
            pl.velocity[0] = pl.velocity[1] = pl.velocity[2] = 0;
            break;
        }
    }
}

/* the slide move, and again from a step higher up: whichever goes further */
static void         step_slide_move(void)
{
    s32             start_o[3], start_v[3], down_o[3], down_v[3], up[3], down[3];
    s32             dx, dy, down_dist, up_dist;
    q_trace         t;
    int             i;

    for (i = 0; i < 3; ++i)
    {
        start_o[i] = pl.origin[i];
        start_v[i] = pl.velocity[i];
    }
    step_slide_move_();
    if (slide_clear)
        return;                                 /* (the whole way with nothing in it: a step up can't go further,
                                                   and pushed back down lands here; three traces saved) */
    for (i = 0; i < 3; ++i)
    {
        down_o[i] = pl.origin[i];
        down_v[i] = pl.velocity[i];
        up[i] = start_o[i];
    }
    up[2] += STEPSIZE;
    t = pm_trace(up, up);
    if (t.allsolid)
        return;                                 /* can't step up */
    for (i = 0; i < 3; ++i)
    {
        pl.origin[i] = up[i];
        pl.velocity[i] = start_v[i];
    }
    step_slide_move_();
    for (i = 0; i < 3; ++i)
        down[i] = pl.origin[i];
    down[2] -= STEPSIZE;
    t = pm_trace(pl.origin, down);
    if (!t.allsolid)
        for (i = 0; i < 3; ++i)
            pl.origin[i] = t.endpos[i];
    /* which went further (in whole units, so the squares fit) */
    dx = (down_o[0] - start_o[0]) >> 16;
    dy = (down_o[1] - start_o[1]) >> 16;
    down_dist = dx * dx + dy * dy;
    dx = (pl.origin[0] - start_o[0]) >> 16;
    dy = (pl.origin[1] - start_o[1]) >> 16;
    up_dist = dx * dx + dy * dy;
    if (down_dist > up_dist || (t.plane && t.plane->n[2] < MIN_STEP_NORMAL))
    {
        for (i = 0; i < 3; ++i)
        {
            pl.origin[i] = down_o[i];
            pl.velocity[i] = down_v[i];
        }
        return;
    }
    pl.velocity[2] = down_v[2];                 /* walking along a plane: keep its z */
}

static bool         ladder;

/* a ladder brush just in front? (Quake's PM_CheckSpecialMovement) */
static void         check_ladder(int yaw)
{
    s32             spot[3];
    q_trace         t;

    spot[0] = pl.origin[0] + fcos(yaw);
    spot[1] = pl.origin[1] + fsin(yaw);
    spot[2] = pl.origin[2];
    {
        /* nowhere near a ladder (more than a unit clear of its bounds: DIST_EPSILON's 1/32, so
           no side of it can clip the move): the trace couldn't say ladder, so it's left out */
        s32 lo[3], hi[3];
        int k;

        for (k = 0; k < 3; ++k)
        {
            lo[k] = imin(pl.origin[k], spot[k]) + p_mins[k] - FIX(1);
            hi[k] = imax(pl.origin[k], spot[k]) + p_maxs[k] + FIX(1);
        }
#ifdef LADDER_CHECK
        {
            /* (OPT=-DLADDER_CHECK: the trace always, against what leaving it out would have said) */
            extern u32 lc_frames, lc_near, lc_ladder, lc_bad;
            bool near = ladder_near(lo, hi);

            t = pm_trace(pl.origin, spot);
            ladder = t.fraction < FIX(1) && (t.contents & CONTENTS_LADDER);
            ++lc_frames;
            lc_near += near;
            lc_ladder += ladder;
            lc_bad += !near && ladder;
            return;
        }
#endif
        if (!ladder_near(lo, hi))
        {
            ladder = false;
            return;
        }
    }
    t = pm_trace(pl.origin, spot);
    ladder = t.fraction < FIX(1) && (t.contents & CONTENTS_LADDER);
}

static void         friction(void)
{
    s32             speed = len3(pl.velocity), drop = 0, control, newspeed;
    int             i;

    if (speed < FIX(1))
    {
        pl.velocity[0] = pl.velocity[1] = 0;
        return;
    }
    if ((pl.on_ground && !(pl.ground_flags & SURF_SLICK)) || ladder)
    {
        control = speed < PM_STOPSPEED ? PM_STOPSPEED : speed;
        drop += fmul(control * PM_FRICTION, frametime);
    }
    if (pl.waterlevel)
        drop += fmul(speed * PM_WATERFRICTION * pl.waterlevel, frametime);
    newspeed = imax(speed - drop, 0);
    newspeed = fdiv(newspeed, speed);
    for (i = 0; i < 3; ++i)
        pl.velocity[i] = fmul(pl.velocity[i], newspeed);
}

static void         accelerate(const s32 *wishdir, s32 wishspeed, int accel)
{
    s32             add = wishspeed - dot3(pl.velocity, wishdir), accelspeed;
    int             i;

    if (add <= 0)
        return;
    accelspeed = imin(fmul(accel * frametime, wishspeed), add);
    for (i = 0; i < 3; ++i)
        pl.velocity[i] += fmul(accelspeed, wishdir[i]);
}

/* the wished-for velocity -> direction and speed, capped at max */
static s32          wish(s32 *wishvel, s32 *wishdir, s32 max)
{
    s32             speed = len3(wishvel);
    int             i;

    if (speed < FIX(1))
    {
        wishdir[0] = wishdir[1] = wishdir[2] = 0;
        return 0;
    }
    for (i = 0; i < 3; ++i)
        wishdir[i] = fdiv(wishvel[i], speed);
    return imin(speed, max);
}

static void         categorize(void)
{
    s32             point[3];
    int             cont;
    bool            was_in;

    point[0] = pl.origin[0];
    point[1] = pl.origin[1];
    point[2] = pl.origin[2] - FIX(0.25);
    if (pl.velocity[2] > FIX(180))
    {
        pl.on_ground = false;
        pl.ground_ent = 0;
    }
    else
    {
        q_trace t = pm_trace(pl.origin, point);

        pl.ground_flags = t.surf_flags;
        pl.ground_ent = t.ent;
        if (t.fraction == FIX(1) || !t.plane || (t.plane->n[2] < FIX(0.7) && !t.startsolid))
        {
            pl.on_ground = false;
            pl.ground_ent = 0;
        }
        else
        {
            if (!pl.on_ground && pl.velocity[2] < -FIX(200))
            {
                pl.land_time = pl.velocity[2] < -FIX(400) ? 25 : 18;   /* no jumping for a moment */
                s_play_queued(pl.velocity[2] < -FIX(400) ? SND_PLAYER_FALL : SND_PLAYER_LAND, NULL, ATTN_NONE);
            }
            pl.on_ground = true;
        }
    }
    /* how deep in water: feet, waist, eyes */
    was_in = pl.waterlevel != 0;
    pl.waterlevel = 0;
    point[2] = pl.origin[2] + p_mins[2] + FIX(1);
    cont = point_contents(point, 0);
    if (cont & MASK_WATER)
    {
        pl.watertype = cont;
        pl.waterlevel = 1;
        point[2] = pl.origin[2] + p_mins[2] + FIX(23);
        if (point_contents(point, 0) & MASK_WATER)
        {
            pl.waterlevel = 2;
            point[2] = pl.origin[2] + p_mins[2] + FIX(46);
            if (point_contents(point, 0) & MASK_WATER)
                pl.waterlevel = 3;
        }
    }
    if (!was_in && pl.waterlevel)
        s_play_queued(SND_WATER_IN, NULL, ATTN_NONE);
    else if (was_in && !pl.waterlevel)
        s_play_queued(SND_WATER_OUT, NULL, ATTN_NONE);
}

static void         check_jump(bool jump)
{
    if (pl.land_time)
        return;
    if (!jump)
    {
        pl.jump_held = false;
        return;
    }
    if (pl.jump_held)
        return;
    if (pl.waterlevel >= 2)
    {
        pl.on_ground = false;
        if (pl.velocity[2] > -FIX(300))
            pl.velocity[2] = pl.watertype & CONTENTS_WATER ? FIX(100) : pl.watertype & CONTENTS_SLIME ? FIX(80) : FIX(50);
        return;
    }
    if (!pl.on_ground)
        return;
    pl.jump_held = true;
    pl.on_ground = false;
    pl.velocity[2] = imax(pl.velocity[2] + PM_JUMP, PM_JUMP);
    s_play_queued(SND_PLAYER_JUMP, NULL, ATTN_NONE);
}

/* a footstep every 64 units walked on the ground (Quake's bob cycle, near enough) */
static void         footsteps(void)
{
    static s32      walked;
    s32             speed;

    if (!pl.on_ground || pl.waterlevel >= 2)
        return;
    speed = (iabs(pl.velocity[0]) > iabs(pl.velocity[1]) ? iabs(pl.velocity[0]) + (iabs(pl.velocity[1]) >> 1)
                                                          : iabs(pl.velocity[1]) + (iabs(pl.velocity[0]) >> 1));
    if (speed < FIX(100))
        return;
    walked += fmul(speed, frametime);
    if (walked >= FIX(64))
    {
        walked -= FIX(64);
        s_play_queued(SND_STEP1 + (int)(rng() & 3), NULL, ATTN_NONE);
    }
}

static s32          cat_origin[3], cat_vz;  /* (pmove: the first categorize's) */
static bool         cat_ground;
static int          cat_ent, cat_water;

void                pmove(const q_usercmd *cmd, s32 dt)
{
    s32             fwd[3], right[3], wishvel[3], wishdir[3], wishspeed;
    int             i;

    frametime = dt;
    if (pl.land_time)
        pl.land_time = imax(pl.land_time - (int)((dt * 1000) >> 16) / 8, 0);   /* Quake's 8 ms units */
    if (pl.noclip)
    {
        /* fly through everything, where you're looking */
        s32 sy = fsin(cmd->yaw), cy = fcos(cmd->yaw), sp = fsin(cmd->pitch), cp = fcos(cmd->pitch);

        for (i = 0; i < 3; ++i)
            pl.velocity[i] = 0;
        pl.origin[0] += fmul(fmul(cy, cp), fmul(cmd->forward, dt)) + fmul(sy, fmul(cmd->side, dt));
        pl.origin[1] += fmul(fmul(sy, cp), fmul(cmd->forward, dt)) - fmul(cy, fmul(cmd->side, dt));
        pl.origin[2] += -fmul(sp, fmul(cmd->forward, dt)) + fmul(cmd->up, dt);
        return;
    }
    categorize();
    {
        /* what it found, and from where: again below only if any of it's changed */
        int k;

        for (k = 0; k < 3; ++k)
            cat_origin[k] = pl.origin[k];
        cat_vz = pl.velocity[2];
        cat_ground = pl.on_ground;
        cat_ent = pl.ground_ent;
        cat_water = pl.waterlevel;
    }
    check_ladder(cmd->yaw);
    check_jump(cmd->up > 0);
    friction();

    /* the wish: on the level (yaw only) */
    fwd[0] = fcos(cmd->yaw);
    fwd[1] = fsin(cmd->yaw);
    fwd[2] = 0;
    right[0] = fwd[1];
    right[1] = -fwd[0];
    right[2] = 0;
    for (i = 0; i < 3; ++i)
        wishvel[i] = fmul(fwd[i], cmd->forward) + fmul(right[i], cmd->side);
    if (pl.waterlevel >= 2)
    {
        /* swimming: towards where you look, up with jump, drifting down */
        s32 sp = fsin(cmd->pitch), cp = fcos(cmd->pitch);

        wishvel[0] = fmul(wishvel[0], cp);
        wishvel[1] = fmul(wishvel[1], cp);
        wishvel[2] = -fmul(sp, cmd->forward) + (cmd->up ? cmd->up : (!cmd->forward && !cmd->side ? -FIX(60) : 0));
        wishspeed = wish(wishvel, wishdir, PM_MAXSPEED) >> 1;
        accelerate(wishdir, wishspeed, PM_WATERACCEL);
        step_slide_move();
    }
    else if (ladder)
    {
        /* up by looking up (or jump), down by looking down; hardly any sideways (PM_AddCurrents) */
        if (iabs(pl.velocity[2]) <= FIX(200))
        {
            if (cmd->pitch <= -0xAAA && cmd->forward > 0)
                wishvel[2] = FIX(200);
            else if (cmd->pitch >= 0xAAA && cmd->forward > 0)
                wishvel[2] = -FIX(200);
            else if (cmd->up > 0)
                wishvel[2] = FIX(200);
            else
                wishvel[2] = 0;
            wishvel[0] = iclamp(wishvel[0], -FIX(25), FIX(25));
            wishvel[1] = iclamp(wishvel[1], -FIX(25), FIX(25));
        }
        wishspeed = wish(wishvel, wishdir, PM_MAXSPEED);
        accelerate(wishdir, wishspeed, PM_ACCELERATE);
        if (!wishvel[2])
        {
            s32 g = fmul(PM_GRAVITY, frametime);

            pl.velocity[2] = pl.velocity[2] > 0 ? imax(pl.velocity[2] - g, 0) : imin(pl.velocity[2] + g, 0);
        }
        step_slide_move();
    }
    else
    {
        wishspeed = wish(wishvel, wishdir, PM_MAXSPEED);
        if (pl.on_ground)
        {
            pl.velocity[2] = 0;
            accelerate(wishdir, wishspeed, PM_ACCELERATE);
            pl.velocity[2] = 0;
            if (pl.velocity[0] || pl.velocity[1])
                step_slide_move();
        }
        else
        {
            accelerate(wishdir, wishspeed, 1);
            pl.velocity[2] -= fmul(PM_GRAVITY, frametime);
            step_slide_move();
        }
    }
    /* where you are now, if you've moved (standing, the same as it found above: the world
       doesn't move during this, and it reads nothing else) */
    if (pl.origin[0] != cat_origin[0] || pl.origin[1] != cat_origin[1] || pl.origin[2] != cat_origin[2]
        || pl.velocity[2] != cat_vz || pl.on_ground != cat_ground || pl.ground_ent != cat_ent
        || pl.waterlevel != cat_water)
        categorize();
    footsteps();
}

/* put the player down at a spawn point: nudged up out of the floor */
void                pmove_spawn(const s32 *origin)
{
    int             i;

    for (i = 0; i < 3; ++i)
    {
        pl.origin[i] = origin[i];
        pl.velocity[i] = 0;
    }
    pl.origin[2] += FIX(9);
    pl.on_ground = false;
    pl.jump_held = false;
    pl.land_time = 0;
}
