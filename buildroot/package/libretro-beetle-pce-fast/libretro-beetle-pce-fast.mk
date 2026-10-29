################################################################################
#
# libretro-beetle-pce-fast
#
################################################################################

# PC Engine / TurboGrafx-16 (and PC Engine CD): Beetle PCE Fast, a lighter port of Mednafen's PCE core. The core's
# name -- the soname RetroArch and systems.conf use -- is mednafen_pce_fast, while the repository is
# beetle-pce-fast-libretro; the two disagree, and only the installed soname matters. Built through the repo's own
# Makefile (there is no Makefile.libretro), platform=unix.
LIBRETRO_BEETLE_PCE_FAST_VERSION = 4eee99c451f85ebe66d1591de88b892015b23059
LIBRETRO_BEETLE_PCE_FAST_SITE = $(call github,libretro,beetle-pce-fast-libretro,$(LIBRETRO_BEETLE_PCE_FAST_VERSION))
LIBRETRO_BEETLE_PCE_FAST_LICENSE = GPL-2.0+
LIBRETRO_BEETLE_PCE_FAST_LICENSE_FILES = COPYING

define LIBRETRO_BEETLE_PCE_FAST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_BEETLE_PCE_FAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mednafen_pce_fast_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_pce_fast_libretro.so
endef

$(eval $(generic-package))
