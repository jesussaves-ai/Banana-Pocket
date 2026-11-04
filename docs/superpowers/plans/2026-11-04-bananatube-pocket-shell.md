# BananaTube Pocket shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Give BananaTube the Pocket look — brown-black glass, butter ink, banana gold focus, Pixelify Sans, a framed
screen, a branded top strip, Pocket cards, a chunky keyboard and button-chip hints — without changing any behaviour.

**Architecture:** One-file SDL2 client. A `Skin` value (palette + border width) is threaded through the draw functions;
a full-surface backdrop (bezel, ring, panel, dot grid) is baked once, so the dirty-frame loop still copies one texture
per frame. All geometry moves behind the existing shared rect helpers (draw and touch use the same ones). The player
stays mpv's text OSD, restyled.

**Tech Stack:** C++17, SDL2, SDL_ttf, SDL_image, no new dependency.

**Spec:** `docs/superpowers/specs/2026-11-04-bananatube-pocket-shell-design.md`

## Global Constraints

- Files: `app/bananatube/main.cpp`, `app/bananatube/app.json`, `system/tests/run-banana-tube-test.sh`, `docs/BANANATUBE.md`,
  `docs/ROADMAP.md`; no new dependency, no new source file.
- Palette (exact): bg `#17140d`, bezel `#0b0906`, ring `#342c19`, panel `#201b11`, card `#2a2315`, line `#4a3d18`,
  edge `#8c7328`, ink `#ffefb2`, soft `#bfa963`, gold `#ffd21e`, goldHi `#ffe873`, dot `#241f14`. Gold is for
  focus, the active tab, the brand and a primary action only.
- Type: `PixelifySans-Medium.ttf` for `fBig`/`fMed`, `PixelifySans-Regular.ttf` for `fSmall`; Inter is the fallback and
  `--font FILE` overrides both faces.
- Border width `bw = max(2, W / 310)`; no rounded corners, no animations, no new features.
- No drawing while `Mode::PLAYING`; no per-frame allocation; text cache keyed by font+colour+string; the backdrop is
  built once per output size, copied once per dirty frame, freed at exit.
- Draw and touch share `backRect`, `queryRect`, `tabRect`, `keyRect`, `actRect`, `resultsTop`, `resultsRowH`.
- Works at 1240x1080 and at the host test's 1024x768. `make -s test` and `make -s agent-check` stay green.

## Review Focus

- Long titles/status strings at 32/43 px in Pixelify must truncate inside their box, never run under the ring or the
  counter.
- The keyboard at 1024x768: five rows must end above the footer and stay inside the frame; the computed key height is
  the risk.
- Gold focus must be visible on the keyboard, a card, a channel row and the active tab, and must not become decorative
  gold on idle elements.
- The backdrop must be rebuilt if the output size changes and freed at exit; a stale texture or a leak is the failure.
- mpv must still play if a colour/font option is rejected; the OSD options must not abort playback.

---

### Task 1: the shell (tokens, type, backdrop, brand, header, tabs)

**Files:**
- Modify: `app/bananatube/main.cpp` (constants near `DEFAULT_FONT`; `struct App` members; `draw()`; geometry block;
  `selftest()`; cleanup in `run()`)
- Test: `system/tests/run-banana-tube-test.sh`

**Interfaces:**
- Produces: `struct Skin` with `SDL_Color bg, bezel, ring, panel, card, line, edge, ink, soft, gold, goldHi, dot;` and
  `int bw;`; `Skin skin(int W) const`; `SDL_Texture* backdrop;` `void ensureBackdrop(int W, int H, const Skin& sk);`
  `void box(int x, int y, int w, int h, SDL_Color fill, SDL_Color edge, int border);`
  `int brandH(int H) const` (7 % H), `int contentTop(int H) const` (28.5 % H), `int contentBottom(int H) const` (88 % H),
  `int hintTop(int H) const` (89.5 % H), `int hintH(int H) const` (7 % H);
  `SDL_Rect frameRect(int W, int H) const` (2–98 % W, 8–97 % H), `SDL_Rect hintRect(int W, int H) const`;
  `TTF_Font* openFont(const std::string&, int px, const char* fallback)`.
- Produces geometry (values): `backRect` = 4 % W, 9.5 % H, 10 % W, 7 % H; `queryRect` = 15 % W, 9.5 % H, 81 % W,
  7 % H; `tabRect(i)` = x 4 % W + i*(tw+gap), y 17.5 % H, tw = (92 % W - 3*gap)/4, gap = 1.5 % W, h = 6.5 % H.
- Produces defaults: `DEFAULT_FONT = /usr/share/banana/rnds/fonts/PixelifySans-Regular.ttf`,
  `DEFAULT_FONT_MED = /usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf`,
  `FALLBACK_FONT = /usr/share/banana/banana/fonts/Inter-Regular.ttf`,
  `FALLBACK_FONT_MED = /usr/share/banana/banana/fonts/Inter-SemiBold.ttf`.

- [x] **Step 1: write the failing layout test**

In `selftest()`, before the bar case, add `selftest layout`: for (1240,1080) and (1024,768) assert the frame is inside
the surface; `backRect`/`queryRect` end above `tabRect`; `tabRect(3)` ends inside the frame; tab rects do not overlap;
`tabRect(0).y + h <= contentTop(H)`; `contentBottom(H) <= hintTop(H)`; and `hintRect` ends inside the frame. Print
`bananatube selftest layout: ok`/`fail`. Add `grep -q 'selftest layout: ok'` to `run-banana-tube-test.sh` as
"layout: frame, tabs and rows fit".

- [x] **Step 2: run it and watch it fail to compile**

Run: `make -s -C app/bananatube` and `system/tests/run-banana-tube-test.sh`.
Expected: the new helpers/fields do not exist; the test script reports no layout line.

- [x] **Step 3: implement the shell**

In `main.cpp`:
- Add the constants, `openFont`, `Skin`/`skin(W)` with the exact palette and `bw`, the five height helpers, `frameRect`,
  `hintRect`, and new `backRect`/`queryRect`/`tabRect`. Leave `resultsTop`/`resultsRowH`/`resultsPerPage` as they are
  until Task 2.
- Fonts: `fontPath`/`fontPathMed` members; `--font` sets both; load `fBig`/`fMed` from `fontPathMed` and `fSmall` from
  `fontPath` with `openFont` and the Inter fallbacks, both in the initial block and the H-scaled block.
- Add `SDL_Texture* backdrop = nullptr; int backdropW = 0, backdropH = 0;` and `ensureBackdrop`: build an
  `SDL_Surface` (`SDL_CreateRGBSurfaceWithFormat(..., SDL_PIXELFORMAT_ARGB8888)`), fill bg, draw the bezel/ring/panel
  bands from `frameRect` (`bw` and `2*bw` insets), the brand strip (`0..brandH`, bezel, `bw` ring at `brandH-bw`), the
  hint band (`hintRect`, bezel, `bw` ring on top), and 2x2 dot pixels every 16 px inside the panel; upload once with
  `SDL_CreateTextureFromSurface`. Return early if the size did not change; destroy/recreate on change. Free it in the
  exit path next to the thumbnails.
- Add `box()` (fill + four edge bars of `border` px) and use it for the new chrome.
- Load the logo once after the renderer: `IMG_Load("/usr/share/banana/banana/banana-48.png")`, fallback
  `banana-32.png`; keep `SDL_Texture* logo`; free at exit. Missing file → nullptr.
- Rewrite `draw()`: `ensureBackdrop`, copy it, make `Skin sk = skin(W)`, then
  - brand strip: logo at 2.5 % W (size `H*45/1000`, centred in the strip), wordmark "Banana" in `gold` + "Tube" in
    `ink` at `fMed`; if there is no logo start at 2.5 % W. Quality chip at the right: `qq = "720p"`, `fSmall`, `gold`
    text, `box(..., CARD, EDGE, bw)`, right margin 2.5 % W, height `H*35/1000`;
  - header: Back box (`CARD`/`EDGE`, `bw`, `fSmall` `ink`); query well (`BEZEL` fill, `RING` border, `GOLD` border
    while `Mode::KEYS`, text `fBig`, placeholder `SOFT`, inset 2 % W);
  - tabs: four `box`es (`card`/`line`, active `gold`/`goldHi`, label `soft`/`ink`/`bg` with count when it fits);
  - status line at 25.5 % H (`soft` status left, `ink` counter right, same strings);
  - content via `drawKeyboard(W,H,sk)` / `drawChannels(W,H,sk)` / `drawResults(W,H,sk)` (compile-time signature change;
    the old bodies keep their colours for this task);
  - delete the old footer strip/text; call `drawHints(W,H,sk)` (add a no-op stub now; Task 4 fills it).
- In the exit path, destroy `backdrop`, `logo`, then the existing caches.

- [x] **Step 4: run the tests**

Run: `make -s -C app/bananatube -B`, `system/tests/run-banana-tube-test.sh` (26 checks, layout ok), `make -s test`.
Expected: green. The look is not verified yet.

- [x] **Step 5: commit**

```bash
git add app/bananatube/main.cpp system/tests/run-banana-tube-test.sh \
    docs/superpowers/specs/2026-11-04-bananatube-pocket-shell-design.md \
    docs/superpowers/plans/2026-11-04-bananatube-pocket-shell.md
git commit -m "bananatube: the Pocket shell's frame, brand and tabs"
```

---

### Task 2: Pocket cards and channel rows

**Files:**
- Modify: `app/bananatube/main.cpp` (`drawResults`, `drawChannels`, `selftest`)
- Test: `system/tests/run-banana-tube-test.sh` (same layout case)

**Interfaces:**
- Consumes Task 1's `Skin`, `box`, `contentTop`.
- Produces `int rowsPerPage(int H) const` (`(contentBottom - contentTop) / resultsRowH`, floor 1); `resultsTop(H)` =
  `contentTop(H)`; `resultsRowH(H)` = `max(56, H*14/100)`; `resultsPerPage()` calls `rowsPerPage` with the renderer
  height. Produces `drawResults(int W, int H, const Skin& sk)` / `drawChannels(int W, int H, const Skin& sk)` with Pocket
  colours; card `H*14/100` tall, row gap `W*1/100`, thumb inset `max(4, W/250)`, thumb `16:9`, selected border `gold`
  + a `max(6, W*8/1000)` left bar, selected title `goldHi`.

- [x] **Step 1: extend the failing layout test**

Add to `selftest layout`: `resultsRowH(1080) == H*14/100`, `resultsTop(1080) == H*285/1000`, and
`rowsPerPage(1080) == 4`; run the test. The current `resultsTop`/`RowH` values (26 %, 12 %) fail.

- [x] **Step 2: implement the cards**

- `drawResults`: empty text in `sk.soft` at `W*6/100, contentTop`; card rect `x=W*4/100`, `w=W*92/100`,
  `h=resultsRowH(H)-W*1/100`; `box(card, sk.card, on ? sk.gold : sk.line, sk.bw)`; selected left bar `sk.gold`; thumb
  `thumbH = cardH - 2*inset`, `thumbW = thumbH*16/9` (cap `W*32/100`), at `x+W*2/100`, `y+inset`; draw image or
  `RING` placeholder with a `LINE` border; duration chip `BEZEL`/`INK` bottom-right; Recent resume chip `GOLD`/`BG`
  top-left; title `fMed` at `y + cardH*12/100`, coloured `sk.goldHi` when selected else `sk.ink`; uploader
  `fSmall`/`sk.soft` at `y + cardH*55/100`; `textW = W*94/100 - textX`.
- `drawChannels`: same card geometry; avatar square `min(cardH - 2*inset, H*8/100)` at `W*6/100`, `RING` fill,
  `EDGE` border, initial `fBig`/`sk.gold`; name `fMed` (`goldHi` when selected) and the video count `fSmall`/`soft`;
  selected border + left bar as the cards.
- Keep the thumbnail prune (call it before the loop) and the scroll clamp exactly as they are.

- [x] **Step 3: run the tests**

Run: `make -s -C app/bananatube -B`, `system/tests/run-banana-tube-test.sh`, `make -s test`.
Expected: green, 26 checks.

- [x] **Step 4: commit**

```bash
git add app/bananatube/main.cpp system/tests/run-banana-tube-test.sh
git commit -m "bananatube: Pocket cards, channel rows and four-a-page density"
```

---

### Task 3: the Pocket keyboard

**Files:**
- Modify: `app/bananatube/main.cpp` (`keyRect`, `actRect`, `drawKeyboard`, `selftest`)

**Interfaces:**
- Consumes Task 1's `contentTop`, `contentBottom`, `frameRect`, `Skin`, `box`.
- Produces `keyRect`/`actRect` inside the frame: `gap = W*1/100`, `kw = (92 % W - 9*gap)/10`,
  `kh = (contentBottom - contentTop - 4*gap)/5`, `x0 = 4 % W`, `aw = (92 % W - 3*gap)/4`;
  `drawKeyboard(int W, int H, const Skin& sk)`.

- [x] **Step 1: add the failing keyboard bounds test**

In `selftest layout`, add: `keyRect(0,0,0).x >= frameRect.x`, `keyRect(3,9).x+w <= frameRect.x+frameRect.w`,
`keyRect(0,0,0).y == contentTop(H)`, `actRect(3).x+w <= frameRect right`, and
`actRect(3).y+h <= hintTop(H)`, for both sizes. Run: the current centred/full-width keys overflow the frame.

- [x] **Step 2: implement**

- New `keyRect`/`actRect` formulas above; `kTop` returns `contentTop(H)`.
- `drawKeyboard`: key `box(..., sk.card, sk.line, sk.bw)` with `fMed` `sk.ink`; focused key `box(..., sk.gold,
  sk.goldHi, sk.bw)` with `sk.bg` text and a 2 px `goldHi` inner outline inset `sk.bw+2`; action labels
  `Space`, `Del`, `upper ? "ABC" : "abc"`, `Search`; Search gets a `gold` border while `!query.empty()` and is
  unfocused (focused still uses the gold fill); all labels centred with `TTF_SizeUTF8`.
- Keep `activate()`/`tapAt()`/`move()` untouched: they already use the rect helpers and `keyCol`/`keyRow`.

- [x] **Step 3: run the tests**

Run: `make -s -C app/bananatube -B`, `system/tests/run-banana-tube-test.sh`, `make -s test`.
Expected: green, 26 checks; `selftest keynav`/`keyrow` unchanged.

- [x] **Step 4: commit**

```bash
git add app/bananatube/main.cpp system/tests/run-banana-tube-test.sh
git commit -m "bananatube: a Pocket keyboard that fills the frame"
```

---

### Task 4: button-chip hints (and Tab for QEMU)

**Files:**
- Modify: `app/bananatube/main.cpp` (`draw`, new `drawHints`, the SDL_KEYDOWN branch)

**Interfaces:**
- Consumes Task 1's `hintRect`, `Skin`, `box`.
- Produces `void drawHints(int W, int H, const Skin& sk);` with the arrays:
  keyboard `{A,Type},{B,Back},{Y,Search},{L/R,Tab}`; results `{A,Play},{B,Back},{Y,Search},{L/R,Tab}`; an open channel
  `{A,Play},{B,Back},{L/R,Tab}`.

- [x] **Step 1: run the layout test**

The band is already pinned by Task 1 (`hintRect` inside the frame, below the content). No new pure geometry is added;
the chip layout is verified visually in Task 6.

- [x] **Step 2: implement**

- `drawHints`: chip height `H*42/1000`, key box `box(..., PANEL, EDGE, 2)` with `fSmall`/`ink`, action
  `fSmall`/`soft`; start at `W*4/100`, y = `hintTop + (hintH - chipH)/2`, group gap `W*3/100`.
- Choose the array from `mode`/`view`; call it at the end of `draw()`.
- In the `SDL_KEYDOWN` branch, before the ASCII append:
  `else if (k == SDLK_TAB && mode != Mode::PLAYING) cycleTab((ev.key.keysym.mod & KMOD_SHIFT) ? -1 : 1);`
  (desktop/QEMU convenience; the pad's L/R is unchanged).

- [x] **Step 3: run the tests**

Run: `make -s -C app/bananatube -B`, `system/tests/run-banana-tube-test.sh`, `make -s test`.
Expected: green, 26 checks.

- [x] **Step 4: commit**

```bash
git add app/bananatube/main.cpp system/tests/run-banana-tube-test.sh
git commit -m "bananatube: button-chip hints and Tab on a desktop keyboard"
```

---

### Task 5: the player OSD and the app tile

**Files:**
- Modify: `app/bananatube/main.cpp` (mpv argv in `play()`), `app/bananatube/app.json`, `docs/BANANATUBE.md`

**Interfaces:**
- Consumes nothing new; the OSD content (start card, bar, buffering, SELECT) is unchanged.

- [x] **Step 1: restyle the mpv OSD**

In the `argv` in `play()`, keep `--osd-level=1`, `--osd-font-size=40`, `--osd-duration=700`, and replace the font block
with `--osd-color=#ffefb2`, `--osd-border-color=#17140d`, `--osd-back-color=#201b11`,
`--osd-font-provider=none`, `--osd-fonts-dir=/usr/share/banana/rnds/fonts`, `--osd-font=Pixelify Sans`; update the
comment (the family is the OS's pixel face; a missing family falls back inside mpv).

- [x] **Step 2: app tile + docs**

`app.json` `"accent": "#ffd21e"`. In `docs/BANANATUBE.md`, add a short **Look** paragraph (Pocket palette, Pixelify,
framed screen, wordmark + banana mark, gold focus) and the desktop Tab note in the controls; leave the pad table intact.

- [x] **Step 3: run the tests**

Run: `make -s -C app/bananatube -B`, `system/tests/run-banana-tube-test.sh`, `make -s test`, `make -s agent-check`.
Expected: green; the player still builds.

- [x] **Step 4: commit**

```bash
git add app/bananatube/main.cpp app/bananatube/app.json docs/BANANATUBE.md
git commit -m "bananatube: the player OSD and the app tile wear the Pocket palette"
```

---

### Task 6: QEMU screenshots, the unit, and close

**Files:**
- Modify: `docs/ROADMAP.md` (status), `docs/superpowers/plans/2026-11-04-bananatube-pocket-shell.md` (checkboxes)
- Test: QEMU screenshots (not committed) + the host suites

**Interfaces:** none; verification only.

- [x] **Step 1: cross-build the app**

```bash
BR=$PWD/output/retroid-pocket-classic
export PKG_CONFIG_SYSROOT_DIR=$BR/staging PKG_CONFIG_LIBDIR=$BR/staging/usr/lib/pkgconfig:$BR/staging/usr/share/pkgconfig
make -B -C app/bananatube CXX=$BR/host/bin/aarch64-linux-g++ PKG_CONFIG=$BR/host/bin/pkg-config \
    CXXFLAGS="-O2" LDFLAGS="" banana-tube
mkdir -p /tmp/opencode/deploy && cp app/bananatube/banana-tube /tmp/opencode/deploy/banana-tube-pocket
```

Expected: no errors/warnings; a ~205 KB aarch64 binary.

- [x] **Step 2: boot QEMU and inject the build**

```bash
KEEP=1 tools/qemu-headless.sh output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz /tmp/opencode/qemu 240
(cd /tmp/opencode/deploy && python3 -m http.server 8801 --bind 127.0.0.1 &)
tools/qemu-console.py /tmp/opencode/qemu/console.sock --timeout 60 \
  'mkdir -p /data/banana/cache/tube /data/music /data/videos && wget -q -O /tmp/banana-tube http://10.0.2.2:8801/banana-tube-pocket && chmod +x /tmp/banana-tube && mount --bind /tmp/banana-tube /usr/bin/banana-tube && echo ready'
```

Expected: `ready`; the menu is still on the panel.

- [x] **Step 3: seed deterministic content and launch**

```bash
tools/qemu-console.py /tmp/opencode/qemu/console.sock --timeout 60 \
  "printf 'dQw4w9WgXcQ\tNever Gonna Give You Up\tRick Astley\t3:33\t42\n' > /data/banana/bananatube-recent.tsv; touch /data/music/Example track.mp3; echo /usr/share/banana/apps/bananatube/BananaTube.app > /run/banana/launch"
```

Wait ~8 s, then capture the keyboard:
`tools/qemu-monitor.py /tmp/opencode/qemu/monitor.sock 'screendump /tmp/opencode/qemu/keyboard.png -f png gpu 0'`.

- [x] **Step 4: capture every screen and inspect**

- Type with the monitor (`sendkey c`, `sendkey a`, `sendkey t`, `sendkey s`) then `sendkey down` x3, `sendkey right` x3,
  `sendkey ret` for the search; capture the results.
- `sendkey tab` cycles Search → Recent → Channels → Local; capture each (Recent uses the seeded row and its resume
  chip; Local the file).
- Check: the gold focus is readable, titles/status truncate inside the boxes, the dot grid/frame renders, the keyboard
  ends above the hints, the tab strip fits beside the quality chip.
- If a nudge is needed (wordmark size, dot spacing, chip height, card inset), change the value, rebuild, redeploy,
  recapture once; stop the VM afterwards (`kill $(cat /tmp/opencode/qemu/qemu.pid)`).

- [x] **Step 5: the real panel**

Deploy the same binary to the unit with the existing bind mount (`cat ... | ssh 'cat > /tmp/... && mount --bind ...'`),
relaunch, and ask for one pass over the keyboard, results, Recent and the player. Measure idle CPU (`top -b -n1`) and
confirm it is still near 0.2 %.

- [x] **Step 6: close**

Update `docs/ROADMAP.md` (the Pocket shell entry; note QEMU screenshots + the unit pass), tick this plan's boxes, run
`make -s test` and `make -s agent-check`, and commit:

```bash
git add docs/ROADMAP.md docs/superpowers/plans/2026-11-04-bananatube-pocket-shell.md
git commit -m "bananatube: the Pocket shell is verified and closed"
```

## Execution notes (2026-11-04)

- QEMU: the 0.5.8 image boots to the menu, but cage/SDL cannot start on the headless virtio GPU
  (`MESA-EGL: failed to create dri2 screen`); the `virtio-gpu-gl` + `egl-headless` path boots the menu but exposes no
  surface for `screendump`. The UI was rendered from the host build under Xvfb at 1240x1080 (keyboard and results) and
  the unit runs the cross-build; the QEMU menu screenshot proves the harness, not the app.
- Unit: bind-mounted at `/usr/bin/banana-tube`, keyboard visible, pad found, idle 4 ticks/20 s (0.2 % of a core).
  Awaiting the one-pass visual confirmation.
- Known minor (deferred): emoji in YouTube titles render as empty boxes (Pixelify/Inter carry no emoji glyphs); the
  previous Inter build showed the same tofu.
