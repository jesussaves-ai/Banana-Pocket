# Hardening sweep: the bugs, and the dead weight (2026-11-05)

## Intent

Four read-only passes over the tree (app, daemon/system, UI, docs/build/tools) turned up defects and stale items.
The user's call: **fix all the bugs, delete the stale items.** This is maintenance (target 0.5.10), not a feature
release: no new capability, no redesign, no kernel or Buildroot-package patch (the scope guard in `AGENTS.md` §4).

## Goals

- Every defect below is fixed, and where the repo already has a harness for it, covered by a check that failed first.
- Every stale item is deleted or corrected, so nothing dead ships and no doc contradicts the code.

## Non-goals

- New features (the roadmap's Ideas list is untouched).
- Wi-Fi-across-suspend: that is a measurement, and it stays in the sleep study.
- A rewrite of anything that works. A stale *claim* is fixed by correcting the claim, not by bending the code to it,
  unless the claim is a safety rule (see call 2).

## The calls

1. **The dual-screen machinery: delete it.** `Display::openSingle` hard-sets `count = 1` (`ui/src/kms.cpp:178`), yet
   `SoftBackend` always allocates two full canvases (`ui/src/soft.cpp:151-158`, ~10.7 MB at 1240x1080) and
   `layout.cpp:1185-1203` renders, diffs and ends both screens every frame. Every shipped theme defines only `top`.
   The user said delete the stale items, so the second screen goes: one canvas, the `screen == 0` path only, the dead
   theme rows and their art, the stale comments. Cost if wrong: a future two-panel board needs the loop back (~30 lines,
   in git history).
2. **The `amixer` fork: correct the docs, and put the code on the roadmap.** `ARCHITECTURE.md:46` (and `AGENTS.md` §5)
   say bananad drives the amplifier "strictly via the ALSA C mixer API (`snd_mixer_*`) ... NEVER fork `amixer`"; in fact
   `volume()` forks `/usr/libexec/banana/volume` (`bananad.c:1224-1227`), which runs `amixer` (`volume:42-45`), and
   `save_settings`/`restore_settings` fork `alsactl` (`bananad.c:1386,1399`). The C API is used only for the sleep
   mute/routing. Rewriting a working, subtle path (the amp's two per-channel controls, the `auto` card discovery) in one
   sweep is the wrong risk; the *law* is the thing that is wrong, so it is corrected and the rewrite is a roadmap item.
   Cost if wrong: the invariant stays aspirational a while longer.
3. **`tools/qemu-e2e.sh`: delete it.** It is referenced by one spec doc and nothing else; `qemu-boot.sh` is the QEMU
   path wired into CI. Its scenario (an A/B update end to end) is already covered on hardware by
   `tools/rpc-push-update.sh --apply`. Cost if wrong: recover it from git history.
4. **`.superpowers/sdd/.gitignore`: keep.** It is the ignore rule for the plan workspace, not dead content (the recon
   was wrong about this one).

## The defects

Daemon (`system/bananad/bananad.c`, `system/rootfs/usr/libexec/banana/*`):

- **D1 — an OTA can be suspended mid-write.** Only the scraper writes `/run/banana/awake.pid` (`ui/src/scrape.cpp:387`);
  bananad honours it to block idle-sleep (`bananad.c:1911,1977`). `update`, `update-check` and `backup` must write and
  remove it too, so a 200 MB slot write is never interrupted by an idle suspend. *(fix)*
- **D2 — power off in USB transfer mode yanks the host's disk.** `usb-msc rw` unmounts `/data` and hands it over
  (`usb-msc:87-107`); `rcK` stops share/ssh/wifi but never runs `usb-msc off`. Add it to `rcK` before the unmount. *(fix)*
- **D3 — the wake reason is logged wrong.** `woke` is set to `"power button"` even when the charger edge woke it
  (`bananad.c:731-736`), and an unplug is counted as a spurious wake. Name the cable, and do not spend a retry on it. *(fix)*
- **D4 — `pid_of()` trusts a wall-clock file age.** `file = time(NULL) - st_mtime` compared with the *monotonic* process
  age (`bananad.c:286-287`). After an NTP/hwclock step (`wifi:62`) the file looks older than the boot, the pid looks
  reused, and the menu can suspend over a running game. A pidfile cannot be older than the boot: clamp `file` to the
  uptime and ignore the reuse test when it exceeds it. *(fix)*
- **D5 — the sleep.d hooks are never exercised.** `sleep-test.sh:39` copies the real hooks in but never provides
  `/usr/libexec/banana`, so `10-wifi`/`20-sound`/`30-usb` all hit `[ -x ... ] || exit 0` and no-op. Give the test a
  recording stand-in directory and assert each hook ran, in order, with `pre`/`post`. *(fix, test-first)*

BananaTube (`app/bananatube/main.cpp`):

- **A1 — data race on `results`.** The thumbnail worker copies it under `searchLock` (`:547`); six main-thread writes do
  not take it (`:580,758,769,787,797,614`). Take the lock on every write. *(fix)*
- **A2 — `qualitySave` can truncate its own conf.** It writes in place (`:870-876`) while `recentSave` uses
  temp+fsync+rename (`:669-681`). Use the same pattern. *(fix)*
- **A3 — every tap is logged.** `:1591-1592` and `tapAt:1379-1449` log unconditionally; only `:1374` is gated by
  `BANANA_TUBE_TOUCH_DEBUG`, and `docs/BANANATUBE.md:94` says the logging is debug-gated. Gate them. *(fix)*
- **A4 — `feedTouch` needs `BTN_TOUCH`.** A panel that reports only `ABS_MT_TRACKING_ID` gets zero taps (`:1554`).
  Latent here (the FT5x06 sends `BTN_TOUCH`), cheap and safe to accept either. *(fix)*
- **A5 — `resolveStream` rejects more than two URLs** (`:975`) although yt-dlp can print three; take the first video
  and the first audio, ignore the rest. *(fix)*
- **A6 — `ipcSend` ignores a short write** on a non-blocking socket (`:907-912`). Loop on `EAGAIN`. *(fix)*

UI (`ui/src/*`):

- **U1 — X opens per-game options for every system but the chip only shows for NDS** (`main.cpp:1504` vs
  `library.cpp:1184`). One line. *(fix)*
- **U2 — a ROM copied while the Library is open waits for a keypress.** The rescan block covers HOME *or* LIBRARY
  (`main.cpp:1752-1753`) but the poll timeout only shortens to the 2 s debounce on HOME (`:1845`). *(fix)*
- **U3 — `wifi::Job` reads `active`/`done` outside its mutex** (`wifi.cpp:298` vs writes at `290-292`). Make them
  atomics. *(fix)*
- **U4 — `readGamelist` matches the root `<gameList>` as a `<game>` element** (`library.cpp:139`). Require the tag to be
  `<game` followed by a space or `>`. *(verify, then fix)*
- **U5 — the update/box-art row values are English-only.** `updatesWaiting()` `"1 package"/"N packages"` (`settings.cpp:143`)
  and `scrapeShort()` `"Not yet"/"Starting"/"N of M found"/"No connection"/"Didn't work"` (`:2653-2661`) are drawn through
  `tr()` but are not keys in `ui/assets/lang/en.json`; `tools/check-lang.py` only checks en->src, so it passes while
  `ROADMAP.md:48` claims the values are translated. Give them keys (the count forms become whole keys) and teach the
  checker to flag a literal that reaches `tr()` without being a key. *(fix, test-first)*
- **U6 — the second canvas and the bottom screen** (call 1). *(fix, with the ui tests)*

## The stale items

Delete: `system/rootfs/etc/fstab` (nothing runs `mount -a`), `system/rootfs/etc/banana/accounts.conf.example` (no
references), `tools/make-console.py` (only its own usage line), `tools/qemu-e2e.sh` (call 3), `docs/img/readme/` (29
tracked PNGs, referenced by nothing), the stray untracked `bgm.wav` at the repo root, the dead ROCKNIXDS rows in
`ui/assets/rnds/systems.cfg` (`rocknixds`, `rocknixdsbank`, `dodakvarter`) and the art that exists only for them, the
bottom-screen render path and its stale comments (`layout.cpp:1192-1200`, `settings.h:210`, `settings.cpp:1860,2035`,
`main.cpp:1243-1246`).

Verify, then correct: the never-read `sleep_debug` line in the image conf (does `/data/banana/bananad.conf` ever exist
from it?), `autosleep_minutes = 10` being overridden by the slate default (`bananad.c:1798-1804`), and
`usb_mass_storage = rw` persisting while `usb-gadget:225-233` always boots the LUN read-only.

Doc corrections: `CODEBASE-ANALYSIS.md:5` (0.4.52 -> 0.5.9) and `:4` ("two AW88166 amplifiers" -> one, mono); SwanStation
5x -> 4x in `EMULATORS.md:30`, `ROADMAP.md:115` and the defconfig comment; `PERFORMANCE.md:6` (cites a section that does
not exist); `ARCHITECTURE.md:18` ("`lease()` refused"), `:19` ("4-px SIMD-style edge blend"), `:56` (the stale `make test`
list), `:46` (call 2); `README.md:48` (twenty-two shelves -> 21); `RPC-TEST.md:30` ("both channels" -> one); `layout.h:26-27`
(551x480 -> 640x557); `Makefile:115` ("the CI asserts this hash"), `Makefile:16-19` (the tracked inputs); `BANANATUBE.md`
(`touch-rot` -> `touch.conf`, the nonexistent `tests/selftest.sh`, the invented `systems.cfg`/`video.png`,
`--ytdl-format` -> yt-dlp `-f`, the "debug-gated" logging claim); the stale audio comments in `main.cpp:23-24`,
`run-bananatube:23-24`, `run-port:72-73`, `ui/src/sound.cpp:90`; `app/bananatube/app.json:5` (the launcher applies no
`perf`).

CI fragility: the `cores` gate watches only `buildroot/package/libretro-*/`, so a core *patch* triggers no rebuild
(`ci.yml:85,88`); `sh -n` globs `usr/libexec/banana/*` with no filter (`ci.yml:40`, `Makefile:131-134`); `make test`
never compiles bananad.

## Constraints

- `system/bananad/` is C99/C11 with `-Wall -Wextra -Werror`; `ui/src/` is C++17 (`-fno-exceptions -fno-rtti`) and
  `ui/rnds` parity is maintained; no new dependency.
- `make -s test` and `make -s agent-check` green after every task; a sound/UCM change also `make sound-test`.
- The unit runs 0.5.9, slot b, with `/tmp` bind mounts; nothing here reaches it until an image is cut.

## Risks and verification

- D3, D4 and U4 are reasoned from code, not reproduced; each fix must be conservative (never less safe than today).
- The dual-screen deletion changes the render path's shape: the ui tests and a host render must both pass.
- The device-only checks (a real idle suspend blocked by `awake.pid`; the wake log naming the cable) are listed as
  device steps, to be run when the unit is awake.
