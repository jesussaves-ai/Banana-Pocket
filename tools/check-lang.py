#!/usr/bin/env python3
# The bundled language catalogs (ui/assets/lang), on the host.
#
#   tools/check-lang.py [repo root]     coverage per language, then any problems; nonzero if there were any
#
# A catalog's keys are the English strings the menu passes to tr(); a value is the translation. So:
#   - every key in es/de must exist in en (a typo or a removed string is an error);
#   - every en key must still appear in ui/src (reworded English would otherwise silently fall back forever);
#   - a missing translation is coverage, not a failure (the menu falls back to English);
#   - a catalog must have a "name" and string values, or the row and the loader would show nothing.
import glob, json, os, sys

def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)

def main(argv):
    root = next((a for a in argv[1:] if not a.startswith("-")), os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    langdir = os.path.join(root, "ui", "assets", "lang")
    src = "".join(open(p, encoding="utf-8").read() for p in glob.glob(os.path.join(root, "ui", "src", "*.cpp")))
    files = sorted(glob.glob(os.path.join(langdir, "*.json")))
    if not files:
        print("check-lang: no catalogs in %s" % langdir)
        return 2
    errors, catalogs = [], {}
    for path in files:
        code = os.path.splitext(os.path.basename(path))[0]
        try:
            doc = load(path)
        except (OSError, ValueError) as e:
            errors.append("%s: %s" % (code, e)); continue
        name, strings = doc.get("name"), doc.get("strings")
        if not isinstance(name, str) or not name:
            errors.append("%s: no \"name\"" % code)
        if not isinstance(strings, dict):
            errors.append("%s: no \"strings\" object" % code); continue
        for k, v in strings.items():
            if not isinstance(v, str) or not v.strip():
                errors.append("%s: %r has no text" % (code, k))
        catalogs[code] = (name, strings)
    if "en" not in catalogs:
        print("check-lang: no en.json (the reference list of strings)")
        return 2
    en = catalogs["en"][1]
    for k, v in en.items():
        if v != k:
            errors.append("en: %r maps to %r; en.json must map each key to itself" % (k, v))
        if k not in src and k.replace('"', '\\"') not in src:
            errors.append("en: %r is not in ui/src any more (reworded? update the catalogs)" % k)
    for code, (name, strings) in sorted(catalogs.items()):
        if code == "en":
            continue
        extra = [k for k in strings if k not in en]
        for k in extra:
            errors.append("%s: %r is not an English string in en.json" % (code, k))
        missing = [k for k in en if k not in strings]
        print("  %-3s %-10s %3d/%3d strings%s" % (code, name, len(strings), len(en),
            "" if not missing else "  (%d still English)" % len(missing)))
    for e in errors:
        print("FAIL " + e)
    print("check-lang: %d catalog(s), %d problem(s)" % (len(catalogs), len(errors)))
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
