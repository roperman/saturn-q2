# While you were out

## 1. More game

### The infantry
`src/m_infantry.c`, from Quake's `m_infantry.c`. It stands, notices you, runs at you, and fires machinegun
bursts (it holds the firing frame for 1-2 seconds, as in Quake). Up close it punches. It has two pains, and
two deaths: in one it sprays bullets as it falls. It has 100 health and its hurt skin below half. base1 has
6 of them.

### Items, weapons, ammo, the HUD
- Items (`src/g_items.c`) are the real MD2 models, turning in place: health (stimpack, medium, large, mega),
  armour (shard, jacket, combat, body), ammo (shells, bullets, grenades, rockets, cells, slugs), the
  weapons, quad damage, adrenaline, invulnerability, silencer, the blue key. Walking over one picks it
  up, with Quake's amounts and caps, "You got the ..." and the yellow pickup flash. Armour takes its
  share of damage (jacket 30%, combat 60%, body 80%).
- Weapons, with Quake's damage, spread and fire rates: blaster, shotgun (12 pellets), super shotgun (two
  barrels of 10), machinegun, chaingun, grenade launcher (grenades bounce, 2.5 s fuse), rocket launcher
  (direct hit plus radius damage). Radius damage falls off with distance, doesn't go through walls, and
  hurts you (at half) if you're close. Rockets and grenades are drawn as their MD2 models, pointing
  where they're going. A new weapon switches in if you're on the blaster.
- Explosions: a big orange light fading over half a second, and a round glowing fireball (a glow
  texture tinted by Gouraud, half-transparent). Bolts and sparks use the same glow.
- The HUD is Quake's own status bar pictures (`tools/bake_hud.py`, `src/hud.c`): 8-bit, with Quake's
  palette in VDP2 colour RAM. Health, ammo and armour with their icons, and the yellow numbers when
  low. Messages appear in the middle.

### Triggers and targets
The baker now exports every entity (`q_erec` in `src/game.h`, classes in `obj/gen/q2classes.h`). There is
one name table, so everything can target everything. `src/g_target.c` has:
- `trigger_once` / `trigger_multiple` (with messages; "triggered" ones wait to be enabled),
  `trigger_relay`, `trigger_always`, `trigger_counter`, `func_timer`
- `target_explosion` (with radius damage), `target_splash`, `target_secret` ("You found a secret area!"),
  `target_goal`, `target_help`, `target_changelevel` (a LEVEL COMPLETE screen with kills and secrets;
  START plays the level again)
- killtargets and delays
- the two monsters that wait for a trigger now appear when it fires, awake and after you
- monsters use their targets when they die
- `misc_explobox`: the barrels explode (shoot them)
- `func_explosive`: walls that blow up when shot or triggered, gone afterwards
- buttons fire their targets through the same system

Checked: the trigger at (812, -260) sets off its two explosions (the stats show USES 7 EXPL 2).

### Also
- Ladders (Quake's `PM_CheckSpecialMovement`): look up and walk forward, or hold jump, to climb. Not yet
  tested in the game: the ladder's deep in the level.
- Monster AI traces: line traces walk the BSP Quake 1's way; idle soldiers look every third tick and only
  when they're in your PVS; one trace answers both "can I see them" and "can I shoot them"; a shotgun
  blast traces the walls once. Game ticks cost about 1 ms idle, 6-15 ms in a fight (they were 25-30).

### Controls
B (hold) fires and Y changes weapon. Debugging, with START: A warps to the next monster, C to the next
item, B to the next door/lift/button, L into the next trigger; X is god mode, Z gives every weapon, Y is
noclip.

## 2. Speed

The benchmark (START + R): six fixed views, the master's CPU time and the
frame time, summed over them (ms).

| | CPU | frame |
|---|---|---|
| before the speed work | 493 | 481 |
| the walk overlapping the drawing, a fast path for whole cells | 347 | 419 |
| the grid's outer lines on the faces' edges | 334 | 379 |
| per-cell counters out (they're stores) and the divide overlapped | 311 | 379 |
| the grid rows in assembly (`src/grid.s`) | 298 | 359 |
| a whole face's grid in one call; texture records kept by the slots | 292 | 359 |
| grid points always projected, so gv_xy is a load (draw_face 6.3 KB to 2.8 KB); the DSP models off | 279 | 359 |
| faces 52 to 32 bytes (a shared axes table), cells' exactness baked in | 274 | 359 |
| the cells in assembly (`src/cells.s`), a whole face a call | 263 | 359 |
| the models' vertices on the DSP, alongside the CPUs (`engine/xformm.dsp`) | 253 | 339 |

Per view the frames are now 60, 80, 20, 60, 60 and 60 ms (PAL: 50 Hz
steps); views 1 and 5 need 41 and 40 ms of CPU, right at 40 (25 fps).

What changed:
- **Faces' edges.** Quake 2's faces rarely end on a tile boundary, so the
  cells along their edges were cropped, and each cropped cell had its
  corners worked out at run time. The baker now puts the grid's outer lines
  on the face's edges (`eu0`, `eu1`, `ev0`, `ev1` in `q_face`), and 77% of
  cropped cells are simply their grid cell. The lights are sampled at the
  real edges too, which is slightly more accurate.
- **`src/grid.s`**: the grid in SH-2 assembly. The divider runs on its own,
  so each point starts the next point's divide and the 39 cycles go by
  while the stores do; the outcode is built with ROTCL straight from the
  screen position; the divider's registers are reached through GBR.
  80 cycles a point against the compiler's 140. It checks itself against
  the C at boot (a difference and the C is used).
- **The SCU DSP**: the packed model-vertex transform (`engine/xformp.dsp`)
  now works; I'd misread the ALU (its result exists only in the instruction
  that makes it, as the manual's `AD2 MOV ALU,A` shows), and the DSP's CT2
  wasn't reset between runs. But with the CPU waiting for it, it's slower
  than the CPU doing it, so it's off (`OPT=-DDSP_MODELS` turns it on) until
  it runs alongside the walk.
- **`src/cycles.c`** measures what things cost at boot (in PLAN.md). HWRAM is
  cheap; LWRAM, the cart and VRAM writes are not; code size matters.
- `OPT=-DR_PROFILE` builds the per-part profile (the benchmark's green
  lines); `OPT=-DONE_CPU` runs everything on the master.

Then, in this order:
- **The data.** Faces are 32 bytes (two cache lines, not four): their grid
  axes are a table in HWRAM (575 pairs for 7,500 faces). Whole tiles and
  crops that are exactly their grid cell share one cell layout with
  everything the command needs baked in. HWRAM's end is checked now: running
  out stops with a message instead of writing over the slave's stack (it
  had been 22 KB into the space below it). The game's entities moved to
  LWRAM for room.
- **`src/cells.s`**: the cells in assembly, a whole face a call (a row a
  call was slower: the call cost more than it saved). It does the whole
  tiles and exact crops in front of the camera with their textures in VRAM,
  and lists the rest for the C (`cell_c`). `tools/compare.sh` renders the
  benchmark's views with it on and off: the only differences are single
  pixels on cells' shared edges (which of two cells draws them).
- **The DSP alongside.** At the start of a frame the master lists the
  models that may be seen, both animation frames' matrices weighted for the
  blend, and starts the DSP (`engine/xformm.dsp`: the blend is six
  multiply-adds a coordinate). It counts each model off in work RAM; a CPU
  drawing a model waits only if that one isn't done, then invalidates just
  the lines the DSP wrote. `OPT=-DNO_DSP` has the CPUs do it.

All three demo levels run: `MAP=demo2 ./build.sh` (or demo3). demo2 leaves
115 KB of HWRAM free and demo3 38 KB; the cart holds any one of them with
the models (demo3, the biggest, is 2.8 MB, the models 0.8).

## 3. Speed, second round

Each step measured on its own with the benchmark (ms, summed over the six
views; demo2 has six views of its own now, picked by the map's name):

| | demo1 CPU | demo1 frame | demo2 CPU | demo2 frame |
|---|---|---|---|---|
| before | 254 | 339 | 211 | 339 |
| near cells split before VDP1 gets them | 259 | 359 | 215 | 259 |
| 1. faces outside the view skipped before their grid | 233 | 282 | 195 | 259 |
| 3. grid points 8 bytes (x y z worked out again when needed) | 230 | 279 | 192 | 259 |
| 4. the grid axes into view space once a frame, not once a face | 224 | | 188 | |
| 5. the walk: bit tables, the PVS test before the call | 218 | | 185 | |
| 5. the walk in assembly (`src/walk.s`) | 201 | 259 | 172 | |
| 2. coarse grids for distant faces | 200 | | 171 | |
| 6. cut-down meshes for distant models | 190 | 259 | 171 | 259 |

demo1's views are now 40, 60, 20, 40, 40 and 60 ms (they were 60, 80, 20,
60, 60, 60); demo2's 60, 40, 40, 40, 40, 40.

- **The near split.** VDP1 draws a distorted sprite whole, including what's
  off the screen, so a cell right in front of the camera could cost it
  more than the rest of the frame. Whole tiles that reach well off the
  screen (or behind the near plane) are cut into quarters with quartered
  textures first. It costs the CPU a little and saved demo2's heaviest
  views 20-60 ms each.
- **1. Face culling.** A face's four corners against the view before any of
  its grid is worked out.
- **3. Smaller grid points**: the screen position and outcode only; the few
  cells that need a point's view-space position (the near plane, dynamic
  lights) rebuild it from the face's axes.
- **4. Face setup.** Faces share 575 grid axes; each is turned into view
  space once a frame (on first use). The DSP doing the whole face setup
  (`engine/xformf.dsp`, kept) was tried against it: no faster (230 against
  230), so the DSP stays on the models.
- **5. The walk** in SH-2 assembly: the child's PVS test before the call,
  only what's needed saved, the far child a jump, the view planes unrolled.
  Its time summed over the views went from 78 to 48 ms. `OPT="-DWALK_CHECK -DONE_CPU"` runs the C
  walk alongside and counts differences: none in 1,883 frames over both maps.
- **2. Coarse grids**: faces of 12 cells or more also get a 64-texel grid,
  used past 384 units (`LOD_Z`). On the screen I couldn't tell them apart.
- **6. Distant models**: the soldier and the infantry have a second, cut-down
  mesh (vertices merged on a 6-cell grid across the model: `--lod=6` in
  `tools/models.txt`), used past 400 units (`MODEL_FAR`); only the vertices
  it uses are transformed.

Not done: 7 (frame pacing), as asked.

Memory: all three maps still boot. demo3 needed two changes: its coarse
grids only for faces of 24 cells or more (`LODMIN`, set in `build.sh`), to
keep it and the models inside the cart's 4 MB, and the brushes (only the
collision traces use them) stay on the cart when copying them to LWRAM
would leave under 48 KB for what's allocated after the level. Running out
of the cart now stops with a message too. HWRAM free: demo1 12 KB, demo2
118 KB, demo3 45 KB.

Checked: `tools/compare.sh` (the cells in assembly against the C) shows
only single pixels on shared cell edges, as before.

## 4. The frame swap, the monsters' ticks

**The swap by interrupt** (`vdp_set_pipelined`, `engine/vdp.c`). The frame
used to end with both CPUs idle until the next vblank: a frame needing 41 ms
was shown for 60. Now `vdp_submit` sends the list and returns, the CPUs start
the next frame, and the timer-0 interrupt just before each vblank swaps to
the waiting list once VDP1 has finished the one before. The vblank interrupt
then sets the sky for the picture now on screen (each frame's sky is kept in
a small ring). Up to three frames are in hand, so textures stay cached for
two frames after their last use, and the text colours in VRAM no longer
change. The interrupt wrappers save MACH/MACL and GBR too (the walk's MAC
sums, grid.s's divider). `OPT=-DNO_PIPE` has the old swap.

| benchmark frames (ms, six views) | old swap | by interrupt |
|---|---|---|
| demo1 | 259 | 227-229 |
| demo2 | 259 | 242 |

demo2's views 3-6 stay at 40 ms: VDP1 is the limit there (their CPU time is
20-30 ms). The CPU total went up about 2% (1904 to 1938 in the benchmark's
units), the same with the old swap: the code and data moving in the cache,
not the new swap (padding the code back into place didn't recover it).

**The lists' copy to VDP1** (the benchmark's new DMA column): 0.2 to 1.5 ms
a frame. Small, so it's left as it is.

**The fight benchmark** (`OPT=-DFIGHT_BENCH`, `tools/fight.sh`): START + R
stands you in the round room in god mode, wakes the monsters around it and
times 20 seconds of the fight with the game running. In a fight the game,
not the drawing, is the heavy part: 30-36 ms of game a frame, of which 26-31
is traces (31-32 a frame, about 1 ms each).

**The monsters' ticks staggered** (`MON_GROUPS`, `src/g_main.c`): the
monsters think in four groups (edict number % 4), each at 10 Hz in its own
time but a quarter of a tick apart, so a fight's AI is spread over the
frames instead of all landing on one every 100 ms. Twice each:

| fight, 20 s | together | staggered |
|---|---|---|
| frame (ms) | 82-83 | 77-81 |
| the worst frame's game (ms) | 65-66 | 54 |
| pictures up 100 ms or more | 104-105 | 38-56 |
| pictures up 80 ms | 84-87 | 148-154 |

With the old swap the same fight's frames are 102 ms.
