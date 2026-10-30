# Installing BananaOS on the Retroid Pocket Classic

Two things stand between a stock Retroid Pocket Classic and BananaOS. The handheld ships with Qualcomm's own ABL,
which boots **Android only**, so first you flash a Linux bootloader (**OpenABL**); then the card needs the Banana
image. This page walks both, from a stock Android handheld.

> [!CAUTION]
> <span style="color:red">**A Retroid Pocket Classic (Qualcomm Snapdragon G1 Gen 2 / SM4450, board "Rev Q" — the one
> this build targets).**</span> Getting the **wrong chipset's** ABL (`abl_signed-<other>.elf`) is the classic way to
> brick the device, and flashing a bootloader is the one step here that can. It is only needed once.
>
> **I am not responsible if you accidentally brick your device.**

## What you need

- A **Retroid Pocket Classic** (Qualcomm Snapdragon G1 Gen 2 / **SM4450**, board "Rev Q" — the one this build targets).
- A **microSD card**: the image is ~0.7 GB, and the rest of the card becomes the games partition, so 32 GB or more for
  a library. (exFAT is made on first boot; you don't format it.)
- A **card reader** and a computer (Linux, Windows or macOS).
- Optional but handy: a USB‑C **data** cable — for `fastboot`, for the Android ABL scripts, and later for updates.
- The image: **`banana-retroid-pocket-classic.img.gz`** from
  [the Releases page](https://github.com/jesussaves-ai/Banana-Pocket/releases).

## Step 1 — Install a Linux bootloader (SM4450)

The handheld ships with Qualcomm's own ABL, which boots **Android only**. Banana needs a patched **ABL** — a menu and
a standard Linux boot, so it reads `extlinux/extlinux.conf` from the card. **Two different projects** provide one, and
either works with Banana (the menu differs a little):

- **OpenABL** — <https://github.com/OpenHandheldCollective/OpenABL> — the graphical menu; this is what Banana's docs
  and current ROCKNIX SM4450 builds use. Its releases carry `abl_signed-SM4450.elf` (+ `.sha256`) and
  `openabl-recovery-flashable-sm4450.zip`.
- **ROCKNIX‑ABL** — <https://github.com/ROCKNIX/abl> — ROCKNIX's own, installed *from the device* with its
  `update.sh` (or the `rocknix_abl` folder's `backup_abl.sh` / `flash_abl.sh` run as root from Android).

Whichever you pick, the image must be for **SM4450**, and check its `.sha256` before flashing.

### 1a. OpenABL with `fastboot` (the usual way)

The Retroid Pocket Classic's bootloader is **already unlocked**, so `fastboot` flashes the ABL straight away — there
is no OEM‑unlocking step and nothing is wiped. (`fastboot getvar unlocked` says `unlocked: yes`; only a *locked*
bootloader would need OEM unlocking and `fastboot flashing unlock`, which erases Android's data.)

1. Download `abl_signed-SM4450.elf` and its `.sha256` from the OpenABL releases and check it (`sha256sum -c`).
2. Power off; hold **Volume Down** and press **Power** → `fastboot devices` lists the handheld.
3. Flash **both** A/B slots:

   ```sh
   fastboot flash abl_a abl_signed-SM4450.elf
   fastboot flash abl_b abl_signed-SM4450.elf
   ```

4. `fastboot reboot`; hold **Volume Down** at power‑on for the OpenABL menu (*Boot mode → Linux*, *Boot source →
   Auto*, *Start*).

### 1b. OpenABL from a custom recovery

`openabl-recovery-flashable-sm4450.zip` in a custom recovery such as TWRP: it writes ABL to both slots and adds an
`addon.d` script that re‑flashes the other slot after an Android OTA.

### 1c. ROCKNIX‑ABL, from the device

ROCKNIX's `update.sh` fetches its release and writes `/dev/disk/by-partlabel/abl_a` and `abl_b`; run it on the device
(from its Linux, or from Android with root via **Handheld Settings → Advanced → "Run script as Root"**, or the
`rocknix_abl` folder's **`backup_abl.sh` then `flash_abl.sh`**). **Back up first** — those scripts write `abl_a.img` /
`abl_b.img`, which is how you restore the factory bootloader.

### If the flash goes wrong

A bad ABL is recoverable through Qualcomm **EDL (9008)** with the device's own firmware, or from a backup of the ABL
partitions (take one before flashing OpenABL too: from a root shell,
`dd if=/dev/block/by-name/abl_a of=abl_a.img`). Getting the chipset wrong is the classic way to brick the device.

## Step 2 — Write the Banana card

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

## Step 3 — First boot

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

## Step 4 — Games

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

In the bootloader menu set **Boot mode → Android**. Installing OpenABL (or ROCKNIX‑ABL) does **not** remove Android or
its data — the bootloader is already unlocked and nothing is wiped to flash it. An ABL backup (`abl_a.img` /
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
