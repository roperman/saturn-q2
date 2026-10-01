/*
** Sounds: Quake 2's own, from tools/bake_sound.py's banks (a level's: DEMO1.SND
** for DEMO1.MAP, what every level has and its monsters') in sound RAM, played
** by the 68000 (engine/snd.c, engine/m68k/driver.c).
** One heard in the world is quieter the further it is from the view, by
** Quake's attenuations (ATTN_NORM: silent at 1,000 units; IDLE at 500;
** STATIC at 333), and panned to the side of the view it's on.
*/
#include "q2.h"
#include "game.h"

static bool         s_ready;
static s32          lag_us = -1;            /* (s_lag: how long ago the moment the game's at was; -1 now) */

/* The game's tick (10 Hz) runs as a frame starts, so its moments come to the sound as the frames
   fall (at 25 fps 80 and 120 ms apart, not 100). Its sounds are started at the moment each is
   of, a frame on (a frame: two fields), so they're as even as Quake's; the rest at once */
void                s_lag(s32 ago)
{
    lag_us = ago < 0 ? -1 : (s32)(((s64)ago * 1000000) >> 16);
}

/* the level's bank: its map's name, .SND */
static void         bank_file(char *out, const char *map)
{
    int             i;

    for (i = 0; map[i] && map[i] != '.' && i < 10; ++i)
        out[i] = map[i];
    memcpy(out + i, ".SND", 5);
}

void                s_init(const char *map)
{
    char            f[16];

    bank_file(f, map);
    s_ready = snd_init(f);
}

/* a new level: its bank in place of the last */
void                s_level(const char *map)
{
    char            f[16];

    bank_file(f, map);
    s_ready = snd_bank(f);
}

/* id at origin (NULL: the player's own, full and in the middle) */
void                s_play(int id, const s32 *origin, int atten)
{
    s32             d[3], dist, vol = 127, pan = 0, side;
    int             k;

    if (!s_ready || id < 0)
        return;
    if (origin && atten)
    {
        for (k = 0; k < 3; ++k)
            d[k] = (origin[k] - cam.pos[k]) >> 16;
        if (iabs(d[0]) > 4000 || iabs(d[1]) > 4000 || iabs(d[2]) > 4000)
            return;
        dist = (s32)isqrt((u32)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        vol = 127 - dist * atten * 127 / 1000;
        if (vol < 6)
            return;                         /* too far to hear */
        /* how far to the view's right, over the distance: the pan (not quite all to one side) */
        side = (d[0] * cam.right[0] + d[1] * cam.right[1] + d[2] * cam.right[2]) >> 16;
        pan = dist > 0 ? side * 12 / dist : 0;
    }
    if (lag_us >= 0)
        snd_sfx_later(id, (int)vol, (int)pan, (VDP2_TVSTAT & 1 ? 40000 : 33367) - lag_us);
    else
        snd_sfx_at(id, (int)vol, (int)pan);
}
