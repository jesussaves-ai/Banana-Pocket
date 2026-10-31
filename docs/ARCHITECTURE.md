# BananaOS Architecture (RPC / SM4450)

## 1. Storage & A/B Update
- **Bootloader:** OpenABL (`arm-efi` in `abl_a`/`b`; no U-Boot/initramfs). Reads `p1:extlinux/extlinux.conf` (`root=PARTLABEL=banana-root-a|b rootfstype=squashfs ro rootwait quiet`).
- **Partitions:**
  - `p1` (`BANANA-BOOT`, FAT, 160MB): `a|b/{Image,<board>.dtb}`, `extlinux/extlinux.conf`.
  - `p2`/`p3` (`banana-root-a|b`, squashfs LZ4, 256MB): Active/inactive RO rootfs slots.
  - `p4` (`BANANA`, exFAT, `/data`, init via `/usr/libexec/banana/firstboot`): `roms/`, `bios/`, `saves/`, `states/`, `banana/{settings.conf,bananad.conf,accounts.conf,wifi.conf,asound.state,setup-done,logs/,updates/,ssh/}`.
- **Update (`/usr/libexec/banana/update apply`):** Unpacks `/data/banana/updates/banana-<board>-<version>.update.tar`. Checks `board` & `sha256sums` -> streams `rootfs.squashfs` (16MB chunks) to inactive slot -> verifies read-back hash -> populates `a/` or `b/` boot slot -> atomically renames `extlinux.conf`. Logs: `banana/logs/updates.log`.

## 2. Boot & Init
- **Built-in Drivers:** CH13726A DSI (`rotation = <90>`), `rsinput` (MCU), FT5452 touch, PMIC/`qcom_battmgr`, `pwm_fan`. No fbcon/VTs/udev.
- **Modules:** Audio (`sc8280xp`/q6apm/LPASS/SoundWire, WCD937x, AW88xx), GPU (`msm` a613 + Freedreno, post-first UI frame), Wi-Fi (`ath10k_snoc` WCN3950), SMB3 (`ksmbd` on toggle), USB drivers (e.g., `xpad` via `bananad` netlink `MODALIAS`).
- **Init (`busybox init` -> `/etc/init.d/rcS`):** Mounts `/proc /sys /run /tmp /data` -> loads audio -> starts `bananad` -> sets `banana-perf` clocks -> launches `banana-ui` (plays boot anim unless `splash = off`; bg worker scans library) -> starts `usb-gadget` in bg.

## 3. UI Engine (`banana-ui`)
- **Core (`ui/rnds`):** Imported via `tools/import-rnds.sh` (avoid divergence). Canvas: 640x557 scaled 1.9375x to 1240x1080 (`themes/pocket.json`).
- **Display (`src/kms.cpp`):** `Display::openSingle()`, 2 KMS dumb buffers, vblank flip, `lease()` refused. Composes in cached RAM; `Display::turn()` copies dirty rects to uncached scanout via 4x4 register transpose (90°).
- **Renderer (`src/soft.cpp`):** CPU rasterizer. Per-rect draw-list diffing (redraws clipped/changed regions). Textures: transparent/opaque (`memcpy`)/blended (4-px SIMD-style edge blend) runs + cached resampled/sub-pixel phases.
- **Loop & Workers (`src/main.cpp`, `src/library.cpp`):** 3 niced threads decode/fit PNGs lock-free. Main blocks in `poll()` (input, touch, DRM flip, workers, signals, anim/clock tick). ROM dir watch: `inotify` (2s home debounce).
- **Apps shelf (`src/library.cpp`, `Library::scanApps`):** scans `/usr/share/banana/apps`, `/data/apps` and `/data/banana/apps` for a folder with a manifest (`app.json`, `app.conf`, or a freedesktop `*.desktop`); each becomes an app system after `systems.conf`'s, one tile on the Apps shelf (the theme's `[app]` mark, else `r.app`). The `banana-tube` app ships this way from `buildroot/package/banana-tube` (manifest + marker under `/usr/share/banana/apps/bananatube`), its launcher `run-bananatube` under cage with the other apps.
- **Settings (`src/settings*.cpp`):** Merges `/etc/banana/settings.conf` -> `/data/banana/settings.conf`. Live-writes `settings.conf`, `bananad.conf` (+`SIGHUP`), `retroarch.cfg`, POSIX `TZ`.
- **Game Handover (`cage`):** Writes `/run/banana/game.pid` -> releases DRM/input/ALSA (`Sound::release()`) -> sets clock profile -> runs game in `cage` (Wayland kiosk, DRM master, applies panel orientation for RetroArch/OSD) -> reclaims panel on exit.

## 4. System Daemon & Power (`bananad`)
- **Input/Hotkeys:** Non-grabbing `evdev` + netlink `power_supply` (PWM LEDs). Slate (`EVIOCGBIT(EV_SW)`: power + jack, NO `SW_LID`).
  - *Short Power:* Sleep (unless `/run/banana/screen-off` exists; then `banana-ui` takes `KEY_POWER` as `Key::WAKE`).
  - *Hold Power (2s) / <=3% batt:* Clean shutdown.
  - *Vol / F+Vol:* HW gain (`/usr/libexec/banana/volume`) / PWM backlight.
- **Idle/Burn-in:** Dims at 2m (`dim_minutes`/`dim_percent`), blanks at 90s/180s (writes `/run/banana/screen-off`, calls `Sound::setScreenOn(false)`). Disabled in-game. Auto-suspend ONLY if `autosleep_minutes` set in `bananad.conf`.
- **Suspend/Resume (`deep` -> `s2idle` -> backlight fallback):**
  1. `SIGSTOP` `/run/banana/game.pid` pgrp.
  2. `/etc/banana/sleep.d/* pre` (`10-wifi` downs link/supplicant).
  3. `sync()` -> enable `pm_async`/`fast_resume` -> write `deep` to `/sys/power/mem_sleep`, `mem` to `/sys/power/state`. *(RPC PMIC RTC is `qcom,no-alarm`: `wakealarm`/`sleep_check_hours` are no-ops; wakes ONLY on Power key or USB-C).*
  4. Wake: `SIGCONT` game, `SIGUSR1` `banana-ui` (instant redraw, no modeset), swallow wake key, bg `sleep.d/* post`, restore LEDs & audio route. Logs: `banana/logs/sleep.log` (+`sleep-devices.log` if `sleep_debug = yes`).

## 5. Audio (Qualcomm AudioReach)
- **Stack:** `sc8280xp`/q6apm DSP + WCD937x + dual AW88166 I2S amps. No sound server/UCM. Config: `/run/banana/asound.conf`.
- **Rules:**
  1. *Pre-open routing:* DSP latches graph at PCM open. `bananad` writes `/run/banana/audio-route`; `banana-ui` (via ALSA C mixer API, NEVER fork `amixer`) and `run-retroarch` apply route *before* PCM open.
  2. *Two frontends:* the menu keeps `MultiMedia1`; games open **`MultiMedia2`** (`hw:0,1`) through their own `$HOME/.asoundrc` — the frontend `banana-ui` never opens or releases, so a game's route stays latched (a game that shared `MultiMedia1` latched the route off once the menu had released and reopened it across a game). `audio-handoff` waits out the menu's teardown and re-asserts the route; the **first game after a boot** gets one extra settle (that teardown is the slowest, keyed off a tmpfs marker shared by both launchers). `run-retroarch` bounds a stalled core teardown with a ~1 s watchdog so the menu always returns.
  3. *Volume:* Stream vol latches at open; control AW88166 HW gain (`Left PCM`/`Right PCM` `0..511`, mapped in `/run/banana/mixer`, persisted in `/data/banana/asound.state`). RetroArch SW gain = unity.

## 6. Network & Services
- **Wi-Fi:** `wpa_supplicant` + `wpa_cli` scan thread (`/data/banana/wifi.conf`, WPA3 `key_mgmt=SAE`). `udhcpc` via `wpa_cli` action on connect. Disconnected scan backoff: 5m.
- **SMB3:** `ksmbd` + `ksmbd.mountd` (`\\<ip>\banana` -> `/data`, guest=root, no DOS/ext attrs, default off).
- **SSH:** `dropbear` (key-only, default off; copies `/data/banana/ssh/authorized_keys` -> `/run/banana/ssh` `0600`).

## 7. Perf Profiles, Testing & CI
- **Clocks (`banana-perf`):** Applies to BOTH `policy0` (6xA55) & `policy6` (2xA78): `ui` (`schedutil` ~1.4GHz), `powersave` (`schedutil`), `balanced` (`schedutil` full), `performance` (`performance`).
- **Tests & Harnesses:**
  - `make -s test`: Runs `tools/check-systems.py` (validates `systems.conf` -> launchers/Buildroot cores), `system/tests/run-retroarch-test.sh` (per-core defaults/cfg/RA handoff, rootless), `system/tests/rpc-thermals-test.sh` (mock `/sys` vs `tools/rpc-thermals.sh`), and `ui/tests/run.sh` (32x24 grey PNG diffs in `ui/tests/golden/`; `UPDATE_GOLDEN=1` rewrites).
  - `make -s bench`: Headless 600-game UI benchmark (vs `docs/PERFORMANCE.md`).
  - `system/bananad/tests/sleep-test.sh`: Namespace-isolated mock `/sys` suspend test.
  - `tools/qemu-boot.sh`: QEMU `virt` boot (`Virtual-1`, virtio input/GPU, tests A/B update, logs RAM/CPU to `resources-*.txt`).
  - **CI (`.github/workflows/ci.yml`):** `checks` (host tests, native+aarch64 `bananad`, `sleep-test`, launcher `sh -n`, defconfig check), `cores` (change-gated Buildroot `libretro-*` builds), `image`/`boot` (manual dispatch).
- **Board Pkg:** `buildroot/board/retroid-pocket-classic` (ROCKNIX SM4450 patches, DTs, `linux-banana.fragment`, audio topology, OpenABL).

## 8. USB Dev Link & Screen Capture
- **Gadget (`/usr/libexec/banana/usb-gadget`):** Mounts `configfs`, sets up USB NCM Ethernet (`WINNCM` MS OS descriptor, built-in kernel `CONFIG_USB_GADGET`/`NCM=y`, DT `dr_mode="otg"` + `role-switch-default-mode="peripheral"`). Handheld = `169.254.170.2/16`, host = `169.254.170.1` via 1-lease `udhcpd` (~4ms RTT, ~7MB/s SSH). `usb-gadget down` tears down. *(Note: Type-C `data_role` always reads `host`; actual USB role switch governs device mode. Re-seat cable if bound disconnected; OTG gamepad unbinds gadget).*
- **Screen Capture (`tools/rpc-screen.sh`):** Touches `/run/banana/screen.req` (caught by `banana-ui` existing `/run/banana` `inotify` watch) -> `banana-ui` dumps pre-rotated `be.pixels(0)` to `/run/banana/screen.jpg` (`stbi_write_jpg`). `-w` streams MJPEG (`ffplay`/`mpv`). Menu-only (in-game uses RetroArch `SCREENSHOT` FIFO cmd).
