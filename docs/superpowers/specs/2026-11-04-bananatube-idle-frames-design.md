# BananaTube: stop paying for frames nobody sees (Track A1)

## Intent

The app redraws the whole window every 8 ms and, for every visible string, renders a fresh TTF surface and creates and
destroys a texture. With nothing happening — the common state of a media browser — that is pure CPU, allocation churn
and battery on a handheld. Cut the idle cost to ~0 without changing the look.

## Goals

- Draw and present only when something on screen changed.
- No heap allocation on an unchanged frame (the text cache's hit path).
- A bounded text cache (RAM capped, textures freed on exit).
- No drawing at all while a video plays: mpv's window covers the app, the UI is not visible.
- Same behaviour and layout as today; Track A2 (cards, keyboard, focus) comes later.

## Non-goals

Layout changes, animations, theming, new dependencies, the player overlay (Track B).

## Approach

1. A `dirty` flag with one `markDirty()` used everywhere the frame can change: any SDL event, touch press/move, a
   gamepad auto-repeat tick, `startSearch`/results arriving, tab/view/sel changes, status text, play/stop/back,
   thumbnail arrival, scale/resize. `draw()` and `SDL_RenderPresent` run only when it is set.
2. While `Mode::PLAYING`, skip drawing entirely; keep the IPC polling (position/duration for SponsorBlock) and the
   touch handling that drives mpv.
3. Text cache: key = font size + colour + string, value = texture + w/h, cap 256 entries, evicted all-at-once when the
   cap is exceeded (simple, bounded, and misses only happen on content changes). Destroyed on exit.
4. The thumbnail worker bumps a counter when it downloads a file; the loop compares it and marks dirty when it moved
   (the visible rows may now have a real picture).

## Files

`app/bananatube/main.cpp` only; the selftest gains a cache check, and `system/tests/run-banana-tube-test.sh` asserts it.

## Verification

- Selftest: two draws of the same string return the same texture (one miss, one hit), the counters move as expected,
  and teardown destroys them.
- Unit (device): idle CPU in the results screen before/after (`top -b -n1`), and a screen that still updates on a
  press, a tab switch, a search landing and a thumbnail arriving.
- No allocation while idle: cache misses stay 0 with no input (log the counters on exit or via the selftest).

## Risks and calls

- A missed `markDirty()` leaves a screen stale until the next input. The trigger list above is the review checklist.
- Cache cap 256 and evict-all: my call; it keeps ~1-3 MB worst case and the code tiny.
- Skipping draws in PLAYING: my call; nothing app-side is visible there today.
