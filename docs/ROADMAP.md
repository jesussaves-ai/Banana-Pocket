# BananaOS Roadmap (RPC / SM4450)
Refs: `docs/RPC-PORT.md` (port log), `docs/RPC-TEST.md` (hardware checklist). Ordered by what is recommended next.

## Recommended next (in order)
The 0.5.0 sound server and its leftovers, the library-UX additions (multi-disc `.m3u`, shoulder-button jump-to-letter)
and OTA updates over Wi-Fi are done (see *Done*). Next:
1. **bananad's volume path should use the ALSA C API.** `ARCHITECTURE.md` and `AGENTS.md` say the volume keys drive
   the mixer through `snd_mixer_*`; the code forks `/usr/libexec/banana/volume` (busybox `amixer`) and `alsactl`, one
   fork/exec per key and a whole-card store per change (the C API is used for the sleep mute and the routing only).
   The docs now say what the code does; this item is the rewrite back to the law.
2. **Wi-Fi management depth** -- 802.1X, static IP, hotspot. **Saved-network priority and WEP are done**: L/R on a
   network in Settings > Network moves a saved one up or down `wifi.conf`, and an old WEP network takes its 5/13-
   character (10/26-hex) key on the same keyboard and is written as `key_mgmt=NONE` + `wep_key0`. What is left:
   802.1X needs an identity/certificate screen, static IP needs address fields, hotspot needs hostapd (not in the
   image -- I would drop it).
3. **Bluetooth** -- the kernel is stripped of it and the QCA BT firmware is unused; BT audio/controllers would need a
   bespoke path (no BlueZ/DBus in the stack).
4. **Store / app catalog** (the Apps shelf only scans local folders).

## In progress

- **An Android-like sleep: find and cut the 37–42 mA standby drain.** Measured on the unit with the gauge's
  `charge_counter` (the awake line has recorded it since `a81a11f`): `battery 99% -> 96% (164 mAh in 3.9 h, 42 mA)` and
  `96% -> 94% (105 mAh in 2.8 h, 37 mA)` in `deep` — about **140 mW**, where an Android-class standby is 5–20 mW. The
  spurious wakes (3–5 a night, the SD host's IRQ, no key or cable) cost well under 1 mAh together, so this is a steady
  state, not wake churn; the 99→75 in the field notes was the 2026-11-02 hang (100→74 in 15 h).
  - **Done — test 1, the sound server (measured win).** The overnight sleep with `sleep.d/20-sound` (stop
    PipeWire/WirePlumber in `pre`, restart in the background in `post`) held **25–29 mA**, against the 37–42 mA
    baseline: 3.3 h at 29 mA, 8.0 h at **26 mA** (206 mAh), 2.0 h at 27 mA. The AudioReach graph pinned by
    `session.suspend-timeout-seconds = 0` was keeping the ADSP awake. `make sound-test` after the wake: all 14 checks
    pass (DSP graph RUNNING, amp at 81 %), so the chain comes back on its own.
  - **Running:** test 2 — the USB gadget is unbound for the sleep (`sleep.d/30-usb`: `usb-gadget down` in `pre`, back
    in the background in `post`), to see whether a bound UDC keeps the controller and its PHY clocks alive. Read
    against test 1's 25–29 mA.
  - **Queued, one variable per sleep:** (3) a `s2idle` sleep for comparison, to know what `deep` is worth. If none
    moves it, the floor is the platform's S2R depth (firmware) and the remaining wins are on the wake side.
    (Unbinding `ath10k_snoc`/WPSS was tried and rejected: 69 mA over 0.4 h, no win.)
  - **Wake side:** 57–60 ms from the kernel's resume to a drawn menu; the kernel's own resume is ~1.05 s; Wi-Fi coming
    back is the slow part (the post hook reconnects in the background). Measure it, then consider keeping the link
    associated across suspend (`sleep.d/10-wifi` currently always downs it) — "works instantly" is mostly this.
  - The meter (`battery_charge_uah`, the awake line) is in the image (0.5.8+); the unit runs **0.5.9, slot b**.

- **BananaTube: what is left of the YouTube work.** A1 (idle frames), A2 (cards, keyboard, thumbnail bound), B1 (player
  OSD + Y quality) and the Pocket shell all shipped in 0.5.9. Left: **captions** (Track B3 -- yt-dlp's subtitle path
  into mpv's `--sub-file`, no new dependency) and the three unscheduled Ideas below -- a **Subscriptions** tab from the
  channels' saved `browseId`s (no account), an **offline shelf** (a yt-dlp download queue with progress and a local mpv
  path) and a **play-next queue** in the player OSD. Designs/plans: `docs/superpowers/specs/2026-11-04-bananatube-*`
  and `docs/superpowers/plans/2026-11-04-bananatube-*`.
- **Languages: Spanish and German, Settings screen first.** The pipeline is in: the English string is the key,
  `tr()` in `ui/src/i18n.cpp` falls back to English, catalogs are JSON in `/usr/share/banana/lang/<code>.json` with a
  `/data/banana/lang/` override on the card (like a theme), and Settings > Display & sound gains a **Language** row
  (English / Español / Deutsch) that switches at once. The whole main Settings list is translated — group names, row
  names, help strip, values, buttons, the toasts whose text is catalogued — 121 strings per language, informal
  (tú/du). `tools/check-lang.py` runs in `make test` (stale or misspelled keys fail, coverage is printed) and the
  headless menu test loads the German catalog. Next: the Settings sub-pages, the menu chrome, system/app names,
  date/time formats, then another language or two.

## Ideas (unscheduled)
Not commitments: sketches to weigh against the list above. Each one builds on something the image already has.
- **Session recap card.** After a game, a short card on the menu: time played, achievements won this session, and the
  new state slot. The data is already written (`playstats.tsv`, `/run/banana/cheevos`), so this is a menu view, not a new
  recorder.
- **Haptic cues.** `bananad` already drives the vibrator. Use short pulses for achievements, a low-battery warning and
  a sleep refusal, each switchable in Settings, so the slate can tell you something without a sound.
- **Quick-resume shelf.** The last three games with their saved state, one button away from the home screen, so a
  pick-up-where-you-left-off session needs no library browsing.
- **Per-system ambience.** The menu's background loop (`bgm`) varies by system, from a WAV on the card, so each shelf
  sounds different. Falls back to the current single loop.
- **Burn-in-aware home screen.** Rotate the home shelf's static chrome by a pixel or two every few minutes (the guard
  already knows idle time), so a long session on the AMOLED never leaves a ghost of the menu bar.
- **Battery session planner.** From the battery curve (`bananad`'s battery reads) and the play history, show "about 3 h
  left at this pace" beside the percentage. Pairs with item 6's time-remaining estimate.
- **Saves you can read.** A "last session" log view per game (start, end, states written, cheevos), built on the
  in-menu log viewer in item 6. Makes a lost save easy to trace.
- **BananaTube subscriptions without an account.** Search results carry each channel's `browseId`; keep a local list of
  those ids and fetch each channel's uploads from InnerTube's browse endpoint, so a Subscriptions tab works with no
  Google sign-in. The Channels tab already gathers the names; this adds the id and the feed.
- **BananaTube offline shelf.** A download queue over yt-dlp (picture + sound to `/data/banana/downloads`) with a fifth
  tab showing progress and a local mpv playback path, so a flight or a bad Wi-Fi spot does not end the session.
- **Play next queue.** Queue a few results with a long A press or SELECT, show "up next" in the player OSD, and start
  the next one when the current video ends; a single toggle keeps autoplay from draining the battery unasked.
- **Field diagnostics bundle.** One Settings button that packs the logs, the sound-server log, `dmesg` tail and
  `settings.conf` (with secrets stripped) into `/data/banana/backups` for a bug report. Complements `make sound-test`
  and the `tools/rpc-*.sh` probes.
- **Offline update cache.** Keep the last good update package on the card until the new slot boots once, so a failed
  update can be reapplied without Wi-Fi.

## Field notes (open questions, from working on the unit)

- **The settings.conf reset on 2026-11-03 was a device test, not an update.** During the back-up/restore work the
  busybox `tar` on the unit failed to create an archive (`-z` is not built in); the test had already moved
  `settings.conf` aside and the script's own cleanup removed that copy, so the file was restored from the image's
  template (`/etc/banana/settings.conf`). That template has `ssh = off`, `adb = off` and every other default, which is
  what reset the unit's SSH/ADB (and theme) settings. The OTA does not touch `/data`. Current values were set back to
  `ssh = on`, `adb = on` at 23:21; the 23:04 archive still holds the defaulted copy. **Rule for future device tests:
  never modify `/data/banana/settings.conf` while testing; keep any safety copy on the card, outside the tree under
  test, and verify the archive exists before deleting anything.**
- **Unexplained, 2026-11-03 23:21:59 on 0.5.5:** `banana-ui-session` exited and init respawned the menu, and
  `settings.conf` was rewritten at 23:21. The user did not press Restore, and the file is not the 23:04 archive's copy
  (`ssh`/`adb` differ), so it was not a restore of that archive. A dropbear restart at 23:21:55 (an SSH/ADB toggle from
  the menu) is the only nearby event. Watch for a repeat: `banana-ui.log.1` tail + `/run/banana/messages`.
- **Update packages pile up:** `/data/banana/updates` held 31 packages, 6.9 GB on 2026-11-03 -- nothing prunes them.
  Proposed: after a successful boot into the new slot, keep the two newest and delete the rest (the one just applied is
  no longer needed once its slot booted).

## Maintenance
- **Agent DX:** `make agent-check` (quiet syntax + host compile + the fast suite, 4 lines on success) and `make agent-map` (regenerates the source map) were added 0.5.x; keep the `AGENTS.md` Source Map fresh when core files move.

*(Dropped: a quick-settings/performance overlay -- performance is fine and RetroArch's own OSD (`fps_show` /
`statistics_show`, already rotated by its patches) covers FPS/CPU if wanted. And on 2026-11-04, by request: a screenshot
gallery and collections/tags (auto genre shelves from the scraped `gamelist.xml` were proposed; not wanted). The
library-UX item is closed: the scraper already skips games that have art, and now starts only on the charger or cable.
Also dropped by request: porting BananaTube's newer settings screen (its in-app settings are kept as they are), and the
accessibility / in-menu log viewer / battery-time item (2026-11-04).)*

## Completed Baseline (0.4.9+)
- **Core & CI:** End-to-end QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA updates, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`.
- **Input & Library:** `rsinput` Nintendo layout (`A=east, B=south`). Single-screen RetroArch library only (no DS/3DS dual-screen launchers).
- **Display & Graphics:** 90°-rotated 1240x1080 panel in `banana-ui`; games run upright via `cage` (`video_rotation = 0`, Wayland context; requires `SEATD_VTBOUND=0` so `bananad` `SIGSTOP` on suspend never deadlocks VT switch). Mesa **26.2.6** (freedreno GL + **Turnip Vulkan**) on the Adreno `a613` -- supported natively from Mesa 26.2.0, so the image carries no local a613 source patch (SwanStation 60 fps @ 4x).
- **Development access:** the USB-C port is an NCM Ethernet gadget with a DHCP server (`169.254.170.2`) plus SSH, and **adb** over an adb (FunctionFS) function on the same cable, with a TCP `5555` listener on its own **ADB debugging** switch (off by default, independent of SSH). The loopback interface is up, which the adbd control socket needs.
- **Hardware Validation (`docs/RPC-TEST.md`):** In progress on physical units.

## Done
- [x] **The hardening sweep (2026-11-05).** A full read of the tree for bugs, dead weight and doc drift, then one
  pass over all of it. **Daemon:** `update`, `update-check` and `backup` write `/run/banana/awake.pid`, so a 200 MB
  slot write, a release download or a whole-card tar can no longer be idle-suspended; `rcK` runs `usb-msc off` so a
  card handed to a computer comes back before shutdown unmounts it; the wake reason distinguishes a real cable plug
  from an unplug, which no longer burns one of the three phantom-wake retries; `pid_of` clamps a pidfile whose
  wall-clock age exceeds the uptime, so a clock step (ntpd at Wi-Fi) cannot make a running game look gone. **The
  sleep.d hooks are now exercised**: `sleep-test.sh` provides stand-in `wifi`/`sound-server`/`usb-gadget` helpers and
  asserts the pre hooks ran in name order and the post hooks brought all three back (46 checks, was 41) -- until now
  every hook took its `[ -x ... ] || exit 0` guard and no-oped, which is what let a broken one ship. **BananaTube:**
  every write to the shared `results` now takes the thumbnail worker's lock, `qualitySave` writes a temp file with
  fsync and renames it (a power cut cannot truncate the saved cap), the per-tap log lines are behind
  `BANANA_TUBE_TOUCH_DEBUG`, a panel that reports only `ABS_MT_TRACKING_ID` still registers taps, `resolveStream`
  takes the first video and audio URL instead of rejecting three, and `ipcSend` writes the rest of a command on a
  non-blocking socket. **UI:** X offers a game's options for every system (it worked, only NDS advertised it),
  `readGamelist` no longer matches the root `<gameList>`, the library rescans a newly copied ROM without waiting for
  a keypress, and `wifi::Job`'s `active` is atomic. **i18n:** the update and box-art row values are catalog keys
  (es/de 129/129) and `tools/check-lang.py` now fails on a literal passed to `tr()` that is not a key -- the rule
  that was missing when they shipped English-only. **Dead weight:** `etc/fstab`, `accounts.conf.example`,
  `tools/make-console.py`, `tools/qemu-e2e.sh`, `docs/img/readme/`, `docs/img/boot-animation.gif` and the stray
  `bgm.wav` are gone; `autosleep_minutes` ships 0 (the slate default), `sleep_debug` is commented out as card-only.
  **CI:** a core's own patch triggers the cores job, the `sh -n` glob no longer trips on a non-shell file, and
  `make test` builds bananad. ~20 docs corrected to the code (the mono amp, SwanStation at 4x, the lease/blend/test
  list/volume law, the Makefile inputs, BANANATUBE's touch.conf and harness). Deferred, with the reasoning in
  `docs/superpowers/specs/2026-11-05-hardening-sweep-design.md`: the second canvas and the bottom-screen path are the
  headless dual-mode renderer's tested path, not dead code.
- [x] **The on-device self-test: dropped, the pieces already exist (2026-11-05).** A `tools/rpc-selftest.sh` that
  walks the `docs/RPC-TEST.md` checklist was going to be the next thing built; looking at the checklist against what
  is already in the tree, it would be mostly a driver around existing parts, so it is closed instead. The audio route
  end to end is `make sound-test` (14 checks: server, ALSA default, sink, AW88166 gain, the AudioReach graph, a real
  client, the menu handover); `tools/rpc-thermals.sh`, `rpc-usb-trace.sh`, `rpc-hang-watch.sh`, `rpc-gpu-probe*` and
  `rpc-screen.sh` cover heat/fan, the USB gadget across a plug, a hang and a frame; and the remaining boxes (version
  and slot, panel size and rotation from banana-ui's log, Wi-Fi state, UDC state, `/data` free space, the update
  packages on the card) read straight off the unit with one ssh. `docs/RPC-TEST.md` stays the human checklist -- the
  parts a script cannot judge (does a game run, does a save survive a reboot) are the parts that need eyes anyway.
- [x] **BananaTube: the Pocket shell, the player OSD and a near-free idle (0.5.9).** Three tracks in one release.
  **Pocket shell:** the client wears the OS's own theme -- `#17140d` ground, butter ink and olive metadata, banana-gold
  focus, Pixelify Sans (Medium titles/wordmark, Regular body, Inter fallback), a crisp framed screen with the dot grid,
  a brand strip with the banana mark and the *BananaTube* wordmark, four Pocket cards per page (duration chip on the
  thumbnail, gold resume chip), a keyboard that fills the frame (Shift shows `abc`/`ABC`, Search gets a gold ring while
  the query is non-empty), and button-chip hints; the mpv OSD takes the same palette and face and the app tile is gold.
  **Player OSD (B1 + Y):** while a video plays the app draws nothing (mpv owns the screen), so every cue is mpv text --
  `03:21 / 12:04` over a 12-cell bar on seek/pause/SELECT, a throttled `Buffering N%` from
  `paused-for-cache`/`cache-buffering-state` (polled twice a second, 10 % buckets), a start card with the title, the cap
  and the whole pad map, and **Y** cycles the quality cap (stop → re-resolve → replay from the same position, ~2 s,
  announced). The pad had been dead in the player: SDL2 drops joystick events while the app is unfocused and mpv's
  window has the focus, so `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is what made A/B/d-pad/L/R/L2/R2/Y/SELECT/START
  work there; OSD font 90 → 40 px, four tabs that no longer run under the quality badge, L/R switch tabs, L2/R2 page
  the list, SELECT cycles the quality cap everywhere. **Idle (A1/A2):** a `dirty` flag gates drawing (nothing at all
  while mpv owns the screen), a bounded 256-entry text-texture cache ends the per-frame TTF allocation, the thumbnail
  worker wakes the loop when a picture lands, and the thumbnail texture map is pruned to the visible page ±1 -- idle
  CPU **65 % → 0.2 % of a core** (654 → 2 ticks/10 s; 3–4 ticks/20 s on the unit). Selftests: cache, local, thumbs,
  bar/quality cycle, layout at 1080 and 768, textfit (`run-banana-tube`, 27 checks). Verified on the unit at 0.5.9
  (`surface 1240x1080`, pad opened) and by host renders at a true 1240x1080; the headless QEMU image cannot start
  cage/SDL (no EGL on its virtio GPU), so the panel pass is the unit. Known minor: emoji in YouTube titles are tofu.
  Designs/plans: `docs/superpowers/specs/2026-11-04-bananatube-*`, `docs/superpowers/plans/2026-11-04-bananatube-*`.
- [x] **Resume the last game at boot (0.5.8).** A clean shutdown while a game runs boots back into it (Quick resume
  loads the state); a shutdown in the menu boots the menu. The menu writes `/run/banana/game.path` at launch, `bananad`
  keeps it in `banana/resume` at shutdown, and the next menu start feeds the existing `/run/banana/launch` hook. The
  cancel window is 0.5 s and ignores the power key — 0.5.6's window was cancelled by the very press that powered the
  console on, so every real boot showed the menu (found from the unit's log, fixed in 0.5.8). Three launches cap a game
  that wedges the device; a missing card/ROM clears the marker and logs. A pending resume skips the boot animation:
  measured menu-ready **7.80 → 6.41 s** and launch **~9.8 → ~6.9 s** after power-on on the unit. A quit back to the menu
  clears the marker and the tries count (unless `shutting-down` says the exit is the shutdown). Tests: the sleep test
  (41 checks; marker on a battery-guard shutdown, none from the menu) and five headless `resume` cases; verified on the
  unit with a real power cycle (`ready` → `launching`, tries 1, no cancel). Only the deliberate non-power cancel press
  has not been done on hardware; its decision path is the headless cancel case.
- [x] **Theme validation (0.5.x).** `tools/check-themes.py` validates every bundled theme against
  `docs/theme.schema.json` in `make test` (and CI): a small validator for the keywords the schema actually uses
  (`$ref`, `type`, `enum`, `pattern`, `properties`, `items`, `anyOf`, `oneOf`, `additionalProperties`, `required`),
  with comments and trailing commas stripped the way the menu's parser allows them. A schema keyword it does not
  implement is an error, so the check cannot silently skip a constraint, and `--selftest` proves it catches a wrong
  type and an out-of-enum value. The other half of the item, live theme reload, already worked: `Settings::THEME`
  rebuilds the menu in place (layout and colour themes both).
- [x] **WEP networks (0.5.x).** The scan's `[WEP]` flag now means "secured, key_mgmt=NONE + wep_key0" instead of
  "unsupported": A joins it, the password keyboard takes the key (5 or 13 printable characters, or 10/26 hex digits --
  quotes and backslashes refused so the quoted conf stays valid), and `banana/wifi.conf` gets `key_mgmt=NONE`,
  `wep_key0="…"`, `wep_tx_keyidx=0` with the usual priority. Enterprise (802.1X) and OWE stay hand-written. Tested
  through the headless menu (`wifi-add`, `wifi-scan` script verbs): the WEP block, a refused 3-character key, and the
  scan flags for WEP vs WPA2.
- [x] **Saved-network priority (0.5.x).** Settings > Network's L/R moves the selected saved network one place up
  (higher priority, tried first) or down by swapping the two `priority=` values in `banana/wifi.conf`; every other
  byte of the file is kept, and a network that is not saved is left alone. Tested through the headless menu (the
  `wifi <ssid> <up|down>` script verb) against a two-block fixture, with the "not saved changes nothing" case.
- [x] **Local media in BananaTube (0.5.x).** A fourth tab, **Local**, lists `/data/music` and `/data/videos` (one
  level, common audio/video extensions, by file name) and plays straight from the card: mpv takes the path, so there is
  no yt-dlp resolve, no SponsorBlock lookup and no thumbnail fetch, and a local file is not written to Recent (a list of
  video ids). A file added while the app runs appears the next time the tab is opened. The host selftest walks a temp
  folder (extensions, file names, one level) and `run-banana-tube` checks it (22 checks). The other half of that
  roadmap item, "porting BananaTube's newer settings screen", was dropped by request -- its in-app settings stay as
  they are.
- [x] **Controller remaps per core and per game -- RetroArch's own, not a Banana screen (0.5.4+).** No remap UI was
  built: RetroArch's *Settings > Input > RetroPad Binds > Save Remap File / Save Game Remap File* is the UI, and it
  already writes to the card (`input_remapping_directory = /data/retroarch/remaps`, `HOME`/`XDG_CONFIG_HOME` there) as
  `remaps/<Core>/<Core>.rmp` and `remaps/<Core>/<Game>.rmp`; per-game core options are its *Save Game Override*
  (`config/<Core>/<Game>.opt`). Two real gaps closed: `run-retroarch` rewrote the Dreamcast core remap whenever its L
  line was not Banana's `"12"`, so a remap saved from RetroArch's menu was undone on the next launch -- it now writes
  only when the core has **no** remap at all (37-check launcher test covers both a player's remap surviving and a fresh
  card getting Banana's swap); and `Settings > System > Back up settings` now carries `retroarch/remaps` (the backup
  test's 22 checks include a remap restored), so a controller layout survives a restore. `docs/EMULATORS.md` gains
  *Controller remaps and per-game options* with the exact menu path and file names.
- [x] **Back up & restore `/data/banana` (0.5.4+).** Settings > System's **Back up settings** (A twice) writes
  `banana/backups/banana-<date>.tar.gz` with `settings.conf`, `bananad.conf`, `wifi.conf`, `accounts.conf` (when
  present), `saves/` and `states/` -- logs, updates, box art and the backups themselves stay out. **Restore settings**
  (A twice) unpacks the newest over `/data` and restarts the menu so the restored file is read; Wi-Fi and sleep
  settings take effect at the next boot / bananad's SIGHUP. The tool is `/usr/libexec/banana/backup`
  (`create`/`list`/`restore`, one `ok …`/`error: …` line the menu toasts), written with `tar` then `gzip` because the
  image's busybox tar has no `-z`. A restore refuses absolute and `../` members before unpacking (a backup on the card
  is user-supplied), and a `.part` name until the archive is whole. Verified on the unit: create, delete
  `settings.conf` and a state, restore, both identical by sha256; host test `run-backup-test.sh` (19 checks: archive
  contents, newest-first list, restore, a hostile `../` archive) and a headless menu test that the two rows run the
  tool off the loop.
- [x] **Audio follow-ups (0.5.4).** The automatic recovery in the menu's open path is gone: `banana-ui` forked
  `sound-server recover` after ~10 s of failed opens, and it never fired in normal use (0 asks across two boots, 439
  log lines); the wedged-session-manager case it was written for has not been seen since the config re-read. A
  failing open still logs `open failed: …` and retries every 250 ms (the 0.5.4 backoff), and `sound-server recover`
  stays as the manual cure the sound test names. `sound-config-test` now asserts the menu never forks the server from
  its open path.
- [x] **Confirm the audio on hardware (0.5.4).** All three by-ear checks passed on the unit, each followed by
  `make sound-test` (14/14): a cold boot (the splash cue and the test's four beeps), a game quit (the menu reopened its
  PCM and logged `panel back after the game (0 tries)` — the 0.5.4 DRM-master fix), and a sleep/wake (`menu: released
  the sound device in 0 ms`, `awake (power button)` after 11 s, beeps). The headphone-jack check is dropped: the test
  unit has no headphones.
- [x] **The audit's findings, fixed and pruned (0.5.4).** Two independent passes over `system/`, `ui/src`, `app/`,
  `buildroot/` and `tools/` (bloat, then correctness), every finding re-verified in the source. Fixed: BananaTube died
  on SIGPIPE whenever mpv exited with a key still in flight (one `signal(SIGPIPE, SIG_IGN)`); the ambient loop read
  past a truncated `bgm.wav` and divided by zero on an empty one (guard `total >= nframes`, `% total`); `Display::
  restore()` reported success without `drmSetMaster`/`SetCrtc`, so the 400-try panel-handover retry after a game could
  never fire (it returns false now); a wedged session manager (EBUSY) spun the sound thread at 100 % CPU and wrote a
  log line per try (every failed open backs off 250 ms); the BananaTube recent list was rewritten in place and a full
  card truncated the history (temp + fsync + rename); a pad rescan dropped the evdev fds without the release, so a held
  direction repeated forever (clear `rep.key`); `run-retroarch` left `cage.pid` behind and `pid_of()` trusted any live
  pid, so a reused pid got SIGSTOP/SIGTERM/SIGUSR1 (removed at exit; `pid_of()` rejects a pid whose process began
  after the file was written); the state backup said "Copied" before the data reached the card (`fflush`/`fsync`).
  Pruned: `pad-profile` (44 lines, no caller), rcS's non-PipeWire routing block (45 lines, could not run), the three
  launchers' no-cage arms (~70 lines, dead on this image), BananaTube's byte-identical copy of the JSON reader (239
  lines, now `JSON_SRC`), `sset()`, the inline `retroarch.cmd` write, the duplicate `acct()` (now `ra-secrets get`),
  the SDL pad map (one `/etc/banana/sdl-pad.map`), six `/sys/class/backlight` walkers (one `backlights()` + `bl_file()`,
  policy unchanged) and settings' ad-hoc `jsonField()` (the linked `JVal` reader). Net −289 lines. `run-retroarch-test`
  now stubs cage and checks the Wayland context; all gates green (`make test`, the 37-check sleep test, `make
  sound-test` on the unit).
- [x] **A sound test, because the chain fails silently (0.5.3).** Five links (server, ALSA's `default`, the Speaker
  sink, the AW88166 gain, the AudioReach graph) each fail into the same symptom -- silence -- and the only way to tell
  them apart was a reboot and guesswork. Now `make sound-test` runs `/usr/libexec/banana/sound-test` on the handheld
  over ssh (streamed, so it also checks an older image; `ARGS=--listen` plays the cue): server + socket, both asound
  configs, the Speaker sink *and* that it is the default (a Headphones default is the after-update stall), both amp
  controls, the frontend `RUNNING` and owned by pipewire, a real client (`aplay -D default`, else mpv) with a stream
  arriving at the Speaker, and the menu's own `/run/banana/sound-state` handover (`SIGHUP` -> closed, `SIGUSR1` ->
  open, 2 s each). `make test` runs the half that needs no hardware: `system/tests/sound-config-test.sh`, 33 checks
  of the laws that have been broken before (config re-read per open, the handover latch, no client on `hw:0`/`amixer`,
  FE<->BE switches only in `SectionVerb`, `session.suspend-timeout-seconds = 0`, per-node buffering in
  `monitor.alsa.rules`, sound-server's Speaker wait + `recover`, the runtime dir, and the image's pieces). It found two
  real defects while being written: the menu's note stayed on `releasing (handover)` for good when the device was
  already closed (bananad then waited out its 2 s bound on every sleep), and `/etc/asound.conf` existed only in a
  hand-modified build tree, not in the repo -- a clean build would have had no default device. Both fixed. See
  `docs/SOUND-TEST.md`.
- [x] **The 03:40 overnight hang: instrumented, and the handover it was seen mid-way through is closed (0.5.2).** An
  overnight hang on the 0.5.1 image (asleep at 03:40, no wake line, pstore empty, 100 % -> 74 % by morning) left
  nothing to read, and the sleep log's `wake mmc0 (irq ?)` was fiction: `mmc0` is only the table's `last_change` --
  measured on the unit, a real power-key wake leaves `pm_wakeup_irq` absent and **every** `wakeup_sources` counter at 0,
  so the kernel cannot name a waker on this board (and `wake_only_power` cannot disarm `mmc0` at all: it is a virtual
  source with no `power/wakeup`). Now: `sleep.log` reports what `bananad` itself saw (`wake: power key|cable (kernel
  wakeup_count +n)`), `logs/sleep-state` is written just before `mem` with the mode, the armed-source count and the
  menu's sound state, and the kernel's PM lines are appended on the way out -- the only survivor of a resume that never
  comes back. That note immediately showed the suspect: `sound device: releasing (handover)` at the write of `mem`, a
  PCM close in flight (the same boot had reopened the device 16 s before sleeping). `bananad` now sends its `SIGHUP` and
  **waits for the ack** in `/run/banana/sound-state` (bounded by the menu's own 2 s) instead of a fixed 200 ms, and
  `Sound::release()` latches `handedOver` so the loop cannot reopen the PCM for the warm-up hold (it did, 10 ms after
  closing, which is how the `mem` still happened with the device open). Verified on the unit: `menu: released the sound
  device in 20 ms`, `sound device: closed (release)` at the `mem`, 1.08 s round trip, and the UI reopens on the wake.
  Whether this was the whole hang is open -- the next one will say so in two files.
- [x] **BananaTube: the three-tab shelf, recent list and quality cap (0.5.x, uncommitted).** The results screen now has a
  tab strip under a taller search field: **Search** (the keyboard and its hits), **Recent** (the last 30 videos that
  actually started, newest first) and **Channels** (the unique uploaders either list mentions, with a video count; A
  opens one channel's videos, B walks back out, X opens the focused video's channel). Tabs switch with L/R on the
  keyboard, SELECT (Recent), or a tap; the search field is drawn with the larger face and lights its accent border while
  typing. Recent persists as `/data/banana/bananatube-recent.tsv` (with the resume point, written every 10 s and on
  stop; a finished video clears it) and the playback cap (720/480/360p, applied by `yt-dlp -g`) as
  `/data/banana/bananatube.conf`. SELECT on a list cycles the cap, and in the player it prints
  position/length/quality. mpv's dead `stats` script (no Lua in this mpv) is gone; the start card and every pause/seek
  OSD now come from `show-text` over IPC, and the position/duration poll runs even when a video has no SponsorBlock
  segments (the IPC socket is also connected on the first playback frame, not the first button press, so SponsorBlock
  and the resume seek work from the start). Verified with the host suite (21 checks); the panel look is not yet checked
  on the unit.
- [x] **Menu audio after updates, sleep and games (0.5.x, uncommitted).** Four separate causes, all fixed on the unit: (1) `sound-server` waited only for "no sinks at all" before restarting WirePlumber, so an update's reboot that came up Headphones-only never got its Speaker sink; it now restarts WirePlumber up to four times while the Speaker is missing. (2) `banana-ui` read ALSA's config once, before `bananad` had written the PipeWire default, so it kept the hardware card and fought the server for the Speaker after every game; each device open now re-reads the config (`snd_config_update_free_global`). (3) The menu held the device open with nothing to play, and a boot-time warm-up hold could expire immediately after a game; the device now closes after 20 s of quiet with the loop off, and the warm-up deadline counts from the warm-up itself. (4) Game exit to menu reopen is about 100 ms with no busy retries. Verified on the unit: two clean reboots with the Speaker present, two game quit cycles, and a sleep/wake cycle with a game launch after it.
- [x] **USB mass storage + gadget reliability (0.5.x).** Settings > System > *Mass storage* adds a USB disk (the card's data partition) over the same cable as NCM and adb: `usb-gadget` creates `functions/mass_storage.0` and `usb-msc` owns the `/data` side -- `ro` exposes a read-only volume with `/data` still mounted, `rw` is transfer mode (`banana-ui-session` waits on `/run/banana/usb-msc.mode`, `/data` is unmounted and handed over, `bananad` refuses suspend), `off` remounts and restarts the menu. It has no udev, so the LUN path is `${DISK}p4` from `/run/banana/disk` (there is no `/dev/disk/by-*`). Both modes were verified on the unit (host mounted the card, wrote into `roms/`, eject remounted `/data` and restarted the menu); the kernel only accepts `lun.0/ro` **before** `lun.0/file` (otherwise `EBUSY`, which silently left the disk read-write), so both the boot and `msc` paths set the attributes first. A reverse-orientation hotplug used to leave the gadget unenumerated: the controller clears the gadget's UDC binding and nothing re-binds it (there is no UDC "add" event), even though the port reports `orientation=reverse`, the PHY and the mux are all fine. `bananad` now re-binds the complete tree within ~3 s (the typec roles never change, so writing `data_role`/`usb_role_switch` does nothing -- the re-bind is the fix); verified on the unit, the host enumerates in either orientation. The intermittent disconnect was a role switch dropping the UDC: a complete gadget is now re-bound in place instead of rebuilt (which used to kill adbd and re-enumerate everything), `usb-adb` backs off and re-binds after an adbd restart, and `bananad` debounces UDC events. `tools/rpc-usb-trace.sh` records a plug for diagnosis; `docs/RPC-TEST.md` and `ARCHITECTURE` §8 carry the checklist and the design.
- [x] **The last audio leftovers retired (0.5.x).** `audio_route_ar()` and its FE/BE csets, the `/run/banana/audio-route` file and `sound.cpp`'s `routeCheck`/`routeId` reopen are deleted; `rcS`'s manual routing now runs only when `/usr/bin/pipewire` is absent, and `sound-server` no longer forces the HiFi verb or writes the speaker switch (ACP + the `0002` UCM-reopen patch own them). The same audit pass set `sleep_check_hours = 0` (no RTC wakealarm on the RPC), removed the dead `leds-qcom-lpg` patches + `CONFIG_LEDS_QCOM_LPG` + the stale `AnbernicRG55G1` topology, made `update` reject `../`/absolute tar members, made `volume` reject non-numeric levels, and bounds-checked the `/run/banana` inotify walk. Only the hardware jack confirmation is left (see *Recommended next*).
- [x] **OTA updates over Wi-Fi (0.5.x).** Settings > System > Update gains a **"Check for updates"** row: A runs `/usr/libexec/banana/update-check` (a sh wrapper around python3 `urllib` -- busybox wget has no TLS here, CA bundle in `/etc/ssl`), which reads the GitHub releases list (`releases/latest` skips the alpha prereleases), takes the newest `banana-<board>-*.update.tar` by numeric version and streams it into `banana/updates`, writing progress to `/run/banana/update-status`; the page polls that and re-lists when it lands, and the existing apply/verify/Restart path takes over. Verified on the unit (`up to date: 0.5.0`; the asset URL resolves at 238 MB). A first cut stuck the page (`compare(0,9,"up to date")` never matched the 10-char literal, so `checking` never cleared and the page ignored every key); fixed to 10 with a 90 s bound.
- [x] **Library UX: multi-disc `.m3u` and shoulder-button jump-to-letter (0.5.x).** The scan reads a `.m3u` (lines relative to its folder; `#`/absolute/`..` skipped) and hides the disc files it names, so a PS1 game is the playlist entry alone -- verified on the unit, an `.m3u` naming 2 discs + `Alpha/Beta/Delta` shows 4 games, not 6. Shoulder **L/R** jump the games by the title's first letter (`Library::letterStep`, and a new `Menu::jumpTo` across the Rnds/Layout/Carts engines): R the next letter, L this letter's first game or the previous letter's; the D-pad still steps one at a time, and on the home shelf L/R still walk the systems. (The recent-played shelf was already there, commit `10fcecf`.)
- [x] **The sound server (0.5.0): PipeWire + WirePlumber + the RPC's UCM2 profile.** ACP brings the AudioReach (`q6apm`) card up from the UCM profile and every client -- the menu's cues, RetroArch, BananaTube -- mixes through `pcm.!default { type pipewire }` instead of the two fronts being handed over one at a time. Three DSP-specific things made it work: the card's UCM verb is re-applied in the node open path immediately before `snd_pcm_open` (`buildroot/patches/pipewire/0002` -- ACP runs its sequences once, before its probe's open/close tear-down, so the held-open sink latched the topology's "off" and boot was silent); the sink's buffering is set per node with a `monitor.alsa.rules` match (`monitor.alsa.properties` never reaches the ACP-created nodes), which is what cured the short-cue crackle; and the menu's old pre-PipeWire handover wait was deleted (`ui/src/sound.cpp`), so the first cue after a game is instant (measured 0.07 s open-to-ready, was ~1.9 s). The speaker is one mono AW88166 (`0-0035`, on FL -- `FL [on]`/`FR [off]` is normal). `AGENTS.md` carries the DSP laws: the FE<->BE switch only takes a write while its front-end's graph is closed, and shared switches belong only in `SectionVerb.EnableSequence`.
- [x] **ADB debugging gets its own switch (0.5.x).** The TCP `5555` listener no longer rides the SSH setting: Settings > System > **ADB debugging** (persisted as `adb` in `settings.conf`, off by default) spawns `/usr/libexec/banana/adb up|down`, which restarts the adb supervisor in `usb-adb`; that reads `adb` and starts adbd with `BANANA_ADB_TCP` (on) or without it (off). `ssh` no longer touches adb, and USB adb over the cable is always served, so turning the network listener off cannot disturb SSH or the adb/NCM link.
- [x] **The pre-PipeWire audio mess cleaned out (0.5.0).** `audio-handoff` and `audio-settle` are deleted, bananad's `audio_handoff_ms`/`audio_menu_ms` knobs and `audio_publish_handoff()` are gone, and the launchers no longer call the handoff or write the FE/BE route with `amixer` before an open (games are PipeWire clients; the handoff's "wait for the frontend to read closed" never ended under the held-open sink and cost ~1.5 s a launch -- measured: launch file -> game 0.17 s, exit -> menu reopen 0.09 s). `HiFi.conf`'s `SectionDevice` `EnableSequence` copies of the shared FE/BE switches are gone too (only the verb's is left, per the UCM rule). `README`, `ARCHITECTURE` §5, `EMULATORS`, `RPC-PORT`, `BANANATUBE`, `CODEBASE-ANALYSIS` and `BOOT-SOUND` now describe the server. The last leftover -- `/run/banana/audio-route`, bananad's dead csets and `sound.cpp`'s `routeCheck` -- was retired in 0.5.x (see the first *Done* entry above).
- [x] **CI green again (0.5.0).** Five separate reds fixed: `run-bananatube`'s stale `hw:0,1`/format assertions (it plays through the server now), bananad's `-Werror=unused-result` on the `wakeup_sources` header read, the `cores` job calling a `.banana-patched` target that does not exist (the Makefile stamp is absolute), the aarch64 bananad step linking a cross `libasound` the runner has not got (now a compile check), the sleep test releasing the suspend FIFO with no power-key press (broken since the 0.4.54 spurious-wake guard, which `wake()` now satisfies and `rtc_wake()` covers the battery checks), and the defconfig check running on a fresh Buildroot clone without `buildroot/buildroot-patches` (so it dropped the freedreno Vulkan option). Tag pushes no longer trigger CI (`on.push.branches: [main]`), so a release no longer spawns a duplicate run.
- [x] RetroAchievements progress display on the home shelf.
- [x] Save-state browser (browse, copy and delete a game's states from the menu).
- [x] Settings cleanup: the dead rows removed (VSync, Integer scaling, Audio latency, Waking up, LEDs, Accent colour, Motion), the section renamed to **RetroArch**, single-screen only, and the dead ScreenScraper entry gone.
- [x] A lighter keyboard and a redesigned game list (all five Pocket themes).
- [x] **Wii/GameCube removed; Mesa bumped to 26.2.6.** The standalone Dolphin frontend is gone -- `[wii]` in `systems.conf`, `run-dolphin`, `buildroot/package/dolphin` (and its ROCKNIX Wayland patches), the `/usr/share/banana/dolphin` configs, the pad profiles, the icon and its test. Measured on the unit: Turnip's a613 Vulkan path topped out at 40-45 fps where the stock Android Adreno driver held the 60 fps cap; the gap is driver efficiency, and closing it needs Qualcomm's closed userspace driver through a libhybris-style bionic shim over a ported KGSL kernel driver -- a large project for a console the handheld already runs on Android. In the same change Mesa goes 26.0.1 -> 26.2.6 (a Release whose `freedreno_devices.py` carries the a613 natively with the correct `reg_size_vec4 = 32`; the local `patches/mesa3d` a613 source patch is dropped). The bump also steadies every Turnip-based RetroArch core. OpenGL was tried as an alternative backend and is clearly worse (more CPU, hotter); a `TU_DEBUG` knob sweep was dropped with the console.
- [x] **adb on the handheld (0.4.57).** Debian's glibc `android-tools` `adbd` serves the adb protocol over an adb (FunctionFS) function in the same composite gadget as the NCM network -- `adb devices` over the cable -- and, when the SSH setting is on, over TCP 5555 on every interface, so `adb connect <ip>:5555` works over Wi-Fi (a local patch makes the listener opt-in and binds it everywhere; the stock code only listens on TCP when USB is absent, and only on loopback). `usb-adb` mounts FunctionFS and supervises adbd; the `ssh` toggle restarts it so the port follows the switch live. Verified over Wi-Fi: `adb shell` as root, `push`, `pull`.
- [x] **Loopback was never brought up.** `rcS` now does `ip link set lo up`. Nothing did, and any program binding `127.0.0.1` failed with `EADDRNOTAVAIL` -- which is what actually killed `adbd` (its control socket on `tcp:5037`) before it reached `usb_init()`, so USB adb failed too.
- [x] **adb hardened, and USB verified (0.4.59).** adbd now reports Banana's own identity (`product:banana model:Pocket_Classic device:rpc`, not a Nexus 4 -- the values were compiled defaults) and the gadget's adb function binds correctly at boot: an ffs function whose ep0 is not yet open makes the controller refuse the whole gadget (ENODEV, taking the network down with it), so `usb-gadget` now starts adbd and waits for the function's `ready` before binding, `usb-adb` re-binds the controller if a restart leaves it with no gadget, `flock` keeps `rcS` and bananad's UDC-event re-run from racing, and an image without adbd just skips the function. Verified from a clean boot: the host enumerates the composite gadget and `adb devices` lists it (`adb shell` as root over the cable). Windows needs its own adb driver (documented): configfs `f_fs` exposes no OS-descriptor entry to bind WinUSB to that interface.
- [x] **`audio_latency` test/doc mismatch fixed.** The image deliberately lowered it 64 -> 48 ms (audit `f28412b`), but `run-retroarch-test.sh` still expected 64 and `docs/RPC-PORT.md` still said 64. Both now say 48; `make test` is green again.
- [x] **Audio handover tuned; the launch wait removed (0.4.60).** The "one beat" a launcher waited was covering the wrong thing: it went in when a game shared the menu's MultiMedia1 frontend, and `4075c75` fixed that properly by giving games their own frontend (`$HOME/.asoundrc` -> MultiMedia2, the one the menu never releases). Measured on the unit with the kernel's q6apm close timing (patch 0063): no close is slow and none fails, so a launch needs no settle -- games had sound over many launches and after a reboot with `audio_handoff_ms = 0`, which also takes 0.8 s off every launch. The menu's own reopen *does* race the outgoing graph (0 cuts its cues out for a moment, ~400 leaves a trace, 600 is clean), so it keeps a settle of its own. The knob is now two: `audio_handoff_ms` (launchers, default 0) and `audio_menu_ms` (banana-ui's Sound, default 600), published as `/run/banana/audio-handoff-ms` and `/run/banana/audio-menu-ms`. A *fast relaunch* is the one case that still races (the outgoing game's own graph may still be tearing down when the new one is instantiated, which resets the route switches to off and leaves the new frontend silent for its life), so the launchers note when they exit and `audio-settle` waits out the remainder of a short cooldown -- an ordinary launch stays at 0, an immediate relaunch is covered.
- [x] **Thermals & fan: nothing to tune.** Profiled on the unit under the then-heaviest load (Dolphin/NSMBW, since the console was dropped): `cpuss0` avg 46 / peak **55 °C**, `gpu` peak 52 °C, clocks steady (`policy0` 1.96 / `policy6` 2.40 GHz), no CPU or GPU throttle -- and the **`pwm_fan` never engaged** (its first trip is 60 °C). Nothing in the image reaches the SoC's limits, so the curve is a safety net that does not need characterising. (`tools/rpc-thermals.sh`'s bogus `gpu=<Hz>` column is fixed, and `tools/rpc-emu-bench.sh` is the general profiler if a heavier load ever appears.)
- [x] **Sleep/Wake: closed (0.4.54).** The power-key sleep that came straight back was traced to the PMIC pwrkey being the only armed wake source and its edge-both IRQ firing on a glitch, or on the release of the very press that asked for the sleep: bananad now names the waker (reads debugfs `wakeup_sources`), a wake with no real `KEY_POWER` press and no cable change logs `wake ... (spurious): back to sleep` and re-suspends (bounded x3), `&pon_pwrkey` gained `debounce = <15625>`, and the idle burn-in guard stops at the dim stage while charging (a dark panel on the cable had read as a sleep). Working well enough on the unit; the leftovers (residual spurious wakes, panel/SD resume latency, keeping the Wi-Fi module across suspend, overnight drain) are not being tracked. An RTC wake alarm is not available (the alarm IRQ is ADSP-owned, no `qcom,no-alarm`), so a low battery refuses the sleep instead (`bananad`'s `sleep_floor_percent`).
