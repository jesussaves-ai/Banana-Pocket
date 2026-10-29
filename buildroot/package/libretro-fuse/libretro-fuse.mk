################################################################################
#
# libretro-fuse
#
################################################################################

# The ZX Spectrum (and its clones): Fuse, the Free Unix Spectrum Emulator, ported
# to libretro. A software core, the lightest of the computers, so it runs on the
# A55 cluster alone. Its own Makefile.libretro builds a fuse_libretro.so for
# platform=unix; no TARGET_AS, the core has no assembly.
#
# The GitHub tarball, as the other Makefile.libretro cores here: the Makefile
# generates src/version.c with `git rev-parse HEAD`, but Buildroot extracts the
# source so there is no .git either way and the version string just comes out
# empty. No submodules to fetch.
LIBRETRO_FUSE_VERSION = e997e2bc32c888348f862f69f2c53babfedf7791
LIBRETRO_FUSE_SITE = $(call github,libretro,fuse-libretro,$(LIBRETRO_FUSE_VERSION))
LIBRETRO_FUSE_LICENSE = GPL-3.0
LIBRETRO_FUSE_LICENSE_FILES = LICENSE

# zlib and bzip2 are vendored in the tree (zlib/ and bzip2/): the core links its
# own, so there is nothing to depend on. libspectrum reads the tape and disk
# images through libretro-common's VFS, so a .zip of a game loads too.
define LIBRETRO_FUSE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_FUSE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/fuse_libretro.so $(TARGET_DIR)/usr/lib/libretro/fuse_libretro.so
endef

$(eval $(generic-package))
