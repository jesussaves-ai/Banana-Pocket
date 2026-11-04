# Languages

The menu has a language setting, currently for the **Settings screen**: English (the original), Spanish and German.
The rest of the menu follows in a later update.

## How it works

The English string is the key. Every text the Settings screen draws goes through `tr()` (`ui/src/i18n.cpp`), which
returns the active language's text or, when there is no catalog or no entry, the English itself. So a missing
translation is never a blank or a crash — it is just English.

Catalogs are JSON files, one per language:

```json
{ "name": "Español", "strings": { "Theme": "Tema", "Volume": "Volumen" } }
```

They live in `/usr/share/banana/lang/<code>.json` in the image. **A copy on the card wins**: put
`/data/banana/lang/es.json` there (File sharing, or a computer) and it is used instead — the same rule as themes, so a
wrong word can be fixed without an update. It is read with the menu's own parser, so `//` comments and trailing commas
are fine, and it must be UTF-8.

`en.json` is the reference list: every key maps to itself. `es.json` and `de.json` translate it; both are **informal**
(Spanish *tú*, German *du*), matching the menu's tone.

## Checking a catalog

`make test` (and CI) runs `tools/check-lang.py`:

- every key in `es`/`de` must exist in `en` (a typo or a removed string fails);
- every key in `en` must still appear in `ui/src` (a reworded English string fails instead of silently falling back);
- `en` maps each key to itself, values are non-empty strings, each catalog has a `name`;
- coverage is printed (`121/121 strings`), so a half-finished translation is visible but not a failure.

The headless menu test also loads the German catalog once and checks the `i18n: de (N strings)` log line.

## Adding a language

Two things: a catalog (`<code>.json`, copied from `en.json` and translated), and the code added to the **Language**
row's list in `ui/src/settings.cpp` (its values are the codes). A card catalog can only override a code the row already
offers; new codes come with an update. Plural forms, right-to-left scripts and a CJK font are not handled yet — Spanish
and German needed none of that.

## What is not translated yet

- The menu's own chrome (home screen, splash, OSD) and the sub-pages of Settings (Network, Update, Setup, States).
- System and app names (`systems.conf`, app manifests) and BananaTube.
- Date and time formats (month names, AM/PM).
