################################################################################
#
# pocketos (Doors first-party userspace, synced from the Doors repository; the
# package keeps its PocketOS-era name, ADR-005 Phase 4)
#
# Two build steps: the GNU make tree (doors with its pos alias, radiod with the
# sx1262 backend, meshcored, pos-hwcheck, pos-supervise) and the CMake shell
# against the vendor LVGL package in staging.
#
# meshcored is built and installed here (ENABLE_MESHCORED=1) since its
# third-party notices landed (docs/LICENSING.md item 9). The MeshCore and
# Crypto sources it compiles are exported by apply_to_sdk.sh into
# third_party/, pin-checked. Installing it starts nothing: S65meshcored ships
# disabled and each unit switches it on in /etc/default/meshcored.
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
POCKETOS_LICENSE = Not yet decided (Doors; no licence granted), MIT (RadioLib, ggwave, Reed-Solomon, MeshCore, Arduino Cryptography Library), Zlib (Ed25519, in MeshCore), Ooura FFT licence (ggwave FFT), OFL-1.1 (IBM Plex font bitmaps)
POCKETOS_LICENSE_FILES = THIRD_PARTY_NOTICES.txt
POCKETOS_REDISTRIBUTE = NO
POCKETOS_INSTALL_TARGET = YES
# host-python3: the shell's CMake converts the PocketTimber sprites to LVGL
# image arrays at configure time (docs/design/timber-art/tools/png2lvgl.py,
# exported into the package by apply_to_sdk.sh). Buildroot's own python3 in
# $(HOST_DIR)/bin, first on the PATH of every package build, is the one it
# finds, so the image does not depend on the build host's python.
# alsa-lib: pos-wave, Wave's audio helper (docs/apps/WAVE.md), and pos-record,
# the Recorder's (docs/apps/RECORDER.md). It was already in the image
# (alsa-utils), so this adds a build dependency, not a package.
# jpeg: pos-camera, Camera's helper, writes photos as JPEG with libjpeg
# (docs/apps/CAMERA.md). Like alsa-lib it was already in the image (the vendor
# camera tools use it), so this adds a build dependency, not a package.
# libcurl: pos-zabbix, the Zabbix viewer's helper (docs/apps/ZABBIX.md, ADR-007
# ACCEPTED; the shell includes the app by default), reaches the server with libcurl and its OpenSSL backend. Like
# alsa-lib and jpeg it was already in the image (BR2_PACKAGE_LIBCURL, with the
# curl tool and ca-certificates), so this adds a build dependency, not a
# package.
# libpng: pos-browser, the Browser's helper (docs/apps/BROWSER.md, ADR-009
# accepted), decodes a page's PNG pictures with it, and its JPEG ones with the
# jpeg above; it fetches with the libcurl above. libpng was already in the
# image and its sysroot (with headers, for OpenCV; docs/hardware/
# CAMERA_PLATFORM_RESEARCH.md), so this adds a build dependency, not a
# package.
POCKETOS_DEPENDENCIES = cjson libgpiod2 lvgl libdrm libevdev alsa-lib jpeg libcurl libpng host-cmake host-python3

POCKETOS_SHELL_BUILD_DIR = $(@D)/ui/shell/build-k230

define POCKETOS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 POCKETCAM_JPEG=1 ZABBIX_CURL=1 BROWSER_CURL=1 BROWSER_IMAGES=1 ENABLE_MESHCORED=1 -C $(@D) all
	mkdir -p $(POCKETOS_SHELL_BUILD_DIR)
	cd $(POCKETOS_SHELL_BUILD_DIR) && $(TARGET_MAKE_ENV) $(BR2_CMAKE) $(@D)/ui/shell \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_BUILD_TYPE=Release \
		-DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot
	$(TARGET_MAKE_ENV) $(MAKE) -C $(POCKETOS_SHELL_BUILD_DIR)
endef

# The shell ships as doors-shell (ADR-005 Phase 3). The build artefact keeps
# its CMake name - that is internal - and the identity is the installed path,
# the init script and the supervisor's name. The two removals are what keeps a
# rebuilt target tree from carrying both services at once: Buildroot never
# deletes from $(TARGET_DIR) on its own, so a tree that once held the
# PocketOS-era shell would otherwise still hold it, and the rootfs gate in
# build_image.sh would refuse the image (correctly, but late).
define POCKETOS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 POCKETCAM_JPEG=1 ZABBIX_CURL=1 BROWSER_CURL=1 BROWSER_IMAGES=1 ENABLE_MESHCORED=1 -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(POCKETOS_SHELL_BUILD_DIR)/pocketos-shell $(TARGET_DIR)/usr/bin/doors-shell
	rm -f $(TARGET_DIR)/usr/bin/pocketos-shell
	rm -f $(TARGET_DIR)/etc/init.d/S90pocketos-shell
endef

$(eval $(generic-package))
