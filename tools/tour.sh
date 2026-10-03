#!/bin/bash
# The level tour: a build with OPT=-DLEVEL_TEST (each level's exit 80 frames in) booted, a new
# game started from the title (START twice: NEW GAME, then MEDIUM), START at each LEVEL COMPLETE,
# a snapshot before each; the last is the title with every level's line (what the walls' DSP
# programs, the gun's slots and each RAM had left). Keys as tools/fight.sh assumes (START = Return).
#   tools/tour.sh outdir
cd "$(dirname "$0")/.."
OUT=${1:-tour}
mkdir -p "$OUT"
OPT=-DLEVEL_TEST ./build.sh >/dev/null || exit 1
tools/emu.sh start game.cue >/dev/null
sleep ${BOOT:-50}
w=$(xdotool search --class mednafen | tail -1)
xdotool windowactivate --sync "$w"
tools/tap.sh Return; sleep 1; tools/tap.sh Return
sleep 10; tools/emu.sh snap "$OUT/level1.png" >/dev/null; tools/tap.sh Return
sleep 55; tools/emu.sh snap "$OUT/level2.png" >/dev/null; tools/tap.sh Return
sleep 65; tools/emu.sh snap "$OUT/level3.png" >/dev/null; tools/tap.sh Return
sleep 60; tools/emu.sh snap "$OUT/level4.png" >/dev/null; tools/tap.sh Return
sleep 45; tools/emu.sh snap "$OUT/title.png" >/dev/null
tools/emu.sh stop
./build.sh >/dev/null
echo "$OUT/title.png"
