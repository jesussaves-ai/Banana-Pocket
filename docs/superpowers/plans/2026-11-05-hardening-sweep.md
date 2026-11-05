# Hardening sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** fix every defect the roadmap recon found and delete the tree's dead weight, shipping no new features.

**Architecture:** eight independently committable tasks, one per subsystem, each ending green on `make -s test`; the
two behaviour-carrying ones (the sleep hooks, the i18n values) are test-first.

**Tech Stack:** POSIX sh (busybox), C99 (bananad), C++17 (ui/, app/bananatube), Python 3 (tools/), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-05-hardening-sweep-design.md` (the item list, with the four calls)

## Global Constraints

- C99/C11 for `system/bananad/` with `-Wall -Wextra -Werror`; C++17 for `ui/src/` and `app/bananatube/` with
  `-fno-exceptions -fno-rtti`; keep `ui/rnds` parity for anything in `ui/src/`; no new dependency.
- After every task: `make -s test` and `make -s agent-check` green. A sound/UCM/`banana-ui` audio change also runs
  `make sound-test` on the handheld.
- Never touch `linux/`, `drivers/`, `sound/soc/qcom/` or Buildroot `package/` (the scope guard).
- `ui/src/soft.cpp`'s draw path keeps zero heap allocation; the damage-diff shape stays.

## Review Focus

- **A pidfile read from `cat`** (`rcK:6`, `usb-msc:65`, `sound-test:175`): `pid_of`'s new clamp must not change what
  those shell readers see.
- **`awake.pid` left behind by a killed job**: an update that dies must not block idle sleep forever (the pid's
  liveness is the guard, not the file's presence).
- **The single canvas**: `beginScreen(1)`/`endScreen()` must not be called by anything that still exists, or the
  renderer writes into a screen nothing presents.
- **A translated value with a count in it**: `tr()` matches whole strings, so a "N packages" form must be a key that
  actually occurs, not a template.
- **Deleting `docs/img/readme/`**: any README image link must go with it.

---

### Task 1: daemon safety (D1-D4)

**Files:**
- Modify: `system/rootfs/usr/libexec/banana/update`, `.../update-check`, `.../backup` (awake.pid)
- Modify: `system/rootfs/etc/init.d/rcK` (usb-msc off)
- Modify: `system/bananad/bananad.c` (wake label ~731-736, `pid_of` ~280-288)

**Interfaces:**
- Produces: `/run/banana/awake.pid` = a running job's pid, written on start and removed on exit (bananad's existing
  `pid_of("/run/banana/awake.pid")` checks are the consumer, `bananad.c:1911,1977`).

- [ ] **Step 1: awake.pid in the three long jobs.** In each script, next to the existing `STATUS`/work setup, write
  `echo $$ > /run/banana/awake.pid` and add a `trap 'rm -f /run/banana/awake.pid' EXIT INT TERM`. `update` writes it
  only in the `apply` branch (a `list` is instant).
- [ ] **Step 2: usb-msc off at shutdown.** In `rcK`, before the `/usr/libexec/banana/share down` line, add
  `/usr/libexec/banana/usb-msc off 2>/dev/null` so a card handed to a computer is taken back and mounted before the
  unmount.
- [ ] **Step 3: name the wake.** In `bananad.c`, keep the boolean but set the reason string from it: when
  `charger_online()` differs from `plugged0` (either direction), set `woke = "cable"` and skip the spurious counter;
  keep `"power button"` otherwise. Do not change the re-suspend count for a real spurious wake.
- [ ] **Step 4: clamp the file age in `pid_of`.** After computing `file`, add: if `file > up + 5` the wall clock has
  stepped since the file was written, so `file = 0` (trust the pid). Leave the `-3.0` slack.
- [ ] **Step 5: verify.** `make -s agent-check` (compiles bananad) and `system/bananad/tests/sleep-test.sh` pass; then
  `sh -n` each edited script.
- [ ] **Step 6: commit** — `git commit -m "bananad: an update cannot be slept through, and a step in the clock cannot fake a dead game"`.

### Task 2: the sleep.d hooks run in the test (D5)

**Files:**
- Modify: `system/bananad/tests/sleep-test.sh`

**Interfaces:**
- Consumes: `hooks_run()`'s contract — every executable in `/etc/banana/sleep.d`, in name order, with `pre` or `post`.

- [ ] **Step 1: write the failing check.** In the test's namespace setup, create a stand-in `/usr/libexec/banana`
  whose `wifi`, `sound-server`, `usb-gadget` are one-line recorders (`echo "$0 $*" >> "$CALLS"`), and assert after a
  sleep that `$CALLS` holds `wifi down`, `sound-server down`, `usb-gadget down` (in that order) and the matching `up`
  after the wake. Run it: it must fail (today the hooks no-op).
- [ ] **Step 2: make it pass.** Bind (or copy) the stand-in over `/usr/libexec/banana` in the test's namespace, and
  make `usb-gadget`'s stand-in report a bound UDC (the hook's `[ -n "$(cat $G/UDC)" ]` guard) so `30-usb` takes its
  real branch. Do not change the hooks.
- [ ] **Step 3: verify.** `sudo system/bananad/tests/sleep-test.sh` prints every existing check plus the new one, green.
- [ ] **Step 4: commit** — `git commit -m "sleep-test: the sleep.d hooks are exercised, not assumed"`.

### Task 3: BananaTube (A1-A6)

**Files:**
- Modify: `app/bananatube/main.cpp`; `system/tests/run-banana-tube-test.sh` if a selftest case is added

- [ ] **Step 1: A1 — lock the writes.** Wrap every assignment to `results` (`:580,758,769,787,797,614`) in
  `std::lock_guard<std::mutex> lk(searchLock);` (or give the thumbnail worker a snapshot taken under the lock — the
  snapshot is preferred if a lock is held while the worker waits).
- [ ] **Step 2: A2 — atomic conf.** Give `qualitySave` the same temp-file + `fflush`/`fsync` + `rename` body
  `recentSave` uses (`:669-681`); same for any other in-place writer found by grepping `fopen(..., "w")` in the file.
- [ ] **Step 3: A3 — gate the logging.** The two tap log sites (`:1591-1592`, `tapAt`'s per-hit lines) test the same
  `BANANA_TUBE_TOUCH_DEBUG` env var `:1374` does.
- [ ] **Step 4: A4 — accept a tracking id.** In `feedTouch`, set the down state when `ABS_MT_TRACKING_ID >= 0` as well
  as on `BTN_TOUCH`.
- [ ] **Step 5: A5 — take the first of each.** `resolveStream` accepts the first video URL and the first audio URL and
  ignores extra lines instead of failing above two.
- [ ] **Step 6: A6 — loop on a short write.** `ipcSend` writes the remainder on `EAGAIN`/partial writes.
- [ ] **Step 7: verify.** `make -s -C app/bananatube` clean; `system/tests/run-banana-tube-test.sh` green (27 checks).
- [ ] **Step 8: commit** — `git commit -m "bananatube: no race on the results, no truncated conf, and no per-tap log spam"`.

### Task 4: the UI's small fixes (U1-U4)

**Files:**
- Modify: `ui/src/library.cpp` (U1 hint, U4 gamelist), `ui/src/main.cpp` (U2 poll timeout), `ui/src/wifi.cpp` (U3)

- [ ] **Step 1: U1** — `Library::hints` adds the `X Options` chip for every system (drop the `nds` test).
- [ ] **Step 2: U2** — the poll timeout's short branch covers `LIBRARY` as well as `HOME` (`main.cpp:1845`).
- [ ] **Step 3: U3** — `wifi::Job`'s `active`/`done` become `std::atomic<bool>`; `take`/`start` keep their lock.
- [ ] **Step 4: U4** — `readGamelist` only treats a tag as a game when it is `<game` followed by a space or `>`.
- [ ] **Step 5: verify.** `make -s test` (ui tests + bench) and `make -s agent-check` green.
- [ ] **Step 6: commit** — `git commit -m "ui: the options chip matches the key, the library notices a new ROM, and no torn job flags"`.

### Task 5: one screen (U6, call 1)

**Files:**
- Modify: `ui/src/soft.cpp` (one canvas), `ui/src/kms.cpp`/`kms.h` if the present loop's `count` use changes,
  `ui/src/layout.cpp` (the `screen < 2` loop), `ui/src/main.cpp`, `ui/src/settings.h`, `ui/src/settings.cpp` (comments),
  `ui/assets/rnds/systems.cfg` + the dead art

- [ ] **Step 1: one canvas.** `SoftBackend` allocates `scr[0]`/`dmg[0]` only; any `beginScreen(1)` caller is gone.
- [ ] **Step 2: one screen.** `layout.cpp`'s render loop drops to `screen == 0` (keep the `screen` var at 0 for the
  theme expressions that read it).
- [ ] **Step 3: delete the dead rows and art.** The `rocknixds`/`rocknixdsbank`/`dodakvarter` lines in
  `ui/assets/rnds/systems.cfg` and the images only they use.
- [ ] **Step 4: correct the comments** in `settings.h:210`, `settings.cpp:1860,2035`, `main.cpp:1243-1246`.
- [ ] **Step 5: verify.** `make -s test` green (the ui tests + a host render), `make -s agent-check` green; check the
  menu still draws by running the app once under the host render used in the Pocket-shell task.
- [ ] **Step 6: commit** — `git commit -m "ui: one panel, so one canvas (the second screen was rendered and never shown)"`.

### Task 6: the untranslated values (U5)

**Files:**
- Modify: `ui/assets/lang/en.json` (+ `es.json`, `de.json`), `ui/src/settings.cpp`, `tools/check-lang.py`

- [ ] **Step 1: write the failing check.** In `tools/check-lang.py`, add a rule: every string literal passed to `tr(`
  in `ui/src/*.cpp` must be a key in `en.json`. Run it — it must fail, naming `updatesWaiting`/`scrapeShort`'s values.
- [ ] **Step 2: add the keys.** `"1 package"`, `"N packages"` (the exact strings that occur), `"Not yet"`,
  `"Starting"`, `"N of M found"`, `"No connection"`, `"Didn't work"` — and their es/de translations.
- [ ] **Step 3: make the count forms occur.** Where the count varies, the draw site picks the whole key (`"1 package"`
  vs `"N packages"`) rather than building a string the table cannot match.
- [ ] **Step 4: verify.** `tools/check-lang.py` green, `make -s test` green.
- [ ] **Step 5: commit** — `git commit -m "i18n: the update and box-art values are keys, and the checker can see a hardcoded one"`.

### Task 7: delete the dead weight, correct the docs

**Files:** the deletions and the corrections listed in the spec's *stale items* section

- [ ] **Step 1: delete** `system/rootfs/etc/fstab`, `system/rootfs/etc/banana/accounts.conf.example`,
  `tools/make-console.py`, `tools/qemu-e2e.sh`, `docs/img/readme/`, the stray `bgm.wav` (untracked, so `rm` only), and
  any README link to the deleted images (`rg -n 'img/readme' README.md docs`).
- [ ] **Step 2: verify the three conf questions** on the tree (does `/data/banana/bananad.conf` get created from the
  image's? is `autosleep_minutes` really overridden? does the boot honour `usb_mass_storage`?), and correct what is
  actually wrong — a conf line that is never read goes, a default that is always overridden is fixed at the default.
- [ ] **Step 3: correct the docs** — one edit per item in the spec's *doc corrections* paragraph.
- [ ] **Step 4: verify.** `make -s test` green; `rg -n` each deleted path returns nothing.
- [ ] **Step 5: commit** — `git commit -m "docs: the tree tells the truth, and the dead weight goes"`.

### Task 8: CI fragility, and close

**Files:** `.github/workflows/ci.yml`, `Makefile`, `docs/ROADMAP.md`

- [ ] **Step 1:** the `cores` job's path filter also matches `buildroot/patches/libretro-*/**`.
- [ ] **Step 2:** the `sh -n` loop skips non-shell files (`case` on the shebang, or an explicit `*.sh` + known list).
- [ ] **Step 3:** `make test` gains the bananad compile when host ALSA is present (the same guard `agent-check` uses).
- [ ] **Step 4: ROADMAP** — a Done entry for the sweep, and the `amixer`-to-`snd_mixer_*` rewrite added to Recommended
  next (call 2).
- [ ] **Step 5: verify.** `make -s test`, `make -s agent-check` green; the CI YAML parses (`python3 -c "import yaml, sys;
  yaml.safe_load(open('.github/workflows/ci.yml'))"`).
- [ ] **Step 6: commit** — `git commit -m "ci: a core patch rebuilds, and make test compiles bananad"`.

## Device steps (when the unit is awake)

- `system/bananad/tests/sleep-test.sh` covers D1/D4's logic on the host; confirm on the unit that an `update apply`
  keeps the SoC awake, and that unplugging the charger during a sleep logs `cable`, not `spurious`.
- `make sound-test` after the sweep (nothing here should touch audio, but the build changed).
