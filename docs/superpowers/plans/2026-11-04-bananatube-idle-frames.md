# BananaTube idle frames — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the app draws only when the screen changed, and re-renders a string only when it first appears.

**Architecture:** one `dirty` flag gates `draw()` + `SDL_RenderPresent`; a per-font/per-colour text-texture cache owns its textures; the thumbnail worker bumps a counter the loop watches.

**Tech Stack:** C++17, SDL2 + SDL_ttf (already linked), no new dependency.

**Spec:** `docs/superpowers/specs/2026-11-04-bananatube-idle-frames-design.md`

## Global Constraints

- No new dependency, no layout change, no new file.
- Cache: cap 256 entries, evict-all, hit path performs no allocation; textures destroyed on resize and exit.
- No drawing at all while `Mode::PLAYING`.
- `make -s test` and `make agent-check` stay green.

## Review Focus

- A change that forgets `markDirty()` leaves a stale screen until the next input (trigger list below).
- Text cache keyed by a font pointer: fonts are recreated on resize, so the cache must be cleared there (stale pointer reuse).
- Thumbnails arriving with no input must appear (counter → dirty).
- Entering/leaving PLAYING must redraw (results after playback).
- Cache eviction must not free a texture still referenced by the current frame.

---

### Task 1: text texture cache (with selftest)

**Files:** `app/bananatube/main.cpp`; `system/tests/run-banana-tube-test.sh`

**Interfaces:**
- `SDL_Texture* cachedText(const std::string& s, TTF_Font* f, SDL_Color c, int* w, int* h)`
- `void clearTextCache()` — destroys every texture, clears; counters `textHits`, `textMisses` for the selftest.

- [ ] Step 1: selftest first — `selftest cache`: `clearTextCache()`, draw "Cache me" twice, expect 1 miss + 1 hit; a different colour is a second miss. Print `bananatube selftest cache: ok|fail`. Add the grep to `run-banana-tube`.
- [ ] Step 2: run → fail (no cache yet: two misses).
- [ ] Step 3: implement `cachedText` (nested maps: font → colour → string, `.find()` with the caller's string so a hit allocates nothing) and use it from `drawText`; `clearTextCache()` on font recreation (resize) and before the renderer is destroyed.
- [ ] Step 4: run → pass.
- [ ] Step 5: commit.

### Task 2: draw only when dirty; no drawing while playing

**Files:** `app/bananatube/main.cpp`

- [ ] Step 1: add `bool dirty = true;` and `void markDirty() { dirty = true; }`.
- [ ] Step 2: in the loop replace `draw(); SDL_RenderPresent(ren);` with `if (dirty && mode != Mode::PLAYING) { draw(); SDL_RenderPresent(ren); dirty = false; }`.
- [ ] Step 3: mark dirty at: any SDL event consumed (top of the event pump), a touch tap, `pollSearch()` when results landed, `play()`/`stopPlayback()`, the mpv-exit block, `startSearch()`, and the gamepad auto-repeat `move()`.
- [ ] Step 4: build host; commit.

### Task 3: thumbnails wake the loop

**Files:** `app/bananatube/main.cpp`

- [ ] Step 1: `std::atomic<unsigned> thumbGeneration{0};` incremented by `thumbWorkerLoop` when it writes a file.
- [ ] Step 2: in the loop, compare it with a static `seen`; on change, `markDirty()`.
- [ ] Step 3: commit.

### Task 4: verify and close

- [ ] Step 1: `make -s test`, `make agent-check`; build the aarch64 app.
- [ ] Step 2: on the unit (after the running sleep test): idle CPU in the results screen before/after (`top -b -n1`, and `grep` the misses counter via the selftest), a press, a tab switch, a search landing, a thumbnail arriving, and leaving playback all update.
- [ ] Step 3: move the milestone in `ROADMAP.md` to Done with the measured CPU numbers; commit.

## Track A2: cards + the thumbnail bound (implemented)

Approved in session 2026-11-04; no separate design doc (the A1 design's "Track A2 comes later").

- [x] Cards: the thumbnail is the card's full height in 16:9, the duration moved from the row's right edge to the
  picture's bottom-right, and a Recent row's resume point sits on the picture's top-left instead of in the channel line.
- [x] `thumbTex` is bounded: `pruneThumbs()` keeps only the visible page +/- one row and runs when the window moves
  (no work and no allocation in the per-frame path).
- [x] Selftest `thumbs` (six ids, window 1..3 keeps b/c/d); `run-banana-tube` is at 25 checks.
- [ ] Device: the badges read at arm's length, and scrolling a long list keeps the memory flat.
