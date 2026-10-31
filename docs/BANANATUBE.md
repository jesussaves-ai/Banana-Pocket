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

The theme's `systems.cfg` has a line for `bananatube` (the `video.png` icon, the `[app]` mark), so the tile has its
own look. Drop another folder with an `app.json` (or an `app.conf`, or a freedesktop `.desktop`) into any of the three
app folders and it appears on the shelf the same way, with no code change.

The manifest keys are `id` (default: the folder name), `name`, `maker`, `run` (`%ROM%` is the marker file above),
`ext` (the marker's extension), `perf` (the clock profile the launcher applies), and optionally `icon` (a picture in
the app folder) and `accent` (`#rrggbb`). An id `systems.conf` already lists wins over a scanned one.

## Controls

The client reads the RPC's pad through SDL2 (the launcher exports `SDL_GAMECONTROLLERCONFIG`, the same Nintendo-order
mapping `run-port` uses: **A is the east button**).

| Button | Search keyboard | Results | Playing |
|---|---|---|---|
| d-pad | move on the keys | move through the results | seek (left/right to the minute) |
| A | type the key / `Shift` / `Search` | play the chosen video | play / pause |
| B | clear, then quit | back to the keyboard | stop and go back |
| X | backspace | a page back | — |
| Y | `Shift` | a page forward | — |
| L / R | — | a page back / forward | — |
| START | run the search | a new search | — |
| SELECT | — | — | play / pause |
| touch | tap a key / `Search` | tap a row to focus, tap again to play | mpv's own controls (below) |

**Touch:** the panel's touchscreen is the FT5x06 (multitouch, `/dev/input/event*`). SDL2's **Wayland** backend does not
bind `wl_touch`, so under `cage` the app receives no `SDL_FINGER` and no emulated pointer — `banana-tube` therefore
reads the touchscreen from **evdev** itself, with the same mapping `banana-ui` uses: scale the raw axes to the panel's
native mode (1080x1240), then turn them by the panel's rotation into the app surface's upright 1240x1080 space. The
turn defaults to 90 (the RPC's panel) and can be overridden on the card with `/data/banana/bananatube/touch-rot` (read
once at start) or `BANANA_TUBE_TOUCH_ROT`, so a differently-mounted panel is fixed without a rebuild. Draw and
hit-testing share one set of geometry functions (`keyRect`/`actRect`/`resultsTop`), so a tap lands on exactly what is
drawn. `BANANA_TUBE_TOUCH_DEBUG=1` logs each tap's mapped coordinates.

During playback mpv owns the screen (its own fullscreen window under `cage`), so its on-screen controls are the
player buttons a finger reaches: they are enlarged with `--osd-scale=1.8 --osd-scale-by-window=no`, and mpv maps
`wl_touch` to a click on them. `banana-tube` ignores evdev taps while playing, so the two do not both act.


Playback: `banana-tube` starts `mpv` (full screen under `cage`) and keeps the pad to itself, telling mpv what to do
over its JSON IPC socket (`/run/banana/bananatube.sock`). mpv resolves the URL through `yt-dlp` itself
(`--ytdl`), capped at 720p — what the Adreno `a613` holds comfortably. When mpv exits, the results come back.

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

**Audio output:** S16 to the second AudioReach frontend (MultiMedia2, `hw:0,1`) — see the launcher and the note
below — with a small `--audio-buffer` matched to the tiny AudioReach period, and a graceful stop (mpv's own `quit`
over IPC, then TERM) so the frontend is released cleanly and the next open does not hit `EBUSY`.



**Codec:** YouTube offers **AV1** first, and the `a613` has no AV1 decoder — mpv then renders no frames and the
screen stays black. `banana-tube` therefore tells yt-dlp to prefer **H.264 (`avc1`)**, then **VP9**, and never
`av01` (`--ytdl-format=…[vcodec^=avc1]…/[vcodec^=vp9]…/[vcodec!*=av01]…`). Both decode far faster than realtime
here (measured ~120 fps for a 720p 120-frame decode); AV1 does not decode at all.

## Audio

`run-bananatube` calls `/usr/libexec/banana/audio-handoff` once, before `banana-tube` (and so before mpv): the menu
has just released its AudioReach frontend and the DSP needs a beat to tear the graph down. mpv then opens the ALSA
`default` device from `/run/banana/asound.conf` (there is no PulseAudio/PipeWire). The same contract `run-retroarch`
and `run-dolphin` have.

## Packages

`buildroot/package/banana-tube` (local, from `app/bananatube`) selects `sdl2`, `sdl2_ttf` and `mpv`, and installs the
binary, the manifest and the marker. `buildroot/package/yt-dlp` installs yt-dlp's official self-contained release
zipapp. Both are enabled in `buildroot/configs/banana_retroid_pocket_classic_defconfig`; yt-dlp's version and hash are
pinned in `buildroot/package/yt-dlp/yt-dlp.mk` and `yt-dlp.hash`.

## Tests

`app/bananatube/tests/selftest.sh` builds the client and runs its logic pass under SDL's dummy driver (the harness has
no display): the search-line parse, the keyboard navigation and the `back()`/`activate()` state machine. It runs from
`make test`. Playback needs mpv, yt-dlp and a network and is not exercised there.
