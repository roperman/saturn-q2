/*
** The menus: the title (Quake 2's plaque, logo, items and spinning cursor,
** from tools/bake_hud.py), the skill to start at, the options, and the
** pause menu (START in the game). While one's up the game stands still and
** the world's still drawn behind it: at the title, turning slowly at the
** level's start.
**
** UP / DOWN choose, A (or C, or START) takes it, B goes back; LEFT / RIGHT
** change an option.
*/
#include "game.h"

#define ITEMS_MAX       (6)

menu_id             menu_cur = MENU_MAIN;   /* the title first */
static int          sel, cursor_frame;
static menu_id      options_from;           /* where the options go back to */

int                 g_skill = -1;           /* 0 easy, 1 medium, 2 hard; -1 everything (until one's chosen) */
int                 opt_volume = 15;
bool                opt_crosshair = true;   /* (no gun on the screen to aim by) */
#ifdef STATS
bool                opt_stats = true;       /* the debugging overlay (OPT=-DSTATS: on from the start) */
#else
bool                opt_stats;              /* the debugging overlay */
#endif
extern bool         game_during_draw;       /* (main.c) */

static const char   *skill_names[3] = { "EASY", "MEDIUM", "HARD" };

static int          slen(const char *s)
{
    int             n = 0;

    while (s[n])
        ++n;
    return n;
}

bool                menu_active(void)
{
    return menu_cur != MENU_NONE;
}

/* the title's menus (not the pause menu's): the game hasn't started */
bool                menu_at_title(void)
{
    return menu_cur == MENU_MAIN || menu_cur == MENU_SKILL || (menu_cur == MENU_OPTIONS && options_from == MENU_MAIN);
}

void                menu_open(menu_id m)
{
    menu_cur = m;
    sel = 0;
}

static int          n_items(void)
{
    switch (menu_cur)
    {
        case MENU_MAIN:     return 2;
        case MENU_SKILL:    return 3;
        case MENU_OPTIONS:  return 5;
        case MENU_PAUSE:    return 4;
        default:            return 0;
    }
}

/* one frame's presses: what the game's to do about them */

menu_action         menu_input(u16 pressed)
{
    int             n = n_items();


    if (++cursor_frame >= 8 * 3)
        cursor_frame = 0;
    if (pressed & PAD_UP)
    {
        sel = (sel + n - 1) % n;
        s_play(SND_MENU_MOVE, NULL, ATTN_NONE);
    }
    if (pressed & PAD_DOWN)
    {
        sel = (sel + 1) % n;
        s_play(SND_MENU_MOVE, NULL, ATTN_NONE);
    }
    if (menu_cur == MENU_OPTIONS && pressed & (PAD_LEFT | PAD_RIGHT))
    {
        int d = pressed & PAD_RIGHT ? 1 : -1;

        if (sel == 0)
        {
            opt_volume = imin(imax(opt_volume + d, 0), 15);
            snd_music_volume(opt_volume);   /* (the driver's master level) */
        }
        else if (sel == 1)
            game_during_draw = !game_during_draw;
        else if (sel == 2)
            opt_crosshair = !opt_crosshair;
        else if (sel == 3)
            opt_stats = !opt_stats;
        s_play(SND_MENU_MOVE, NULL, ATTN_NONE);
    }
    if (pressed & PAD_B)
    {
        s_play(SND_MENU_BACK, NULL, ATTN_NONE);
        switch (menu_cur)
        {
            case MENU_SKILL:    menu_open(MENU_MAIN); sel = 0; break;
            case MENU_OPTIONS:  menu_open(options_from); sel = 1; break;       /* (OPTIONS is item 1 in both) */
            case MENU_PAUSE:    menu_cur = MENU_NONE; return MA_RESUME;
            default:            break;
        }
        return MA_NONE;
    }
    if (!(pressed & (PAD_A | PAD_C | PAD_START)))
        return MA_NONE;
    s_play(SND_MENU_SELECT, NULL, ATTN_NONE);
    switch (menu_cur)
    {
        case MENU_MAIN:
            if (sel == 0)
            {
                menu_open(MENU_SKILL);
                sel = 1;                    /* medium */
            }
            else
            {
                options_from = MENU_MAIN;
                menu_open(MENU_OPTIONS);
            }
            break;
        case MENU_SKILL:
            g_skill = sel;
            menu_cur = MENU_NONE;
            return MA_NEW_GAME;
        case MENU_OPTIONS:
            if (sel == 4)
            {
                menu_open(options_from);
                sel = 1;
            }
            break;
        case MENU_PAUSE:
            switch (sel)
            {
                case 0: menu_cur = MENU_NONE; return MA_RESUME;
                case 1:
                    options_from = MENU_PAUSE;
                    menu_open(MENU_OPTIONS);
                    break;
                case 2: menu_cur = MENU_NONE; return MA_RESTART;
                case 3: menu_open(MENU_MAIN); return MA_TITLE;
            }
            break;
        default:
            break;
    }
    return MA_NONE;
}

/* a line of the menu's text, the chosen one lit */
static void         item_text(int i, int y, const char *s)
{
    vdp_text(160 - slen(s) * 4, y, i == sel ? RGB(255, 220, 120) : RGB(170, 150, 120), s);
}

static void         cursor(int x, int y)
{
    char            name[12] = "m_cursor0";
    int             f = cursor_frame / 3 * 2;       /* 0 2 4 .. 14, a frame every 3 */

    if (f >= 10)
    {
        name[8] = '1';
        name[9] = (char)('0' + f - 10);
        name[10] = 0;
    }
    else
        name[8] = (char)('0' + f);
    hud_pic(name, x, y);
}

void                menu_draw(void)
{
    char            line[40];
    int             i;

    switch (menu_cur)
    {
        case MENU_MAIN:
            /* Quake 2's: the plaque down the left, the logo under it, the items beside */
            hud_pic("m_main_plaque", 40, 24);
            hud_pic("m_main_logo", 41, 194);
            hud_pic(sel == 0 ? "m_main_game_sel" : "m_main_game", 112, 64);
            hud_pic(sel == 1 ? "m_main_options_sel" : "m_main_options", 112, 104);
            cursor(84, 64 + sel * 40);
            vdp_text(112, 200, RGB(150, 130, 100), "SATURN PORT");
            break;
        case MENU_SKILL:
            vdp_text(160 - 8 * 4, 70, RGB(255, 255, 255), "NEW GAME");
            for (i = 0; i < 3; ++i)
                item_text(i, 100 + i * 16, skill_names[i]);
            cursor(96, 96 + sel * 16);
            break;
        case MENU_OPTIONS:
        {
            vdp_text(160 - 7 * 4, 60, RGB(255, 255, 255), "OPTIONS");
            fmt(line, "VOLUME %d", opt_volume);
            item_text(0, 90, line);
            item_text(1, 106, game_during_draw ? "FASTER FIGHTS  ON" : "FASTER FIGHTS OFF");
            item_text(2, 122, opt_crosshair ? "CROSSHAIR  ON" : "CROSSHAIR OFF");
            item_text(3, 138, opt_stats ? "STATISTICS  ON" : "STATISTICS OFF");
            item_text(4, 162, "BACK");
            vdp_text(160 - 19 * 4, 186, RGB(120, 110, 90), "LEFT RIGHT TO CHANGE");
            break;
        }
        case MENU_PAUSE:
            hud_pic("pause", 160 - 52, 50);
            item_text(0, 96, "RESUME");
            item_text(1, 112, "OPTIONS");
            item_text(2, 128, "RESTART LEVEL");
            item_text(3, 144, "QUIT TO TITLE");
            break;
        default:
            break;
    }
}
