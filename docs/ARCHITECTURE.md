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
- **Game Handover (`cage`):** Writes `/run/banana/game.pid` -> releases DRM/input and its PipeWire stream (`Sound::release()`) -> sets clock profile -> runs game in `cage` (Wayland kiosk, DRM master, applies panel orientation for RetroArch/OSD) -> reclaims panel on exit.

## 4. System Daemon & Power (`bananad`)
- **Input/Hotkeys:** Non-grabbing `evdev` + netlink `power_supply`. Slate (`EVIOCGBIT(EV_SW)`: power + jack, NO `SW_LID`, no LEDs).
  - *Short Power:* Sleep (unless `/run/banana/screen-off` exists; then `banana-ui` takes `KEY_POWER` as `Key::WAKE`).
  - *Hold Power (2s) / <=3% batt:* Clean shutdown.
  - *Vol / F+Vol:* HW gain (`/usr/libexec/banana/volume`) / PWM backlight.
- **Idle/Burn-in:** Dims at 2m (`dim_minutes`/`dim_percent`), blanks at 90s/180s (writes `/run/banana/screen-off`, calls `Sound::setScreenOn(false)`). Disabled in-game. Auto-suspend ONLY if `autosleep_minutes` set in `bananad.conf`.
- **Suspend/Resume (`deep` -> `s2idle` -> backlight fallback):**
  1. `SIGSTOP` `/run/banana/game.pid` pgrp.
  2. `/etc/banana/sleep.d/* pre` (`10-wifi` downs link/supplicant).
  3. `sync()` -> enable `pm_async` -> write `deep` to `/sys/power/mem_sleep`, `mem` to `/sys/power/state`. *(RPC PMIC RTC is `qcom,no-alarm` -- the alarm IRQ is the ADSP's, so `wakealarm`/`sleep_check_hours` are no-ops and it wakes ONLY on Power key or USB-C. A discharging battery at/under `sleep_floor_percent` therefore refuses to sleep: the panel goes dark but the SoC stays awake, so the 60 s poll can still shut down cleanly.)*
  4. Wake: `SIGCONT` game, `SIGUSR1` `banana-ui` (instant redraw, no modeset), swallow wake key, bg `sleep.d/* post`, re-read the jack (the loop's deferred settle; WirePlumber switches the sink). Logs: `banana/logs/sleep.log` (+`sleep-devices.log` if `sleep_debug = yes`).

## 5. Audio (Qualcomm AudioReach)
- **Stack:** `sc8280xp`/q6apm DSP + WCD937x + one **mono** AW88166 I2S amp (`0-0035`, on FL). **PipeWire +
  WirePlumber** is the sound server: ACP brings the card up from the RPC's UCM2 profile (`/usr/share/alsa/ucm2`), and
  bananad writes `/run/banana/asound.conf` (`/etc/asound.conf` points at it) pinning `pcm.!default`/`ctl.!default` to
  `type pipewire` — every client (the menu, RetroArch, BananaTube) is a PipeWire client and mixes through it. No
  PulseAudio, no DBus.
- **The q6apm rules (why the server needs the help):**
  1. *The FE<->BE switch only takes a write while that front-end's graph is closed, and the DSP clears it at graph
     tear-down.* ACP runs its UCM sequences once, before its probe's open/close tear-down, so the held-open sink
     otherwise latched the topology's "off" (silent boot). PipeWire re-applies the card's UCM verb in the node open
     path immediately before `snd_pcm_open` (`buildroot/patches/pipewire/0002`). Shared switches live only in the
     verb's `EnableSequence` — never a `SectionDevice` (`AGENTS.md` §2).
  2. *The sink must not idle-suspend* (`session.suspend-timeout-seconds = 0`): it stays `RUNNING`, so the graph and
     its route are never torn down between cues and games. q6apm-safe buffering (`api.alsa.period-num`/`headroom`) is
     set on the sink with a `monitor.alsa.rules` node match — `monitor.alsa.properties` never reaches the ACP nodes.
  3. *Jack:* the UCM `Headphones` device's `JackControl "Headphone Jack"` (+ `JackHWMute "Speaker"`) drives the switch
     and WirePlumber picks the active sink. Nothing writes `hw:0` mixers by hand any more.
- **Volume:** the heard volume is the AW88166 HW gain (`Left PCM`/`Right PCM`, `0..511`, mapped in
  `/run/banana/mixer`, persisted in `/data/banana/asound.state`), which bananad's volume keys drive through the ALSA
  C mixer API (NEVER fork `amixer`); RetroArch SW gain = unity.

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
- **Gadget (`/usr/libexec/banana/usb-gadget`):** Mounts `configfs`, sets up USB NCM Ethernet (`WINNCM` MS OS descriptor, built-in kernel `CONFIG_USB_GADGET`/`NCM=y`, DT `dr_mode="otg"` + `role-switch-default-mode="peripheral"`). Handheld = `169.254.170.2/16`, host = `169.254.170.1` via 1-lease `udhcpd` (~4ms RTT, ~7MB/s SSH). `usb-gadget down` tears down. *(Note: the port charges but doesn't enumerate when the Type-C cable is in the reverse orientation — `pmic-glink` doesn't drive the data-lane mux here, so `orientation=reverse` leaves the UDC `not attached`; flip the connector if a bound gadget gives no interface. An OTG gamepad also unbinds the gadget).*
- **Screen Capture (`tools/rpc-screen.sh`):** Touches `/run/banana/screen.req` (caught by `banana-ui` existing `/run/banana` `inotify` watch) -> `banana-ui` dumps pre-rotated `be.pixels(0)` to `/run/banana/screen.jpg` (`stbi_write_jpg`). `-w` streams MJPEG (`ffplay`/`mpv`). Menu-only (in-game uses RetroArch `SCREENSHOT` FIFO cmd).
