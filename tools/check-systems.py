#!/usr/bin/env python3
# Static checks for the systems Banana ships, on the host and from the tree (no device, no buildroot run).
#
# systems.conf names a `run` command per system; the command is a launcher in system/rootfs/usr/libexec/banana,
# and run-retroarch's first argument names a libretro core that a buildroot package builds. A removed launcher, a
# typo'd core, a core with no package, or a `run` line with no %ROM% is a system whose games will not start -- and
# that is only ever discovered on the handheld. This walks those links the way the image would resolve them, so a
# change to systems.conf, the launchers or the cores is caught by `make test` (and CI) instead.
#
#   tools/check-systems.py [repo root]
#
# Prints one line per system it looks at, then any problems; exits nonzero if there were any. A missing explicit
# icon/accent in the theme's systems.cfg is only a note (the theme has a `*` fallback, so every system still gets
# one), so it does not fail the check.
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, ".."))

SYSTEMS = os.path.join(ROOT, "system", "rootfs", "etc", "banana", "systems.conf")
ROOTFS = os.path.join(ROOT, "system", "rootfs")
PKG = os.path.join(ROOT, "buildroot", "package")
CORES_IN = os.path.join(PKG, "retroarch", "cores.in")
LOOKS = os.path.join(ROOT, "ui", "assets", "rnds", "systems.cfg")
DEFAULTS = os.path.join(ROOT, "ui", "src", "settings_defaults.cpp")

errors = []
notes = []
CORE_PKG = {}


def parse_systems(path):
    # [id] sections of key = value, comments and blanks ignored -- what Library::loadDefs reads
    out = []
    cur = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^\[(.+)\]$", line)
            if m:
                cur = {"id": m.group(1), "keys": {}}
                out.append(cur)
            elif cur is not None and "=" in line:
                k, v = line.split("=", 1)
                cur["keys"][k.strip()] = v.strip()
    return out


def looks_map(path):
    # every explicit mapping line: the comma-separated ES names -> the icon file beside the theme (last line wins,
    # the way the theme's loader reads it). Kind/'@maker'/'*' fallback lines are not a system's own line.
    m = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            toks = s.split()
            if len(toks) < 2 or not toks[1].endswith(".png"):
                continue
            for n in toks[0].split(","):
                if n:
                    m[n] = toks[1]
    return m


def core_packages():
    # map <core>_libretro.so -> the package that installs it, over every libretro-* package. Keyed on the installed
    # soname, not the package name: a core's repo/package and its core name disagree (mednafen_pce_fast is built by
    # libretro-beetle-pce-fast), and only the install line is authoritative.
    m = {}
    for name in sorted(os.listdir(PKG)):
        d = os.path.join(PKG, name)
        if not name.startswith("libretro-") or not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith(".mk"):
                continue
            with open(os.path.join(d, f), encoding="utf-8") as fh:
                for core in re.findall(r"/([a-z0-9_]+)_libretro\.so", fh.read()):
                    m[core] = name
    return m


def check_core(core, where):
    package = CORE_PKG.get(core)
    if not package:
        errors.append("%s: no buildroot package installs %s_libretro.so" % (where, core))
        return
    symbol = "BR2_PACKAGE_" + package.replace("-", "_").upper()
    with open(CORES_IN, encoding="utf-8") as f:
        if not re.search(r"^config\s+%s\b" % re.escape(symbol), f.read(), re.M):
            errors.append("%s: core %s (%s) is not enabled in cores.in (%s)" % (where, core, package, symbol))


def main():
    global CORE_PKG
    CORE_PKG = core_packages()
    systems = parse_systems(SYSTEMS)
    looks = looks_map(LOOKS)
    icons = os.path.join(os.path.dirname(LOOKS), "icons")
    for s in systems:
        sid, keys = s["id"], s["keys"]
        run = keys.get("run")
        if not run:
            errors.append("%s: no run command" % sid)
            continue
        if "%ROM%" not in run:
            errors.append("%s: run has no %%ROM%%: %s" % (sid, run))
        parts = run.split()
        prog = parts[0]
        if not prog.startswith("/"):
            errors.append("%s: run is not an absolute launcher: %s" % (sid, run))
            continue
        fs = os.path.join(ROOTFS, prog.lstrip("/"))
        if not (os.path.isfile(fs) and os.access(fs, os.X_OK)):
            errors.append("%s: launcher missing or not executable: %s" % (sid, prog))
            continue
        # run-retroarch <core> %ROM%: the core the image must build
        if os.path.basename(prog) == "run-retroarch":
            core = next((p for p in parts[1:] if p != "%ROM%"), None)
            if not core:
                errors.append("%s: run-retroarch without a core: %s" % (sid, run))
            else:
                check_core(core, sid)
        if sid not in looks:
            notes.append("%s: no explicit line in theme systems.cfg (the * fallback gives it an icon)" % sid)
        elif not os.path.isfile(os.path.join(icons, looks[sid])):
            errors.append("%s: theme icon missing: %s" % (sid, looks[sid]))
        print("  %-12s %s" % (sid, run))

    # Banana's own apps, shipped in this tree (app/<id>/app.json): banana-ui's registry scans the installed folder
    # (/usr/share/banana/apps), so an app is not in systems.conf; check its manifest names a launcher that ships.
    # The manifest allows // comments (banana-ui's JSON reader), so the two fields are read with a regex, not json.
    apps_dir = os.path.join(ROOT, "app")
    if os.path.isdir(apps_dir):
        for app in sorted(os.listdir(apps_dir)):
            mf = os.path.join(apps_dir, app, "app.json")
            if not os.path.isfile(mf):
                continue
            with open(mf, encoding="utf-8") as f:
                text = f.read()
            m = re.search(r'"run"\s*:\s*"([^"]*)"', text)
            if not m:
                errors.append("app %s: no run in app.json" % app)
                continue
            run = m.group(1)
            if "%ROM%" not in run:
                errors.append("app %s: run has no %%ROM%%: %s" % (app, run))
            prog = run.split()[0]
            fs = os.path.join(ROOTFS, prog.lstrip("/"))
            if not (os.path.isfile(fs) and os.access(fs, os.X_OK)):
                errors.append("app %s: launcher missing or not executable: %s" % (app, prog))
                continue
            print("  %-12s %s" % (app, run))

    # a core a Settings row offers as a default must exist too (core.<system> in settings_defaults.cpp)
    with open(DEFAULTS, encoding="utf-8") as f:
        for m in re.finditer(r'\{\s*"core\.([a-z0-9]+)",\s*"([a-z0-9_]+)"', f.read()):
            check_core(m.group(2), "settings core.%s" % m.group(1))

    for n in notes:
        print("  note: " + n)
    for e in errors:
        print("  FAIL: " + e)
    if errors:
        print("check-systems: %d problem(s)" % len(errors))
        return 1
    print("check-systems: %d systems, launchers and cores consistent" % len(systems))
    return 0


if __name__ == "__main__":
    sys.exit(main())
