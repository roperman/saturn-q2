/*
** The status bar: Quake 2's own pictures (tools/bake_hud.py), 8-bit, drawn as
** VDP1 sprites in 256-colour bank mode against Quake's palette in VDP2
** colour RAM (at 0x200: the sky has 0x100). Health, ammo and armour along the
** bottom as in Quake, with its yellow numbers when health is low; messages
** in the middle.
*/
#include "game.h"

#define CRAM_BANK       (0x200)
#define MAX_PICS        (64)

typedef struct { u16 srca, size, w, h; char name[12]; } t_pic;

static t_pic        pics[MAX_PICS];
static int          npics;
/* the status bar's pictures, found once (hud_init): the names compared each frame were a tenth of a
   millisecond, and the slave draws the HUD now (main.c: at its job's start, into its own overlays) */
static const t_pic  *p_health, *p_ammo[AMMO_COUNT], *p_armor[3], *p_quad, *p_ch, *p_num[11], *p_anum[11];
static const u16    *hud_pal;               /* Quake's palette (on the cart) */

/* the palette into VDP2's colour RAM, at the brightness chosen */
void                hud_palette(void)
{
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    int             i;

    if (hud_pal)
        for (i = 0; i < 256; ++i)
            cram[CRAM_BANK + i] = r_gamma(hud_pal[i]);
}

static const t_pic  *pic(const char *name);

__attribute__((cold)) void                hud_init(void)
{
    const u8        *b = cart_load("HUD.BIN");
    const u8        *tab, *data;
    u32             base;
    int             i, k;

    if (!b || memcmp(b, "Q2HD", 4))
        return;
    npics = imin(((const u16 *)b)[2], MAX_PICS);
    hud_pal = (const u16 *)(b + 8);
    hud_palette();
    tab = b + 8 + 512;
    data = tab + npics * 20;
    base = vdp_tex_mark();
    for (i = 0; i < npics; ++i)
    {
        const u8    *t = tab + i * 20;
        u16         w = (u16)(t[12] << 8 | t[13]), h = (u16)(t[14] << 8 | t[15]);
        u32         ofs = (u32)t[16] << 24 | (u32)t[17] << 16 | (u32)t[18] << 8 | t[19];

        for (k = 0; k < 12; ++k)
            pics[i].name[k] = (char)t[k];
        pics[i].w = w;
        pics[i].h = h;
        pics[i].srca = (u16)(base >> 3);
        pics[i].size = (u16)(((w >> 3) << 8) | h);
        memcpy((u8 *)VDP1_VRAM + base, data + ofs, (u32)w * h);
        base = (base + (u32)w * h + 31) & ~31u;
    }
    vdp_tex_release(base);
    {
        static const char *ammo_pic[AMMO_COUNT] = { "a_blaster", "a_shells", "a_bullets", "a_grenades", "a_rockets",
                                                    NULL, NULL };
        char        name[12] = "num_0";

        p_health = pic("i_health");
        for (i = 0; i < AMMO_COUNT; ++i)
            p_ammo[i] = ammo_pic[i] ? pic(ammo_pic[i]) : NULL;
        p_armor[0] = pic("i_jacketarmor");
        p_armor[1] = pic("i_combatarmor");
        p_armor[2] = pic("i_bodyarmor");
        p_quad = pic("p_quad");
        p_ch = pic("ch1");
        for (i = 0; i < 10; ++i)
        {
            name[4] = (char)('0' + i);
            p_num[i] = pic(name);
            name[0] = 'a'; name[1] = 'n'; name[2] = 'u'; name[3] = 'm'; name[4] = '_'; name[5] = (char)('0' + i); name[6] = 0;
            p_anum[i] = pic(name);
            name[0] = 'n'; name[1] = 'u'; name[2] = 'm'; name[3] = '_'; name[5] = 0;
        }
        p_num[10] = pic("num_minus");
        p_anum[10] = pic("anum_minus");
    }
    vdp_text_colr(RGB(255, 255, 255));     /* (the messages' colour given its LUT now, by the master) */
}

static const t_pic  *pic(const char *name)
{
    int             i, k;

    for (i = 0; i < npics; ++i)
    {
        for (k = 0; k < 12 && name[k] && pics[i].name[k] == name[k]; ++k)
            ;
        if (k == 12 || (!name[k] && !pics[i].name[k]))
            return &pics[i];
    }
    return NULL;
}

static void         draw_pic(const t_pic *p, int x, int y)
{
    vdp1_cmd        *c;

    if (!p || !(c = vdp_overlay()))
        return;
    c->ctrl |= VDP1_SPRITE;
    c->pmod = PMOD_ECD | (4 << 3);          /* 256-colour bank */
    c->colr = CRAM_BANK;
    c->srca = p->srca;
    c->size = p->size;
    c->xa = (s16)x;
    c->ya = (s16)y;
}

int                 hud_pic(const char *name, int x, int y)
{
    const t_pic     *p = pic(name);

    draw_pic(p, x, y);
    return p ? p->w : 0;
}

/* a number right-aligned in a field of 3 (Quake's SCR_DrawField), 16 pixels a digit */
static void         draw_field(int x, int y, int value, bool alt)
{
    char            buf[8];
    int             n, i;

    n = fmt(buf, "%d", imin(imax(value, -99), 999));
    x += 16 * (3 - n);
    for (i = 0; i < n; ++i, x += 16)
    {
        int d = buf[i] == '-' ? 10 : buf[i] - '0';

        draw_pic(alt ? p_anum[d] : p_num[d], x, y);     /* (anum_: the yellow ones) */
    }
}

/* a message: centred, lines split at \n and at 38 characters */
static void         draw_center(const char *m, int y)
{
    char            line[40];
    int             n;

    while (*m)
    {
        for (n = 0; m[n] && m[n] != '\n' && n < 38; ++n)
            line[n] = m[n];
        line[n] = 0;
        vdp_text(160 - n * 4, y, RGB(255, 255, 255), line);
        m += n;
        if (*m == '\n')
            ++m;
        y += 10;
    }
}

void                hud_draw(void)
{
    const g_weapon  *w = &weapons[client.weapon];
    int             y = SCREEN_H - 26;
    const t_pic     *ch = opt_crosshair && !g_player->dead && !menu_active() ? p_ch : NULL;

    if (ch)
        draw_pic(ch, SCREEN_W / 2 - ch->w / 2, SCREEN_H / 2 - ch->h / 2);   /* (Quake's crosshair 1) */
    if (npics)
    {
        draw_pic(p_health, 8, y);
        draw_field(34, y, g_player->health, g_player->health < 25);
        if (w->ammo != AMMO_NONE)
        {
            draw_pic(p_ammo[w->ammo], 114, y);
            draw_field(140, y, client.ammo[w->ammo], client.ammo[w->ammo] < 5);
        }
        if (client.armor)
        {
            draw_pic(p_armor[client.armor_protect >= 80 ? 2 : client.armor_protect >= 60 ? 1 : 0], 216, y);
            draw_field(242, y, client.armor, false);
        }
        if (level.time < client.quad_until)
            draw_pic(p_quad, 290, y - 28);
    }
    else
        vdp_printf(16, SCREEN_H - 20, RGB(255, 220, 120), "HEALTH %d  AMMO %d  ARMOR %d", g_player->health,
                   w->ammo != AMMO_NONE ? client.ammo[w->ammo] : 0, client.armor);
    if (center_msg && level.time < center_until)
        draw_center(center_msg, 76);
#ifdef WARP_ONLY
    {   /* (OPT=-DWARP_ONLY=MDL_x: how many of the level's entities took that model, how many are monsters now) */
        int         i, n = 0, live = 0, d = 0, dz = 0;
        const g_ent *near = NULL;
        s32         best = 0;

        for (i = 1; i < g_nfull; ++i)
            if (g_edicts[i].mdl == &models[WARP_ONLY])
            {
                const g_ent *e = &g_edicts[i];
                s32         v[3], l;
                int         k;

                ++n;
                live += e->kind == EK_MONSTER;
                for (k = 0; k < 3; ++k)
                    v[k] = e->origin[k] - g_player->origin[k];
                l = vlen(v);
                if (e->kind == EK_MONSTER && (!near || l < best))
                {
                    near = e;
                    best = l;
                    d = l >> 16;
                    dz = v[2] >> 16;
                }
            }
        {
            extern int warp_how;

            /* (and the nearest live one: how far, how high, its move's frame, its AI flags, its enemy) */
            vdp_printf(8, 20, RGB(255, 255, 120), "WARP %d OF %d%s %s D%d Z%d F%d A%X E%c", live, n,
                       models[WARP_ONLY].loaded ? "" : " NOT LOADED", warp_how == 1 ? "SPOT" : warp_how == 2 ? "OWN" : "-",
                       d, dz, near && near->move ? near->frame - near->move->first : -1, near ? near->aiflags : 0,
                       !near || !near->enemy ? '-' : near->enemy == g_player ? 'P' : 'M');
        }
#ifdef WARP_ALSO
        {   /* (OPT=-DWARP_ALSO=MDL_y: a second model's, the same: the Makron's riding Jorg) */
            int     n2 = 0, live2 = 0, j;

            for (j = 1; j < g_nfull; ++j)
                if (g_edicts[j].mdl == &models[WARP_ALSO])
                {
                    ++n2;
                    live2 += g_edicts[j].kind == EK_MONSTER;
                }
            vdp_printf(8, 32, RGB(255, 255, 120), "ALSO %d OF %d%s N%d", live2, n2, models[WARP_ALSO].loaded ? "" : " NOT LOADED", g_nfull);
        }
#endif
    }
#endif
#ifdef MLIGHT_DSP_CHECK
    {   /* (OPT=-DMLIGHT_DSP_CHECK: the DSP's lighting jobs and tables, and the block's head and the DSP's data RAM) */
        extern u32  lit_vram, lit_tag;
        const volatile u32 *h = (const volatile u32 *)(VDP1_VRAM + lit_vram) + LB_HEAD;

        vdp_printf(8, 44, RGB(255, 255, 120), "LIT JOBS %u GIVEN %u", lit_stat[0], lit_stat[1]);
        vdp_printf(8, 56, RGB(255, 255, 120), "CHECKED %u WRONG %u", lit_stat[2], lit_stat[3]);
        vdp_printf(8, 68, RGB(255, 255, 120), "V %X TAG %u DONE %u N %u", lit_vram, lit_tag, h[2], h[4]);
        vdp_printf(8, 80, RGB(255, 255, 120), "BAD N %u DSP %X C %X NL %u T %u", lit_dbg[0], lit_dbg[1], lit_dbg[2],
                   lit_dbg[3] & 255, lit_dbg[3] >> 8);
    }
#endif
#ifdef MEDIC_TEST
    {   /* (OPT=-DMEDIC_TEST: the medic nearest you, the bodies about (V: within its reach and sight), how many raised) */
        extern int  g_revived;
        const g_ent *m = NULL;
        int         i, medics = 0, bodies = 0, seen = 0, d = 0, near = 9999, nt = '?', ni = '?', nv = '?';
        s32         best = 0;

        for (i = 1; i < g_nfull; ++i)
        {
            const g_ent *e = &g_edicts[i];
            s32         v[3], l;
            int         k;

            if (e->kind != EK_MONSTER || e->mdl != &models[MDL_MEDIC])
                continue;
            ++medics;
            for (k = 0; k < 3; ++k)
                v[k] = e->origin[k] - g_player->origin[k];
            l = vlen(v);
            if (!m || l < best)
            {
                m = e;
                best = l;
            }
        }
        for (i = 1; m && i < g_nfull; ++i)
        {
            const g_ent *e = &g_edicts[i];
            s32         v[3], l;
            int         k;

            if (e->kind != EK_MONSTER || !e->dead || e->move)
                continue;
            ++bodies;
            for (k = 0; k < 3; ++k)
                v[k] = e->origin[k] - m->origin[k];
            l = vlen(v) >> 16;
            if (l < near)
            {
                near = l;
                nt = e->takedamage ? 't' : '-';
                ni = e->inactive ? 'i' : '-';
                nv = visible(m, e) ? 'v' : '-';
            }
            if (e->takedamage && !e->inactive && l <= 1024 && visible(m, e))
                ++seen;
        }
        if (m && m->enemy)
        {
            s32 v[3];

            for (i = 0; i < 3; ++i)
                v[i] = m->enemy->origin[i] - m->origin[i];
            d = vlen(v) >> 16;
        }
        vdp_printf(8, 8, RGB(255, 255, 120), "M%d%c%c%c A%X E%c D%d Z%d F%d H%d B%d V%d N%d%c%c%c UP%d T%d", medics, m && m->inactive ? 'i' : ' ',
                   m && m->on_ground ? 'g' : '-', m && m->chase_fail == level.time - FRAMETIME ? 'c' : '-',
                   m ? m->aiflags : 0, !m || !m->enemy ? '-' : m->enemy == g_player ? 'P' : m->enemy->dead ? 'B' : 'M', d,
                   m && m->enemy ? (m->enemy->origin[2] - m->origin[2]) >> 16 : 0,
                   m && m->move ? m->frame - m->move->first : -1, m ? m->health : 0, bodies, seen, near, nt, ni, nv, g_revived, level.framenum);
    }
#endif
}
