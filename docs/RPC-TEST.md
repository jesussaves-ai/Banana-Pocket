# Retroid Pocket Classic: what to check on the hardware

Write the card (from the host, with the card in a reader):

    sudo tools/write-sd.sh output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz /dev/sdX

Then put it in the handheld and power on: OpenABL (already in `abl_a`/`abl_b`) reads
`extlinux/extlinux.conf` from the card's FAT partition, so the card boots whether or not the
internal Android is there.

## First boot

- [ ] The panel lights up and the Banana splash plays (bunch, BANANAOS, LOADING)
- [ ] The menu comes up **upright** and fills the panel, not sideways or upside down
      (if it is turned: add `banana.rotate=0`, `90`, `180` or `270` to the `APPEND` lines in
      `/boot/extlinux/extlinux.conf` -- no rebuild needed)
- [ ] The first-time setup runs: Wi-Fi lists networks, the time zone changes the clock
- [ ] The menu's text and art are sharp (the UI draws at 1240x1080, 1.9375x the DS layout)

## Panel and power

- [ ] Brightness (Settings > Display & sound) changes the backlight over its whole range
- [ ] Sleep (Settings > Sleep & power, or the power button) turns the panel off, and waking it
      shows the menu again -- this is the ROCKNIX backlight bug, watch for a black screen
- [ ] Charging shows in the status bar, and the percentage rises

## Sound

- [ ] Menu sounds play (Settings > sounds on)
- [ ] A game's music and effects come out of the speaker, both channels (the AW88166 fix)
- [ ] Headphones: plugging in mutes the speaker

## Buttons and sticks

- [ ] D-pad, A/B/X/Y, L/R, Start/Select move through the menu as the hints say
- [ ] Every button registers in Settings (the gamepad is the MCU's `rsinput` device)
- [ ] The 6-button variant's extra buttons (if this is one) work

## Settings

- [ ] The pages are Display & sound, Sleep & power, Other systems, Network, System -- **no Nintendo DS page**: this
      handheld ships without the DS (one screen; there is no two-panel DS emulator here). If a DS page appears, the
      shelf list on the card has an `[nds]` section in it
- [ ] Sleep when idle is off by default here (a slate has no lid to close): the screen dims after two idle minutes and
      stays awake; the power button still sleeps it, and Settings > Sleep & power or `autosleep_minutes` in
      `banana/bananad.conf` asks for the timer back

## Fan and heat

- [ ] The fan spins up under a game and settles when idle (`/sys/class/hwmon/*/pwm1`)
- [ ] Nothing gets too hot after 10 minutes of a N64/PSX game

## Card and data

- [ ] Games on the card's `banana-data` partition show in the library
- [ ] A save survives a reboot

## Updates

- [ ] Settings > System > Update lists a package put in `banana/updates` on the card, and
      applying it writes the other slot and restarts into it

## USB link (development)

The handheld's own port is a USB **device**, and it runs an Ethernet (NCM) gadget with a DHCP server, so plugging it
into a computer gives that computer a network interface **and an address** with nothing to configure -- no Wi-Fi.
`ssh` has to be on for a shell (Settings > Network), and its key is the same one the Wi-Fi link uses.

- [ ] Plugged into a computer, the handheld answers at `169.254.170.2` (the interface is named from the USB path:
      `enp0s20f0u1` on a systemd host, `usb0`/`enx…` elsewhere), around 4 ms and 7 MB/s over `ssh`
- [ ] If nothing enumerates, re-seat the cable: the port attaches when the cable is in a computer, and the gadget is
      bound at boot whether or not one is there
- [ ] A charge-only cable, or a charger, gives you this instead: the handheld charges, `/sys/class/udc/*/state` stays
      `not attached` and no interface appears on the computer
- [ ] With the menu showing, `tools/rpc-screen.sh` fetches a frame of it (`-w` streams them for a viewer); a game
      running gives no frame -- that screen is RetroArch's, and its `SCREENSHOT` command is the way (on the command
      FIFO bananad already writes `TOGGLE_ANALOG_DPAD` to)



The panel going black a second after a game starts has two very different causes, and the handheld looks the same for
both. Tell them apart before touching the kernel:

    sh /data/rpc-hang-watch.sh start 1200        # the kernel's buffer onto the card, synced every second
    /data/rpc-gpu-probe offscreen                # the GPU on its own: prints "freedreno ... FD613" and 120 frames
    killall banana-ui; /data/rpc-gpu-probe dumb   # the panel on its own: a modeset and 64 flips, no GPU
    killall banana-ui; /data/rpc-gpu-probe kms    # both together: the GPU's buffers scanned out

Build the probe on the computer with `tools/rpc-probe-build.sh` (it uses the image's own cross toolchain) and copy
both tools to the card's `/data` with scp; they are not part of the image — they are for a machine that can be read
afterwards.

- All three pass and the game is still black: it is RetroArch's own setup, not the hardware. Its log is
  `/data/banana/logs/retroarch.log` and the keys that must be right (`video_driver` above all — `"null"` there is a
  black panel with the game running under it) are forced by `run-retroarch` every start.
- A probe hangs (the box stops answering, `sh /data/rpc-hang-watch.sh show` after the power button ends the story at
  the step the probe printed last): that is kernel territory — the DPU splash handover, the GPU's power domains. Send
  that file.
- `offscreen` alone hangs: the GPU, its firmware (`/lib/firmware/qcom/sm4450/`) or its clocks.

## What to send back

- Photos of the screen at the menu and in a game
- `logs/boot.txt`, `logs/dmesg.txt`, `logs/messages.txt` and, for a game that would not show anything,
  `logs/retroarch.log` and `logs/hang-watch.*.txt` from the card's `banana/logs`
- Anything on the panel, sound or buttons that is wrong, with the setting or game
