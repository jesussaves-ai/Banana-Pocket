#!/bin/sh
# The bridge's text, tested without the handheld: bananad's own formatter and the achievement parser, run through
# the --selftest hook the same way `banana-tube --selftest` is tested.
set -e
[ -f /usr/include/alsa/asoundlib.h ] || { echo "hud-test: skipped (no libasound headers)"; exit 0; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
cc -O2 -Wall -Wextra -o "$W/bananad" "$HERE/bananad.c" -lasound -lm

out=$("$W/bananad" --selftest hud)
printf '%s\n' "$out"
mkdir -p "$W/run"
out2=$("$W/bananad" --selftest cheevos "$W/run")
printf '%s\n' "$out2"

fail=0
want() { printf '%s\n' "$out" | grep -qxF "$1" || { echo "FAIL: wanted \"$1\"" >&2; fail=1; }; }
want 'hud en: Volume 62 %'
want 'hud en: Brightness 80 %'
want 'hud en: Battery low: 9 %'
want 'hud en: Battery empty, shutting down'
want 'hud es: Volumen 62 %'
want 'hud de: Akku schwach: 9 %'
want 'hud off: 0'
want 'hud no game: 0'
want 'cheevos: Sonic (+15)'
want 'cheevos odd: 100% Fun (+15)'
want 'cheevos 4 fields: 0'
want 'cheevos 6 fields: 0'
want 'cheevos no points: Sonic'
want2() { printf '%s\n' "$out2" | grep -qxF "$1" || { echo "FAIL: wanted \"$1\"" >&2; fail=1; }; }
want2 'cheevos absent: 0 0'
want2 'cheevos two: 2 1'
want2 'cheevos append: 1 1'
want2 'cheevos shrink: 1 1'
want2 'cheevos off: 0 0'
want2 'cheevos no reader: 0 0'
has() { grep -qF "$2" "$1" || { echo "FAIL: $1 lacks $2" >&2; fail=1; }; }
has "$HERE/bananad.c" 'signal(SIGPIPE, SIG_IGN);'
has "$HERE/bananad.c" 'else osd("volume", pct);'
has "$HERE/bananad.c" 'ra_cmd("PAUSE_TOGGLE")'
has "$HERE/bananad.c" 'IN_CLOSE_WRITE'
has "$HERE/bananad.c" '/run/banana/hud-live'
has "$HERE/bananad.c" 'cheevos_line(ln, buf, sizeof buf) && hud_show(buf)'   # never mark a line the game did not get
has "$HERE/../../ui/src/main.cpp" 'unlink("/run/banana/hud-live");'
# a language change must reach the daemon now, not at the next boot: the reload block re-reads it, and the menu pokes
[ "$(grep -c 'load_lang();' "$HERE/bananad.c")" -ge 2 ] \
	|| { echo "FAIL: the reload block does not call load_lang()" >&2; fail=1; }
has "$HERE/../../ui/src/settings.cpp" 'if (it.key == "lang") { i18nSet(it.values[at]); poke("bananad"); return CHANGED; }'
[ "$fail" = 0 ] && echo "hud-test: all checks passed"
exit "$fail"
