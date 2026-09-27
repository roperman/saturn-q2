#!/bin/bash
# The fight benchmark (a build with OPT=-DFIGHT_BENCH): boot, stats off,
# START + R, 20 seconds of the fight in the round room, save the results.
#   tools/fight.sh out.png
cd "$(dirname "$0")/.."
OUT=${1:-fight.png}
tools/emu.sh start game.cue >/dev/null
sleep ${BOOT:-45}
w=$(xdotool search --class mednafen | tail -1)
xdotool windowactivate --sync "$w"
tools/tap.sh Return                                     # the stats off
xdotool keydown Return; sleep 0.1; xdotool keydown e; sleep 0.12; xdotool keyup e; sleep 0.1; xdotool keyup Return
sleep 32
tools/emu.sh snap "$OUT" >/dev/null
tools/emu.sh stop
python3 - "$OUT" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).crop((0, 100, 330, 240))
im.resize((660, 280)).save(sys.argv[1])
PY
echo "$OUT"
