################################################################################
#
# libretro-handy
#
################################################################################

# Atari Lynx: Handy (the libretro port of the emulator of the same name). A software core; the Lynx is a wide
# handheld with a d-pad and two buttons. It needs the Lynx boot ROM: lynxboot.img in banana/bios (RetroArch's
# system directory). Built through the repo's own Makefile (there is no Makefile.libretro), platform=unix.
LIBRETRO_HANDY_VERSION = bc55d462f0b2d6b073ea93dc552ebd73cec60fd1
LIBRETRO_HANDY_SITE = $(call github,libretro,libretro-handy,$(LIBRETRO_HANDY_VERSION))
LIBRETRO_HANDY_LICENSE = Zlib
LIBRETRO_HANDY_LICENSE_FILES = lynx/license.txt

define LIBRETRO_HANDY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_HANDY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/handy_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/handy_libretro.so
endef

$(eval $(generic-package))
