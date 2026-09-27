#!/bin/bash
# Headless-ish test harness: boot a .cue in Mednafen, optionally press keys, grab game-frame snapshots.
# usage: emu.sh start <cue> | key <keysym> [holdms] | snap <outfile> | stop
SNAPDIR="$HOME/.mednafen/snaps"
win() { xdotool search --class mednafen 2>/dev/null | tail -1; }
case "$1" in
  start)   # silent (a dummy audio driver), without saving "sound 0" into your mednafen.cfg
    pkill -x mednafen; sleep 0.5
    SDL_AUDIODRIVER=dummy mednafen ${RECORD:+-soundrecord "$RECORD"} -video.fs 0 -sound 1 -ss.xscale 2 -ss.yscale 2 -ss.region_autodetect 0 -ss.region_default "${REGION:-eu}" -ss.cart extram4 "$2" >${TMPDIR:-/tmp}/mednafen.log 2>&1 &
    for i in $(seq 1 50); do [ -n "$(win)" ] && break; sleep 0.2; done
    echo "window: $(win)";;
  key)
    w=$(win); xdotool windowactivate --sync "$w" 2>/dev/null; xdotool keydown --window "$w" "$2"; sleep "$(echo "${3:-120}/1000" | bc -l)"; xdotool keyup --window "$w" "$2";;
  snap)
    w=$(win); xdotool windowactivate --sync "$w" 2>/dev/null
    before=$(ls -t "$SNAPDIR"/*.png 2>/dev/null | head -1)
    xdotool key F9; sleep 0.6
    after=$(ls -t "$SNAPDIR"/*.png | head -1)
    [ "$after" != "$before" ] && cp "$after" "$2" && echo "saved $2" || echo "snap failed";;
  stop) pkill -x mednafen;;
esac
