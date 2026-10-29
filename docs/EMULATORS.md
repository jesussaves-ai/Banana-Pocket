# What Banana gives an emulator, and what it expects back

This is the contract between the menu and any program that plays a game — on the Retroid Pocket Classic, that is
RetroArch and its cores. Keep to it and the rest of Banana (sleep, the volume keys, the logs, the settings) works
around the emulator without knowing it.

## Starting

`systems.conf` names a `run` command per system, with `%ROM%` the game's path (quoted) and `%SYSTEM%` the system's
id. The commands are the launchers in `/usr/libexec/banana/`: `run-retroarch <core> <rom>`, `run-ps1` (which picks
SwanStation or PCSX-ReArmed from Settings and hands over), and `run-port` for PortMaster.

Before the launcher runs, banana-ui has:

- closed its display (it is no longer DRM master; the panel is free), closed the input devices and the sound
  device;
- set the system's clock profile (`banana-perf`, the `perf` in `systems.conf`);
- written the game's pid to `/run/banana/game.pid`;
- exported `BANANA_ROM` and `BANANA_SYSTEM` (the system's id, what `run-retroarch` reads for its per-system core),
  and, where set by the kernel command line (`banana.top=`), `BANANA_TOP`.

The launcher runs in its own session (`setsid`), with signals at their defaults and `/data` as the card.

## The hardware, as an emulator sees it

| | |
|---|---|
| The panel | Through **cage** (a kiosk Wayland compositor: one application, fullscreen). cage takes DRM master on the `msm` card (`/dev/dri/card1`), sets the panel's mode as it is (never the timing) and page-flips on vblank, and it turns the output with the panel: the device tree's `rotation = <90>` becomes the connector's "panel orientation", which wlroots reads. RetroArch is a Wayland client, so **everything** it draws -- the game, its menu, the OSD, the widgets -- is composed in the landscape shape the user is looking at and turned once, by cage, on the way to the panel. `run-retroarch` therefore forces `video_rotation 0`; there is nothing above cage that wants a rotation. |
| GPU | The Adreno a613 through Mesa's freedreno (A6xx), EGL on GBM, OpenGL ES 3.2, and **Turnip** (Mesa's freedreno Vulkan; Vulkan 1.0 on this part, the a6xx gen1 low block having no hardware multiview). SwanStation's Vulkan renderer runs at 60 fps at 5x through it. Software rendering is fine for the lighter cores. |
| Touch | evdev; the panel's touchscreen is the FT5452, the only one, so its events are screen 0. It reports the panel's own (unrotated) axes, so coordinates need the same turn as the picture. |
| Buttons | evdev, one device: the MCU's `rsinput` gamepad. A = `BTN_EAST`, B = `BTN_SOUTH`, X = `BTN_NORTH`, Y = `BTN_WEST`, d-pad = `BTN_DPAD_*`, L/R = `BTN_TL`/`BTN_TR`, L2/R2 = `BTN_TL2`/`BTN_TR2`, Start/Select = `BTN_START`/`BTN_SELECT`, F = `BTN_MODE`. Read it, never grab it (`EVIOCGRAB`): bananad reads the same device for the power button and the volume keys. The volume keys (`KEY_VOLUMEUP/DOWN`) and F + volume (brightness) are bananad's; ignore them. In a game a volume press is handed to RetroArch through `/run/banana/retroarch.cmd` (`VOLUME_UP`/`VOLUME_DOWN`) so it changes its own volume live and shows its OSD; only the menu edits the card's mixer. |
| Sound | ALSA, device `default` (bananad routes it and writes `/run/banana/asound.conf`; the card is the WCD937x codec with the two AW88166 amplifiers through the AudioReach DSP). Three rules on this platform: the route must be set **before** the stream opens (the DSP takes the mixer values at the open), only the first frontend makes sound (so `run-retroarch` re-asserts the route and waits for the menu's stream to be released first), and the device can abort (`SIGABRT`, exit 134) if the DSP is still tearing the menu's graph down — `run-retroarch` reads the substreams' status and waits, and retries the start if it aborts anyway. |
| CPU | Six Cortex-A55 (`policy0`) and two Cortex-A78 (`policy6`); `banana-perf` sets every policy before the game starts. An emulator that lowers a policy's `scaling_max_freq` must put it back on exit. |
| Clock | The kernel's; the time zone is in `TZ`. |

## While it runs

- **Sleep**: a short power press (there is no lid on this handheld) sends `SIGSTOP` to the game's process group,
  then suspends; on waking, `SIGCONT`, within a millisecond of the kernel returning. The emulator sees nothing but a
  gap in time: don't drop audio on a timing jump, and don't treat a vblank that took seconds as an error. The panel
  comes back on by itself (the kernel's DRM resume restores the last framebuffer), so no modeset is needed on wake.
- **Quitting**: a hotkey of RetroArch's own (F + Start; `quit_press_twice` is off). On exit, banana-ui reopens the
  panel and records the play (anything over 5 s). Exit code 0 is a normal quit; anything else is logged.
- **Shutdown while playing**: a long power press or an empty battery sends `SIGTERM` to the process group and waits
  up to 5 s for saves to be written before powering off. Write saves on `SIGTERM`.

## Where things go on the card

| | |
|---|---|
| Games | `/data/roms/<system>` (`folder` in `systems.conf`); `.zip` ROMs are the emulator's to read |
| BIOS files | `/data/bios` |
| Saves and states | `/data/saves/<system>`, `/data/states/<system>` |
| RetroArch's own files | `/data/retroarch` (config, remaps, core options, playlists; `HOME`/`XDG_CONFIG_HOME` point there, never the read-only root) |
| Logs | `/data/banana/logs/retroarch.log` (the one thing worth having after a game that would not start) |
| Screenshots | `/data/screenshots` |

Everything under `/usr` is read-only. `run-retroarch` forces the keys a picture and a sound depend on at every
start (`video_driver`, `video_context_driver`, `input_driver`, `audio_driver`, `audio_device`, `video_monitor_index`
on a one-panel handheld, `midi_driver = null`, `config_save_on_exit`, `quick_menu_show_core_options_flush`), because
RetroArch writes `retroarch.cfg` back when it quits and one trip through its own menus otherwise leaves a value the
handheld cannot use. What the user changes in RetroArch's own menu is kept: RetroArch saves `retroarch.cfg` on quit
and flushes a core's options to its `.opt` as the core unloads (on their own; the manual *Save Core Options* item is
also shown, where Banana used to hide it). **Quick resume** (Settings > Other systems) is on by default: RetroArch
saves an automatic state when a game quits and loads it when the game starts again (`savestate_auto_save` and
`savestate_auto_load`, which `run-retroarch` keeps in step).

## Settings

Settings > Other systems holds, per system, the core (`core.<system>` in `banana/settings.conf` — another installed
core instead of `systems.conf`'s, e.g. mGBA for the Game Boy Advance for its RetroAchievements), and the shader
(`ra_shader`), audio latency (`ra_audio_latency`) and VSync (`ra_vsync`) RetroArch runs with, plus integer scaling
(`ra_scale`). `run-retroarch` writes them into `retroarch.cfg` before each start; a change made in RetroArch's own
menu afterwards is kept until the setting changes here again. The shader is RetroArch's global preset, a reference
to one of the image's **Slang** presets in `/usr/share/libretro/shaders/slang` (copied to the card they were hundreds
of MB of small files in exFAT's large clusters and a long first start). Slang, not GLSL: the game's video driver is
Vulkan, whose shader backend is Slang, and a GLSL preset under it crashes RetroArch (a backend mismatch).

RetroAchievements: the account from the first-time setup (`banana/accounts.conf`) is written into `retroarch.cfg`
as its name, but its password and the token RetroArch gets with it go in a config appended from `/run`, never in
`retroarch.cfg`, which file sharing shows. **Badges** are on (`cheevos_badges_enable`): the menu's achievement toast
shows the PNG RetroArch downloads per achievement, in `thumbnails/cheevos/badges` on the card.

### What the menu learns from a game

RetroArch's unlock path draws its own widget and stops there -- nothing outside the process knows an achievement was
won, so the menu could only show it the next time the game was opened. `buildroot/patches/retroarch/0013` writes two
files from where RetroArch already has the information, both in the runtime directory the launcher names:

* `BANANA_RA_CHEEVOS` (default `/run/banana/cheevos`), one tab-separated line per unlock: `unlocked`, `total`, `title`,
  `description`, `points`, `rarity`, and the full path of the badge PNG. The menu's toast is built from these lines and
  the badge is drawn in the tile. It is written before `rcheevos_award_achievement()`, so turning RetroArch's own
  in-game widget off (`cheevos_visibility_unlock`) does not turn the menu's toast off with it.
* `BANANA_RA_PROGRESS` (default `/run/banana/ra-progress`), the game's `"<unlocked> <total>"`, written at each unlock and
  once as the game loads. The menu's per-game card reads it (`library.cpp`), and it used to have no writer at all.

Both are only ever written by RetroArch. A missing file, a full card or an unwritable `/run` writes nothing, and the
game carries on: nothing about this changes the emulator's own behaviour.

`banana-ui` sets both environment variables for the game it starts (`run-retroarch` and RetroArch inherit them), unlinks
the files before the game and reads `cheevos` after it, queuing one toast per line. A **system-wide hook** exists beside
them:
`/run/banana/toast`, one line `"<glyph>\t<title>\t<body>"` (glyph 0 "i", 1 a tick, 2 a cross), which anything can
write -- an update, a Wi-Fi join, the account check -- and the menu reads on its next pass.

## The PlayStation

`run-ps1` picks the core from `core.psx`: **SwanStation** (DuckStation's libretro core, the default) or
**PCSX-ReArmed**. SwanStation's renderer is its own option; Banana writes **Vulkan at 5x** (the panel's own 1080p),
which runs at 60 fps through Turnip -- where its software renderer and the freedreno OpenGL path both fell short.
PCSX-ReArmed is the ari64 dynarec with its NEON rasterizer, the lighter of the two. RetroArch's integer scaler is
off for SwanStation, which fills its own 4:3 viewport. A core override saved from RetroArch's own menu can't take
the panel settings away: `run-ps1` drops `video_monitor_index` and `stdin_cmd_enable` from it.

`run-retroarch` runs **every** core on Vulkan (the a613 through Mesa's Turnip) -- except Dreamcast, below: the games and their shaders share
one driver, whose shader backend is Slang. A core that asks for OpenGL -- the N64 core's GLideN64 -- is no problem:
RetroArch forces `glcore` for that core by itself (a libretro core's hardware context comes from the video driver,
and `gfx/video_driver.c` switches to the one the core's context asks for), so N64 keeps its GL and the rest run on
Vulkan. A machine with no Turnip (the QEMU test's virtio GPU) has no Vulkan device, so `run-retroarch` falls back to
GL there (it checks the device tree's `qcom,sm4450`). The *context* driver is the Wayland one, because the game runs
under cage: `vk_wayland` for Vulkan (the Wayland surface Turnip's `VK_KHR_wayland_surface`) and `wayland` for GL, in
place of the `khr_display`/`kms` pair it used when it took the panel itself. That also means the vulkan-loader must
be built with Wayland WSI (`BUILD_WSI_WAYLAND_SUPPORT`) and Mesa with the `wayland` platform, or the instance has no
`VK_KHR_wayland_surface` and RetroArch gives up on the video driver and exits -- the game is back in the menu before
it starts.

## The Dreamcast

**Flycast** is the core (`systems.conf`'s `[dreamcast]`, run by `run-dreamcast`). It needs a real BIOS --
`dc_boot.bin` and `dc_flash.bin` in `banana/bios` on the card -- and reads `.chd`/`.gdi`/`.cdi` images. Master System
and Game Gear are not Flycast: they share the Mega Drive's Genesis Plus GX core (`systems.conf`'s `[mastersystem]`
and `[gamegear]`). Four things are Banana's rather than the core's:

- **glcore, not Vulkan (Dreamcast only).** Flycast is the one core this handheld cannot quite hold: sampled on the
  unit, its emulation thread spends most of its time in Flycast's own render-thread wait (a futex) with the GPU
  already at its top step, the console runs 55-60 fps, and the sound comes out choppy -- with no ALSA underrun at all,
  because the renderer is what paces the emulation. ROCKNIX runs this SoC on `glcore` (freedreno's GLES) for the same
  reason, and Flycast keeps a GLES renderer when the frontend asks for GL instead of Vulkan, so `run-retroarch` gives
  this one system `glcore`. glcore's shader backend is GLSL rather than Slang, so the Slang preset Banana would apply
  cannot load under it and the shader is left off for Dreamcast (the preset file is untouched).
- **The triggers are on the shoulders.** The Dreamcast's L/R are analogue *triggers* (Crazy Taxi's brake and
  accelerator), and Flycast reads them from the RetroPad's **L2/R2**, not its L/R -- which it reads as the Dreamcast's
  digital C/Z (`shell/libretro/libretro.cpp`, `lt[port] = get_analog_trigger(..., RETRO_DEVICE_ID_JOYPAD_L2)`).
  `run-dreamcast` writes a core remap (`/data/retroarch/remaps/Flycast/Flycast.rmp`) swapping the two shoulder pairs,
  so **L1/R1** work them, and leaves it alone once RetroArch has one (RetroArch rewrites that file itself whenever a
  core remap is active). Two traps in its format: a remap file spells its keys `input_playerN_btn_<name>`, the
  *opposite* order from an autoconfig's `input_playerN_<name>_btn`, and its values are RetroPad ids (L=10, R=11,
  L2=12, R2=13) rather than physical button indices.
- **No analogue stick.** The RPC is a stickless slate, so the d-pad drives the emulated left stick (RetroArch's
  "Analog to Digital", `input_player1_analog_dpad_mode=1`, which `run-dreamcast` sets), and **F + B** toggles it while
  a game runs -- bananad writes `TOGGLE_ANALOG_DPAD` to RetroArch's command FIFO, the command the RetroArch patch
  adds.
- `Flycast.opt` (`/usr/share/banana/cores/`, written once by `run-dreamcast` into the core's own config) is ROCKNIX's
  for this SoC: native 640x480, no texture upscaling, the fast alpha sorting, no anisotropic filtering, the renderer
  on its own thread, and **auto skip frame on** (`some`) so a frame the a613 cannot draw is dropped rather than
  stalling the emulation thread.

## The ZX Spectrum

**Fuse** is the core (`systems.conf`'s `[zxspectrum]`, run by `run-retroarch fuse`, with no per-core setup of its
own). It is a software core, the lightest of the computers, so the system's
`perf` is `powersave` -- the A55 cluster alone, no A78s woken for it. It loads tapes (`.tzx`, `.tap`), snapshots
(`.z80`, `.sna`, `.szx`), disks (`.dsk`, `.trd`, `.scl`), `.rzx` recordings, and a `.zip` of any of them
(libspectrum reads through libretro-common's VFS, so an archive is opened in place).

The Spectrum expects a **keyboard**, which this handheld has not: the core maps the RetroPad to a Kempston-style pad
for user 1 by default, so the d-pad and A/X/Y (fire) play the many joystick games, and **SELECT** raises the core's
own on-screen keyboard for the ones that want a typed command (L1/R1 are RETURN and SPACE). RetroArch's own Controls
and Core Options change all of it. The machine is a core option too (`fuse_machine`, 48K by default); the 48K, 128K,
+2 and +3 are built into the core, while the Pentagon and Scorpion clones need their ROMs in `banana/bios/fuse`
(RetroArch's system directory, the core's `fuse/` subdirectory), which is what `systems.conf`'s comment above the
system says.

Fuse takes no RetroAchievements (its `.info` has memory monitoring off), so the only setting that touches it is the
core pick, which is not offered: it is the one Spectrum core in the image.
