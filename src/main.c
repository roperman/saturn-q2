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
static bool         show_stats = true;

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
static u32          us_game, bench_us[5];

/* the benchmark (START + R): fixed views, 16 frames each, the game paused. x y z (eye), yaw, pitch */
static const s32    bench_views[][5] = {
    { 128, -320, 46, 0x6000, 0 },           /* the start, looking down the hall */
    { -1064, 1632, -2, 0x8000, 0 },         /* by the door to the crate room */
    { -1960, 1444, -2, 0xC000, 0 },         /* the dark corridor */
    { 600, -428, -74, 0x4000, 0 },          /* the round room with the soldiers */
    { 20, -213, 46, 0x7000, 0x800 },        /* the hall, turned */
    { -1636, 1488, 142, 0x0000, 0 },        /* the sliding doors */
};
#define NBENCH          ((int)(sizeof(bench_views) / sizeof(bench_views[0])))
#define BENCH_FRAMES    (16)
static int          bench_view = -1, bench_frame;
static u32          bench_acc[NBENCH][5];   /* walk, master, slave, cpu, frame (us, summed) */
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

void                main(void)
{
    u32             t_last, t0, us_frame = 0, us_cpu = 0;
    int             waited = 0;

    frt_init();
#ifndef NO_SLAVE
    slave_start();
    wait_signal();                              /* the slave says hello */
    slave_ok = true;
#endif
    vdp_init(RGB(0, 0, 0));
    vdp_set_hw_erase(false);
    vdp_set_min_frame(1);
    message("QUAKE II", "LOADING DEMO1 ONTO THE RAM CART");
    if (!level_load(MAP_FILE))
        for (;;)
            message(cart_mb < 4 ? "THIS NEEDS THE 4MB RAM CART" : MAP_FILE " WON'T LOAD", NULL);
    message("QUAKE II", "LOADING THE MODELS");
    models_load_all();
    hud_init();
    render_init();
    trace_init();
    movers_init();
    g_init();
    render_sky_init();
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
    pad_request();
    for (;;)
    {
        q_usercmd   cmd;
        s32         dt = (s32)(((u64)imax(imin((s32)us_frame, 100000), 10000) << 16) / 1000000);
        int         turn = (int)fmul(dt, 0x6000);   /* 135 degrees a second */

        pad_prev = pad_now;
        pad_now = pad_collect();
        pad_request();
        t0 = frt_read();
        if (g_player->dead || level_complete)
        {
            /* dead: START to go again (from the start, with what you had); level done: the level again */
            pad_now &= PAD_START;
            if (pad_prev & PAD_START && !(pad_now & PAD_START))
            {
                if (level_complete)
                {
                    movers_init();
                    g_init();
                }
                pmove_spawn(lv.start);
                cam.yaw = lv.start_yaw;
                cam.pitch = 0;
                g_player->dead = false;
                g_player->health = 100;
                start_used = true;              /* not the stats too */
            }
        }
        if (pad_now & PAD_START && pad_now & (PAD_A | PAD_B | PAD_C | PAD_X | PAD_Y | PAD_Z | PAD_L | PAD_R))
            start_used = true;
        if (pad_prev & PAD_START && !(pad_now & PAD_START))
        {
            if (!start_used)
                show_stats = !show_stats;               /* on letting go, unless it was START + A/B */
            start_used = false;
        }
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
        if (pressed(PAD_R) && (pad_now & PAD_START))
        {
            bench_view = 0;
            bench_frame = 0;
            bench_done = false;
            memset(bench_acc, 0, sizeof(bench_acc));
            memset(bench_prof, 0, sizeof(bench_prof));
        }
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
        if (bench_view >= 0)
        {
            /* the benchmark: the camera where the table says, the game paused */
            const s32 *bv;

#ifdef BENCH_HOLD
            /* (OPT=-DBENCH_HOLD: a view held, DOWN for the next, UP to switch the cells'
               assembly on and off: the two should be identical, pixel for pixel) */
            {
                extern bool r_cells_asm;

                if (pressed(PAD_DOWN))
                    bench_view = (bench_view + 1) % NBENCH;
                if (pressed(PAD_UP))
                    r_cells_asm = !r_cells_asm;
                bench_frame = 0;
            }
#endif
            bv = bench_views[bench_view];

            cam.pos[0] = FIX(bv[0]);
            cam.pos[1] = FIX(bv[1]);
            cam.pos[2] = FIX(bv[2]);
            cam.yaw = (int)bv[3];
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

                a[0] += (u32)rs.nodes;
                a[1] += rs.t_face;
                a[2] += rs.t_grid;
                a[3] += us_cpu;
                a[4] += us_frame;
                bench_prof[0] += rs.p_setup;
                bench_prof[1] += rs.p_grid;
                bench_prof[2] += rs.p_cells;
                bench_prof[3] += rs.p_slow;
                bench_prof[4] += rs.t_models;
                bench_prof[5] += (u32)rs.nfast;
                bench_prof[6] += (u32)rs.nslow;
                bench_prof[7] += (u32)rs.faces;
                bench_prof[8] += rs.t_mlight;
                bench_prof[9] += rs.t_mverts;
                bench_prof[10] += rs.t_mpolys;
                bench_prof[11] += (u32)rs.nexact;
                bench_prof[12] += rs.p_corners;
                bench_prof[13] += (u32)rs.gverts;
                bench_prof[14] += (u32)rs.culled;
            }
            if (++bench_frame == BENCH_FRAMES)
            {
                bench_frame = 0;
                if (++bench_view == NBENCH)
                {
                    bench_view = -1;
                    bench_done = true;
                    pmove_spawn(pl.origin);
                }
            }
            continue;
        }
        movers_update(dt);
        pmove(&cmd, dt);
        {
            u32 tg = frt_read();

            g_player_fire(pad_now & PAD_B && !(pad_now & PAD_START), cam.pos, cam.yaw, cam.pitch);
        g_frame(dt);
            us_game = frt_to_us((frt_read() - tg) & 0xFFFF);
        }
        cam.pos[0] = pl.origin[0];
        cam.pos[1] = pl.origin[1];
        cam.pos[2] = pl.origin[2] + (g_player->dead ? FIX(-8) : FIX(22));   /* the eyes (dead: on the floor) */
        cam_update();
        render_sky();
        fx_update(dt);
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
        vdp_begin();
        r_two_cpus = slave_ok;
#ifdef ONE_CPU
            r_two_cpus = false;             /* (OPT=-DONE_CPU: the master alone, to see what sharing gains) */
#endif
        render_world(vdp_get_writer(0), vdp_get_writer(1));
        /* the status bar and messages */
        hud_draw();
        if (bench_done)
        {
            u32 tot[5] = { 0, 0, 0, 0, 0 };
            int v, k, n = BENCH_FRAMES - 2;

            vdp_text(8, 96, RGB(255, 220, 120), "V WALK MAST SLAV CPU FRM");
            for (v = 0; v < NBENCH; ++v)
            {
                u32 *a = bench_acc[v];

                vdp_printf(8, 106 + v * 9, RGB(255, 255, 255), "%d %4d %4d %4d %4d %4d", v + 1, a[0] / n / 100,
                           a[1] / n / 100, a[2] / n / 100, a[3] / n / 100, a[4] / n / 100);
                for (k = 0; k < 5; ++k)
                    tot[k] += a[k] / n;
            }
            vdp_printf(8, 106 + NBENCH * 9, RGB(255, 220, 120), "A %4d %4d %4d %4d %4d", tot[0] / 100,
                       tot[1] / 100, tot[2] / 100, tot[3] / 100, tot[4] / 100);
            n *= NBENCH;
            vdp_printf(8, 106 + (NBENCH + 1) * 9, RGB(160, 255, 160), "S%d G%d C%d K%d L%d M%d",
                       bench_prof[0] / n / 100, bench_prof[1] / n / 100, bench_prof[2] / n / 100, bench_prof[12] / n / 100,
                       bench_prof[3] / n / 100, bench_prof[4] / n / 100);
            vdp_printf(8, 106 + (NBENCH + 2) * 9, RGB(160, 255, 160), "F%d L%d X%d Q%d P%d MV%d", bench_prof[5] / n,
                       bench_prof[6] / n, bench_prof[11] / n, bench_prof[14] / n, bench_prof[13] / n, bench_prof[9] / n / 100);
        }
        if (level_complete)
        {
            vdp_text(160 - 7 * 8, 60, RGB(255, 220, 120), "LEVEL COMPLETE");
            vdp_printf(160 - 9 * 8, 80, RGB(255, 255, 255), "KILLS   %d / %d", kills, total_monsters);
            vdp_printf(160 - 9 * 8, 92, RGB(255, 255, 255), "SECRETS %d / %d", found_secrets, total_secrets);
            vdp_text(160 - 11 * 8, 116, RGB(200, 200, 200), "PRESS START TO GO AGAIN");
        }
        {
            if (show_stats)
                vdp_printf(8, 190, RGB(255, 255, 255), "LINE %d POINTBOX %d BOX %d SHORT %d", bench_us[0], bench_us[1],
                           bench_us[2], bench_us[3]);
            if (g_player->dead)
                vdp_text(160 - 11 * 8, 100, RGB(255, 80, 60), "YOU DIED - PRESS START");
        }
        if (show_stats)
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

            }
            vdp_printf(8, 48, c, "%d %d %d%s%s%s W%d M%d G%d", pl.origin[0] >> 16, pl.origin[1] >> 16, pl.origin[2] >> 16,
                       pl.on_ground ? " GROUND" : "", pl.noclip ? " NOCLIP" : "", god ? " GOD" : "", pl.waterlevel,
                       warp_m, pl.ground_ent);
            vdp_printf(8, 8, c, "FPS %d.%d  CPU %dMS  WAIT %d", 10000000 / (us_frame ? us_frame : 1) / 10,
                       10000000 / (us_frame ? us_frame : 1) % 10, us_cpu / 1000, waited);
            vdp_printf(8, 18, c, "FACES %d CELLS %d CULL %d NEAR %d", rs.faces, rs.cells, rs.culled, rs.near);
            vdp_printf(8, 28, c, "UPLOADS %d FULL %d DROP %d CMDS %d", rs.uploads, rs.nocache, rs.dropped,
                       vdp_cmd_count());
            vdp_printf(8, 38, c, "WALK %d MASTER %d SLAVE %d", rs.nodes / 1000, rs.t_face / 1000, rs.t_grid / 1000);
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
        us_cpu = frt_to_us((frt_read() - t0) & 0xFFFF);
        waited = vdp_submit();
        t0 = frt_read();
        us_frame = frt_to_us((t0 - t_last) & 0xFFFF);
        t_last = t0;
    }
}
