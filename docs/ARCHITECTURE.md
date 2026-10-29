# BananaOS Technical Architecture (RPC / SM4450)

## 1. Storage & A/B Update Layout
- **Bootloader:** OpenABL (`arm-efi` in device `abl_a`/`abl_b`; no U-Boot on SD, no initramfs). Reads p1 `extlinux/extlinux.conf` (`DEFAULT` slot, `root=PARTLABEL=banana-root-a|b rootfstype=squashfs ro rootwait quiet`).
- **Partitions:**
  - `p1` (`BANANA-BOOT`, FAT, 160MB): `a|b/Image`, `a|b/<board>.dtb`, `extlinux/extlinux.conf`.
  - `p2` (`banana-root-a`, squashfs LZ4, 256MB): Read-only rootfs.
  - `p3` (`banana-root-b`, squashfs LZ4, 256MB): Inactive A/B rootfs slot.
  - `p4` (`BANANA`, exFAT, remaining space): Mounted at `/data` (created on first boot via `/usr/libexec/banana/firstboot`). Holds `roms/`, `bios/`, `saves/`, `states/`, `banana/{settings.conf,bananad.conf,accounts.conf,wifi.conf,asound.state,setup-done,logs/,updates/,ssh/}`.
- **Update Flow (`/usr/libexec/banana/update apply`):** Reads `/data/banana/updates/banana-<board>-<version>.update.tar`. Verifies `board` & `sha256sums`, streams `rootfs.squashfs` (16MB chunks) to inactive slot, verifies read-back hash, populates boot slot (`a/` or `b/`), atomically renames `extlinux.conf`. Logs to `banana/logs/updates.log`.

## 2. Boot & Init Sequence
- **Built-in Kernel Drivers:** CH13726A DSI panel (`rotation = <90>`), `rsinput` (MCU buttons), FT5452 touch, PMIC/`qcom_battmgr`, PWM fan (`pwm_fan`). No fbcon/VTs/udev.
- **Modules (loaded by `/etc/init.d/rcS` or on-demand):** Audio (`sc8280xp`/q6apm/LPASS/SoundWire, WCD937x, AW88xx), GPU (`msm` Adreno a613 + Freedreno, loaded after first UI frame), Wi-Fi (`ath10k_snoc` for WCN3950), SMB3 (`ksmbd`, loaded on toggle). `bananad` loads USB driver modules (e.g., `xpad`) via netlink `MODALIAS`.
- **Init (`busybox init` -> `/etc/init.d/rcS`):** Mounts `/proc`, `/sys`, `/run`, `/tmp`, `/data`; loads audio; starts `bananad`; sets `banana-perf` clocks; starts `banana-ui` (plays boot anim unless `splash = off` while worker thread scans library).

## 3. UI Engine (`banana-ui`)
- **Core (`ui/rnds`):** Imported via `tools/import-rnds.sh` (do not diverge unnecessarily). Canvas: 640x557 scaled 1.9375x to 1240x1080 (`themes/pocket.json`).
- **Display (`src/kms.cpp`):** Single connector (`Display::openSingle()`), 2 KMS dumb buffers, vblank page-flipping. Composes in cached RAM; `Display::turn()` copies only dirty rects into uncached scanout buffer via 4x4 register transpose (90° rotation). `lease()` is refused on single-panel.
- **Software Renderer (`src/soft.cpp`):** CPU rasterizer. Diffs current vs. previous draw lists per-rect; redraws only changed/clipped regions. Textures stored as transparent/opaque/blended runs (`memcpy` opaque, 4-pixel SIMD-style blend on edges) with cached resampled/sub-pixel phases.
- **Workers & Event Loop (`src/main.cpp`, `src/library.cpp`):** Up to 3 niced threads decode/fit PNG art lock-free. Main thread blocks in `poll()` (input, touch, DRM flip, workers, signals, next anim/clock tick). Watches ROM dirs via `inotify` (2s debounce on home screen).
- **Settings (`src/settings.cpp`, `src/settings_defaults.cpp`):** Merges `/etc/banana/settings.conf` into `/data/banana/settings.conf`. Writes live changes to `settings.conf`, `bananad.conf` (+ `SIGHUP`), `retroarch.cfg`, and POSIX `TZ`.
- **Game Handover (`cage`):** Writes PID to `/run/banana/game.pid`, releases DRM/input/ALSA (`Sound::release()`), sets clock profile, launches game inside `cage` (kiosk Wayland compositor, DRM master, applies connector panel orientation so RetroArch/OSD render upright). Reclaims panel on exit.

## 4. System Daemon & Power (`bananad`)
- **Input & Hotkeys:** Non-grabbing `evdev` reader + netlink `power_supply` listener (for PWM LEDs). Slate device (`EVIOCGBIT(EV_SW)` has power key + codec jack, NO `SW_LID`).
  - Short Power: Sleep (unless `/run/banana/screen-off` exists from UI burn-in guard; then `banana-ui` consumes `KEY_POWER` as `Key::WAKE`).
  - Hold Power (2s): Clean shutdown (also triggered at <=3% discharging).
  - Vol Up/Down: Adjusts hardware gain via `/usr/libexec/banana/volume`.
  - F + Vol: Adjusts PWM backlight.
- **Idle & Burn-in Guard:** UI dims after 2m (`dim_minutes`/`dim_percent`), blanks at 90s/180s (writes `/run/banana/screen-off`, closes ALSA via `Sound::setScreenOn(false)`). Never dims during active game. Auto-suspend occurs ONLY if `autosleep_minutes` is explicitly set in `/data/banana/bananad.conf`.
- **Suspend/Resume Flow (`deep` -> `s2idle` -> backlight fallback):**
  1. `SIGSTOP` to `/run/banana/game.pid` process group.
  2. Run `/etc/banana/sleep.d/* pre` (`10-wifi`: the link and its supplicant down, if Wi-Fi is up).
  3. `sync()` (the config does not set `SUSPEND_SKIP_SYNC`, so this duplicates the kernel's own flush — kept as a belt-and-braces).
  4. Enable `pm_async`/`fast_resume`, write `deep` to `/sys/power/mem_sleep`, `mem` to `/sys/power/state`. *(Note: RPC PMIC RTC is `qcom,no-alarm`; `wakealarm` & `sleep_check_hours` are no-ops on RPC; wakes only via Power button or USB-C charger).*
  5. On wake: `SIGCONT` game, `SIGUSR1` `banana-ui` (instant redraw, no modeset), swallow wake button press, run `sleep.d/* post` in background, restore LEDs & audio route. Logs to `banana/logs/sleep.log` (and `sleep-devices.log` if `sleep_debug = yes`).

## 5. Audio Architecture (Qualcomm AudioReach)
- **Hardware:** `sc8280xp`/q6apm DSP + WCD937x codec + dual AW88166 I2S amps. No sound server, no UCM. Config in `/run/banana/asound.conf`.
- **DSP Constraints:**
  1. Mixer route MUST be applied via ALSA C mixer API (never fork `amixer`) *before* opening PCM frontend (DSP latches graph at open). `bananad` writes target to `/run/banana/audio-route`; `banana-ui` and `run-retroarch` apply it before open.
  2. Only one active PCM frontend produces audio (`banana-ui` and games must strictly hand over the device with a 1-beat delay).
  3. Stream volume is latched at open; system volume MUST control AW88166 hardware gain (`Left PCM` / `Right PCM`, `0..511` mapped to dB/percent in `/run/banana/mixer` and persisted via `alsactl` in `/data/banana/asound.state`). RetroArch software gain stays at unity.

## 6. Network & Services
- **Wi-Fi:** `wpa_supplicant` + `wpa_cli` scan thread (`/data/banana/wifi.conf`, `key_mgmt=SAE` for WPA3). `udhcpc` triggered via `wpa_cli` action script only upon connection. Scans back off to 5m when disconnected.
- **SMB3:** `ksmbd` + `ksmbd.mountd` (`\\<ip>\banana` -> `/data`, guest forced to root, no DOS/ext attrs, off by default).
- **SSH:** `dropbear` (key-only, off by default). Copies `/data/banana/ssh/authorized_keys` to `/run/banana/ssh` (`0600`).

## 7. Performance Profiles (`banana-perf`) & Test Harnesses
- **CPU Clusters:** Must apply governors to BOTH `policy0` (6x Cortex-A55) and `policy6` (2x Cortex-A78):
  - `ui`: `schedutil` (~1.4 GHz) | `powersave`: `schedutil` | `balanced`: `schedutil` (full) | `performance`: `performance`.
- **Verification & Tooling:**
  - `make -s test`: Host unit tests (`tools/check-systems.py`, the launcher test, then the headless UI test).
  - `tools/check-systems.py`: Host check that every `systems.conf` system names an existing launcher and a core a buildroot package builds (the launchers/cores graph, without a device).
  - `system/tests/run-retroarch-test.sh`: Host test of `run-retroarch`'s per-core defaults, config writes and RetroAchievements hand-off, from a path-rewritten copy (no root, no device).
  - `make -s bench`: Headless 600-game UI benchmark (compare against `docs/PERFORMANCE.md`).
  - `system/bananad/tests/sleep-test.sh`: Namespace-isolated mock `/sys` suspend/wake test.
  - `tools/qemu-boot.sh`: Boots full image in QEMU `virt` (`Virtual-1`, virtio input/GPU, tests A/B update & measures RAM/CPU into `resources-*.txt`).
  - `tools/rpc-thermals.sh`: On the handheld, a line a second of every thermal zone, the fan's duty/tachometer and the CPU/GPU clocks, for profiling the fan curve (docs/RPC-TEST.md); `system/tests/rpc-thermals-test.sh` drives it against a mock `/sys` on the host.
  - **CI (`.github/workflows/ci.yml`):** `checks` (the host tests above, `bananad` native+aarch64, the sleep test, `sh -n` on every launcher, the defconfig-preservation check) and `cores` — a change-gated Buildroot build of whichever `libretro-*` packages changed, so an upstream or pin break fails on the pull request; the SD card `image` and `boot` jobs run on dispatch.
- **Board Package:** `buildroot/board/retroid-pocket-classic` (pinned ROCKNIX SM4450 patches, DTs, `linux-banana.fragment`, audio topology, OpenABL).

## 8. USB Device Link (development)
- **What it is:** `usr/libexec/banana/usb-gadget` (`rcS`, in the background) turns the handheld's own USB-C port into a **USB Ethernet (NCM) gadget with a one-lease DHCP server**, so a development machine sees an interface *and an address* with nothing to configure -- no Wi-Fi, and far faster for pushing the ~196 MB update tars. `usb-gadget down` tears it down.
- **Why it needs no new kernel work:** the port is `dr_mode = "otg"` with `role-switch-default-mode = "peripheral"` (device tree), so an ordinary cable to a computer is already the *device* case; the kernel has `CONFIG_USB_GADGET`, `CONFIG_USB_CONFIGFS` and `NCM`/`ECM`/`RNDIS`/`Mass Storage` **built in** (`=y`, nothing to modprobe). The script mounts `configfs` itself -- nothing else does, there being no udev or systemd.
- **Link:** fixed locally-administered MACs; the handheld is `169.254.170.2/16` and hands the computer `169.254.170.1` (BusyBox `udhcpd`, `max_leases 1`, `lease_file /dev/null`), both inside `169.254.0.0/16` so the computer needs no particular subnet. Recipe (configfs layout, the Microsoft OS descriptor `WINNCM` that keeps Windows on its own NCM driver, the address scheme) follows ROCKNIX's `scripts/usbgadget` for these handhelds. Measured on the RPC: ~4 ms round trip, ~7 MB/s over `ssh` (the host binds `cdc_ncm` and names the interface from the USB path, e.g. `enp0s20f0u1`).
- **The port on the handheld's side:** the USB role switch reports `device` and the controller enumerates as a device **once the cable is in a computer**; the Type-C port's own `data_role`/`power_role` attributes read `host`/`source` on this board and are not what decides it. If the gadget was bound while the cable was elsewhere and nothing enumerates, a re-seat is what attaches it. An OTG adapter with a gamepad turns the port into a host and the kernel unbinds the gadget.
- **The menu's own screen (debugging):** `tools/rpc-screen.sh` asks `banana-ui` for the frame it is showing and fetches it over this link. Writing `/run/banana/screen.req` -- the inotify watch the menu already keeps on `/run/banana` for the launch file sees it, so nothing new is polled -- makes it write the composed screen (`be.pixels(0)`, the picture a person looking at the handheld sees; `Display::present()` is what turns it for the panel) to `/run/banana/screen.jpg` as a JPEG with `stbi_write_jpg`. One frame comes back as a file; `-w` streams MJPEG for `ffplay -f mjpeg -` or `mpv -`. It is the *menu*: with a game running, cage owns the panel and there is no frame to give -- a game's screen is RetroArch's own `SCREENSHOT` command, which is on the same command FIFO as `TOGGLE_ANALOG_DPAD`.
