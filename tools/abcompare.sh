#!/bin/bash
# Pixel-compare the benchmark's six views between two builds: the working
# tree against a git stash of the given files (the change taken out).
#   tools/abcompare.sh outdir file...
# Builds with OPT=-DBENCH_HOLD $CMP_EXTRA twice; rebuild normally afterwards.
cd "$(dirname "$0")/.."
OUT=$1; shift
mkdir -p "$OUT"
snaps() {
    OPT="-DBENCH_HOLD $CMP_EXTRA" ./build.sh >/dev/null || exit 1
    tools/emu.sh start game.cue >/dev/null
    sleep 40
    w=$(xdotool search --class mednafen | tail -1)
    xdotool windowactivate --sync "$w"
    tools/tap.sh Return
    xdotool keydown Return; sleep 0.1; xdotool keydown e; sleep 0.12; xdotool keyup e; sleep 0.1; xdotool keyup Return
    sleep 3
    for v in 1 2 3 4 5 6; do
        SNAPS=$OUT tools/tap.sh wait:2 snap:v${v}_$1 s
    done
    tools/emu.sh stop
}
snaps b
git stash push -q -- "$@" || exit 1
snaps a
git stash pop -q
python3 - "$OUT" <<'PY'
import sys
from PIL import Image, ImageChops
d = sys.argv[1]
for v in range(1, 7):
    a = Image.open(f"{d}/v{v}_a.png").convert("RGB")
    b = Image.open(f"{d}/v{v}_b.png").convert("RGB")
    diff = ImageChops.difference(a, b)
    n = sum(1 for p in diff.getdata() if p != (0, 0, 0))
    print(f"view {v}: {n} pixels differ")
PY
