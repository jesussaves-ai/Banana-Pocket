#!/bin/sh
# BananaTube, exercised on the host without the handheld. Three seams are checked by running the client's own logic:
#
#   1. the touch transform: the panel's raw touch axes (1080x1240, the FT5x06) turned 90 must land the four screen
#      corners in the app surface's own 1240x1080 space (the client's `touch` selftest lines cover this);
#   2. the search parsers: the InnerTube `videoRenderer` walk and the yt-dlp `--print` line split;
#   3. the launcher: `run-bananatube` must call the shared audio-handoff and give the client the second AudioReach
#      frontend (MultiMedia2, hw:0,1) with an AudioReach-sized ALSA default (S16, 48000, 2ch), on the display the
#      menu releases (cage) -- the same contract run-dolphin/run-retroarch have, checked the same way.
#
# The client selftest runs under SDL's dummy driver (no panel here): the run stops before the event loop.
#   system/tests/run-banana-tube-test.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)      # system/
ROOT=$(cd "$HERE/.." && pwd)                 # the repo
APP="$ROOT/app/bananatube"

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }

echo "1. the touch transform: the panel's corners, turned 90, in the app's 1240x1080 surface"
if ! pkg-config --exists sdl2 SDL2_ttf SDL2_image libcurl 2>/dev/null; then
    echo "  skip: SDL2/libcurl not found on this host"
else
    make -s -C "$APP" banana-tube >/dev/null 2>&1 || { bad "the client did not build"; }
    out=$(SDL_VIDEODRIVER=dummy BANANA_TUBE_SELFTEST=1 "$APP/banana-tube" \
        --font "$ROOT/ui/assets/banana/fonts/Inter-Regular.ttf" 2>/dev/null)
    for c in tl tr bl br spot; do
        line=$(printf '%s\n' "$out" | grep "touch $c " | head -n 1)
        case "$line" in
        *": ok") ok "touch $c: ${line##*selftest }" ;;
        "") bad "touch $c: no line (the client did not run)" ;;
        *) bad "touch $c: $line" ;;
        esac
    done
    # the parsers
    printf '%s\n' "$out" | grep -q 'selftest innertube: ok' && ok "InnerTube videoRenderer parse" || bad "InnerTube parse"
    printf '%s\n' "$out" | grep -q 'selftest split: ok' && ok "yt-dlp --print line split" || bad "yt-dlp line split"
    printf '%s\n' "$out" | grep -q 'selftest quit: 1' && ok "B on an empty query quits" || bad "quit state"
fi

echo "2. run-bananatube: the AudioReach handoff, the second frontend, and the panel's turn"
L=$ROOT/system/rootfs/usr/libexec/banana
[ -x "$L/run-bananatube" ] && ok "launcher present" || bad "no run-bananatube"
grep -q 'audio-handoff' "$L/run-bananatube" && ok "calls the shared audio-handoff" || bad "no audio-handoff call"
grep -q 'hw:0,1' "$L/run-bananatube" && ok "opens MultiMedia2 (hw:0,1), the frontend the menu never releases" || bad "not on MultiMedia2"
grep -qE 'format S16_LE.*channels 2.*rate 48000' "$L/run-bananatube" && ok "S16 / 48000 / 2ch (the AudioReach format)" || bad "unexpected ALSA format"
grep -q 'SEATD_VTBOUND=0' "$L/run-bananatube" && ok "cage's seat is not VT-bound" || bad "seatd VT bound not set"
grep -q 'cage --' "$L/run-bananatube" && ok "runs under cage (the kiosk compositor turns the panel)" || bad "no cage"

echo "3. the app is on the shelf: a manifest that banana-ui scans, and the client it names"
if [ -f "$APP/app.json" ]; then
    grep -q '"run"' "$APP/app.json" && grep -q 'run-bananatube' "$APP/app.json" \
        && ok "app.json names run-bananatube" || bad "app.json run line"
    grep -qE '"ext"[[:space:]]*:[[:space:]]*"app"' "$APP/app.json" && ok "app.json ext = app" || bad "app.json ext"
else
    bad "no app/bananatube/app.json"
fi

echo
echo "run-banana-tube: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
