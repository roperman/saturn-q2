#!/bin/bash
# Boot the game in Mednafen, run the benchmark (START + R), save the results table.
#   tools/bench.sh out.png
cd "$(dirname "$0")/.."
OUT=${1:-bench.png}
tools/emu.sh start game.cue >/dev/null
sleep ${BOOT:-45}
w=$(xdotool search --class mednafen | tail -1)
xdotool windowactivate --sync "$w"
xdotool keydown Return; sleep 0.1; xdotool keydown e; sleep 0.12; xdotool keyup e; sleep 0.1; xdotool keyup Return
sleep 18
tools/emu.sh snap "$OUT" >/dev/null
python3 - "$OUT" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).crop((0, 96, 330, 200))
im.resize((660, 208)).save(sys.argv[1])
PY
echo "$OUT"
