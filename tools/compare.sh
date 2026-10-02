#!/bin/bash
# Pixel-compare the benchmark's views with a renderer switch on and off (the
# BENCH_HOLD build: DOWN steps to the next view, UP flips r_cells_asm; with
# COMPARE=models draw_model's old loops, with COMPARE=prefetch the faces'
# prefetch, with COMPARE=far the monsters' coarse mesh from 200 units
# (CMP_EXTRA=-DFAR_B=n: from n)).
#   [COMPARE=models|prefetch|far] [CMP_EXTRA=-D...] tools/compare.sh [outdir]
# Builds with OPT=-DBENCH_HOLD (rebuild normally afterwards), prints each
# view's differing pixels, and saves a/b pairs plus diffN.png (differences
# in magenta, doubled) in outdir.
cd "$(dirname "$0")/.."
OUT=${1:-${TMPDIR:-/tmp}/compare}
mkdir -p "$OUT"
EXTRA="$CMP_EXTRA"
[ -n "$COMPARE" ] && EXTRA="-DCOMPARE_$(echo "$COMPARE" | tr a-z A-Z) $CMP_EXTRA"
OPT="-DBENCH_HOLD $EXTRA" ./build.sh >/dev/null || exit 1
tools/emu.sh start game.cue >/dev/null
sleep 40
w=$(xdotool search --class mednafen | tail -1)
xdotool windowactivate --sync "$w"
tools/tap.sh Return                                     # the stats off
xdotool keydown Return; sleep 0.1; xdotool keydown e; sleep 0.12; xdotool keyup e; sleep 0.1; xdotool keyup Return
sleep 3
for v in 1 2 3 4 5 6; do
    SNAPS=$OUT tools/tap.sh wait:2 snap:v${v}_a w wait:2 snap:v${v}_b w s
done
tools/emu.sh stop
python3 - "$OUT" <<'PY'
import sys
from PIL import Image, ImageChops
d = sys.argv[1]
for v in range(1, 7):
    a = Image.open(f"{d}/v{v}_a.png").convert("RGB")
    b = Image.open(f"{d}/v{v}_b.png").convert("RGB")
    diff = ImageChops.difference(a, b)
    n = sum(1 for p in diff.getdata() if p != (0, 0, 0))
    worst = max((max(p) for p in diff.getdata()), default=0)
    out = a.copy()
    px, dp = out.load(), diff.load()
    for y in range(a.size[1]):
        for x in range(a.size[0]):
            if dp[x, y] != (0, 0, 0):
                px[x, y] = (255, 0, 255)
    out.resize((a.size[0] * 2, a.size[1] * 2), Image.NEAREST).save(f"{d}/diff{v}.png")
    print(f"view {v}: {n} pixels differ (largest channel difference {worst})")
PY
