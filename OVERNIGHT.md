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
