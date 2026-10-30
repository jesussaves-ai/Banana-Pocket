#!/bin/sh
# run-dolphin, exercised on the host without the handheld. The launcher is run from a copy with every absolute root it
# touches rewritten into a temp tree (/data, /run, /usr/share/banana/dolphin, /usr/libexec/banana, the dolphin-emu-nogui
# binary and cage, and the panel's /proc), and the emulator and compositor are stubs that record how they were called.
# Nothing here reads or writes the real /data or /run.
#
# The path rewrite is the one seam: a new absolute root in run-dolphin must be added to xform() below.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)      # system/
ROOT=$(cd "$HERE/.." && pwd)                 # the repo
LAUNCH="$ROOT/system/rootfs/usr/libexec/banana"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }

mkdir -p "$W/libexec" "$W/bin" "$W/share/dolphin" "$W/proc/device-tree"

xform() {
    sed -e "s#/usr/share/banana/dolphin#$W/share/dolphin#g" \
        -e "s#/usr/libexec/banana#$W/libexec#g" \
        -e "s#/usr/bin/dolphin-emu-nogui#$W/bin/dolphin-emu-nogui#g" \
        -e "s#/usr/bin/cage#$W/bin/cage#g" \
        -e "s#/data#$W/data#g" \
        -e "s#/run#$W/run#g" \
        -e "s#/proc#$W/proc#g" "$1"
}
xform "$LAUNCH/run-dolphin" > "$W/libexec/run-dolphin"
xform "$LAUNCH/pad-profile" > "$W/libexec/pad-profile"
xform "$LAUNCH/audio-handoff" > "$W/libexec/audio-handoff"
chmod +x "$W/libexec/run-dolphin" "$W/libexec/pad-profile" "$W/libexec/audio-handoff"
# the templates the launcher copies
cp "$ROOT/system/rootfs/usr/share/banana/dolphin/"*.ini "$W/share/dolphin/"
# the panel says SM4450 (so the Vulkan path is kept); a later case removes it to check the fallback
printf 'qcom,sm4450\0' > "$W/proc/device-tree/compatible"

# the stubs: cage records argv then runs the command; dolphin-emu-nogui records argv
cat > "$W/bin/cage" <<EOF
#!/bin/sh
shift    # the "--"
"\$@"
EOF
cat > "$W/bin/dolphin-emu-nogui" <<EOF
#!/bin/sh
printf '%s\n' "\$*" > "$W/dolphin.args"
exit 0
EOF
chmod +x "$W/bin/cage" "$W/bin/dolphin-emu-nogui"

run() { # run <rom> [env...]
    rom=$1; shift
    printf '%s\n' "$@" > "$W/env"
    env PATH="$W/bin:$PATH" "$@" "$W/libexec/run-dolphin" "$rom" 2>"$W/err" || true
}

# a clean card between scenarios
fresh() {
    rm -rf "$W/data" "$W/run"
    rm -f "$W/dolphin.args" "$W/err"
}

has() { grep -q -- "$2" "$1" 2>/dev/null; }

echo "1. the Wii on Dolphin writes the card's config and launches nogui under cage"
fresh
run /data/roms/wii/game.rvz BANANA_PAD_PROFILE=4
C=$W/data/dolphin-emu/Config
[ -f "$C/Dolphin.ini" ] && ok "Dolphin.ini written" || bad "Dolphin.ini missing"
[ -f "$C/GFX.ini" ] && ok "GFX.ini written" || bad "GFX.ini missing"
has "$C/Dolphin.ini" 'Backend = ALSA' && ok "ALSA audio backend" || bad "audio backend"
has "$C/Dolphin.ini" 'GFXBackend = Vulkan' && ok "Vulkan on SM4450" || bad "GFXBackend"
has "$C/GFX.ini" 'AspectRatio = 2' && ok "4:3 (ForceStandard)" || bad "aspect ratio"
has "$C/GFX.ini" 'ShaderCompilationMode = 0' && ok "synchronous shaders (cheaper on the a613)" || bad "shader mode"
has "$W/data/dolphin-emu/home/.asoundrc" 'hw:0,1' && ok "the game's ALSA default is MultiMedia2" || bad "game asoundrc"
has "$W/dolphin.args" '-p wayland' && ok "Wayland platform (under cage)" || bad "platform"
has "$W/dolphin.args" '-a HLE' && ok "DSP HLE" || bad "audio emulation"
has "$W/dolphin.args" '/data/roms/wii/game.rvz' && ok "the game was passed with -e" || bad "no game"

echo "2. the 4-button pad plays the sideways Wii Remote with the shake on R1"
fresh
run /data/roms/wii/game.rvz BANANA_PAD_PROFILE=4
W1=$W/data/dolphin-emu/Config/WiimoteNew.ini
has "$W1" 'Options/Sideways Wiimote = True' && ok "sideways mode" || bad "sideways"
has "$W1" 'Shake/X = Button 6' && ok "shake on R1" || bad "shake button"
has "$W1" 'Device = evdev/0/Retroid Pocket Classic Gamepad' && ok "the handheld's own pad" || bad "device"
[ "$(cat "$W/data/dolphin-emu/.banana-pad-profile")" = 4 ] && ok "profile marker 4" || bad "profile marker"

echo "3. the 6-button variant shifts the indices and keeps the shake on a shoulder"
fresh
run /data/roms/wii/game.rvz BANANA_PAD_PROFILE=6
W6=$W/data/dolphin-emu/Config/WiimoteNew.ini
has "$W6" 'Shake/X = Button 8' && ok "shake moved with the shifted pad" || bad "6-button shake"
has "$W6" 'Buttons/Home = Button 13' && ok "home on the shifted MODE button" || bad "6-button home"
[ "$(cat "$W/data/dolphin-emu/.banana-pad-profile")" = 6 ] && ok "profile marker 6" || bad "profile marker 6"

echo "4. a machine without Turnip falls back to OGL"
fresh
: > "$W/proc/device-tree/compatible"
run /data/roms/wii/game.rvz BANANA_PAD_PROFILE=4
has "$W/data/dolphin-emu/Config/Dolphin.ini" 'GFXBackend = OGL' && ok "OGL fallback" || bad "no OGL fallback"
printf 'qcom,sm4450\0' > "$W/proc/device-tree/compatible"

echo
echo "run-dolphin: $pass passed, $fail failed"
[ "$fail" = 0 ]
