# Theme icons

The system/collection icons the menu draws on the shelf tiles, the resume cards and the game lists. One PNG per
system (or one per kind), named in `ui/assets/rnds/systems.cfg`.

## Where they live

- `ui/assets/overrides/icons/` — **Banana's own** icons (the ones this project adds or redraws). `tools/import-rnds.sh`
  copies this folder over the theme it imports from ROCKNIXDS, so a re-import never loses them. **Add new icons here.**
- `ui/assets/rnds/icons/` — the working set (ROCKNIXDS's BenJelter pack plus the overrides). This is what the build
  installs to `/usr/share/banana/rnds/icons`. It is regenerated on import, so treat it as output, not source.
- `tools/gen-icons.py` — draws some of the simple ones (`pcengine`, `atari2600`, `sega32x`, `segacd`, `ngp`,
  `wonderswan`, `atarilynx`) as reproducible 32×32 pixel art. Run it to rewrite those; it does **not** touch the
  overrides.

## Format

The theme reads one PNG per entry and draws it at a fixed size on the panel, so the file's own pixels are only a
source. Sizes in use:

- **128×128** — the crisp choice (see below). Current: `dreamcast`, `gamegear`, `zxspectrum`, `c64`,
  `mastersystem`, `saturn`, `psp`.
- **32×32** — the older/imported set (BenJelter's pack, and what `gen-icons.py` writes).

Why 128: the software backend draws a system icon at **~96 CSS px (~186 device px)** on the 1240×1080 panel and scales
with **bilinear** filtering. A 32×32 source is upscaled ~5.8× and looks soft; 128 is ~1.45× and stays crisp. Use a
**multiple of 32** (128 is good; 256 also fine) so the art scales cleanly. Transparency (alpha) is expected; the
engine draws the icon on whatever the tile's background is.

## Adding an icon

1. Drop the PNG in `ui/assets/overrides/icons/<id>.png` (and, for the current tree, the same file in
   `ui/assets/rnds/icons/`).
2. Add a line to `ui/assets/rnds/systems.cfg` mapping the system id (the `[id]` in `/etc/banana/systems.conf`) to the
   file and an accent colour, in the "dedicated art" block:

   ```
   <id>   <id>.png   #rrggbb
   ```

   A name listed twice keeps its last line, so a dedicated line overrides the kind line it used to share.
3. `python3 tools/check-systems.py` (and `make test`) — a system's id must map to a file that exists, or the theme
   falls back to its `*` icon (a gamepad).

If you also draw it with `gen-icons.py`, add a `def <id>(c):` and an `ICONS` entry; otherwise just ship the PNG.

## From AI-generated art

The art in `overrides/icons` for the "new" systems came from an image model, then:

1. Generate **pixel art** (bold 1px-ish outline, flat fills, chunky) at a **multiple of 32** — e.g. 512×512 (=16× the
   32 grid) — on a **solid background**.
2. `convert src.png -alpha set -fuzz 12% -fill none -draw 'alpha 0,0 floodfill' -draw 'alpha W,0 floodfill' \
   -draw 'alpha 0,H floodfill' -draw 'alpha W,H floodfill' nb.png` to clear the background (skip if already
   transparent), then `convert nb.png -filter point -resize 128x128 out.png`.

Keep it flat: the engine's bilinear scaling softens gradients and shading, and they won't match the pixel-art set.

## Status

Every shelf Banana ships today has its own art (either a dedicated file or a shared family icon such as
`snes,snesh,...` → `snes.png`). `mastersystem` was the last one without and now has `mastersystem.png`.

## Missing dedicated icons (for future projects)

These systems have **no icon of their own** yet; if a shelf is added for one, the theme shows the generic kind icon in
brackets until art is made (a 128×128 PNG in `overrides/icons`, wired in `systems.cfg` as above).

- **Sega** — `sg1000` (console), `saturn` ✓, `master system` ✓, `game gear` ✓ · have: 32X, Mega-CD, Dreamcast
- **Nintendo** — `gc`/`gamecube` (disc), `wiiu` (disc), `switch` (disc), `virtualboy` (console), `gameandwatch`
  (handheld), `pokemini` (handheld), `3ds` (handheld) · have: GB/GBC/GBA, NES, SNES, N64, NDS
- **Sony** — `ps2` (disc), `ps3` (disc), `psvita` (handheld) · have: PS1 (`psx`), PSP ✓
- **Microsoft** — `xbox`, `xbox360` (console)
- **SNK** — `neogeo`, `neogeocd` (arcade/disc) · have: Neo Geo Pocket
- **Atari** — `atari5200`, `atari7800`, `atarijaguar` (console); `atari800`, `atarist` (computer) · have: 2600, Lynx
- **Computers / other consoles** — `intellivision`, `colecovision`, `vectrex`, `odyssey2`/`videopac` (console);
  `msx`, `msx2`, `amiga`, `amstradcpc`, `dos`, `zx81` (computer)
- **Engines / apps** — `scummvm` (book), `pico-8`, `tic-80` (fantasy), `doom`, `quake` (fantasy)
- **Apps / media kinds** — the bundled apps have their own tiles (`bananatube` = the banana TV, `dodakvarter`,
  `bank`); generic kinds still exist for the rest: `video`, `music`, `picture`, `book`, `gear` (settings), `tools`,
  `apps`, `collection`, `fav`, `search`, `recent`.

Where the BenJelter pack (the source of the good 32×32 ones) lacks a system, generate it as above; where it has it,
prefer that (it matches the imported set, and can be upscaled to 128 with nearest).
