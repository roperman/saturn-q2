#!/bin/bash
# Run in Mednafen with the 4 MB RAM cart (needs a Saturn BIOS in ~/.mednafen/firmware/).
cd "$(dirname "$0")"
[ -f game.cue ] || ./build.sh
exec mednafen -video.fs 0 -ss.cart extram4 -ss.region_autodetect 0 -ss.region_default ${REGION:-eu} game.cue
