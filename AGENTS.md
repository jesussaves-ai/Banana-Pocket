# Banana Pocket (RPC / SM4450)

## 1. Agent I/O & Token Discipline
- **Output:** Zero prose/preambles/summaries. Output ONLY tool calls, minimal line-diffs, or direct answers. Never echo unchanged code.
- **Reads:** NO full-file reads or tree dumps (`cat`, `ls -R`, `find .`). Locate via `rg -n`, read exact line ranges (`sed -n 'X,Yp'`).
- **Arch Docs:** Do NOT read full `docs/ARCHITECTURE.md`. Query via `rg -n "^## " docs/ARCHITECTURE.md` + `sed -n 'X,Yp'` ONLY when touching boot/A-B, KMS, audio/DSP, `bananad` power, or USB gadget.
- **Ignore:** `build/`, `output/`, `dl/`, `host/`, `.git/`, `*.o`, `*.img`, `*.tar`, binary blobs.
- **Shell/Verify:** Quiet flags (`make -s`, `git status -s`, `git diff --stat`). Filter logs (`| tail -n 30` or `| grep -iE "error|warning|undefined"`). Run `make -s test` to validate.

## 2. Hardware & Stack Invariants
- **SoC/GPU:** SM4450 (`policy0` 6xA55 + `policy6` 2xA78; set governors on BOTH), Adreno `a613` (`msm` + Freedreno/Turnip).
- **Display:** 1240x1080 AMOLED (`CH13726A`, `rotation = <90>`). `banana-ui` (`ui/src/soft.cpp`, `kms.cpp`): CPU render, dirty-rect diff, 4x4 transpose to KMS dumb buffers, vblank flip. Games: `cage` Wayland kiosk ONLY.
- **Audio (`q6apm` + PipeWire/WirePlumber):** AudioReach (`sc8280xp`/q6apm) + `WCD937x` + single mono `AW88166` (`0-0035`, on FL; `FL [on]`/`FR [off]` is normal).
  - **DSP Hardware Law:** `q6apm` rejects FE<->BE mixer changes (`MultiMedia1`, etc.) while that FE's PCM (`pcm0p`) is `RUNNING`. Routes MUST be set while PCM is `CLOSED` prior to `snd_pcm_open`.
  - **UCM Rule:** Shared FE/BE switches belong ONLY in `SectionVerb` `EnableSequence` (NEVER in `SectionDevice` `DisableSequence`, or inactive port teardown will clobber the active route).
  - **C Code:** NEVER fork `amixer`/`alsaucm` in C/C++; use ALSA C API (`snd_mixer_*`). Volume: `Left PCM`/`Right PCM` HW gain (`0..511`).
- **Input/Power:** Stickless slate (`rsinput` MCU + `FT5452` touch + power/vol; NO `SW_LID`, NO RTC `wakealarm`). Preserve AMOLED burn-in guard (`/run/banana/screen-off`) + `s2idle`/`deep` wake-key swallowing.
- **Storage:** RO A/B `squashfs` (`banana-root-a`/`b`), RW `exFAT` `/data` (`BANANA`), `tmpfs` `/run/banana`.
- **Forbidden:** NO `systemd`, `udev`, `PulseAudio`, `NetworkManager`, or extra compositors in `banana-ui`.

## 3. Code Standards
- **Lang:** `ui/src/` = C++17 (`-fno-exceptions -fno-rtti`, keep `ui/rnds` parity); `system/bananad/` = strict POSIX C99/C11. Zero unauthorized 3rd-party libs.
- **Concurrency:** Zero polling loops; strictly blocking `poll`/`epoll` on `evdev`, `inotify`, netlink, and IPC FDs.

## 4. Scope & Escalation Guardrails (STRICT)
- **No Unapproved Upstream/Kernel Patches:** NEVER patch kernel (`drivers/`, `sound/soc/qcom/`) or upstream packages (`pipewire`, `mesa`, `wlroots`) in `package/` without proving userspace config/UCM/env-vars cannot solve it AND getting explicit user approval first.
- **Hypothesis Limit:** Test ONE variable at a time. If 2 consecutive attempts fail to produce expected hardware state, STOP and report exact state diff (`amixer`, `sysfs`, logs) in <=5 lines. Do NOT spiral into multi-file refactors.
- **Stay in Scope:** Modify ONLY files directly implicated by the prompt. Never revert commits or alter startup/init architecture without asking first.
