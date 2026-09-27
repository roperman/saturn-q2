/*
** The status bar: Quake 2's own pictures (tools/bake_hud.py), 8-bit, drawn as
** VDP1 sprites in 256-colour bank mode against Quake's palette in VDP2
** colour RAM (at 0x200: the sky has 0x100). Health, ammo and armour along the
** bottom as in Quake, with its yellow numbers when health is low; messages
** in the middle.
*/
#include "game.h"

#define CRAM_BANK       (0x200)
#define MAX_PICS        (40)

typedef struct { u16 srca, size, w, h; char name[12]; } t_pic;

static t_pic        pics[MAX_PICS];
static int          npics;

void                hud_init(void)
{
    const u8        *b = cart_load("HUD.BIN");
    const u16       *pal;
    const u8        *tab, *data;
    volatile u16    *cram = (volatile u16 *)0x25F00000;
    u32             base;
    int             i, k;

    if (!b || memcmp(b, "Q2HD", 4))
        return;
    npics = imin(((const u16 *)b)[2], MAX_PICS);
    pal = (const u16 *)(b + 8);
    for (i = 0; i < 256; ++i)
        cram[CRAM_BANK + i] = pal[i];
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

/* a number right-aligned in a field of 3 (Quake's SCR_DrawField), 16 pixels a digit */
static void         draw_field(int x, int y, int value, bool alt)
{
    char            buf[8];
    int             n, i;

    n = fmt(buf, "%d", imin(imax(value, -99), 999));
    x += 16 * (3 - n);
    for (i = 0; i < n; ++i, x += 16)
    {
        char name[12] = "num_minus";

        if (buf[i] != '-')
        {
            name[0] = 'n'; name[1] = 'u'; name[2] = 'm'; name[3] = '_';
            name[4] = buf[i];
            name[5] = 0;
        }
        if (alt)
        {
            /* anum_: the yellow ones */
            int k;

            for (k = 11; k > 0; --k)
                name[k] = name[k - 1];
            name[0] = 'a';
        }
        draw_pic(pic(name), x, y);
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
    static const char *ammo_pic[AMMO_COUNT] = { "a_blaster", "a_shells", "a_bullets", "a_grenades", "a_rockets",
                                                NULL, NULL };
    const g_weapon  *w = &weapons[client.weapon];
    int             y = SCREEN_H - 26;

    if (npics)
    {
        draw_pic(pic("i_health"), 8, y);
        draw_field(34, y, g_player->health, g_player->health < 25);
        if (w->ammo != AMMO_NONE)
        {
            draw_pic(pic(ammo_pic[w->ammo]), 114, y);
            draw_field(140, y, client.ammo[w->ammo], client.ammo[w->ammo] < 5);
        }
        if (client.armor)
        {
            draw_pic(pic(client.armor_protect >= 80 ? "i_bodyarmor" : client.armor_protect >= 60 ? "i_combatarmor"
                                                                                                    : "i_jacketarmor"),
                     216, y);
            draw_field(242, y, client.armor, false);
        }
        if (level.time < client.quad_until)
            draw_pic(pic("p_quad"), 290, y - 28);
    }
    else
        vdp_printf(16, SCREEN_H - 20, RGB(255, 220, 120), "HEALTH %d  AMMO %d  ARMOR %d", g_player->health,
                   w->ammo != AMMO_NONE ? client.ammo[w->ammo] : 0, client.armor);
    if (center_msg && level.time < center_until)
        draw_center(center_msg, 76);
}
