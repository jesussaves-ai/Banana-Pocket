# BananaOS Architecture (RPC / SM4450)

## 1. Storage & A/B Update
- **Bootloader:** OpenABL (`arm-efi` in `abl_a`/`b`; no U-Boot/initramfs). Reads `p1:extlinux/extlinux.conf` (`root=PARTLABEL=banana-root-a|b rootfstype=squashfs ro rootwait quiet`).
- **Partitions:**
  - `p1` (`BANANA-BOOT`, FAT, 160MB): `a|b/{Image,<board>.dtb}`, `extlinux/extlinux.conf`.
  - `p2`/`p3` (`banana-root-a|b`, RO squashfs LZ4, 256MB): Active/inactive rootfs slots.
  - `p4` (`BANANA`, RW exFAT, `/data`, init via `/usr/libexec/banana/firstboot`): `roms/`, `bios/`, `saves/`, `states/`, `banana/{settings.conf,bananad.conf,accounts.conf,wifi.conf,asound.state,setup-done,logs/,updates/,ssh/}`.
- **Update (`/usr/libexec/banana/update apply`):** Unpacks `/data/banana/updates/banana-<board>-<version>.update.tar` -> verifies `board` & `sha256sums` -> streams `rootfs.squashfs` (16MB chunks) to inactive slot -> verifies read-back SHA256 -> populates `a/` or `b/` boot slot -> atomic `mv` on `extlinux.conf`. Logs: `/data/banana/logs/updates.log`.

## 2. Boot & Init
- **Built-in Drivers:** `CH13726A` DSI (`rotation = <90>`), `rsinput` (MCU gamepad), `FT5452` touch, PMIC/`qcom_battmgr`, `pwm_fan`. Zero `fbcon`/VTs/`udev`.
- **Modules:** Audio (`sc8280xp`/q6apm/LPASS/SoundWire, `WCD937x`, `AW88xx`), GPU (`msm` Adreno `a613` + Freedreno, loaded post-first UI frame), Wi-Fi (`ath10k_snoc` WCN3950), SMB3 (`ksmbd` on toggle), USB hotplug (`xpad` via `bananad` netlink `MODALIAS`).
- **Init (`busybox init` -> `/etc/init.d/rcS`):** Mounts `/proc /sys /run /tmp /data` -> loads audio modules -> starts `bananad` -> sets `banana-perf` clocks -> launches `banana-ui` (plays boot anim unless `splash = off`; bg worker scans library) -> starts `usb-gadget` in bg.

## 3. UI Engine (`banana-ui`)
- **Core (`ui/rnds`):** Imported via `tools/import-rnds.sh` (preserve parity). Logical canvas: `640x557` scaled `1.9375x` to `1240x1080` (`themes/pocket.json`).
- **Display (`ui/src/kms.cpp`):** `Display::openSingle()`, 2 KMS dumb buffers, vblank flip, `lease()` refused. Renders in cached RAM; `Display::turn()` copies dirty rects to uncached scanout via 4x4 register transpose (90° CW).
- **Renderer (`ui/src/soft.cpp`):** CPU rasterizer with per-rect draw-list diffing (redraws clipped/changed regions only). Textures: transparent/opaque (`memcpy`)/blended (4-px SIMD-style edge blend) runs + cached resampled/sub-pixel phases.
- **Loop & Workers (`ui/src/main.cpp`, `ui/src/library.cpp`):** 3 niced threads decode/fit PNGs lock-free. Main thread blocks exclusively in `poll()` (evdev, touch, DRM flip, workers, signals, anim/clock tick). ROM watch: `inotify` (2s home debounce).
- **Apps Shelf (`ui/src/library.cpp`, `Library::scanApps`):** Scans `/usr/share/banana/apps`, `/data/apps`, `/data/banana/apps` for folders containing `app.json`, `app.conf`, or `*.desktop`; appends after `systems.conf` systems as tiles on Apps shelf (`[app]` theme mark or `r.app`). `banana-tube` ships via `buildroot/package/banana-tube` (`/usr/share/banana/apps/bananatube`, launched via `run-bananatube` under `cage`).
- **Settings (`ui/src/settings*.cpp`):** Merges `/etc/banana/settings.conf` -> `/data/banana/settings.conf`. Live-writes `settings.conf`, `bananad.conf` (+`SIGHUP`), `retroarch.cfg`, POSIX `TZ`.
- **Game Handover (`cage`):** Writes `/run/banana/game.pid` -> drops DRM master/input + PipeWire stream (`Sound::release()`) -> sets game clock profile -> runs target in `cage` (Wayland kiosk, DRM master, applies panel orientation for RetroArch/OSD) -> reclaims DRM/input/audio on exit.

## 4. System Daemon & Power (`bananad`)
- **Input/Hotkeys:** Non-grabbing `evdev` + netlink `power_supply`. Slate capabilities (`EVIOCGBIT(EV_SW)`: power + headphone jack; NO `SW_LID`, no LEDs).
  - *Short Power:* Suspend (unless `/run/banana/screen-off` exists -> `banana-ui` consumes `KEY_POWER` as `Key::WAKE`).
  - *Hold Power (2s) / `<= 3%` battery:* Clean shutdown.
  - *Vol / F+Vol:* HW gain (`/usr/libexec/banana/volume`) / PWM backlight.
- **Idle/Burn-in:** Dims at 2m (`dim_minutes`/`dim_percent`), blanks at 90s/180s (writes `/run/banana/screen-off`, calls `Sound::setScreenOn(false)`). Disabled while `/run/banana/game.pid` active. Auto-suspend ONLY if `autosleep_minutes` set in `bananad.conf`.
- **Suspend/Resume (`deep` -> `s2idle` -> backlight fallback):**
  1. `SIGSTOP` `/run/banana/game.pid` pgrp.
  2. `SIGHUP` `banana-ui` (`Sound::release`, only with no game) and **wait for the ack**: `/run/banana/sound-state` must stop saying `open`/`releasing` (it publishes each step), bounded at the menu's own 2 s — a PCM close still in flight when the DSP goes under is the one thing the 03:40 overnight hang was seen doing. Logged: `menu: released the sound device in N ms`.
  3. Run `/etc/banana/sleep.d/* pre` (`10-wifi` downs link/supplicant).
  4. `sync()` -> enable `pm_async` -> write `deep` to `/sys/power/mem_sleep`, `mem` to `/sys/power/state`. *(Hardware invariant: RPC PMIC RTC is `qcom,no-alarm`—alarm IRQ belongs to ADSP; `wakealarm`/`sleep_check_hours` are no-ops; wakes ONLY on Power key or USB-C plug. Discharging battery `<= sleep_floor_percent` refuses suspend: blanks panel while SoC stays awake so 60s poll can shut down cleanly at `<= 3%`).*
  5. Wake: `SIGCONT` game pgrp, `SIGUSR1` `banana-ui` (instant redraw, no modeset), swallow wake key, bg `sleep.d/* post`, deferred jack re-read (WirePlumber switches sink). Logs: `/data/banana/logs/sleep.log` (`wake: power key|cable (kernel wakeup_count +n)`: the kernel cannot name the waker here — `pm_wakeup_irq` is absent and every `wakeup_sources` counter stays 0 through a real key wake, so the old `wake mmc0` was the table's `last_change`, i.e. whoever resumed first; +`sleep-devices.log` if `sleep_debug = yes`). `logs/sleep-state` holds what the handheld looked like just before `mem` (mode, the menu's `sound-state`, armed-source count) plus the kernel's PM lines after a wake — the only survivor of a resume that never came back.

## 5. Audio (Qualcomm AudioReach)
- **Stack:** `sc8280xp`/q6apm DSP + `WCD937x` codec + single **mono** `AW88166` I2S amp (`0-0035` on FL). **PipeWire + WirePlumber** sound server: ACP initializes card via UCM2 (`/usr/share/alsa/ucm2`); `bananad` writes `/run/banana/asound.conf` (symlinked from `/etc/asound.conf`) pinning `pcm.!default`/`ctl.!default` to `type pipewire`. All clients (`banana-ui`, RetroArch, BananaTube) mix through PipeWire. Zero PulseAudio, zero DBus.
- **q6apm DSP Invariants:**
  1. *Closed-Graph Routing:* FE<->BE switches only accept writes while that FE's graph is closed; DSP clears them on teardown. PipeWire re-applies the active UCM verb in the node open path immediately before `snd_pcm_open` (`buildroot/patches/pipewire/0002`). Shared FE/BE switches MUST reside only in `SectionVerb` `EnableSequence` (never `SectionDevice`).
  2. *No Idle-Suspend:* Sink sets `session.suspend-timeout-seconds = 0` (stays `RUNNING` permanently across UI cues and games). q6apm buffering (`api.alsa.period-num`/`headroom`) is applied via `monitor.alsa.rules` node match (`monitor.alsa.properties` does not reach ACP nodes).
  3. *Jack Routing:* UCM `Headphones` device `JackControl "Headphone Jack"` (+ `JackHWMute "Speaker"`) drives switching via WirePlumber. Zero manual `hw:0` route writes.
- **Tests:** the chain fails silently, so it has two: `make test` → `system/tests/sound-config-test.sh` (the rules above, plus the menu's client laws, no hardware), and `make sound-test` → `/usr/libexec/banana/sound-test` on the handheld (server, `default`, Speaker sink *and* default, AW88166 gain, frontend `RUNNING`/owned by pipewire, a real client stream, and the menu's `SIGHUP`→`closed`/`SIGUSR1`→`open` sleep handover). See `docs/SOUND-TEST.md`.
- **Volume:** AW88166 HW gain (`Left PCM`/`Right PCM`, `0..511`, cached in `/run/banana/mixer`, persisted in `/data/banana/asound.state`), driven by `bananad` via ALSA C mixer API (`snd_mixer_*`; NEVER fork `amixer`). RetroArch SW gain = unity (`1.0`).

## 6. Network & Services
- **Wi-Fi:** `wpa_supplicant` + `wpa_cli` scan thread (`/data/banana/wifi.conf`, WPA3 `key_mgmt=SAE`). `udhcpc` triggered via `wpa_cli` action on connect. Disconnected scan backoff: 5m.
- **SMB3:** `ksmbd` + `ksmbd.mountd` (`/` + `/<ip>/banana` -> `/data`, `guest=root`, no DOS/ext attrs, default off).
- **SSH:** `dropbear` (key-only, default off; copies `/data/banana/ssh/authorized_keys` -> `/run/banana/ssh` `0600`).

## 7. Perf Profiles, Testing & CI
- **Clocks (`banana-perf`):** Sets BOTH `policy0` (6xA55) & `policy6` (2xA78): `ui` (`schedutil` ~1.4GHz), `powersave` (`schedutil`), `balanced` (`schedutil` full), `performance` (`performance`).
- **Test Suites:**
  - `make -s test`: Runs `tools/check-systems.py` (`systems.conf` -> launchers/Buildroot cores), `system/tests/run-retroarch-test.sh` (per-core defaults/cfg/RA handoff, rootless), `system/tests/rpc-thermals-test.sh` (mock `/sys` vs `tools/rpc-thermals.sh`), `ui/tests/run.sh` (32x24 grey PNG diffs in `ui/tests/golden/`; `UPDATE_GOLDEN=1` rewrites).
  - `make -s bench`: Headless 600-game UI benchmark (vs `docs/PERFORMANCE.md`).
  - `system/bananad/tests/sleep-test.sh`: Namespace-isolated mock `/sys` suspend test.
  - `tools/qemu-boot.sh`: QEMU `virt` boot (`Virtual-1`, virtio input/GPU, tests A/B update, logs RAM/CPU to `resources-*.txt`).
  - **CI (`.github/workflows/ci.yml`):** `checks` (host tests, native+aarch64 `bananad`, `sleep-test`, launcher `sh -n`, defconfig check), `cores` (change-gated Buildroot `libretro-*`), `image`/`boot` (manual dispatch).
- **Board Pkg:** `buildroot/board/retroid-pocket-classic` (ROCKNIX SM4450 patches, DTs, `linux-banana.fragment`, audio topology, OpenABL).

## 8. USB Dev Link & Screen Capture
- **Gadget (`/usr/libexec/banana/usb-gadget`):** Mounts `configfs`, configures USB NCM Ethernet (`WINNCM` MS OS descriptor, built-in `CONFIG_USB_GADGET`/`NCM=y`, DT `dr_mode="otg"` + `role-switch-default-mode="peripheral"`). Handheld = `169.254.170.2/16`, host = `169.254.170.1` via 1-lease `udhcpd` (~4ms RTT, ~7MB/s SSH). `usb-gadget down` tears down; `usb-gadget msc off|ro|rw` sets the disk's mode. *(Reverse-orientation hotplug: the port reports `orientation=reverse` correctly, but the controller clears the gadget's UDC binding and nothing re-binds it -- there is no UDC "add" event, so the host sees nothing while the port, PHY and mux are all fine. `bananad` re-binds the complete gadget tree within ~3 s and logs the typec roles; verified on the unit, the host enumerates in either orientation. Writing `data_role`/`usb_role_switch` does nothing, so the re-bind is the mechanism. An OTG gamepad (power `source`) still takes the port as host and unbinds the gadget).*
- **Mass Storage (`usb-gadget` + `/usr/libexec/banana/usb-msc`):** Settings > System > **Mass storage** adds `functions/mass_storage.0` on the data partition (`${DISK}p4`; there is no udev, so `/dev/disk/by-*` does not exist). `usb-msc ro` exposes it read-only while `/data` stays mounted; `usb-msc rw` is transfer mode — `banana-ui-session` holds the respawn on `/run/banana/usb-msc.mode`, `/data` is unmounted (refused if busy) and handed over, and `bananad` refuses suspend meanwhile; `usb-msc off` remounts `/data` and restarts the menu (a short power press does the same, since the menu is stopped). A complete gadget whose UDC a role switch dropped is **re-bound in place** (no rebuild, adbd untouched), and `usb-adb` backs off and re-binds after an adbd restart, so a role bounce is no longer a random disconnect (`tools/rpc-usb-trace.sh` records one).
- **Screen Capture (`tools/rpc-screen.sh`):** Touches `/run/banana/screen.req` (caught by `banana-ui` `/run/banana` `inotify` watch) -> dumps pre-rotated `be.pixels(0)` to `/run/banana/screen.jpg` (`stbi_write_jpg`). `-w` streams MJPEG (`ffplay`/`mpv`). Menu-only (in-game uses RetroArch `SCREENSHOT` FIFO cmd).
