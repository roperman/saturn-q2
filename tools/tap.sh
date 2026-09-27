#!/bin/bash
# tap.sh key [key...]: short taps with a settle delay; "snap:NAME" saves a frame; "wait:SEC" sleeps
# Keys go through XTEST to the focused Mednafen window (SDL can ignore XSendEvent).
S=${SNAPS:-/tmp/claude-1000/-home-roper-claude-saturn-q2/65dc405c-a9ab-4e48-a5d3-36b914e8af27/scratchpad}
w=$(xdotool search --class mednafen | tail -1)
xdotool windowactivate --sync "$w" 2>/dev/null
for k in "$@"; do
  case "$k" in
    snap:*) "$(dirname "$0")/emu.sh" snap "$S/${k#snap:}.png" >/dev/null;;
    wait:*) sleep "${k#wait:}";;
    *) xdotool keydown "$k"; sleep 0.1; xdotool keyup "$k"; sleep 0.55;;
  esac
done
