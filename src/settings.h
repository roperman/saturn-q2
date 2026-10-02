/*
** Build-time settings: the trade-offs between looks, latency and speed that
** someone building the game may want to make differently. Change them here,
** or give them to the build without touching this file:
**
**     OPT="-DMODEL_FAR=250 -DNO_WATER" ./build.sh
**
** Those marked (Options) are only where the game starts: its Options menu
** changes them while playing. OVERNIGHT.md has what each one was measured at.
*/
#ifndef SETTINGS_H
#define SETTINGS_H

/* ---- looks against speed ---- */

/* A monster further away than this (units, across the floor) is drawn with
   its coarse mesh: its vertices merged on a grid six cells across, a third
   of the polygons. Nearer is faster where monsters are close, and plainer:
   in the static benchmark 250 takes ~2.5% off the CPUs' time, 200 ~4%, and
   at 200 the infantry's gun arm visibly loses its shape (OVERNIGHT.md 44). */
#ifndef MODEL_FAR
#define MODEL_FAR       (400)
#endif

/* A wall face wholly further away than this (units, in depth) is drawn on
   its coarse grid with half-resolution textures, as mipmapping would. */
#ifndef LOD_Z
#define LOD_Z           (384)
#endif

/* (Options) Translucent surfaces (water, glass): 0 solid, 1 a mesh,
   2 half-transparent, 3 half-transparent with the water untextured.
   Half-transparency costs VDP1 dear where there's a lot of it. */
#ifndef TRANS_MODE
#define TRANS_MODE      (1)
#endif

/* (Options) Brightness: 0 as baked, up to 4 (a gamma on every colour). */
#ifndef BRIGHT
#define BRIGHT          (0)
#endif

/* (Options) NO_WATER: the water's waves and ripple of light off. */
/* #define NO_WATER */

/* ---- latency against speed ---- (each one off is slower) */

/* NO_GAME_DURING_DRAW: (Options, and START + UP) the game's tick before the
   drawing rather than during it. During, the monsters are drawn as they
   were a frame earlier (your view isn't); off, a fight's frame is ~10%
   slower (OVERNIGHT.md 7). */
/* #define NO_GAME_DURING_DRAW */

/* NO_LIGHT_AHEAD: the models lit by this frame's dynamic lights (muzzle
   flashes, rockets) rather than last frame's (OVERNIGHT.md 30). */
/* #define NO_LIGHT_AHEAD */

/* NO_PIPE: each frame waits for its picture to be shown before the next
   starts, rather than up to three frames in hand (OVERNIGHT.md 4). */
/* #define NO_PIPE */

#endif
