################################################################################
#
# libretro-beetle-wswan
#
################################################################################

# WonderSwan / WonderSwan Color: Beetle WonderSwan, a port of Mednafen's WS core. The soname is mednafen_wswan. A
# software core; the WonderSwan's pad is a d-pad rotated 45 degrees, which the core maps from the RetroPad's own.
# Built through the repo's own Makefile (there is no Makefile.libretro), platform=unix.
LIBRETRO_BEETLE_WSWAN_VERSION = 4b01295838ea89e3f1355bbe4cb5cf98aa6108cd
LIBRETRO_BEETLE_WSWAN_SITE = $(call github,libretro,beetle-wswan-libretro,$(LIBRETRO_BEETLE_WSWAN_VERSION))
LIBRETRO_BEETLE_WSWAN_LICENSE = GPL-2.0+
LIBRETRO_BEETLE_WSWAN_LICENSE_FILES = COPYING

define LIBRETRO_BEETLE_WSWAN_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_BEETLE_WSWAN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mednafen_wswan_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_wswan_libretro.so
endef

$(eval $(generic-package))
