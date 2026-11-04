# Resume the last game at boot

## Intent

Power on should land where you left off: if a game was running when the handheld shut down, boot
into that game (RetroArch's Quick resume state loads it); if it shut down in the menu, boot into the
menu. Fewer steps from power-on to playing, and no boot loop a wedged game could cause.

## What counts as "left off"

The last **visible** state at shutdown:
- Shutdown while a game runs (long-press power, low battery, Settings > Restart/Shut down from the
  menu cannot happen mid-game) -> resume that game.
- Shutdown in the menu -> menu. A stale marker is removed.
- A crash/hard reset does **not** create a marker (only a clean shutdown does), so a wedged game
  cannot by itself cause a resume at the next boot.

## How

- **Launch:** the menu writes the running game's absolute ROM path to `/run/banana/game.path`, next to
  the existing `game.pid`. It removes both when the game returns to the menu.
- **Shutdown:** `bananad`'s `shutdown_now()` reads `game.path` while the game is still running, writes
  it to `/data/banana/resume` (persistent), and removes that file when no game runs.
- **Boot:** after the menu is ready, if `/data/banana/resume` names an existing ROM, the menu uses the
  same search-and-launch path as `/run/banana/launch` (openLibrary + launch), then deletes the marker.
  A missing ROM (card changed) clears the marker and shows the menu, with a log line.
- **Cancel:** any input in the first ~2 s after the menu is ready cancels the resume, deletes the
  marker, and stays in the menu. Logged as `resume: cancelled`.
- **Loop guard:** `/data/banana/resume-tries` counts auto-launches; at 3 the menu skips the resume,
  deletes the marker and logs it. The counter is reset when the menu is shown with no marker.

## Files and size

`ui/src/main.cpp` (marker at launch/exit, the boot hook, the window, the guard) and
`system/bananad/bananad.c` (persist at shutdown). Estimated 100-130 lines total, no new screen, no
per-frame work (one `stat`/read at startup and one at shutdown).

## Tests

- UI headless: a seeded marker + ROM starts the game (log line, marker consumed); a key inside the
  window cancels and stays in the menu; a missing ROM clears the marker; the tries cap skips.
- Sleep test: `shutdown_now()` writes `/data/banana/resume` when `game.pid` + `game.path` exist, and
  leaves none when the menu is showing.

## Non-goals

Skipping the boot animation when resuming (a separate, measurable idea); resuming after a crash;
per-game or per-setting resume options; any new page.
