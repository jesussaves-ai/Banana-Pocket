# BananaTube: the player tells you where you are (Track B1 + Y quality)

## Intent

While a video plays the app draws nothing (mpv owns the screen), so the only feedback is mpv's text OSD. Today you get
`<< -10s` on a seek and a position readout on SELECT; you cannot see where you are, whether the stream stalled, or what
the pad can do. And quality can only be changed from the browse screen.

## Goals

- Buffering is visible: `paused-for-cache` + `cache-buffering-state` polled while playing, "Buffering 47 %" while
  stalled, "Playing" when it recovers (throttled to state changes).
- Every seek, pause and SELECT indicator shows the position **and** a textual bar: `03:21 / 12:04` over
  `──────●─────`.
- The start card names the video, its cap and the whole pad map (`A pause · B stop · D-pad seek · L/R chapter ·
  L2/R2 hold · Y quality · SEL where · START full`).
- **Y cycles quality** in the player (720 → 480 → 360): stop, re-resolve at the new cap, replay from the same
  position. A visible ~2 s reload, announced (`Reloading at 480p…`), saved like the browse screen's cap.
- SELECT keeps the position/length/quality readout, now with the bar.
- Non-goals: captions (needs yt-dlp's subtitle path), an in-app graphical bar (no compositor), animations, any new
  dependency.

## How

- `play()` gains `restartAt` (seconds): when set, the existing `pendingSeek` mechanism applies it once the IPC socket
  is up, so a quality switch resumes where it was.
- IPC: two request ids for buffering (`paused-for-cache`, `cache-buffering-state`), polled every 500 ms while playing;
  `showInd` only when the state or the rounded percentage changes.
- `barText(pos, dur, cells)` is a pure helper (clamped, `●` at the right cell, `─` elsewhere) used by seeks, pause and
  SELECT; selftested (start, middle, end, dur 0).
- Y in the player: read `pos`, save it to `restartAt`, stop, `nextQuality()`, save the cap, `play(current)`; the start
  card announces the new cap. `nextQuality(q)` is selftested.

## Files

`app/bananatube/main.cpp` only, plus a selftest case and its assertion in `run-banana-tube-test.sh`.

## Verification

- Selftest: bar at 0/mid/end and dur 0; `nextQuality`.
- On the unit: play, seek (bar + times), pause (times), SELECT (readout), Y (reload at the same position, OSD cap
  changes), and a throttled connection (or the log) shows the buffering state.
- `make -s test`, `make agent-check`.

## Risks and calls

- Y restarts mpv: the reload is accepted and announced (the alternative, switching in place, is not possible with
  yt-dlp's resolved URLs).
- mpv OSD is text only; the "bar" is characters, sized for the 90 px OSD font.
- Buffering percentage is mpv's estimate; when the property is unavailable the indicator says just "Buffering…".
