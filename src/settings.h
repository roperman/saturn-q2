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

/* NO_IDLE_HALF: a monster standing out of the view's PVS (it can't be seen)
   thinks at 10 Hz as the rest do, rather than every other tick; at 5 Hz it
   notices you up to 0.1 s later and costs ~0.2 ms a frame less in a fight
   (OVERNIGHT.md 59). */
/* #define NO_IDLE_HALF */

/* CHASE_TRIES: a blocked monster's search for a way round (Quake's
   SV_NewChaseDir: the diagonal towards you, the two axes, its old direction,
   then every direction) tries at most this many a tick, standing till the
   next if none worked; 0 is Quake's whole search (up to twelve, a box trace
   each: the fight's worst frames). In the fight 40% of blocked monsters
   find their way on the 5th try or later, so 4 holds those up a tick
   (OVERNIGHT.md 59 has the numbers). */
/* The A-bus's timing for the RAM cart (SCU ASR0, CS0 and CS1). SAROO, the SD card drive, sets
   0x38803880 as it boots a game (8 waits, the cart's own wait signal heeded) and needs it: with
   Sega's 0x23301FF0 for its own cart (3 waits, the wait signal ignored) a read from SAROO's cart
   now and then got the bus's stale word instead (its SDRAM serves the CD too), and the game hung
   somewhere new each run. 0x38803880 works on a Sega cart too, which never asks to wait; slower */
#ifndef CART_ASR0
#define CART_ASR0       (0x38803880)
#endif

#ifndef CHASE_TRIES
#define CHASE_TRIES     (0)
#endif

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
