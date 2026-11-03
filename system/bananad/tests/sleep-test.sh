#!/bin/sh
# bananad's sleep, tested without the handheld: bananad runs in its own mount and PID namespaces against a made-up
# /sys (the battery, the backlight, the RTC, /sys/power) and /dev/input (FIFOs that carry real
# struct input_event records). /sys/power/state is a FIFO: bananad's write of "mem" blocks until the test reads it,
# which is the handheld being asleep; the test decides what the world looks like when it "wakes". A stand-in game
# (its process group must be stopped while asleep) and a stand-in banana-ui (it must get SIGUSR1 after a wake).
#
#   sudo system/bananad/tests/sleep-test.sh        (root: it mounts in a private namespace; nothing outside changes)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOTFS=$(cd "$HERE/../rootfs" && pwd)
if [ "$1" != --inside ]; then
    W=$(mktemp -d)
    cc -O2 -o "$W/bananad" "$HERE/bananad.c" -lasound
    cc -O2 -o "$W/banana-ui" -x c - <<'EOF'
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
static void on(int s) { FILE *f = fopen("/run/test/ui-signals", "a"); if (f) { fprintf(f, "SIGUSR1\n"); fclose(f); } }
int main(void) { signal(SIGUSR1, on); signal(SIGHUP, SIG_IGN); for (;;) pause(); }
EOF
    mkdir -p /data 2>/dev/null || true
    exec unshare -m -p -f --mount-proc sh "$0" --inside "$W" "$ROOTFS"
fi

# ---- inside: pid 1 is this shell, so bananad's "shut down" (SIGUSR2 to init) comes here ------------------------
W=$2 ROOTFS=$3
POWEROFF=0
trap 'POWEROFF=1' USR2
mount -t tmpfs none /sys/power
mount -t tmpfs none /sys/class
# The wakeup_sources table bananad reads to name a wake. /sys may be read-only (a runner) and debugfs may already be
# mounted there -- and bananad mounts debugfs itself at start, over whatever is here. So the fake is only set up
# where those cannot happen (a userns without CAP_SYS_ADMIN), and the check that reads it looks for its own row first.
WAKESRC=
mkdir -p /sys/kernel/debug 2>/dev/null && mount -t tmpfs none /sys/kernel/debug 2>/dev/null &&
    : > /sys/kernel/debug/wakeup_sources 2>/dev/null && WAKESRC=/sys/kernel/debug/wakeup_sources
mount -t tmpfs none /dev/input 2>/dev/null || { mkdir -p /dev/input; mount -t tmpfs none /dev/input; }
mount -t tmpfs none /run
mount -t tmpfs none /data
mkdir -p /etc/banana && mount -t tmpfs none /etc/banana && cp -r "$ROOTFS/etc/banana/." /etc/banana/
mkdir -p /run/banana /run/test /data/banana/logs /sys/class/rtc/rtc0 /sys/class/power_supply/battery
echo Battery > /sys/class/power_supply/battery/type
echo Discharging > /sys/class/power_supply/battery/status
echo 50 > /sys/class/power_supply/battery/capacity
: > /sys/class/rtc/rtc0/wakealarm
echo "s2idle [deep]" > /sys/power/mem_sleep
echo 0 > /sys/power/pm_async
for b in backlight0 backlight1; do mkdir -p /sys/class/backlight/$b; echo 255 > /sys/class/backlight/$b/max_brightness; echo 200 > /sys/class/backlight/$b/brightness; echo 0 > /sys/class/backlight/$b/bl_power; done
mkfifo /sys/power/state /dev/input/event0
exec 3<>/dev/input/event0       # a writer that stays, so bananad's poll doesn't see a hang-up

pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
# an input event: type code value (then a SYN_REPORT)
ev() { python3 -c "
import struct, sys
t, c, v = (int(x) for x in sys.argv[1:4])
sys.stdout.buffer.write(struct.pack('llHHi', 0, 0, t, c, v) + struct.pack('llHHi', 0, 0, 0, 0, 0))" "$@" >&3; }
EV_KEY=1 KEY_POWER=116
# press and release power: bananad sleeps on the release
power() { ev $EV_KEY $KEY_POWER 1; sleep 0.1; ev $EV_KEY $KEY_POWER 0; }
# is bananad blocked writing "mem" (asleep)? wait up to 3 s
# (blocked opening the FIFO: its wait channel is wait_for_partner)
asleep() { for i in $(seq 30); do [ "$(cat /proc/$BD/wchan 2>/dev/null)" = wait_for_partner ] && return 0; sleep 0.1; done; return 1; }
# read "mem": the write returns, it is awake. A real power-key wake carries a KEY_POWER press (the spurious-wake
# guard re-sleeps a wake with no key and no cable), so wake() queues one before releasing the FIFO; an RTC wake
# (the battery check's alarm) has no key. 0.4.54 added the guard after this test, which is why wake() has the press.
wake() { ev $EV_KEY $KEY_POWER 1; head -c 3 /sys/power/state > /dev/null; sleep 0.4; }
rtc_wake() { head -c 3 /sys/power/state > /dev/null; sleep 0.4; }
gstate() { awk '{print $3}' /proc/$GAME/stat; }
start() {   # start [bananad.conf lines]
    echo "s2idle [deep]" > /sys/power/mem_sleep     # the kernel's file always lists both
    printf '%s\n' "autosleep_minutes = 0" "sleep_check_hours = 0" "$@" > /data/banana/bananad.conf
    "$W/bananad" & BD=$!
    sleep 0.6
}
stop() { kill $BD 2>/dev/null; wait $BD 2>/dev/null || true; }

"$W/banana-ui" & UI=$!
setsid sleep 600 & GAME=$!
sleep 0.2
echo $GAME > /run/banana/game.pid

echo "1. the power button sleeps and wakes it"
start
# the table as the kernel has it, snapshotted before "mem": the waker is the source whose event_count moves. The
# table's last_change column (what the log used to read) names only mmc0, the SD host, for every wake.
if [ -n "$WAKESRC" ]; then
    printf 'name\tactive_count\tevent_count\twakeup_count\texpire_count\ntest-waker\t0\t0\t0\t0\nmmc0\t0\t0\t0\t0\n' > "$WAKESRC"
fi
power
if asleep; then ok "suspended on the power button"; else bad "no suspend on the power button"; fi
[ "$(gstate)" = T ] && ok "game stopped while asleep" || bad "game not stopped ($(gstate))"
[ "$(cat /sys/power/pm_async)" = 1 ] && ok "devices resume in parallel (pm_async 1)" || bad "pm_async $(cat /sys/power/pm_async)"
grep -q "\[deep\]" /sys/power/mem_sleep && ok "deep sleep chosen" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
grep -q 'going to sleep (power button)' /data/banana/logs/sleep-state && ok "the pre-sleep note was written" || bad "no sleep-state note"
WAKER=0     # the kernel's own table is in its place (a runner with debugfs, or a read-only /sys): nothing to fake
if [ -n "$WAKESRC" ] && grep -q test-waker "$WAKESRC" 2>/dev/null; then
    printf 'name\tactive_count\tevent_count\twakeup_count\texpire_count\ntest-waker\t0\t1\t1\t0\nmmc0\t0\t0\t0\t0\n' > "$WAKESRC"
    WAKER=1
fi
wake
[ "$(gstate)" != T ] && ok "game running again after the wake" || bad "game still stopped"
rm -f /run/banana/game.pid; sleep 0.1
grep -q 'awake (power button)' /data/banana/logs/sleep.log && ok "sleep.log: $(tail -n1 /data/banana/logs/sleep.log | cut -c21-)" || bad "no wake in sleep.log"
if [ $WAKER = 1 ]; then
    grep -q 'wake test-waker +1' /data/banana/logs/sleep.log && ok "the wake names the source whose count moved" || bad "the wake did not name test-waker"
fi
stop

echo "2. the battery guard: an RTC wake with the battery nearly empty shuts down cleanly"
echo $GAME > /run/banana/game.pid
echo 3 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002" "sleep_poweroff_percent = 5"
power
asleep && ok "suspended with the RTC alarm set ($(cat /sys/class/rtc/rtc0/wakealarm))" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake      # the alarm fired (the RTC clears it): the RTC woke it
for i in $(seq 30); do [ $POWEROFF = 1 ] && break; sleep 0.1; done
[ $POWEROFF = 1 ] && ok "shut down cleanly (init asked to power off)" || bad "no shutdown at 3%"
kill -0 $GAME 2>/dev/null && bad "the game wasn't told to quit and save" || ok "the game was told to quit and save first"
grep -q 'battery check: 3%' /data/banana/logs/sleep.log && ok "logged the battery check" || bad "battery check not logged"
stop
POWEROFF=0
setsid sleep 600 & GAME=$!; sleep 0.1; echo $GAME > /run/banana/game.pid

echo "3. an RTC wake with enough battery goes back to sleep"
echo 60 > /sys/class/power_supply/battery/capacity
start "sleep_check_hours = 0.002"
power
asleep && ok "suspended" || bad "no suspend"
: > /sys/class/rtc/rtc0/wakealarm; rtc_wake
if asleep; then ok "back to sleep after the battery check"; else bad "awake after the battery check"; fi
[ "$(gstate)" = T ] && ok "the game stayed stopped" || bad "the game ran during the check"
wake; sleep 0.2
grep -q 'battery checked' /data/banana/logs/sleep.log && ok "logged" || bad "not logged"
stop

echo "4. a suspend that fails: the safer ways, then the panel dark"
rm -f /sys/power/state; : > /sys/power/state; chmod 000 /sys/power/state
# root can write a mode-000 file; a directory can't be written at all
rm -f /sys/power/state; mkdir /sys/power/state
start
power
sleep 1.5
grep -q 'trying the safer way' /data/banana/logs/sleep.log && ok "fell back after the failure" || bad "no fallback"
[ "$(cat /sys/power/mem_sleep)" = s2idle ] && ok "s2idle tried" || bad "mem_sleep $(cat /sys/power/mem_sleep)"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "panel dark after the failed suspend" || bad "backlight bl_power $(cat /sys/class/backlight/backlight0/bl_power)"
stop
rmdir /sys/power/state; mkfifo /sys/power/state

echo "5. idle in the menu: the panel dims, and comes back at the first press"
rm -f /run/banana/game.pid
echo 200 > /sys/class/backlight/backlight0/brightness; echo 200 > /sys/class/backlight/backlight1/brightness
start "dim_minutes = 0.02" "dim_percent = 30"      # 1.2 s
sleep 2.5
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 60 ] && ok "dimmed to 30% after the idle time" || bad "not dimmed: brightness $(cat /sys/class/backlight/backlight1/brightness)"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3         # BTN_SOUTH: any button
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "back to 200 at the press" || bad "still dim: $(cat /sys/class/backlight/backlight1/brightness)"
echo $GAME > /run/banana/game.pid; sleep 2
[ "$(cat /sys/class/backlight/backlight1/brightness)" = 200 ] && ok "never dimmed while a game runs" || bad "dimmed during a game"
rm -f /run/banana/game.pid
stop

echo "6. charging: the idle timer does not suspend while it charges"
mkdir -p /sys/class/power_supply/charger
echo USB > /sys/class/power_supply/charger/type; echo 1 > /sys/class/power_supply/charger/online
start "autosleep_minutes = 0.02"
sleep 2
asleep 2>/dev/null && bad "it suspended while charging" || ok "no idle suspend while charging"
stop
rm -rf /sys/class/power_supply/charger

echo "7. no game: the RTC alarm is set for the battery check, and the wake tells banana-ui to draw again"
rm -f /run/banana/game.pid
rm -f /run/test/ui-signals
start "sleep_check_hours = 4"
power
asleep && ok "suspended" || bad "no suspend"
[ -n "$(cat /sys/class/rtc/rtc0/wakealarm)" ] && ok "an RTC alarm for the battery check set ($(cat /sys/class/rtc/rtc0/wakealarm))" || bad "no RTC alarm set"
wake
sleep 0.4
grep -q SIGUSR1 /run/test/ui-signals 2>/dev/null && ok "banana-ui was told to draw again (SIGUSR1)" || bad "banana-ui was not signalled after the wake"
grep -q 'awake' /data/banana/logs/sleep.log && ok "the wake was logged" || bad "no wake logged"
stop

echo "8. the vibrate trigger: a file under /run/banana pulses it (no FF input here, so a no-op, but it must be consumed)"
rm -f /run/banana/game.pid
start
: > /run/banana/vibrate
sleep 0.4
[ ! -e /run/banana/vibrate ] && ok "the trigger was consumed" || bad "the vibrate trigger was not consumed"
power
asleep && ok "still sleeps after a pulse" || bad "no suspend after a pulse"; wake
stop

echo "9. sleep_skip: a skipped SIGSTOP leaves the game running through the sleep (the debug bisect)"
kill -CONT $GAME 2>/dev/null || true
echo $GAME > /run/banana/game.pid
start "sleep_check_hours = 0" "sleep_skip = stop"
power
asleep && ok "suspended with the stop skipped" || bad "no suspend"
[ "$(gstate)" != T ] && ok "the game kept running (stop skipped)" || bad "the game was stopped anyway"
wake; sleep 0.2
grep -q "skipping 'stop'" /data/banana/logs/sleep.log && ok "logged the skipped step" || bad "the skip was not logged"
stop

echo "10. no RTC wakealarm (the RPC's ADSP-owned alarm): a low discharging battery refuses to sleep"
rm -f /sys/class/rtc/rtc0/wakealarm
echo Discharging > /sys/class/power_supply/battery/status
echo 10 > /sys/class/power_supply/battery/capacity
rm -f /run/banana/game.pid
start "sleep_floor_percent = 15"
power
sleep 0.5
asleep 2>/dev/null && bad "it slept a low battery with no wake source" || ok "refused to sleep the low battery"
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 4 ] && ok "the panel went dark instead" || bad "panel not dark ($(cat /sys/class/backlight/backlight0/bl_power))"
grep -q 'no RTC wake alarm' /data/banana/logs/sleep.log && ok "logged the refusal" || bad "refusal not logged"
ev $EV_KEY 304 1; ev $EV_KEY 304 0; sleep 0.3      # BTN_SOUTH: the SoC never slept, so a press lights it again
[ "$(cat /sys/class/backlight/backlight0/bl_power)" = 0 ] && ok "a press brings the panel back" || bad "panel stayed dark"
stop

kill $UI $GAME 2>/dev/null || true
echo "sleep test: $pass passed, $fail failed"
[ $fail = 0 ]
