# Banana Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `bananad` shows volume, brightness, battery and achievement events inside a running game through RetroArch's own message line, and pauses the game when the headphones are pulled.

**Architecture:** One process (`bananad`) formats one line and writes `SHOW_MSG <text>` to `/run/banana/retroarch.cmd`, the FIFO that is a running RetroArch's stdin (`ra_cmd()` already exists at `bananad.c:1812`). Achievements are read from `/run/banana/cheevos` through an inotify watch. No new process, no socket, no launcher change.

**Tech Stack:** C11 (`-Wall -Wextra -Werror`), POSIX, inotify, existing ALSA lib link; C++17 for the one menu change.

**Spec:** `docs/superpowers/specs/2026-11-05-banana-bridge-design.md`

## Global Constraints

- No new dependency, package, or process; `bananad` keeps its `-Wall -Wextra -Werror` build.
- The menu's card path (`osd()` → `/run/banana/osd` + SIGUSR2) stays exactly as it is; the HUD never writes `/run/banana/osd`.
- `hud = 0` disables **both** the text and the headphone pause.
- Achievement titles are the game's own text and are never translated; all other strings come from the daemon's own `en`/`es`/`de` table selected by `lang` in `settings.conf`.
- BananaTube and PortMaster ports are out of scope; the event surface only writes to the RetroArch FIFO.
- Any new user-visible menu string goes into all three catalogs (`ui/assets/lang/{en,es,de}.json`); `tools/check-lang.py` must report 0 problems and every catalog must hold the same key count.
- Verification gate: `make -s test` and `make -s agent-check` both green.

## Review Focus

- A volume key in the **menu** (no game) must still raise the existing pill, not write to the FIFO.
- `hud = 0` with a game running: no line appears and the jack does not pause; the menu's post-game toasts still work.
- An achievement title containing `%`, a tab, or a newline must not break the one-line command or the format string.
- A new game's `cheevos` file starts at size 0 after an old game's file: no old achievement may be re-shown.
- A game whose stdin is never read (FIFO full) or already gone (EPIPE) must not block or kill the daemon.

---

### Task 1: The daemon's line, and a self-test that can prove it

**Files:**
- Modify: `system/bananad/bananad.c` (cfg struct `:53-72`, prototypes `:78-80`, `ra_cmd` area `:1812`, `main` `:1990`)
- Create: `system/bananad/tests/hud-test.sh`
- Modify: `Makefile:92-104` (the `test:` recipe)

**Interfaces:**
- Produces:
  - `static int cfg.hud` (default `1`), `static char cfg.lang[8]` (default `"en"`)
  - `static int hud_on(void)` — 1 when `cfg.hud && pid_of("/run/banana/game.pid") > 0`
  - `static void hud_fmt(int key, int value, char *buf, size_t n)` — `key` is one of `HUD_VOLUME, HUD_BRIGHT, HUD_LOW, HUD_EMPTY`; `value` is a percent (ignored by `HUD_EMPTY`)
  - `static void hud_show(const char *text)` — `SHOW_MSG <text>` through `ra_cmd` when `hud_on()`
  - `static int selftest_hud(void)` — exit status 0 when every expectation below held

- [ ] **Step 1: Write the failing test**

Create `system/bananad/tests/hud-test.sh`:

```sh
#!/bin/sh
# The bridge's text, tested without the handheld: bananad's own formatter and the achievement parser, run through
# the --selftest hook the same way `banana-tube --selftest` is tested.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
cc -O2 -Wall -Wextra -o "$W/bananad" "$HERE/bananad.c" -lasound -lm

out=$("$W/bananad" --selftest hud)
printf '%s\n' "$out"

fail=0
want() { printf '%s\n' "$out" | grep -qxF "$1" || { echo "FAIL: wanted \"$1\"" >&2; fail=1; }; }
want 'hud en: Volume 62 %'
want 'hud en: Brightness 80 %'
want 'hud en: Battery low: 9 %'
want 'hud en: Battery empty, shutting down'
want 'hud es: Volumen 62 %'
want 'hud de: Akku schwach: 9 %'
want 'hud off: 0'
want 'hud no game: 0'
want 'cheevos: Sonic (+15)'
want 'cheevos odd: 100% Fun (+15)'
want 'cheevos 4 fields: 0'
want 'cheevos 6 fields: 0'
want 'cheevos no points: Sonic'
has() { grep -qF "$2" "$1" || { echo "FAIL: $1 lacks $2" >&2; fail=1; }; }
has "$HERE/bananad.c" 'signal(SIGPIPE, SIG_IGN);'
[ "$fail" = 0 ] && echo "hud-test: all checks passed"
exit "$fail"
```

- [ ] **Step 2: Run it and watch it fail**

Run: `chmod +x system/bananad/tests/hud-test.sh && sh system/bananad/tests/hud-test.sh`
Expected: bananad starts as the daemon and never prints (the test then fails on the first `want`); `--selftest` is not implemented.

- [ ] **Step 3: Implement the formatter and the parser**

In `bananad.c`:

- `cfg`: add `int hud;` and `char lang[8];`; extend the initialiser to `…, "", 0, 1, "en"` so the two new fields are `hud = 1`, `lang = "en"`. Add to `load_conf`: `else if (!strcmp(k, "hud")) cfg.hud = strcmp(v, "off") && strcmp(v, "0") ? 1 : 0;` and `else if (!strcmp(k, "lang")) snprintf(cfg.lang, sizeof cfg.lang, "%s", v);`. Add `hud = 1` to `system/rootfs/etc/banana/bananad.conf` under `vibrate_ms` (the image's default; `/data` overrides it).
- `settings.conf`'s `lang` is read by a new `static void load_lang(void)` next to `load_timezone()` (`:158`), same shape: open `/data/banana/settings.conf`, find `lang =`, copy the value. Call it in `main()` right after `load_timezone()`.
- The string table, above `ra_cmd`:

```c
/* the bridge's text: the daemon speaks its own three languages (see the shortcut note in hud_fmt) */
enum { HUD_VOLUME, HUD_BRIGHT, HUD_LOW, HUD_EMPTY, HUD_NKEYS };
static const char *const hud_strings[3][HUD_NKEYS] = {
	{ "Volume %d %%", "Brightness %d %%", "Battery low: %d %%", "Battery empty, shutting down" },
	{ "Volumen %d %%", "Brillo %d %%", "Batería baja: %d %%", "Batería vacía, apagando" },
	{ "Lautstärke %d %%", "Helligkeit %d %%", "Akku schwach: %d %%", "Akku leer, fährt herunter" },
};
```

- `hud_fmt`: pick the row (`es` → 1, `de` → 2, else 0), `snprintf(buf, n, hud_strings[l][key], value)`; for `HUD_EMPTY` copy the string unchanged (it has no `%d`). One-line comment above the table: `shortcut: the bridge carries its own three-language table instead of the JSON catalogs; when a fourth language lands, fold these four strings into the catalogs and generate the table at boot.`
- `hud_on`: `return cfg.hud && pid_of("/run/banana/game.pid") > 0;`
- `hud_show(const char *text)`: `if (!hud_on()) return;` then `char line[256]; snprintf(line, sizeof line, "SHOW_MSG %s", text); ra_cmd(line);` (declared in the prototype block so `volume()`/`brightness()` above it can call it).
- `cheevos_line(const char *line, char *out, size_t n)` → `int`: copy into `char c[1024]`, split on `\t` into 7 fields with `strtok_r`. Return 0 unless all 7 exist and field 3 (index 2, the title) is non-empty. Sanitise the title into `out`: `\t`, `\r`, `\n` become `' '`, capped at 120 bytes. `points` = field 5 (index 4); return 0 without filling `out` when it is empty or all `'0'`. Append `" (+<points>)"` when the points are non-zero, then `hud_show(out)` and return 1 (the caller and the self-test both read `out`).
- `signal(SIGPIPE, SIG_IGN);` in `main()` next to `signal(SIGCHLD, SIG_DFL);` — a game that closes its stdin between the FIFO open and the write must not kill the daemon.
- `selftest_hud` (called from `main` before `openlog`: `if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest_hud();`, replacing `(void)argc; (void)argv;`). It prints exactly, one per line, to stdout:

```
hud <lang>: <hud_fmt output>        for en Volume 62, en Brightness 80, en Battery low 9, en Battery empty,
                                    es Volume 62, de Battery low 9
hud off: <hud_on() with cfg.hud = 0>            (0, and the field restored after)
hud no game: <hud_on() with cfg.hud = 1>        (0 when /run/banana/game.pid is absent)
cheevos: <cheevos_line("…") of a 7-field line titled Sonic with points 15>   (1)
cheevos odd: <cheevos_line of a 7-field line titled "100%\tFun", points 15>  (1, the tab shown as one space)
cheevos 4 fields: <cheevos_line of "1\t2\tSonic\tDesc">                       (0)
cheevos 6 fields: <cheevos_line of six fields, 15 points>                     (0)
cheevos no points: <cheevos_line of a 7-field line with points 0>             (1, no "(+0)")
```

The "cheevos" lines print the formatted result (`Sonic (+15)`, `Sonic`), so the test above can grep them; the numbered ones print the return value. Add a short header comment saying it runs no daemon and touches nothing.

- [ ] **Step 4: Run the test and the builds**

Run: `sh system/bananad/tests/hud-test.sh` → `hud-test: all checks passed`.
Run: `make -s -C system/bananad` → no output.
Run: `make -s agent-check` → `bananad: OK`.

- [ ] **Step 5: Wire the test into `make test` and commit**

Add `system/bananad/tests/hud-test.sh` to the `test:` recipe in `Makefile` after `system/tests/sound-config-test.sh`.

```bash
git add system/bananad/bananad.c system/bananad/tests/hud-test.sh system/rootfs/etc/banana/bananad.conf Makefile
git commit -m "daemon: a line into the game, and a self-test that proves it"
```

---

### Task 2: The four events

**Files:**
- Modify: `system/bananad/bananad.c` — `volume()` `:1358-1381`, `brightness()` `:1383-1407`, the `EV_SW` branch in `handle()` `:1829-1832`, the battery tick in `main()` `:2115-2122`

**Interfaces:**
- Consumes: `hud_fmt`, `hud_show`, `hud_on` from Task 1; `ra_cmd` (`:1812`).

- [ ] **Step 1: Volume and brightness, menu vs game**

In `volume()` replace the last two lines

```c
	if (pid_of("/run/banana/game.pid") <= 0)
		osd("volume", pct);
```

with

```c
	if (pid_of("/run/banana/game.pid") > 0) { char b[96]; hud_fmt(HUD_VOLUME, pct, b, sizeof b); hud_show(b); }
	else osd("volume", pct);
```

and rewrite the comment above it to say the game gets the same number as a line in RetroArch's message box, the menu gets its pill. In `brightness()` do the same at `:1405` with `HUD_BRIGHT`.

- [ ] **Step 2: The headphone unplug pauses the game**

In the `EV_SW` branch of `handle()` (`:1829`), when the state changes:

```c
	if (e->type == EV_SW && e->code == SW_HEADPHONE_INSERT) {
		if (headphones != e->value) {
			/* unplugged while a game runs: it would blast the speaker, so pause it -- the same command F+B's
			 * sibling uses; plugging back in does not resume (the player decides) */
			if (!e->value && hud_on()) ra_cmd("PAUSE_TOGGLE");
			headphones = e->value;
			audio_route();
		}
		return;
	}
```

- [ ] **Step 3: The battery warnings**

In the battery tick (`main()` `:2115`), before `if (low_count >= 2)`:

```c
			/* two thresholds, once each, and only while a game is on top (the menu has the charge icon) */
			static int warned = 1000;
			if (pct >= 0 && pct <= 10) {
				int band = pct <= 5 ? 5 : 10;
				if (band != warned) { char b[96]; warned = band; hud_fmt(HUD_LOW, pct, b, sizeof b); hud_show(b); }
			} else if (pct > 10) warned = 1000;
			/* the first reading at or under the floor warms while the second shuts down (a load spike is not a
			 * shutdown), so the line is on screen for the whole of that minute */
			if (pct >= 0 && pct <= cfg.battery_off_pct && low_count == 0) {
				char b[96]; hud_fmt(HUD_EMPTY, 0, b, sizeof b); hud_show(b);
			}
```

(`low_count` is incremented on the next line, so this is the first reading at or under the floor.)

- [ ] **Step 4: Verify**

The menu path must survive the edit, so pin it in the test (append to `system/bananad/tests/hud-test.sh`, before the final `[ "$fail" = 0 ]`):

```sh
has() { grep -qF "$2" "$1" || { echo "FAIL: $1 lacks $2" >&2; fail=1; }; }
has "$HERE/bananad.c" 'else osd("volume", pct);'
has "$HERE/bananad.c" 'ra_cmd("PAUSE_TOGGLE")'
```

Run: `sh system/bananad/tests/hud-test.sh` → passes.
Run: `make -s test` → all suites pass (the bridge changed no test's expectations).
Run: `make -s agent-check` → `bananad: OK`.

- [ ] **Step 5: Commit**

```bash
git add system/bananad/bananad.c system/bananad/tests/hud-test.sh
git commit -m "daemon: volume, brightness, battery and the jack speak to the game"
```

---

### Task 3: Achievements as they unlock

**Files:**
- Modify: `system/bananad/bananad.c` — the inotify setup `:2008-2013`, the directory-event loop `:2094-2102`
- Modify: `ui/src/main.cpp:1628-1639` (the post-game cheevos replay)

**Interfaces:**
- Consumes: `cheevos_line(const char *)` from Task 1.
- Produces: `/run/banana/hud-live` (created by `bananad` when it showed an unlock; deleted by `banana-ui` after its decision).

- [ ] **Step 1: The watch and the read**

Add above `cheevos_line`'s area:

```c
static int ino_cheev = -1;	/* the achievement file's own watch: the directory only reports its creation */
static long cheevos_off;	/* where the last read stopped; a shrunken file means a new game */

static void cheevos_watch(int ino)
{
	if (ino < 0) return;
	if (ino_cheev >= 0) inotify_rm_watch(ino, ino_cheev);
	ino_cheev = inotify_add_watch(ino, "/run/banana/cheevos", IN_MODIFY | IN_CLOSE_WRITE);
}

static void cheevos_poll(void)
{
	FILE *f = fopen("/run/banana/cheevos", "r");
	if (!f) { cheevos_off = 0; return; }
	if (fseek(f, 0, SEEK_END) || ftell(f) < cheevos_off) cheevos_off = 0;	/* a new game's file starts empty */
	fseek(f, cheevos_off, SEEK_SET);
	char ln[1024], buf[160];
	while (fgets(ln, sizeof ln, f)) {
		cheevos_off += (long)strlen(ln);
		if (cheevos_line(ln, buf, sizeof buf)) {
			FILE *m = fopen("/run/banana/hud-live", "w");	/* the menu must not toast it again */
			if (m) fclose(m);
		}
	}
	fclose(f);
}
```

Call `cheevos_watch(ino);` once after the two `inotify_add_watch` calls in `main()`. In the directory-event loop add `int cheevos_changed = 0;` and, next to the `vibrate` test:

```c
					if (e->len && e->wd == ino_run && !strcmp(e->name, "cheevos")) cheevos_changed = 1;
					if (e->wd == ino_cheev) cheevos_changed = 1;
```

and after the loop: `if (cheevos_changed) { cheevos_watch(ino); cheevos_poll(); }` (the re-add catches the file's deletion and re-creation between games).

- [ ] **Step 2: The menu stops repeating them**

In `ui/src/main.cpp`, wrap the replay:

```cpp
			// Bananad showed these as they unlocked (the marker), so the menu only clears it; without the marker
			// (HUD off, or a daemon that predates it) the menu's own toasts are the only place they appear.
			if (access("/run/banana/hud-live", F_OK) != 0)
			{
				std::ifstream ch(CHEEVOS);
				std::string line;
				while (std::getline(ch, line))
				{
					std::vector<std::string> f = tsv(line, 7);
					if (f[0].empty() || f[1].empty()) continue;
					settingsView.notify(f[2], f[3], 0, f[6]);
				}
			}
			unlink("/run/banana/hud-live");
```

Progress recording (`recordPlay`) above it is untouched.

- [ ] **Step 3: Verify**

Run: `sh system/bananad/tests/hud-test.sh` → passes.
Run: `make -s test` → green, including `banana-ui: all checks passed`.
Run: `make -s agent-check` → `sh: OK`, `ui: OK`, `bananad: OK`.

- [ ] **Step 4: Commit**

```bash
git add system/bananad/bananad.c ui/src/main.cpp
git commit -m "daemon: achievements show as they unlock, and the menu stops repeating them"
```

---

### Task 4: The switch, in the menu and in the image

**Files:**
- Modify: `ui/src/settings.cpp` (the `S` = System block, `:322-343`)
- Modify: `ui/assets/lang/{en,es,de}.json`

**Interfaces:**
- Consumes: `cfg.hud` / `cfg.lang` parsing from Task 1 (the file is `/data/bananad.conf`, key `hud`, values `1`/`0`).

- [ ] **Step 1: The row**

In the System block, immediately before `add(S, "gear", "First-time setup", …)`:

```cpp
		add(S, "picture", "On-screen info while playing",
			"While a game runs, a change of volume or brightness and a low battery show as a line inside the game, achievements appear as they unlock, and pulling the headphones pauses it. Off: the screen is the game's alone; the menu's own cards are unchanged.",
			power, "hud", { "On", "Off" }, { "1", "0" });
```

- [ ] **Step 2: The catalog entries**

Add both strings to each catalog's `strings` object (alphabetical position does not matter; keep the file's two-space indent):

```json
    "On-screen info while playing": "On-screen info while playing",
    "While a game runs, a change of volume or brightness and a low battery show as a line inside the game, achievements appear as they unlock, and pulling the headphones pauses it. Off: the screen is the game's alone; the menu's own cards are unchanged.",
```

Spanish (`es.json`):

```json
    "On-screen info while playing": "Información en pantalla al jugar",
    "While a game runs, a change of volume or brightness and a low battery show as a line inside the game, achievements appear as they unlock, and pulling the headphones pauses it. Off: the screen is the game's alone; the menu's own cards are unchanged.": "Mientras juegas, un cambio de volumen o brillo y una batería baja aparecen como una línea dentro del juego, los logros salen al desbloquearse y quitar los auriculares lo pausa. Desactivado: la pantalla es solo del juego; las tarjetas del menú no cambian.",
```

German (`de.json`):

```json
    "On-screen info while playing": "Bildschirminfo beim Spielen",
    "While a game runs, a change of volume or brightness and a low battery show as a line inside the game, achievements appear as they unlock, and pulling the headphones pauses it. Off: the screen is the game's alone; the menu's own cards are unchanged.": "Während ein Spiel läuft, erscheinen eine Änderung von Lautstärke oder Helligkeit und ein schwacher Akku als Zeile im Spiel, Erfolge kommen beim Freischalten, und das Ziehen der Kopfhörer pausiert es. Aus: Der Bildschirm gehört dem Spiel allein; die Karten des Menüs bleiben, wie sie sind.",
```

- [ ] **Step 3: Verify**

Run: `tools/check-lang.py` → `3 catalog(s), 0 problem(s)` and each catalog reports the same count (132).
Run: `make -s test` → `banana-ui: all checks passed`, `check-lang: 0 problems`.
Run: `make -s agent-check` → all four lines OK.

- [ ] **Step 4: Commit**

```bash
git add ui/src/settings.cpp ui/assets/lang/en.json ui/assets/lang/es.json ui/assets/lang/de.json
git commit -m "settings: a switch for the in-game line, in three languages"
```

---

### Task 5: The tree tells the truth

**Files:**
- Modify: `docs/ARCHITECTURE.md:25-35` (§4)
- Modify: `docs/ROADMAP.md` (*Ideas* entry `:87`, *Done* list)

- [ ] **Step 1: ARCHITECTURE §4**

Add one bullet under **Input/Hotkeys** or as its own line in §4:

```markdown
- **In-game line (Banana Bridge):** volume/brightness/battery/achievement text goes to a running game as `SHOW_MSG` on `/run/banana/retroarch.cmd` (RetroArch's stdin FIFO); `hud = 0` in `bananad.conf` turns it and the headphone-unplug pause off. The menu's `/run/banana/osd` card is untouched, and achievements shown live leave `/run/banana/hud-live` so the menu does not toast them again.
```

- [ ] **Step 2: ROADMAP**

Delete the *Banana Bridge (next)* bullet from *Ideas (unscheduled)* and add a Done entry at the top of *Done*:

```markdown
- [x] **Banana Bridge (0.5.11).** While a game runs, `bananad` speaks through RetroArch's own message line: volume and
  brightness keys, a low battery (10 % and 5 %) and the shutdown at the floor, the unplugged headphones pausing the
  game, and achievements as they unlock (an inotify watch on `/run/banana/cheevos`, with `/run/banana/hud-live` so the
  menu does not toast them again). Four strings in three languages, one Settings row (`hud`, default on), and no new
  process or socket: the FIFO the daemon already writes was the whole channel.
```

- [ ] **Step 3: Commit**

```bash
git add docs/ARCHITECTURE.md docs/ROADMAP.md
git commit -m "docs: the bridge is built"
```

---

### Task 6: On the handheld

**Files:** none (verification only)

- [ ] **Step 1: Build and push the daemon and the menu**

Cross-build `bananad` with the image toolchain (`output/retroid-pocket-classic/host/bin/aarch64-linux-gcc … -lasound -lm`) and `banana-ui` with `tools/push-ui.sh` after a local `make -s ui`; stream both to the unit the way `tools/rpc-push-update.sh` does (`cat file | ssh root@192.168.1.112 'cat > /tmp/…'`), then `bind`-mount or restart per the unit's current layout. Do not reboot into a new image for this test.

- [ ] **Step 2: The four events, in a game**

Start any game, then: volume up/down (a line appears in the game and the level changes), F+volume (brightness line), `printf '1\t1\tTest\tDesc\t15\t0\t\n' >> /run/banana/cheevos` (a `Test (+15)` line appears within a second), and unplug the headphones (the game pauses and audio reroutes to the speaker). Plug them back in and confirm audio returns and the game stays paused; resume by hand. Then quit and start a **second** game: the first game's `Test` line must not appear again (the new file is shorter, so the daemon starts it from zero).

- [ ] **Step 3: The dialogs that must not change**

Quit to the menu and confirm the same achievement is **not** toasted there. Set `hud = 0` in `/data/banana/bananad.conf`, `SIGHUP` the daemon (`killall -HUP bananad`), repeat a volume key in a game (nothing appears) and an unplug (no pause), then play a game to a new unlock and confirm the menu toasts it on exit.

- [ ] **Step 4: Report**

Record what was seen, including whether the menu's toast was skipped when `hud = 1`, and note the one thing the design left open: whether RetroArch's default message timeout is long enough (if a line vanishes too fast to read on the unit, the fix is a longer `SHOW_MSG` timeout in `run-retroarch`, not a code change in the daemon).
