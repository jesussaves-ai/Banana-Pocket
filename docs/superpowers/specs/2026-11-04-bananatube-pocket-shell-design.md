# BananaTube: the Pocket shell (visual rehaul)

## Intent

BananaTube works, but it does not look like an app that ships with BananaOS. The OS's identity is already written down in
`system/rootfs/usr/share/banana/themes/pocket.json`: warm brown-black glass, butter text, muted olive, banana gold,
crisp 2 px borders, a dot-grid backdrop and Pixelify Sans. BananaTube today is flat navy-black with 1 px hairlines,
Inter and a vermillion-red accent; it reads as a separate product.

This is a visual rehaul, not a feature change: same search, playback, tabs, bindings and IPC. The chrome, palette, type
and cards move to the Pocket vocabulary, and the top-left corner gets the BananaTube wordmark with the OS banana mark
beside it.

## Goals

- Pocket tokens everywhere: `#17140d` ground, `#201b11` panel, `#2a2315` card, butter `#ffefb2` ink, olive `#bfa963`
  secondary, gold `#ffd21e` for focus/active/brand, `#ffe873` highlight, `#8c7328`/`#4a3d18` edges, `#0b0906` bezel.
- Pixelify Sans (installed by banana-ui at `/usr/share/banana/rnds/fonts`), Medium for titles and the wordmark, Regular
  for body and chips; Inter stays the fallback and `--font` still overrides everything.
- A branded top strip: banana mark + **BananaTube** wordmark on the left, the quality cap on the right.
- A framed content panel (bezel, ring, panel, dot grid) like the menu's screen, with the header, tabs, content and
  footer inside it.
- Four larger result cards per page, with gold focus, thumb badges and a clean title hierarchy.
- A keyboard that looks like the OS's: chunky bordered keys, gold selected key with dark ink, `abc`/`ABC` Shift label,
  Search marked when it can act.
- A footer of button chips (the menu's pill idiom) instead of one long line of text.
- The player's mpv OSD uses the same palette and type as far as mpv allows.
- Keep A1: no drawing while playing, no per-frame allocation, dirty frames only, idle CPU stays ~0.2 %.

## Non-goals / anti-goals

- No new features: no settings screen, captions, downloads, subscriptions or navigation changes.
- No layout engine, theme picker, light theme, rounded corners (Pocket is crisp), animations or transitions.
- No app-side overlay over the video; mpv's text OSD remains the only player UI.
- No clock/battery in the top strip (the app does not read them; that would be new sysfs work).
- Anti-goal: a golden everything look. Gold is reserved for focus, the active tab, the brand and a primary action.
  Cards stay brown-black; text stays butter/olive.
- Anti-goal: sacrificing legibility for the pixel look. Body text stays at the current 32 px minimum; titles are Medium.

## Constraints

- `app/bananatube/main.cpp` (plus `app.json`, the host test and docs); SDL2, SDL_ttf, SDL_image already linked.
- Works at 1240x1080 and at the host test's 1024x768; all geometry in percentages of the renderer output.
- Draw and touch share the same rect helpers (`backRect`, `queryRect`, `tabRect`, `keyRect`, `actRect`, `resultsRowH`,
  `resultsTop`); a rehaul must not grow a second set.
- No per-frame heap allocation; text textures stay cached by font+colour+string; the dot grid and frame are baked into
  one full-surface texture and copied once per dirty frame (rebuilt on resize, freed at exit).
- The player OSD stays text: `--osd-color`, `--osd-border-color`, `--osd-back-color`, `--osd-fonts-dir`,
  `--osd-font`; the existing 12-cell bar and start card remain.

## Approaches

- **A. Reskin.** Palette, font, 2 px borders, dot backdrop, same layout. Lowest risk, but it still reads as the old app
  in new colours.
- **B. Pocket shell (chosen).** A plus the menu's chrome: brand strip, framed panel, themed cards, chunky keys, button
  chips. Same draw code and layout logic, no new dependency.
- **C. Full redesign.** Left tab rail, summary panels, transitions. Most work and it stops looking like the menu's
  DS-style layout, which is the opposite of "within the theme".

Decision: **B**, with the requested banana mark + wordmark. If the pixel type ever feels too playful, that is a new
decision, not part of this one.

## The design

### Tokens (single set of constants in `draw()`)

| Token | Value | Use |
|---|---|---|
| `BG` | `#17140d` | window ground, text on gold |
| `BEZEL` | `#0b0906` | outer strip, input wells, footer |
| `RING` | `#342c19` | frame ring, footer top line |
| `PANEL` | `#201b11` | content panel |
| `CARD` | `#2a2315` | cards, keys, idle buttons |
| `LINE` | `#4a3d18` | card/key borders |
| `EDGE` | `#8c7328` | secondary borders, chips |
| `INK` | `#ffefb2` | primary text |
| `SOFT` | `#bfa963` | metadata, inactive labels |
| `GOLD` | `#ffd21e` | focus, active tab, brand, resume chip |
| `GOLD_HI` | `#ffe873` | inner highlight, selected title |
| `DOT` | `#241f14` | dot grid |

The red accent (`#e0402f`) is removed. Border width is `bw = max(2, W / 310)` (4 px at 1240, 3 px at 1024).

### Type

- Load `PixelifySans-Medium.ttf` for `fBig` (5 % H) and `fMed` (4 % H), `PixelifySans-Regular.ttf` for `fSmall` (3 % H).
  If a file is missing, both fall back to the current Inter path; `--font FILE` overrides both faces for tests.
- Titles, tab labels, the wordmark and key letters use `fMed`; metadata, chips, status and hints use `fSmall`.
- The text cache already keys by font pointer and colour, so the palette/font change needs no cache change; fonts are
  recreated on resize with the existing cache clear.

### Frame and backdrop

- Bake a full-surface texture once: `BG`, then the outer frame from 2 % W / 8 % H to 98 % W / 97 % H (bezel, 2*bw ring,
  panel), then a 2x2 px `DOT` every 16 px inside the panel.
- One `SDL_RenderCopy` per drawn frame; rebuild on output-size change; destroy at exit. No per-frame loops of dots.
- The brand strip (0–7 % H) sits on `BEZEL` above the frame, with a `RING` line at its foot.
- The footer strip (89.5–96.5 % H) is a `BEZEL` band inside the panel with a `RING` top line.

### Brand strip

- Banana mark: `/usr/share/banana/banana/banana-48.png` (fallback `banana-32.png`) at 2.5 % W, vertically centred,
  48 px square; loaded once into a texture, freed at exit. If absent, the wordmark moves left and no placeholder is
  drawn.
- Wordmark: **Banana** in `GOLD` + **Tube** in `INK`, `fMed`, one baseline, 12 px after the mark. It is a text
  wordmark, not a new image asset.
- Right: the quality cap (`720p`) in a `CARD`/`EDGE` chip with `GOLD` text. SELECT still cycles it.

### Header (9.5–16.5 % H)

- Back chip, 10 % W wide, `CARD` fill, `EDGE` border, `fSmall` "Back" in `INK`. Same `back()` target.
- Search well from 15 % to 96 % W: `BEZEL` fill, `RING` border; while typing, a `GOLD` 2*bw border and `INK` query;
  idle, `SOFT` placeholder "Search YouTube". Text is `fBig` as today, inset 2 % W.

### Tabs (17.5–24 % H)

- Four equal tabs across 4–96 % W (`GAP` 1.5 % W), `CARD` fill and `LINE` border, `SOFT` label.
- Active tab: `GOLD` fill, `GOLD_HI` border, `BG` label; counts ("Recent 8") ride inline and switch to `BG` on gold.
- The quality chip is not duplicated here; it lives in the brand strip.

### Status line (25.5 % H)

- Existing status text in `SOFT` on the left, `n / total` counter in `INK` on the right, `fSmall`, same truncation.

### Content (28.5–88 % H)

- Four rows per page at 1080 (`resultsRowH = H*14/100`); `resultsPerPage` keeps computing from the new top/bottom.
- Card: `CARD` fill, `LINE` border, `bw` wide; selected: `GOLD` border + an 8 px `GOLD` bar on the left edge; title
  `GOLD_HI` when selected, `INK` otherwise; uploader `SOFT`.
- Thumbnail: 16:9, the card's full height minus a 5 px inset, `LINE` border; placeholder is `RING` fill with a
  `LINE` border. Duration chip `BEZEL`/`INK` bottom-right; Recent resume chip `GOLD` with `BG` text top-left.
- Channels: 72 px square avatar, `RING` fill, `EDGE` border, initial in `GOLD`; selected card as above.
- Local rows use the same card without a thumbnail image (the placeholder is the file card).

### Keyboard (28.5–88 % H)

- Ten columns × four rows plus the action row, all inside the panel: `kw` from the 92 % W content width, `keyH` from the
  space between 28.5 % and 88 % H (five rows, four gaps), so the rows fill the panel instead of leaving dead height.
- Key: `CARD` fill, `LINE` border, `INK` letter, `fMed`; centred.
- Focused key: `GOLD` fill, `BG` letter, a 2 px `GOLD_HI` inset line; it reads as a key, not just a red block.
- Action row: Space, Del, Shift, Search (same order and hit rects). Shift shows the layer it is in: `abc` when the
  lower layer is active, `ABC` when upper. Search gets a `GOLD` border whenever the query is non-empty and keeps the
  focus fill when focused; with an empty query it stays `CARD`/`EDGE`.
- All other keys/actions keep their current behaviour.

### Footer hints

- Up to four pills per screen, the menu's idiom: a small `PANEL`/`EDGE` chip with the key (`A`, `Y`, `L/R`) in `INK`,
  then the action in `SOFT`.
  - Keyboard: `A Type`, `B Back`, `Y Search`, `L/R Tab`.
  - Results: `A Play`, `B Back`, `Y Search`, `L/R Tab`.
  - Channels (one open): `A Play`, `B Back`, `L/R Tab`.
  - The player has no app footer (mpv owns the screen).
- Chips are laid out left-to-right with a fixed gap; the full binding list stays in `docs/BANANATUBE.md`.

### Player OSD

- Start card, bar, buffering and SELECT readouts keep their content; style only:
  `--osd-color=#ffefb2`, `--osd-border-color=#17140d`, `--osd-back-color=#201b11`,
  `--osd-fonts-dir=/usr/share/banana/rnds/fonts`, `--osd-font=Pixelify Sans`, font size stays 40.
- If the family is not found, mpv falls back to its default; the app still works. No app-side overlay.

### App tile

- `app.json` accent changes from `#e0402f` to `#ffd21e`, so the Apps shelf tile matches the app.

## Left to the builder

- Exact visual nudges after seeing it on the panel (wordmark size, dot spacing 16 vs 20 px, chip height).
- Whether every tab count fits the tab width at 1024; truncate or drop the count there.
- Whether the mpv build resolves "Pixelify Sans" by family name; if not, try the file name or keep Inter for the OSD.

**Calls settled after the builder review:** no in-app capture code — screenshots come from QEMU, not the app. The page
count is a floor, not an exact number: four rows fit at both 1080 and 768 with `resultsRowH = H*14/100`, and the
layout selftest asserts a minimum of four at 1080 and three at 768.

## Verification

- Host: `make -s test`, `make -s agent-check`, and a new `selftest layout` that asserts, for 1240x1080 and 1024x768:
  the keyboard's action row bottom stays above the footer, the header/tabs/content rects do not overlap, the tabs end
  inside the panel, and at least four result rows fit at 1080 (three at 768 — four fit, but the assertion is a floor).
- QEMU: boot the 0.5.8 image with `KEEP=1 tools/qemu-headless.sh <img> <out>`, serve the cross-built `banana-tube` from
  the host, `wget` it into `/data` over the serial console (`tools/qemu-console.py`), bind-mount it over
  `/usr/bin/banana-tube`, write the app marker into `/run/banana/launch`, then capture each screen with
  `tools/qemu-monitor.py <sock> 'screendump ... -f png gpu 0'`. `sendkey` drives the menu, the keyboard and the tabs.
  This is the path `tools/qemu-e2e.sh` already uses; no extra tooling.
- Unit: the same screens on the real panel (QEMU has no DSI/backlight/touch), plus a quick scroll and idle CPU still
  near 0.2 % (`top`).

## Risks

- Pixelify at 32 px is smaller-looking than Inter at 32 px; if metadata is hard to read at arm's length, raise `fSmall`
  one step rather than shrink the borders.
- The baked background costs one 1240x1080 texture (~5 MB) and must be rebuilt on resize; guard the pointer.
- The frame removes ~1 % of width from the keyboard; keys become slightly narrower (about 103 px at 1240) and taller.
  The selftest layout catches an overflow.
- Gold focus on brown-black is high contrast; the risk is overuse, which the anti-goal above settles.
- mpv OSD styling is best-effort; a font/colour it rejects must not break playback.
