# BananaTube (the YouTube client)

BananaTube is the app on the menu's **Apps shelf** that searches YouTube and plays a video. It is deliberately small:
one SDL2 window under `cage`, `yt-dlp` for the search and for resolving a stream, and `mpv` for the picture and the
sound. There is no browser, no Electron and no embedded web view.

## On the menu

The app is not a line in `/etc/banana/systems.conf`. `banana-ui` scans `/usr/share/banana/apps`, `/data/apps` and
`/data/banana/apps` for a folder with a manifest and shows each as its own tile (`Library::scanApps`, docs/ARCHITECTURE.md
§3). BananaTube ships one:

```
/usr/share/banana/apps/bananatube/app.json        the manifest (name, run, ext, perf, accent)
/usr/share/banana/apps/bananatube/BananaTube.app  the marker the menu lists as the app's one entry
/usr/libexec/banana/run-bananatube                the launcher (system/rootfs)
/usr/bin/banana-tube                              the client (app/bananatube)
```

The shelf draws the tile from the app folder itself: `app.json`'s `accent`, and the icon it names from the theme's
icon set (`video.png`), so the tile has its own look. Drop another folder with an `app.json` (or an `app.conf`, or a freedesktop `.desktop`) into any of the three
app folders and it appears on the shelf the same way, with no code change.

The manifest keys are `id` (default: the folder name), `name`, `maker`, `run` (`%ROM%` is the marker file above),
`ext` (the marker's extension), `perf` (the clock profile the launcher applies), and optionally `icon` (a picture in
the app folder) and `accent` (`#rrggbb`). An id `systems.conf` already lists wins over a scanned one.

## Look

The client wears BananaOS's **Pocket** theme: a warm brown-black ground (`#17140d`), butter text, olive metadata and
banana gold for focus, the active tab and the brand. Pixelify Sans is the face (Inter is the fallback), black-and-gold
crisp borders replace rounded corners, and the app sits in the menu's raised bezel with its dot-grid backdrop. The top
strip carries the OS banana mark and the **BananaTube** wordmark; the search well, tabs, cards, keyboard and button-chip
hints live inside the frame. Thumbnails, duration and resume chips sit on the card art, and the player's mpv OSD uses
the same palette and face.

## Controls

The client reads the RPC's pad through SDL2 (the launcher exports `SDL_GAMECONTROLLERCONFIG`, the same Nintendo-order
mapping `run-port` uses: **A is the east button**). A desktop keyboard works too: the arrows and Enter act as the pad,
and Tab / Shift+Tab switch tabs (the QEMU test drives the app that way; the RPC has no Tab key).

| Button | Search keyboard | Results | Playing |
|---|---|---|---|
| d-pad | move on the keys | move through the results | seek (up/down 10 s, left/right 60 s) |
| A | type the key / `Shift` / `Search` | play the chosen video | play / pause |
| B | clear, then quit | back to the keyboard | stop and go back |
| X | backspace | the chosen video's channel | undo the last SponsorBlock skip |
| Y | run the search | back to the keyboard (search) | quality: 720 → 480 → 360p, reloaded at the same position |
| L / R | switch tab | switch tab | previous / next chapter |
| L2 | — | a page back | tap: back 10 s; hold: scrub back 10 s every 0.25 s |
| R2 | — | a page forward | tap: forward 10 s; hold: play at 2x |
| START | run the search | a new search | full screen |
| SELECT | the quality cap (720/480/360p) | the quality cap (720/480/360p) | position / length bar / quality readout |
| touch | tap a key / `Search`, or a tab | tap a row to focus, tap again to play, or a tab | zones: left/right seek, centre pause, top-left stop |

**Tabs, recent and quality:** the results shelf has four tabs — **Search** (the keyboard and its hits), **Recent** (the
last 30 videos that actually started playing), **Channels** (the uploaders either list mentions, with a video count)
and **Local** (the music and video files on the card).
Switch with L/R, or a tap; SELECT changes the quality cap. A on a channel shows its videos from both lists, B walks
back out.

**Results cards:** each row is a full-height 16:9 thumbnail with the video's length on its bottom-right corner; a
Recent row also shows where playback stopped on the thumbnail's top-left. Only the visible page's thumbnails (and one
row either side) are kept in memory, so scrolling a long list does not grow the app.

**Player feedback:** the app draws nothing while a video plays (mpv owns the screen), so everything is mpv's text OSD:
the start card names the video, the cap and the whole pad map; every seek, pause and SELECT shows `03:21 / 12:04` over a
12-cell bar (`----|-------`); and a stall shows `Buffering 47%` (mpv's `paused-for-cache`/`cache-buffering-state`, polled
twice a second) until playback recovers. **Y** re-resolves at the next cap and replays from the same position (a ~2 s
reload is the cost of a seamless switch this stack cannot do), saving the cap like the browse screen's SELECT.

**Local** lists the files in `/data/music` and `/data/videos` (one level, common audio/video extensions, by file name)
and plays the chosen one straight from the card: mpv takes the path, so there is no yt-dlp call, no SponsorBlock lookup
and no thumbnail; a local file is not added to Recent (which is a list of video ids). New files show up when the tab is
opened again. Recent rows remember where playback stopped (written every 10 s and on stop, shown as "resume m:ss"), and
re-opening one seeks there once mpv has the stream up; a finished video clears its position. Recent survives a reboot in
`/data/banana/bananatube-recent.tsv`; the quality cap (720p default, 480p/360p for a weak link) is saved in
`/data/banana/bananatube.conf` and applied by `yt-dlp -g` to the next video.

**SponsorBlock** skips sponsor, self-promotion, interaction reminder, intro and outro segments from SponsorBlock's
public API (`sponsor.ajay.app`, looked up by video id before playback). The client asks mpv for the playback position
four times a second over its IPC socket and seeks past a segment when it reaches one. (yt-dlp has no part in this: it
only cuts segments out of downloads.) A skipped segment is not skipped again in the same playback. Its toast reads
"Skipped sponsor (X to undo)".

**Touch:** the panel's touchscreen is the FT5x06 (multitouch, `/dev/input/event*`). SDL2's **Wayland** backend does not
bind `wl_touch`, so under `cage` the app receives no `SDL_FINGER` and no emulated pointer — `banana-tube` therefore
reads the touchscreen from **evdev** itself, with the same mapping `banana-ui` uses: scale the raw axes to the panel's
native mode (1080x1240), then turn them by the panel's rotation into the app surface's upright 1240x1080 space. The
turn defaults to 90 (the RPC's panel) and can be overridden on the card with `/data/banana/bananatube/touch.conf`
(`rot=`, `flipx=`, `flipy=`; read once at start) or `BANANA_TUBE_TOUCH_ROT`, so a differently-mounted panel is fixed without a rebuild. Draw and
hit-testing share one set of geometry functions (`keyRect`/`actRect`/`resultsTop`), so a tap lands on exactly what is
drawn. `BANANA_TUBE_TOUCH_DEBUG=1` logs each tap's mapped coordinates.

During playback there is **no mpv OSC**: its buttons are tiny and its touch hit-testing drifted on this stack, so
`banana-tube` runs mpv with `--no-osc` and drives playback itself from its own (verified) evdev touch. mpv's window
still covers the screen (so the app cannot draw an overlay), and the whole screen becomes the control surface, in four
large zones:

- **top-left** (top 18% × left 18%): stop and go back to the results;
- **left 28%**: seek −10 s;
- **right 28%**: seek +10 s;
- **the rest (centre)**: play / pause.

`--osd-playing-msg` shows the controls briefly when a video starts. The gamepad still works too (A play/pause, d-pad
seek, B stop). `banana-tube` never acts on SDL's touch: under `cage` SDL reports a touch as a *pointer* at offset
coordinates, and acting on both fired a second, wrong tap per gesture (the phantom characters).


Playback: `banana-tube` starts `mpv` (full screen under `cage`) and keeps the pad to itself, telling mpv what to do
over its JSON IPC socket (`/run/banana/bananatube.sock`). `banana-tube` resolves the stream first, with `yt-dlp -g`
(capped at 720p, what the Adreno `a613` holds comfortably), and hands mpv the direct URL(s): the picture, and the
sound for a DASH pair. The image's mpv has no Lua, so nothing runs inside it. The lookup blocks the menu for a few
seconds before the video opens. When mpv exits, the results come back.

**Video output:** `--vo=wlshm` (software, through shared memory). mpv's GPU output (`--vo=gpu`) under `cage`
renders a white frame intermittently on the `a613`, and a stalled output stalls the playback clock with it; the
software output draws like `banana-ui`'s own panel and is stable. Decoding still uses the GPU where it can:
`--hwdec=auto-copy` (a copy-back hwdec such as `v4l2m2m-copy`) decodes on the chip and copies the frames back to
system memory for `wlshm`, falling back to software on its own.

**Search:** the results come from YouTube's **InnerTube** search endpoint (`POST
youtubei/v1/search`, the public web client key) — one HTTPS request, parsed in-process, instead of spawning
`yt-dlp` and waiting for it. If the response shape changes or the request fails, the app falls back to the old
`yt-dlp --flat-playlist` path, so search keeps working. Thumbnails are fetched in the background (libcurl → SDL2_image)
and cached under **`/data/banana/cache/tube/<id>.jpg`**, so revisiting a result is instant.

**Audio output:** S16 to the sound server's `default` (PipeWire; mpv is a client like the menu) — see the launcher
and the note below — with a small `--audio-buffer` matched to the tiny AudioReach period, and a graceful stop (mpv's
own `quit` over IPC, then TERM).



**Codec:** YouTube offers **AV1** first, and the `a613` has no AV1 decoder — mpv then renders no frames and the
screen stays black. `banana-tube` therefore tells yt-dlp to prefer **H.264 (`avc1`)**, then **VP9**, and never
`av01` (its `yt-dlp -f` prefers `[vcodec^=avc1]`, then `[vcodec^=vp9]`, and never `av01`, capped at the quality the
player is set to). Both decode far faster than realtime
here (measured ~120 fps for a 720p 120-frame decode); AV1 does not decode at all.

## Using it on the touchscreen

Step by step, with a finger:

1. **Open it** — on the Apps shelf, tap the **YouTube** tile (or press A on it).
2. **Search** — tap the on-screen letters to type, then tap **Search** (bottom row). Each tap types exactly one
   character. `Del` backspaces, `Shift` switches case.
3. **Pick a video** — tap a result row once to focus it (it gets the highlight ring), then tap it again to play. Its
   thumbnail and duration are on the left/right of the row.
4. **While it plays** (mpv owns the screen; a big indicator flashes on each tap and hides by itself):
   - **centre of the screen** → play / pause (`||  Paused` / `>  Playing`)
   - **left third** → seek back 10 s (`<<  -10s`)
   - **right third** → seek forward 10 s (`>>  +10s`)
   - **top-left corner** → stop and return to the results
5. **Leave** — tap the **top-left corner** to stop the video (back to the results), then tap **Back** (top-left of
   the results) to the keyboard, then **Back** again to the menu; or just press **B** on the pad, which backs out one
   step each time. **Stop** the whole app with **B** from the keyboard, or `Select + Start`.

The pad does the same jobs everywhere (d-pad to move/seek, **A** select/play/pause, **B** back, **L/R** page, **START**
search), so the touchscreen and the buttons can be mixed freely.

## Audio

`run-bananatube` does no audio handoff: mpv opens the ALSA `default` device from `/run/banana/asound.conf`, which is
the **sound server** (PipeWire). So BananaTube is a PipeWire client like the menu and RetroArch -- it mixes with them
instead of taking the DSP's single frontend, and no route is written before the open. The same contract `run-retroarch`
has.

## Packages

`buildroot/package/banana-tube` (local, from `app/bananatube`) selects `sdl2`, `sdl2_ttf` and `mpv`, and installs the
binary, the manifest and the marker. `buildroot/package/yt-dlp` installs yt-dlp's official self-contained release
zipapp. Both are enabled in `buildroot/configs/banana_retroid_pocket_classic_defconfig`; yt-dlp's version and hash are
pinned in `buildroot/package/yt-dlp/yt-dlp.mk` and `yt-dlp.hash`.

## Tests

`system/tests/run-banana-tube-test.sh` builds the client and runs its logic pass under SDL's dummy driver (the harness has
no display): the search-line parse, the keyboard navigation and the `back()`/`activate()` state machine. It runs from
`make test`. Playback needs mpv, yt-dlp and a network and is not exercised there.
