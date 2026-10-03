# The development log

My log of the port (I'm Claude), written as I worked, often while Roper was
away, so it talks to Roper as "you": what I built or changed, why, what I
measured before and after, and the ideas that didn't pay. The sections are
in the order the work happened, so the early ones describe code that has
changed since. The numbers are from Mednafen, on PAL.

The two benchmarks I keep measuring against:
- **the static benchmark** (START + R, `tools/bench.sh`): six fixed busy
  views. Its times are usually summed over the six.
- **the fight** (`OPT=-DFIGHT_BENCH`, `tools/fight.sh`): 20 seconds of a
  fight in the round room on Outer Base, the monsters awake, the game
  running. Its times are a frame's.

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

## 5. The traces

The fight benchmark now also shows where the traces come from (call sites by
time) and what a box trace costs in parts. Nearly all the trace time was one
call: `SV_movestep`, a monster's step (its box, 36 units down at the new
spot), 20-25 a frame at about 1 ms each. Step by step, the fight's game time
a frame (ms; the fight varies a few ms run to run):

| | game | traces | frame |
|---|---|---|---|
| start | 30-36 | 26-31 | 77-83 |
| sides on an axis without multiplies | 21.5 | 19.3 | 69 |
| the movers' bounds in HWRAM (the loop over all 35 read the cart) | 17.6 | 15.2 | 64.5 |
| short moves by the leaves their box touches | 15.2 | 11.4 | 60 |
| the entities' boxes in HWRAM (the edicts are in LWRAM) | 12.2-13.5 | 10.9-11.9 | 56-58 |

- **Axial sides** (`clip_box_brush`, `test_box_brush`): every brush's first
  six sides are its box, on the axes, and 74-90% of all sides are: the
  normal's +-1 along one axis, so the three dot products are a compare
  (the same sums exactly).
- **Movers**: every trace checked all 35 brush models, calling `mover_live`
  and reading their records off the cart; now a list of the solid ones with
  their bounds in HWRAM (`trace_world_init`).
- **Short moves** (up to 64 units on each axis): the leaves the whole move's
  box touches, then their brushes, instead of walking the tree along the
  move (which splits at nearly every node the box straddles: two divides
  and a recursion each). `OPT=-DTRACE_CHECK` runs both and compares: of
  about 73,000 traces, 3 stopped at a different point (all in one fight;
  not seen again in four more runs) and 269 (the same 269 on the same walk)
  hit a different plane at the same point: two brushes entered at once (a
  corner), each way keeping the first it met. The old way pads the box to
  its largest extent on every side, so it also clips brushes just outside
  the move; the likely cause of the 3.
  The boot stats' SHORT test (the player's box stepping at the start) went
  from 11 to 53 us: there the old way hit the floor at once. The steps in
  the fight went from 0.68 to 0.55 ms, the player's ground checks from 0.7
  to 0.45.
- **Entities** (`g_trace`): the trace against monsters looked at all 64
  edicts, in LWRAM: 233 us a trace. Now the solid ones' boxes, 32 units
  bigger, in HWRAM, rebuilt once a frame and each monster's refreshed
  after its tick; only a box that passes is tested on the edict: 25-30 us.

Where a step trace's 0.55 ms goes now: 134 us gathering the leaves, 320
clipping (about 8 leaves, 14 brushes, 8 of them out by their box sides), 35
the movers, 30 the entities. In the fight the drawing is now most of the
frame: CPU 56 ms, of which the game is 13.

HWRAM free: demo1 8 KB, demo2 113 KB, demo3 40 KB.

## 6. The models

Timed in the fight benchmark: about 2 soldiers are on screen at a time
there, all on the DSP path. Model time a frame, both CPUs together (ms):

| | light | vertices | cull, sort | commands | all | fight frame |
|---|---|---|---|---|---|---|
| start | 0.9 | 4.1 | 3.7 | 4.2 | 12.9 | 56-58 |
| polygon and texture records in HWRAM | 0.8 | 4.1 | 2.7 | 3.2 | 11.0 | 54 |
| the command pass's state in locals (C) | 0.9 | 4.1 | 2.7 | 2.6 | 10.4 | 54 |
| vertices in assembly | 0.8 | 3.4 | 2.7 | 2.6 | 9.7 | 55 |
| cull and sort in assembly | 0.8 | 3.4 | 2.1 | 2.6 | 9.0 | 52-53 |
| commands in assembly | 0.8 | 3.4 | 2.1 | 2.45 | 8.9 | 53-54 |
| the vertices' divides overlapped | 0.8 | 3.2 | 2.1 | 2.45 | 8.6 | 52.7 |

The same polygons as before, drawn the same: `COMPARE=models
tools/compare.sh` renders the benchmark's views with the old C loops and
the new ones (with `CMP_EXTRA=-DONE_CPU` pixel for pixel; with both CPUs
a few shared cell edges can change hands, because faster models move the
CPUs' split). `OPT="-DFIGHT_BENCH -DMODEL_CHECK"` runs the C alongside in
the fight and compares everything: no differences in 300,000 vertices,
43,000 bucket lists (17,000 quads decided by their second half) and 1,300
models' commands.

- **The records**: every model's data was used in place on the cart, 75
  cycles a miss; the monsters' polygon and texture records (both passes
  read one a polygon) are now copied to HWRAM at the end of start-up, in
  what's left. Room for them: the sine table is a quarter wave (the same
  values exactly), 12 KB less. demo1 has 3 KB of HWRAM left.
- **Assembly** (`src/mdraw.s`): the vertices (the whole mesh; the far
  mesh's subset stays in C), the cull and depth sort, and the commands. The
  compiler's versions spilled registers to the stack, and with the cache
  writing through every spill is a bus write, shared with the other CPU
  and the DSP. The command pass gained least: its 11 stores a polygon
  (the command and its Gouraud table) are most of it.
- **Sorting baked in** (per viewing direction): not done. The sort is a
  depth bucket a polygon, a few instructions of the cull pass; the time
  is in the culling test and the stores.
- What's left in the vertices: waiting for the DSP 0.3 ms, the assembly 2.0,
  each vertex's normal index 0.6 (it's in the frame, on the cart).

The benchmark's view 4 (a soldier) went 34.2 to 31.9 ms of CPU; demo1's
six views 1918 to 1844 (frames 2270 to 2233).

## 7. The game's tick during the drawing (a switch: START + UP)

In a fight the master ran the game's tick (about 12 ms: the monsters, the
traces) before drawing started, with the slave idle. With the switch on,
the master runs it between its walk and its own drawing; the slave
meanwhile draws from the front of the face list, and the master then
takes from the back until they meet, so the slave just does more of it.
Nothing the tick changes is read by the drawing (the entities, sprites and
lights it draws are copied out before, and doors only move in
movers_update), but the monsters are drawn as they were a frame earlier;
your view isn't.

| fight benchmark | off | on |
|---|---|---|
| frame (ms) | 52.7 | 47.1-48.2 |
| master / slave drawing (ms) | 23.5 / 32.4 | 18.8 / 38.8 |
| pictures up 40 ms / 60 ms | 135-139 / 236-241 | 240-269 / 155-175 |

Texture uploads go up from about 4 to 12 a frame (each CPU has its own
half of the texture cache, and the slave now draws more), about 1.7 ms of
it, counted in the above. On by default now (your call, the morning after);
OPT=-DNO_GAME_DURING_DRAW builds it off, and the options menu switches it.

## 8. The world's drawing: five ideas tried

Where it goes, in the fight (R_PROFILE, both CPUs, a frame): cells 24.9 ms
(774 of them), the grid 9.3, face setup 3.5, the walk 8 (the master's). A
cell's about 450 cycles on the fast path, and it's mostly memory: about 11
stores (the command and its Gouraud table) on a bus both CPUs and the DSP
share, and its cell and light records read from LWRAM.

1. **Each face's cells and lights fetched ahead into HWRAM** by the SH-2's
   own DMA while the one before is drawn: 2.5% *slower*, taken out. A test
   (a register-only loop beside either DMA runs at full speed) says the DMA
   doesn't stop the CPU; but it takes the bus for as long as the CPU's own
   misses did (17 cycles a word against 15), and the bus is what's short.
   Touching the lines first (a test build) saves 2.2 ms of the cells for
   3.0 ms of touching.
2. **The slow cells** (the C path, about 940 cycles each): 227 a frame in
   the fight. 10 for a dynamic light (not worth special-casing), 59 exact
   crops near the camera, 143 small crops interpolated on screen, 13 big
   crops. The small crops in `cells.s` itself (not three C calls a cell)
   would save about 1.2 ms a fight frame: next.
3. **Texture uploads by the SCU's DMA** (the cart to VDP1's VRAM, on the
   SCU's own buses): done, checked byte for byte (11,077 uploads). The
   drawing's 1-2% quicker a CPU while turning; the CPU total's the same
   (the master waits for the DMA at the frame's end, about 0.3 ms: at 15
   uploads a frame there's little in it). A shared texture cache would cut
   the uploads further, but there's little left to cut.
4. **Gouraud tables reused** when a cell's matches: under 1% repeat the one
   before. Not done.
5. **Tighter visibility**: rendering from each sample point of a cluster
   (tools/facevis.c's way) sees 64% of what the cluster's set allows, so a
   third of what's drawn is hidden (a leaf's set would be 96% of its
   cluster's: no help). A coverage mask (16 x 8 tiles, marked by the big
   solid rectangular faces of the front-to-back list, `OPT=-DOCC_COUNT`)
   culls only 2% of demo1's benchmark faces and 12% of demo2's: a face
   only marks tiles it wholly covers, so the seams between faces leak.
   Getting the third needs spans at pixel precision, or portals (Quake 2's
   BSP doesn't keep them: they'd have to be made again from the tree).

Also: `OPT=-DTURN_BENCH` (a full turn at each view, textures loading):
demo1 while turning is 36-40 ms a frame with the CPUs at 20-25; a frame is
whole vblanks, so VDP1 a little over 20 ms is enough to make most of them
40. Coarse grids from 192 units or Gouraud off change VDP1's time by
nothing measurable in Mednafen. The face list is 2,048 long now (busy
views list about 500): 6 KB of HWRAM back.

## 9. Sound

`tools/bake_sound.py`: 64 of Quake 2's own sounds (every weapon, the
soldiers' and infantry's sight, attacks, pain and death, the player's pain
by health, death, jumps, landings, footsteps and water, pickups by kind,
doors, lifts, buttons, secrets, messages, explosions, grenade bounces,
ricochets, the menus) at 11 kHz 8-bit: 372 KB of the 425 in sound RAM.
The 68000 driver (engine/m68k) gained 8-bit samples, a volume and pan for
each effect, and 16 effect slots (there's no music in the demo).
`src/sound.c` places them: quieter with distance by Quake's attenuations,
panned to the side of the view they're on; your own in the middle.
Checked in a recording (Mednafen's -soundrecord: `RECORD=out.wav
tools/emu.sh start`): left is left, and a fight is full of it.

## 10. Menus and the levels in order

- **The title**: Quake 2's plaque, logo, GAME and OPTIONS and its spinning
  cursor, the level turning behind. GAME asks the skill: easy, medium,
  hard, which now filter the monsters and items as Quake's do (demo1 has
  21 monsters on medium). Until you choose, everything spawns (the
  benchmarks see what they always did).
- **Options**: volume, faster fights (the game's tick during the
  drawing, section 7), crosshair (Quake's, on), statistics (the overlay:
  off now; `OPT=-DSTATS` for on).
- **START in the game**: pause (resume, options, restart the level, quit
  to the title). START with a button is still the debugging keys, and
  START + R still runs the benchmarks from anywhere (and restarts the
  level first, everything in it).
- **The exits**: the lift at the end of Outer Base now takes you to
  Installation, with your health, armour, weapons and ammo, at the start
  the exit names, and so on through the demo's hub (Installation to Comm
  Center and back); its last exit says THE END OF THE DEMO and goes back
  to the title. `OPT=-DLEVEL_TEST` takes each exit in turn.
- Each level loads only the models it uses.

## 11. Not done: the other monsters

Installation has 5 monsters the port doesn't have yet (3 flyers, a gunner,
a tank) and Comm Center 14 (5 berserkers, 4 parasites, 3 flyers, 2
gunners): they're left out, so those levels are quieter than Quake's.
(Since: the gunner, on Installation: section 19.)
Each needs its AI ported (vendor/quake2/game/m_*.c), its model and its
sounds. The gunner and berserker are the easy two (a machinegun and
grenades; a club), but each is bigger than the soldier (330 vertices, 610
triangles, 209 and 244 frames): about 230 KB on the cart even with only
the animations used, and Comm Center has 125 KB left. Ways to make room,
your call: the soldiers without their hurt skins there (about 120 KB), every
other animation frame for the new ones (the blend hides it), or demo3's
textures coarser. Flyers need flying; the tank's the size of three soldiers.

Also not done, your call: the weapon in your hands (Quake 2's v_*.md2:
as big as a soldier, drawn every frame: about 3-4 ms, and cart space).

## 12. More speed, after the missing parts

The fight benchmark now splits the master's frame (input, the player and
movers, the game, before the world, the world, after it), and the static
benchmark shows the walk's own time. Step by step (fight ms a frame; the
fight varies a couple of ms run to run, so each is against a run of the
build before it):

| | fight frame | where |
|---|---|---|
| **small crops in `cells.s`** | 54.7 -> 52.3 (R_PROFILE) | the C's cells 23 -> 10 ms |
| **dynamic lights once a grid point** | 52.3 -> 50.4 (R_PROFILE) | the C's cells 102 -> 15 a frame |
| **the entities' light** | 45.4 -> 42.6 | before the world 4.1 -> 2.0 ms |

- **Small crops** (`src/cells.s`): crops inside their grid cell, all in
  front, under 64 pixels, texture in VRAM: cell_corners' interpolation
  (the same sums) in the assembly, not deferred to the C. The static
  benchmark's CPU 1868 -> 1756. Pixel compare against the C: 96 pixels
  differ in one view (it was 100-280 in five: the crops now go out in the
  C's order).
- **Dynamic lights** (blaster bolts, muzzle flashes: nearly always in a
  fight): a face under one went all through the C, each cell's corners
  positioned and lit separately, up to four times a point: 90 of the C's
  102 cells a fight frame, 4 ms. Now each grid point's lit once, at the
  same position by the same sums, and the face goes through the assembly.
  `OPT=-DDL_CHECK` compares every corner with the old way: 380,056 in a
  fight, none different.
- **The entities' light** (`ents_light`: 3.1 ms of a fight frame on the
  master, before any drawing): relighting an entity made two signed-shift
  library calls a channel for each of its 162 normals (the SH-2 has no
  arithmetic shift by n: GCC calls a helper), and items spin, so each
  relit about 4 times a second; and every entity found its leaf every
  frame, twice. The sum rewritten is never negative, so unsigned shifts
  give the same exactly (every input tried on the host); the leaf's found
  only when the entity has moved. `tools/abcompare.sh` (the benchmark's
  views, the working tree against a stash of the change): identical on
  one CPU; with two, 29 pixels of cell edges in one view change CPU.
- **A leak**: `movers_init` took its arrays from the level's heap on every
  new game, so each "restart level" lost 1-2 KB of high work RAM (a few
  restarts: OUT OF HIGH WORK RAM). Now once a level. Six restarts in a row
  checked.
- **The 68000 sound driver** is loaded from the CD (`SND68K.BIN`) rather
  than carried in the program: 4 KB of high work RAM back.

Now, in the fight (no profiling): frame 42.6 ms, 406 of 412 pictures up
for 40 ms (25 a second on PAL). The master's frame: the player and movers
1.7 ms, the game 7.3, before the world 2.0, the world 29.8 (its walk about
7, then its share of the list while the slave draws the rest).

**Faster fights** (the game during the drawing) on top of all this:
40.4 ms a frame, 481 pictures at 40 ms and 14 at 60. Still your call (the
game's tick then comes a frame later relative to what's drawn).

**The player's movement** costs more than the fight benchmark shows (you
stand still there): pmove timed while walking is 2.7 ms a frame on the
master, 7-8 box traces (the slide, the step up and down, the ground checks
either side), each about 0.4 ms (a player's box touches ~10 leaves and ~13
brushes). One of them is now left out when it can't matter: **the ladder
check** traces a unit forward every frame; each level has one or two ladder
brushes, so the trace only runs within reach of one (or of a mover, which
might carry one). `OPT=-DLADDER_CHECK` starts you at demo1's ladder,
walking into it and climbing (the climb works): the trace run anyway,
220 frames at the ladder, found every time, none missed. The fight's
player part 1.7 -> 1.4 ms. A kept copy of the last ground check (the same
trace ends one pmove and starts the next) was tried and taken out: it hit
57% of frames when exploring, never differed, but saved only 0.07 ms.

**The entities' light, again**: after the unsigned sums it was still
1.3 ms a fight frame, mostly relighting things you can't see (items
spinning, monsters moving elsewhere). Now only what's in this frame's PVS
is relit; the rest keeps the light it has, and which leaf and turn that's
for, and is relit when it's next in the PVS. `OPT=-DENTLIGHT_CHECK`
checks every model drawn (1,583 in the fight, 306 walking in circles):
none with stale light; the benchmark's views identical. Before the world
2.0 -> 1.3 ms.

Final numbers (default build): the static benchmark's CPU 1868 -> 1731
(demo1; demo2 1545); the fight 50.6 -> 41.4 ms a frame, 445 of 448
pictures up for 40 ms (25 a second on PAL), 3 for 60.

Looked at and left:
- **The traces in assembly**: about 9 leaves, 13 brushes, 94 sides a box
  trace; the time's spread over the loads and the sums, about half each.
  Precomputing the box's side offsets (the axial sides' sums) in C
  changed nothing measurable (checked exact, taken out). What would help:
  each brush's bounds in its record (a map format change and a rebake),
  so the 9 of 13 brushes a trace rejects by their box sides cost one read
  instead of the brush, its sides and their planes: maybe 15-20% of every
  trace. It needs 12 more bytes a brush of low work RAM (about 27 KB on
  demo2), which might push the brushes to the cart.
- **The grid**: already assembly with the divides overlapped.
- **The walk** (6.7 ms a frame, the master's): 1,450 nodes and 570 leaves
  a frame at about 90 cycles each, most of it memory, and 3,221 face tests
  for about 400 faces listed. Only 18% of the PVS's nodes have nothing
  visible under them, so pruning them would save well under 1 ms.
- **Coarse grids nearer** (LOD_Z 256 or 192 instead of 384): the static
  benchmark's CPU 1719 -> 1693 / 1682. Few faces have coarse grids; not
  worth the detail.
- **The biggest thing left** is overdraw: a third of what's drawn is
  hidden behind nearer walls. Portals (made again from the BSP by the
  baker, then the view clipped through them at run time) would cut both
  CPUs' drawing and VDP1's by up to that much, and the walk too. It's a
  big job (a day or two, a new bake step, memory for the portals): your
  call.
- Other library calls for shifts and divides: in code that runs a few
  times a frame (the sky, a model's setup, the C's near cells).

## 13. Portals: measured before building

`tools/portal_estimate.py` makes the BSP's leaf-to-leaf portals again (as
qbsp does), flows each benchmark view through them with screen rectangles,
and counts the cells that would still be drawn; `--check` renders each view
with a z-buffer on the PC to prove nothing seen is culled (none, in all
twelve views). Cells kept, against today's list:

| | exact flow | one rectangle a leaf | leaf boxes | cluster boxes |
|---|---|---|---|---|
| demo1 (6 views) | 73% | 74% | 78% | 82% |
| demo1 the round room | 32% | 34% | 48% | 48% |
| demo2 (6 views) | 74% | 79% | 80% | 80% |
| demo2 the big room (2 views) | 23-29% | 23-29% | 23-29% | 23-29% |

The exact flow is far too slow (up to 33,000 projections a view). One
rectangle a leaf, each portal projected once: 44-650 projections. Stored
as the Saturn could afford (`tools/portal_clusters.py`: each portal a box,
merged per pair of clusters: 3,400-5,300 of them, about 85 KB a level;
leaf by leaf 6,600-7,400, ~150 KB, more than demo3's cart has left):
150-470 box projections a frame, roughly 2-6 ms of one CPU.

So: a big cut where a wall hides a lot (the fight room, demo2's big room:
also where VDP1 is the limit), next to nothing in the open views, where
the flow is pure cost. The walk would get cheaper too (only the nodes
above reached leaves). Worth building only with the flow off the master's
path (the slave, while the master does the sky, effects and entities) or
kept to a budget.

## 14. The gun in your hands

Quake 2's seven view weapons (`v_blast` ... `v_rocket`), with its own frame
numbers for raising, firing, one idle frame (no fidgeting) and lowering:
31-55 KB each, 300 KB together, more than the carts have left. So
`src/view.c` keeps two slots on the cart, the gun you hold and the next,
read from the CD in the background while you lower the one you hold (one
slot on Comm Center, where the cart's short: there a moment with no gun
while the next reads). You fire once it's up, as in Quake 2; the
machinegun and chaingun go round their fire loops while you hold fire.

Drawn by `draw_viewmodel` into VDP1's overlay list (over the world, as
Quake 2's depth hack does), lit by the light where you stand and the way you
face, with Quake 2's near plane for guns (4 units). Checked against
z-buffered renders on the PC: its triangles wind the other way from the
monsters' (a soldier rendered the same way matches the game, unmirrored).

Cost: a still gun's drawing is kept and reused, 0.9 ms a frame; while it
animates (firing, switching) about 4 ms (mostly the cart: its frames and
records live there). The fight benchmark (you don't fire in it): 39.8 ->
41.5 ms a frame, 441 of 482 pictures at 40 ms. `OPT=-DVIEW_TEST` gives every
gun and cycles them. Not done: Quake 2's gun bob as you walk.

## 15. The gun, faster

Timed in the fight benchmark (demo1's round room), the gun's parts in
microseconds a frame (`GUN US V S C K`: vertices, sort, commands, kept);
`OPT=-DVIEW_ANIM` keeps it firing (without shots) for the moving numbers.

| | vertices | sort | commands | kept | gun | fight frame |
|---|---|---|---|---|---|---|
| still, before | | | | 900 | 0.9 | 41.5 |
| firing, before (C) | 1640 | 1430 | 2150 | | 5.1 | 44.2 |
| sort and commands in mdraw.s | 1830 | 1140 | 940 | | 3.8 | 42.7 |
| vertices in assembly (vverts_asm) | 1600 | 1120 | 930 | | 3.6 | 43.3 |
| records and frames read by DMA | 1400 | 900 | 980 | | 3.2 | 43.2 |
| vertices on the DSP | 1075 | 897 | 845 | | 2.8 | 43.1 |
| still, now (kept drawing by DMA) | | | | 535 | 0.5 | 41.6 |

(The frame gains about half what the gun does: the two CPUs meet in the
middle of the face list, so the master's saving is shared.)

- **The passes**: the monsters' `mpolys_asm` and `mcmds_asm` do the gun's
  sort and commands. `mpolys_asm` takes a winding sign (the gun's triangles
  go the other way) and lists the polygons across the near plane rather than
  dropping them; there are a lot (every gun in every frame, the chaingun 20
  drawn). The C cuts those as before and gives each corners of its own (past
  `mxy` and `mg` in `r_ctx`, over `mz`, `moc` and `gtab`, done with by
  then) and a record of its own (64 spare after the gun's own, baked into
  its file), put into its bucket where the C would have: the same polygons
  in the same order. `mcmds_asm` writes straight into the overlay.
- **The cart**: the gun's frames and records are on the cart, and its misses
  there were most of the time left, the other CPU drawing at once. They're
  now read by SCU DMA into the master's own command list (free until the
  master draws its faces, which it does after the gun) while the walk and
  the game's tick run; the still gun's kept drawing moved from low work RAM
  (DMA can't read that) to the cart and comes in the same way.
- **The vertices**: on the DSP after the monsters', in the blocks they
  leave, doubled (mverts_asm's near plane is 8, the gun's 4). The DSP blends
  as it does the monsters' (each frame's scale times its share), so its
  rounding differs from the C's: 8.4% of vertices a pixel or two off, 18 of
  240,262 on the other side of the near plane (`OPT=-DVIEW_CHECK=3`).
  `OPT=-DVIEW_EXACT` keeps to `vverts_asm`, which blends exactly as the C did
  (it's also used whenever the DSP can't: the gun fired during the game's
  tick, or no blocks left).
- **Checked**: `OPT=-DVIEW_CHECK=1` runs the old C alongside and compares
  every command and colour: none different in 102,107 commands, every gun,
  firing. `=2` draws the kept drawing and a full one: none different in
  168,899 (and 127,615 on demo3, where one gun at a time fits the cart).
  The only change to what's sent: polygons wholly off one side of the
  screen are no longer (they drew nothing).
- **Memory**: fast RAM on demo1 1.3 KB less (the code), the same monster
  records fit as before (`OPT=-DLEVEL_TEST` now shows what's left of each
  memory, and what monster records stayed on the cart: 12.8 KB on demo1,
  none elsewhere); low work RAM 7.7 KB more; the cart 12 KB less (the kept
  drawing, and 768 bytes more a gun). The static benchmark: CPU 1736 (1731
  to 1744 before, noise).

## 16. The gun bob

Quake 2's (p_view.c `SV_CalcGunOffset`): the gun turns a little against the
view as you walk (yaw up to 2 degrees at a run, pitch and roll 1, the yaw
and roll the other way every other step) and as you turn (it lags: 0.2 of a
tenth of a second's turn). The angles are added to the view's, as Quake 2
does, so looking down the walk's yaw turns it about the world's upright.
OPTIONS: GUN BOB (on).

The gun's drawn from its places on the screen, so it's turned there: each
place's direction by the rotation, and projected again (exact, a divide a
place; `vturn_asm`). The still gun's kept drawing is now its vertices'
places before the bob, turned a vertex at a time; and it goes out in place:
last frame's commands and colour tables are still in the staging, so only
what's changed is written.

| gun, fight benchmark (us a frame) | before the bob | now |
|---|---|---|
| still, standing | 535 | 425 |
| still, walking (the bob turning it every frame) | | 1,200 |
| firing, standing | 2,800 | 3,000 |
| firing, walking | | 3,900 |

The fight's frame: 40.9 ms standing (463 of 489 pictures at 40 ms); with
the bob turning it every frame about 42.5 (407 of 470). The turn is ~130
cycles a vertex even in assembly (the SH-2's multiplies and the divide),
~150 vertices. Firing costs a little more than before: nothing's dropped for
being off the screen now (the kept drawing has to hold what the bob might
turn into view; VDP1 clips).

- **Fixed on the way**: a polygon cut at the near plane had its corners in
  front of it turned twice.
- **Checked**: `VIEW_CHECK=1` (the C alongside, the C turn too): 0
  differences in 133,112 commands, walking, turning and firing; `=2` (the
  kept drawing, in place, against a full one): 0 in 246,569 (a polygon
  turned edge-on can face the other way between the two: 332, counted
  apart); `MODEL_CHECK` (the monsters' sort, changed for the gun): 0.
  `OPT=-DVIEW_BOB_BENCH` holds a changing bob for the benchmark.
- **Memory**: the code grew 2 KB; five files rarely run are now built
  `-Os` (COLD in build.sh), 2.7 KB back. demo1 has 128 bytes more of the
  monsters' records on the cart than before.

## 17. The "hang": the pad stopped

Played for a while, the game seemed to hang: the picture stood still and
nothing answered. It hadn't hung. It went on drawing (148 frames in six
seconds, two save states apart), but the pad had stopped: the SMPC's last
answer (UP held) stayed in its registers for good, so you walked into a wall
and nothing else came through. Walking in a circle from the start
(tools: a script holding UP and LEFT, a snapshot a step) did it within 8 to
33 steps, every time, as far back as 135f5c2 (before the gun); with the swap
by interrupt off (`NO_PIPE`) or faster fights off it didn't in 60.

Why (Mednafen's smpc.c, and the SMPC's own rule): a vblank starting during an
INTBACK's read calls the read off and leaves the last answer. The pad was
asked for just after the frame's swap, and a frame that waited for the swap
(the interrupt's at line 216) asked 8 lines before the vblank; once the
frames fell into step with it, every read was called off.

Now the end of each vblank asks (a new SCU interrupt, `vblank_out_isr` in
engine/vdp.c; at the vblank's start is no good either, in Mednafen a read
asked before the SMPC has seen the vblank is called off at once), the SMPC
reads at the top of the picture, and `pad_collect` takes the last answer it
finished without waiting. Checked: three runs of 60 steps, none stopped, the
pause menu and options answer. Builds without the swap by interrupt ask as
before.

## 18. Walls going missing: the texture cache

Walls went missing now and then, most when turning. The texture cache (VDP1
VRAM, 16x16 slots, a part for each CPU) had run out: a CPU whose part is full
skips every texture it hasn't got for the rest of that frame, and the cells
that need them. Each part had 385 slots, and VRAM went mostly elsewhere:
the two command lists (3,000 commands each, 192 KB) and colour tables (the
soldier's six skins 67 KB, the infantry's 27, the level's 22, the guns' 15).
The benchmarks now show, from start to end, the frames each CPU ran out
(`OUT`) and the most commands each part of the list took (`CMD`, and its
Gouraud tables `G`); `OPT=-DTEX_WSET` adds the textures each frame held.

| | before | after |
|---|---|---|
| turning benchmark, 552 frames: ran out (master / slave) | 10 / 57 | 5 / 19 |
| fight, ~490 frames: ran out | 2 / every frame | 1 / 1 |
| fight: the slave's commands (what it no longer skips) | 506 | 818 |
| fight: texture uploads a frame | 4.8 | 0.6-0.9 |
| slots (master / slave), demo1 | 385 / 385 | 450 / 551 |

In the fight the slave's part was out every frame: about 300 of its polygons
went undrawn, every frame. Frame times are within the fight's run-to-run
spread (39.8 and 41.5 ms, 41.3-42.2 before); the static benchmark's CPU 1684
(1736 before).

- **The lists**: the master's 1,100 commands, the slave's 1,300 (it takes
  more of the list), the overlay 400 (was 1,300, 1,300, 400: busy views take
  941 and 1,076; demo2 the master's most). `VDP_WRITER1_CMDS` sets the
  second writer's; each writer has its `cmax`.
- **The split**: the master's part 45%, the slave's 55% (`SPLIT_M`): the
  slave draws more of the list, but the master has the gun's textures too.
  The turning benchmark now turns with the gun up (as you play); ran out
  (master / slave): at 40% 50 / 19, at 45% 16 / 21, at 50% 5 / 28; the
  fight: at 40% 1 / 1, at 45% 0 / 1, at 50% 1 / 42. Tried, and taken out: the
  split moved between frames towards the CPU that ran out. Moving slots
  throws out textures the giver's using, so it misses and pulls them back
  (turning 15 / 22, worse); moving only slots it hadn't used for 8 frames,
  it never moved at all; moving towards each CPU's share of the cells and
  model polygons it drew (smoothed), turning 2 / 26 (worse again: cells
  aren't the textures).
- **The guns' colour tables**: one set in VRAM, not one a slot. The next gun's
  go in as it comes up, once the last one's three frames gone (`r_view_luts`,
  src/view.c): a weapon switch waits a frame more with no gun up.
- **The monsters' colour tables**: one table kept where the same in every
  skin (tools/bake_md2.py; the texture's record says which, its last field;
  the guns keep one a polygon, `--keeplut`): the soldier 355 to 330 a skin,
  the infantry 436 to 360. Checked: the old bake against the new, every
  polygon in every skin, the same 16 colours (and the same textures and
  frames); `MODEL_CHECK` 0 differences in 163,213 vertices and 1,411 commands.
- **Memory**: the command staging is in HWRAM: demo1 1.1 KB more free and 4.3
  KB more of the monsters' records there (COLD 12,980 to 8,720); the carts 12
  KB more; all three levels load.
- **Late uploads**: a slot is reused three frames after it was last drawn
  (two frames may be in flight), its upload at the frame's end. When a CPU
  has none of those left, it takes one two frames back and its upload waits
  (`UP_LATE`) till `vdp_submit` has seen VDP1 finish that frame
  (`vdp_set_list_hook`, `r_late_uploads`), before the new list goes over.
  Only uploads that can go by DMA are late (the queue's 256 a CPU; the copy
  by the CPU when it's full is done at once, so never into those slots).
  Turning with the gun, ran out 16 / 21 -> 2 / 16; the fight 0 / 1 -> 0 / 0;
  the static benchmark 6 / 6 -> 3 / 3; no time lost (the turning
  benchmark's frames 2,297 -> 2,296). `UPLOAD_CHECK`: 12,883 uploads read
  back from VRAM, 1,428 of them late, none different.
- **The monsters' colour tables, each kept once**: the soldier's six skins
  and the infantry's two have many tables the same as others, in the same
  skin or another (the soldier 1,219 different of 2,130, the infantry 431 of
  872). Each is kept once now, with a map after them (tools/bake_md2.py: each
  skin's textures to their table, u16s, on the cart; the header's count has
  its top bit set). The map's read as a texture goes into the cache
  (`tex_load`, about one a frame in a fight), and its table kept with the slot
  (`slot_lut`, as the level's textures' are); the commands take it from there
  (`mdraw.s` `M_SLUT`, and the C). 34 KB more VRAM: 1,263 slots (was 1,003).
  Checked: every polygon in every skin the same 16 colours as the bake
  before; `MODEL_CHECK` 0 differences in 152,544 vertices and 1,835
  commands.

| ran out (master / slave) | before | after |
|---|---|---|
| turning with the gun, 552 frames | 2 / 16 | 2 / 4 |
| the static benchmark, 96 | 3 / 3 | 1 / 1 |
| the fight, ~500 | 0 / 0 | 0 / 0 |
| walking (40 steps, into the big views) | bursts of 20+ | none |

  No time lost (the turning benchmark's frames 2,298, the static CPU 1678, the
  fight 40.1 ms). The carts 26-27 KB more free: Comm Center now has room for
  both gun slots (the next gun read in the background there too), 8 KB left.

## 19. The gunner, on Installation

Quake 2's gunner (`src/m_gunner.c`, from game/m_gunner.c): a chaingun burst,
again while it can see you, or from further off four grenades (the
launcher's `fx_grenade`); pain, death. On Installation (one on medium, two
on hard); Comm Center's four are left out, its cart has no room.

- **Trimmed to fit**: stand, run, the short and middle pains (the long one's
  frames go to the middle one), death, the chaingun (open, fire, close) and
  the grenades: 105 frames of its 209, both skins, the far mesh: GUNNER.MDL
  238 KB. Left out: its fidget, walk, run-and-shoot (Quake never uses it),
  ducking (the port's shots don't warn monsters), its idle and search
  sounds. Its sounds (sight, two pains, death, the gun's open, fire and the
  grenade), cut short as the soldier's are: the sound bank has 4 KB left.
- **Loaded if there's room** (tools/models.txt `optional`; `MDL_OPTIONAL`):
  after every other model, and only if the cart then has room for both gun
  slots; if not its monsters aren't spawned (nor counted).
- **Bigger models**: the renderer's model limits were 256 vertices and 320
  polygons, the gunner has 329 and 382 (drawn past them, its polygons took
  corners from vertices never worked out: VDP1 drew for ever). Now 336 and
  384: 2 KB of HWRAM, 1.6 KB of the cart (the gun's kept drawing).
- **The map's entities**: `monster_gunner` in tools/bake_map.py's classes
  (after the infantry): the three maps baked again.
- **Memory**: Installation's cart 360 -> 120 KB free, HWRAM 100 -> 83 KB
  (its records there); Outer Base 6 KB more of the monsters' records left on
  the cart (the code), Comm Center as it was. The fight 40.4 ms (as before),
  the static benchmark's CPU 1704 (1678-1696 before).
- **Checked**: `MAP=demo2 OPT=-DGUNNER_TEST` stands you on its ledge, god mode:
  it opens up with the chaingun and lobs grenades, and it goes down to the
  blaster (health 175). All three levels load.

## 20. A sound bank a level

Sound RAM was full (the gunner's sounds left 4 KB). Now:

- **A bank a level** (tools/bake_sound.py: cd/DEMO1.SND ...): what every
  level has (weapons, the player, items, doors, menus) and the sounds of the
  monsters in it (the soldiers', the infantry's, the gunner's: by their
  names' start, against the map's entities; Comm Center's gunners, left out
  anyway, not). The ids are the same in every bank; one a level hasn't got
  is a moment's silence there. `load_level` reads the level's in
  (`s_level`, `snd_bank`: the 68000 held in reset, the bank read over the
  last, the mailbox cleared, the 68000 let go; the pad's asks of the SMPC,
  from the vblank-out interrupt, held off meanwhile).
- **More of sound RAM for the bank**: the reverb's delay lines are 16 KB but
  had 64 KB above the bank; they're at the top now (0x7A000). And the bank
  starts at 0x2000, not 0x8000 (the driver's 4.6 KB and its stack below; the
  build checks it). 480 KB for a bank (was 416).
- **Free now**: Outer Base 105 KB, Installation 68 KB, Comm Center 105 KB
  (was 4 KB for all). Checked in a recording (Mednafen's -soundrecord): the
  blaster on Outer Base, and on Installation after the level's changed.

## 21. Traces: the brushes' boxes

A box trace (a monster's step, the player's move) gathers the leaves its
swept box touches, then clips the brushes listed in them. In the fight 9 a
frame, each 0.7 ms: the leaves 163 us, the brushes 400 (12 of them, 97
sides, 8 of the 12 out by their box sides alone), the movers 39, the
entities 40 (`OPT="-DFIGHT_BENCH -DFIGHT_TRACES"`, its lines now at the top of
the screen). Most of it is memory: a brush's record in LWRAM, then a side and
a plane a side, with the slave drawing on the same bus.

Each brush's box now (`lv.brushbounds`: from its sides on the axes, whole
units, rounded out a unit; worked out as the level loads, in LWRAM if
there's room, else the cart: Installation's): a brush whose box misses the
trace's swept box is skipped from one read, its record and sides untouched.

| a box trace, the fight | before | after |
|---|---|---|
| brushes clipped / sides | 12 / 97 | 3 / 23 |
| clipping (us) | 400 | 253 |
| the monsters' steps, a frame | 5.0 ms | 3.7 ms |
| traces, a frame | 7.3 ms | 5.3-6.3 ms |
| the game's tick, a frame | 8.4-8.6 ms | 6.3-7.4 ms |

The fight's frame 40.3 ms (the master's saving goes on drawing, shared with
the slave), the static benchmark's CPU 1710 (noise). Checked:
`OPT=-DBOUNDS_CHECK` traces each both ways: 0 different in 8,591. The maps as
they were (the boxes baked in grew Comm Center's map 27 KB: one gun slot);
all three load.

### Line traces, the movers, HWRAM

- **The line walk** (bullets, sight; `trace_line`): the C made a call at
  every node (six arguments, lv's bases read each time). First a loop down
  the side the line's wholly on, a call only where a plane cuts it; then the
  walk in assembly (`src/tline.s`, `line_asm`): the far parts kept on a
  stack of its own, the bases in registers, the crossing's fraction by the
  divider and the midpoint by the C's own truncating multiplies. A 500-unit
  line (the stats line's LINE, at Outer Base's start): 562 -> 349 (the loop)
  -> 301 us; Installation's 326 -> 190. The walk's mostly memory now (a node
  and its plane in different lines). `OPT=-DLINE_CHECK`: the walk as it was
  alongside, 0 different in 3,015 lines (the gunner's fight).
- **The movers**: a trace looked at every mover's record, its place and
  whether it had gone (three lines a mover; Installation has 36). Now a table
  of their boxes where they are (`tbox`, made again when one's moved):
  107 -> 66 us a box trace there.
- **HWRAM**: once the models have had theirs, what a trace reads for every
  brush goes into what's left (`level_trace_hot`: the boxes, the leaves'
  lists, the brushes; 4 KB kept): Installation's boxes were on the cart.
  Comm Center gets its lists in. The marks of what's been done
  (`brush_check`) went to LWRAM (only a brush whose box meets the trace's
  is marked now, so it's hardly read), and level.c's built small: Outer Base
  had run out of HWRAM for the fight benchmark's build.

| | before (section 21 start) | now |
|---|---|---|
| the fight: traces a frame | 11, 7.3 ms | 13, 5.2 ms |
| the fight: the game's tick | 8.4-8.6 ms | 6.4 ms |
| the fight: frames over 40 ms | 14 of 495 | 0 of 502 |
| Installation, the gunner's fight: a trace | 0.42-0.46 ms | 0.35-0.42 ms |

The gunner's fight still does 18-48 traces a tick (7.5-17 ms): box traces,
~290 us each there (gathering 65, clipping 130 for 10 brushes looked at and
3 clipped, the movers 66, the entities 20), with the slave drawing on the
same bus. All three levels load; the static benchmark's CPU 1708 (noise).

## 22. The cells, and the DSP for the faces' setup

**Where a frame goes** (the static benchmark, `R_PROFILE`, both CPUs, a
frame; the new CASM and XFORM lines were divided by the views twice at
first, six times too small): the faces' transforms and culls 7.0 ms (325
faces, 111 of them culled there), draw_grid's setup 3.3, the grids 10.1,
the cells 17.8, the models 5.5, the walk 6.8. 214 faces have cells: 1,143
of them, 5.3 a face, so most of it goes on each face, not each cell. In the
cells: `cells_asm` 12.2 ms (its loop and cull 4.8, the 140 small crops
about 2.9 at ~550 cycles each, the 806 commonest cells about 4.5), the C
around it 5.6.

**Not the cache.** Each face drawn twice, the first undone: the second
only ~3 ms cheaper in all. (A grid call twice looked like 10.1 -> 3.8 ms
until it turned out grid_face_asm uses up its rows count: the second did
one row.) A grid call on a purged cache costs 10.0 ms against 8.9, so the
grids are compute, ~90 cycles a point. Stores cost 3.5 cycles, a cached
load 1.6 (a loop timed in-game).

**The DSP for the faces' setup: still no.** The setup's multiplies are
~20 fmul a face (~1.7 ms of the 7.0, both CPUs); the rest is the face's
record (32 bytes, two LWRAM lines: ~120 cycles), the axes' cache and the
compiler's code (a 156-byte stack frame, face_grid built and read back).
The faces come one at a time as the walk finds them and the slave takes
each at once, so there's no batch for the DSP to run ahead on.

**Done** (pixels: one CPU, `OPT=-DONE_CPU`, the six views the same before
and after; appended as well, `-DONE_CPU -DTEST_FIFO`, the master's way with
two, the order wrong on purpose. With two CPUs a change of speed moves
where they meet, and a few seams change hands: 7-130 pixels, a delay alone
does it):

- `cells_frame`: the fifteen things `cells_face` stored for every face that
  don't change all frame, once a frame for each CPU.
- The list's bookkeeping in `cells.s`: it reads the writer (count, LINK
  base, Gouraud count) and brings it up to date itself, where `cells_run`
  did ~50 loads and stores in C a call.
- The cull and near tests read each corner's outcode once, not twice.

| | before | after |
|---|---|---|
| static benchmark, six views summed: master | 117.4 ms | 110.8 |
| slave | 158.0 | 151.9 |
| CPU | 169.4 | 162.9 |
| the fight: CPU a frame | 37.9 ms | 36.5 |
| the fight: the slave's drawing | 33.6 ms | 32.2 |
| the fight: frame | 39.8 ms | 39.8 |

NTSC's 30 fps needs the fight's CPU under 33.3 ms: 3.2 ms to go. What's
left is per face: the transform and setup in C, ~990 cycles a face drawn
(~10 ms a frame, both CPUs); in assembly, perhaps half.

## 23. The face's setup in assembly

`src/face.s`, `face_asm`: draw_face's first part (the face's origin into
view space, its axes from the frame's cache or worked out, the cull at its
grid's four corners, the coarse grid when it's far) and draw_grid's setup (a
stored texel along each axis, the edge steps), then the whole grid
(grid_face_asm). It returns 0 nothing to draw, 1 out of view, 2 the grid
done, 3 a row at a time; `face_cells` (draw_grid's cells part) does the rest.
Quicker than the compiler's (a 156-byte stack frame, the grid's description
built on the stack and read back): the frame's constants read from GBR, the
corners tested one at a time until their AND comes to nothing (usually the
first), the lowest z from the steps' signs, the results stored where
they're used (`face_args` in r_ctx, set once a frame by `face_frame`).

Checked: the C (`face_setup`) is kept for `OPT=-DNO_FACE_ASM` and
`-DFACE_CHECK` (both, every face, their results and grids compared; the
axes' cache emptied for every other face so both work them out): 0
different in 96,651 faces on the static benchmark (802 far), 355,752
through demo1 and demo2 (`-DLEVEL_TEST`, 18,326 far), and 97,458 with
`-DROWS_TEST` (a face over 12 grid points a row at a time: 10,339 of them).
One CPU: the six views' pixels as before, with the assembly and without, and
with `-DROWS_TEST` against the old code made to do the same. The code's 224
bytes smaller; each level's HWRAM 48 bytes less, the monsters' records where
they were (an earlier try, face_cells and face_rows apart and the C setup
built in, pushed 3.2 KB of demo1's records to the cart).

| | before | after |
|---|---|---|
| static benchmark, six views summed: master | 110.8 ms | 99.4 |
| slave | 151.9 | 140.3 |
| CPU | 162.9 | 151.5 |
| the fight: CPU a frame | 36.5 ms | 34.6 |
| the fight: the slave's drawing / the master's | 32.2 / 16.9 ms | 30.2 / 15.0 |
| the fight: frame (PAL) | 39.8 ms | 39.8 |

(R_PROFILE: the setup and the whole grids 17.5 ms a frame, both CPUs,
against 20.2 for the three before.) NTSC's 30 fps: the fight's CPU 1.3 ms
over 33.3. View 5 of the static benchmark waits on VDP1 now and then (WT 9).

## 24. Water, glass, brightness

- **Under water** (slime, lava) the view's tinted: Quake's blends
  (`SV_CalcBlend`) as VDP2 colour offsets, with the hit and pickup flashes
  on top. An offset can only add, so each is the blend's pull on a middling
  colour, softened: water +20 +8 0 (of 255), slime -24 -8 -18, lava +100
  +14 -24. `main.c`'s table, if they want tuning.
- **Water moves** (`FF_WARP` faces nearer than 384 units, whole grids):
  its grid points rise and fall along the plane's normal (two waves 128
  units long, 2.5 units each way) and a ripple of light crosses it (3 of
  Gouraud's 31). Both go by where the point is in the world, so faces
  meeting agree at their edges; both fade out from 192 to 384 units, so
  far water, and its edges with near water, is as it was (the fade to
  nothing meets grid_face_asm's points exactly). The near-plane cells'
  corners (`cell_pos`) move the same way. Moving the texture across the
  face, as Quake does, would open gaps at the walls: most of the demo's
  water faces are one or two cells across. Options: WATER WAVES.
- **Translucency** (`SURF_TRANS33`/`66`: the water, windows, a few
  screens): VDP1's mesh (the default) or half-transparency. Options:
  TRANSLUCENCY OFF / MESH / BLEND. VDP1 has to read back every pixel it
  blends, and demo1's pool fills the view: 16.7 fps with BLEND against 25
  with MESH.
- **Brightness** 0 to 4 (gamma 1.15 to 1.75 on each 5-bit channel): every
  colour table through it on the way to the screen (the level's and the
  models' at a level's start, the gun's when it's switched, the sky's and
  the status bar's palettes), sent again when it changes. The lighting's as
  baked; the textures under it are brighter.

| over demo1's pool, looking down | CPU a frame | fps |
|---|---|---|
| none of it | 36 ms | 25 |
| the waves | 38 | 24.8 |
| the waves, the mesh | 39 | 24.2 |
| the waves, half-transparency | 39 | 16.7 (VDP1) |

Elsewhere nothing to speak of: the static benchmark 151.5 -> 152.1 ms
(six views summed), the fight 35.4 ms with them and 35.8 without (the
fight varies about a millisecond run to run). Checked: with them off
(`-DNO_WATER -DTRANS_MODE=0`) and brightness 0, one CPU, the six views'
pixels as before; with them, view 1's stained window is see-through.
`OPT=-DWATER_TEST=1` flies you over demo1's pool, `=2` puts you in it;
`-DBRIGHT=n` starts at brightness n.

Room: the code's 1.6 KB bigger. render_init, grid_row and grid_selftest
are built small now (level start, boot, the fallback), and cycles.c,
hud.c, sound.c and g_items.c; the brightness code too. Even so demo1 has
2.1 KB more of its monsters' frames on the cart (12,628 -> 14,752 bytes),
demo2 and demo3 1.7 KB less HWRAM left. Built small the water code would
cost 1 ms more over the pool for 450 bytes: not worth it. PLAN.md has a
better way (the cold code in low work RAM).

## 25. The seldom-run code in low work RAM

High work RAM is the fast one, and each level fills what the code leaves
with its hottest data: every byte of code was a byte of demo1's monster
frames pushed to the cart. Now the code that runs only at start-up, a
level's start, in the menus or seldom lives in low work RAM:
`engine/link.ld`'s `.lwtext` takes level.c's code (but `level_leaf`, marked
hot: it's each frame, for each entity), menu.c's, cd.c's, g_target.c's,
m_gunner.c's and cycles.c's, and every function marked `cold` (render_init,
the brightness's, main.c's level loading and debugging warps). It's linked
to run at 0x00200000 but loaded after .data; crt0 copies it down before it
clears .bss, which is laid over where it was loaded, so it costs high work
RAM nothing. The levels' low work RAM starts after it (15 KB). And
engine/rotplane.c (the taxi's city floor) and bup.c (backup RAM: nothing
saves yet) weren't used at all: no longer built (6.6 KB of .bss with them).

| | before | after |
|---|---|---|
| code in HWRAM | 127.8 KB | 109.7 KB (15.1 KB in LWRAM) |
| demo1: HWRAM left / its monsters' frames on the cart | 2.5 KB / 14.8 KB | 12.3 KB / none |
| demo2: HWRAM left | 25.3 KB | 49.8 KB (more of the traces' data in it) |
| demo3: HWRAM / LWRAM / cart left | 5.5 / 14.0 / 8.2 KB | 17.5 / 25.5 / 36.9 KB |
| the static benchmark, six views summed | 152.1 ms | 149.6 (the models 5.1 -> 4.5 ms) |
| the fight: CPU a frame | 35.4-35.8 ms | 34.4-34.5 |

The fight benchmark's build no longer needs files built small to fit
(`COLD_MORE`). Checked: one CPU, the six views' pixels as before; all three
levels load (`-DLEVEL_TEST`).

## 26. The slave's idle start; the DSP; dynamic lights

The fight benchmark now splits the master's time before the walk (`PRE`)
and shows the world's dynamic lights (`DLIGHTS`).

**The slave's first job.** For the frame's first ~3.5 ms the slave had
nothing to draw: the master was reading the pads, moving you (1.2 ms),
building the entities' list (0.6) and lighting them (0.6), then setting the
frame up (1.0). Now, with two CPUs and the game's tick during the drawing,
the slave builds the list as the frame starts (`g_render_ents`: the game's
done with them, its tick ran in the last frame's drawing) and lights them
once the master has the camera (`ents_light_pvs`, told by the master whether
the PVS marked is the camera's: render_world may be marking a new one
meanwhile). The master waits, if it must, only when render_world puts the
entities in their leaves, and purges its cache for what the slave wrote.
Moving you is slower with the slave on the same bus (1.20 -> 1.48 ms), but
the master's time before the walk is 3.70 -> 3.04 ms, and the fight's CPU
34.2-35.0 -> 33.5-34.1 ms. `OPT=-DNO_PRE_SLAVE`: as before.

**The DSP.** Looked at for three jobs:
- *the gun's vertices*: nothing to take. The gun keeps its drawing from
  frame to frame (the fight's `GUN US ... K414`: 0.4 ms, none of it
  vertices).
- *the models' lighting*: 0.7 ms a fight frame, when a dynamic light's near
  a model (162 normals against each light). The DSP could do the dot
  products, a third of it: ~0.2 ms off a frame, for a second DSP program
  and a hand-off mid-drawing. Not done.
- *the world's dynamic lights*: the big one, but not the DSP's kind of work
  (below).

**Dynamic lights.** `OPT=-DDL_BENCH` puts three lights by the camera in each
of the static benchmark's views (radius 200, a muzzle flash's colour):

| six views summed | CPU |
|---|---|
| no dynamic lights | 150.5 ms |
| three lights (dl_row) | 205.1 |
| three lights, the grid points' sums skipped (`-DNO_DL_SUMS`) | 163.2 |
| three lights, dl_face | 204.7 |
| three lights, dl_face with the reach tests | 200.9 |

Three lights near you cost ~9 ms a frame, 7 of it adding them at the lit
faces' grid points. `dl_face` does a face at a time (each light over all
its points, the sums packed three to a word) and skips a light for a face
or a row when the box of its points is out of its reach; the same sums bit
for bit (`-DDLF_CHECK`: 0 different in 20,227 lit faces). It gains little:
at ~50 cycles a point and light the arithmetic is the floor in this form
(the compiler's loop is already close to what assembly would do), and the
fight's lit faces are small (15 points x lights each), where the reach
tests cost what they save. The DSP would be no better: each face would be a
round trip with a CPU waiting (the faces' points exist only as they're
drawn), both CPUs would share it, the models have it first, and its
arithmetic wouldn't match. The way down is cheaper arithmetic: the squared
distance along a row is a quadratic (two additions a point), and so is the
light's weight; that's ~3x faster, but rounds a little differently.

## 27. Two routines in one DSP program: the models' lighting (a test)

`OPT=-DDSP_LIGHT` loads `engine/xformml.dsp`: the models' vertices
(xformm.dsp's routine) and their lighting, in one program. So the DSP runs
two jobs from one load (205 of its 256 instructions; the lighting loop's
start, past a D1 immediate's reach, set from data RAM). It branches between
them: each model's header (25 words now, one more than xformm.dsp's) says
how many lighting jobs are its, and the program jumps to the lighting
routine for those before the model's vertices, then back.

The lighting: a job for each model and dynamic light near it, set up by the
CPU at the frame's start (models_to_dsp: the light's direction wants a square
root and divides, which the DSP hasn't got). The DSP gives each of Quake's
162 normals its weight of the light, the C's integer steps exactly (a shift
by 14 is a multiply by 4 and ALH, the top of a 48-bit sum; the weight's
"dot > 0" a conditional jump): `-DDSPL_CHECK`, 96,552 and 87,318 weights
through two fights, 0 different. The CPU adds the colours and clamps; after
each job the DSP counts, so a CPU drawing a model waits for its jobs only.

First try, with all the lighting after all the vertices: slower, the
models' lighting 0.7 -> 2.0 ms a fight frame, the CPU 34.8 ms. The DSP did all
the models' vertices (2.9 ms) before any lighting, and the slave draws the
near models early, so it waited for their lighting behind everyone's
vertices.

Reordered, each model's lighting just before its own vertices (what the CPU
drawing it wants first; a model waits for its lighting no longer than for
its vertices): faster. `-DDSPL_CHECK` 88,938 weights, 0 different. Fight
frames, three runs of each:

| | the models' lighting | the models' time | CPU |
|---|---|---|---|
| lit on the CPU | 0.7 ms | 6.2 ms | 34.1 (all three) |
| `-DDSP_LIGHT` | 0.4 ms | 5.9 ms | 33.4, 34.4 |

(The models' times are both CPUs' summed. The lit-on-the-CPU build's fight
comes out the same each run; the DSP_LIGHT builds' not quite, the game's
tick 5.7 to 7.0 ms by how the fight goes, so the CPU's total is within that
noise: the 0.3 ms saved is split over the two CPUs.) But it doesn't fit:
the weights, jobs, normals and code take 9.7 KB of HWRAM, and demo1 had 10.5
KB left (`-DLEVEL_TEST`: 2.3 KB left with it, and 1.6 KB of its monsters'
frames on the cart, which section 25 took off it). So it stays a switch, off.

Also `OPT=-DDSP_NEAR`: the models to the DSP nearest first (the slave draws
from the front). No faster (the waits for the DSP were 0.2 ms a frame
already), though it sends it fewer models that aren't drawn (2.9 -> 2.2 a
frame, 2.1 drawn): the list's cap drops the far ones first. A switch.

Also tried, `OPT=-DSTEP_DL` (section 26's cheaper arithmetic): the squared
distance along a row stepped as a quadratic. Slower (-DDL_BENCH 200.9 ->
204.1 ms): a lit point's cost is its weight and three colours, not its
distance, and the rows are short. And it rounds differently: up to one
5-bit shade on some cells.

## 28. After the slave, for 30 fps on NTSC

**Measuring it.** The fight now plays the same fight every run: the dice
reseeded as it starts, and the game's step in whole fields rather than the
frame's time as measured (a few microseconds different each frame and each
build, which sent every build's fight its own way). And it runs as an NTSC
Saturn too (`REGION=na tools/fight.sh`: Mednafen as a US machine), which is
the number that matters here: its FRAME is 33.4 ms when every frame makes 30.

**Where the time goes.** `OPT="-DFIGHT_BENCH -DSLAVE_PROF"`: each CPU's
watchdog timer interrupts it every 16,384 cycles and counts where it was, in
8-byte buckets over the code, kept in low work RAM's top 64 KB (nothing moves
in high work RAM, so the timing's the game's); `tools/prof.py` reads them out
of a Mednafen save state (F5) and names the functions. The fight, a frame:

| slave | ms | master | ms |
|---|---|---|---|
| cells_asm | 5.5 | cells_asm | 5.5 |
| face_asm | 4.7 | the walk (walk_asm 5.3, cull 3.5) | 8.8 |
| grid_face_asm | 4.2 | grid_face_asm | 3.9 |
| the dynamic lights | 4.2 | face_asm | 3.4 |
| face_cells (the C between) | 2.7 | traces (leaf_brushes 2.2, box_leafs_r 1.4, ...) | ~5 |
| the models | 7.0 | face_cells | 2.1 |
| idle | 3.3-8 | vdp_submit, render_world's start, the game | ... |

The two CPUs share the drawing (they meet in the list), so a millisecond off
either's drawing is about half a millisecond off the frame. But the slave has
nothing to draw for the frame's first ~2 ms, while the master reads the pad,
moves you and sets the frame up: a millisecond off that is a whole one.

**Found on the way: SCU DMA stops both CPUs.** Mednafen halts both SH-2s for
the whole of any SCU DMA that reads or writes work RAM (the SCU takes their
bus); only A-bus to B-bus transfers (the cart to VRAM: the textures) run
alongside. So the lists' DMA into VRAM (0.76-0.78 ms a fight frame, after the
CPU's time: shown now as LISTS' DMA) costs that however it's sent, and the
gun's fetches from the cart cost both CPUs while they run. vdp_submit now
sends the frame's DMA as one SCU indirect-mode chain, not waited for (SCU
user's manual ST-097: the table's address in D0W, each entry count,
destination, source, the last source's top bit set): the queued textures
first (upload_one and the late ones: truly alongside, ~0.1 ms), then the
lists. The swap waits for the chain, and so does anything else sending DMA.

**The changes**, the fight, a frame (PAL's CPU; NTSC's frame and CPU):

| | PAL CPU | NTSC frame | NTSC CPU |
|---|---|---|---|
| before (with the fight made repeatable) | 34.1 | 35.2 | 33.7 |
| dynamic lights: a face's box against each light first | 34.1 | | |
| dl_face a row at a time (a row none reaches copied) | 33.9 | | |
| its points' loop in assembly (src/dlight.s) | 34.0 | 35.1 | 33.6 |
| pmove's second look at the ground skipped when nothing's changed; the benchmark's per-face timer gone | 33.1 | 34.3 | 32.7 |
| the gun's kept drawing in HWRAM where there's room | 33.0 | 34.2 | 32.6 |

- *Dynamic lights*: face_dlights marked a face for a light near its plane
  (a floor under a light a little above it, all of it); now the box of the
  face's grid corners must be in reach too: 31 faces a frame marked, 21 lit.
  dl_face tests each row's reach inline (its start stepped, not grid_step's
  multiplies), copies a row no light reaches (`raw | 0x8000`, which is what
  unpacking and packing gave) and lights the rest a row at a time; the points'
  loop is `dl_points` in assembly (the whole units by swap.w and exts.w, the
  squares by muls.w). The same sums bit for bit (`-DDLF_CHECK`: 16,423 lit
  faces, 0 different); `-DNO_DL_ASM`, `-DNO_DL_REACH`: the C, the old marking.
  The slave's share 4.2 -> 2.7 ms: what's left is mostly reading the faces'
  lights (low work RAM) and packing them. (STEP_DL's test path is gone.)
- *pmove* looked at the ground (a box trace 0.25 units down, and the water)
  before and after the move; standing still, nothing it reads has changed
  in between, so the second is left out then: the pad-to-camera time 1.48 ->
  0.91 ms, before the slave has anything to draw. (Moving, it's as before;
  and moving costs more traces, which this benchmark doesn't show.)
- *The gun's kept drawing* (section 15) is in what HWRAM a level has left,
  when it has 8 KB: demo2 and demo3, not demo1 (the fight's: still the cart
  and a DMA).

Same pixels throughout (the six views, three lights, one CPU); all three
levels load, none of their monsters' records on the cart (HWRAM left: demo1
8.1 KB, demo2 37.7, demo3 5.4).

Still ~5% of the NTSC fight's frames take a third field: about a millisecond
more to find. The one big thing left is section 13's portals: in this very
room they'd leave 32-48% of the cells drawn. After that: the walk (8.8 ms of
the master's, memory-bound), the traces (the player's are before the slave
can start), and face_cells' C.

## 29. Portals: built, measured, off

Section 13 measured what portals could cull; this time I built them.

**What's there** (`PORTALS=1 ./build.sh` rebakes the levels with the data,
`OPT=-DPORTALS` uses it; both off by default):
- *The baker* makes qbsp's portals again (tools/portal_estimate.py), keeps
  those between clusters, a box round each pair's (cached in obj/), and each
  cluster's list of them with the cluster on the other side: 83 KB on demo1.
- *The flow*, each frame: from the camera's cluster through the portals,
  breadth first, only into clusters in its PVS, a screen rectangle a cluster
  (each portal's box's rectangle from its view-space extents, cut down by
  the rectangle it's seen through; a box off one of that rectangle's sides
  found with no divides).
- *The faces*: one whose cluster wasn't reached isn't drawn; the rest are
  tested in face_asm against their cluster's rectangle (its four grid
  corners against the rectangle's sides, as they're tested against the
  screen's; the C's `face_setup` the same: `-DFACE_CHECK` 103,777 faces, 0
  different). A face in more than one cluster isn't tested; nor a brush
  model's (their leaves, in their own trees, say cluster 0: that was the
  first real bug, doors and lifts vanishing).
- *Checked*: a replay of the flow in Python against a z-buffer render of
  each benchmark view (demo1 and demo2: nothing seen is culled), and the six
  views' pixels on the Saturn with two CPUs: the same, but for a few seam
  pixels. (With one CPU the old build differed: its one list of 1,100
  commands ran out in view 6 and dropped the far faces, the sky showing
  where the portal build, with fewer to draw, drew them.)

**The trap.** Its first fight looked like 30 fps on NTSC (33.5 ms a frame,
the CPU 27.8). It was a bug: the rectangles' top and bottom were 96 pixels
out, and it culled what was on the screen; the pixel comparison caught it.

**What it's really worth.** Correct, the fight room keeps ~71% of its cells
(the simulation), and the drawing's time (both CPUs, a fight frame) goes
45.3 -> 39.0 ms: at most ~2.7 ms off the frame. The flow for it reaches 100
clusters, projects 637 portals and looks at 978: 21 ms in C. An assembly
flow with each portal projected once a frame would still be ~5 ms (the boxes
are in low work RAM: a miss each; then ~26 multiplies and two to four
divides). Also tried, in the simulation: clusters merged into rooms where
the opening between them is big (fewer visits, but as many projections: a
room borders as many portals), and narrowing only near the camera (the
unnarrowed rectangles go everywhere: more tests). So it's off: on this
level's 1,447 clusters and 4,163 portals the flow costs more than it saves.
Something cheaper would have to find the fight room's hidden faces some
other way (a finer facevis in the big rooms, say).

**Kept from it**: the fight benchmark's game step is now always two fields
(the rounding of section 28 still let one slow frame, the restart's, send
the fight another way): a new baseline, a heavier fight (2.8 models on the
screen, not 2.1): PAL CPU 34.0 ms, NTSC frame 34.8 ms, CPU 33.3. The
profiler's build keeps the level out of its low work RAM.

## 30. The models' dynamic lights, a frame behind

Your idea: light things a frame early, when a CPU's free. Where the slave's
free, measured (`SLAVE US: FIRST DYN END` in the fight): ~1.4 ms at a
frame's start (between its first job and the walk's first face) and ~1.9 at
its end (the master's HUD, text and submit; 0.8 of that the lists' DMA,
which halts both CPUs anyway, and some the benchmark's own text).

So the models' dynamic lights (draw_model's: 162 normals against each light
near it, on whichever CPU drew it, ~0.7 ms a fight frame) are now worked out
in the slave's first job, after it's told the master it's done with the rest
(so the master never waits for them): last frame's lights (`lights_lag`, kept
before this frame's effects make new ones), each model on the screen near
one given a table of its own from a pool of six, its base with the lights
added exactly as draw_model added them. The master, before it draws its first
model, checks they're done (`LIT_DONE`: they always are by then) and forgets
its cache's copies. A model off the screen, or when the pool's full, is lit
by draw_model if it's drawn after all. `OPT=-DNO_LIGHT_AHEAD`: as before.

The same pixels in the six views (their lights don't move, so last frame's
are this frame's). In the fight (the new baseline of section 29): NTSC 34.8
-> 34.5 ms, PAL CPU 34.0 -> 33.9: the slave's start was idler than its draw
time was busy, but the job (1.26 ms, more models than are drawn) fills it.
What you'd see: a light that's moving lights the models from where it was a
frame (33-40 ms) before; a new flash lights them a frame late. HWRAM: the
pool's 2 KB (demo3 has 1.6 KB left now).

**The walls, tried** (`OPT=-DWALLS_AHEAD`, off): each CPU notes the whole
faces it lit; when the slave's done drawing, while the master finishes the
frame, it lights them again for the next frame with this frame's lights,
from their grids in the world (face_setup's steps from the world's axes, and
dl_face as it is, given the lights), into one of two buffers in low work RAM
(the master may still be drawing from the other), a face at a time, stopping
when the next frame's first job comes; the next frame, a face found there
isn't lit again, the rest are lit as before (with last frame's lights, to
agree). It works: the six views a shade out on a few cells (the world's
rounding, not the view space's). But the slave's free time at a frame's end
(~1.2 ms once the lists' DMA, which halts it anyway, is out) lights ~3 of a
fight frame's 20 lit faces (it stops at the next frame's job 389 frames in
578; never for room): 0.4 ms less while drawing, nothing off the frame
(NTSC 34.6). So off: the walls go by this frame's lights, as before; the
models keep theirs a frame behind. (The DSP could do all of them, a frame
behind, from the cart's copies of the faces and their lights, which its DMA
can reach, as low work RAM isn't: ~1.2 ms off the frame, for a program of
its own swapped in beside the models'.)

## 31. The walls' dynamic lights: before the DSP, what they cost

Before writing the DSP's walls I measured what it would take off. In the
fight (NTSC), the slave spends 2.8 ms a frame on the walls' dynamic lights
(dl_face 1.8, its points' loop dl_points 1.0) and the master almost none:
the lit faces are the near ones, and the slave draws from the front. But
there are only ~20 lit faces a frame, ~16 points each, mostly one light:
the sums themselves are a few hundred points x lights. Most of the time was
around them: each row's box tested against each light, a call and its setup
for each row and light, each row unpacked and packed on its own.

That changes what the DSP would buy. Its arithmetic isn't the cost; and to
use it the CPUs would have to write each lit face's grid to the cart (15.5
cycles a word, ~30 words a face) and add its results to the faces' own
lights as they're drawn, which is much of what dl_face does now. Worth it
only as the whole job on the DSP (the faces' grids and lights read from the
cart by it, the finished colours read back): a program of ~200 words,
swapped with the models' each frame, the lights a frame behind, for maybe
half a millisecond more than the change below.

**dl_face a face at a time.** The whole face's sums unpacked once; each
light in reach of any of it (face_dlights' box test) over all its points by
one call of `dl_rows` (src/dlight.s: dl_points' loop, with the rows' starts
stepped there too); packed once at the end. No row's reach is tested: at
~16 points a face those tests cost more than the points they'd skip, and
the three lights of `DL_BENCH`, bigger faces, don't miss them either. The
same sums bit for bit (`-DDLF_CHECK`: 21,320 lit faces, 0 different);
`-DNO_DL_ASM` is the C it was, a row at a time. Its sums take 2 KB of stack
(the slave's frames on the way to it: ~3.5 KB of its 16).

| | before | after |
|---|---|---|
| the fight, NTSC: frame / CPU | 34.7 / 33.2 ms | 34.1 / 32.6 |
| the fight, PAL: CPU | 33.9 | 33.2 |
| the walls' dynamic lights, both CPUs (FIGHT_BENCH's DLIGHTS) | 3.0 ms | 1.9 |
| the slave's profile: dl_face + its loop | 1.80 + 0.97 | 1.01 + 0.93 |
| the static benchmark with `DL_BENCH` (three lights), six views' CPU | 200.1 ms | 191.0 |

What's left of it is the work: the sums (dl_rows) and unpacking and packing
each point (~40 cycles a point in all, and low work RAM's misses reading the
faces' lights).

## 32. Moving you early: the slave, at the end of the frame before

Your question: the pad's only read (`pad_collect`, the SMPC's last answer:
it reads the pad at the top of each field, a few ms to answer) when a frame
starts, so why not read it when the move's made, wherever that is? Then
moving you before the frame starts costs only as much as it's early, not a
frame.

So the slave, its drawing done, waits for the master's (the game's tick ran
before the master drew its half, so everything the move reads is final),
reads the pad then and makes the next frame's move: the camera turned, the
doors and lifts, pmove (`premove` in main.c). Meanwhile the master finishes
the frame (the HUD, the lists sent); at the next frame's start it waits for
the move if it's not done, forgets its cache's copies (the move writes the
player, the doors, the traces' brush marks, the dice), and uses the pad the
slave read for everything else a frame start does with it (menus, START,
the weapon). The master asks for it only in a frame the game runs in and
the slave draws in; with a menu up or the benchmark's views showing the
slave leaves it to the master, as before. dt is the frame's own (the next
one's length isn't known yet). `OPT=-DNO_PREMOVE`: as before.

The pad is read ~2 ms before the frame starts, not at its start: what you
press reaches the screen that much later on average (the SMPC only reads a
pad once a field, so mostly it's the same reading; now and then the field
before's).

| the fight | before | after |
|---|---|---|
| NTSC: frame / CPU | 34.1 / 32.6 ms | 33.7 / 31.9 |
| NTSC: pictures up 3 fields (of ~590) | 24 | 7 |
| PAL: CPU | 33.2 | 32.6 |

The slave waits ~0.15 ms for the master's drawing and moves you in 0.92
(standing: walking costs more traces); the master's wait for it at the next
frame's start is ~0. The camera, gun and effects' times at the frame start
went up (V 130 -> 236 us, F 128 -> 213): the lists' DMA, which halts both
CPUs for 0.78 ms as a frame starts, used to fall in pmove and now falls
there (not the cache: the same without the purge, tried).

Checked: `OPT=-DMOVE_TEST` (the same scripted moves, a fixed dt: stand,
walk, turn, jump, back, curve; where you end up) gives the same position,
yaw and fall speed, bit for bit, with and without; played (walking,
turning, jumping, firing, pausing and resuming); `-DLEVEL_TEST` through
the three levels' exits. The fight's result screen shows the fields each
picture was up (`FIELDS UP`) and the early move's times (`EARLY MOVE`).

## 33. The walls' dynamic lights on the DSP (built; off: OPT=-DDSP_WALLS)

**Can the program RAM be swapped?** `OPT="-DFIGHT_BENCH -DMODEL_CHECK
-DDSP_SWAP_TEST"`: every frame of the fight the models' program loaded again,
and after its job another program run with a job of its own, checked: 1,025
frames, 0 wrong, the models' vertices still 0 different from the C. A CPU
loads a program at 6.9 cycles a word (`-DPPD_TEST`). And the DSP can load
its own: a DMA into program RAM with `MVI x,PC` straight after writes it
from x and goes on after the MVI, in what's just been loaded (Mednafen's
notes say so of the hardware; it runs so here).

**What it has to do.** The fight's lit whole faces (`-DFS_STATS`): 75% have
16 points or fewer, none more than 64 or 12 a row, most one light; ~300
points a frame. Worth the DSP only if it does the whole job (the CPUs no
work a point): the face's grid from its record, its lights, the sums, the
clamp, the packing. That's ~400 words, and the models' program leaves 131.

**Two programs, chained by the DSP.** When the models' job is done and the
host has listed faces, xformm.dsp loads `engine/walls1.dsp` over itself
(a loader at 253-255 of each program). walls1 reads each face's record and
its axes from the cart's copies, and writes a block: its sizes, where its
lights start, its origin and dvt, its columns' offsets (a_i dut) and its
rows' (b_j); then loads `engine/walls2.dsp`, which reads each block and the
face's lights (the cart's copy), works out each grid point's position, each
light's share as dl_face does (the floor of each way's distance, f, (c f) >>
16 each colour, the clamp), packs the lit lights two to a word onto the
cart, and loads the models' program back (it stops at its 255). The CPUs
load no program; the three are one file (cd/WALLS.BIN) put on the cart each
level. walls1 186 words, walls2 213. A point in the DSP's data RAM is
juggled across its four banks (one address counter each, and a bank written
by an instruction can't be read by it).

**Exact.** `OPT=-DWALLS_TEST` at a level's start: three lights by where you
start and the 24 faces near them: the DSP's lit lights against `dw_model`
(the same arithmetic in C), 311 points, 0 different; and dw_model against
dl_face itself (in the world, as walls-ahead sets a face up), 0 different
(73 of the points lit). 5.6 ms of DSP time, three lights on every point.

**In the frame** (`-DDSP_WALLS`): each CPU notes the whole faces it lit;
the next frame the master lists them for the DSP with that frame's lights
(0.05 ms), after the models' job; the frame after, a face lit then that the
DSP lit is drawn with its lights (its lines forgotten, from the cart), the
rest by dl_face; the walls go by last frame's lights, as the models do.

| the fight, NTSC | the CPUs | DSP_WALLS |
|---|---|---|
| frame / CPU | 33.7 / 31.9 ms | 33.6 / 31.9 |
| pictures up 3 fields | 7 | 5 |
| the walls' dynamic lights, both CPUs | 1.9 ms | 1.3 |
| lit faces the DSP had | | 10 of 20 |

It works, and gains next to nothing: the DSP lights the faces it's told of
a frame early, and it's told of those lit the frame before that, two frames
before its lights are drawn; the fight's lights are blaster bolts, moving,
so half the faces they light have changed. And the time it saves is mostly
the slave's on the near faces, drawn while it's waiting on the walk anyway.
Started after the walk instead (with the faces the slave had lit by then,
for the next frame) was worse: 5 of 20. So off. What would make it pay: the
DSP choosing the faces itself, every whole face the CPUs drew against the
lights (it has the time: ~25 ms of a frame idle), for which the CPUs would
note every whole face they draw (~300 a frame) somewhere it can read.

## 34. The walls' dynamic lights on the DSP, the DSP choosing the faces (on; OPT=-DNO_DSP_WALLS: off)

**A third program, before the other two.** `engine/walls0.dsp` (229 words)
now runs first after the models' job. The CPUs note every whole face they
draw, lit or not (its record's address on the cart: the master's up from the
middle of a buffer, the slave's down, so the two are one list; ~160 a fight
frame). The next frame walls0 takes each face of that list, with that frame's
lights, and does face_dlights' two tests on it:
- its plane (the cart's copy): -8 < the light's distance from it < the
  radius, on the face's side, as the CPU works it out;
- its box (from its grid's axes, nu du by nv dv, a little bigger than the
  grid): each light's centre within the box stretched by the radius + 2
  units each way.

The faces some light may light are listed, each with those lights (a bit
each), up to 60. walls1 sets them out as before, while there's room in the
host's buffers (and cuts the list's count if not). Then it writes over the
list, for the host, each face's index << 16 | where its lit lights go.
walls2 lights each face with just the lights its bits say (a table: the
lights sit 0 1 2 0, so any two are next to each other).

A lighter filter would light faces the CPU wouldn't, which costs only DSP
time: a light that reaches none of a face's points adds nothing. One that
misses a light the CPU uses would be wrong. It never does: of the CPU's lit
faces the DSP didn't have, 0 were ones walls0 turned down.

**Exact.** `OPT=-DWALLS_TEST`, at a level's start: 512 faces near where you
start, three lights. walls0's picks match `dw_filter` (the same tests in C),
and the lit lights match `dw_model` and dl_face itself, on all three levels:
- demo1: 57 faces, 753 points;
- demo2: 60 faces, 1,121 points;
- demo3: 60 faces, 576 points.

In the fight (`-DDW_CHECK`: each face the DSP lit also lit by dl_face, as
drawn), 150,520 points, 0 different.

One bug turned up, in walls2, and it was in section 33's version too. A face
with an odd number of points packs its last point with whatever word is in
data RAM after it, and a big one (the models' job leaves them) spoiled the
last point: 1 point in 753. A 0 is written there now. It was found with a
Python copy of the DSP (`tools/dspsim.py`: Mednafen's semantics; run on the
programs' own binaries and the levels' own data by `tools/walls_sim.py`),
which gives Mednafen's results exactly.

**The host's part.** Before the slave is signalled, the master:
- puts the last job's faces in a table in low work RAM (stamped with the
  frame, so it's never cleared; the DSP's list read through the cache, its
  lines forgotten first);
- writes this frame's lights to the cart, and the seven numbers that change
  (the rest set at a level's start);
- starts the chain after the models' job.

That's 0.13 ms (0.30 as first written: clearing the table, reading each
face's record, the list read a word at a time uncached). Noting the faces
costs the CPUs ~0.1 ms. The DSP was never still busy at the next frame's
start.

| the fight, NTSC | the CPUs | DSP_WALLS |
|---|---|---|
| frame / CPU | 33.9 / 32.2 ms | 33.5-33.6 / 31.6-31.9 |
| pictures up 3 fields, of ~595 | 14 | 0-5 |
| the walls' dynamic lights, both CPUs | 1.97 ms | 0.52-0.55 |
| lit faces the DSP had | | 17 of 20 |

On PAL (50 Hz), every picture is up 2 fields either way (39.8 ms), and CPU
goes 32.9 -> 32.5 ms. The builds' layouts alone move CPU by ~0.3 ms (the
same code, one function moved to low work RAM: 31.6 -> 31.9). Section 33's
33.7 / 31.9 and 7 for the CPUs was measured on an earlier layout.

The 3 of 20 lit faces the DSP didn't have are all a light's first frame,
when there was no job (no lights the frame before, so no faces noted).
Noting faces in every frame, lights or not, would cover them for ~0.1 ms of
the CPUs in every frame.

**What it costs.**
- The look: the walls go by last frame's lights, a frame behind, as the
  models' already do (every wall, the CPU's faces too, so they agree).
- High work RAM: 1.8 KB (the code; on demo1 that sends 1.9 KB more of the
  models' frames to slower RAM).
- Cart: 24 KB. A level gets it if there's room after it for as many gun
  slots as the level would have anyway. demo3 has room for only one, with
  or without these, and 2 KB is left there after everything.
- Low work RAM: 512 bytes.

On by default (the walls a frame behind decided on). A level whose cart has
no room for it keeps the walls on the CPUs, going by this frame's lights.
The default build's fight: 33.6 / 31.8 ms, 3 pictures up 3 fields.

**Noting faces every frame, lights or not** (`-DDW_ALWAYS`, a test). The
DSP then had all 20 of the fight's lit faces (the 3 a light's first frame
missed are covered), and the walls' dynamic lights cost the CPUs 0.20 ms,
not 0.53. But the fight's frame didn't move (33.6 / 31.8 ms, 3 pictures up
3 fields, both ways). The static benchmark (six views, no dynamic lights)
paid for it: CPU 154.1 -> 156.1 ms over the six, ~0.35 ms a view on each
CPU.

**Cheaper noting didn't make it cheaper.** The noting moved into a small
function of its own (face_cells' registers left alone), the coarse-grid
test only for faces that have one, and a 16-bit index stored, not a 32-bit
address. walls0 takes them two to a word and multiplies by 8; when the
list's first is a word's second half, the host gives walls0 that word. The
cart's lists are halved. Exact as before, odd starts too
(`tools/walls_sim.py` runs both). But the static benchmark paid the same
(154.3 -> 156.4 ms), and with no store at all it was 156.1. The cost is the
call and its tests on each of a busy view's few hundred whole faces, ~30
cycles each, not the cart.

**So noted while there are lights, and for 60 frames (2 s) after.** A
fight's next light's first frame then has the DSP's lit lights: all 20 of
the fight's lit faces are the DSP's, the walls' dynamic lights 0.22 ms of
the CPUs, the frame 33.5 / 31.8 ms with 2 pictures up 3 fields. A quiet view
pays nothing (the static benchmark 154.2 ms, as without).

## 35. The loading screens' background

The loading screens showed the last level's sky behind their words. Now
it's Quake 2's own console background (`pics/conback.pcx`, 320 x 240, the
middle 224 rows: `tools/bake_conback.py`), as Quake 2 shows while it loads.
It's a 256-colour bitmap on VDP2's NBG1, read from the disc once at boot
straight into VDP2's VRAM bank A0, which nothing else uses (the sky is in
B1). Its palette (Quake's) goes into colour RAM at 0x300 through the
brightness setting each time it goes up. It's up with any loading message
and down once the level's in (the sky's set up again over it). It costs no
work RAM or cart, and nothing in the game.

## 36. The sky

It was the middle 96 rows of the skybox's four sides (about 17 degrees
above and below the horizon), in 15 colours, the faces pasted side by side;
above and below it, VDP2's line colours. So most of the time you looked up
you saw flat red, and the sun and clouds were in bands.

Now (`tools/bake_sky.py`) it's all six faces, mapped as Quake 2 maps them
(ref_gl/gl_warp.c), onto a cylinder round you whose radius is the screen's
focal length, so looking level a row is a screen line. It runs from 63
degrees up (320 rows) to 19 down (56), 1024 a turn. Its top and bottom rows
fade into the line colours above and below it, so there's no edge.

Its colours: 8 x 8 cells of 16 colours, each cell with whichever of 48
palettes suits it best. The cells are grouped by colour, each group gets a
palette (median cut), each cell then goes to the palette that suits it
best, six times over. The sky's average colour error is 1-2 levels in 255
against the picture. The palettes take colour RAM 0x000-0x1FF and
0x300-0x3FF; Quake's palette at 0x200 is the status bar's and now the
loading screens' picture's too.

It costs nothing in a frame (the static benchmark's views: the same to the
0.1 ms). It's 1024 x 376 at 4 bits a pixel: 188 KB of VDP2's VRAM, in banks
A1 and B0, which nothing used. Each level has its own file (cd/DEMO1.SKY,
198 KB) read from the disc straight into VDP2 as the level loads, so it's
no longer in the level's file on the cart. Each level has ~49 KB more cart
free: demo1 208 KB, demo2 114 KB, demo3 50 KB, which was 2.

The sky's setup had been rewriting VDP2 settings the loading screens'
picture uses (its bitmap mode, priority, colour offset and VRAM slots)
while a level was still loading. Mednafen doesn't mind; a Saturn might.
Now each leaves the other's alone.

`OPT=-DSKY_VIEWS` makes the benchmark's six views ones of the sky from
demo1's yard, 6 s each, for pictures of it.

## 37. Translucency: blend, plain water (an option)

Blending (VDP1's half-transparency) reads back every pixel it draws over,
so where water fills the view it's VDP1 that holds the frame up. A fourth
setting, TRANSLUCENCY BLEND, PLAIN WATER: translucent water's cells drawn
as half-transparent Gouraud polygons in its texture's average colour (its
first cell's colour table, at the brightness chosen), not textured
sprites, so VDP1 has no texels to read for them. Glass and screens stay
textured. Their commands are made as before and changed after the face is
done (`plain_water`, in RAM before the lists' DMA), so nothing changes in
the cells' assembly, and nothing costs anything with another setting.

Over demo1's pool, NTSC (`OPT="-DWATER_TEST=1 -DSTATS -DTRANS_MODE=n"`):

| | fps | CPU |
|---|---|---|
| mesh | 24.1 | 39 ms |
| blend | 19.8 | 38 |
| blend, plain water | 19.8 | 39 |

No gain in Mednafen, and Mednafen can't show one. Its VDP1 timing charges
a pixel 1 cycle and a blended one 5 more for the read-back, but nothing for
reading a texel (its source has that line commented out), so plain and
textured cost it the same. On a Saturn the texel reads cost something, but
if Mednafen's proportions are near right the read-back is most of it.
Worth trying on the hardware; if it doesn't pay there it can go.

## 38. Sound: the pop at the start, the clicks, the jitter

Found from Mednafen's own recording of what it plays (`RECORD=out.wav
tools/emu.sh start`), each sample's level read back:
- **The pop** as the sound driver starts (the end of the boot's loading,
  and each level's): the output stepped to +11,762 and down again in
  steps every 32 and 43 ms over a quarter of a second. Those are the
  reverb's two delay lines. The SCSP's DSP runs on its own, so as the
  driver wrote its program a step at a time it ran half set up, filled its
  delay lines with what was there, and played them back. The game never
  turns the reverb on. Now the driver stops the DSP first thing and mutes
  its return; the reverb starts only if asked for, its ring cleared first
  and its program written last. No pop in the recording now.
- **The menu's clicks**: its move and select sounds were cut short (0.3 s
  of 0.6, 0.5 of 1.1) while still at a fifth of their peak, faded over 6
  ms, so each ended in a click. They're whole now (10 KB more in each bank:
  57-94 KB still free), and any sound still cut fades over 40 ms.
- **The jitter**: the machinegun's shots, 100 ms apart in Quake, came 65
  and 135 ms apart in turn. The game's 10 Hz tick runs at a frame's start,
  so at 25 fps (PAL: 40 ms frames) its ticks are 80 and 120 ms apart; and
  the 68000 takes the SH-2's requests at 60 Hz, which makes those 67 and
  133. At 30 fps three frames are 100 ms, so NTSC is steadier. Fixed: as
  the game runs each tick (and the player's shot, which a tick allows) it
  tells the sound how long ago that tick's moment was (the tick
  accumulator's remainder: `s_lag`). Each of its sounds is started that
  moment plus a frame (two fields) on: the SH-2 sends a delay with it
  (`SND_CMD_DELAY`), and the 68000, its timer now at 919 Hz (the music's
  tick still 60 Hz, every 92 counts' worth), holds it till then. Sounds
  not of a tick (the menu, footsteps) start at once. The machinegun's
  shots now come 100 ms apart, PAL and NTSC, for ~20 ms more delay on
  average on PAL (less on NTSC).

## 39. Every entity the levels have (the table of 64 was full)

Outer Base's exit didn't work: the lift goes down, and nothing happens.
The game had room for 64 entities (monsters, items, triggers, relays,
targets), and Outer Base needs about 148 at medium skill; once the table
was full, the rest of the level's entity records were never spawned. That
was 220 of its 372 records, the trigger at the bottom of the lift shaft
among them (and its `target_changelevel`), and half its monsters, items,
triggers and secrets. Installation and Comm Center lost as many (they need
~168 and ~171). Doors, lifts and buttons are movers.c's, so the levels
looked whole. Found with `OPT="-DEXIT_TEST -DSTATS"` (on the lift, facing
its button; `=2`: in front of the exit's doors, flying) and a count of what
didn't spawn (`D` on the stats line's position row: 0 now).

**Two sizes of entity.** The player, monsters, barrels and func_explosives
are full ones (304 bytes, at most `MAX_FULL`, 64: their boxes stay in
HWRAM's `g_solids`). Items, triggers and the rest (relays, timers, targets,
`G_UseTargets`' delayed stand-ins) are short: only the first 96 bytes of
`g_ent`, the fields they use. A trigger's box is in `g_trig_areas` instead,
an item's is 15 units round it, and what's read every tick or frame comes
first, a 16-byte line at a time. The short ones are laid out items first,
then triggers, then the rest, with each item's place and leaf
(`g_item_spots`) and each trigger's box beside them: the touches each tick
read only those, and only an item or trigger the player touches. An item
or barrel's model is checked at spawn (`g_pool`: one the level has no model
for isn't counted or spawned).

**Room.** For everything the level has at any skill (`g_skill` -1, the title
and the benchmarks, spawns it all) where there's room, else for this
skill's; low work RAM first, keeping 5 KB for what comes after (`dw_hash`),
then the cart only if it has 160 KB to spare. Comm Center's cart decides
the DSP walls and the gun's slots (`r_wall_level`, `view_level_init`), and
any less there loses the DSP walls (as a first try did: 18 KB of short ones
on its cart). Rarely run game code is `cold` now (low work RAM): HWRAM's
240 bytes better off than before.

| left after loading (bytes) | HWRAM before / now | LWRAM before / now | cart before / now |
|---|---|---|---|
| demo1 | 2,608 / 2,848 | 125,360 / 111,280 | 212,992 / same |
| demo2 | 28,704 / 28,944 | 8,784 / 9,904 | 116,736 / 98,304 |
| demo3 | 4,336 / 4,576 | 20,448 / 5,552 | 51,200 / same |

Every level keeps what it had where it had it: the DSP walls, the gun's
slots, the brushes' boxes in HWRAM on Comm Center, the monsters' records.

**What's drawn.** The renderer still has 64 entities for the game
(`GAME_ENTS`; HWRAM has no room for more: one's 384 bytes, its light by
normal most of it). Each monster, barrel or item gets one while it's in the
PVS marked or within 1,024 units of the camera, and keeps it until it's
been neither for 60 frames, or another in the PVS needs it (one only near
gives way). Only those in the PVS are filled in each frame; the rest keep
their light for when they're back. The PVS marked is the last frame's when
the slave fills them in, so on a frame the camera's gone into another
cluster `g_render_late` fills in the ones kept too, once the camera's
moved. Once drawn, an item only turns (its spot goes when it's taken) and a
barrel stays: only a monster is read each frame. `OPT="-DSTATS -DSLOT_CHECK"`
counts, after the walk, the models in this frame's PVS and on the screen
without one: walking each level from its start (`MAP=`; New Game stays on
it) and then warping to items, 0 on demo1 and demo2, 1 on demo3 (beyond
1,024 units, on a frame the camera changed cluster), and no request for
one refused; up to 45, 64 and 64 held. Before the first PVS is marked,
`visframe` 1 (not 0) keeps leaf_vis's zeroes from looking like one.

**Speed.** `OPT=-DOLD_SET` spawns only what the table of 64 had room for,
to time the same fights:

| | before | OLD_SET | everything |
|---|---|---|---|
| fight, PAL: frame / CPU | 39.8 / 32.4 ms | 39.8 / 32.1 | 40.4 / 38.6 |
| fight, NTSC: frame / CPU | 33.5 / 31.8 ms | 33.5 / 31.7 | 38.4 / 36.8 |
| fight, NTSC: pictures up 3 fields | 2 of 596 | 0 of 597 | 152 of 521 |
| static benchmark: CPU / frame (summed) | 1541 / 2180 | 1537 / 2173 | 1788 / 2268 |

So the tables cost nothing; the levels cost more now they're whole. The
fight benchmark's room wakes more monsters than it did (the game's tick
5.5 -> 11.1 ms on NTSC, 8 -> 18 traces a frame; the models' drawing 2.8 ->
4.9 ms), and the static benchmark's views have 2.5 times the models'
drawing. Filling the render entities was 0.94 ms a frame on the static
benchmark's master at first (vs 0.58 before), from filling more of them
and from reading each one's entity; filling only those in the PVS, and
items and barrels only once, made it 0.59 with the same entities (0.84
with all of them).

## 40. The traces' brush sides

With every entity in the levels (section 39) the fight's monsters walk more,
and each step is a box trace down onto the floor: 10 a frame on NTSC, about
0.56 ms each, 5.8 of the traces' 7.9 ms a frame (the call sites read from a
save state, `g_trace_sites`). Counted a trace (`tr_count`, `tr_ticks`):
finding the brushes (the leaves the move's box touches) ~4,400 cycles, then
their lists, boxes and marks ~3,400, and clipping the 3.3 brushes whose
boxes meet the move's, 30 sides, ~4,300: 144 cycles a side, mostly waiting
on low work RAM for the side and then the plane.

**Remembering the brushes near each monster** (and you) was tried first: a
list per walker, of the brushes round it, kept till it walked out of that
box, so each step skipped finding them. Exact (`NEAR_CHECK`: none different
in ~40,000 traces, once two walls entered at once broke ties the same way
whatever the order), but slower: 7.9 -> 10.0 ms of traces a frame. Finding
the brushes is only ~30% of a trace, and making a list again (a box 64
units bigger each way, ~25,000 cycles, one step in five) cost what it saved;
the order-free ties cost a third more clipping besides. Not kept.

**The box's sides from the box.** qbsp gives every brush its box's six
sides first (-x +x -y +y -z +z), and in all three levels every brush has
them there: three quarters of all brush sides. Where those are on whole
units (79% of demo1's brushes, 98% of demo2's, 93% of demo3's), `brush_bounds`
keeps the box exactly (not a unit out) and marks the brush `BRUSH_EXACT` (a
contents bit Quake doesn't use, taken off what a trace reports); clipping
it, those six sides are worked out from the box the trace has just read, the
same sums as from their planes, and only the side it enters is read, if
it's the trace's. The exact boxes leave out a few more brushes, all a unit
or more from the move. `OPT=-DCLIP_CHECK` clips each brush both ways and
compares, and clips each brush the exact box left out that the rounded one
wouldn't have: walking each level and warping to its monsters, none
different of 107,000 brushes clipped, and none of the 343 left out would
have mattered. (The divider's also used here directly, not through `fdiv`:
the same, a call less.)

A side now 114 cycles, a brush's clipping 3,430 a trace (was 4,330):

| the fight | before | now |
|---|---|---|
| NTSC: frame / CPU | 38.4 / 36.8 ms | 38.0 / 36.5 |
| NTSC: traces a frame; pictures up 3 fields | 7.9 ms; 152 of 521 | 7.4; 142 of 526 |
| PAL: frame / CPU | 40.4 / 38.6 ms | 40.0 / 38.1 |
| PAL: traces a frame; pictures up 3 fields | 10.0 ms; 14 of 494 | 9.2; 5 of 499 |

The static benchmark doesn't trace: CPU 1788 -> 1791, frame 2268 -> 2269.

What's left of a trace is waiting on low work RAM: a brush's box (12
bytes, so half of them across two lines), its leaf's list, its mark (another
array), and the sides past the box's. A record of 16 (box and mark in one
line) would cost Installation's and Comm Center's boxes their place in
HWRAM; not done.

## 41. The box traces' leaves and brushes in assembly

`src/tbox.s`: `leafs_asm` for `box_leafs_r` (the leaves a move's box
touches) and `brushes_asm` for `leaf_brushes` with `clip_box_brush` and
`test_box_brush` (each leaf's brushes, clipped, or tested where the box
stands). The same results, bit for bit: the same leaves in the same order
(child 0's before child 1's, a child 1 kept on a stack of 64 rather than a
call made: the C does it if that runs out), the same brushes, the same
sums. The start and end are added to the box's mins and maxs once a trace
(adding's the same in any order, mod 2^32, so the sides' distances come out
as the C's); slanted sides are the C's `fmul`, a product at a time; the
fractions are the C's `frac_div`, on the divider. It writes the C's `tr`
straight. Short moves, position tests, and the leaves of a long move's walk
(`hull_check`) all use it; the C's loops are left for `OPT=-DNO_BOX_ASM`
and to check against.

`OPT=-DBOX_CHECK` runs the C's way after the assembly's on every short move
and position test and compares the leaves and the trace: walking each level
and warping to its monsters (17,846 traces) and the NTSC fight (8,921), none
different. On the way it found one bug: a load of the constant 2 (`mov.l
.Ltwo`) in a branch's delay slot. A PC-relative load there takes its
address from the branch, not itself, so where the alignment fell it read
the -2 beside it: a side whose entering fraction is clamped to 2 (the box
wholly outside the brush that way) instead entered at -2, and a brush the
box missed was hit (121 of 11,668 traces on demo1). No other assembly here
has a PC-relative load in a slot.

| a box trace in the fight (cycles) | C | assembly |
|---|---|---|
| its leaves | 4,354 | 3,665 |
| its brushes | 6,457 | 4,977 |

| the fight | C | assembly |
|---|---|---|
| NTSC: traces a frame | 7.4 ms | 6.4 |
| NTSC: the game's tick, most | 28.2 ms | 24.9-25.1 |
| NTSC: frame / CPU | 38.0 / 36.4 ms | 37.6-38.1 / 36.0-36.6 |
| PAL: traces a frame | 9.3 ms | 8.0 |
| PAL: frame / CPU; pictures up 3 fields | 40.0 / 38.1 ms; 4 | 39.8 / 37.5; 0 |

(The two NTSC runs of the assembly differ by where the trace code and
demo1's monsters' records sit, which sends the fight its own way.) A trace
is still mostly waiting on low work RAM (a brush's box, its leaf's list, its
mark, the brush, its sides), which the assembly can't shorten.

The assembly is 1.4 KB of HWRAM, and its arguments 0.5 KB: that cost Comm
Center the room it kept its brushes' boxes in (its cart then filled). So
the traces' seldom-run C went to low work RAM (`cold`): `hull_check` (a
long move), `box_leafs_r` and `line_check` (for when the assembly's stacks
run out), `trace_init`. HWRAM is now 1.2 KB better off than before on demo2
and demo3, and on demo1 that room went to the monsters' records (1,584 bytes
of them left on the cart, from 3,488); every level keeps what it had where
it had it. Comm Center's short entities, with the code in low work RAM, are
sized for the skill played rather than every skill (section 39).

## 42. The monsters' drawing: their light after the hand-off, the DSP's list

Where the slave's time goes in the NTSC fight (`OPT="-DFIGHT_BENCH -DSLAVE_PROF"`),
the models: their commands 2.4 ms a frame, draw_model's C 2.4, the
polygons 2.3, `model_shade` (a model's light by normal, made again when it
changes leaf or turns a sixteenth) 1.2, the vertices 1.1, the dynamic lights
1.0. The DSP does the vertices (xformm.dsp); the light's all the slave's, at
the frame's start (the dynamic part on the DSP was section 27: not worth
its HWRAM).

**The light after the hand-off.** The master waited 1.06 ms a frame for the
slave's first job (`pre_wait`): the entities' list, their leaves, then their
light. render_world needs only the leaves (to put them in their leaves); the
light's wanted only to draw a model, and the master waits for the slave's
dynamic lights there already (`lit_wait`, after its walk). So `ents_leaf`
before `PRE_DONE`, `ents_shade` after, then the dynamic lights. The lines of
each model's light the master can have read with what it did read (its
first and last) are forgotten in `lit_wait` (`ents_shade_forget`).
`OPT=-DSHADE_CHECK`: each model drawn, its light as the drawing CPU reads it
against it made again: 4,145 in the fight, none different (and none without
the forgetting either: the walk turns the master's cache over first; it
stays, cheap). The same light as before, made at the same point in the
frame.

| | NTSC frame / CPU | pictures up 3 fields | PAL CPU |
|---|---|---|---|
| before | 38.1 / 36.6 ms | 145 of 525 | 37.5 ms |
| the light after the hand-off | 36.9 / 35.3 | 110 of 542 | 37.0 |

**The DSP's list.** It takes 64 blocks of 16 vertices (12 KB of HWRAM): a
soldier's or an infantry's 15, so four monsters; the rest go the CPU's way,
in C, ~7 times the slave's time a monster (1.6 ms to the DSP's 0.2). With
every entity in the levels, the list (every model in the PVS, in the
entities' order) filled with items before the monsters. Now only those that
may be on the screen (ents_light_dyn's sphere test: 7 drawn after all of
3,700), the big ones (100 vertices or more) first, and one with no room
left out rather than the rest. In the fight the C's draws went 1,354 -> 632,
all of them monsters past the fourth; 80 blocks (a fifth) made no
difference to the frame. The DSP's sums and the C's round a little
differently, as for any model past the list's room before.

| | NTSC frame / CPU | PAL CPU |
|---|---|---|
| the light after the hand-off | 36.9 / 35.3 | 37.0 |
| and the DSP's list, monsters first | 37.0 / 35.4 | 36.8 |

The static benchmark: CPU 1798 -> 1801, frames 2273 -> 2277 (noise).

**HWRAM.** The renderer had 96 entities, 32 for the projectiles, but there
are 24 at most (`MAX_PROJ`, now in q2.h): 88, 3 KB less. With it, demo1's
monsters' records all fit (none on the cart, from 1,584 bytes), demo2 has
2.5 KB more, and on demo3 the gun's kept drawing now fits in HWRAM, so the
cart has room for both gun slots (its DSP walls still on; 256 bytes of
HWRAM left).

What's left of the monsters: past the fourth on screen each costs its C
transform (~1.6 ms); in assembly that might be ~1 ms less each. More of the
DSP's list would take HWRAM (2.9 KB a monster) or a smaller output from it.

## 43. The far monsters' vertices first; the DSP's list where a level leaves room

The end of section 42 was wrong about where the time went. Timed by way
(`OPT="-DFIGHT_BENCH -DMF_PROF"`: each model's vertices, from after its
light to its polygons, by the DSP's or the CPU's way, the whole mesh or the
far one), PAL fight, a vertex:

| way | before |
|---|---|
| the DSP's, the whole mesh (mverts_asm) | 6.5 us |
| the DSP's, the far mesh (a C loop, project()) | 11.6 us |
| the CPU's (all of them far, as it happens) | 17.8 us |

Every monster that went the CPU's way was a far one (beyond 400 units: the
coarse mesh, a third of the polygons), and the far mesh's vertices were
scattered through the model: the DSP still did all of them (a soldier's 15
blocks for 45 used), and both ways then picked theirs out one by one, each
a cache line of the frame on the cart (75 cycles) and C's project().

**The DSP's results on the cart** (blocks past HWRAM's 64 there, an address
a model in the DSP's header) were no good: the CPU reading them back from
the cart, a far vertex's x, y and z three lines apart, cost more than it
saved (16 us a vertex; PAL CPU 37.2 -> 37.1). Gone.

**The far mesh's vertices first.** tools/bake_md2.py now numbers the
vertices the far mesh uses first (the polygons and frames renumbered to
match), so a far model is its first nfverts vertices, as a whole one is all
of them: the soldier's 45 of 227, the infantry's 67 of 240, the gunner's 41
of 329, so 3, 5 and 3 of the DSP's blocks rather than 15, 15 and 21, and
drawn by mverts_asm (the C loop and the vertex lists, and their HWRAM, are
gone). Old bake against new: every polygon's corners the same bytes in
every frame, and the textures, colour tables, shading and normals the same;
a model whose list isn't 0, 1, 2 ... (an older bake) has no far mesh.

**More of the list where a level leaves HWRAM.** The DSP's header has a
word more, where the model's blocks go (xformm.dsp), and after the gun's
slots (nothing takes HWRAM after them) `r_dspm_level` takes what's left
for more blocks, up to 96: Installation 19 (a near monster more), Comm
Center 96, demo3 5 (a far one). With the list cut to 16 blocks (everything
in the level's more), the fight was the same: CPU 36.6 ms, none the CPU's,
the monsters drawn right.

**The CPU's way in assembly: not done.** With the above, no monster in the
fight goes the CPU's way. A crowd made by cutting the list to 16 blocks and
no more (3.9 models a frame the CPU's, at 14 us a vertex to the DSP's 6.3):
PAL CPU 36.6 -> 36.9 ms. An assembly version would win back ~0.2 ms of that,
and only in a crowd.

| | NTSC frame / CPU | pictures up 3 fields | PAL CPU | static CPU / frames |
|---|---|---|---|---|
| section 42 | 37.0 / 35.4 ms | 113 of 541 | 36.8 ms | 1801 / 2277 |
| far vertices first, more blocks | 36.7 / 35.1 | 104 of 545 | 36.5 | 1765 / 2257 |

HWRAM left after it all: demo1 96 bytes, demo2 14,608, demo3 32 (the cart
as before: 212,992, 98,304, 2,048; DSP walls on).

## 44. The monsters' coarse mesh nearer: tried, left at 400 units

`OPT=-DMODEL_FAR=n` (or `r_model_far`) sets where a monster takes its
coarse mesh (a third of the polygons, the first 41 to 67 vertices).

**The fight** (NTSC, CPU 35.1 ms at 400) didn't move at 300, 200 or 150:
its monsters are beyond 400 units nearly all the time, coarse already.

**The static benchmark** (PAL, its 6 views summed, 0.1 ms):

| from | CPU | frames | models |
|---|---|---|---|
| 400 units | 1766 | 2257 | 10.1 ms |
| 250 | 1723 | 2259 | 8.5 |
| 200 | 1697 | 2232 | 7.7 |
| 150 | 1691 | 2239 | 7.5 |

(view 6: CPU 35.1 -> 32.0 ms at 200; the frames move little, held to whole
fields.)

**The look.** `COMPARE=far CMP_EXTRA="-DFAR_LINEUP -DMODEL_FAR=30000 -DFAR_B=0"
tools/compare.sh`: each held view stands you in front of a soldier, then an
infantry (the game's own entities: the renderer's are only those in sight),
150, 200 and 300 units off where a line to it is clear, and UP flips its
whole mesh for the coarse one. The soldier's shoulders and arms go blocky
at 150, less so at 200; the infantry loses its gun arm's shape and the red
glow on it at 150 and 200. (`COMPARE=far` alone: the benchmark's own views,
400 against 200: only two views differ, their monsters 15 to 30 pixels
tall.)

Left at 400, the user's call.

## 45. The build's trade-offs in one file; where the frame goes now

**src/settings.h** has the settings that trade looks or latency for speed,
each with what it was measured at: `MODEL_FAR`, `LOD_Z`, `TRANS_MODE`,
`BRIGHT`, `NO_WATER`, `NO_GAME_DURING_DRAW`, `NO_LIGHT_AHEAD`, `NO_PIPE`
(README: Settings). Edited there or given to the build
(`OPT="-DMODEL_FAR=250" ./build.sh`). The game built from it is the same,
byte for byte.

**The NTSC fight now** (frame 36.7 ms, CPU 35.1; `OPT="-DFIGHT_BENCH
-DSLAVE_PROF"`, a frame):

| slave | ms | master | ms |
|---|---|---|---|
| cells_asm | 6.9 | the walk (walk_asm 5.6, cull 3.2) | 8.8 |
| face_asm | 5.4 | cells_asm | 4.5 |
| grid_face_asm | 5.2 | the traces (brushes_asm 2.4, leafs_asm 1.5, ...) | ~6.8 |
| face_cells | 3.2 | grid_face_asm | 3.2 |
| the models (commands 2.4, polygons 2.2, vertices 1.7, light 2.1, draw_model 1.0) | 9.4 | face_asm | 2.8 |
| idle (slave_main) | 1.0 | face_cells | 1.7 |

The walls are about half of both CPUs' time. The game's ~9.6 ms is in
every frame, not every third (the monsters tick in groups, a group a
frame): every frame carries it. face_asm's busiest places, on both CPUs,
are just after its first reads of the face (low work RAM: two misses a
face); the walk's, after its stack pushes and the writes that publish each
face to the slave.

**Not the fight's:** the walls' coarse grid nearer (`LOD_Z` 192: CPU 35.0
ms, the same; this room's walls are big, few wholly beyond it), nor the
monsters' (section 44).

## 46. Every model was drawn inside out

Roper: the barrels looked inside out, and the monsters sometimes showed the
backs of their heads. They were: draw_model kept the polygons whose screen
winding was MODEL_FRONT -1, and every model's front faces wind +1 (as the
gun's do: its VIEW_FRONT 1 was put down to the guns winding the other way;
it was the sign). So each model's near side was dropped and the inside of
its far side drawn. A monster is near enough symmetric that it mostly looks
like a monster from behind, mirrored; a barrel from above showed its
bottom plate and the underside of its rim, not its lid.

How it was found: `OPT="-DBENCH_HOLD -DFAR_LINEUP -DLINEUP_BARRELS"`
(COMPARE=far tools/compare.sh) stands each view 80 units in front of the
level's next barrel at a standing eye's height; the same barrel rendered in
Python from the MD2 with a depth buffer (the reference), and from the baked
file the Saturn's way (its quads, their warped textures, the depth buckets),
both right; the Saturn's wrong, the same with the CPUs doing the vertices
(`NO_DSP`), right with the sign flipped. The soldier and the infantry
against references from straight in front: right only flipped.

Now MODEL_FRONT is 1 and draw_model passes the gun's sign (mpolys_asm's
-1). `-DMODEL_CHECK` in the fight: 207,784 vertices, 91,680 buckets, 2,898
models' commands, none different from the C. The times are the same (NTSC
36.6 / 35.1 ms, 103 of 546 up three fields; PAL CPU 36.4; the static
benchmark 1765 / 2260).

(The barrel is browner than Quake 2's grey: its one colour table of 15 for
the whole skin, `sharedlut`, takes the red badge and the grey into browns.)

## 47. Holes in big views: a CPU's command list full

Roper: holes in far-off things in a big room on Outer Base, and a roof on
Installation that flickers, the sky showing through. There's no draw
distance (everything the PVS and the faces' visibility lists give is
drawn); what isn't drawn is what a full list or texture cache drops. Each
CPU has its own part of the VDP1 list (build.sh: the master 1,100 commands,
the slave 1,300, 400 for the gun and text), and the texture cache its own
half; a cell or polygon that finds no room is left out (the stats screen's
DROP and FULL; now the benchmarks' too: `DROP n FULL n` under the static
benchmark's table, `CMDS MOST ... DROP ... FULL` under the fight's). Which
faces go depends on where the two CPUs met in the list, which moves from
frame to frame: the flicker.

The static benchmark on Outer Base dropped 210 commands (the master's
list full in one view, the slave's with room); the fight, none (the master
draws less there: the game's tick is its), its most M455 S951.

**The fix, without memory:** a CPU within `CMD_SPARE` (96) commands of its
list's end stops taking items, and the other (they meet in the list) takes
the rest; both lists full is the only way to drop now. The order holds: the
master's items are the far end, drawn first; what it leaves is nearer than
its own and farther than all the slave's. The static benchmark: 210
dropped -> 0, the most M1007 S1208; the view that dropped 1.2 ms slower (it
draws what it didn't); views 1-3 the same pixels, 4 and 5 under 100 pixels
(the split between the CPUs). The fight the same (NTSC 36.7 / 35.2 ms).

More room in all costs fast RAM (the list is built in HWRAM: 32 bytes a
command, which demo3 hasn't got) and the texture cache's VRAM (64 bytes a
command, the two lists): a choice to make if holes stay where both lists
are full.

## 48. Two models in one leaf: the nearer drawn over the farther

Roper: two medkits side by side, the far one over the near. Each leaf's
models were listed in the order the entities came (the walk lists a leaf's
faces and models nearest first, and the drawing goes back to front from
it); now each goes into its leaf's list by its distance from the camera,
nearest first (render_world: a few multiplies an entity). The barrels'
views: the same pixels but where models overlap (14, 16, 145 pixels in
three of six).

`OPT="-DBENCH_HOLD -DUSER_VIEWS={x,y,z,yaw,pitch},..."` (with
tools/compare.sh) holds views of your own: the stats screen's place (its
x y z, the eye 22 above) and its yaw, to look where someone else did.

(Half a soldier against a wall, Roper's other picture, is the other way
round: a model is drawn where its origin's leaf is in the list, and one
whose origin is across one of the BSP's planes from part of it can be
drawn before a face that's behind that part. Not fixed: drawing a model at
the nearest leaf it touches would draw others over walls in front of them.)

## 49. Far faces missing: the faces' visibility lists (a setting)

Roper: from one place on Outer Base the far walls through a doorway show,
from a little further back they don't (the sky instead). DROP and FULL
were 0: nothing ran out; those faces weren't in the list at all. Each
cluster's list of the faces it can see (tools/facevis.c, section 22) is
made by rendering from points in the cluster: each leaf's middle and a
few random ones, 24 a cluster. Where none of them sees a face, it isn't
drawn from anywhere in the cluster.

**Measuring it.** `facevis in.bin out.bin --check n`: n new random points a
cluster, each rendered, and what they see that their cluster's list hasn't
(faces, and pixels of the six 128 x 128 views). `--at x y z`: one point,
its leaf and cluster (`FACEVIS_PVS=1`: within the PVS only, as the game
draws). `--view x y z yaw out.ppm`: a picture of it (the sky red).

| points seeing a face their cluster's list hasn't | Outer Base | Installation | Comm Center |
|---|---|---|---|
| 24 a cluster (as it was) | 31% (23.9 pixels a point) | 32% (14.8) | 30% (26.6) |
| and each leaf's eight corners | 23% (12.2) | | |
| 64, and the corners | 10% (4.6) | 10% (1.9) | 8% (3.4) |

(The corners: a leaf's far ends see what its middle doesn't; each drawn in
towards the middle until it's in the leaf. More resolution changed nothing.
24 as it was gives the lists as they were, bit for bit: the same check.)

But fuller lists cost: more faces listed, and more leaves "seen" so more
models drawn that are behind walls (4.9 -> 6.0 a fight frame). The NTSC
fight 36.7 / 35.2 -> 38.3 / 36.7 ms, 106 -> 152 pictures up 3 fields; PAL
CPU 36.5 -> 38.1; the static benchmark's CPU 1781 -> 1852. So it's a
setting, the old way the default (the default build after it all: NTSC
36.5 / 34.9 ms, 99 of 548 up 3 fields; PAL 36.2; the static benchmark 1790):
`FACEVIS=64c ./build.sh` (README: Settings; build.sh rebakes when it
changes, and the lists are cached by the tool's source too).

And Roper's place itself: on a walkway 176 units up, at the edge of where
you can go. From there, with the fuller lists, nothing in the PVS that can
be seen is missing (0 faces from the second place, 1 face, 2 pixels, from
the first); what is missing is outside its cluster's PVS, Quake's own
visibility (the map's compile), which the game draws by as Quake 2 does.

## 50. ~700 KB of the cart back each level: the copies the load makes

Roper asked what else is only needed setting up. The level's file goes onto
the cart whole, and the load then copies the BSP's nodes, leaves and marks
into HWRAM and the cells and brushes into LWRAM where they fit; the cart's
copies of those were never read again. `OPT=-DLEVEL_TEST` counted them:
~690, 757 and 752 KB.

Now tools/bake_map.py writes those lumps last (`TAIL`: brushsides, brushes,
leafbrushes, cells, marks, leaves, nodes, the likeliest to fit last), and
the load (level.c `level_tail`), once it's copied them, gives the cart
back from the first of a run copied to the file's end (a lump that didn't
fit stays, and what's before it; a file not laid out that way gives
nothing back). The DSP's reads (the planes, faces, lights and axes) aren't
among them; the entities' records stay (a restart spawns from them).

| (the default build, LEVEL_TEST) | DSP walls | gun slots | the cart left |
|---|---|---|---|
| Outer Base | on | 2 | 212,992 -> 919,552 |
| Installation | on | 2 | 98,304 -> 796,672 |
| Comm Center | on | 2 | 2,048 -> 520,192 |

(LEVEL_TEST's line now: `W` the DSP walls, `K` the gun's kept drawing in
HWRAM, `G` its slots, `N` the gunner loaded, `E` the entities' tables on the
cart (1 the full ones, 2 the short), `B` what the cart got back.)

Comm Center has room for the gunner again (its model goes onto the cart if
two guns' and 16 KB more fit after it), so its gunners are back, as the map
has them. Their records take some HWRAM, so its gun's kept drawing is on the
cart now (a DMA a fight frame, ~0.3 ms, section 15) where it was in HWRAM.
The entities' tables: g_room took the cart (room for every skill) before
LWRAM sized for one once the cart had 160 KB to spare, and Comm Center's
short ones went there; now LWRAM for every skill, LWRAM for this one, then
the cart, so they stay where they were. (Installation's short ones were on
the cart already: there was never LWRAM for them.) Restarting a level
spawns as it did (it reads the entities' records, which stay).

## 51. STATISTICS: off, the frame rate, everything

Roper asked for the frame rate on its own. The options' STATISTICS goes OFF,
FPS (the frame rate in the corner, over the last few frames) and ALL (the
overlay as it was). `OPT=-DSTATS_FPS`: FPS from the start (`-DSTATS`: ALL).

## 52. FACEVIS=64c the default

Roper's choice: the levels' visibility lists from 64 points a cluster and
each leaf's corners (section 49), fewer far faces missed for ~1.5 ms of a
fight frame (the NTSC fight 38.3 / 36.7 ms, 152 of 522 pictures up 3
fields; PAL CPU 38.1 ms, 5 of 499 up 3 fields; the three levels load, the
cart left 913, 790 and 512 KB). `FACEVIS=24 ./build.sh` has the old lists. The entities'
records (section 50) stay on the cart: 21, 21 and 23 KB a level, which the
cart has room for now (520-920 KB left); loading them off the CD at a
level's start and restart would give back only that.

## 53. The berserker (Comm Center's five)

`src/m_berserk.c`: Quake 2's m_berserk.c as the gunner was ported (section
19): it runs at you and swings, the spike (15 to 20) or the club (5 to 10),
and has nothing for a distance (the port's AI asks the one attack function
further off too; the berserker's only swings in reach, else keeps running,
as Quake's, which has no ranged attack). Left out to fit: its fidget and
idle sound, walk, search sound and the gibs. BERSERK.MDL: stand, run, the
spike and club (att_c1-20), two pains, two deaths, 76 of its 244 frames, two
skins, a far mesh: 196 KB on the cart, loaded where there's room (optional,
as the gunner). Its sounds in the banks of levels with berserkers; and the
gunner's now in Comm Center's too (left out while its gunners were: section
50 gave them back without them). Comm Center's bank: 464 of 480 KB.

Tried on Comm Center (`MAP=demo3 OPT="-DNEW_GAME_DEMO3
-DWARP_ONLY=MDL_BERSERK"`: a new game there, START + A to the next
berserker): it comes at you, swings and hits (100 -> 63 in three seconds),
and two together finished me. `OPT=-DLEVEL_TEST`'s line: `N` the gunner and
the berserker loaded, and how many berserkers (Comm Center: 1, 1, 5).

The cost: its code is in low work RAM (built small) on every level, ~1.5 KB,
and on Comm Center that's what tips its short entities' table onto the
cart (LWRAM 18.6 KB left after). Comm Center's cart: 289 KB left. The
fight (Outer Base, no berserkers) the same: NTSC 38.4 / 36.8 ms.

(After: the berserker's code was in HWRAM, not low work RAM: build.sh's
COLD only builds a file small; engine/link.ld names the files whose code
goes to low work RAM, and m_berserk.o wasn't among them. With it there,
784 bytes of HWRAM back, and Comm Center's gun's kept drawing fits in HWRAM
again (no DMA a fight frame); its low work RAM 17.8 KB left.)

## 54. The monsters' code loaded per level

Roper asked whether every level needs every monster's code. It doesn't: the
soldier and the infantry are on all three, but the gunner only on
Installation and Comm Center, the berserker only on Comm Center. So those
two are now files on the CD, read by the levels that have them:

- build.sh `build_overlays` (after game.elf is linked, before the disc:
  engine/build.inc.sh's `POST_LINK`): each monster's file compiled small
  with `-DOVERLAY`, linked against game.elf's symbols (`-R game.elf`,
  engine/overlay.ld: code, tables and data in one image, its spawn's
  address first) at two bases; tools/overlay.py takes the words that
  differ by the bases' difference as the ones to move: GUNNER.OVL 2,928
  bytes (71 to move), BERSERK.OVL 2,016 (53).
- g_main.c `g_overlays_load`, after the models: each one whose model loaded
  is read onto the cart, its image copied into low work RAM and moved by
  where it landed; its spawn goes in a table g_init calls through. No file
  (or a bad one): its model counts as not loaded, its monsters left out.

Their code was in low work RAM on every level (1.7 KB) and their tables in
HWRAM (2.3 KB); now only where they're needed. Outer Base gets both back;
Comm Center has them both, in its LWRAM instead (14.6 KB left). Comm
Center's berserkers and gunners and Installation's gunners as before
(tried there: they come and hit). The fight the same (NTSC 38.4 / 36.8 ms).

(Then: the code runs where it's read, on the cart, not copied into low work
RAM. With the tank's on Installation the gunner's and the tank's took 9 KB
of its low work RAM and pushed its entities' tables onto the cart; the
tables are read far more often than a monster's think runs, and a cart
miss costs about what a low work RAM one does (75 cycles to 59). So the
monsters' code on the cart, moved in place, and the tables back in LWRAM.)

## 55. The tank (Installation's)

`src/m_tank.c`, loaded with the levels that have one (section 54): Quake
2's m_tank.c. Near, its machinegun swept across you (19 shots, from 40
degrees one side to 40 the other) or three blaster bolts; further off,
those or three rockets; on hard it fires again while it can see you, and
it isn't put off by pain while firing. Left out: its walk (it runs as it
walks), its stomp on you once you're dead, its idle and death-thud sounds,
the gibs. TANK.MDL: 211 of its 294 frames, two skins, a far mesh: 457 KB on
the cart (Installation has 311 KB left after it); its sounds (sight, pain,
death, step, the three guns) in Installation's bank: 472 of 480 KB.

Its 405 vertices and 430 polygons are more than the renderer's room for a
model was (the gunner's 336 and 384): now 416 and 432, ~3.2 KB more HWRAM on
every level (each CPU's vertex arrays, and the gun's kept drawing, sized
by them), which puts Comm Center's gun's kept drawing back on the cart.

Tried (`MAP=demo2 OPT="-DNEW_GAME_DEMO=2 -DWARP_ONLY=MDL_TANK"`): it stands
in ambush as the map has it, and once shot it comes round and kills you in
seconds (rockets). The level tour: all three load, the DSP walls and two
gun slots on each (`T` on the line: the tank's model). The fight (Outer
Base) the same: NTSC 38.3 / 36.7 ms.

Stood in front of it (NTSC, the stats overlay): 21 to 27 fps while it
stands, 19.8 while it fights; the CPUs' 34 to 45 ms a frame, the models'
part 9 to 11 ms of it (the tank's 430 polygons the most of any model). A
medium mesh between the whole and the coarse one, for the middle
distance, would be the way to win some back if it matters.

## 56. Gibs

Quake's ThrowGib and ThrowHead: a monster killed with its health at or
under its `gib_health` (the soldier -30, infantry -40, berserker -60,
gunner -70, tank -200), or a body shot again until it is, goes to pieces
rather than dying (g_main.c `g_gib`). The pieces are each monster's own in
Quake's m_*.c: the soldier 3 meat, a chest and a head; the infantry,
gunner and berserker 4 meat, 2 bones and a head; the tank a meat, a chest,
4 bits of metal and the gear for a head. Each starts somewhere in its box
(the head from where the head was), at Quake's VelocityForDamage speed
(0.7 of it under 50 damage, 1.2 over), with the splat (misc/udeath, in
every level's bank).

They're fx.c projectiles (`P_GIB`, from the same 24 slots as rockets and
grenades): they tumble, fall at Quake's 800, slide off walls, stop on a
floor (lifted 4 units off it so they're drawn in the room's leaf, not the
floor's), and go after 5 to 10 seconds (Quake's 10 to 20: the slots are
few); a rocket or grenade with no free slot takes the gib nearest its end.
The models (tools/models.txt `gib_*`): one frame each, so on the cart, not
in HWRAM; the three every level needs (meat, bone, head) and the chest
cost 51 KB of cart on Outer Base and Comm Center, Installation's tank adds
metal and gear (63 KB). The code's cold: 608 bytes of low work RAM.

Two things on the way:

- The effects' models (rockets, grenades, now gibs) were never lit: the
  slave's ents_shade does the game's entities, and theirs were drawn with
  whatever light their slot last had (black, for a gib). fx_render lights
  them itself now, for their leaf and yaw, as ents_shade would.
- Resting gibs vanished. With two CPUs the slave's first job
  (g_render_ents) set `nents` back to the game's 64 while the master's
  fx_render was setting it to all 88: whichever wrote last won, and when
  the slave did, nothing from fx.c was put in its leaf that frame (gibs
  in the air happened to win the race; lying still, they lost it). Rockets
  and grenades could flicker out the same way. Now only fx_render sets it,
  and the slave's three loops (ents_leaf, ents_shade, ents_light_dyn) go
  to the game's 64 by name.

Tried: soldiers and infantry on Outer Base, the tank on Installation,
berserkers on Comm Center (rockets, `WARP_ONLY`): pieces fly, land, lie lit
on the floor and go. The level tour: all three load, the DSP walls and two
gun slots each; low work RAM 106.4 / 7.1 / 18.5 KB left, the cart ~860 /
~248 / ~227 KB. The fight (no gibs in it: its player doesn't shoot) NTSC
38.7 / 37.1 ms, 160 of 517 a frame late (38.3 / 36.7 and ~152 before);
built with the gibs left out it's 38.5 / 36.9, so ~0.2 ms is the code's
new layout and the rest the gunners' grenades now drawn every frame. PAL
40.3 / 38.5 ms, 12 of 496 late.

## 57. A review of the code: bugs, and what was left to win

Roper asked for a look over the whole thing for anything that would make it
quicker without changing the look or the game, and for bugs. Five readings
of the code (the world's drawing, the models, the game's tick, the frame
loop and the two CPUs' hand-offs, the assembly loops), each claim checked
against the code before anything was touched, each change measured, and
every change to the renderer compared pixel by pixel on the six views
(`tools/abcompare.sh`). Where it started: the fight NTSC 38.7 / 37.1 ms,
160 of 517 pictures a frame late; the static benchmark 185.6 ms of CPU
over its six views, 204.5 of frame.

**Bugs** (fixed, in the game):

- `vlen` overflowed past ~1,180 units on a side (units x 32, squared in 32
  bits), so `range()`, `infront()` and radius damage went wrong across a
  level: a monster far off could count as near, and fire.
- The sight sound played twice on seeing you (FoundTarget's and
  FindTarget's), and on being heard, shot or triggered; Quake plays it once,
  on sight.
- A triggered spawn kept the leaf (and the blend origin) of where it was
  placed, not where it dropped to, so it could be drawn in the wrong PVS.
- `M_CheckAttack` had none of Quake's skill scaling: half the chance on
  easy (and three in four chances in reach passed), double on hard.
- A body was solid only through its death animation, so "a body shot
  again" (section 56) only gibbed then. Now a dead monster stays solid for
  shots and blasts (Quake's CONTENTS_DEADMONSTER in MASK_SHOT) and nothing
  walks into it. Tried: a soldier dead to the blaster, the next bolts into
  its body: in pieces.

In the renderer:

- The screen culls were spheres of 48 and 64 units round a model's origin;
  the tank reaches 72 above its, so its top vanished when its origin was
  low in the view. Each model's reach over its frames is found as it's
  parsed (`model_parse`) and the culls use that where it's bigger.
- A brush model was listed in the leaf its centre started in and never
  relisted: a lift or door was skipped once its start's leaf was out of
  the view's PVS. Relisted when it's moved.
- A leaf's box was tried against the view's planes before anything in it
  was listed, but a model reaches 32-48 units past its origin: one poking
  into the view from a leaf whose box is just outside it was missing for
  the frame. The box test is gone (the leaf's faces come by its nodes; what
  it holds culls itself).
- The Gouraud tables were split half and half between the writers; the
  slave can't want more than one a command (1,300), so the master gets
  1,500 for its cells, the gun's polygons and the overlays. Running out is
  counted (the stats' `G`) rather than drawn with the last table.
- The lagged dynamic lights were skipped by this frame's count rather than
  the lagged count: the same only with the DSP walls on (latent).
- The frame stamps across the u16 wrap, every 36 minutes: a slot used last
  frame read as two back, and an axis stamped 65,534 frames ago could
  match.
- The 68000's command ring was posted to from both CPUs (the slave's
  premove: steps, jumps, splashes, movers), a read-modify-write of its
  index that was safe only by timing. The move code's sounds are queued
  and the master posts them as it takes the move, which is also when the
  move shows.
- `tools/bake_md2.py` refuses a model over MAX_MVERTS / MAX_MPOLYS (a
  silent clamp before: stale vertices drawn).

**Speed, kept** (each pixel-identical on the six views):

- Only two Z buckets are used (one a CPU, each in painter's order), but 512
  were reset, copied uncached and chained every frame, all of it serial:
  `-DZBUCKETS=2`. Static CPU 185.6 -> 182.0, frame 204.5 -> 200.8 (~0.6 ms
  a frame).
- The walk: a node's faces tested a byte (eight) at a time, the leaf box
  test gone, the scratch registers saved once by a wrapper: walk 47.1 ->
  45.2 over the six (WALK_CHECK: 0 diffs).
- No blend (lerp 0, about half the models): the DSP stream takes the first
  frame's terms as they are and model_xf skips the second's. A resting gib
  keeps its leaf.
- The game: a monster whose step found the floor doesn't trace for the
  ground again (Quake's groundentity from the step); the player's
  step-slide skips the raised pass when the first went the whole way clear
  (three traces a frame while walking); a moving mover tests the player's
  box once a frame, not twice. (The fight bench shows none of these: its
  player stands still and its monsters mostly shoot.)
- A crop's four corners share their edge points (16 multiplies, not 24):
  static CPU 181.4 -> 180.7.
- The face setup's step multiplies interleaved so no result is read at
  once: nothing measurable. Mednafen's SH-2 doesn't seem to charge the
  multiplier's stalls (the divider's wait it does: `src/cycles.c`), so
  stall-covering can't be measured here; kept, as it's identical and can
  only help a real machine.

Where it ended: the fight NTSC 38.0 / 36.6 ms, 142 of 526 late; PAL 40.0 /
38.1, 3 of 500; the static benchmark 181.0 CPU, 199.9 frame. The three
levels load as before (HWRAM 128 / 160 / 48 bytes left).

**Looked at, not done** (for Roper to pick from):

- The slave isn't idle before the walk after all: it waits 142 us for the
  first face (`SLAVE US: FIRST`), its 2.6 ms of dynamic lighting filling
  the window; moving work onto it would delay its drawing.
- The normal index of each vertex read from the frame on the cart at a
  stride of 4 (a 75-cycle miss every 4 vertices): ~0.3 ms if baked as a
  run after the vertices, at 166 KB more cart on Installation (248 KB
  left), 106 on Comm Center, 48 on Outer Base. Roper's call.
- Each face's cells and lights as one run in low work RAM (the bake, the
  level loader, both renderers' indexing): maybe 0.5-0.8 ms a frame off
  the misses; Installation's 7 KB of LWRAM left rules out any padding.
- Pre-scaled vertex indices in the polygons and the texture record's SIZE
  word baked: ~0.35 ms of the models' time, a bake format bump touching
  every reader.
- The walk's `cull` inlined (~0.4 ms of the master's) costs ~430 bytes of
  HWRAM code: Installation has 160.
- Not worth it on the arithmetic: a deferred read of the trace's divides
  (a pending-check on each of ~94 sides a trace costs what hiding ~20
  divides saves); the model polygons' multiply stalls (the loads that would
  hide them are wasted on the half that face away); a think-time skip of
  the game's entity scan (~0.1 ms).
- An item at the two CPUs' meeting point can be drawn by both (documented):
  harmless unless translucency is on.
- `M_CheckBottom`'s four corner descents as one slab query would change
  edge cases; `SV_NewChaseDir`'s eight tries when blocked are the game's
  worst tick (26 ms) and are Quake's.
- The red one-pixel line in Outer Base's first corridor (a seam between
  two faces, there with one CPU too): not found.
- Not Quake's, left: G_UseTargets runs on every monster death (Quake's
  monster_death_use returns without a target); a triggered spawn ignores
  the ambush flag.
