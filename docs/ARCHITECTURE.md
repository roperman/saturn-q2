# How it works

A tour of the port, part by part: what runs where, what the data looks like,
and what happens in a frame. Two pages beside this one draw it:
[timeline.html](timeline.html) (a frame of the fight across every processor,
to scale) and [flowchart.html](flowchart.html) (the frame's and the game
tick's branches, down to a monster's step); open them in a browser. [README.md](../README.md) is the short version;
[OVERNIGHT.md](../OVERNIGHT.md) has the history of every piece, with the
numbers, and is referred to below by section.

## 1. The machine, and what each part does here

| part | used for |
|---|---|
| master SH-2 (28 MHz) | the frame: input, the game's tick, the BSP walk, the back half of the drawing, the uploads, the lists' submission |
| slave SH-2 | the front half of the drawing (from the walk's list as it fills), the models' setup and lighting ahead of the walk, the next frame's player move |
| SCU DSP | the models' vertices (both frames blended and transformed), the walls' dynamic lights, the textures made as they're uploaded |
| VDP1 | every polygon: the world's cells, the models, the gun, sprites and beams, the status bar, text |
| VDP2 | the sky (a scrolling layer) and the gradient above and below it, the console picture behind the loading screens, the colour offset for tints and flashes |
| SCSP + 68000 | all sound: the 68000 runs a sequencer that plays a bank out of sound RAM; the SH-2s only post orders |
| the 4 MB RAM cart | the level, the models, the monsters' code, the gun's slots, the DSP's buffers: everything big |
| high work RAM (1 MB, fast) | the program, the stacks, and the level's hottest data: the BSP, the entities' render records |
| low work RAM (1 MB, slow) | the seldom-run code, and the level's faces, cells, lights, brushes, the entities |

The SH-2s have 4 KB caches each, write-through with no write buffer: a
store costs a bus cycle, and a miss costs 10 cycles a line from high work
RAM, 59 from low, 75 from the cart ([PLAN.md](../PLAN.md) has the table). A
lot of the design is about which memory a thing lives in and how often it's
touched.

## 2. The build

`build.sh` (and `engine/build.inc.sh` for the compiler and disc image)
bakes everything the game reads from `data/pak0.pak` into `cd/`, then
compiles and links `game.elf`, and makes `game.iso`/`game.cue` with Jo
Engine's boot sector. The bakes, each cached by timestamp:

- **Levels** (`tools/bake_map.py` → `DEMO1.MAP` etc., with `DEMO1.SKY` from
  `tools/bake_sky.py`): the BSP, the faces cut into cells, the textures as
  tiles with masks, the lightmap as Gouraud values at grid points, the
  entities, and the per-cluster visible-face lists from `tools/facevis.c`
  (a C program that renders the level from sample points, slow and cached
  in `obj/`). Section 3 of this file has the layout.
- **Models** (`tools/bake_models.py` over `tools/models.txt`, each by
  `tools/bake_md2.py` → `*.MDL`): Quake 2's MD2s as quads with per-polygon
  textures, the animations named, a coarse mesh for the distance, the
  normals' shading tables.
- **Sounds** (`tools/bake_sound.py` → `DEMO1.SND` etc., and
  `obj/gen/sound_ids.h`): one bank a level, 8-bit at 11 kHz, every effect
  the level can need, cut to a length each; a level without a monster gets
  silence in that monster's slots, so the ids are the same everywhere.
- **The status bar and menus** (`tools/bake_hud.py` → `HUD.BIN`), **the
  console picture** (`tools/bake_conback.py`).
- **The DSP programs** (`tools/dspasm.py`, a small assembler for the SCU
  DSP's instruction set): `engine/*.dsp` → C headers (kept in low work RAM:
  they're read only as they're loaded), and `cd/WALLS.BIN` for the walls'
  three, copied into VDP1's VRAM when the walls' job is on.
- **The monsters' code** (`build_overlays` in `build.sh`): each monster only
  some levels have (`src/m_gunner.c`, `m_berserk.c`, `m_tank.c`, `m_flyer.c`,
  `m_parasite.c`) is compiled on its own, linked twice against `game.elf` at
  two bases, and `tools/overlay.py` turns the two images into `*.OVL`: the
  image plus the list of words that need moving by wherever it lands. The
  game reads a level's onto the cart and fixes those words up
  (`g_overlays_load`).

Generated headers go in `obj/gen/`: class ids (`q2classes.h`), model ids
(`q2models.h`), sound ids, the DSP programs, a sine table.

## 3. Memory and the level file

### The cart

`src/level.c` reads the level file off the CD onto the cart at 0x02400000
and uses it in place; everything after it is bump-allocated on the cart in
this order: the sky and sound banks aren't here (VDP2 VRAM and sound RAM),
but the models are (`models_load_all`: the ones the level's entities need,
the optional ones only if there's room after them for the gun's two slots),
the monsters' code overlays, the gun's slots (`src/view.c`: the weapon you
hold and the next one, read from the CD in the background), the faces the
CPUs note for the walls' DSP job (when it's on), the ring of textures the
CPUs make, the entities if low work RAM is full. `OPT=-DLEVEL_TEST` prints what each level leaves.

### The level file's lumps

`tools/bake_map.py` writes, big-endian, in `src/level.c`'s order:

| lump | what |
|---|---|
| planes, nodes, leafs, marks | Quake's BSP (the nodes with a face range each: the faces on that node's plane) |
| faces | 32 bytes each: origin, two axes (an index into the axes lump), plane, flags, grid size (nu x nv cells), edge crops, first light |
| cells | one per grid cell: its texture, colour table, or a deferred form for the odd ones |
| lights | the lightmap at the grid points, as 15-bit Gouraud colours (Quake's light, gamma-lifted for a TV) |
| textures, texdata, tileofs, masks | the texture records; the unique 16 x 16 tiles' texels (4 bpp); each tile's offset; the crop masks (16 rows of 16 bits each) |
| luts | the colour tables (16 entries each) |
| vis, facevis | Quake's PVS (by cluster), and the faces really visible from each cluster |
| models, brushes, brushsides, leafbrushes | the brush models (doors, lifts, exploding walls) and the collision brushes |
| movers, spawns, entities2, strings | the movers' records, the player starts, the entities (class, origin, angles, spawnflags, targets, keys) |
| leaflight | the light at each leaf's middle, for the models |
| axes | the faces' texture axes, shared |
| lodfaces, lodcells, lodlights | the coarse grids of the big faces (`LOD_Z`) |
| portals, cportals | the areaportal flow (doors that block visibility; `PORTALS=1`) |
| starts | the level's starts by name (the exits between levels) |

### What's copied into work RAM

What the renderer reads every frame is copied out of the cart as the level
loads (`HOT_D` in `src/level.c`): the nodes, leafs and marks into high work
RAM (the walk touches them most), the faces, cells, lights, brushes and
brush sides into low. The textures, masks, visibility rows and entity
records stay on the cart. High work RAM is nearly full on every level (tens
to a few hundred bytes left), so code that isn't hot is kept out of it:
`engine/link.ld` puts whole objects (`level.c`, `menu.c`, `cd.c`,
`g_target.c`, `cycles.c`) and every function marked `cold` into `.lwtext`
in low work RAM.

## 4. A frame

The master runs `src/main.c`'s loop; the slave runs `slave_main` and waits
to be signalled. In order, for one frame:

1. **The pad**, or the slave's pre-move: in a normal frame the slave, when
   its drawing is done, reads the pad and runs the player's move for the
   next frame (`pmove`, with the doors and lifts, `PM_REQ`), so the master
   starts the next frame with it done.
2. **The game's tick** (`game_step` → `g_tick`, section 8): at 10 Hz by
   Quake's clock, so most frames have none and some one. With "game during
   drawing" on (the default) the master runs it while the slave is drawing,
   after handing the slave the front of the list.
3. **The view**: the camera from the player (`cam_update`), the gun
   (`view_update`), the sky's scroll (`render_sky`), the effects' lights and
   sprites for this frame (`fx_update`), the colour offset (under water, hit
   and pickup flashes).
4. **The models for the DSP** (`models_to_dsp`): every model that might be
   on screen gets a 25-word header (its two frames' matrices, already
   weighted by the blend) in a stream in work RAM; the DSP is started on it
   (`dsp_models`) and transforms vertices block by block while everything
   else goes on, bumping a count the CPUs poll when they need a model. The
   walls' job and the texture maker are chained after it (section 7).
5. **The slave's first job** (`PRE_JOB`): which entities are in the PVS and
   where, their leaf light and shading tables, the dynamic lights on the
   models — ahead of the walk, so the master isn't waiting on them.
6. **The walk** (`walk_asm`, `src/walk.s`): the master marks the camera
   cluster's PVS, walks the BSP front to back, culls each node's box
   against the view's side planes, and lists the faces facing the camera
   that the cluster's facevis says are visible, publishing the list's
   length as it goes. Leaves with entities, sprites or brush models in them
   hand those to C (`walk_leaf_extra`) so they join the order at their depth.
7. **Drawing**: the slave draws from the front of the list as it fills,
   the master from the back once the walk is done; they meet in the list
   (at worst the one face where they meet is drawn by both). Each writes
   VDP1 commands into its own region of the list
   and its own bucket, which VDP1 draws last-in first: the result is back to
   front with no sort. The gun is drawn by the master over everything.
8. **The textures' uploads**: each CPU queued what it needed in VRAM
   (`tex_upload`); when both are done the master tells the DSP's texture
   maker the frame's over and makes anything it hadn't (`mk_finish`), then
   the queued ones go with the list's DMA (`upload_one`).
9. **Submit** (`vdp_submit`): the list and its Gouraud tables are DMA'd into
   VDP1 VRAM and VDP1 is started on them; the swap to the new picture
   happens by interrupt at the next vblank (pipelined: submit returns at
   once, so the CPUs start the next frame while VDP1 draws this one). A
   frame may take one vblank (NTSC 30 fps, PAL 25) or two if VDP1 or the
   CPUs are late.

The fight benchmark's lines (`OPT=-DFIGHT_BENCH`) time each of these; the
stats overlay (START) shows the frame's totals.

## 5. The renderer

### Cells

VDP1 draws a distorted sprite: four screen corners and a whole texture
mapped across them, no perspective correction, no depth buffer. So each face
is a grid of cells, 32 texels of texture space square, aligned to the
texture's repeat, and each cell is one such sprite of a 16 x 16 tile (the
textures are half-resolution: `RES=2`). The grid is projected once per face
(`grid.s`: the face's origin and axes into view space, then the points by
additions, with the hardware divider running alongside), outcodes mark which
points are off which side, and the cells are emitted in rows (`cells.s`)
with their Gouraud tables from the lights lump. A cell crossing the near
plane is cut in C (`cell_split`); a cell the face only partly covers is a
crop (section 5.3). Faces wholly beyond `LOD_Z` use their coarse grid and
half-resolution tiles.

### Visibility and order

Quake's PVS is conservative: in a big room most of what it lets through is
behind nearer walls, and with painter's order the Saturn pays for all of
it. `tools/facevis.c` fixes that offline: it renders the level from sample
points in every cluster (64 and each leaf's corners) into face-id buffers
with a depth test, and keeps the faces that show. The walk uses that list.
OVERNIGHT.md sections 11-15 and 30 have the numbers.

### Textures: tiles, crops and the cache

The level holds each unique tile once (90-135 KB a level). Where a cell
needs more than a whole tile:

- whole rows or columns of it (the face cut off along the texture's rows
  or columns) are drawn by starting the sprite part way into the tile, or
  into a transposed copy of it;
- anything else is a **variant**: the tile cropped to what the face covers,
  the uncovered texels transparent. These are made at run time from the
  tile and a **mask** (32 bytes: 16 rows of 16 bits, shared where alike),
  most by the DSP (section 7.3), the rest by the CPU (`tex_make`). The
  transposed and quartered (near-plane) tiles are made the same way.

VDP1 VRAM has about 2,800 slots of a tile each (`MAX_SLOTS`), half for each
CPU (`tex_load`). A slot is reused only when VDP1 has finished every frame
that used it: the list being built, the one in flight, and the one before,
since two can be in flight. If none are that old, one two frames back is
taken and its upload goes late, once VDP1 is done with that frame
(`r_late_uploads`). Uploads are SCU DMA from the cart or the CPUs' ring of
made textures, not CPU stores (111 cycles a word into VRAM); the DSP's
maker writes its textures into their slots itself.

### Models

`tools/bake_md2.py` pairs triangles that share an edge in the mesh and the
skin into quads; each polygon gets its piece of the skin pre-warped into a
texture of its own (at most 16 x 16, so it fits a cache slot), 4 bpp with a
colour table. Frames keep MD2's packing (a byte a coordinate, a normal
index). The DSP transforms a model's two frames at once, each matrix
scaled by its share of the blend (`engine/xformm.dsp`), 16 vertices a
block, into a buffer the CPU reads uncached. `draw_model` (and `mdraw.s`)
then shades each vertex by its normal (Quake's 16-step yaw tables) and the
leaf's light plus any dynamic lights in reach, sorts the polygons into
depth buckets and emits them. Beyond `MODEL_FAR` the coarse mesh (a third
of the polygons) is used. Monsters that aren't on screen aren't sent to the
DSP (a sphere test against the view's sides).

### The gun

`src/view.c`: Quake's `v_*.md2` weapon models, animated as Quake does
(raise, fire, idle, lower, 10 Hz blended), with its bob. All seven don't
fit a level's cart, so there are two slots: the weapon held and the next,
read from the CD a few sectors a frame while the held one is put away.

### Sprites, beams and effects

`src/fx.c` keeps the projectiles (bolts, rockets, grenades, gibs: moved
each frame with Quake's physics, rockets and grenades drawn as models),
flashes (muzzle, impact, explosion: a light and a sprite that fade), sparks,
and beams (the parasite's tongue). Each frame it turns them into the
renderer's dynamic lights (`r_dlights`, at most 8) and sprites
(`r_sprites`): a sprite is a glowing blob drawn as two half-transparent
quads of a round white texture, tinted by Gouraud (`draw_sprite`); a beam
is the same texture stretched along a bar between two points, cut at the
near plane since it ends at you (`draw_beam`). Sprites are listed into their
leaves so the walk draws them at their depth.

### Dynamic lights

A dynamic light adds to the Gouraud colours of the grid points and model
vertices it reaches, with Quake's falloff. On the models the slave does it
ahead of the walk. On the walls most of it is the DSP's (section 7.2): a
frame behind, as the models are. Faces the DSP didn't do (too big for its
RAM, or more than it had time for) are lit by the CPU as they're drawn
(`dl_face`, `dlight.s`).

### The sky

VDP2's NBG0 shows the skybox as seen on a cylinder round you (all six
faces, 63 degrees up to 19 down), baked to 16-colour cells with the best
of 48 palettes each, read from the disc straight into VDP2 VRAM. The layer
scrolls with your yaw and pitch; above and below it VDP2 fills each line
with a colour fading to the zenith's. Sky faces in the level are drawn as
nothing (VDP1 leaves the sky showing through).

### Water and glass

Translucent faces (`TRANS_MODE`) are drawn with VDP1's mesh (every other
pixel) by default, or real half-transparency (slower on VDP1). Water's grid
points move with a wave (`wave_at`) and a ripple of light; under water
there's a tint through VDP2's colour offset.

## 6. The two CPUs

The master owns the frame; the slave is signalled with jobs through a few
uncached words. The cache is the thing to mind: each CPU has its own, so
anything one writes for the other is read uncached (`UNCACHED(p)`) or
after a purge, and the slave purges its whole cache at the start of each
job (`cache_purge`), since the master changed the list, the camera and the
frame's counters. Data the slave writes that the master reads (its stats,
its upload queue, its texture list) is read through `UNCACHED(&ctx[1])`.

The split of the drawing isn't fixed: the slave starts on the list's front
as soon as the walk publishes the first faces, the master starts on the
back when the walk's done, and they meet wherever that falls. A change in
speed moves the meeting point, so a few pixels on seams can change hands
between builds; `OPT=-DONE_CPU` fixes the order for comparing pictures.

## 7. The SCU DSP

The DSP is a small VLIW processor: one instruction a cycle at 14 MHz, four
banks of 64 words of data RAM, 256 words of program RAM, a 32-bit ALU and
multiplier, no divider, and its own DMA that reaches the cart, high work
RAM and the video and sound chips, but not low work RAM. The host loads a
program and its parameters through ports, starts it, and polls a busy bit;
a program can also DMA another program over itself. `tools/dspasm.py`
assembles `engine/*.dsp` (its header documents the syntax), and
`tools/dspsim.py` is a Python copy of Mednafen's DSP for testing programs
off the Saturn (`tools/walls_sim.py`, `tools/make_sim.py`).

Every frame runs a chain of programs, each loading the next by the DSP's
own DMA, from copies in high work RAM (the models', the maker's) or VDP1's
VRAM (the walls'):

1. **The models** (`xformm.dsp`): started by `models_to_dsp` with the
   models' stream; counts up a word in work RAM as each is done.
2. **The walls' lights** (`walls0.dsp`, `walls1.dsp`, `walls2.dsp`; off
   unless `OPT=-DDSP_WALLS`), when there are dynamic lights: walls0 picks,
   from the faces the CPUs drew last frame (each CPU notes its whole faces
   as it draws them, `dw_note`), those any light reaches; walls1 lays each
   one's grid out; walls2 sums the lights at every grid point and writes
   the lit Gouraud values the CPUs use the next frame (`dw_find`). The
   parameters (`dsp_walls_p`, RAM0 words 40-63) are the level's constants
   and this frame's lists. walls1's blocks for walls2 are in VDP1's VRAM,
   the lit values and picks in high work RAM.
3. **The texture maker** (`make.dsp`, section 7.3), for the rest of the
   frame.
4. Back to the models' program, which stops.

Each program keeps its working set in the four data banks and is written
around the DSP's quirks: a JMP's delay slot always runs, an ALU result
exists only in the instruction that made it, a bank read by an instruction
can't be written by it, a bank being filled by DMA mustn't be touched till
it's done, and the right shift is arithmetic. A DMA writes the B-bus (VDP1's
VRAM) with add mode 1 (each word goes as two halves, each moving the address
by 2), high work RAM with mode 2. The host-side driver is `engine/dsp.c`.

**What a real Saturn won't do** (Sega's "SCU Final Specifications:
Precautions", and the RAM cart's bulletin), which Mednafen allows:

- The SCU's DMA, the DSP's included, mustn't write the cart: on the Saturn
  the bus locks. Nothing the DSP writes is on the cart.
- A DMA level's registers mustn't be written while it's active, and it's
  active till its move, standby and held-back flags are all clear
  (`scu_dma0`'s `D0_ACTIVE`).
- An indirect DMA's table must start on the power-of-two boundary its size
  rounds up to (`vdp.c`'s table: the slave stack's bottom KB).
- The A-bus timing register is written only after a read of the cart.

And on SAROO's cart a DMA read has been seen to arrive a word late, the
rest shifted. So what the DSP reads to know where to write, and every
program it loads, is in high work RAM or VRAM; the maker checks the tile
and mask it reads; and the gun's DMA copies from the cart are checked
before use (`dma_copy_ok`).

### 7.3 The texture maker

Each CPU, as `tex_load` finds a texture to make (not a late one), writes a
14-word job into the maker's block in high work RAM (the tile, the mask,
the texture's slot in VDP1's VRAM, the crop's rows, words and shifts, all
worked out by the host, and the tile's and mask's first word XOR their
last) and bumps its count; the maker polls the two counts, DMAs the tile
(32 words) and mask (8) in from the cart and checks them, makes the 32
words out (a row at a time: the mask's bits through one 16-entry nibble
table, the second lookup from the same entry's high half, ANDed with the
row's words, shifted into place when the crop starts inside a word) and
DMAs them straight into the slot. A read that fails its check gives the job
back and stops the maker for the frame. At the frame's end the master
raises an end flag and waits for the maker's state word (its current job
done), then makes whatever is left into the CPUs' ring and queues their
uploads. Sections 62 and 66 of OVERNIGHT.md have the design and the
measurements.

## 8. The game

`src/g_*.c` and `src/m_*.c` are Quake 2's game code in 16.16 fixed point
(angles as 16 bits a turn), cut to what the demo needs.

### Entities

`g_init` lays the level's entities out in pools: the **full** ones (the
player, monsters, barrels, explosive walls: all of `g_ent`) and the
**short** ones (items, triggers, targets: only the first part of the
struct), each with the records the frame reads first in its first cache
line. Items keep a spot (leaf and position) for the renderer; triggers keep
a box. Entities at a skill the game isn't at don't exist. Monsters that a
trigger spawns are there from the start but inactive (`inactive`, not
solid, not drawn) until used.

The renderer's view of an entity is a `q_entity` (`ents[]`): origin, yaw,
model, the two frames and the blend, the leaf. `g_render_ents` fills them
from the game's entities each frame, blending position and frame between
the last tick's values and this one's by how far the frame is through the
tick.

### The tick

`g_tick` runs Quake's frame at 10 Hz: the thinkers (entities with a
`nextthink`, kept in a list so a tick doesn't scan everything), the
monsters in 8 groups spread across the tick's ten frames (so the cost is
even), the player's weapon and the damage flash, the movers. Monsters
standing out of sight run at 5 Hz. Targets fire by chains built at the
level's start (`g_tn_first`). Section 59 of OVERNIGHT.md profiles it.

### Monsters

Each monster is Quake 2's `m_*.c`: tables of moves (a named animation, a
frame range, what the AI does on each frame, what happens after), and the
callbacks (`stand`, `run`, `attack`, `pain`, `die`, `sight`). The frame
ranges are resolved against the baked model's animation names when it
spawns. `src/g_ai.c` is Quake 2's `g_ai.c` and `m_move.c`: standing,
chasing (`SV_NewChaseDir`'s turns), the step movement with its ground
test, `M_CheckAttack`'s chances, finding and losing the player. Flying
monsters (`FL_FLY`: the flyer) take a different step: no stairs or ground,
a lean up or down towards the goal, never falling. A monster's death plays
its death animation and leaves a body that further hits gib (`g_gib`), or
for the flyer an explosion.

The soldiers and infantry are in `game.elf`; the gunner, berserker, tank,
flyer and parasite are overlays (section 2): a level loads the code of the
monsters it has next to their models, and `g_overlays_load` finds each
one's spawn function at the image's start. A monster whose model doesn't
fit the cart is left out of the level.

### Traces

`src/trace.c` is Quake 2's `CM_BoxTrace`: the BSP finds the leaves a box's
sweep touches, each leaf's brushes are clipped against the box (`tbox.s`),
and point traces go down the tree (`tline.s`). `g_trace` adds the entities:
the monsters' and the player's boxes, and bodies for shots. Traces are the
dear part of a fight's tick (OVERNIGHT.md sections 40, 41, 59).

### The player, movers, items, weapons

`src/pmove.c` is Quake 2's player movement (`qcommon/pmove.c`): the ground
check, friction, acceleration, gravity, jumping, the step-slide move,
swimming, ladders. `src/movers.c` has doors, lifts and buttons (brush
models that move between two positions); `src/g_items.c` the items, armour
and keys; `src/g_target.c` the triggers, relays, counters, secrets, goals,
level changes; `src/fx.c` the weapons' projectiles; the hitscan weapons
are in `g_main.c` with the damage, armour and gibs.

## 9. Sound

`tools/bake_sound.py` makes a bank a level: a header, an instrument table,
and the 8-bit 11 kHz samples, under 480 KB (sound RAM less the reverb's
delay lines). The 68000 driver (`engine/m68k/driver.c`) runs a sequencer out
of sound RAM, keeps time with the SCSP's timer, and takes orders through a
mailbox at the top of sound RAM: the SH-2s never touch the SCSP. `src/sound.c`
turns a world sound into a volume and pan by Quake's attenuations (normal:
silent at 1,000 units; idle at 500) and the view's position, and queues the
game's sounds to start at the moment each is of, so they land as evenly as
Quake's though the tick runs where the frames fall (`s_play_queued`).

## 10. Menus, status bar, loading

`src/menu.c` is the title (Quake's plaque and logo, from `HUD.BIN`), the
skill choice, options (brightness, translucency, the water, the gun's bob,
the game during drawing) and the pause menu; the world keeps drawing behind
them. `src/hud.c` draws Quake's own pictures as 256-colour VDP1 sprites
against Quake's palette in VDP2 colour RAM. Loading screens show the console
background on VDP2's NBG1 with the message over it.

## 11. Checking a change

The discipline, kept throughout: anything meant to be invisible is compared
pixel for pixel before and after (`tools/abcompare.sh`: the six benchmark
views, the change stashed and not), new assembly runs beside the C it
replaces with results compared (`-DFACE_CHECK` and friends), and the
numbers come from the two benchmarks (`tools/bench.sh`, `tools/fight.sh`,
NTSC and PAL) and the level tour (`tools/tour.sh`: all three levels load,
and what each leaves of every memory). The DSP programs are run in
`tools/dspsim.py` against Python copies of the C before they go near the
Saturn. The stats overlay (START) and `OPT=-DR_PROFILE`, `-DTICK_PROF`,
`-DSTATS` show where a frame goes. README.md's table lists the switches.

## 12. Conventions

- 16.16 fixed point everywhere (`FIX(x)`, `fmul`, `fdiv`); angles are 16
  bits a turn (`ANG(degrees)`); units are Quake's.
- Everything baked is big-endian; the SH-2 is big-endian.
- Addresses given to the DSP are `>> 2` (it addresses words), and masked
  to 0x07FFFFFF (the cached view of memory).
- `cold` marks a function for low work RAM; keep hot code out of there.
- Writes meant for the other CPU or the DSP go through `UNCACHED`, or rely
  on the cache writing through; reads of what they wrote must be uncached
  or after a purge.
- Never write to a fixed work RAM address: the level data lives there.
