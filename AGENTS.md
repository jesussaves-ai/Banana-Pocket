# Banana Pocket (RPC / SM4450)

## 1. Agent I/O & Token Discipline
- **Output:** Zero prose/preambles/summaries. Output ONLY tool calls, minimal line-diffs, or direct answers. Never echo unchanged code.
- **Reads:** NO full-file reads or tree dumps (`cat`, `ls -R`, `find .`). Locate via `rg -n`, read exact line ranges (`sed -n 'X,Yp'`).
- **Arch Docs:** Do NOT read full `docs/ARCHITECTURE.md`. Query specific sections via `rg -n "^## " docs/ARCHITECTURE.md` + `sed -n 'X,Yp'` ONLY when modifying boot/A-B updates, KMS/renderer, ALSA/DSP, `bananad` power/sleep, or USB gadget.
- **Ignore:** `build/`, `output/`, `dl/`, `host/`, `.git/`, `*.o`, `*.img`, `*.tar`, binary blobs.
- **Shell/Verify:** Quiet flags only (`make -s`, `git status -s`, `git diff --stat`). Filter build logs (`| tail -n 30` or `| grep -iE "error|warning|undefined"`). Validate changes via `make -s test` (inspect full log only on failure).

## 2. Hardware & Stack Invariants
- **SoC/Clocks:** SM4450 (`policy0` 6xA55 + `policy6` 2xA78; apply governors to BOTH), Adreno `a613` (`msm` + Freedreno).
- **Display:** 1240x1080 AMOLED (`CH13726A`, `rotation = <90>`). `banana-ui` (`ui/src/soft.cpp`, `kms.cpp`): CPU render, dirty-rect diffing, 4x4 transpose copy to KMS dumb buffers, vblank flip. Games: `cage` Wayland kiosk ONLY.
- **Audio:** AudioReach (`sc8280xp`/q6apm) + `WCD937x` + dual `AW88166`. `banana-ui`/`bananad` MUST apply mixer route via ALSA C API *before* PCM open (NEVER fork `amixer` in C/C++; only shell scripts `volume`/`run-retroarch` fork `amixer`). Close PCM before game handover. Volume: `Left PCM`/`Right PCM` HW gain (`0..511`).
- **Input/Power:** Stickless slate (`rsinput` MCU + `FT5452` touch + power/vol; NO `SW_LID`, NO RTC `wakealarm`). Preserve AMOLED burn-in guard (`/run/banana/screen-off`) + `s2idle`/`deep` wake-key swallowing.
- **Storage:** RO A/B `squashfs` (`banana-root-a`/`b`), RW `exFAT` `/data` (`BANANA`), `tmpfs` `/run/banana`.
- **Forbidden:** NO `systemd`, `udev`, `PipeWire`, `PulseAudio`, `DBus`, `NetworkManager`, or extra compositors in `banana-ui`.

## 3. Code Standards
- **Lang:** `ui/src/` = C++17 (`-fno-exceptions -fno-rtti`, avoid diverging `ui/rnds`); `system/bananad/` = strict POSIX C99/C11. Zero unauthorized 3rd-party libs.
- **Concurrency:** Zero polling loops; strictly blocking `poll`/`epoll` on `evdev`, `inotify`, netlink, and IPC FDs.
