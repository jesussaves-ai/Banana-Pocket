################################################################################
#
# retroarch-slang-shaders
#
################################################################################

# libretro's Slang shaders and presets (the kind RetroArch's Vulkan driver runs, and the only kind it can: the GLSL
# set is for the "gl" driver). In the image so Shaders > Load Preset isn't empty (RetroArch's video_shader_dir points
# here) and so Settings > Other systems > Shader has presets this driver can load. The GLSL package this replaces is
# gone: Banana runs the games on Vulkan, and its shader backend is Slang.
# `bezel/` (Mega Bezel and friends) is left out: 35 MB of textures for a look the handheld's one screen doesn't need.
RETROARCH_SLANG_SHADERS_VERSION = 2b0530bac0e8083d9d643c194929dc0a6b69bd35
RETROARCH_SLANG_SHADERS_SITE = $(call github,libretro,slang-shaders,$(RETROARCH_SLANG_SHADERS_VERSION))
RETROARCH_SLANG_SHADERS_LICENSE = various (per shader, mostly GPL-2.0+/public domain)

define RETROARCH_SLANG_SHADERS_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/usr/share/libretro/shaders/slang
	cd $(@D) && cp -a $$(ls -A | grep -vxE 'Makefile|\.git.*|configure|README.*|bezel') $(TARGET_DIR)/usr/share/libretro/shaders/slang/
endef

$(eval $(generic-package))
