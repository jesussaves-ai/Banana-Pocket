#!/bin/sh
# Profile the RPC's thermals and its fan on the handheld, under the load you are profiling: one line a second of
# every thermal zone, the fan's duty and tachometer, each CPU cluster's clock and the GPU's, until Ctrl-C (or for N
# seconds). It is the tool for the "Tune pwm_fan curve and profile thermals under sustained load" roadmap item, run
# on a unit rather than guessed at.
#
#   scp tools/rpc-thermals.sh root@169.254.170.2:/data/          # over the USB link, docs/RPC-TEST.md
#   ssh root@169.254.170.2 sh /data/rpc-thermals.sh 600 > thermals.txt
#
# Start it, then play the game you are profiling (an N64 or PSX title is the heavy case). The fan is the pwm-fan:
# /sys/class/hwmon/hwmon*/pwm1 is the duty the thermal governor set (0 = off .. 255 = full) and fan1_input its
# tachometer. cpuss0-thermal and gpu-thermal are the two zones whose trips move it
# (buildroot/board/retroid-pocket-classic/dts/.../sm4450-retroid-pocket-classic-revq.dts: off below 60 C, then 60/120/
# 180/255). Send the log back with the game, the `banana-perf` profile it ran under, and the room temperature.
#
#   sh tools/rpc-thermals.sh [seconds]          0 (or no argument): until Ctrl-C
#   RPC_SYS=<dir> RPC_PROC=<dir> sh ... 1        a mock tree, which is how the host test drives it
SYS=${RPC_SYS:-/sys}
PROC=${RPC_PROC:-/proc}
secs=${1:-0}

fmt() { awk -v v="$1" 'BEGIN { printf "%.1f", v / 1000 }'; }
ghz() { awk -v v="$1" 'BEGIN { printf "%.2f", v / 1000000 }'; }

printf '# epoch  <zone>=<C> ...  fan=<pwm>/<rpm>  <policy>=<GHz> ...  gpu=<Hz>  load\n'
end=0
if [ "$secs" -gt 0 ] 2>/dev/null; then end=$(( $(date +%s) + secs )); fi
while :; do
	now=$(date +%s)
	[ "$end" -gt 0 ] && [ "$now" -ge "$end" ] && break
	{
		printf '%s' "$now"
		for z in "$SYS"/class/thermal/thermal_zone*; do
			[ -r "$z/temp" ] || continue
			printf ' %s=%s' "$(cat "$z/type" 2>/dev/null)" "$(fmt "$(cat "$z/temp" 2>/dev/null)")"
		done
		printf ' fan=%s/%s' "$(cat "$SYS"/class/hwmon/hwmon*/pwm1 2>/dev/null | head -n 1)" \
			"$(cat "$SYS"/class/hwmon/hwmon*/fan1_input 2>/dev/null | head -n 1)"
		for p in "$SYS"/devices/system/cpu/cpufreq/policy*; do
			[ -r "$p/scaling_cur_freq" ] || continue
			printf ' %s=%s' "${p##*/}" "$(ghz "$(cat "$p/scaling_cur_freq" 2>/dev/null)")"
		done
		printf ' gpu=%s' "$(cat "$SYS"/class/devfreq/*/cur_freq 2>/dev/null | head -n 1)"
		printf ' load=%s' "$(cut -d' ' -f1-3 "$PROC/loadavg" 2>/dev/null)"
		printf '\n'
	} 2>/dev/null
	sleep 1
done
