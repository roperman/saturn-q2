# Quake 2 on the Sega Saturn

A port of Quake 2's shareware demo to the Sega Saturn: its three levels, its
monsters' own AI, its weapons, items, sounds and status bar, on a bare-metal
engine that uses both SH-2s, the SCU DSP and the sound CPU. It needs the 4 MB
RAM cart.

## Why, and how it's made

> I'm porting Quake 2 to the Saturn with Claude, Anthropic's AI model, to
> show how easy it now is to make code efficient on old hardware. I choose
> what to work on, make the calls where there's a trade-off, and play it.
> Claude has written most of the code, so the rest of this is in its words.
> *Roper*

I'm Claude (Claude Opus 5.5). The Saturn has two 28 MHz SH-2 CPUs with 4 KB
caches, 2 MB of work RAM and a sprite chip that draws quads with no
perspective correction and no depth buffer. Getting Quake 2 onto that has
mostly been a job of finding where the time goes and winning it back, which
for me has been the same loop over and over:
- measure a frame (profile builds, and benchmarks that play back six fixed
  views or a scripted fight);
- find the hot spot and try an idea: often SH-2 assembly, sometimes moving
  data into faster memory or work onto another processor (the SCU's DSP,
  the second SH-2);
- check the result: the picture compared pixel for pixel with the build
  before, and new assembly run alongside the C it replaces, result for
  result;
- keep the change only if the numbers got better.

Anything that trades looks for speed (a coarser grid for far-off walls,
say) is a switch, and that call is Roper's, as is any that costs memory or
latency.

[OVERNIGHT.md](OVERNIGHT.md) is my log of it all: each change, why I made
it, what I measured before and after, and the ideas that didn't pay. Every
commit has me as co-author.

Two numbers from that log (Mednafen, PAL):

| | first measured | now |
|---|---|---|
| the fight benchmark: 20 s in a room of monsters, the game running | 82 ms a frame (12 fps) | 39.8 ms (25 fps) |
| the static benchmark: six busy views, frame times summed | 481 ms | 217 ms |

The static benchmark draws more now than it did then (the gun in your
hands, for one).

## What works

- The demo's three levels (Outer Base, Installation, Comm Center) with the
  exits between them. Your health, armour and weapons carry over, and
  there's the level-complete screen and the three skill levels.
- Monsters with Quake 2's own AI, ported from its game code: soldiers
  (light, standard, SS), infantry, and the gunner (on Installation). They
  see and hear you, chase you, shoot, flinch and die.
- Weapons: blaster, shotgun, super shotgun, machinegun, chaingun, grenade
  launcher, rocket launcher. Quake's damage and fire rates, radius damage,
  and the weapon drawn in your hands.
- Items, armour, power-ups and keys; Quake 2's status bar; messages.
- Doors, lifts, buttons, triggers, secrets, exploding barrels and walls,
  swimming.
- Quake 2's own sounds, played by the Saturn's 68000, a bank for each level.
- Dynamic lights (muzzle flashes, rockets, explosions) and the skybox.
- Water that moves (gentle waves and a ripple of light), a tint when you're
  under it, and see-through water and glass (VDP1's mesh, or real
  half-transparency if you choose it: it costs more).
- A title menu with options, including brightness (for a PC screen, say).

Not yet: the other monsters (flyers, the tank, berserkers and parasites,
so Installation and Comm Center are quieter than Quake's), flickering
lights, saving, and full-resolution textures. [PLAN.md](PLAN.md) has what's next.

## Speed

In Mednafen (PAL, 50 Hz), most of the benchmark's views run at 25 fps and
the open ones at 50. The fight benchmark holds 25 fps: its frames are 39.8
ms, of which the CPUs are busy 34.6. On NTSC the aim is a steady 30 fps,
which needs that under 33.3 ms. The per-change numbers are in OVERNIGHT.md.

## Building

It builds on Linux. You need:

- **Jo Engine**, for its SH-2 GCC and the disc's boot sector (nothing else
  of it is used): `git clone https://github.com/johannes-fetz/joengine
  vendor/joengine`, or a symlink there to a copy you have.
- **A 68000 GCC** for the sound driver: `m68k-linux-gnu-gcc` and its
  binutils (Debian and Ubuntu: `gcc-m68k-linux-gnu`).
- **A host C compiler** (`cc`), **Python 3 with Pillow**, and **mkisofs**
  (`genisoimage`).
- **Quake 2's `pak0.pak`** in `data/`. The shareware demo's will do
  (`q2-314-demo-x86.exe`, from id's old FTP site, mirrored on
  deponie.yamagi.org). Its licence allows personal use and free electronic
  distribution with the licence attached, so nothing made from it is in
  this repository.

Then:

    ./build.sh      # bakes the levels, models, sounds and HUD from pak0.pak, builds game.cue
    ./run.sh        # runs it in Mednafen (REGION=jp or na for NTSC)

The first build bakes all three levels, which takes a few minutes: working
out which faces each part of a level can see is slow, and cached in `obj/`
after. `MAP=demo2 ./build.sh` starts the game on another level.

## Running

In Mednafen, with a Saturn BIOS in `~/.mednafen/firmware/`. `run.sh` turns
on the 4 MB RAM cart (`-ss.cart extram4`), which the game needs: without it
it says so and stops.

On a Saturn it needs the 4 MB RAM cart and a way to boot the disc image.

## Controls

| | |
|---|---|
| up / down | forward / back |
| left / right | turn |
| L / R | strafe |
| A | jump, swim up |
| B | fire |
| Y | next weapon |
| X / Z | look up / down |
| C | centre the view |
| START | pause menu |

Holding START, for debugging: X god mode, Y noclip, Z every weapon, A / C /
B / L warp to the next monster / item / door or lift / trigger, UP the game's
tick during the drawing (also in Options), R the benchmark.

## How it draws

The Saturn's VDP1 draws textured quads (distorted sprites): no texture
coordinates, no perspective correction, no depth buffer. So
`tools/bake_map.py` turns every face into a grid of **cells** aligned to the
texture's repeat, each one sprite of a small shared tile. Tiles are 4-bit
with 16 colours each, so VDP1's Gouraud shading still works, and it carries
the lightmap: sampled at the cells' corners, lifted by a gamma for a TV.
Where a face only partly covers a cell, the sprite starts part way into its
tile, uses a transposed copy, or gets a cropped variant. Far faces use a
coarser grid with half-resolution textures (as mipmapping would).

At run time (`src/render.c`, and SH-2 assembly in `src/*.s`):

- **Visibility**: Quake's PVS, then a per-cluster list of faces that can
  really be seen (`tools/facevis.c` renders the level from sample points in
  each cluster offline).
- **Order**: the BSP walked front to back (`src/walk.s`); VDP1 draws each
  command list's bucket last-in first, so the list comes out back to front
  without sorting. Doors, lifts and sprites join the order at their leaf.
- **Two CPUs**: the master walks the BSP and publishes faces as it finds
  them; the slave draws from the front straight away, the master from the
  back when the walk's done, and they meet in the middle.
- **Per face**: its origin and axes into view space and culled (`face.s`),
  the grid projected with the hardware divider running alongside
  (`grid.s`), and each cell a VDP1 command and Gouraud table (`cells.s`).
  Cells crossing the near plane are cut in C.
- **Textures**: a cache of tile slots in VDP1's VRAM, split between the
  CPUs, filled from the cart; a slot is reused only once VDP1 has drawn the
  frames that used it.
- **Models**: Quake 2's MD2s (`tools/bake_md2.py`), each triangle pair a
  quad with its piece of the skin pre-warped into a little texture. The SCU
  DSP blends and transforms the vertices while the CPUs do other things;
  the polygons are sorted and drawn in assembly (`mdraw.s`).
- **Lights**: dynamic lights add to the Gouraud colours at the grid points
  they reach, with Quake's falloff. The sky is a panorama on VDP2.

Level data sits on the 4 MB cart, and what's read every frame is copied
into work RAM: the BSP into the fast high 1 MB, faces, cells and lights into
the low. The game code (`src/g_*.c`, `src/m_*.c`) is Quake 2's, in fixed
point, ticking at its 10 Hz with the monsters blended between ticks; in a
fight it runs on the master while the slave draws. Its traces are Quake 2's,
the line walk in assembly (`tline.s`).

## Checking a change

Build switches (`OPT=-D... ./build.sh`) and scripts in `tools/`:

| | |
|---|---|
| `tools/bench.sh` | the static benchmark: six views, both CPUs' time (START + R in the game) |
| `OPT=-DFIGHT_BENCH`, `tools/fight.sh` | the fight benchmark |
| `OPT=-DR_PROFILE` | where the time goes, part by part |
| `tools/compare.sh` | the benchmark's views with the assembly on and off, pixel by pixel |
| `OPT=-DONE_CPU` | everything on the master (fixed drawing order, for comparing builds) |
| `OPT=-DFACE_CHECK`, `-DLINE_CHECK`, `-DBOUNDS_CHECK`, `-DMODEL_CHECK` | assembly and C side by side, results compared |
| `OPT=-DLEVEL_TEST` | each level in turn, and the memory each leaves |

`tools/emu.sh` drives Mednafen for these. It uses the real display and
keyboard, so leave the machine alone while it runs.

## Layout

| | |
|---|---|
| `src/` | the game: renderer, game code, traces, sound, menus, and the SH-2 assembly (`*.s`) |
| `engine/` | the bare-metal engine: start-up, VDP1/VDP2, the SCU DSP, CD, sound (the 68000 driver in `engine/m68k/`) |
| `tools/` | the bakers (map, models, sounds, HUD) and the test scripts |
| `OVERNIGHT.md` | the development log |
| `PLAN.md` | what's next |

## Credits and licence

GPL v2 or later (`LICENSE`), as Quake 2's source is: much of `src/` is
ported from id Software's Quake 2 (the game code, the traces, the player's
movement), and `tools/anorms.h` is its table of normals. The Quake 2 data isn't in this repository and isn't covered by
this licence.

- The CD driver (`engine/cd.c`) follows the command sequences of libyaul's
  (MIT, Israel Jacquez).
- `engine/font8x8_basic.h` is Daniel Hepper's (public domain).
- Lobotomy's SlaveDriver engine (PowerSlave, Saturn Quake) showed the way
  for the dynamic lights.
- The SH-2 compiler and boot sector are Jo Engine's (MIT, Johannes Fetz).

Quake is a trademark of id Software and Sega Saturn is a trademark of Sega.
This is a fan project, not affiliated with or endorsed by either.
