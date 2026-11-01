# BananaOS Roadmap (RPC / SM4450)
Refs: `docs/RPC-PORT.md` (port log), `docs/RPC-TEST.md` (hardware checklist).

## Completed Baseline (0.4.9+)
- **Core & CI:** End-to-end QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA updates, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`.
- **Input & Library:** `rsinput` Nintendo layout (`A=east, B=south`). Single-screen RetroArch library only (no DS/3DS dual-screen launchers).
- **Display & Graphics:** 90°-rotated 1240x1080 panel in `banana-ui`; games run upright via `cage` (`video_rotation = 0`, Wayland context; requires `SEATD_VTBOUND=0` so `bananad` `SIGSTOP` on suspend never deadlocks VT switch). Mesa Freedreno GL + **Turnip Vulkan** enabled on Adreno `a613` (SwanStation 60fps @ 5x).
- **Hardware Validation (`docs/RPC-TEST.md`):** In progress on physical units.

## Next (Active Targets)
- [x] **Plug-in vibration (0.4.46+):** `bananad` holds the PMIC `pm8xxx_vib_ffmemless` FF input open for its lifetime, uploads the `FF_RUMBLE` effect once and reuses it, plays and stops it on the loop's own clock (a `vib_off_at` deadline, never a `sleep_us`), logs `charger plugged in/unplugged` on the edge and every failure inside `vibrate()`, answers a `touch /run/banana/vibrate` trigger, and takes the pulse length from `vibrate_ms` (default 600). **Confirmed on the unit** (0.4.50+).
- [x] **Wi-Fi on the unit (0.4.53):** the WCN3950 `ath10k_snoc` associates (SSID `SpectrumSetup-F5`, 5 GHz, CCMP) and SSH answers over `wlan0`. The firmware + `modprobe` in `rcS` were the fix; association is confirmed.
- [ ] **Sleep/Wake Optimization:** a power-key sleep used to come straight back (1–18 s). The PMIC pwrkey is the **only armed wake source** and its edge-both IRQ was firing on a glitch / on the release of the very press that asked for the sleep. In 0.4.54: `bananad` **names the waker** (mounts debugfs, reads `wakeup_sources`; `pm_wakeup_irq` is absent here) and a wake with **no real `KEY_POWER` press and no cable change** logs `wake … (spurious): back to sleep` and re-suspends (bounded ×3); `&pon_pwrkey` gained `debounce = <15625>`; and the idle **burn-in guard no longer blanks while charging** (it stops at the dim stage — a dark panel on the cable read as a sleep). **To confirm:** unplug, sleep, and read `banana/logs/sleep.log` for the named source + the spurious line. Still open: the residual spurious wakes once the debounce is in, cutting panel power-on & SD re-init latency (`MMC_CAP_RUNTIME_RESUME`), keeping the Wi-Fi module loaded across suspend, and measuring overnight standby drain.
- [ ] **Thermals & Fan:** profiled 2026-11-01 under the heaviest load in the image (New Super Mario Bros. Wii / Dolphin, 12 min): `cpuss0` avg 46 / peak **55 °C**, `gpu` peak 52 °C, **the fan never engaged** (its first trip is 60 °C), no CPU/GPU throttle, clocks steady (`policy0` 1.96 / `policy6` 2.40 GHz). So the `pwm_fan` curve is not reached even by the heaviest emulator -- it is a safety net, not a normal load. Left: exercise it with a hotter load (a 3D Wii title, `tools/rpc-gpu-probe`, or a hot room) before tuning. Also fix `tools/rpc-thermals.sh`'s `gpu=<Hz>` column, which reads a bogus node.
- [ ] **Audio handover (menu and game):** `run-retroarch` and banana-ui each wait "one beat" for the DSP to tear a frontend's graph down before reopening (an open inside that window gets a half-built graph and the amps fail to start). Kernel patch `0063-…` reports the `APM_CMD_GRAPH_CLOSE` result and logs a slow (>= 50 ms) close, so the window is measurable; the wait is the `audio_handoff_ms` knob (`banana/bananad.conf`, published to `/run/banana/audio-handoff-ms`, read by `audio-settle` and banana-ui's `Sound`). **To confirm:** play a game, watch `dmesg | grep -i q6apm`, and tune or drop the waits. (The codec's late-card fallback was fixed: `ctl_set()` now retries the `" Switch"` name and `audio_setup()` waits for a real DAPM control.)
- [ ] **PipeWire (Deferred/Optional):** needs a SoC UCM2 profile + `libudev-zero`/`udev-virtual` integration before replacing direct ALSA (see `docs/RPC-PORT.md`). UCM only helps a UCM-aware opener, so with no sound server Banana keeps its own route writes.

## Later — features not started

- **OTA updates over Wi-Fi** (updates are card-only today; BananaLinux installs over the air).
- **Store / app catalog** (the Apps shelf only scans local folders).
- **Bluetooth** — the kernel is stripped of it and the QCA BT firmware is unused; BT audio/controllers would need a bespoke path (no BlueZ/DBus in the stack).
- **Wi-Fi management depth** — WEP/802.1X (currently `unsupported`), saved-network priority, static IP, hotspot.
- **i18n** — no locale layer (BananaLinux ships Simplified Chinese).
- **Library UX** — collections/tags, a recent-played shelf, a screenshot gallery, multi-disc `.m3u`, jump-to-letter.
- **Controller remapping UI** (on top of `pad-profile`), and **per-game core options** beyond core + shader.
- **Quick-settings overlay** and a **performance overlay** (FPS / temperature).
- **Local media / music player**, and porting BananaTube's newer settings screen.
- **Accessibility** (colour-blind palettes, larger type), an **in-menu log viewer**, and a **battery time-remaining** estimate with a graceful low-battery shutdown.

## Later — done
- [x] RetroAchievements progress display on the home shelf.
- [x] Save-state browser (browse, copy and delete a game's states from the menu).
- [x] Settings cleanup: the dead rows removed (VSync, Integer scaling, Audio latency, Waking up, LEDs, Accent colour, Motion), the section renamed to **RetroArch**, single-screen only, and the dead ScreenScraper entry gone.
- [x] A lighter keyboard and a redesigned game list (all five Pocket themes).
