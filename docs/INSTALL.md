# Installing BananaOS on the Retroid Pocket Classic

Two things stand between a stock Retroid Pocket Classic and BananaOS: the handheld needs a bootloader that reads a
Linux card (that is **OpenABL**, which this device may or may not already have), and the card needs the Banana image.
This page walks both, from a stock Android handheld.

> **Before anything else.** Flashing a bootloader is the one step here that can brick the device, and it is only
> needed once. If your handheld already shows the OpenABL menu (Step 1), skip to Step 3 and flash nothing. Getting
> the **wrong chipset's** ABL (`abl_signed-<other>.elf`) is the classic way to brick it; the Retroid Pocket Classic is
> **SM4450**.

## What you need

- A **Retroid Pocket Classic** (Qualcomm Snapdragon G1 Gen 2 / **SM4450**, board "Rev Q" — the one this build targets).
- A **microSD card**: the image is ~0.7 GB, and the rest of the card becomes the games partition, so 32 GB or more for
  a library. (exFAT is made on first boot; you don't format it.)
- A **card reader** and a computer (Linux, Windows or macOS).
- Optional but handy: a USB‑C **data** cable — for `fastboot`, for the Android ABL scripts, and later for updates.
- The image: **`banana-retroid-pocket-classic.img.gz`** from
  [the Releases page](https://github.com/jesussaves-ai/Banana-Pocket/releases).

## Step 1 — Does the handheld already have OpenABL?

Power it **off**, then hold **Volume Down** and press **Power**.

- **A menu appears** (its items include *Start*, *Boot mode*, *Device model*, *Boot source*) → **OpenABL is already
  there. Skip to Step 3.**
- **Android boots** (or nothing does) → install OpenABL in Step 2.

OpenABL is a patched Qualcomm ABL with a graphical menu and a standard Linux boot: it is what reads
`extlinux/extlinux.conf` from the card. Without it the handheld boots Android only.

## Step 2 — Install OpenABL (SM4450)

Download, from the OpenABL releases, the file **for this chipset** and its checksum:

- `abl_signed-SM4450.elf`
- `abl_signed-SM4450.elf.sha256` — check it before flashing (`sha256sum -c`).

<https://github.com/OpenHandheldCollective/OpenABL/releases>

Then pick **one** of the three ways to put it on the handheld.

### 2a. From Android, as root — no unlock, Android data kept (recommended)

Retroid's Android can run a script as root (its own **Handheld Settings → Advanced → "Run script as Root"**; the exact
path varies by firmware), with no Magisk. This is how ROCKNIX installs its ABL on these handhelds, and it **backs up
the current bootloader first**:

1. Take the **`rocknix_abl`** folder from a ROCKNIX SM4450 SD card (or the equivalent OpenABL scripts), and copy it to
   the **root of Android's internal storage**.
2. Android → **Handheld Settings → Advanced → Run script as Root** → select `rocknix_abl/<SM4450>/backup_abl.sh` →
   **Run**. It writes `abl_a.img` / `abl_b.img` next to the script — **copy those to your computer**; they are how you
   get Android's own bootloader back.
3. Run **`flash_abl.sh`** the same way.
4. Power off, hold **Volume Down** + **Power** → the OpenABL menu → set *Boot mode* to **Linux** → *Start*.

### 2b. With `fastboot` — unlocks the bootloader, wipes Android

1. In Android: **Settings → About** → tap **Build number** seven times; then **Settings → System → Developer options**
   → enable **OEM unlocking** (and **USB debugging**).
2. `adb reboot bootloader` (or power off, hold **Volume Down**) → `fastboot devices` shows the handheld.
3. `fastboot flashing unlock` — the handheld asks for confirmation; **this erases Android's user data**.
4. Flash both slots:

   ```sh
   fastboot flash abl_a abl_signed-SM4450.elf
   fastboot flash abl_b abl_signed-SM4450.elf
   ```

5. `fastboot reboot`; hold **Volume Down** at power‑on for the menu.

### 2c. From a custom recovery

`openabl-recovery-flashable-sm4450.zip` (OpenABL releases) installs in a custom recovery such as TWRP. It writes ABL
to both slots and adds an `addon.d` script that re‑flashes the other slot after an Android OTA.

### If the flash goes wrong

A bad ABL is recoverable: the handheld can always be put back through Qualcomm **EDL (9008)** with its own firmware,
or Android's own bootloader restored with the `abl_a.img` / `abl_b.img` backup from 2a. This is why the backup is the
first thing 2a does — and why the chipset must match.

## Step 3 — Write the Banana card

Download **`banana-retroid-pocket-classic.img.gz`** and write it to the microSD card. **Everything on the card is
erased.**

- **balenaEtcher** takes the `.gz` as it is — pick the file, pick the card, flash.
- Or with the repository's checker (it confirms the card is removable, big enough and unmounted, then writes it and
  reads it back sector for sector — a card that lies about writes is the usual cause of a half‑boot):

  ```sh
  sudo tools/write-sd.sh banana-retroid-pocket-classic.img.gz /dev/sdX
  ```

- Or plain `dd`:

  ```sh
  zcat banana-retroid-pocket-classic.img.gz | sudo dd of=/dev/mmcblk0 bs=4M status=progress
  ```

## Step 4 — First boot

Put the card in the handheld, then power on (holding **Volume Down** if the OpenABL menu is set to Android):

- In the OpenABL menu set **Boot mode → Linux** and **Boot source → Auto**, then *Start*. (Once set, an ordinary
  power‑on boots Linux; **Volume Up** boots Android.)
- The boot animation plays; the very first boot takes a few seconds longer, making the data partition from the rest of
  the card.
- The **first‑time setup** opens: **Wi‑Fi**, **time zone**, **RetroAchievements**, **Box art**, **Finish**. **B** skips
  it (Settings → System → First‑time setup brings it back).
- The menu appears on the shelf of systems. There is no Nintendo DS page: this handheld has one screen and ships the
  RetroArch systems.

If the picture is turned or upside down, add `banana.rotate=0`, `90`, `180` or `270` to the `APPEND` lines in
`extlinux/extlinux.conf` on the card's **BANANA-BOOT** partition and reboot — no rebuild needed.

## Step 5 — Games

Power off, put the card in a computer: a **BANANA** drive with `roms/`, `bios/`, `banana/`.

- Games go in **`roms/<system>`** — `roms/gba`, `roms/snes`, `roms/psx`, and so on (the folder names are in
  `banana/README.txt`, and each shelf says where its games go when it is empty).
- **BIOS** files go in **`banana/bios`** for the systems that need one: Dreamcast (`dc_boot.bin`, `dc_flash.bin`),
  Sega CD (`bios_CD_U.bin` / `_E` / `_J`), PC Engine CD (`syscard3.pce`), Atari Lynx (`lynxboot.img`).
- **Box art** is fetched by the menu itself over Wi-Fi (Settings → System → Box art).

## Updating later

A release's **`banana-retroid-pocket-classic-<version>.update.tar`** goes in **`banana/updates/`** on the card (from a
computer, over File sharing, or over SSH), then **Settings → System → Update** lists it and **A** twice writes it to
the slot not in use and checks it hash by hash. **Previous version** (Settings → System) restarts into the build
before. Nothing on the data partition — games, saves, settings — is touched.

## Booting Android again, or uninstalling

In the OpenABL menu set **Boot mode → Android**. Installing OpenABL does **not** remove Android. If you took the
`fastboot` route (2b) or installed a custom recovery, Android's data was wiped and a backup (`abl_a.img` /
`abl_b.img`) is your way back to the factory bootloader.

## When something is wrong

`banana/logs/` on the card (put it in a computer) has `boot.txt` (the milestones in seconds), `dmesg.txt`, the menu's
`banana-ui.log`, `sleep.log`, and the last game's `retroarch.log`. **`docs/FIRST-BOOT.md`** has the common cases: a
black panel with no `BANANA` drive (the kernel or init — a serial console on `ttyMSM0`, `banana.console=1`), a black
panel with the drive there (the menu could not open the panel), a black panel a second after a game starts (the probes
in `docs/RPC-TEST.md`), and "can't read the card's games" (the data partition did not mount).

---

Sources for the bootloader steps: the [OpenABL project](https://github.com/OpenHandheldCollective/OpenABL) and the
[ROCKNIX](https://rocknix.org) SM4450 ABL install (the `rocknix_abl` scripts). The card's own layout, the A/B slots
and the update flow are in `docs/ARCHITECTURE.md`.
