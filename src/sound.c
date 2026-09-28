/*
** Sounds: Quake 2's own, from tools/bake_sound.py's bank (SOUND.BIN) in
** sound RAM, played by the 68000 (engine/snd.c, engine/m68k/driver.c).
** One heard in the world is quieter the further it is from the view, by
** Quake's attenuations (ATTN_NORM: silent at 1,000 units; IDLE at 500;
** STATIC at 333), and panned to the side of the view it's on.
*/
#include "q2.h"
#include "game.h"

static bool         s_ready;

void                s_init(void)
{
    s_ready = snd_init("SOUND.BIN");
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
    snd_sfx_at(id, (int)vol, (int)pan);
}
