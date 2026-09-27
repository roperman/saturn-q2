#!/bin/bash
# Build the Quake 2 walkthrough on the bare-metal engine.
# Needs the Quake 2 data: data/pak0.pak (the shareware demo's will do).
set -e -o pipefail
cd "$(dirname "$0")"
. engine/build.inc.sh
mkdir -p obj/gen cd
MAP=${MAP:-demo1}
OUT=cd/$(echo "$MAP" | tr a-z A-Z).MAP
if [ ! -f "$OUT" ] || [ -n "$(find tools -newer "$OUT" -name '*.py')" ]; then
    python3 tools/bake_map.py data/pak0.pak "maps/$MAP.bsp" "$OUT" --res=${RES:-2}
fi
# the status bar's pictures
[ -f cd/HUD.BIN ] && [ cd/HUD.BIN -nt tools/bake_hud.py ] || python3 tools/bake_hud.py data/pak0.pak cd/HUD.BIN
# the models (tools/models.txt): whichever are out of date
python3 tools/bake_models.py data/pak0.pak cd
engine_build QUAKE2 "src/main.c src/math.c src/level.c src/render.c src/trace.c src/pmove.c src/movers.c src/fx.c src/model.c src/g_main.c src/g_ai.c src/m_soldier.c src/m_infantry.c src/g_target.c src/g_items.c src/hud.c src/cycles.c src/grid.s" \
    "-DVDP_MAX_CMDS=3000 -DVDP_WRITER_CMDS=1400 -DVDP_GOURAUD_MAX=2800 ${OPT}"
