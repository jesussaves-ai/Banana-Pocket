#!/bin/sh
# What the handheld is doing under an emulator, one line a second, so a game can be compared across builds, settings
# or operating systems on the same SoC (BananaOS/Linux and the stock Android). It logs only things both expose --
# each thermal zone, the fan, every CPU policy's clock, every devfreq node's clock, the load, memory and the
# emulator's own CPU share -- from /sys and /proc, so it runs unchanged in a Linux shell, over ssh, or from
# `adb shell` on Android.
#
#   scp tools/rpc-emu-bench.sh root@<handheld>:/data/          # or: adb push tools/rpc-emu-bench.sh /data/local/tmp/
#   ssh root@<handheld> RPC_EMU=<name> sh /data/rpc-emu-bench.sh 300 > bench-linux.txt
#   adb shell RPC_EMU=<name> sh /data/local/tmp/rpc-emu-bench.sh 300 > bench-android.txt
#
# Start it, then launch the game and play the SAME scene for the run; a summary is printed at the end (and to
# RPC_OUT). RPC_EMU is the process name to watch for the CPU share (the default "retroarch" suits the libretro
# cores); its command name carries it. A drawn FPS counter is not logged, so read it alongside the log.
#
#   RPC_SYS=<dir> RPC_PROC=<dir> sh ...    a mock tree (the host test); RPC_OUT names the log file
SYS=${RPC_SYS:-/sys}
PROC=${RPC_PROC:-/proc}
EMU=${RPC_EMU:-retroarch}
secs=${1:-300}

hz=$(getconf CLK_TCK 2>/dev/null); [ -n "$hz" ] || hz=100
# the log file: RPC_OUT if set, else the first writable of the usual spots (Linux /tmp; Android has no /tmp but keeps
# /data/local/tmp). Falls back to /dev/null, where the per-second lines still reach stdout but the summary is empty.
if [ -n "$RPC_OUT" ]; then OUT=$RPC_OUT
else
	OUT=
	for d in "$TMPDIR" /data/local/tmp /tmp; do
		[ -n "$d" ] && [ -d "$d" ] && [ -w "$d" ] && { OUT=$d/rpc-emu-bench.$$; break; }
	done
	[ -n "$OUT" ] || OUT=/dev/null
fi
: > "$OUT" 2>/dev/null || OUT=/dev/null

# the watched processes: their command name carries $EMU (the kernel truncates it, so match a part), but skip the
# launcher shells -- a `run-<name>` wrapper forks the real binary, and the script idles at 0%.
# Everything here is shell builtins (comm is one line; no awk/cut per pid), because Android has ~600 processes and
# forking per pid made a sample take ~17s instead of one second.
emu_pids() {
	for c in "$PROC"/[0-9]*/comm; do
		[ -r "$c" ] || continue
		read -r n < "$c" 2>/dev/null || continue
		case "$n" in *"$EMU"*) ;; *) continue ;; esac
		case "$n" in run-*|sh|bash|dash) continue ;; esac
		p=${c#"$PROC"/}; echo "${p%/comm}"
	done
}
# their combined CPU time in ticks (utime + stime); the comm can hold spaces, so drop everything through the last ')'
emu_ticks() {
	t=0
	for p in $1; do
		[ -r "$PROC/$p/stat" ] || continue
		read -r line < "$PROC/$p/stat" 2>/dev/null || continue
		line=${line##*)}
		set -- $line
		t=$((t + ${12:-0} + ${13:-0}))
	done
	printf '%d' "$t"
}

printf '# epoch <zone>=<C> ... fan=<pwm>/<rpm> <policy>=<GHz> ... <devfreq>=<MHz> ... load1=<L> mem=<avail>/<total>MB emu=<pid>:<cpu%%>\n'
end=0
[ "$secs" -gt 0 ] 2>/dev/null && end=$(( $(date +%s) + secs ))
prev_ticks=
while :; do
	now=$(date +%s)
	[ "$end" -gt 0 ] && [ "$now" -ge "$end" ] && break
	pids=$(emu_pids); pid=$(printf '%s\n' "$pids" | head -n1); t=$(emu_ticks "$pids")
	cpu=""
	dt=1; [ -n "$prev_now" ] && dt=$((now - prev_now)); [ "$dt" -gt 0 ] || dt=1
	[ -n "$prev_ticks" ] && [ -n "$t" ] && cpu=$(( (t - prev_ticks) * 100 / (hz * dt) ))
	line="$now"
	for z in "$SYS"/class/thermal/thermal_zone*; do
		read -r v < "$z/temp" 2>/dev/null || continue
		[ -n "$v" ] || continue
		read -r ty < "$z/type" 2>/dev/null
		case "$v" in -*) s=-; v=${v#-} ;; *) s= ;; esac
		line="$line $ty=${s}$((v/1000)).$(( (v%1000)/100 ))"
	done
	line="$line fan=$(cat "$SYS"/class/hwmon/hwmon*/pwm1 2>/dev/null | head -n1)/$(cat "$SYS"/class/hwmon/hwmon*/fan1_input 2>/dev/null | head -n1)"
	for p in "$SYS"/devices/system/cpu/cpufreq/policy*; do
		[ -r "$p/scaling_cur_freq" ] || continue
		line="$line ${p##*/}=$(awk -v v="$(cat "$p/scaling_cur_freq")" 'BEGIN{printf "%.2f", v/1000000}')"
	done
	for d in "$SYS"/class/devfreq/*; do
		[ -r "$d/cur_freq" ] || continue
		line="$line ${d##*/}=$(awk -v v="$(cat "$d/cur_freq")" 'BEGIN{printf "%.0f", v/1000000}')"
	done
	line="$line load1=$(cut -d' ' -f1 "$PROC/loadavg" 2>/dev/null) mem=$(awk '/MemAvailable/{printf "%d",$2/1024}' "$PROC/meminfo" 2>/dev/null)/$(awk '/MemTotal/{printf "%d",$2/1024}' "$PROC/meminfo" 2>/dev/null) emu=${pid:-none}:${cpu:-0}"
	printf '%s\n' "$line"; printf '%s\n' "$line" >> "$OUT" 2>/dev/null
	prev_ticks=$t; prev_now=$now
	sleep 1
done

awk '
{
	for (i = 2; i <= NF; i++) {
		split($i, a, "="); n = a[1]; v = a[2]
		if (n == "fan") { split(v, b, "/"); if (b[1] + 0 > fp) fp = b[1] + 0; continue }
		if (n == "load1") { l += v; nl++; continue }
		if (n == "mem") continue
		if (n == "emu") { split(v, b, ":"); c = b[2] + 0; cu += c; nc++; if (c > cp) cp = c; continue }
		if (v ~ /^[0-9.]+$/) { s[n] += v; k[n]++; if (v + 0 > pk[n]) pk[n] = v + 0 }
	}
}
END {
	printf "# SUMMARY  samples=%d\n", NR
	for (n in s) printf "#   %-20s avg=%.1f peak=%.1f\n", n, s[n]/k[n], pk[n]
	printf "#   %-20s peak=%d\n", "fan pwm", fp
	printf "#   %-20s avg=%.2f\n", "load1", (nl ? l/nl : 0)
	printf "#   %-20s avg=%.0f%% peak=%.0f%%\n", "emulator cpu", (nc ? cu/nc : 0), cp
}' "$OUT"
[ "$OUT" = /dev/null ] || printf '# log: %s\n' "$OUT"
