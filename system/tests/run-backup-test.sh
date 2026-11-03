#!/bin/sh
# backup: the handheld's settings and saves in one file, and back (system/rootfs/usr/libexec/banana/backup), run on
# the host against a fake card root (BANANA_DATA). It checks what goes into the archive, that a restore brings deleted
# files back, and that a hostile archive cannot write outside the card's data.
#   system/tests/run-backup-test.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)		# system/
ROOT=$(cd "$HERE/.." && pwd)
TOOL="$ROOT/system/rootfs/usr/libexec/banana/backup"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "  ok: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL: $*"; }
run() { BANANA_DATA=$D "$TOOL" "$@"; }

mkdir -p "$D/banana/logs" "$D/banana/updates" "$D/saves" "$D/states" "$D/retroarch/remaps/Flycast" "$D/retroarch/config/Flycast"
echo 'theme = "dark"' > "$D/banana/settings.conf"
echo 'autosleep_minutes = 2' > "$D/banana/bananad.conf"
printf 'ssid = "x"\n' > "$D/banana/wifi.conf"
printf 'ra_user = "u"\n' > "$D/banana/accounts.conf"
printf 'save' > "$D/saves/a.srm"
printf 'state' > "$D/states/b.state"
printf 'input_player1_btn_l = "4"\n' > "$D/retroarch/remaps/Flycast/Flycast.rmp"
printf 'per-core option' > "$D/retroarch/config/Flycast/Flycast.opt"
printf 'junk' > "$D/banana/logs/junk.log"
printf 'pkg' > "$D/banana/updates/big.tar"

echo "1. create: one file with the settings and the saves, nothing else"
out=$(run create 2>&1) || bad "create failed: $out"
name=$(printf '%s\n' "$out" | sed -n 's/^ok //p')
[ -n "$name" ] && [ -f "$D/banana/backups/$name" ] && ok "created $name" || bad "no file (output: $out)"
list=$(tar -tzf "$D/banana/backups/$name" 2>/dev/null)
for m in banana/settings.conf banana/bananad.conf banana/wifi.conf banana/accounts.conf saves/a.srm states/b.state retroarch/remaps/Flycast/Flycast.rmp; do
    printf '%s\n' "$list" | grep -qx "$m" && ok "archive has $m" || bad "archive is missing $m"
done
for m in banana/logs/junk.log banana/updates/big.tar retroarch/config/Flycast/Flycast.opt; do
    printf '%s\n' "$list" | grep -qx "$m" && bad "archive took $m" || ok "archive leaves out $m"
done
printf '%s\n' "$list" | grep -q '^banana/backups/' && bad "the backup took itself" || ok "the backup is not in itself"

echo "2. list: the newest first"
first=$(run list | head -n 1 | cut -f1)
[ "$first" = "$name" ] && ok "list names it" || bad "list says '$first'"
sleep 1
second=$(run create | sed -n 's/^ok //p')
[ "$(run list | head -n 1 | cut -f1)" = "$second" ] && ok "a newer backup sorts first" || bad "list order"

echo "3. restore: a deleted settings file and save come back"
rm -f "$D/banana/settings.conf" "$D/saves/a.srm" "$D/retroarch/remaps/Flycast/Flycast.rmp"
rm -rf "$D/states"
out=$(run restore "$D/banana/backups/$name" 2>&1) || bad "restore failed: $out"
grep -q 'theme = "dark"' "$D/banana/settings.conf" 2>/dev/null && ok "settings.conf is back" || bad "settings.conf is not back"
[ "$(cat "$D/saves/a.srm" 2>/dev/null)" = save ] && ok "the save is back" || bad "the save is not back"
[ -f "$D/states/b.state" ] && ok "the state tree is back" || bad "the state tree is not back"
[ "$(cat "$D/retroarch/remaps/Flycast/Flycast.rmp" 2>/dev/null)" = 'input_player1_btn_l = "4"' ] && ok "the controller remap is back" || bad "the controller remap is not back"

echo "4. restore with no argument takes the newest"
rm -f "$D/banana/wifi.conf"
run restore >/dev/null 2>&1
[ -f "$D/banana/wifi.conf" ] && ok "newest restored" || bad "no-argument restore did nothing"

echo "5. a hostile archive is refused and nothing is written outside the root"
printf 'pwn' > "$D/pwn-$$"
( cd "$D" && tar -czf hostile.tar.gz --transform='s|^|../|' "pwn-$$" ) 2>/dev/null
out=$(run restore "$D/hostile.tar.gz" 2>&1); rc=$?
[ $rc -ne 0 ] && ok "refused ($out)" || bad "a ../ member was accepted"
[ ! -e "$D/../pwn-$$" ] && ok "nothing written outside the root" || { bad "wrote outside the root"; rm -f "$D/../pwn-$$"; }

echo "6. a card with no accounts.conf still backs up"
rm -f "$D/banana/accounts.conf"
out=$(run create 2>&1) && ok "create without accounts.conf" || bad "create needs accounts.conf: $out"

echo
if [ "$fail" -eq 0 ]; then echo "backup test: all $pass checks passed"; exit 0; fi
echo "backup test: $fail failed, $pass passed"; exit 1
