/*
** The gun in your hands: Quake 2's v_*.md2 (tools/models.txt, the "view"
** rows), drawn over the world by src/render.c draw_viewmodel.
**
** All seven are 300 KB, more than the levels' carts have left, so there are
** two slots on the cart: the gun you hold, and the next, read from the CD in
** the background (a few sectors a frame) while the one you hold is put away.
** A slot is only read into once its last gun has been off the screen for the
** whole of a raise (VDP1 may still be drawing the frames before: their
** textures and colour tables must stay as they were). Comm Center's cart has
** room for one: there the next is read once the one you held is down and
** gone (a moment with none).
**
** The animation is Quake 2's (Weapon_Generic's frames, 10 a second, drawn in
** between): raising, firing, the first idle frame (no fidgeting), lowering.
** You can't fire until the gun's up, as in Quake 2.
**
** And Quake 2's bob (SV_CalcGunOffset): the gun turned a little against the
** view as you walk (a step a half-cycle) and as you turn (it lags behind),
** view_bob for the renderer to turn it by (OPTIONS: GUN BOB).
*/
#include "game.h"

enum { VS_ACTIVATE, VS_IDLE, VS_FIRE, VS_PUTAWAY, VS_WAIT };

static const char   *view_files[VIEW_COUNT] = { VIEW_FILES };
static u8           *buf[VIEW_SLOTS];
static int          nslots;                     /* 2, or 1 where the cart's short */
static int          gone;                       /* frames the gun's been down (the next one's tables wait for 2; one slot for 3) */
static int          slot_weapon[VIEW_SLOTS];    /* the gun in each slot, or -1 */
static int          cur;                        /* the slot shown */
static int          loading = -1, load_weapon;  /* the slot being read into, and its gun */
static int          lut_slot = -1;              /* the slot whose colour tables are in VRAM (they share one set) */
static int          state = VS_WAIT, idx;       /* where in the animation */
static s32          acc, vclock, last_shot;     /* seconds (16.16) */
static bool         ok;                         /* anything to draw with */
bool                view_on;                    /* (main.c: not at the title, not the benchmark's views) */
bool                opt_gun_bob = true;
s32                 view_bob[3];                /* the gun's turn against the view: pitch (down), yaw (left),
                                                   roll (right side down), radians 16.16 */
static s32          bobtime, bobmove;           /* steps (16.16): Quake 2's, a server frame's worth */
static int          last_yaw, last_pitch;
static bool         bob_fresh = true;           /* (a new level or game: no turn yet to lag behind) */

/* firing's loop: the fire animation's frames it goes round while you keep firing, -1 none
   (the machinegun's two; the chaingun's spun-up middle, Quake 2's 15-21) */
static const s8     fire_loop[VIEW_COUNT][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { 0, 1 }, { 10, 16 },
                                                  { -1, -1 }, { -1, -1 } };


static const q_manim *anim(int slot, const char *name)
{
    const q_mdl     *m = &models[MDL_VIEW0 + slot];

    return &m->anims[model_anim(m, name)];
}

static bool         load_now(int w, int slot)
{
    int             n = cd_load(view_files[w], buf[slot], VIEW_MAX_BYTES);

    slot_weapon[slot] = -1;
    models[MDL_VIEW0 + slot].loaded = false;
    if (n < 64 || !model_parse(&models[MDL_VIEW0 + slot], buf[slot]))
        return false;
    r_view_slot(slot);
    r_view_luts(slot);                      /* (a new level: no gun's been drawn for a while) */
    lut_slot = slot;
    slot_weapon[slot] = w;
    return true;
}

static void         load_later(int w, int slot)
{
    u32             lba, size;

    if (loading >= 0)
        return;
    if (!cd_find(view_files[w], &lba, &size) || size > VIEW_MAX_BYTES)
    {
        ok = false;                         /* (not on the disc: no gun, and firing as without one) */
        return;
    }
    slot_weapon[slot] = -1;
    models[MDL_VIEW0 + slot].loaded = false;
    if (cd_async_start(lba, (size + 2047) / 2048, buf[slot]))
    {
        loading = slot;
        load_weapon = w;
    }
}

static int          slot_of(int w)
{
    int             k;

    for (k = 0; k < nslots; ++k)
        if (slot_weapon[k] == w)
            return k;
    return -1;
}

/* a new level (after render_init): the slots on the cart again, the gun you hold in one */
void                view_level_init(void)
{
    int             k;

    while (cd_async_busy() && cd_async_poll(32) == 0)
        ;                                   /* (a read under way: done with, whatever it was) */
    loading = -1;
    bob_fresh = true;
    r_view_level();
    nslots = cart_free() >= 2 * VIEW_MAX_BYTES ? 2 : 1;
    for (k = 0; k < VIEW_SLOTS; ++k)
    {
        buf[k] = k < nslots ? cart_alloc(VIEW_MAX_BYTES) : NULL;
        slot_weapon[k] = -1;
        models[MDL_VIEW0 + k].loaded = false;
    }
    cur = 0;
    lut_slot = -1;
    ok = load_now(client.weapon, 0);
    state = VS_IDLE;
    idx = 0;
    acc = 0;
}

/* a new game or a restart: the gun you now hold (the blaster), up */
void                view_reset(void)
{
    int             s = slot_of(client.weapon);

    if (s < 0)
    {
        while (cd_async_busy() && cd_async_poll(32) == 0)
            ;
        loading = -1;
        s = nslots > 1 ? cur ^ 1 : 0;
        ok = load_now(client.weapon, s);
    }
    if (s != lut_slot && s >= 0)
    {
        r_view_luts(s);                     /* (the old gun's frames in flight: a restart's picture changes anyway) */
        lut_slot = s;
    }
    cur = s;
    state = VS_IDLE;
    idx = 0;
    acc = 0;
    bob_fresh = true;
}

/* can the gun fire? (it's up, and it's the one you hold) */
bool                view_ready(void)
{
    return !ok || ((state == VS_IDLE || state == VS_FIRE) && slot_weapon[cur] == client.weapon);
}

/* a shot: the fire animation (again, or round its loop) */
void                view_fired(void)
{
    const s8        *lp = fire_loop[client.weapon];

    if (state == VS_FIRE && lp[0] >= 0)
    {
        if (idx > lp[1])
            idx = lp[0];                    /* spinning down: back up to speed */
    }
    else
    {
        state = VS_FIRE;
        idx = 0;
        acc = 0;
    }
    last_shot = vclock;
}

static void         step(void)
{
    const s8        *lp;

    switch (state)
    {
        case VS_ACTIVATE:
            if (++idx >= anim(cur, "active")->count)
            {
                state = VS_IDLE;
                idx = 0;
            }
            break;
        case VS_FIRE:
            lp = fire_loop[slot_weapon[cur] >= 0 ? slot_weapon[cur] : 0];
            if (lp[0] >= 0 && idx == lp[1] && vclock - last_shot < FIX(0.15))
                idx = lp[0];                /* still firing: round the loop */
            else if (++idx >= anim(cur, "fire")->count)
            {
                state = VS_IDLE;
                idx = 0;
            }
            break;
        case VS_PUTAWAY:
            if (++idx >= anim(cur, "putway")->count)
            {
                state = VS_WAIT;
                idx = 0;
            }
            break;
        default:
            break;
    }
}

/* Quake 2's gun angles (degrees there): from the walk, xyspeed * |sin(bobtime pi)| * 0.005
   pitch, 0.01 yaw and 0.005 roll (those two the other way every other step); from turning,
   0.2 of the last server frame's turn (0.1 s), and 0.1 of the yaw's as roll */
static void         bob(s32 dt)
{
    s32             xyspeed, fs, b, dyaw, dpitch;
    int             vx = pl.velocity[0] >> 16, vy = pl.velocity[1] >> 16;

    if (bob_fresh)
    {
        last_yaw = cam.yaw;
        last_pitch = cam.pitch;
        bob_fresh = false;
    }
    dyaw = (s16)(last_yaw - cam.yaw);           /* (angles: 65536 a turn) */
    dpitch = (s16)(last_pitch - cam.pitch);
    last_yaw = cam.yaw;
    last_pitch = cam.pitch;
    if (!opt_gun_bob || dt <= 0)
    {
        if (!opt_gun_bob)
            view_bob[0] = view_bob[1] = view_bob[2] = 0;
        return;
    }
    xyspeed = (s32)isqrt((u32)(vx * vx + vy * vy));
    if (xyspeed < 5)
    {
        bobmove = 0;
        bobtime = 0;                            /* (from the start of a step again) */
    }
    else if (pl.on_ground)
        bobmove = xyspeed > 210 ? FIX(0.25) : xyspeed > 100 ? FIX(0.125) : FIX(0.0625);
    bobtime = (bobtime + fmul(bobmove, dt) * 10) & 0x1FFFF;     /* (bobmove a tenth of a second's; two steps kept) */
    fs = fsin((int)((u32)bobtime >> 1));        /* sin(bobtime pi): 65536 a turn is 2 pi */
    b = xyspeed * (fs < 0 ? -fs : fs);          /* xyspeed |sin|, 16.16 */
    view_bob[0] = fmul(fmul(b, FIX(0.005)), 1144);  /* (degrees, then radians: pi / 180 is 1144) */
    view_bob[1] = view_bob[0] * 2;
    view_bob[2] = view_bob[0];
    if (bobtime >> 16 & 1)
    {
        view_bob[1] = -view_bob[1];
        view_bob[2] = -view_bob[2];
    }
    /* turning: a tenth of a second's worth of this frame's turn (last angle - this one), at
       most 45 degrees; 65536 a turn, so 0.2 of it in radians 16.16 is * 2 pi * 0.2 */
    dyaw = iclamp((s32)((s64)dyaw * FIX(0.1) / dt), -0x2000, 0x2000);
    dpitch = iclamp((s32)((s64)dpitch * FIX(0.1) / dt), -0x2000, 0x2000);
    view_bob[0] += (dpitch * 82354) >> 16;      /* (2 pi 0.2: 1.2566) */
    view_bob[1] += (dyaw * 82354) >> 16;
    view_bob[2] += (dyaw * 41177) >> 16;        /* (2 pi 0.1) */
#ifdef VIEW_BOB_BENCH
    {
        /* (OPT=-DVIEW_BOB_BENCH: as if walking, standing still: its cost; changing every frame) */
        static int bb;

        ++bb;
        view_bob[0] = 1000 + (bb & 7) * 60;
        view_bob[1] = -2000 + (bb & 15) * 40;
        view_bob[2] = 800 - (bb & 3) * 90;
    }
#endif
}

/* once a frame: the animation on by dt, the background read on by a few sectors */
void                view_update(s32 dt)
{
    int             s;

    if (loading >= 0)
    {
        int r = cd_async_poll(4);

        if (r != 0)
        {
            if (lut_slot == loading)
                lut_slot = -1;              /* (another gun in it: its tables aren't up) */
            if (r > 0 && model_parse(&models[MDL_VIEW0 + loading], buf[loading]))
            {
                r_view_slot(loading);
                slot_weapon[loading] = load_weapon;
            }
            else
                ok = false;                 /* (a read gone wrong: no gun rather than none for ever) */
            loading = -1;
        }
    }
    bob(dt);
    if (!ok)
        return;
    vclock += dt;
#ifdef VIEW_ANIM
    if (state == VS_IDLE && slot_weapon[cur] == client.weapon)
        view_fired();                       /* (OPT=-DVIEW_ANIM: firing all the time, without shots: the gun's cost animating) */
#endif
    /* a new gun chosen: this one down (the next one read meanwhile), then that one up */
    if (slot_weapon[cur] != client.weapon && (state == VS_IDLE || state == VS_FIRE))
    {
        state = VS_PUTAWAY;
        idx = 0;
        acc = 0;
    }
    gone = state == VS_WAIT ? gone + 1 : 0;
    if (slot_of(client.weapon) < 0 && loading < 0)
    {
        if (nslots > 1 && (state == VS_PUTAWAY || state == VS_WAIT))
            load_later(client.weapon, cur ^ 1);
        else if (nslots == 1 && state == VS_WAIT && gone > 3)
            load_later(client.weapon, 0);           /* (the one slot: its gun's off the screen) */
    }
    /* up it comes: its colour tables in first, where the last gun's were, once that's three
       frames gone (the last drawn, two frames before, is still in flight till then) */
    if (state == VS_WAIT && (s = slot_of(client.weapon)) >= 0 && (s == lut_slot || gone >= 2))
    {
        if (s != lut_slot)
        {
            r_view_luts(s);
            lut_slot = s;
        }
        cur = s;
        state = VS_ACTIVATE;
        idx = 0;
        acc = 0;
    }
    for (acc += dt; acc >= FIX(0.1); acc -= FIX(0.1))
        step();
}

/* what to draw: the model, its two frames and the blend between (NULL: nothing) */
const q_mdl         *view_frame(int *f0, int *f1, s32 *lerp)
{
    const q_manim   *a;
    int             last;

    if (!ok || !view_on || state == VS_WAIT || !models[MDL_VIEW0 + cur].loaded || !g_player || g_player->dead)
        return NULL;
    switch (state)
    {
        case VS_ACTIVATE:
            a = anim(cur, "active");
            *f0 = a->first + idx;
            *f1 = idx + 1 < a->count ? *f0 + 1 : anim(cur, "idle")->first;
            break;
        case VS_FIRE:
        {
            const s8 *lp = fire_loop[slot_weapon[cur] >= 0 ? slot_weapon[cur] : 0];

            a = anim(cur, "fire");
            *f0 = a->first + idx;
            if (lp[0] >= 0 && idx == lp[1] && vclock - last_shot < FIX(0.15))
                *f1 = a->first + lp[0];
            else
                *f1 = idx + 1 < a->count ? *f0 + 1 : anim(cur, "idle")->first;
            break;
        }
        case VS_PUTAWAY:
            a = anim(cur, "putway");
            last = a->first + a->count - 1;
            *f0 = a->first + idx;
            *f1 = *f0 < last ? *f0 + 1 : last;
            break;
        default:
            *f0 = *f1 = anim(cur, "idle")->first;
            break;
    }
    *lerp = imin(acc * 10, FIX(1));
    return &models[MDL_VIEW0 + cur];
}
