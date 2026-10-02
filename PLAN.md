# Plan

The approach: as much detail as we dare first, then win the frame rate back
(assembly, data layout, the other processors) rather than cutting detail up
front. Anything that would change the look is a switch, compared pixel by
pixel, and Roper makes the call; everything else has to leave the picture
exactly as it was. [OVERNIGHT.md](OVERNIGHT.md) has what's been done,
section by section.

## Next: the game

- [ ] **The other monsters.** Installation has flyers and a tank the port
      doesn't have yet, and Comm Center berserkers, parasites, flyers and
      gunners. Each needs its AI ported from Quake 2's `m_*.c`, and room on
      the cart for its model: OVERNIGHT.md section 11 has the sizes and the
      ways to make room. Flyers need flying.
- [ ] **Flickering lights** (Quake's light styles): a lightmap layer a
      style, blended at run time.
- [ ] **Saving** to backup RAM (the engine has a driver, `engine/bup.c`).
- [ ] **Ladders**: done (`src/pmove.c`), not yet tried in the game.
- [ ] **Full-resolution textures** (`RES=1`), if VRAM and the cart allow.

## Next: speed

The aim: a steady 30 fps on NTSC in a fight, so the fight benchmark's CPU
time under 33.3 ms. It was 31.8 on average, with 2 of its ~600 pictures a
frame longer, but with over half of each level's entities missing
(OVERNIGHT.md section 39). With all of them the fight's room has more
monsters: 36.8 ms, 152 of 521 pictures a frame longer (PAL's 25 fps still
held, 14 of 494); 36.5 ms and 142 of 526 since the traces' brush sides
(section 40), and the box traces in assembly (section 41) took a frame's
traces from 7.4 to 6.4 ms; the models' light after the slave's hand-off
(section 42): 35.3 ms, 110 of 542; the far monsters' vertices first and the DSP's list in what
HWRAM's left (section 43): 35.1 ms, 104 of 545. The walls' dynamic lights are mostly the DSP's now
(section 34): what's left is the game's tick (11 ms a frame on average, ~30
at its slowest), the slave's drawing (~33 ms, the monsters' 4.9 of it) and
the half-transparent water and glass.

Where a frame goes (`OPT=-DR_PROFILE`, both CPUs, the static benchmark): the
faces' setup and grids 17.5 ms, the cells 16, the models 5.5, the BSP walk
6.8 (the master's). A drawn face has 5 cells on average, so most of it is
per face.

- **LWRAM**: faces, cells and lights live in low work RAM, at 59 cycles a
  cache miss: about 350 cycles a face. Each face's cells and lights
  together and aligned would halve the misses, if LWRAM has the room.
- **The grids**: about 90 cycles a point, mostly the projection.
- **The fight's game tick**: box traces are about 0.3 ms each in the
  gunner's fight, 18-48 of them a tick.
- **VDP1**: some views wait for it now.

## What things cost

`src/cycles.c` measures them at boot (the stats overlay shows them), in
cycles, in Mednafen:

| | HWRAM | LWRAM | cart | VDP1 VRAM |
|---|---|---|---|---|
| store | 3.5 | 13.5 | 15.5 | 111 |
| cache miss (a 16-byte line) | 10 | 59 | 75 | |

A cache hit is 1.5, an uncached HWRAM read 8, a divide 43 (if you wait for
it), DMULS.L and reading MACH 3.8. So HWRAM's cheap; LWRAM and the cart are
what hurt; VRAM writes are very slow. The cache is 4 KB for code and data
together, but in the face and cell code the misses turned out to be a small
part (OVERNIGHT.md section 22): it's instructions, and LWRAM.

## Notes

- Mednafen needs the 4 MB cart: `-ss.cart extram4` (`run.sh`,
  `tools/emu.sh`).
- `tools/emu.sh start|key|snap|stop` drives Mednafen for testing. It uses
  the real display and keyboard, so leave the machine alone while it runs.
- With both CPUs drawing, a change in speed moves where they meet, and a
  few pixels on seams between cells can change hands. Compare builds with
  `OPT=-DONE_CPU` (and `-DTEST_FIFO` for the master's appended order).
- HWRAM: each level uses what the code leaves for its hottest data, so
  code that runs only at start-up, a level's start, in the menus or seldom
  lives in low work RAM (`engine/link.ld`'s `.lwtext`: some objects, and
  every function marked `cold`). `OPT=-DLEVEL_TEST` shows what each level
  has left; check all three after a change that grows the code.
- Never write to fixed work RAM addresses (a benchmark once did, at
  0x06080000). The level data lives there.
