#!/bin/sh
# rpc-usb-trace: what the USB gadget does across a plug/unplug, on the handheld. Reads the kernel's own words
# (usbcore, gadget, ucsi/pmic_glink, dwc3, xhci) and the gadget's state while you plug the cable in and out, so an
# intermittent disconnect can be told apart: a Type-C role switch (udc state goes to "not attached" and back), an
# adbd restart (a fresh "adbd exited" in usb-adb.log), or a link-speed problem (current_speed changes).
#
#   sh /data/rpc-usb-trace.sh [seconds]      (default 30) -> /data/banana/logs/usb-trace.log
#
# Push it over adb or the NCM link, then watch the log on the computer:
#   adb push tools/rpc-usb-trace.sh /data/ ; adb shell sh /data/rpc-usb-trace.sh 40
#   scp -O root@169.254.170.2:/data/banana/logs/usb-trace.log .
SECS=${1:-30}
G=/sys/kernel/config/usb_gadget/banana
OUT=/data/banana/logs/usb-trace.log
mkdir -p /data/banana/logs 2>/dev/null

{
echo "usb-trace: $(date 2>/dev/null) for ${SECS}s"
echo "--- cmdline: $(cat /proc/cmdline 2>/dev/null)"
echo "--- udc: $(cat /sys/class/udc/*/state 2>/dev/null)  speed: $(cat /sys/class/udc/*/current_speed 2>/dev/null)"
echo "--- plug the cable in and out now; the state is once a second"
i=0
while [ "$i" -lt "$SECS" ]; do
    printf '%4d s  state=%-14s speed=%-12s gadget=%s\n' "$i" \
        "$(cat /sys/class/udc/*/state 2>/dev/null)" \
        "$(cat /sys/class/udc/*/current_speed 2>/dev/null)" \
        "$(cat $G/UDC 2>/dev/null)"
    sleep 1
    i=$((i + 1))
done
echo "--- dmesg (usb/typec lines, last 80)"
dmesg 2>/dev/null | grep -iE 'usb|udc|dwc3|ucsi|pmic.?glink|typec|xhci|gadget' | tail -n 80
echo "--- usb-gadget.log (tail)"
tail -n 40 /data/banana/logs/usb-gadget.log 2>/dev/null
echo "--- usb-adb.log (tail)"
tail -n 40 /data/banana/logs/usb-adb.log 2>/dev/null
} > "$OUT" 2>&1
echo "wrote $OUT"
