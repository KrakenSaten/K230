################################################################################
#
# pocketos (first-party userspace, synced from the PocketOS repository)
#
# Two build steps: the GNU make tree (pos, radiod with the sx1262 backend,
# pos-hwcheck, pos-supervise) and the CMake shell against the vendor LVGL
# package in staging.
#
################################################################################

POCKETOS_VERSION = $(shell cat $(realpath $(TOPDIR))/package/pocketos/src/VERSION 2>/dev/null || echo unknown)
POCKETOS_SITE = $(realpath $(TOPDIR))/package/pocketos/src
POCKETOS_SITE_METHOD = local
# No licence is chosen yet for Doors' own code (PocketOS through v0.0.9): none
# is granted and the package is not redistributable (docs/LICENSING.md). What
# it contains from others is listed, and reproduced in full, in
# THIRD_PARTY_NOTICES.txt, which legal-info collects (checked against
# pocketos.hash) and the image installs as
# /usr/share/doors/THIRD_PARTY_NOTICES.txt, with a link at the old
# /usr/share/pocketos path.
POCKETOS_LICENSE = Not yet decided (Doors; no licence granted), MIT (RadioLib, ggwave, Reed-Solomon), Ooura FFT licence (ggwave FFT), OFL-1.1 (IBM Plex font bitmaps)
POCKETOS_LICENSE_FILES = THIRD_PARTY_NOTICES.txt
POCKETOS_REDISTRIBUTE = NO
POCKETOS_INSTALL_TARGET = YES
# host-python3: the shell's CMake converts the PocketTimber sprites to LVGL
# image arrays at configure time (docs/design/timber-art/tools/png2lvgl.py,
# exported into the package by apply_to_sdk.sh). Buildroot's own python3 in
# $(HOST_DIR)/bin, first on the PATH of every package build, is the one it
# finds, so the image does not depend on the build host's python.
# alsa-lib: pos-wave, Wave's audio helper (docs/apps/WAVE.md). It was already
# in the image (alsa-utils), so this adds a build dependency, not a package.
POCKETOS_DEPENDENCIES = cjson libgpiod2 lvgl libdrm libevdev alsa-lib host-cmake host-python3

POCKETOS_SHELL_BUILD_DIR = $(@D)/ui/shell/build-k230

define POCKETOS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 -C $(@D) all
	mkdir -p $(POCKETOS_SHELL_BUILD_DIR)
	cd $(POCKETOS_SHELL_BUILD_DIR) && $(TARGET_MAKE_ENV) $(BR2_CMAKE) $(@D)/ui/shell \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot
	$(TARGET_MAKE_ENV) $(MAKE) -C $(POCKETOS_SHELL_BUILD_DIR)
endef

define POCKETOS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(POCKETOS_SHELL_BUILD_DIR)/pocketos-shell $(TARGET_DIR)/usr/bin/pocketos-shell
endef

$(eval $(generic-package))
