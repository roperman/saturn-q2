#!/bin/bash
# Build Quake 2 for the Saturn (README.md): bakes what it needs from
# data/pak0.pak (the shareware demo's will do) into cd/, then game.cue.
#   [MAP=demo2] [OPT=-D...] ./build.sh      (MAP: the level it starts on)
set -e -o pipefail
cd "$(dirname "$0")"
if [ ! -x vendor/joengine/Compiler/LINUX/bin/sh-none-elf-gcc-8.2.0 ]; then
    echo "No SH-2 compiler: clone Jo Engine into vendor/joengine (see README.md)" >&2
    exit 1
fi
if [ ! -f data/pak0.pak ]; then
    echo "No Quake 2 data: put pak0.pak in data/ (see README.md)" >&2
    exit 1
fi
. engine/build.inc.sh
mkdir -p obj/gen cd
MAP=${MAP:-demo1}
MAPFILE=$(echo "$MAP" | tr a-z A-Z).MAP
# the demo's three levels (the first bake of each spends a minute or two working out
# which faces each part of the level can see: kept in obj/ after)
for m in demo1 demo2 demo3; do
    out=cd/$(echo "$m" | tr a-z A-Z).MAP
    # faces this many cells or more get a coarse grid too: demo3's the biggest,
    # and with them all its data and the models are more than the cart's 4 MB
    lodmin=${LODMIN:-12}
    [ "$m" = demo3 ] && lodmin=${LODMIN:-24}
    if [ ! -f "$out" ] || [ -n "$(find tools -newer "$out" -name '*.py')" ]; then
        python3 tools/bake_map.py data/pak0.pak "maps/$m.bsp" "$out" --res=${RES:-2} --lodmin=$lodmin
    fi
done
# the status bar's pictures
[ -f cd/HUD.BIN ] && [ cd/HUD.BIN -nt tools/bake_hud.py ] || python3 tools/bake_hud.py data/pak0.pak cd/HUD.BIN
# the sound effects, a bank a level (and their ids for the C)
rm -f cd/SOUND.BIN
[ -f cd/DEMO3.SND ] && [ -f obj/gen/sound_ids.h ] && [ cd/DEMO3.SND -nt tools/bake_sound.py ] \
    || python3 tools/bake_sound.py data/pak0.pak cd obj/gen/sound_ids.h
# the models (tools/models.txt): whichever are out of date
python3 tools/bake_models.py data/pak0.pak cd
# built small (engine/build.inc.sh): start-up, menus, saving, the CD, trigger targets, the gunner (one level's),
# the level loading, the boot-time timings, the HUD, the sounds, the items
COLD="main.c menu.c bup.c cd.c g_target.c m_gunner.c level.c cycles.c hud.c sound.c g_items.c ${COLD_MORE:-}" \
engine_build QUAKE2 "src/main.c src/math.c src/level.c src/render.c src/trace.c src/pmove.c src/movers.c src/fx.c src/model.c src/g_main.c src/g_ai.c src/m_soldier.c src/m_infantry.c src/m_gunner.c src/g_target.c src/g_items.c src/hud.c src/sound.c src/menu.c src/view.c src/cycles.c src/grid.s src/cells.s src/face.s src/dlight.s src/walk.s src/mdraw.s src/tline.s" \
    "-DVDP_MAX_CMDS=2802 -DVDP_WRITER_CMDS=1100 -DVDP_WRITER1_CMDS=1300 -DVDP_GOURAUD_MAX=2800 -DMAP_FILE=\"$MAPFILE\" ${OPT}"
