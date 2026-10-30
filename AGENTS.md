# Banana Pocket Agent Rules & Constraints

## 1. Token & Context Efficiency (CRITICAL)
- **Zero Fluff:** Omit all conversational filler, greetings, preambles, and post-change summaries. Output only code actions, tool calls, or direct answers.
- **Surgical Reads:** NEVER read entire files or run recursive directory dumps (`ls -R`, `find .`, `cat`). Use `grep` or `ripgrep` (`rg -n`) to locate target symbols, then read only exact line ranges (`sed -n 'X,Yp'` or scoped file reads).
- **Minimal Diffs:** Apply targeted line replacements. Never echo unchanged functions or full files in chat responses.
- **Quiet Shell:** Always append quiet/concise flags to bash commands (`make -s`, `git status -s`, `git diff --stat`). Pipe build logs through `tail -n 30` or `grep -iE "error|warning|undefined"` to avoid flooding context.
- **Ignore Build Artifacts:** Never inspect or index `build/`, `output/`, `dl/`, `host/`, `.git/`, `*.o`, `*.img`, or binary firmware blobs.

## 2. Hardware Scope (Strict: Retroid Pocket Classic / SM4450)
- **SoC & GPU:** Qualcomm Snapdragon G1 Gen 2 (`SM4450`, `policy0` 6x A55 + `policy6` 2x A78), Adreno `a613` GPU (`msm` + Freedreno).
- **Display:** Single 1240x1080 AMOLED panel (`CH13726A`, `rotation = <90>`). `banana-ui` renders on CPU (`src/soft.cpp`) to KMS dumb buffers with 4x4 transpose rotation; games run via `cage` kiosk compositor.
- **Audio:** Qualcomm AudioReach (`sc8280xp`/q6apm) + `WCD937x` + dual `AW88166` amps. In `banana-ui`/`bananad`, set the route with the ALSA C mixer API (never fork `amixer`) before opening the PCM (the shell launchers `volume` and `run-retroarch` do fork `amixer`: they run before/around the open, out of process); release PCM before game handover; control volume via `Left PCM`/`Right PCM` amp gain (`0..511`).
- **Input & Power:** Stickless slate (no `SW_LID`, no RTC `wakealarm`). `rsinput` MCU gamepad + `FT5452` touch + power/vol keys.

## 3. Architecture & Minimalism
- **Filesystem:** Read-only A/B `squashfs` rootfs (`banana-root-a`/`b`) + writable `exFAT` partition mounted at `/data` (`BANANA`) and `/run/banana` (`tmpfs`).
- **Forbidden Stack:** NO `systemd`, `udev`, `PipeWire`, `PulseAudio`, `DBus`, or `NetworkManager`. Do not add compositors to `banana-ui` (`cage` is strictly for single-app game execution).

## 4. Performance & Resource Discipline
- **Rendering:** Minimize CPU composition overhead. Use dirty-rectangle / damage-diffing in `banana-ui` and zero-copy DRM page-flipping where applicable.
- **Idle & Memory:** Preserve minimal idle CPU/RAM footprint. Avoid polling loops; use blocking `epoll`/`poll` on `evdev`, `udev`, and IPC sockets.
- **AMOLED Protection:** Maintain burn-in guard logic and wake-input swallowing when dimming or waking from `s2idle`.

## 5. Code Standards & Verification
- **Languages:** Idiomatic C++17 (`-fno-exceptions -fno-rtti` where configured) in `ui/src/`; strict POSIX C11/C99 in `system/bananad/`. No unauthorized third-party libraries.
- **Verification:** Run `make -s test` to validate changes locally before completing a task. Only inspect full test output if a test fails.
