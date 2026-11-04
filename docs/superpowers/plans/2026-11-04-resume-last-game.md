# Resume the last game at boot — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A clean shutdown while a game runs boots back into that game; a shutdown in the menu boots the menu.

**Architecture:** The menu marks the running game at launch (`/run/banana/game.path`); `bananad` persists it at shutdown (`/data/banana/resume`, plus `/data/banana/resume-tries`); after the menu is up, a small decision function consumes the marker after a 2 s cancel window and feeds the existing `/run/banana/launch` path, which the menu already watches and launches from.

**Tech Stack:** C++17 (`ui/src/main.cpp`), C99 (`system/bananad/bananad.c`), BusyBox `sh` tests.

**Spec:** `docs/superpowers/specs/2026-11-04-resume-last-game-design.md`

**Status:** Done in 0.5.8. Tasks 1-3 (`aa78895`, `ae13ce9`, `0a9018c`), the boot-press fix and the speed-ups
(`64ffce2`). Verified on the unit with a real shutdown → power-on: `resume: ready` → `resume: launching`, tries 1, no
cancel; menu 6.41 s, launch ~6.9 s. Pending only a deliberate non-power cancel press on hardware.

## Global Constraints

- No new dependencies; no per-frame allocation or work (one read at startup, one at shutdown).
- Paths exactly: `/run/banana/game.path`, `/data/banana/resume`, `/data/banana/resume-tries`.
- The menu is the only writer of `/run/banana/game.path`; `bananad` is the only writer of `/data/banana/resume`.
- C++17, `-fno-exceptions -fno-rtti`; `bananad` strict C99, `-Wall -Wextra -Werror`.
- Tests: `make -s test` (incl. the sleep test) and `make agent-check` must pass with no warnings.

## Review Focus

- Card changed or ROM deleted between shutdown and boot: log, clear both marker files, menu (Task 2 test).
- A game that wedges the device: 3 launches then stop trying (Task 2 test).
- Shutdown in the menu leaves no marker (Task 1 test).
- Any key or a touch in the window cancels (Task 3, device check).
- `bananad` must read the path **before** it stops the game (Task 1 test writes the path first).

---

### Task 1: `bananad` persists the marker at shutdown

**Files:**
- Modify: `system/bananad/bananad.c` (`shutdown_now`, ~771)
- Test: `system/bananad/tests/sleep-test.sh` (add step 2b after the battery-guard step 2)

**Interfaces:**
- Consumes: `/run/banana/game.path` (one line, absolute ROM path; written by the menu).
- Produces: `/data/banana/resume` (the same line) when a game is running; removed when none is.

- [ ] **Step 1: Write the failing test** — in step 2 (the battery-guard shutdown, which already has `game.pid`), before `power`, add
  `echo /data/roms/gba/Test.gba > /run/banana/game.path`; after the shutdown assertions add
  `grep -qx /data/roms/gba/Test.gba /data/banana/resume && ok "the game is remembered" || bad "no resume marker"`.
  Add a second case: `rm -f /run/banana/game.path`; run the same shutdown in a fresh `start`; assert `[ ! -e /data/banana/resume ]`.
- [ ] **Step 2: Run it to verify it fails** — `unshare -Urmp -f --mount-proc sh system/bananad/tests/sleep-test.sh --inside <W> system/rootfs`; expected: the two new checks fail.
- [ ] **Step 3: Implement** — in `shutdown_now(const char *why)`, before stopping the game:
  `char rom[PATH_MAX]; read_str("/run/banana/game.path", rom, sizeof rom);` then `if (game && rom[0]) write_file("/data/banana/resume", rom); else unlink("/data/banana/resume");`
  (`game` is the `pid_of("/run/banana/game.pid")` already read there).
- [ ] **Step 4: Run the test to verify it passes**; expected: all checks pass.
- [ ] **Step 5: Commit** — `git add system/bananad/bananad.c system/bananad/tests/sleep-test.sh && git commit -m "bananad: remember the game a shutdown interrupted"`.

### Task 2: the resume decision, testable headless

**Files:**
- Modify: `ui/src/main.cpp` (helpers near the top, after `logf`; a script verb in `runHeadless`, near `wifi`)
- Test: `ui/tests/run.sh` (a new block next to the backup/wifi script checks)

**Interfaces:**
- Produces:
  - `enum class Resume { None, Ready, Missing, Blocked };`
  - `Resume resumePlan(const std::string& data, std::string& rom)` — `Blocked` when `resume-tries >= 3`; `None` when `resume` is absent; `Ready` when its path is readable; else `Missing`.
  - `void resumeTake(const std::string& data)` — increment `resume-tries`, remove `resume`.
  - `void resumeCancel(const std::string& data)` — remove `resume` and `resume-tries`.
  - script verb `resume [take|cancel]` logging `resume: ready <path> | missing | none | blocked`, and on `missing`/`blocked` also calling `resumeCancel` (what the live loop does).
- Consumes: the Task 1 marker files.

- [ ] **Step 1: Write the failing test** — in `ui/tests/run.sh`, with `$FX/data`:
  seed `echo <a real fixture ROM> > $FX/data/resume` and run `--script "resume"`; assert the log `resume: ready <rom>` and that the marker still exists;
  seed a bogus path, run `resume`, assert `resume: missing` and the marker gone;
  seed the ROM and `echo 3 > $FX/data/resume-tries`, run `resume`, assert `resume: blocked` and both files gone;
  seed the ROM, run `resume take`, assert `resume: ready` and `resume-tries` = 1 and the marker gone;
  seed the ROM, run `resume cancel`, assert both gone.
- [ ] **Step 2: Run to verify it fails** — `ui/tests/run.sh`; expected: the `resume:` lines are absent.
- [ ] **Step 3: Implement** — the enum and three functions in `main.cpp` (plain reads: `std::ifstream`/`access`, same style as `readSettings`), and the verb in `runHeadless` that applies them as described.
- [ ] **Step 4: Run to verify it passes**.
- [ ] **Step 5: Commit** — `git add ui/src/main.cpp ui/tests/run.sh && git commit -m "menu: the resume decision and its headless checks"`.

### Task 3: wire the live loop and the launch markers

**Files:**
- Modify: `ui/src/main.cpp` (`runGame`, the post-game block, startup before the loop, the input branch of the loop)

**Interfaces:**
- Consumes: Task 1's marker, Task 2's functions.
- Produces: `/run/banana/game.path` at launch; a `/run/banana/launch` write at the deadline.

- [ ] **Step 1: Mark the running game** — in `runGame`, where `/run/banana/game.pid` is written (after `pid > 0`), also write `rom` to `/run/banana/game.path`; in the post-game block (after the panel is back), `unlink("/run/banana/game.path")` and call `resumeCancel(o.data)`.
- [ ] **Step 2: Plan at startup** — after the menu is constructed and before the loop: `std::string resumeRom; Resume p = resumePlan(o.data, resumeRom); if (p == Resume::Ready) { resumePath = resumeRom; resumeDue = nowMs() + 2000; } else if (p != Resume::None) resumeCancel(o.data);`
- [ ] **Step 3: Act in the loop** — before the event dispatch: if `resumePath` is set and `now >= resumeDue`, write `resumePath` to `/run/banana/launch`, call `resumeTake(o.data)`, log `resume: launching <path>`, clear it. In the input branch, if a key or touch event arrives while `resumePath` is set, call `resumeCancel(o.data)`, log `resume: cancelled`, clear it.
- [ ] **Step 4: Build and run the host gates** — `make -s -C ui host`, `make -s test`; expected: green.
- [ ] **Step 5: Device check** — install (next image or bind-mounted menu): sleep in a game → long-press power → boot resumes it; shut down in the menu → menu; press a key in the 2 s window → menu, `resume: cancelled`; `rm /data/banana/resume` and boot → menu.
- [ ] **Step 6: Commit** — `git add ui/src/main.cpp && git commit -m "menu: resume the game a shutdown interrupted"`.

### Task 4: docs and roadmap

- [ ] **Step 1:** Add a paragraph to `docs/ARCHITECTURE.md` §3 (the menu) and a row to `docs/FIRST-BOOT.md`'s logs table if a log line is user-facing; note `banana/resume` and `banana/resume-tries` where `/data/banana` files are listed.
- [ ] **Step 2:** Move the item into `ROADMAP.md`'s *Done* with the measured result and the device checks; `make -s test`, `make agent-check`; commit.
