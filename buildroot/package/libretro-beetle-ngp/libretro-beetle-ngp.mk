################################################################################
#
# libretro-beetle-ngp
#
################################################################################

# Neo Geo Pocket / Neo Geo Pocket Color: Beetle NeoPop, a port of Mednafen's NGP core. The core's soname is
# mednafen_ngp. A software core; a stick, two buttons and a menu button is the whole handheld. Built through the
# repo's own Makefile (there is no Makefile.libretro), platform=unix.
LIBRETRO_BEETLE_NGP_VERSION = a50d5ac288a81f2104ddf43195a4efdd15c72227
LIBRETRO_BEETLE_NGP_SITE = $(call github,libretro,beetle-ngp-libretro,$(LIBRETRO_BEETLE_NGP_VERSION))
LIBRETRO_BEETLE_NGP_LICENSE = GPL-2.0+
LIBRETRO_BEETLE_NGP_LICENSE_FILES = COPYING

define LIBRETRO_BEETLE_NGP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_BEETLE_NGP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mednafen_ngp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_ngp_libretro.so
endef

$(eval $(generic-package))
