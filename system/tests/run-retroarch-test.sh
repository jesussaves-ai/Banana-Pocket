#!/bin/sh
# run-retroarch, exercised on the host without the handheld. The launcher is run from a copy with every absolute root
# it touches rewritten into a temp tree (/data, /run, /etc/banana, /usr/libexec/banana, /usr/lib/libretro, the core's
# /usr/share/banana/cores, and the panel's /proc and /sys), and `retroarch` is a stub that records how it was called.
# Nothing here reads or writes the real /data or /run, and it needs no root or namespaces, so `make test` and CI
# cover the launcher's per-core defaults, its config writes and the RetroAchievements hand-off without a device.
#
# The path rewrite is the one seam: a new absolute root in run-retroarch must be added to xform() below (a bare
# `git grep /data` in the launcher is the tell). Everything else is the real script, unchanged.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)      # system/
ROOT=$(cd "$HERE/.." && pwd)                 # the repo
LAUNCH="$ROOT/system/rootfs/usr/libexec/banana"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }

mkdir -p "$W/libexec" "$W/etc/banana" "$W/libretro" "$W/share/cores"
cp "$ROOT/system/rootfs/etc/banana/retroarch.cfg" "$ROOT/system/rootfs/etc/banana/settings.conf" "$W/etc/banana/"
cp "$ROOT/system/rootfs/usr/share/banana/cores/Flycast.opt" "$W/share/cores/"

# ${W} is a mktemp path with no sed-special characters; the roots are literal strings in the launcher
xform() {
    sed -e "s#/usr/share/banana/cores#$W/share/cores#g" \
        -e "s#/usr/libexec/banana#$W/libexec#g" \
        -e "s#/usr/lib/libretro#$W/libretro#g" \
        -e "s#/etc/banana#$W/etc/banana#g" \
        -e "s#/data#$W/data#g" \
        -e "s#/run#$W/run#g" \
        -e "s#/usr/bin/cage#$W/usrbin/cage#g" \
        -e "s#/proc#$W/proc#g" \
        -e "s#/sys/class/drm#$W/sys/class/drm#g" "$1"
}
xform "$LAUNCH/run-retroarch" > "$W/libexec/run-retroarch"
xform "$LAUNCH/ra-secrets" > "$W/libexec/ra-secrets"
chmod +x "$W/libexec/run-retroarch" "$W/libexec/ra-secrets"

# the stub: record argv, and keep the account config the run appends (the game's own copy of the password/token)
cat > "$W/retroarch" <<EOF
#!/bin/sh
printf '%s\n' "\$*" > "$W/retroarch.args"
for a in "\$@"; do case "\$a" in *retroarch-account.cfg*) cp "\${a##*|}" "$W/account.passed" 2>/dev/null ;; esac; done
exit 0
EOF
chmod +x "$W/retroarch"

# a clean card between scenarios
fresh() {
    rm -rf "$W/data/retroarch" "$W/run/banana" "$W/data/banana/settings.conf" "$W/data/banana/accounts.conf"
    rm -f "$W/retroarch.args" "$W/account.passed"
    mkdir -p "$W/run/banana"
    rm -f "$W/libretro"/*_libretro.so
    for c in gpsp gambatte swanstation pcsx_rearmed flycast; do : > "$W/libretro/${c}_libretro.so"; done
}
# run <system> <core> [settings.conf lines...]: one launch, the way banana-ui would
run() {
    s=$1 core=$2; shift 2
    [ $# -gt 0 ] && printf '%s\n' "$@" > "$W/data/banana/settings.conf"
    PATH="$W:$PATH" BANANA_SYSTEM=$s BANANA_ROM=/g.gba "$W/libexec/run-retroarch" "$core" /g.gba 2>"$W/err" || true
}
cfg() { sed -n "s/^$1 = \"\(.*\)\"\$/\1/p" "$W/data/retroarch/retroarch.cfg" | tail -n 1; }
val() { sed -n "s/^$1 = \"\(.*\)\"\$/\1/p" "$2" | tail -n 1; }
has() { grep -q "$2" "$1" 2>/dev/null; }

echo "1. a plain system gets the shared frame, and no core's own files"
fresh
run gba gpsp
[ "$(cfg video_driver)" = gl ] && ok "video_driver is the no-SoC fallback" || bad "video_driver=$(cfg video_driver)"
[ "$(cfg input_driver)" = udev ] && ok "input_driver udev" || bad "input_driver=$(cfg input_driver)"
[ "$(cfg audio_driver)" = alsathread ] && ok "audio_driver alsathread" || bad "audio_driver=$(cfg audio_driver)"
[ "$(cfg audio_latency)" = 64 ] && ok "audio_latency default" || bad "audio_latency=$(cfg audio_latency)"
[ "$(cfg video_monitor_index)" = 1 ] && ok "one-panel video_monitor_index 1" || bad "video_monitor_index=$(cfg video_monitor_index)"
has "$W/retroarch.args" gpsp_libretro.so && ok "the gpsp core was launched" || bad "the core was not launched"
[ -d "$W/data/retroarch/config/SwanStation" ] && bad "a GBA run wrote SwanStation's files" || ok "no per-core files for a plain system"

echo "2. the PlayStation on SwanStation writes its own defaults"
fresh
run psx swanstation
O=$W/data/retroarch/config/SwanStation/SwanStation.opt
has "$O" 'swanstation_GPU_ResolutionScale = "4"' && ok "ResolutionScale 4" || bad "ResolutionScale"
has "$O" 'swanstation_GPU_Renderer = "Vulkan"' && ok "Vulkan renderer" || bad "renderer"
has "$O" 'swanstation_BIOS_PatchFastBoot = "true"' && ok "fast boot" || bad "fast boot"
[ "$(val video_scale_integer "$W/data/retroarch/config/SwanStation/SwanStation.cfg")" = false ] && ok "integer scaling off (core fills the viewport)" || bad "integer scaling"
[ -f "$W/data/retroarch/.banana-swans-9" ] && ok "written-once marker" || bad "the write-once marker"

echo "3. core.psx picks PCSX-ReARMed instead"
fresh
run psx swanstation 'core.psx = pcsx_rearmed'
has "$W/data/retroarch/config/PCSX-ReARMed/PCSX-ReARMed.opt" 'pcsx_rearmed_drc = "enabled"' && ok "PCSX defaults written" || bad "PCSX defaults"
has "$W/retroarch.args" pcsx_rearmed_libretro.so && ok "the PCSX core was launched" || bad "the core was not launched"

echo "4. a core override cannot keep the panel settings"
fresh
run psx swanstation
mkdir -p "$W/data/retroarch/config/SwanStation"
printf 'video_monitor_index = "2"\nstdin_cmd_enable = "true"\nvideo_scale_integer = "true"\n' > "$W/data/retroarch/config/SwanStation/SwanStation.cfg"
run psx swanstation
F=$W/data/retroarch/config/SwanStation/SwanStation.cfg
has "$F" 'video_monitor_index' && bad "video_monitor_index was kept" || ok "video_monitor_index dropped"
has "$F" 'stdin_cmd_enable' && bad "stdin_cmd_enable was kept" || ok "stdin_cmd_enable dropped"
has "$F" 'video_scale_integer' && ok "Banana's own key kept" || bad "video_scale_integer dropped"

echo "5. the Dreamcast on Flycast writes its defaults and the shoulder remap"
fresh
run dreamcast flycast
has "$W/data/retroarch/config/Flycast/Flycast.opt" 'Banana: Flycast' && ok "Flycast.opt copied" || bad "Flycast.opt"
[ "$(val input_player1_analog_dpad_mode "$W/data/retroarch/config/Flycast/Flycast.cfg")" = 1 ] && ok "d-pad drives the emulated stick" || bad "analog dpad"
R=$W/data/retroarch/remaps/Flycast/Flycast.rmp
{ has "$R" '^input_player1_btn_l = "12"' && has "$R" '^input_player1_btn_r2 = "11"'; } && ok "the triggers are on the shoulders" || bad "the shoulder remap"
has "$W/retroarch.args" flycast_libretro.so && ok "the flycast core was launched" || bad "the core was not launched"

echo "6. the RetroAchievements account reaches the game, and not retroarch.cfg"
fresh
printf 'ra_user=someone\nra_password=secret\nra_retroarch_token=tok\n' > "$W/data/banana/accounts.conf"
run gba gpsp
[ "$(cfg cheevos_enable)" = true ] && ok "cheevos on" || bad "cheevos=$(cfg cheevos_enable)"
[ "$(cfg cheevos_username)" = someone ] && ok "the account's name" || bad "cheevos_username=$(cfg cheevos_username)"
[ "$(cfg cheevos_badges_enable)" = true ] && ok "badges on" || bad "cheevos_badges_enable=$(cfg cheevos_badges_enable)"
has "$W/account.passed" 'cheevos_password = "secret"' && ok "the password was passed to the game" || bad "the password was not passed"
has "$W/account.passed" 'cheevos_token = "tok"' && ok "the token was passed to the game" || bad "the token was not passed"
has "$W/data/retroarch/retroarch.cfg" 'cheevos_password' && bad "the password was left in retroarch.cfg" || ok "the password never reached retroarch.cfg"
[ -f "$W/run/banana/retroarch-account.cfg" ] && bad "the account file was left behind" || ok "the account file was cleaned up"

echo "7. the PlayStation falls back to PCSX-ReARMed when SwanStation is not installed"
fresh
rm -f "$W/libretro/swanstation_libretro.so"
run psx swanstation
has "$W/retroarch.args" pcsx_rearmed_libretro.so && ok "fell back to PCSX-ReARMed" || bad "no fallback"
has "$W/data/retroarch/config/PCSX-ReARMed/PCSX-ReARMed.opt" 'pcsx_rearmed_drc' && ok "PCSX defaults written" || bad "PCSX defaults"

echo
echo "run-retroarch: $pass passed, $fail failed"
[ "$fail" = 0 ]
