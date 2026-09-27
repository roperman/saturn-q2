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

Per view the frames are now 60, 80, 20, 60, 60 and 80 ms (PAL: 50 Hz
steps); views 1 and 5 need 44 and 42 ms of CPU, close to 40 (25 fps).

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

Next (PLAN.md): the cells loop in assembly, the level data out of LWRAM, and
the DSP running alongside.
