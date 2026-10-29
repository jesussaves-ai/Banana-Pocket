#!/bin/sh
# rpc-thermals.sh, on the host against a mock /sys and /proc: it must name each thermal zone, the fan's duty and
# tachometer, each CPU cluster's clock and the GPU's, and the load -- the fields the on-unit profile is read from.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
S=$W/sys P=$W/proc
mkdir -p "$S/class/thermal/thermal_zone0" "$S/class/thermal/thermal_zone1" "$S/class/hwmon/hwmon0" \
	"$S/devices/system/cpu/cpufreq/policy0" "$S/devices/system/cpu/cpufreq/policy6" "$S/class/devfreq/5000000.gpu" "$P"
echo cpuss0-thermal > "$S/class/thermal/thermal_zone0/type"; echo 52000 > "$S/class/thermal/thermal_zone0/temp"
echo gpu-thermal > "$S/class/thermal/thermal_zone1/type"; echo 48000 > "$S/class/thermal/thermal_zone1/temp"
echo 120 > "$S/class/hwmon/hwmon0/pwm1"; echo 3200 > "$S/class/hwmon/hwmon0/fan1_input"
echo 1401000 > "$S/devices/system/cpu/cpufreq/policy0/scaling_cur_freq"
echo 1804000 > "$S/devices/system/cpu/cpufreq/policy6/scaling_cur_freq"
echo 550000000 > "$S/class/devfreq/5000000.gpu/cur_freq"
echo "0.42 0.30 0.10 1/120 123" > "$P/loadavg"

out=$(RPC_SYS=$S RPC_PROC=$P sh "$ROOT/tools/rpc-thermals.sh" 1 | tail -n 1)
want="cpuss0-thermal=52.0 gpu-thermal=48.0 fan=120/3200 policy0=1.40 policy6=1.80 gpu=550000000 load=0.42 0.30 0.10"
case "$out" in
*"$want") echo "rpc-thermals: ok ($out)"; exit 0 ;;
esac
echo "rpc-thermals: FAIL"
echo "  got:  $out"
echo "  want: ...$want"
exit 1
