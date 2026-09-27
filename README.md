# Quake 2 on the Sega Saturn

A port-in-progress of Quake 2 to the Saturn. It runs on the bare-metal engine
from the other Saturn projects: no SGL, both SH-2s, and VDP1 command lists
built by the CPUs. The engine here is a fork of the taxi game's
(`../saturn-ctaxi/taxi/engine`). Id's GPL source
(`vendor/quake2`, GPL v2 or later) is the reference for the game logic.

What works: base1 (the shareware demo's first map) textured and lightmapped,
the skybox, walking with Quake 2's movement and collision, doors, lifts and
buttons, and a blaster whose bolts and impacts light the walls around them.
The soldiers (light, standard, SS) are MD2 models with Quake 2's own AI: they
notice you, chase you, shoot back (blaster, shotgun, machinegun), flinch and
die.

## Building

    ./build.sh      # bakes data/pak0.pak's map into cd/DEMO1.MAP, builds game.cue
    ./run.sh        # Mednafen, with the 4 MB RAM cart (it's needed)

You need Quake 2's `pak0.pak` in `data/`. The shareware demo's will do
(`q2-314-demo-x86.exe` from id's old FTP site, mirrored on
deponie.yamagi.org). Its license allows personal use and free electronic
distribution with the license attached, so nothing baked from it is
committed. `MAP=demo2 ./build.sh` bakes another map. The first bake of a map
spends about 1.5 minutes on face visibility, which is then cached in
`obj/`.

The toolchain is the Jo Engine GCC 8.2 in `../saturn/vendor/joengine`
(symlinked). `vendor/slavedriver` is Lobotomy's Saturn engine (PowerSlave,
Saturn Quake), GPL 3. It's for reference only and isn't built.

## Controls

| | |
|---|---|
| up / down | forward / back |
| left / right | turn |
| L / R | strafe |
| A | jump (swim up) |
| B | fire the blaster |
| X / Z | look up / down |
| C | centre the view |
| Y | noclip on/off |
| START | the stats overlay |
| START + B | (debugging) warp to the next door, lift or button |
| START + A | (debugging) warp in front of the next soldier |

When you die, START starts you again.

## How it draws

VDP1 draws textured quads (distorted sprites): no UVs, no perspective
correction, no Z-buffer. So `tools/bake_map.py` turns every face into a grid
of **cells**, 32x32 texels of texture space, aligned to the texture's repeat.
A whole cell is one sprite of a shared **tile**. Where a face only partly
covers a cell:

- if it covers whole rows of the tile, the sprite starts part way down it
  (srca/height), with no extra texture
- if it covers whole columns, the same trick is used on a transposed copy
  of the tile
- otherwise the cell gets a **variant**: the tile cropped to what's covered,
  with the rest transparent

Tiles are 4bpp with 16 RGB colours each, so VDP1's Gouraud shading still
works. The lightmap is sampled at the cell corners and turned into Gouraud
offsets, lifted by a gamma of 0.55 because Quake 2 is very dark on a TV.
Textures are stored at half resolution for now (`--res=2`).

At runtime (`src/render.c`):

- **Visibility**: Quake's PVS marks leaves. On top of that there's a
  per-cluster set of faces that can *really* be seen (`tools/facevis.c`
  renders ID buffers from sample points in each cluster). That's about 45%
  of what the PVS lets through.
- **Order**: the BSP is walked front to back. VDP1 draws each list bucket
  last-in first, so the list comes out back to front with no sorting.
  Brush models (doors) and sprites join the order at their leaf.
- **Two CPUs**: the master takes the nearer part of the face list and the
  slave the rest. The split adapts each frame so both finish together.
- **Per face**: the origin and two axes go into view space, and grid
  vertices are then just additions. A vertex is only projected (the one
  hardware divide) when a visible cell needs it. Small cropped cells have
  their corners interpolated on screen from their cell's. Cells crossing the
  near plane drop texture rows where that's exact, and squash where it
  isn't.
- **Textures**: a cache of tile-sized slots in VDP1 VRAM, half per CPU,
  filled from the RAM cart on demand. A slot is reused once the frame that
  last used it has been drawn.
- **Dynamic lights**: added to the Gouraud colours of the cell corners they
  reach, with Quake's falloff, squared. This is how SlaveDriver did its
  lights. VDP2's colour offset is free for screen flashes on top.
- **Sky**: the skybox's horizon as a 1024-wide 16-colour panorama on VDP2
  NBG0, scrolled with the view. Above and below it are per-line back
  colours.

Level data is on the 4 MB cart. What's read every frame is copied to work
RAM: the BSP into high, and the faces, cells and lights into low. Models
(`tools/bake_md2.py`) go on the cart too, and their frames into high work RAM.

## The game

`src/g_main.c`, `src/g_ai.c` and `src/m_soldier.c` are Quake 2's game code
(`g_ai.c`, `m_move.c`, `g_monster.c`, `m_soldier.c`) cut down and in fixed
point. It ticks at Quake's 10 Hz, and the renderer blends monsters' positions,
angles and animation frames in between. Traces see monsters and the player as
boxes, as Quake's SV_Trace does. A line trace walks the BSP Quake 1's way,
with no brush clipping, but a long one still costs about 0.5 ms. So the AI
spends traces carefully:
- idle soldiers look for you every third tick, and only when their leaf is
  in your PVS
- one trace answers both "can I see them" and "can I shoot them"
- a shotgun blast traces the walls once, and each pellet only tests boxes

Only soldiers whose leaf has some face visible from your cluster are drawn.

## Performance (Mednafen, PAL)

In base1 it runs 10 to 25 fps, and 50 in small spaces. A busy view is
about 1,500 cells from about 400 faces, at roughly 25 ms of master time
and 25 ms of slave time. It's CPU-bound, and memory is as much the cost as
arithmetic: the SH-2 cache writes through, so every store goes to RAM at
about 12 cycles, and a low work RAM cache miss costs about 70. VDP1 keeps
up so far. See PLAN.md for what's next.

## Licence

GPL v2 or later (`LICENSE`), as Quake 2's source is: much of `src/` is
ported from it (the game code, the traces, the player's movement). The
Quake 2 data isn't in this repository and isn't covered by it: it has id's
own licence (see Building).
