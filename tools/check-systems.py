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


def looks_ids(path):
    # every token on a systems.cfg mapping line (comments skipped): the ES system names, their icon and accent
    ids = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.lstrip().startswith("#"):
                continue
            ids.update(re.findall(r"[a-z0-9][a-z0-9-]*", line))
    return ids


def core_for(package):
    # a libretro package installs <core>_libretro.so; recover <core> from the install line
    mk = os.path.join(PKG, "libretro-" + package, "libretro-" + package + ".mk")
    if not os.path.isfile(mk):
        return None
    with open(mk, encoding="utf-8") as f:
        m = re.search(r"/([a-z0-9_]+)_libretro\.so", f.read())
    return m.group(1) if m else None


def check_core(core, where):
    package = core.replace("_", "-")
    d = os.path.join(PKG, "libretro-" + package)
    if not os.path.isdir(d):
        errors.append("%s: core %s has no buildroot/package/libretro-%s" % (where, core, package))
        return
    got = core_for(package)
    if got != core:
        errors.append("%s: libretro-%s installs %s_libretro.so, not %s_libretro.so" % (where, package, got, core))
    symbol = "BR2_PACKAGE_LIBRETRO_" + core.upper()
    with open(CORES_IN, encoding="utf-8") as f:
        if not re.search(r"^config\s+%s\b" % re.escape(symbol), f.read(), re.M):
            errors.append("%s: core %s is not enabled in cores.in (%s)" % (where, core, symbol))


def main():
    systems = parse_systems(SYSTEMS)
    listed = looks_ids(LOOKS)
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
        if sid not in listed:
            notes.append("%s: no explicit line in theme systems.cfg (the * fallback gives it an icon)" % sid)
        print("  %-12s %s" % (sid, run))

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
