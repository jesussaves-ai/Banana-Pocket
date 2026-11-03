#!/bin/sh
# sound-config-test: the sound chain's rules, checked without a speaker. `make sound-test` tests the real chain on the
# handheld; this one guards the code and the configuration that has broken it before, so a change that would silence
# the menu fails here (in `make test` and CI) instead of after an update and a reboot.
#
# It checks the laws from AGENTS.md §5 / docs/ARCHITECTURE.md §5, not the hardware:
#   - the menu is a PipeWire client: it re-reads ALSA's config per open, publishes its sound state, and a handover
#     stays handed over (the 0.5.2 fixes)
#   - no client writes the DSP's hardware mixer or hw:0 (that was the handoff mess)
#   - the UCM profile puts the shared FE<->BE switches in SectionVerb only (a SectionDevice teardown clobbers the
#     active route -- the DSP clears its routing on graph teardown)
#   - WirePlumber never idle-suspends the node and sets the q6apm-safe buffering per node (monitor.alsa.rules, not
#     monitor.alsa.properties, which ACP's nodes never see)
#   - sound-server waits for the Speaker sink and can recover it; the runtime dir is the one the clients use
#   - the image would carry the pieces (ALSA plugin, aplay, wireplumber) and the menu is linked against libasound
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)		# system/
ROOT=$(cd "$HERE/.." && pwd)
SRC=$ROOT/ui/src
UCM=$ROOT/system/rootfs/usr/share/alsa/ucm2/Retroid/PocketClassic/HiFi.conf
WP=$ROOT/system/rootfs/usr/share/wireplumber/wireplumber.conf.d/20-banana-no-suspend.conf
SERVER=$ROOT/system/rootfs/usr/libexec/banana/sound-server
DEFCONFIG=$ROOT/buildroot/configs/banana_retroid_pocket_classic_defconfig
IMG=$ROOT/output/retroid-pocket-classic/target
PASS=0 FAIL=0 WARN=0

ok() { PASS=$((PASS + 1)); printf '  ok:   %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL: %s\n' "$*"; }
warn() { WARN=$((WARN + 1)); printf '  warn: %s\n' "$*"; }
has() { if grep -q -- "$2" "$1" 2>/dev/null; then ok "$3"; else bad "$3"; fi }
lack() { if grep -q -- "$2" "$1" 2>/dev/null; then bad "$3"; else ok "$3"; fi }

echo "1. the menu is a PipeWire client (ui/src/sound.cpp)"
has "$SRC/sound.cpp" 'snd_config_update_free_global' "each open re-reads ALSA's config (elects the server, not card 0)"
has "$SRC/sound.cpp" 'run/banana/sound-state' "publishes /run/banana/sound-state (bananad's handover ack)"
has "$SRC/sound.cpp" 'handedOver' "a handover stays handed over until warm()/setBgm(true)"
has "$SRC/sound.cpp" 'seconds(20)' "an idle device is closed again after 20 s of quiet"
has "$SRC/sound.cpp" 'warmAt' "the boot warm-up has a deadline"
has "$SRC/sound.cpp" 'type pipewire\|default' "opens ALSA's \"default\" (the server), not a hardware card"
lack "$SRC/sound.cpp" 'hw:0' "no hardware device is opened directly"

echo "2. bananad writes the pipewire default and waits for the handover"
has "$ROOT/system/bananad/bananad.c" 'asound.conf' "writes /run/banana/asound.conf"
has "$ROOT/system/bananad/bananad.c" 'type pipewire' "the default it writes is the pipewire plugin"
has "$ROOT/system/bananad/bananad.c" 'sound-state' "waits for the menu's sound-state ack before sleeping"

echo "3. no client writes the DSP's mixer or hw:0"
files=$(ls "$SRC"/*.cpp "$SRC"/*.h "$ROOT/app/bananatube"/*.cpp 2>/dev/null)
for f in "$ROOT/system/rootfs/usr/libexec/banana/run-port" "$ROOT/system/rootfs/usr/libexec/banana/run-retroarch" \
    "$ROOT/system/rootfs/usr/libexec/banana/run-bananatube"; do
    [ -f "$f" ] && files="$files $f"
done
hits=$(grep -n -E 'amixer|alsaucm|hw:[0-9]' $files 2>/dev/null | grep -v -E ':[0-9]+:[[:space:]]*(//|#|\*)')
if [ -z "$hits" ]; then
    ok "no amixer/alsaucm/hw:0 outside comments (clients are PipeWire only)"
else
    bad "a client touches the hardware mixer: $(printf '%s\n' "$hits" | head -n 3 | tr '\n' ' ')"
fi

echo "4. the UCM profile keeps the shared switches in SectionVerb"
if [ -f "$UCM" ]; then
    wrong=$(awk '
        /^[[:space:]]*Section/ { sec = $1 }
        /cset/ && sec != "SectionVerb" { print FNR ": " sec ": " $0 }
    ' "$UCM")
    if [ -z "$wrong" ]; then ok "every cset is in SectionVerb (no SectionDevice touches a FE<->BE switch)"; else bad "a cset is outside SectionVerb: $(printf '%s\n' "$wrong" | head -n 2 | tr '\n' ' ')"; fi
    spread=$(awk '/^[[:space:]]*Section/ { sec = $1 } /cset/ && sec == "SectionVerb"' "$UCM" | grep -c 'Audio Mixer')
    [ "$spread" -ge 2 ] && ok "the verb routes the frontends ($spread MultiMedia switches)" || bad "the verb has no FE<->BE switches ($spread)"
    [ "$(grep -c 'cset' "$UCM")" -ge 2 ] && ok "the codec/SoundWire path is set up in the verb" || bad "the verb does not touch the codec"
else
    bad "$UCM is missing (the card would come up with no profile)"
fi

echo "5. WirePlumber: no idle suspend, q6apm-safe buffering per node"
has "$WP" 'session.suspend-timeout-seconds = 0' "the nodes never idle-suspend (the DSP clears its route on teardown)"
has "$WP" 'api.alsa.period-num' "the period count is set (api.alsa.period-num)"
has "$WP" 'api.alsa.period-size' "the period size is set"
has "$WP" 'api.alsa.headroom' "headroom is set"
has "$WP" 'monitor.alsa.rules' "per-node props go through monitor.alsa.rules"
props=$(sed -n '/^monitor.alsa.properties *=/,/^}/p' "$WP" | grep -c 'api\.alsa\.')
[ "$props" -eq 0 ] && ok "no api.alsa.* under monitor.alsa.properties (ACP's nodes never see it)" || bad "$props api.alsa.* lines under monitor.alsa.properties"

echo "6. sound-server brings the card up and recovers it"
has "$SERVER" "Speaker playback" "waits for the Speaker sink, not just any sink"
has "$SERVER" "recover)" "has a recover action for a wedged session manager"
has "$SERVER" "pipewire-0" "waits for the server's socket"
has "$SERVER" "RUNDIR=/run/banana/wayland" "uses the clients' runtime dir"
has "$ROOT/system/rootfs/etc/init.d/rcS" 'XDG_RUNTIME_DIR=/run/banana/wayland' "rcS exports that runtime dir to the server and the clients"

echo "7. ALSA's default is the server"
CONF=$ROOT/system/rootfs/etc/asound.conf
if [ -L "$CONF" ] && [ "$(readlink "$CONF")" = /run/banana/asound.conf ]; then
    ok "/etc/asound.conf -> /run/banana/asound.conf (in the image, not only on a hand-made card)"
else
    bad "$CONF must be a symlink to /run/banana/asound.conf (a fresh build would have no default device)"
fi
has "$ROOT/system/bananad/bananad.c" 'pcm.!default { type pipewire }' "bananad writes pcm.!default { type pipewire }"
has "$ROOT/system/bananad/bananad.c" 'ctl.!default { type pipewire }' "and ctl.!default (alsactl/amixer follow it)"

echo "8. the image carries the pieces"
for p in BR2_PACKAGE_PIPEWIRE BR2_PACKAGE_WIREPLUMBER BR2_PACKAGE_ALSA_LIB=y BR2_PACKAGE_ALSA_UTILS_APLAY; do
    has "$DEFCONFIG" "^$p" "$p"
done
if [ -d "$IMG" ]; then
    if [ -x "$IMG/usr/bin/banana-ui" ]; then
        if command -v readelf >/dev/null 2>&1; then
            if readelf -d "$IMG/usr/bin/banana-ui" 2>/dev/null | grep -q libasound; then ok "the image's banana-ui is linked against libasound (built with ALSA)"; else bad "the image's banana-ui has no libasound: it was built without BANANA_ALSA (menu silent)"; fi
        else
            warn "readelf is not here: cannot check that banana-ui links libasound"
        fi
        if [ -f "$IMG/etc/banana/version" ] && [ "$(cat "$IMG/etc/banana/version")" != "$(cat "$ROOT/VERSION")" ]; then
            warn "the image tree is $(cat "$IMG/etc/banana/version"), the tree is $(cat "$ROOT/VERSION") (rebuild before shipping)"
        fi
        [ -x "$IMG/usr/bin/aplay" ] && ok "aplay is in the image (the sound-test client)" || warn "aplay is not in the image yet (sound-test falls back to mpv)"
    fi
else
    warn "no built image tree (output/.../target): skipped the image check -- run make retroid-pocket-classic"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    printf 'sound-config-test: all %d checks passed%s\n' "$PASS" "$( [ "$WARN" -gt 0 ] && echo ", $WARN warning(s)" )"
    exit 0
fi
printf 'sound-config-test: %d failed, %d passed%s\n' "$FAIL" "$PASS" "$( [ "$WARN" -gt 0 ] && echo ", $WARN warning(s)" )"
exit 1
