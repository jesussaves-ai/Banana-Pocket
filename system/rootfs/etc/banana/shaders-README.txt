Shaders are RetroArch-only. RetroArch runs on Vulkan here, so its shader
backend is Slang: use .slangp presets (RetroArch's own Shaders > Load Preset
browses the image's set under /usr/share/libretro/shaders/slang, and one you
choose is kept as config/global.slangp on the card). A .glslp preset cannot be
loaded by the Vulkan driver. Settings > Other systems > Shader is Banana's own
shortcut to three of those presets (an LCD grid, a CRT, sharp pixels).
