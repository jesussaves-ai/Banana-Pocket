################################################################################
#
# yt-dlp
#
################################################################################

# The official release zipapp: a single self-contained file (a zip with a `#!/usr/bin/env python3` shebang), so the
# package downloads it and installs it as it is. Buildroot 2026.02 has no yt-dlp package; banana-tube, and mpv's
# --ytdl (mpv shells out to yt-dlp for a YouTube URL), need the binary on PATH.
#
# ffmpeg is selected for the audio+video mux and for a format yt-dlp cannot serve whole; the Python runtime (with
# ssl and zlib) comes from the image. Update by moving YT_DLP_VERSION and refreshing the hash.
YT_DLP_VERSION = 2026.10.26
YT_DLP_SITE = https://github.com/yt-dlp/yt-dlp/releases/download/$(YT_DLP_VERSION)
YT_DLP_SOURCE = yt-dlp
YT_DLP_LICENSE = Unlicense
YT_DLP_DEPENDENCIES = python3

# The source is a zipapp, not an archive to unpack: Buildroot's default extractor has no rule for an extensionless
# file and would try to *run* it (its `INFLATE` for no suffix is empty, leaving ` <file> | tar ... -xf -`). Copy it
# into the build directory as-is instead.
define YT_DLP_EXTRACT_CMDS
	$(INSTALL) -D -m 0755 $(YT_DLP_DL_DIR)/$(YT_DLP_SOURCE) $(@D)/yt-dlp
endef

define YT_DLP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/yt-dlp $(TARGET_DIR)/usr/bin/yt-dlp
endef

$(eval $(generic-package))
