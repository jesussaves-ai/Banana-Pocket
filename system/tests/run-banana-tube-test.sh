#!/bin/sh
# BananaTube, exercised on the host without the handheld. Three seams are checked by running the client's own logic:
#
#   1. the touch transform: the panel's raw touch axes (1080x1240, the FT5x06) turned 90 must land the four screen
#      corners in the app surface's own 1240x1080 space (the client's `touch` selftest lines cover this);
#   2. the search parsers: the InnerTube `videoRenderer` walk and the yt-dlp `--print` line split;
#   3. the launcher: `run-bananatube` must play through the sound server's ALSA `default` (PipeWire mixes it with the
#      menu), on the display the menu releases (cage) -- the contract run-retroarch has, checked the same way.
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
    # the tap state machine: one gesture = one tap, at the latched press point (the "66d"/"ghh" fix)
    line=$(printf '%s\n' "$out" | grep 'selftest tap:' | head -n 1)
    case "$line" in
    *": ok") ok "one tap per gesture: ${line##*selftest }" ;;
    *) bad "tap state machine: ${line:-no line}" ;;
    esac
    # the parsers
    printf '%s\n' "$out" | grep -q 'selftest innertube: ok' && ok "InnerTube videoRenderer parse" || bad "InnerTube parse"
    printf '%s\n' "$out" | grep -q 'selftest split: ok' && ok "yt-dlp --print line split" || bad "yt-dlp line split"
    printf '%s\n' "$out" | grep -q 'selftest quit: 1' && ok "B on an empty query quits" || bad "quit state"
fi

echo "2. run-bananatube: the AudioReach handoff and the panel's turn"
L=$ROOT/system/rootfs/usr/libexec/banana
[ -x "$L/run-bananatube" ] && ok "launcher present" || bad "no run-bananatube"
grep -q 'SDL_AUDIODRIVER:=alsa' "$L/run-bananatube" && ok "ALSA through the sound server's default (mixes with the menu)" || bad "no ALSA audiodriver default"
! grep -qE 'hw:0,[12]|asoundrc' "$L/run-bananatube" && ok "no second frontend pinned (the server owns the card)" || bad "still pins a raw frontend"
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
