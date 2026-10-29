################################################################################
#
# libretro-vice
#
################################################################################

# Commodore 64: VICE (the libretro port). One package builds the machine Banana uses, x64 (EMUTYPE); the repo can
# also build the 128, VIC-20, Plus/4 and PET machines (PLUS4=1 / etc.) but the image ships only the C64. A software
# core; the RetroPad drives the joystick, and RetroArch's keyboard (SELECT) covers the games that want a typed
# command. Built through the repo's own Makefile (there is no Makefile.libretro), platform=unix.
LIBRETRO_VICE_VERSION = 6cbc44add8b0a03791b343712dc63dc1edad2675
LIBRETRO_VICE_SITE = $(call github,libretro,vice-libretro,$(LIBRETRO_VICE_VERSION))
LIBRETRO_VICE_LICENSE = GPL-2.0+
LIBRETRO_VICE_LICENSE_FILES = COPYING

define LIBRETRO_VICE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile EMUTYPE=x64 platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_VICE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/vice_x64_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/vice_x64_libretro.so
endef

$(eval $(generic-package))
