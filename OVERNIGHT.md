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
it, counted in the above. Off by default, your call; OPT=-DGAME_DURING_DRAW
builds it on.

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
