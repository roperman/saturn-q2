/*
** Quake 2 on the Saturn: the first walkthrough.
**
** Loads a baked map (tools/bake_map.py) onto the RAM cart and walks round it
** with Quake 2's movement (pmove.c) and collision (trace.c).
**   up/down        forward/back        left/right   turn
**   L/R            strafe              A            jump (swim up)
**   X/Z            look up/down        C            centre the view
**   B (held)       fire                Y            next weapon
**   START          the stats overlay   START + Y    noclip on/off
**   START + B      (debugging) warp to the next door/lift/button
**   START + A      (debugging) warp in front of the next monster (or barrel)
**   START + C      (debugging) warp to the next item
**   START + X      (debugging) god mode
**   START + Z      (debugging) all the weapons and ammo
**   START + L      (debugging) into the next trigger
**   START + R      the benchmark: fixed views, then a table of times
*/
#include "game.h"

#ifndef MAP_FILE
# define MAP_FILE       "DEMO1.MAP"             /* (build.sh: MAP=demo2 ./build.sh for another) */
#endif

static u16          pad_now, pad_prev;

static bool         pressed(u16 b)
{
    return (pad_now & b) && !(pad_prev & b);
}

static int          strlen_(const char *s)
{
    int             n = 0;

    while (s[n])
        ++n;
    return n;
}

static void         message(const char *a, const char *b)
{
    int             i;

    for (i = 0; i < 2; ++i)
    {
        vdp_begin();
        vdp_text(160 - (int)strlen_(a) * 4, 100, RGB(255, 200, 120), a);
        if (b)
            vdp_text(160 - (int)strlen_(b) * 4, 116, RGB(200, 200, 200), b);
        vdp_submit();
    }
}

static bool         slave_ok, start_used;
static u32          at_end[13];             /* (the benchmarks) the texture cache's counts at the end: r_full, vdp_peak, r_wset */

static void         counts_reset(void)
{
    memset(r_full, 0, sizeof(r_full));
    memset(vdp_peak, 0, sizeof(vdp_peak));
#ifdef TEX_WSET
    {
        extern u32 r_wset[5];

        memset(r_wset, 0, sizeof(r_wset));
    }
#endif
}

static void         counts_at_end(void)
{
    int             k;

    at_end[0] = r_full[0];
    at_end[1] = r_full[1];
    at_end[12] = r_full[2];
    for (k = 0; k < 5; ++k)
        at_end[2 + k] = (u32)vdp_peak[k];
#ifdef TEX_WSET
    {
        extern u32 r_wset[5];

        for (k = 0; k < 5; ++k)
            at_end[7 + k] = r_wset[k];
    }
#endif
}

#ifdef LEVEL_TEST
u32                 lt_hw[4][4], lt_nhw;
#endif
#ifdef FIGHT_BENCH
/* (OPT=-DFIGHT_BENCH: START + R stands you in the round room, god mode on,
   wakes the monsters around it and times 20 seconds of the fight, the game
   running: frame, CPU and game time, and how long each picture stayed up) */
#define FIGHT_SKIP      (10)                    /* frames before timing starts */
static int          fight_frames = -1;          /* -1: not running */
static u32          fight_us, fight_cpu, fight_game, fight_gmax, fight_n, fight_swaps[8], fight_ntr, fight_ttr;
static bool         fight_done;
static g_trace_site fight_sites[16];            /* the traces' call sites, most time first */
static u32          fight_tr[6];                /* trace.c's counts */
static u32          fight_seg[6], fight_t[7];   /* the master's frame in parts (us): input, player, game, before, world, after */
# define FT(k)          (fight_t[k] = frt_read())
static u32          fight_tt[4];                /* a box trace's parts, 0.1 us */
static u32          fight_gun;                  /* the gun in your hands (us) */
static u32          fight_vph[4];               /* ...its vertices, sort, commands, kept (render.c view_ph) */
static u32          fight_r[14];
#ifdef R_PROFILE
static u32          fight_p[13];                 /* the world: setup, grid, cells, slow cells (us); faces, cells, C cells */
#endif                /* the drawing: master, slave; models, their polygons; the
                                                   models' light, vertices, polygons, commands (cumulative), DSP wait */
#else
# define FT(k)          ((void)0)
#endif
static u32          us_game, bench_us[5];

/* the benchmark (START + R): fixed views, 16 frames each, the game paused. x y z (eye), yaw, pitch */
static const s32    bench_demo1[][5] = {
    { 128, -320, 46, 0x6000, 0 },           /* the start, looking down the hall */
    { -1064, 1632, -2, 0x8000, 0 },         /* by the door to the crate room */
    { -1960, 1444, -2, 0xC000, 0 },         /* the dark corridor */
    { 600, -428, -74, 0x4000, 0 },          /* the round room with the soldiers */
    { 20, -213, 46, 0x7000, 0x800 },        /* the hall, turned */
    { -1636, 1488, 142, 0x0000, 0 },        /* the sliding doors */
};
/* demo2's heavy places (its benchmark: MAP=demo2) */
static const s32    bench_demo2[][5] = {
    { 832, 2292, -210, 0x8000, 0 },         /* the start: the warehouse */
    { 935, 2506, 46, 0x6544, 0 },           /* up among the crates */
    { -103, -300, 30, 0x3D35, 0 },          /* the big room: VDP1 is behind here */
    { -88, -260, 30, 0x4354, 0 },           /* ...and more so */
    { 618, -757, -146, 0x1670, 0 },
    { 503, -1816, 46, 0x90E5, 0 },
};
#define NBENCH          (6)                 /* views in each */
static const s32    (*bench_views)[5];
#ifdef TURN_BENCH
/* (OPT=-DTURN_BENCH: at each view a full turn in 90 frames, textures and all:
   the first column is then the texture uploads a frame, not the walk) */
# define BENCH_FRAMES   (92)
#else
# define BENCH_FRAMES   (16)
#endif
static int          bench_view = -1, bench_frame;
static u32          bench_acc[NBENCH][7];   /* walk, master, slave, cpu, frame (us, summed), vblanks waiting for VDP1, the lists' DMA */
static u32          bench_prof[15];         /* setup, grid, cells, slow, models, nfast, nslow, faces, the models' light, verts, polys */
static bool         bench_done;
static bool         god;
void                slave_main(void)
{
    frt_init();
    FRT_FTCSR = 0;
    signal_master();                            /* hello */
    for (;;)
    {
        wait_signal();
        cache_purge();                          /* the master's list, camera and frame */
        render_slave();
        signal_master();
    }
}

/* debugging: next to the next door, lift or button, facing it */
static int          warp_m;

static void         warp_next(void)
{
    int             tries, r, a, k;

    for (tries = 0; tries < lv.nmodels; ++tries)
    {
        const q_model   *mo;
        s32             c[3];

        warp_m = warp_m + 1 < lv.nmodels ? warp_m + 1 : 1;
        if (lv.movers[warp_m].kind < MV_DOOR)
            continue;
        mo = &lv.models[warp_m];
        for (k = 0; k < 3; ++k)
            c[k] = (mo->mins[k] >> 1) + (mo->maxs[k] >> 1) + mover_ofs[warp_m][k];
        for (r = 48; r <= 160; r += 16)
            for (a = 0; a < 0x10000; a += 0x2000)
            {
                s32 p[3];
                q_trace t;

                p[0] = c[0] + fcos(a) * r;
                p[1] = c[1] + fsin(a) * r;
                p[2] = mo->mins[2] + mover_ofs[warp_m][2] + FIX(25);
                t = pm_trace(p, p);
                if (t.startsolid || lv.leafs[level_leaf(p)].cluster < 0)
                    continue;               /* in something, or outside the map */
                pmove_spawn(p);
                pl.origin[2] -= FIX(9);
                cam.yaw = (a + 0x8000) & 0xFFFF;
                return;
            }
    }
}

/* debugging: in front of the next monster, item or barrel, facing it */
static int          warp_e = -1;

static void         warp_ent(bool items)
{
    int             tries, a;

    for (tries = 0; tries < nents; ++tries)
    {
        const q_entity  *e;

        warp_e = (warp_e + 1) % nents;
        e = &ents[warp_e];
        if (!e->live || warp_e >= MAX_EDICTS || (g_edicts[warp_e].kind == EK_ITEM) != items)
            continue;
        for (a = 0; a < 0x10000; a += 0x2000)
        {
            int     ang = (e->yaw + a) & 0xFFFF;
            s32     p[3];
            q_trace t;

            p[0] = e->origin[0] + fcos(ang) * 100;
            p[1] = e->origin[1] + fsin(ang) * 100;
            p[2] = e->origin[2] + FIX(16);
            t = pm_trace(p, p);
            if (t.startsolid || lv.leafs[level_leaf(p)].cluster < 0)
                continue;
            pmove_spawn(p);
            cam.yaw = (ang + 0x8000) & 0xFFFF;
            return;
        }
    }
}

/* debugging: into the next trigger (which fires it) */
static int          warp_t;

static void         warp_trigger(void)
{
    int             n, k;

    for (n = 0; n < MAX_EDICTS; ++n)
    {
        g_ent   *e;
        s32     p[3];

        warp_t = (warp_t + 1) % MAX_EDICTS;
        e = &g_edicts[warp_t];
        if (e->kind != EK_TRIGGER || e->inactive)
            continue;
        for (k = 0; k < 3; ++k)
            p[k] = (e->mins[k] >> 1) + (e->maxs[k] >> 1);
        pmove_spawn(p);
        pl.origin[2] -= FIX(9);
        g_centerprint(e->message ? e->message : "(trigger)");
        return;
    }
}

/* the player's weapon and the game's tick (run by the renderer on the master
   while the slave draws, unless "faster fights" is off or OPT=-DNO_GAME_DURING_DRAW:
   what's drawn is then the game as it was a frame before, for the monsters; the
   view is this frame's) */
static s32          game_dt;
#ifdef NO_GAME_DURING_DRAW
bool                game_during_draw;
#else
bool                game_during_draw = true;    /* START + UP switches it; the options too */
#endif

static void         game_step(void)
{
    u32             tg = frt_read();

    g_player_fire(pad_now & PAD_B && !(pad_now & PAD_START), cam.pos, cam.yaw, cam.pitch);
    g_frame(game_dt);
    us_game = frt_to_us((frt_read() - tg) & 0xFFFF);
}

static char         cur_map[16] = MAP_FILE; /* the level loaded ("DEMO1.MAP") */
static u32          vram_base;              /* VRAM before the HUD's pictures: all a level's again */

static bool         same(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

/* "demo2" in place of this level, you at its start called spot (NULL, or not
   found: the usual one); keep: you as you were (health, armour, weapons, ammo),
   as from one of Quake's levels to the next. false: it's not on the disc */
static bool         load_level(const char *name, const char *spot, bool keep)
{
    char            file[16], at[16];
    g_client        was = client;
    int             health = g_player ? g_player->health : 100, i;
    u32             lba, size;
    const s32       *origin = lv.start;
    int             yaw = lv.start_yaw;

    for (i = 0; name[i] && name[i] != '$' && i < 10; ++i)
        file[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    memcpy(file + i, ".MAP", 5);
    if (!cd_find(file, &lba, &size))
        return false;
    for (i = 0; spot && spot[i] && i < 15; ++i)
        at[i] = spot[i];                    /* (spot's in this level's strings: gone soon) */
    at[i] = 0;
    message("QUAKE II", "LOADING");
    g_edicts = NULL;                        /* (they, and these, come out of the level's memory again) */
    mover_gone = NULL;
    if (!level_load(file))
        for (;;)
            message(file, "WON'T LOAD");
    memcpy(cur_map, file, sizeof(cur_map));
    s_level(file);                          /* (its sounds: its monsters') */
    bench_views = cur_map[4] == '2' ? bench_demo2 : bench_demo1;
    models_load_all();
    vdp_tex_release(vram_base);
    hud_init();
    render_init();
    trace_init();
    movers_init();
    g_init();
    render_sky_init();
    models_hot();
    fx_reset();
    for (i = 0; i < MAX_ENTITIES; ++i)
    {
        ents[i].live = false;
        ents[i].g_leaf = -1;
    }
    for (i = 0; spot && i < lv.nstarts; ++i)
        if (same(lv.starts[i].name, at))
        {
            origin = lv.starts[i].origin;
            yaw = (int)(((s64)lv.starts[i].angle * 0x10000 / 360) >> 16);
        }
    if (!spot)
        yaw = lv.start_yaw;
    pmove_spawn(origin);
    cam.yaw = yaw & 0xFFFF;
    cam.pitch = 0;
    if (keep)
    {
        client = was;
        client.fire_time = client.quad_until = client.invul_until = client.pickup_flash = 0;
        g_player->health = health;
    }
    view_level_init();                      /* (the gun you hold: after the above) */
#ifdef LEVEL_TEST
    {
        extern u32 models_cold;

        level_free(&lt_hw[lt_nhw & 3][0], &lt_hw[lt_nhw & 3][1], &lt_hw[lt_nhw & 3][2]);      /* (what's left, each level) */
        lt_hw[lt_nhw & 3][3] = models_cold;
        ++lt_nhw;
    }
#endif
    return true;
}

/* the level from the start: its movers, its monsters and items (at the skill chosen), you */
static void         new_game(void)
{
    movers_init();
    g_init();
    view_reset();
    pmove_spawn(lv.start);
    cam.yaw = lv.start_yaw;
    cam.pitch = 0;
#ifdef GUNNER_TEST
    {
        /* (MAP=demo2 OPT=-DGUNNER_TEST: in front of Installation's gunner, facing it, god mode) */
        static const s32 at[3] = { FIX(240), FIX(-1560), FIX(30) };      /* (its ledge) */

        pmove_spawn(at);
        cam.yaw = 0xD000;
        god = true;
    }
#endif
#ifdef LADDER_CHECK
    {
        /* (in front of demo1's ladder, facing it) */
        static const s32 at[3] = { FIX(388), FIX(-60), FIX(-200) };

        pmove_spawn(at);
        cam.yaw = 0x4000;
    }
#endif
}

void                main(void)
{
    u32             t_last, t0, us_frame = 0, us_cpu = 0;
    int             waited = 0;
    bool            paused = false;

    frt_init();
#ifndef NO_SLAVE
    slave_start();
    wait_signal();                              /* the slave says hello */
    slave_ok = true;
#endif
    vdp_init(RGB(0, 0, 0));
    vdp_set_hw_erase(false);
    vdp_set_min_frame(1);
#ifndef NO_PIPE
    vdp_set_pipelined(true);                    /* (OPT=-DNO_PIPE: submit waits for the swap) */
#endif
    message("QUAKE II", "LOADING DEMO1 ONTO THE RAM CART");
    bench_views = MAP_FILE[4] == '2' ? bench_demo2 : bench_demo1;      /* "DEMO2.MAP" */
    if (!level_load(MAP_FILE))
        for (;;)
            message(cart_mb < 4 ? "THIS NEEDS THE 4MB RAM CART" : MAP_FILE " WON'T LOAD", NULL);
    message("QUAKE II", "LOADING THE MODELS");
    models_load_all();
    message("QUAKE II", "LOADING THE SOUNDS");
    s_init(cur_map);
    vram_base = vdp_tex_mark();
    hud_init();
    render_init();
    trace_init();
    movers_init();
    g_init();
    render_sky_init();
    models_hot();
    view_level_init();
#ifdef LEVEL_TEST
    {
        extern u32 models_cold;

        level_free(&lt_hw[0][0], &lt_hw[0][1], &lt_hw[0][2]);
        lt_hw[0][3] = models_cold;
        ++lt_nhw;
    }
#endif
    pmove_spawn(lv.start);
    cam.yaw = lv.start_yaw;
    cam.pitch = 0;
    /* trace costs, for the stats line */
    {
        static const s32 zero[3] = { 0, 0, 0 };
        s32 a[3], b[3];
        u32 t;
        int k, n;

        for (k = 0; k < 3; ++k)
            a[k] = b[k] = pl.origin[k];
        b[0] -= FIX(400);
        b[1] += FIX(300);
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_line(a, b, 0, CONTENTS_SOLID);
        bench_us[0] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, zero, zero, 0, CONTENTS_SOLID);
        bench_us[1] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, p_mins, p_maxs, 0, MASK_PLAYERSOLID);
        bench_us[2] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        b[0] = a[0] + FIX(10);
        b[1] = a[1];
        b[2] = a[2] - FIX(18);
        t = frt_read();
        for (n = 0; n < 10; ++n)
            trace_box(a, b, p_mins, p_maxs, 0, MASK_PLAYERSOLID);
        bench_us[3] = frt_to_us((frt_read() - t) & 0xFFFF) / 10;
        {
            q_trace tl = trace_line(a, b, 0, CONTENTS_SOLID);

            bench_us[4] = tl.fraction;
        }
    }
    cycles_measure();
    t_last = frt_read();
    pad_by_vblank(vdp_get_pipelined());       /* (the SMPC's reads in the picture: engine/sys.c) */
    pad_request();
    for (;;)
    {
        q_usercmd   cmd;
        s32         dt = (s32)(((u64)imax(imin((s32)us_frame, 100000), 10000) << 16) / 1000000);
        int         turn = (int)fmul(dt, 0x6000);   /* 135 degrees a second */

        FT(0);

#ifdef SOUND_TEST
        {
            /* (OPT=-DSOUND_TEST: the blaster hard left, then hard right, then an explosion in the middle) */
            static int sf;

            ++sf;
            if (sf == 60)
                snd_sfx_at(SND_BLASTER, 127, -15);
            if (sf == 120)
                snd_sfx_at(SND_BLASTER, 127, 15);
            if (sf == 180)
                snd_sfx_at(SND_EXPLOSION, 127, 0);
        }
#endif
        pad_prev = pad_now;
        pad_now = pad_collect();
        pad_request();
        t0 = frt_read();
        /* a menu up: it has the presses and the game stands still (START + R still starts a benchmark) */
        paused = false;
        if (menu_active() && !(pad_now & PAD_START && pressed(PAD_R)))
        {
            menu_action a = menu_input((u16)(pad_now & ~pad_prev));

#ifndef GUNNER_TEST
            if (a == MA_NEW_GAME && !same(cur_map, "DEMO1.MAP"))
                load_level("demo1", NULL, false);       /* a new game's from the first level */
#else
            if (0)
                ;                                       /* (the gunner's level stays) */
#endif
            else if (a == MA_NEW_GAME || a == MA_RESTART || a == MA_TITLE)
                new_game();
            paused = menu_active();
            if (pad_now & PAD_START)
                start_used = true;              /* (letting go of a START that chose something isn't a pause) */
        }
        if (g_player->dead || level_complete)
        {
            /* dead: START to go again (from the start, with what you had); level done: the level again */
            pad_now &= PAD_START;
            if (pad_prev & PAD_START && !(pad_now & PAD_START))
            {
                if (level_complete)
                {
                    /* through the exit to the next level (its name, then the start you come in at);
                       not on the disc (the demo's victory screen): the end, back to the title */
                    const char *n = g_next_map, *spot = NULL, *d;

                    if (n)
                    {
                        for (d = n; *d && *d != '$'; ++d)
                            ;
                        if (*d == '$')
                            spot = d + 1;
                    }
                    if (!n || !load_level(n, spot, true))
                    {
                        if (!same(cur_map, "DEMO1.MAP"))
                            load_level("demo1", NULL, false);
                        else
                            new_game();
                        menu_open(MENU_MAIN);
                    }
                    g_next_map = NULL;
                }
                else
                {
                    pmove_spawn(lv.start);
                    cam.yaw = lv.start_yaw;
                    cam.pitch = 0;
                    g_player->dead = false;
                    g_player->health = 100;
                }
                start_used = true;              /* not the pause menu too */
            }
        }
        if (pad_now & PAD_START && pad_now & (PAD_A | PAD_B | PAD_C | PAD_X | PAD_Y | PAD_Z | PAD_L | PAD_R))
            start_used = true;
        if (pad_prev & PAD_START && !(pad_now & PAD_START))
        {
            if (!start_used && !menu_active())
            {
                menu_open(MENU_PAUSE);              /* on letting go, unless it was START + A/B... */
                s_play(SND_MENU_SELECT, NULL, ATTN_NONE);
                paused = true;
            }
            start_used = false;
        }
        if (!paused)
        {
            if (pressed(PAD_Y))
            {
                if (pad_now & PAD_START)
                {
                    pl.noclip = !pl.noclip;
                    start_used = true;
                }
                else
                    g_next_weapon();
            }
            if (pressed(PAD_A) && (pad_now & PAD_START) && nents)
                warp_ent(false);
            if (pressed(PAD_C) && (pad_now & PAD_START) && nents)
                warp_ent(true);
            if (pressed(PAD_X) && (pad_now & PAD_START))
                god = !god;
            if (pressed(PAD_UP) && (pad_now & PAD_START))
            {
                game_during_draw = !game_during_draw;
                g_centerprint(game_during_draw ? "Game during drawing: on" : "Game during drawing: off");
                start_used = true;
            }
#ifdef FIGHT_BENCH
            if (pressed(PAD_R) && (pad_now & PAD_START))
            {
                static const s32 at[3] = { FIX(600), FIX(-428), FIX(-96) };
                int         i;

                menu_cur = MENU_NONE;           /* (from anywhere: the level afresh, everything in it) */
                g_skill = -1;
                new_game();
                god = true;
                pmove_spawn(at);
                for (i = 1; i < MAX_EDICTS; ++i)
                {
                    g_ent   *e = &g_edicts[i];

                    if (e->kind == EK_MONSTER && !e->inactive && e->health > 0 && !e->enemy
                        && iabs(e->origin[0] - at[0]) < FIX(900) && iabs(e->origin[1] - at[1]) < FIX(900))
                    {
                        e->enemy = g_player;
                        FoundTarget(e);
                    }
                }
                fight_frames = 0;
                fight_done = false;
                counts_reset();
                fight_us = fight_cpu = fight_game = fight_gmax = fight_n = fight_ntr = fight_ttr = 0;
                memset(fight_r, 0, sizeof(fight_r));
                memset(fight_seg, 0, sizeof(fight_seg));
                fight_gun = 0;
                {
                    extern u32 view_ph[4];

                    memset(view_ph, 0, sizeof(view_ph));
                }
#ifdef R_PROFILE
                memset(fight_p, 0, sizeof(fight_p));
#endif
            }
            if (fight_frames >= 0)
            {
                cam.yaw = 0x4000;                   /* standing still, looking into the room */
                cam.pitch = 0;
                pad_now &= PAD_START;
            }
#else
            if (pressed(PAD_R) && (pad_now & PAD_START))
            {
                menu_cur = MENU_NONE;           /* (from anywhere: the level afresh, everything in it) */
                g_skill = -1;
                new_game();
                bench_view = 0;
                bench_frame = 0;
                bench_done = false;
                memset(bench_acc, 0, sizeof(bench_acc));
                counts_reset();
#ifdef R_PROFILE
                {
                    extern int wk_nodes, wk_leaves, wk_ftests, wk_models;

                    wk_nodes = wk_leaves = wk_ftests = wk_models = 0;
                }
#endif
                memset(bench_prof, 0, sizeof(bench_prof));
            }
#endif
            if (pressed(PAD_L) && (pad_now & PAD_START))
                warp_trigger();
            if (pressed(PAD_Z) && (pad_now & PAD_START))
            {
                int w;

                for (w = 0; w < W_COUNT; ++w)
                    client.have[w] = true;
                client.ammo[AMMO_SHELLS] = 100;
                client.ammo[AMMO_BULLETS] = 200;
                client.ammo[AMMO_GRENADES] = 50;
                client.ammo[AMMO_ROCKETS] = 50;
                g_centerprint("All weapons");
            }
            if (god)
                g_player->health = imax(g_player->health, 100);
            else if (pressed(PAD_B) && pad_now & PAD_START)
                warp_next();
            if (pad_now & PAD_LEFT)
                cam.yaw += turn;
            if (pad_now & PAD_RIGHT)
                cam.yaw -= turn;
            if (pad_now & PAD_X && !(pad_now & PAD_START))
                cam.pitch = imax(cam.pitch - turn / 2, -0x3800);
            if (pad_now & PAD_Z && !(pad_now & PAD_START))
                cam.pitch = imin(cam.pitch + turn / 2, 0x3800);
            if (pad_now & PAD_C && !(pad_now & PAD_START))
                cam.pitch = 0;
            cam.yaw &= 0xFFFF;
            cmd.yaw = cam.yaw;
            cmd.pitch = cam.pitch;
            cmd.forward = pad_now & PAD_UP ? FIX(300) : pad_now & PAD_DOWN ? -FIX(300) : 0;
            cmd.side = pad_now & PAD_R ? FIX(300) : pad_now & PAD_L ? -FIX(300) : 0;
            cmd.up = pad_now & PAD_A && !(pad_now & PAD_START) ? FIX(300) : 0;
#if defined(ENTLIGHT_CHECK) && !defined(FIGHT_BENCH)
            if (!paused)
            {
                cmd.forward = FIX(300);         /* (walking in wide circles: new clusters, new things in view) */
                cam.yaw = (cam.yaw + 0x60) & 0xFFFF;
                cmd.yaw = cam.yaw;
            }
#endif
#ifdef VIEW_TEST
            if (!paused)
            {
                /* (walking in circles, turning at three speeds in turn, then standing still: the
                   gun's bob and lag, and none) */
                static int vw;
                static const int turns[4] = { 0, 0x60, 0x300, 0 };

                ++vw;
                cmd.forward = vw / 100 % 4 == 3 ? 0 : FIX(300);
                cam.yaw = (cam.yaw + turns[vw / 100 % 4]) & 0xFFFF;
                cmd.yaw = cam.yaw;
            }
#endif
#ifdef LADDER_CHECK
            if (!paused)
            {
                cmd.forward = FIX(300);         /* (into the ladder, and up it) */
                cmd.up = FIX(300);
            }
#endif
        }
        if (bench_view >= 0)
        {
            /* the benchmark: the camera where the table says, the game paused */
            const s32 *bv;

#ifdef BENCH_HOLD
            /* (OPT=-DBENCH_HOLD: a view held, DOWN for the next, UP to switch the cells'
               assembly on and off: the two should be identical, pixel for pixel) */
            {
                if (pressed(PAD_DOWN))
                    bench_view = (bench_view + 1) % NBENCH;
#if defined(COMPARE_MODELS)
                if (pressed(PAD_UP))
                {
                    extern bool r_model_ref;

                    r_model_ref = !r_model_ref;
                }
#else
                if (pressed(PAD_UP))
                {
                    extern bool r_cells_asm;

                    r_cells_asm = !r_cells_asm;
                }
#endif
                bench_frame = 0;
            }
#endif
            bv = bench_views[bench_view];

            cam.pos[0] = FIX(bv[0]);
            cam.pos[1] = FIX(bv[1]);
            cam.pos[2] = FIX(bv[2]);
            cam.yaw = (int)bv[3];
#ifdef TURN_BENCH
            cam.yaw = (cam.yaw + bench_frame * (65536 / 90)) & 0xFFFF;
            view_on = true;                 /* (the turns with the gun up: its textures in the cache too) */
#endif
            cam.pitch = (int)bv[4];
            cam_update();
            render_sky();
            fx_update(0);
            g_render_ents();
            fx_render();
            ents_light();
            vdp_begin();
            r_two_cpus = slave_ok;
#ifdef ONE_CPU
            r_two_cpus = false;             /* (OPT=-DONE_CPU: the master alone, to see what sharing gains) */
#endif
            render_world(vdp_get_writer(0), vdp_get_writer(1));
            vdp_printf(8, 8, RGB(255, 255, 255), "BENCHMARK VIEW %d", bench_view + 1);
            us_cpu = frt_to_us((frt_read() - t0) & 0xFFFF);
            waited = vdp_submit();
            t0 = frt_read();
            us_frame = frt_to_us((t0 - t_last) & 0xFFFF);
            t_last = t0;
            if (bench_frame >= 2)           /* the first two: textures settling */
            {
                u32 *a = bench_acc[bench_view];

#ifdef TURN_BENCH
                a[0] += (u32)rs.uploads * 100;
#else
                a[0] += (u32)rs.nodes;
#endif
                a[1] += rs.t_face;
                a[2] += rs.t_grid;
                a[3] += us_cpu;
                a[4] += us_frame;
                a[5] += (u32)waited;
                a[6] += vdp_us_dma;
                bench_prof[0] += rs.p_setup;
                bench_prof[1] += rs.p_grid;
                bench_prof[2] += rs.p_cells;
                bench_prof[3] += rs.p_slow;
                bench_prof[4] += rs.t_models;
                bench_prof[6] += (u32)rs.nslow;
                bench_prof[7] += (u32)rs.faces;
                bench_prof[8] += rs.t_mlight;
                bench_prof[9] += rs.t_mverts;
                bench_prof[10] += rs.t_mpolys;
                bench_prof[11] += (u32)rs.nexact;
                bench_prof[13] += rs.t_mfar;
                bench_prof[14] += (u32)rs.mfar;
                bench_prof[12] += (u32)rs.models;
                bench_prof[5] += rs.us_tree;
#ifdef OCC_COUNT
                bench_prof[0] += (u32)rs.occ_faces * 100;
                bench_prof[1] += (u32)rs.occ_cells * 100;
                bench_prof[2] += (u32)rs.occ_occluders * 100;
                bench_prof[3] += (u32)rs.faces * 100;
                bench_prof[12] += (u32)rs.cells * 100;
#endif

            }
            if (++bench_frame == BENCH_FRAMES)
            {
                bench_frame = 0;
                if (++bench_view == NBENCH)
                {
                    bench_view = -1;
                    bench_done = true;
                    counts_at_end();
                    pmove_spawn(pl.origin);
                }
            }
            continue;
        }
#ifdef LEVEL_TEST
        /* (OPT=-DLEVEL_TEST: each level's exit in turn, 80 frames in) */
        if (!paused && !level_complete && !g_player->dead)
        {
            static int lt_frames, lt_n;
            static const char *exits[] = { "demo2$base1", "demo3$base2a", "demo2$base3b", "victory.pcx" };

            if (++lt_frames == 80 && lt_n < 4)
            {
                g_next_map = exits[lt_n++];
                level_complete = true;
                lt_frames = 0;
            }
        }
#endif
        FT(1);
        if (!paused)
        {
            movers_update(dt);
            pmove(&cmd, dt);
            FT(2);
            game_dt = dt;
            if (!game_during_draw)
                game_step();
        }
        else
            FT(2);
        FT(3);
        if (paused && menu_at_title())
        {
            /* the title: turning slowly where the level starts */
            static int title_yaw;

            title_yaw = (title_yaw + (int)fmul(dt, 0x0C00)) & 0xFFFF;
            cam.pos[0] = lv.start[0];
            cam.pos[1] = lv.start[1];
            cam.pos[2] = lv.start[2] + FIX(22);
            cam.yaw = (lv.start_yaw + title_yaw) & 0xFFFF;
            cam.pitch = 0;
        }
        else
        {
            cam.pos[0] = pl.origin[0];
            cam.pos[1] = pl.origin[1];
            cam.pos[2] = pl.origin[2] + (g_player->dead ? FIX(-8) : FIX(22));   /* the eyes (dead: on the floor) */
        }
#ifdef VIEW_TEST
        {
            /* (OPT=-DVIEW_TEST: no title; every gun, the next one every 2 seconds) */
            static int vt;
            int w;

            if (menu_cur == MENU_MAIN)
            {
                menu_cur = MENU_NONE;
                paused = false;
            }
            for (w = 0; w < W_COUNT; ++w)
                client.have[w] = true;
            client.ammo[AMMO_SHELLS] = client.ammo[AMMO_BULLETS] = client.ammo[AMMO_GRENADES] = 50;
            client.ammo[AMMO_ROCKETS] = 50;
            if (++vt % 50 == 0)
                g_next_weapon();
        }
#endif
        cam_update();
        view_on = bench_view < 0 && !(paused && menu_at_title());
        view_update(paused ? 0 : dt);
        render_sky();
        fx_update(paused ? 0 : dt);
        g_render_ents();
        fx_render();
        ents_light();
        /* hit: a red flash over everything (VDP2's colour offset) */
        {
            extern s32 player_flash;
            static bool flashing;

            if (client.pickup_flash > 0)
                client.pickup_flash -= dt;
            if (player_flash)
            {
                vdp_color_offset_all(player_flash >> 1, -(player_flash >> 3), -(player_flash >> 3));
                flashing = true;
            }
            else if (client.pickup_flash > 0)
            {
                int y = client.pickup_flash >> 10;      /* up to ~19 */

                vdp_color_offset_all(y * 2, y * 2, 0);  /* Quake's yellow pickup flash */
                flashing = true;
            }
            else if (flashing)
            {
                vdp_color_offset_off();
                flashing = false;
            }
        }
        FT(4);
        vdp_begin();
        r_two_cpus = slave_ok;
#ifdef ONE_CPU
            r_two_cpus = false;             /* (OPT=-DONE_CPU: the master alone, to see what sharing gains) */
#endif
        if (game_during_draw && !paused)
            r_during = game_step;
        render_world(vdp_get_writer(0), vdp_get_writer(1));
        r_during = NULL;
        FT(5);
        /* the status bar and messages; a menu over them */
        if (!(paused && menu_at_title()))
            hud_draw();
        if (paused)
            menu_draw();
        if (bench_done)
        {
            u32 tot[5] = { 0, 0, 0, 0, 0 };
            int v, k, n = BENCH_FRAMES - 2;

#ifdef TURN_BENCH
            vdp_text(8, 96, RGB(255, 220, 120), "V UPLD MAST SLAV CPU FRM  WT DMA");
# ifdef UPLOAD_CHECK
            {
                extern u32 upload_checks, upload_diffs;

                vdp_printf(8, 30, RGB(255, 255, 120), "UPLOADS %d DIFF %d", upload_checks, upload_diffs);
            }
# endif
#else
            vdp_text(8, 96, RGB(255, 220, 120), "V WALK MAST SLAV CPU FRM  WT DMA");
#endif
            for (v = 0; v < NBENCH; ++v)
            {
                u32 *a = bench_acc[v];

                vdp_printf(8, 106 + v * 9, RGB(255, 255, 255), "%d %4d %4d %4d %4d %4d %3d %3d", v + 1, a[0] / n / 100,
                           a[1] / n / 100, a[2] / n / 100, a[3] / n / 100, a[4] / n / 100, (int)(a[5] * 10 / (u32)n),
                           a[6] / n / 100);
                for (k = 0; k < 5; ++k)
                    tot[k] += a[k] / n;
            }
            vdp_printf(8, 106 + NBENCH * 9, RGB(255, 220, 120), "A %4d %4d %4d %4d %4d", tot[0] / 100,
                       tot[1] / 100, tot[2] / 100, tot[3] / 100, tot[4] / 100);
            n *= NBENCH;
            vdp_printf(8, 106 + (NBENCH + 1) * 9, RGB(160, 255, 160), "S%d G%d C%d K%d L%d M%d T%d",
                       bench_prof[0] / n / 100, bench_prof[1] / n / 100, bench_prof[2] / n / 100, bench_prof[12] / n / 100,
                       bench_prof[3] / n / 100, bench_prof[4] / n / 100, bench_prof[5] / n / 100);
            {
#ifdef WALK_CHECK
                {
                    extern int walk_total, walk_frames;

                    vdp_printf(8, 106 + (NBENCH + 3) * 9, RGB(255, 255, 120), "WALK DIFFS %d IN %d FRAMES", walk_total,
                               walk_frames);
                }
#endif
                vdp_printf(8, 106 + (NBENCH + 2) * 9, RGB(160, 255, 160), "MODELS %d, FAR %d: %d.%dMS", bench_prof[12] / n,
                           bench_prof[14] / n, bench_prof[13] / n / 1000, bench_prof[13] / n / 100 % 10);
                vdp_printf(8, 106 + (NBENCH + 3) * 9, RGB(255, 200, 160), "OUT M%d S%d OF %d LATE %d CMD %d %d %d",
                           at_end[0], at_end[1], NBENCH * BENCH_FRAMES, at_end[12], at_end[2], at_end[3], at_end[4]);
#ifdef TEX_WSET
                vdp_printf(8, 106 + (NBENCH + 4) * 9, RGB(255, 200, 160), "TEX M%d/%d S%d/%d OF %d",
                           at_end[9] / (NBENCH * BENCH_FRAMES), at_end[7], at_end[10] / (NBENCH * BENCH_FRAMES), at_end[8],
                           at_end[11]);
#endif
            }
        }
#ifdef FIGHT_BENCH
        if (fight_done)
        {
            u32 n = fight_n ? fight_n : 1;
#ifdef MODEL_CHECK
            {
                extern u32 model_checks[4], model_diffs[3];

                vdp_printf(8, 30, RGB(255, 255, 120), "VERTS %d DIFF %d CMDS %d DIFF %d", model_checks[0], model_diffs[0],
                           model_checks[3], model_diffs[2]);
                vdp_printf(8, 39, RGB(255, 255, 120), "BUCKETS %d DIFF %d QUADS %d", model_checks[1], model_diffs[1],
                           model_checks[2]);
            }
#endif
#ifdef ENTLIGHT_CHECK
            {
                extern u32 el_checks, el_diffs;

                vdp_printf(8, 39, RGB(255, 255, 120), "MODELS DRAWN %d STALE LIGHT %d", el_checks, el_diffs);
            }
#endif
#ifdef DL_CHECK
            {
                extern u32 dl_checks, dl_diffs;

                vdp_printf(8, 30, RGB(255, 255, 120), "DLIGHT CORNERS %d DIFF %d", dl_checks, dl_diffs);
            }
#endif
#ifdef TRACE_CHECK
            {
                extern u32 trace_checks, trace_diffs, trace_dkind[4], trace_later;
                extern s32 trace_worst;

                vdp_printf(8, 30, RGB(255, 255, 120), "CHECKED %d DIFF %d LATER %d WORST %d", trace_checks,
                           trace_diffs, trace_later, trace_worst);
                vdp_printf(8, 39, RGB(255, 255, 120), "START %d ALL %d PLANE %d NORMAL %d", trace_dkind[0],
                           trace_dkind[1], trace_dkind[2], trace_dkind[3]);
            }
#endif

            vdp_printf(8, 96, RGB(255, 220, 120), "FIGHT: %d FRAMES", fight_n);
            vdp_printf(8, 106, RGB(255, 255, 255), "FRAME %d.%d CPU %d.%d MS", fight_us / n / 1000,
                       fight_us / n / 100 % 10, fight_cpu / n / 1000, fight_cpu / n / 100 % 10);
            vdp_printf(8, 115, RGB(255, 255, 255), "GAME %d.%d MS, MOST %d.%d", fight_game / n / 1000,
                       fight_game / n / 100 % 10, fight_gmax / 1000, fight_gmax / 100 % 10);
            vdp_printf(8, 124, RGB(255, 255, 255), "TRACES %d A FRAME, %d.%d MS", fight_ntr / n,
                       fight_ttr / n / 1000, fight_ttr / n / 100 % 10);
            vdp_printf(8, 133, RGB(255, 255, 255), "I%d P%d G%d B%d W%d A%d", fight_seg[0] / n / 100,
                       fight_seg[1] / n / 100, fight_seg[2] / n / 100, fight_seg[3] / n / 100, fight_seg[4] / n / 100,
                       fight_seg[5] / n / 100);
#define MS10(v)     (int)((v) / n / 100)
            vdp_printf(8, 142, RGB(160, 255, 160), "(0.1 MS) MASTER %d SLAVE %d", MS10(fight_r[0]), MS10(fight_r[1]));
            vdp_printf(8, 151, RGB(160, 255, 160), "MODELS %d.%d CPU %d.%d DSP %d.%d: %d", fight_r[2] * 10 / n / 10,
                       fight_r[2] * 10 / n % 10, fight_r[10] * 10 / n / 10, fight_r[10] * 10 / n % 10,
                       fight_r[11] * 10 / n / 10, fight_r[11] * 10 / n % 10, MS10(fight_r[7]));
            vdp_printf(8, 160, RGB(160, 255, 160), "L%d V%d(W%d A%d N%d) P%d C%d", MS10(fight_r[4]),
                       MS10(fight_r[5] - fight_r[4]), MS10(fight_r[8]), MS10(fight_r[12]), MS10(fight_r[13]),
                       MS10(fight_r[6] - fight_r[5]), MS10(fight_r[7] - fight_r[6]));
#ifdef R_PROFILE
            vdp_printf(8, 178, RGB(255, 200, 160), "S%d G%d C%d L%d F%d C%d X%d L%d", MS10(fight_p[0]),
                       MS10(fight_p[1]), MS10(fight_p[2]), MS10(fight_p[3]), fight_p[4] / n, fight_p[5] / n,
                       fight_p[6] / n, fight_p[7] / n);
            vdp_printf(8, 187, RGB(255, 200, 160), "CROP %d EXACT %d SMALL %d", fight_p[9] / n, fight_p[10] / n,
                       fight_p[12] / n);
#endif
            vdp_printf(8, 169, RGB(160, 255, 160), "UPLOADS %d.%d, MODELS' %d.%d GUN %d.%d", fight_r[9] / 1000 * 10 / n / 10,
                       fight_r[9] / 1000 * 10 / n % 10, fight_r[9] % 1000 * 10 / n / 10, fight_r[9] % 1000 * 10 / n % 10,
                       fight_gun / n / 1000, fight_gun / n / 100 % 10);
            vdp_printf(8, 196, RGB(255, 200, 160), "GUN US V%d S%d C%d K%d", fight_vph[0] / n, fight_vph[1] / n,
                       fight_vph[2] / n, fight_vph[3] / n);
            vdp_printf(8, 205, RGB(255, 200, 160), "OUT M%d S%d OF %d LATE %d CMD %d %d %d", at_end[0], at_end[1],
                       fight_n + FIGHT_SKIP, at_end[12], at_end[2], at_end[3], at_end[4]);
#ifdef TEX_WSET
            vdp_printf(8, 214, RGB(255, 200, 160), "TEX M%d/%d S%d/%d OF %d", at_end[9] / (fight_n + FIGHT_SKIP),
                       at_end[7], at_end[10] / (fight_n + FIGHT_SKIP), at_end[8], at_end[11]);
#endif
#undef MS10
#ifdef FIGHT_TRACES
            {
                int j;

                vdp_printf(8, 133, RGB(255, 200, 160), "BOX %d: BOXOUT%d L%d B%d S%d MOV%d", fight_tr[4] / n,
                           fight_tr[0] / imax(fight_tr[4], 1), fight_tr[1] / imax(fight_tr[4], 1),
                           fight_tr[2] / imax(fight_tr[4], 1), fight_tr[3] / imax(fight_tr[4], 1), fight_tr[5] / n);
                vdp_printf(8, 181, RGB(255, 200, 160), "US G%d C%d M%d E%d", fight_tt[0] / 10,
                           fight_tt[1] / 10, fight_tt[2] / 10, fight_tt[3] / 10);
                for (j = 0; j < 4 && fight_sites[j].n; ++j)
                    vdp_printf(8, 145 + 9 * j, RGB(160, 255, 160), "%X %d.%d A FRAME %d.%dMS",
                               fight_sites[j].at & 0xFFFFF, fight_sites[j].n * 10 / n / 10,
                               fight_sites[j].n * 10 / n % 10, fight_sites[j].us / n / 1000,
                               fight_sites[j].us / n / 100 % 10);
            }
#endif
            vdp_printf(8, 190, RGB(255, 255, 255), "UP 20:%d 40:%d 60:%d 80:%d 100+:%d", fight_swaps[1],
                       fight_swaps[2], fight_swaps[3], fight_swaps[4], fight_swaps[5] + fight_swaps[6] + fight_swaps[7]);
        }
#endif
#ifdef LEVEL_TEST
        {
            int k;

            for (k = 0; k < 3; ++k)
                vdp_printf(8, 30 + k * 9, RGB(255, 255, 120), "HW %d LW %d CA %d COLD %d", lt_hw[k][0], lt_hw[k][1],
                           lt_hw[k][2], lt_hw[k][3]);
        }
#endif
        if (level_complete)
        {
            vdp_text(160 - 7 * 8, 60, RGB(255, 220, 120), "LEVEL COMPLETE");
            vdp_printf(160 - 9 * 8, 80, RGB(255, 255, 255), "KILLS   %d / %d", kills, total_monsters);
            vdp_printf(160 - 9 * 8, 92, RGB(255, 255, 255), "SECRETS %d / %d", found_secrets, total_secrets);
            if (g_next_map && g_next_map[0] == 'v')
                vdp_text(160 - 14 * 4, 116, RGB(255, 220, 120), "THE END OF THE DEMO");    /* ("victory.pcx") */
            vdp_text(160 - 20 * 4, 132, RGB(200, 200, 200), "PRESS START TO GO ON");
        }
        {
            if (opt_stats)
                vdp_printf(8, 190, RGB(255, 255, 255), "LINE %d POINTBOX %d BOX %d SHORT %d", bench_us[0], bench_us[1],
                           bench_us[2], bench_us[3]);
            if (g_player->dead)
                vdp_text(160 - 11 * 8, 100, RGB(255, 80, 60), "YOU DIED - PRESS START");
        }
        if (opt_stats)
        {
            u16 c = RGB(255, 255, 255);

            {
                extern s32 cyc[12];

                /* cycles x 10 an operation (src/cycles.c) */
                vdp_printf(8, 88, c, "ST HW%d LW%d CA%d V1%d LD HIT%d UNC%d", cyc[1], cyc[2], cyc[10], cyc[11], cyc[3],
                           cyc[4]);
                vdp_printf(8, 98, c, "MISS HW%d LW%d CA%d DIV%d MUL%d", cyc[5], cyc[6], cyc[7], cyc[8], cyc[9]);
                extern int grid_bad;

                extern bool r_dsp_ok;

                vdp_printf(8, 108, c, "GRID PT%d BAD%d DSP %s%s HEAP %x", cyc[0], grid_bad, r_dsp_ok ? "OK" : "BAD",
                           r_use_dsp ? " ON" : "", level_heap());
#ifdef WALK_CHECK
                {
                    extern int walk_diff, walk_len, walk_clen, walk_first, walk_total, walk_frames;

                    vdp_printf(8, 118, c, "WALK DIFF %d ASM %d C %d AT %d", walk_diff, walk_len, walk_clen, walk_first);
                    vdp_printf(8, 30, c, "WALK TOTAL %d IN %d FRAMES", walk_total, walk_frames);
                }
#endif

            }
            vdp_printf(8, 48, c, "%d %d %d %X%s%s%s W%d", pl.origin[0] >> 16, pl.origin[1] >> 16, pl.origin[2] >> 16,
                       cam.yaw, pl.on_ground ? " GROUND" : "", pl.noclip ? " NOCLIP" : "", god ? " GOD" : "",
                       pl.waterlevel);
#ifdef LADDER_CHECK
            {
                extern u32 lc_frames, lc_near, lc_ladder, lc_bad;

                vdp_printf(8, 8, c, "LADDER F%d NEAR%d ON%d BAD%d", lc_frames, lc_near, lc_ladder, lc_bad);
            }
#elif defined(ENTLIGHT_CHECK)
            {
                extern u32 el_checks, el_diffs;

                vdp_printf(8, 8, c, "MODELS DRAWN %d STALE LIGHT %d", el_checks, el_diffs);
            }
#elif defined(VIEW_CHECK)
            {
                extern u32 view_checks[7];

                vdp_printf(8, 8, c, "GUN CHECK F%d CMDS %d DIFF %d", view_checks[0], view_checks[1], view_checks[2]);
#if VIEW_CHECK == 3
                vdp_printf(8, 208, c, "DSP V%d MOVED %d MOST %d NEAR %d", view_checks[3], view_checks[4],
                           view_checks[5], view_checks[6]);
#elif VIEW_CHECK == 2
                vdp_printf(8, 208, c, "KEPT: TURNED EDGE-ON %d", view_checks[3]);
#endif
            }
#else
            vdp_printf(8, 8, c, "FPS %d.%d  CPU %dMS  WAIT %d", 10000000 / (us_frame ? us_frame : 1) / 10,
                       10000000 / (us_frame ? us_frame : 1) % 10, us_cpu / 1000, waited);
#endif
            vdp_printf(8, 18, c, "FACES %d CELLS %d CULL %d NEAR %d", rs.faces, rs.cells, rs.culled, rs.near);
            vdp_printf(8, 28, c, "UPLOADS %d FULL %d DROP %d CMDS %d", rs.uploads, rs.nocache, rs.dropped,
                       vdp_cmd_count());
            vdp_printf(8, 38, c, "WALK %d MASTER %d SLAVE %d OUT %d %d", rs.nodes / 1000, rs.t_face / 1000,
                       rs.t_grid / 1000, r_full[0], r_full[1]);
#ifdef TEX_WSET
            {
                extern u32 r_wset[5];

                vdp_printf(8, 150, c, "TEX MOST %d %d OF %d", r_wset[0], r_wset[1], r_wset[4]);
            }
#endif
            vdp_printf(8, 58, c, "MODELS %d POLYS %d %dUS", rs.models, rs.mpolys, rs.t_models);
            {
                extern int g_ntraces;
                extern u32 g_trace_ticks;
                static int nt;
                static u32 tt;

                if (us_game > 100)
                {
                    nt = g_ntraces;
                    tt = frt_to_us(g_trace_ticks);
                }
                {
                    extern int fx_explosions, g_uses;

                    vdp_printf(8, 68, c, "GAME %dUS TRACES %d %dUS", us_game, nt, tt);
                    vdp_printf(8, 78, c, "USES %d EXPL %d KILLS %d/%d", g_uses, fx_explosions, kills, total_monsters);
                }
                g_ntraces = 0;
                g_trace_ticks = 0;
            }

        }
        FT(6);
        us_cpu = frt_to_us((frt_read() - t0) & 0xFFFF);
        waited = vdp_submit();
        t0 = frt_read();
        us_frame = frt_to_us((t0 - t_last) & 0xFFFF);
        t_last = t0;
#ifdef FIGHT_BENCH
        if (fight_frames >= 0)
        {
            extern int  g_ntraces;
            extern u32  g_trace_ticks;
            int         k;

            if (fight_frames >= FIGHT_SKIP)
            {
                fight_ntr += (u32)g_ntraces;
                fight_ttr += frt_to_us(g_trace_ticks);
            }
            g_ntraces = 0;
            g_trace_ticks = 0;
            if (++fight_frames == FIGHT_SKIP)
            {
                for (k = 0; k < 8; ++k)
                    fight_swaps[k] = vdp_swap_fields[k];
                memset(g_trace_sites, 0, sizeof(g_trace_sites));
                {
                    extern u32 tr_count[6], tr_ticks[4];

                    memset(tr_count, 0, sizeof(tr_count));
                    memset(tr_ticks, 0, sizeof(tr_ticks));
                }
            }
            else if (fight_frames > FIGHT_SKIP)
            {
                fight_us += us_frame;
                fight_cpu += us_cpu;
                fight_game += us_game;
                {
                    int k;

                    for (k = 0; k < 6; ++k)
                        fight_seg[k] += frt_to_us((fight_t[k + 1] - fight_t[k]) & 0xFFFF);
                }
                fight_r[0] += rs.t_face;
                fight_r[1] += rs.t_grid;
                fight_r[2] += (u32)rs.models;
                fight_r[3] += (u32)rs.mpolys;
                fight_r[4] += rs.t_mlight;
                fight_r[5] += rs.t_mverts;
                fight_r[6] += rs.t_mpolys;
                fight_r[7] += rs.t_models;
                fight_r[8] += rs.t_mwait;
                fight_r[9] += (u32)rs.uploads * 1000 + (u32)rs.muploads;
                fight_r[10] += (u32)rs.mcpu;
                fight_r[11] += (u32)rs.mdsp;
#ifdef R_PROFILE
                fight_p[0] += rs.p_setup;
                fight_p[1] += rs.p_grid;
                fight_p[2] += rs.p_cells;
                fight_p[3] += rs.p_slow;
                fight_p[4] += (u32)rs.faces;
                fight_p[5] += (u32)rs.cells;
                fight_p[6] += (u32)rs.nexact;
                fight_p[7] += (u32)rs.nslow;
                fight_p[8] += (u32)rs.ns_dl;
                fight_p[9] += (u32)rs.ns_crop;
                fight_p[10] += (u32)rs.ns_exact;
                fight_p[11] += (u32)rs.g_same;
                fight_p[12] += (u32)rs.ns_small;
#endif
                fight_r[12] += rs.t_masm;
                fight_gun += rs.t_view;
                {
                    extern u32 view_ph[4];

                    memcpy(fight_vph, view_ph, sizeof(fight_vph));
                }
                fight_r[13] += rs.t_mnorm;
                fight_gmax = imax((s32)fight_gmax, (s32)us_game);
                ++fight_n;
                if (fight_us >= 20000000)
                {
                    for (k = 0; k < 8; ++k)
                        fight_swaps[k] = vdp_swap_fields[k] - fight_swaps[k];
                    memcpy(fight_sites, g_trace_sites, sizeof(fight_sites));
                    {
                        extern u32 tr_count[6], tr_ticks[4];

                        memcpy(fight_tr, tr_count, sizeof(fight_tr));
                        for (k = 0; k < 4; ++k)
                            fight_tt[k] = frt_to_us(tr_ticks[k]) * 10 / imax(tr_count[4], 1);
                    }
                    for (k = 1; k < 16; ++k)
                    {
                        g_trace_site x = fight_sites[k];
                        int          j = k;

                        for (; j > 0 && fight_sites[j - 1].us < x.us; --j)
                            fight_sites[j] = fight_sites[j - 1];
                        fight_sites[j] = x;
                    }
                    fight_frames = -1;
                    fight_done = true;
                    counts_at_end();
                }
            }
        }
#endif
    }
}
