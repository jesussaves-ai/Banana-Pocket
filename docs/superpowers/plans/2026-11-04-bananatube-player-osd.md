# BananaTube player OSD — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the player shows where it is, whether it is buffering, and what the pad does; Y changes quality from the player.

**Architecture:** all feedback is mpv's text OSD over IPC; the app adds a pure bar helper, two polled mpv properties and a
restart-with-resume path (`playingItem` + `resume`).

**Tech Stack:** C++17, existing SDL2/mpv IPC, no new dependency.

**Spec:** `docs/superpowers/specs/2026-11-04-bananatube-player-osd-design.md`

**Status:** implemented; selftests green (24 checks). Pending: device run (seek/pause/SELECT bars, Y reload, a stall).

## Review Focus

- Y reload: it must resume at the current position (not the row's original resume), and must not add a duplicate Recent row.
- Buffering indicators must be throttled (state or 10 % bucket), or mpv's OSD would be rewritten every 500 ms.
- The bar must clamp (negative, past the end, duration 0).

### Task 1: helpers + selftests (done)
- [x] `barText(at,total,cells)` and `nextQuality(q)`; selftest `bar` covers start/middle/end/dur 0 and 720→480→360→720.
- [x] `run-banana-tube` asserts it (24 checks).

### Task 2: indicators (done)
- [x] `whereTag(pos)` (`03:21 / 12:04\n----|-------`) on seek, pause and SELECT; start card gets title + cap + pad map.

### Task 3: Y quality (done)
- [x] `playingItem` remembered at play; `reloadQuality()` stops, cycles, saves the cap and replays with `resume = pos`.

### Task 4: buffering (done)
- [x] Request ids 9/10 polled every 500 ms while playing; `Buffering N%` on the stall, `Playing …` on recovery, 10 % buckets.

### Task 5: device + close
- [ ] On the unit: play, seek (bar), pause, SELECT, Y (same position, 480p), a stall shows Buffering; then move B to Done.
