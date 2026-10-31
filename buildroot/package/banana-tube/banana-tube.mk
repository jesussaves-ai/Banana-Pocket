################################################################################
#
# banana-tube
#
################################################################################

# BananaTube, the YouTube client: its source is a local folder in this tree, like banana-ui and bananad. The app's
# marker and manifest (app.json, BananaTube.app) install with the binary, so banana-ui's Apps shelf sees the app.
BANANA_TUBE_VERSION = local
BANANA_TUBE_SITE = $(BR2_EXTERNAL_BANANA_PATH)/../app/bananatube
BANANA_TUBE_SITE_METHOD = local
BANANA_TUBE_LICENSE = MIT
BANANA_TUBE_LICENSE_FILES = ../LICENSE
BANANA_TUBE_DEPENDENCIES = sdl2 sdl2_ttf sdl2_image libcurl

define BANANA_TUBE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CXX="$(TARGET_CXX)" PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -O2" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define BANANA_TUBE_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
