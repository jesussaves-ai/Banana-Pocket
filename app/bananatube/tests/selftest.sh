#!/bin/sh
# banana-tube's host selftest: build the client and run its logic pass. There is no display in the harness, so SDL
# uses its dummy driver and the run stops before the event loop (BANANA_TUBE_SELFTEST): this covers the search-line
# parse, the keyboard navigation and the back()/activate() state machine. Playback needs mpv, yt-dlp and a network
# and is not exercised here.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
if ! pkg-config --exists sdl2 SDL2_ttf 2>/dev/null; then
    echo "banana-tube selftest: SDL2/SDL2_ttf not found, skipped"
    exit 0
fi
make -s -C "$HERE" banana-tube
out=$(SDL_VIDEODRIVER=dummy BANANA_TUBE_SELFTEST=1 "$HERE/banana-tube" \
    --font "$HERE/../../ui/assets/banana/fonts/Inter-Regular.ttf")
echo "$out"
echo "$out" | grep -q 'selftest split: ok'
echo "$out" | grep -q 'selftest typed: catsA'
echo "$out" | grep -q 'selftest touch: 25,2 (expect 25,2)'
echo "banana-tube selftest: ok"
