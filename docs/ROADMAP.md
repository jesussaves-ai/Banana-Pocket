# BananaOS Roadmap (RPC / SM4450)
Refs: `docs/RPC-PORT.md` (port log), `docs/RPC-TEST.md` (hardware checklist).

## Completed Baseline (0.4.2+)
- **Core & CI:** End-to-end QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA updates, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`.
- **Input & Library:** `rsinput` Nintendo layout (`A=east, B=south`). Single-screen RetroArch library only (no DS/3DS dual-screen launchers).
- **Display & Graphics:** 90°-rotated 1240x1080 panel in `banana-ui`; games run upright via `cage` (`video_rotation = 0`, Wayland context; requires `SEATD_VTBOUND=0` so `bananad` `SIGSTOP` on suspend never deadlocks VT switch). Mesa Freedreno GL + **Turnip Vulkan** enabled on Adreno `a613` (SwanStation 60fps @ 5x).
- **Hardware Validation (`docs/RPC-TEST.md`):** In progress on physical units.

## Next (Active Targets)
- [ ] **Sleep/Wake Optimization:** Profile on hardware (`sleep_debug = yes`). Cut panel power-on & SD re-init latency (evaluate `MMC_CAP_RUNTIME_RESUME`), test keeping Wi-Fi module loaded across suspend, measure overnight standby drain.
- [x] **Wi-Fi Firmware:** the WCN3950 `ath10k` blobs (`ath10k/WCN3990/hw1.0/{firmware-5.bin,board-2.bin,wlanmdsp.mbn}`) and the WPSS firmware are in the board's `firmware/`, and `rcS` `modprobe ath10k_snoc` (verified against the driver's path in `linux-custom`). On-unit Wi-Fi association still to confirm.
- [ ] **Thermals & Fan:** Tune `pwm_fan` curve and profile thermals under sustained 10m+ N64/PSX load (`tools/rpc-thermals.sh`).
- [ ] **PipeWire (Deferred/Optional):** Needs SoC UCM2 profile + `libudev-zero`/`udev-virtual` integration before replacing direct ALSA (see `docs/RPC-PORT.md`).

## Later
- [x] RetroAchievements progress display on home shelf.
- [x] Save-state browser (browse, copy and delete a game's states from the menu).
