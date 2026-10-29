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
  - `make -s test`: Host unit tests.
  - `make -s bench`: Headless 600-game UI benchmark (compare against `docs/PERFORMANCE.md`).
  - `system/bananad/tests/sleep-test.sh`: Namespace-isolated mock `/sys` suspend/wake test.
  - `tools/qemu-boot.sh`: Boots full image in QEMU `virt` (`Virtual-1`, virtio input/GPU, tests A/B update & measures RAM/CPU into `resources-*.txt`).
- **Board Package:** `buildroot/board/retroid-pocket-classic` (pinned ROCKNIX SM4450 patches, DTs, `linux-banana.fragment`, audio topology, OpenABL).
