# Banana Bridge — the OS keeps a voice in game

Date: 2026-11-05
Status: design, awaiting approval

## Intent

While a game runs, `banana-ui` is not drawing: `runGame()` closes KMS and ALSA, forks `cage`, and waits
(`ui/src/main.cpp:1542-1648`). `bananad` deliberately suppresses its on-screen card in that window
(`pid_of("/run/banana/game.pid")`, `system/bananad/bananad.c:1380`), so the user gets no feedback for the
volume keys, the brightness keys, a low battery, or a headphone unplug, and achievements earned during play
are only toasted after the game quits (`ui/src/main.cpp:1628-1639`).

The bridge gives the OS one text line over whatever is on screen, using each app's own OSD.

## Goals

- Volume and brightness keys show their value while a game runs.
- A low battery and the automatic shutdown show a warning while a game runs.
- Pulling the headphones pauses the game instead of blasting the speaker.
- Achievements appear as they unlock, not after the session.
- The menu keeps its current toast behaviour, unchanged, when no game is running.

## Non-goals

- No overlay window, compositor, DBus, or new package.
- BananaTube and PortMaster ports are out of scope for v1: BananaTube has its own player OSD and mpv IPC;
  ports have no text surface in common. The event surface must not preclude them later.
- No clock, FPS counter, or netplay announcements.
- No change to how achievements are recorded (`playstats.tsv`, `/run/banana/cheevos` stay as they are).

## Anti-goals

- A second process watching the filesystem, duplicating what the daemon already watches.
- Network sockets on the handheld (RetroArch's command port binds outside loopback in some builds).
- English-only text when the menu already speaks three languages.

## What exists to build on

- `bananad` already opens `/run/banana/retroarch.cmd` non-blocking and writes `AUDIO_REINIT`
  (`bananad.c:1818-1822`); that FIFO is RetroArch's stdin.
- `run-retroarch` writes `stdin_cmd_enable = "true"` into the appendconfig for every launch
  (`system/rootfs/usr/libexec/banana/run-retroarch:394`), and RetroArch 1.22.2 on the unit carries
  `SHOW_MSG` and `PAUSE_TOGGLE`.
- `bananad` already watches `/run/banana` with inotify (`:2008-2013`), owns every value the HUD needs
  (volume `:1226-1252`, brightness `:1385-1424`, battery `:1539-1560`, jack `:1829-1832`), and knows
  whether a game is running.
- The achievement file is appended by the patched RetroArch as each achievement unlocks
  (`buildroot/patches/retroarch/0013`: 7 tab fields, `unlocked, total, title, description, points, rarity, badge`).

## Approaches

1. **Daemon-side (chosen).** `bananad` formats the line and writes it to the game's FIFO, plus tails the
   achievement file. One process, no launcher change, reuses the existing watch and the existing writer.
   Cost: the daemon grows by ~120 lines and carries six translated strings.
2. **Separate `system/bridge/` process started by `run-retroarch`.** Cleaner separation, and the natural home
   for BananaTube/mpv later. Cost: a new binary, a launcher change, a second inotify watcher, and a lifetime
   problem (it must not outlive the game); more moving parts for the same four events.
3. **Turn on RetroArch's own notifications** (`notification_show_*`, cheevos notifications). Almost free, but
   it only covers what RetroArch knows: no brightness, no battery, no headphone pause, no OS styling or
   language. Rejected as the primary path; it would also duplicate the menu's toasts.

Approach 1 wins on the smallest complete change. The event text is the contract; if BananaTube or a port ever
needs it, the same formatter moves behind a FIFO without touching callers.

## Design

### Channel

`bananad` gains `hud_show(const char *text)`: when `/run/banana/game.pid` names a live process, write
`SHOW_MSG <text>\n` to `/run/banana/retroarch.cmd` (already opened non-blocking, `SIGPIPE` ignored), and
nothing otherwise. Failures are silent — a game that does not read its stdin is not an error.

The menu's card path is untouched: `osd()` keeps writing `/run/banana/osd` exactly as today, and that file is
not written while a game runs.

### Events

| Trigger | Line |
|---|---|
| volume key | `Volume 62 %` |
| brightness key | `Brightness 80 %` (uses the existing permille value) |
| battery at 10 %/5 % while discharging | `Battery low: 9 %` (once per threshold) |
| shutdown at 3 % | `Battery empty, shutting down` |
| headphone unplug | `PAUSE_TOGGLE` (no text; the jack handler already reroutes audio) |
| achievement unlock | `<title> (+<points>)` |

Strings come from a small `en`/`es`/`de` table in `bananad.c`, selected by `lang` in `settings.conf`
(default `en`). Achievement titles are the game's own text and are not translated.
`shortcut: the HUD carries its own three-language table instead of the JSON catalogs; when a fourth language
lands, fold these six strings into the catalogs and have banana-ui generate the table at boot.`

The refresh rate is the key autorepeat (150 ms); RetroArch replaces its OSD text in place, so no dedupe is
needed beyond the battery thresholds.

### Achievements

`bananad` reads `/run/banana/cheevos` from the last offset whenever the directory watch reports it, parses tab
fields, and shows the newest line. Offset resets to 0 when the file shrinks or a game starts.

To avoid a duplicate toast when the game quits, `bananad` touches `/run/banana/hud-live` when it takes over
the achievement file and removes it at shutdown/sleep. `ui/src/main.cpp` skips its cheevos toast replay when
that marker exists; progress recording (`ra-progress` → `recordPlay`) is unaffected.

### Settings

One row, "On-screen info while playing", in Settings > System, `hud = 1` in `settings.conf` (default on).
`bananad` reads `settings.conf` already (`:158`); `hud = 0` disables both the text and the pause.

## Files

- `system/bananad/bananad.c` — `hud_show`, `hud_text`, `cheevos_poll`, the jack/battery/volume/brightness call
  sites, `hud` setting.
- `system/bananad/tests/hud-test.sh` (or extend `system/tests/`) — drives `bananad --selftest hud` (new
  switch, same style as `banana-tube --selftest`), asserting the formatted lines for the three languages, the
  achievement parser (4/5/6-field lines, a truncated line), and the no-game path writing nothing.
- `ui/src/main.cpp` — skip the cheevos replay when `/run/banana/hud-live` exists.
- `ui/src/settings.cpp`, `ui/assets/lang/{en,es,de}.json` — the row and its keys.
- `docs/ARCHITECTURE.md` §4, `docs/ROADMAP.md` (the idea moves to Done).

## Verification

- `make -s test`, `make -s agent-check`, `sh -n` on any touched script.
- On the unit: launch a game, press volume/brightness and watch the OSD; inject a line into
  `/run/banana/cheevos` with a game running and watch the toast; unplug headphones and confirm the game
  pauses; confirm the menu still toasts after a game where the HUD was disabled.
- `make -s sound-test` is not required (no mixer or routing change), but the jack path is touched: once, by
  hand, unplug and re-plug to confirm audio still reroutes.

## Left to the builder

- Exact wording for the three languages.
- Whether the battery thresholds are 10/5 or 15/5 (behaviour, not structure).
- Whether `SHOW_MSG`'s timeout should be extended (RetroArch has no per-message length in the command).
