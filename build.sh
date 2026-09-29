#!/bin/bash
# Build the Quake 2 walkthrough on the bare-metal engine.
# Needs the Quake 2 data: data/pak0.pak (the shareware demo's will do).
set -e -o pipefail
cd "$(dirname "$0")"
. engine/build.inc.sh
mkdir -p obj/gen cd
MAP=${MAP:-demo1}
MAPFILE=$(echo "$MAP" | tr a-z A-Z).MAP
OUT=cd/$MAPFILE
# faces this many cells or more get a coarse grid too: demo3's the biggest,
# and with them all its data and the models are more than the cart's 4 MB
case $MAP in demo3) LODMIN=${LODMIN:-24} ;; esac
if [ ! -f "$OUT" ] || [ -n "$(find tools -newer "$OUT" -name '*.py')" ]; then
    python3 tools/bake_map.py data/pak0.pak "maps/$MAP.bsp" "$OUT" --res=${RES:-2} --lodmin=${LODMIN:-12}
fi
# the status bar's pictures
[ -f cd/HUD.BIN ] && [ cd/HUD.BIN -nt tools/bake_hud.py ] || python3 tools/bake_hud.py data/pak0.pak cd/HUD.BIN
# the sound effects (and their ids for the C)
[ -f cd/SOUND.BIN ] && [ -f obj/gen/sound_ids.h ] && [ cd/SOUND.BIN -nt tools/bake_sound.py ] \
    || python3 tools/bake_sound.py data/pak0.pak cd/SOUND.BIN obj/gen/sound_ids.h
# the models (tools/models.txt): whichever are out of date
python3 tools/bake_models.py data/pak0.pak cd
# built small (engine/build.inc.sh): start-up, menus, saving, the CD, trigger targets, the gunner (one level's)
COLD="main.c menu.c bup.c cd.c g_target.c m_gunner.c ${COLD_MORE:-}" \
engine_build QUAKE2 "src/main.c src/math.c src/level.c src/render.c src/trace.c src/pmove.c src/movers.c src/fx.c src/model.c src/g_main.c src/g_ai.c src/m_soldier.c src/m_infantry.c src/m_gunner.c src/g_target.c src/g_items.c src/hud.c src/sound.c src/menu.c src/view.c src/cycles.c src/grid.s src/cells.s src/walk.s src/mdraw.s" \
    "-DVDP_MAX_CMDS=2802 -DVDP_WRITER_CMDS=1100 -DVDP_WRITER1_CMDS=1300 -DVDP_GOURAUD_MAX=2800 -DMAP_FILE=\"$MAPFILE\" ${OPT}"
