################################################################################
#
# dolphin
#
################################################################################

# The standalone GameCube/Wii emulator, its nogui frontend only. ROCKNIX builds this same source for the SM4450
# (projects/ROCKNIX/packages/emulators/standalone/dolphin-sa) and at this pinned commit, and the patches beside this
# file are ROCKNIX's: the Wayland frontend Dolphin's nogui lacks upstream (the game has to run under cage so the
# panel's orientation is applied), the evdev pad ordering that keeps a profile's button numbers stable, and the small
# default changes a handheld wants. They were rebased onto this commit and re-emitted as git diffs: ROCKNIX applies
# them with the usual fuzz, but Buildroot patches strictly (patch -F0 -g0), and three of the originating hunks
# (GCPadEmu.h, PlatformX11.cpp, GCPadStatus.h, evdev.cpp, CommandProcessor.cpp) only matched with fuzz.
DOLPHIN_VERSION = 6094cfcf7b8fba733b3116fdf3414d51c1c0e4a4
DOLPHIN_SITE = https://github.com/dolphin-emu/dolphin.git
DOLPHIN_SITE_METHOD = git
DOLPHIN_GIT_SUBMODULES = YES
DOLPHIN_LICENSE = GPL-2.0+
DOLPHIN_LICENSE_FILES = COPYING
DOLPHIN_DEPENDENCIES = host-pkgconf alsa-lib bzip2 libcurl libdrm libegl libevdev libpng udev vulkan-loader wayland wayland-protocols xz zlib
DOLPHIN_INSTALL_STAGING = NO

# Out-of-source: Dolphin's bundled zstd submodule (Externals/zstd/zstd/CMakeLists.txt) refuses in-source builds, and
# Buildroot's cmake infra defaults to building in the source directory. Dolphin is normally built out-of-source, so
# this is the ordinary mode; the binary lands in the build dir's Binaries/ (see the install below).
DOLPHIN_SUPPORTS_IN_SOURCE_BUILD = NO

# nogui, no Qt: the frontend the Wii launcher drives. ENABLE_QT stays off (the settings UI would pull Qt6 into the
# image); ENABLE_NOGUI is what builds the game-playing binary. Wayland on (cage), X11 off (a handheld has no X), EGL
# and Vulkan on (the a613 through Mesa's Turnip, and the EGL path for a machine without it). ALSA on, PulseAudio and
# Cubeb off (the image has neither); SDL off (the pad is read straight from evdev, so no SDL3); evdev on. HWDB off
# (the udev hardware database is not wanted, and it is what would otherwise hard-require libudev at configure);
# LLVM/tests/CLI tool/auto-update/analytics/framedumps (ffmpeg) off, the ROCKNIX set for a shipped handheld.
DOLPHIN_CONF_OPTS = \
	-DLIBRETRO=OFF \
	-DENABLE_QT=OFF \
	-DENABLE_NOGUI=ON \
	-DENABLE_HEADLESS=OFF \
	-DENABLE_X11=OFF \
	-DENABLE_WAYLAND=ON \
	-DENABLE_EGL=ON \
	-DENABLE_VULKAN=ON \
	-DENABLE_ALSA=ON \
	-DENABLE_PULSEAUDIO=OFF \
	-DENABLE_CUBEB=OFF \
	-DENABLE_SDL=OFF \
	-DENABLE_EVDEV=ON \
	-DENABLE_HWDB=OFF \
	-DENABLE_LLVM=OFF \
	-DENABLE_TESTS=OFF \
	-DENABLE_CLI_TOOL=OFF \
	-DENABLE_AUTOUPDATE=OFF \
	-DENABLE_ANALYTICS=OFF \
	-DENABLE_LTO=OFF \
	-DENCODE_FRAMEDUMPS=OFF \
	-DUSE_DISCORD_PRESENCE=OFF \
	-DUSE_UPNP=OFF \
	-DUSE_MGBA=OFF \
	-DUSE_RETRO_ACHIEVEMENTS=OFF \
	-DENABLE_GENERIC=OFF \
	-DDISTRIBUTOR="Banana" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5

# -mno-outline-atomics: the toolchain targets cortex_a76_a55 (armv8.2-a), where GCC emits outline-atomic helper calls
# libgcc resolves at link time; ROCKNIX sets the same flag for this SoC.
DOLPHIN_CONF_OPTS += \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -mno-outline-atomics" \
	-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -mno-outline-atomics"

# The bundled dependencies (fmt, glslang, zstd, libusb, hidapi...) link statically into dolphin-emu-nogui, rather
# than becoming shared libraries the image would also have to carry.
DOLPHIN_CONF_OPTS += -DBUILD_SHARED_LIBS=OFF

# just the nogui binary and the Sys tree. Dolphin reads Sys from its compile-time data directory
# (FileUtil.cpp: SYSDATA_DIR = DATA_DIR "Sys", DATA_DIR = /usr/share/dolphin-emu/), which is where the CMake install
# would put it too; the explicit copy keeps the frontend and the data in step with what run-dolphin expects. The
# binary is in the out-of-source build dir's Binaries/ (CMAKE_RUNTIME_OUTPUT_DIRECTORY).
define DOLPHIN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/Binaries/dolphin-emu-nogui $(TARGET_DIR)/usr/bin/dolphin-emu-nogui
	mkdir -p $(TARGET_DIR)/usr/share/dolphin-emu
	cp -r $(@D)/Data/Sys $(TARGET_DIR)/usr/share/dolphin-emu/Sys
endef

$(eval $(cmake-package))
