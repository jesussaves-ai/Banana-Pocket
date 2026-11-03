# BananaOS Roadmap (RPC / SM4450)
Refs: `docs/RPC-PORT.md` (port log), `docs/RPC-TEST.md` (hardware checklist). Ordered by what is recommended next.

## Recommended next (in order)
The 0.5.0 sound server and its leftovers, the library-UX additions (multi-disc `.m3u`, shoulder-button jump-to-letter)
and OTA updates over Wi-Fi are done (see *Done*). Next:
1. **Controller remapping UI** (the pad map the launchers export), and **per-game core options** beyond core + shader.
2. **Library UX, the rest** -- collections/tags and a screenshot gallery; plus **"scrape missing art"** and a scraper
   that only runs while charging or on the cable, so a long scrape cannot flatten a battery (the recent-played shelf,
   multi-disc `.m3u` grouping and shoulder jump-to-letter are done, 0.5.x).
3. **Local media / music player**, and porting BananaTube's newer settings screen.
4. **Accessibility** (colour-blind palettes, larger type), an **in-menu log viewer**, and a **battery time-remaining** estimate with a graceful low-battery shutdown.
5. **Wi-Fi management depth** -- WEP/802.1X (currently `unsupported`), saved-network priority, static IP, hotspot.
6. **On-device self-test** (`tools/rpc-selftest.sh`): one script that walks the `docs/RPC-TEST.md` checklist on the
   unit -- panel/rotation, touch axes, every button, the audio route (speaker + jack), a Wi-Fi join, adb over the
   cable, a suspend/resume -- and prints a pass/fail table with a log tail. Turns the hardware checklist into a
   repeatable gate instead of a manual read.
7. **Sleep leftovers** -- keep Wi-Fi across suspend while a download is in flight (`sleep.d/10-wifi` always downs it),
   and measure overnight drain; residual spurious wakes and panel/SD resume latency are still untracked.
8. **Theme validation & live reload** (small): `make test` validates every bundled theme against `docs/theme.schema.json`, and Settings re-applies the active theme without a restart.
9. **i18n** -- no locale layer (BananaLinux ships Simplified Chinese).
10. **Bluetooth** -- the kernel is stripped of it and the QCA BT firmware is unused; BT audio/controllers would need a bespoke path (no BlueZ/DBus in the stack).
11. **Store / app catalog** (the Apps shelf only scans local folders).

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
  `settings.conf` (with secrets stripped) into `/data/banana/backups` for a bug report. Complements the on-device
  self-test in item 8.
- **Offline update cache.** Keep the last good update package on the card until the new slot boots once, so a failed
  update can be reapplied without Wi-Fi.

## Maintenance
- **Agent DX:** `make agent-check` (quiet syntax + host compile + the fast suite, 4 lines on success) and `make agent-map` (regenerates the source map) were added 0.5.x; keep the `AGENTS.md` Source Map fresh when core files move.

*(Dropped: a quick-settings/performance overlay. Performance is fine and RetroArch's own OSD -- `fps_show` /
`statistics_show`, already rotated by its patches -- covers FPS/CPU if wanted, so there is nothing to build.)*

## Completed Baseline (0.4.9+)
- **Core & CI:** End-to-end QEMU `virt` boot/update/test (`tools/qemu-boot.sh`), A/B OTA updates, first-time setup, `ksmbd`, `dropbear`, `ath10k_snoc`.
- **Input & Library:** `rsinput` Nintendo layout (`A=east, B=south`). Single-screen RetroArch library only (no DS/3DS dual-screen launchers).
- **Display & Graphics:** 90°-rotated 1240x1080 panel in `banana-ui`; games run upright via `cage` (`video_rotation = 0`, Wayland context; requires `SEATD_VTBOUND=0` so `bananad` `SIGSTOP` on suspend never deadlocks VT switch). Mesa **26.2.6** (freedreno GL + **Turnip Vulkan**) on the Adreno `a613` -- supported natively from Mesa 26.2.0, so the image carries no local a613 source patch (SwanStation 60 fps @ 5x).
- **Development access:** the USB-C port is an NCM Ethernet gadget with a DHCP server (`169.254.170.2`) plus SSH, and **adb** over an adb (FunctionFS) function on the same cable, with a TCP `5555` listener on its own **ADB debugging** switch (off by default, independent of SSH). The loopback interface is up, which the adbd control socket needs.
- **Hardware Validation (`docs/RPC-TEST.md`):** In progress on physical units.

## Done
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
