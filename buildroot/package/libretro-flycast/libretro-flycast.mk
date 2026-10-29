################################################################################
#
# libretro-flycast
#
################################################################################

# Sega Dreamcast (and Naomi/Atomiswave): Flycast. Built through its own CMake (it has no Makefile.libretro), the way
# ROCKNIX builds it (projects/ROCKNIX/packages/emulators/libretro/flycast-lr), at ROCKNIX's pinned commit.
# GLES3, not Vulkan: the core's Vulkan renderer needs glslang linked in, and on this handheld the GLES path through
# the Adreno (freedreno) is the one RetroArch already hands the core. The GL entry points come from the frontend
# (retro_hw_get_proc_address), so nothing links -lGLESv2.
LIBRETRO_FLYCAST_VERSION = c3763d8fc4208dd6f8f0bc456383543b8406a8a0
# git, not the GitHub tarball: Flycast's CMake builds several bundled deps from git submodules
# (core/deps/tinygettext, libchdr and its zstd, vixl, xxHash, glm, asio...), which an archive of the commit does not
# carry. Buildroot fetches them with GIT_SUBMODULES (git method only).
LIBRETRO_FLYCAST_SITE = https://github.com/flyinghead/flycast.git
LIBRETRO_FLYCAST_SITE_METHOD = git
LIBRETRO_FLYCAST_GIT_SUBMODULES = YES
LIBRETRO_FLYCAST_LICENSE = GPL-2.0+
LIBRETRO_FLYCAST_LICENSE_FILES = LICENSE
LIBRETRO_FLYCAST_DEPENDENCIES = zlib libzip
# GLES3 *and* Vulkan: with Vulkan compiled in, Flycast asks the frontend for a Vulkan context (SET_HW_RENDER,
# VK_API_VERSION_1_0 -- what Turnip reports) and runs on it, which is the driver RetroArch runs on this handheld, so
# its shaders stay Slang. Without it Flycast instead asks the frontend what it has (GET_PREFERRED_HW_RENDER), gets
# "vulkan", and then cannot use it: "Failed to load content". The Vulkan path needs a glslang to compile its SPIR-V;
# glslang comes from Flycast's own submodule (USE_HOST_GLSLANG=OFF -- the image has no system glslang).
LIBRETRO_FLYCAST_CONF_OPTS = \
	-DLIBRETRO=ON \
	-DWITH_SYSTEM_ZLIB=ON \
	-DUSE_OPENMP=ON \
	-DUSE_OPENGL=ON \
	-DUSE_GLES=ON \
	-DUSE_GLES2=OFF \
	-DUSE_VULKAN=ON \
	-DUSE_HOST_GLSLANG=OFF \
	-Wno-dev

# Its libretro option names are "flycast_*" (the core is Flycast, not its old name reicast): the core reads the
# per-core option file Banana writes. ROCKNIX renames them the same way.
define LIBRETRO_FLYCAST_FIX_OPTION_NAMES
	if [ -f $(@D)/shell/libretro/libretro_core_option_defines.h ]; then \
		$(SED) 's/"reicast"/"flycast"/g' $(@D)/shell/libretro/libretro_core_option_defines.h; \
	fi
endef
LIBRETRO_FLYCAST_POST_PATCH_HOOKS += LIBRETRO_FLYCAST_FIX_OPTION_NAMES

define LIBRETRO_FLYCAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/flycast_libretro.so $(TARGET_DIR)/usr/lib/libretro/flycast_libretro.so
	# Flycast's CMake builds several of its dependencies from its own submodules as shared libraries and links the
	# core against them, but installs only the core: without these the core cannot load ("cannot open shared object
	# file"). None are the image's own packages -- the bundled libzip's soname is "zip.so.5" (the system one is
	# "libzip.so.5"), tinygettext needs its own nowide, and glslang (the Vulkan renderer's SPIR-V compiler) has no
	# system package here at all.
	$(INSTALL) -D -m 0755 $(@D)/core/deps/nowide/nowide.so.11.3.0 $(TARGET_DIR)/usr/lib/nowide.so.11.3.0
	$(INSTALL) -D -m 0755 $(@D)/core/deps/libzip/lib/zip.so.5.5 $(TARGET_DIR)/usr/lib/zip.so.5.5
	ln -sf zip.so.5.5 $(TARGET_DIR)/usr/lib/zip.so.5
	$(INSTALL) -D -m 0755 $(@D)/core/deps/glslang/glslang/glslang.so.15.3.0 $(TARGET_DIR)/usr/lib/glslang.so.15.3.0
	ln -sf glslang.so.15.3.0 $(TARGET_DIR)/usr/lib/glslang.so.15
	$(INSTALL) -D -m 0755 $(@D)/core/deps/glslang/SPIRV/SPIRV.so.15.3.0 $(TARGET_DIR)/usr/lib/SPIRV.so.15.3.0
	ln -sf SPIRV.so.15.3.0 $(TARGET_DIR)/usr/lib/SPIRV.so.15
	$(INSTALL) -D -m 0755 $(@D)/core/deps/glslang/glslang/glslang-default-resource-limits.so.15.3.0 $(TARGET_DIR)/usr/lib/glslang-default-resource-limits.so.15.3.0
	ln -sf glslang-default-resource-limits.so.15.3.0 $(TARGET_DIR)/usr/lib/glslang-default-resource-limits.so.15
endef

$(eval $(cmake-package))
