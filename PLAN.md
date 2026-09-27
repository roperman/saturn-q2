# Plan

The approach: as much detail as we dare first, then win the frame rate back
(assembly, data layout), rather than cutting detail up front.

## Done (27 September 2026)

- Converter (`tools/bake_map.py`): faces become texture-aligned cell grids,
  with shared 4bpp tiles plus row/column crops and masked variants, and
  lightmap-derived Gouraud colours
- Per-cluster face visibility, sampled offline (`tools/facevis.c`)
- Renderer: BSP front to back into one LIFO bucket per CPU, both SH-2s with
  an adaptive split, a VRAM texture cache, near-plane handling, and
  screen-space crop interpolation
- Collision: Quake 2's box traces (`src/trace.c`) and the core of pmove
  (`src/pmove.c`): stairs, slopes, jumping, swimming
- Doors, lifts and buttons (`src/movers.c`): proximity, targets, teams,
  riding a lift, reversing when blocked
- Dynamic lights and depth-sorted sprites; the blaster (`src/fx.c`)
- The sky on VDP2
- MD2 models (`tools/bake_md2.py`, `src/model.c`, `draw_model` in `src/render.c`).
  The soldier's 434 triangles become 266 quads and triangles, each with its
  piece of the skin pre-warped into a little texture that goes through the
  VRAM cache. Its frames are blended at 10 Hz and shaded by normal. Lighting
  is the leaf's light, cached per entity, plus dynamic lights, stronger on
  the side facing them.
- The soldier's AI (`src/g_ai.c`, `src/m_soldier.c`, `src/g_main.c`), ported
  from Quake 2's game code: sight (range, facing, line of sight, and other
  monsters that have seen you), hearing your blaster (non-ambush soldiers),
  chasing with step and ledge checks, attacks (blaster bolts, 12-pellet
  shotgun, machinegun bursts), pain (with the hurt skin), death, and
  corpses. Player health, damage, a red flash on hits, death and respawn.

## Next: features

- [ ] HUD: health, ammo, the status bar pics (`pics/*.pcx`) as VDP1 sprites
      or on a VDP2 layer
- [ ] The weapon view model (MD2) in front of the camera
- [ ] Items: pickups as sprites, or MD2 at a low vertex count
- [ ] More monsters: the infantry (6 in base1), then the rest of the game's
      monsters. The soldier's other attacks and deaths, gibs.
- [ ] Weapon pickups and ammo; the shotgun and machinegun for the player
- [ ] Triggers: `trigger_once/multiple` firing targets (the two
      trigger-spawned soldiers wait for these), `target_*`, messages
- [ ] Triggers: messages, relays, `target_*`, trains (`func_train` along
      `path_corner`s), rotating things
- [ ] Sound: the 68000 driver from the other projects, with Quake's wavs
      resampled
- [ ] Water and translucency: SURF_WARP wobble, TRANS33/66 as VDP1
      half-transparency, `CONTENTS_WATER` tint through VDP2 colour offset
- [ ] Light styles (flickering lights): bake per-style lightmap layers,
      blend them at runtime
- [ ] Full-resolution textures (`--res=1`) once the VRAM budget allows

## Next: speed

AI: a long trace (line of sight, a bullet) walks a few hundred BSP nodes,
about 0.5 ms; a short box trace (a monster's step) is about 11 us. A fight
with one soldier costs 6-15 ms a tick (10 ticks a second); idle, about
1 ms. Ways down: the node walk in assembly, or cheaper sight lines (the
per-cluster face visibility could answer most "can't possibly see" cases
first).

Models: a close soldier costs about 7 ms of one CPU (about 8 with a dynamic
light on it):

| | ms |
|---|---|
| 227 vertices: two frames into view space, blend, project | 3.2 |
| back faces and the depth sort (266 polys) | 2 |
| commands (about 135 polys) | 2 |

Ways down: the vertex transforms on the SCU DSP (both frames, 16 vertices a
block, while the CPUs do other things); assembly for the vertex and
polygon loops; and at a distance either a cut-down mesh or sprites
pre-rendered in 8 directions, as the football sim does.

What things cost (`src/cycles.c` measures them at boot; the stats overlay,
toggled with START, shows them), in cycles, in Mednafen:

| | HWRAM | LWRAM | cart | VDP1 VRAM |
|---|---|---|---|---|
| store | 3.5 | 13.5 | 15.5 | 111 |
| cache miss (a 16-byte line) | 10 | 59 | 75 | |

A cache hit is 1.5, an uncached HWRAM read 8, a divide 43 (if you wait for
it), DMULS.L and reading MACH 3.8. So HWRAM's cheap, LWRAM and the cart are
what hurt, VRAM writes are very slow, and code size matters: the cache is
4 KB for code and data together (draw_face went from 6.3 KB to 2.8 KB and
the frame went 2% faster).

Where a frame goes (`OPT=-DR_PROFILE`, both CPUs, averaged over the
benchmark's views, before the cells and models work): face setup 9 ms, the
grid 25, cells 29, models 12, the walk 9 (the master only). The profile
build is about 10% slower than the real one.

Done: the grid and cells in assembly, faces 32 bytes, the models' vertices
on the DSP alongside; then (OVERNIGHT.md, section 3) the near split, face
culling, 8-byte grid points, per-frame view-space axes, the walk in
assembly, coarse grids for distant faces and cut-down distant models: the
benchmark's CPU 254 to 190 ms, frames 339 to 259. Next:

1. **Frame pacing**: an even 40 ms rather than 40 and 60 alternating
2. **LWRAM**: cells and lights are still there (59 cycles a miss); copy each
   face's ahead of time with the SH-2's own DMA, or 8-bit lights (a palette)
3. **Memory per level**: demo1 has 12 KB of HWRAM left; the three levels
   could each have their own layout
4. The AI's traces (see above): the node walk in assembly like the render's

## Notes

- Mednafen needs the 4 MB cart: `-ss.cart extram4` (`run.sh`,
  `tools/emu.sh`)
- `tools/emu.sh start|key|snap|stop` drives Mednafen for testing. It uses
  the real display, so don't use the machine while it runs.
- Never write to fixed work RAM addresses (a benchmark once did, at
  0x06080000). The level data lives there now.
