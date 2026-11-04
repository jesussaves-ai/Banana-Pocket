# Banana: a light Linux for the Retroid Pocket Classic (Qualcomm SM4450).
#
#   make retroid-pocket-classic   the SD card image   -> output/retroid-pocket-classic/images/banana-retroid-pocket-classic.img.gz
#   make ui                       banana-ui for this computer (headless renders, no panel needed)
#   make test                     checks systems.conf/launchers/cores and runs the launcher, thermals, sound-config
#                                 and BananaTube tests, then renders the menu headless from a test library and checks
#                                 it (needs ImageMagick)
#   make sound-test               the sound chain on the handheld (server, sinks, amp, DSP graph, a client, the menu's
#                                 handover); ARGS=--listen also plays the cue. BANANA_HOST says where it is
#   make bench                    a host measurement baseline (scan/start-up/frame cost) at both canvas sizes
#   make local-rebuild            rebuild the in-tree packages (banana-ui, bananad) before a board build
#   make menuconfig-retroid-pocket-classic / linux-menuconfig-retroid-pocket-classic ...   any Buildroot target
#
# The first image build downloads Buildroot (below) and everything it builds: expect an hour or more.
#
# ui/ and system/bananad/ are Buildroot 'local' packages (built from this tree). Buildroot does not notice a source
# change in them, so a plain `make retroid-pocket-classic` after editing them ships the old binary. The board
# target below rebuilds them when any source in ui/src, ui/rnds or system/bananad is newer than the built binary,
# so a source edit is never shipped in the old binary. `make local-rebuild` does the same by hand.
BUILDROOT_VERSION = 2026.02.3
BUILDROOT_DIR ?= $(CURDIR)/.buildroot
OUT ?= $(CURDIR)/output
BOARDS = retroid-pocket-classic
LOCAL_PKGS = banana-ui-rebuild bananad-rebuild banana-tube-rebuild
# the in-tree packages' sources: the board target compares these against the built binaries to decide whether to
# rebuild. `find` is run once, at parse time, so the list is current. app/bananatube is banana-tube's site.
LOCAL_SRC = $(shell find ui/src ui/rnds system/bananad app/bananatube -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) 2>/dev/null) $(shell find ui/assets -type f 2>/dev/null)

$(BUILDROOT_DIR)/Makefile:
	git clone --depth 1 --branch $(BUILDROOT_VERSION) https://github.com/buildroot/buildroot $(BUILDROOT_DIR)

# Buildroot itself is downloaded, not vendored, so the few changes Banana makes to Buildroot (rather than to a
# package's sources -- those live in buildroot/patches/) sit in buildroot/buildroot-patches/ and are applied here.
# Idempotent: a patch already in place is left alone, so editing a base file and re-running is safe.
BUILDROOT_PATCHES = $(wildcard $(CURDIR)/buildroot/buildroot-patches/*.patch)
$(BUILDROOT_DIR)/.banana-patched: $(BUILDROOT_DIR)/Makefile $(BUILDROOT_PATCHES)
	@for p in $(BUILDROOT_PATCHES); do \
		if git -C $(BUILDROOT_DIR) apply --reverse --check "$$p" 2>/dev/null; then \
			echo "buildroot patch already applied: $$(basename $$p)"; \
		else \
			echo "applying buildroot patch: $$(basename $$p)"; \
			git -C $(BUILDROOT_DIR) apply "$$p" || exit 1; \
		fi; \
	done
	@touch $@

# A readable alias for the patch step: the stamp's own name is a dot path ($(BUILDROOT_DIR) is absolute), which the
# CI's core job cannot spell. `make buildroot-patched` is the same target.
.PHONY: buildroot-patched
buildroot-patched: $(BUILDROOT_DIR)/.banana-patched

define board
$(OUT)/$(1)/.config: $(BUILDROOT_DIR)/.banana-patched buildroot/configs/banana_$(subst -,_,$(1))_defconfig
	$(MAKE) -C $(BUILDROOT_DIR) O=$(OUT)/$(1) BR2_EXTERNAL=$(CURDIR)/buildroot banana_$(subst -,_,$(1))_defconfig

# a stamp that is only rebuilt when an in-tree source changes. Its recipe rebuilds the packages (banana-ui,
# bananad) and touches itself, so the board target below only rebuilds them when something actually changed. On a
# tree where the image was never built the binaries do not exist yet and the board target builds them for the first
# time (the recipe runs regardless, since the stamp has no prerequisites there).
ifneq ($(wildcard $(OUT)/$(1)/target/usr/bin/banana-ui),)
$(OUT)/$(1)/.local-stamp: $(LOCAL_SRC)
	$(MAKE) -C $(OUT)/$(1) $(LOCAL_PKGS)
	@mkdir -p $(OUT)/$(1)/
	@touch $(OUT)/$(1)/.local-stamp
else
$(OUT)/$(1)/.local-stamp:
	@mkdir -p $(OUT)/$(1)/
	@touch $(OUT)/$(1)/.local-stamp
endif

# the image: the stamp rebuilds the in-tree packages when their sources are newer than the built binaries, so a
# board build never ships a stale banana-ui/bananad (Buildroot's 'local' packages are not tracked by its own make).
$(1): $(OUT)/$(1)/.config $(OUT)/$(1)/.local-stamp
	$(MAKE) -C $(OUT)/$(1)

%-$(1): $(OUT)/$(1)/.config
	$(MAKE) -C $(OUT)/$(1) $$*
endef
$(foreach b,$(BOARDS),$(eval $(call board,$(b))))

ui:
	$(MAKE) -C ui host

test: ui
	tools/check-systems.py
	tools/check-themes.py --selftest
	tools/check-themes.py
	system/tests/run-retroarch-test.sh
	system/tests/rpc-thermals-test.sh
	system/tests/run-banana-tube-test.sh
	system/tests/run-backup-test.sh
	system/tests/sound-config-test.sh
	ui/tests/run.sh

# The sound chain on the handheld, end to end: the server, ALSA's default, the sink, the amp's gain, the AudioReach
# graph, a real client, and the menu's sleep handover. The script is streamed (like tools/rpc-push-update.sh), so it
# also checks an image from before it. BANANA_HOST (a make variable, so the environment works) is the handheld;
# ARGS=--listen plays the cue too.
BANANA_HOST ?= root@169.254.170.2
sound-test:
	cat system/rootfs/usr/libexec/banana/sound-test | \
	    ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 $(BANANA_HOST) "sh -s -- $(ARGS)"

# a host-only measurement baseline (the library scan, the menu's start-up and the frame cost) at both canvas
# sizes; the numbers tell a regression from a change. See ui/tests/bench.sh.
bench: ui
	ui/tests/bench.sh

# The in-tree packages are 'local': rebuild them so a board build picks up edits to ui/ or system/bananad/
# (they must have been built once already; `make retroid-pocket-classic` does that on a fresh tree).
local-rebuild:
	$(MAKE) -C $(OUT)/retroid-pocket-classic $(LOCAL_PKGS)

# the image's banana-ui must match a fresh host build. The CI's `checks` job asserts this hash; on a tree where the
# image was never built it asks you to run `make retroid-pocket-classic` first.
# the image's banana-ui must be newer than every in-tree source: a source edited after the image was built is the
# stale-image footgun the stamp guard in the board target prevents, but this catches a tree where that step was
# skipped or a build was interrupted. (The host binary is x86-64 and the image's is aarch64, so hashing them, as an
# earlier version of this did, can never match.)
check-local-build:
	@img=$(OUT)/retroid-pocket-classic/target/usr/bin/banana-ui; \
	if [ ! -f "$$img" ]; then echo "no image built yet: run make retroid-pocket-classic first"; exit 1; fi; \
	newer=$$(find ui/src ui/rnds system/bananad -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) -newer "$$img" 2>/dev/null | head -n 1); \
	if [ -n "$$newer" ]; then echo "stale image: $$newer is newer than the image's banana-ui -- run make retroid-pocket-classic"; exit 1; fi; \
	echo "the image's banana-ui is newer than every source"

# Quiet health check for agent sessions: syntax + host compile + the fast suite. <=6 lines on success,
# tail of the failing log otherwise, so a session spends no tokens reading build noise.
agent-check:
	@for f in system/rootfs/etc/init.d/* system/rootfs/usr/bin/* system/rootfs/usr/libexec/banana/* \
	          system/rootfs/etc/banana/sleep.d/* system/tests/*.sh system/bananad/tests/*.sh \
	          buildroot/board/retroid-pocket-classic/*.sh tools/*.sh; do \
	    sh -n "$$f" || { echo "FAIL sh -n $$f"; exit 1; }; done; echo "sh: OK"
	@$(MAKE) -s -C ui host >/dev/null 2>/tmp/agent-ui.log || { echo "FAIL ui build"; tail -n 5 /tmp/agent-ui.log; exit 1; }; echo "ui: OK"
	@if [ -f /usr/include/alsa/asoundlib.h ]; then \
	    gcc -std=c11 -Wall -Wextra -Werror -fsyntax-only system/bananad/bananad.c || exit 1; echo "bananad: OK"; \
	 else echo "bananad: skipped (no host alsa)"; fi
	@ui/tests/run.sh >/tmp/agent-test.log 2>&1 && echo "tests: OK" || { echo "FAIL tests"; tail -n 20 /tmp/agent-test.log; exit 1; }

# Regenerate the Source Map block in AGENTS.md: file:line: symbol for the core files. Line numbers drift
# after edits; paste the fresh ranges over the snapshot in AGENTS.md.
agent-map:
	@for f in ui/src/main.cpp ui/src/kms.cpp ui/src/soft.cpp ui/src/sound.cpp ui/src/library.cpp ui/src/settings.cpp system/bananad/bananad.c; do \
	    echo "== $$f"; \
	    rg -n --no-heading '^\s*(static\s+)?[A-Za-z_][A-Za-z0-9_:<>,\*\& ]*\s+[A-Za-z_][A-Za-z0-9_]*(\s*::\s*[A-Za-z_~][A-Za-z0-9_]*)?\s*\([^;]*$$' "$$f"; \
	done

clean:
	rm -rf $(OUT)
	$(MAKE) -C ui clean

.PHONY: $(BOARDS) ui test sound-test bench local-rebuild check-local-build agent-check agent-map clean
