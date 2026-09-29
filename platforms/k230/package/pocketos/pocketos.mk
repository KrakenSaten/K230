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
POCKETOS_LICENSE = Not yet decided (Doors; no licence granted), MIT (RadioLib, ggwave, Reed-Solomon, MeshCore, Arduino Cryptography Library), Zlib (Ed25519, in MeshCore), Ooura FFT licence (ggwave FFT), OFL-1.1 (IBM Plex font bitmaps), AGPL-3.0 (YOLOv8n model data, Ultralytics; internal images only)
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
# libnncase, libmmz: pos-vision, the Vision app's helper (docs/apps/VISION.md),
# runs its detector on the KPU through the nncase 2.11 runtime and the AI2D
# engine (POCKETVISION_KPU=1), and the runtime's shared pool through libmmz.
# Both were already in the image and its sysroot (BR2_PACKAGE_AI2D_KPU and
# BR2_PACKAGE_FACE_DETECT select them), so this adds a build dependency, not
# a package. The model the helper runs is installed below.
# ffmpeg: pos-mp3, the MP3 app's helper (docs/apps/MP3.md), decodes with
# libavformat, libavcodec, libswresample and libavutil (MP3_FFMPEG=1), and
# pos-video, the Video app's helper (docs/apps/VIDEO.md, ADR-012
# proposed), reads MP4 with libavformat, decodes H.264 on the K230's video
# processing unit through libavcodec's h264_v4l2m2m (the vendor's patched
# FFmpeg 4.4, package/ffmpeg in the SDK overlay), converts with libswscale
# and resamples the sound with libswresample (POCKETVIDEO_FFMPEG=1). FFmpeg
# 4.4.4 was already in the image and its sysroot, with every decoder
# (BR2_PACKAGE_OPENCV4_WITH_FFMPEG selects it; LGPL-2.1+, the build has
# --disable-gpl), so this adds a build
# dependency, not a package.
POCKETOS_DEPENDENCIES = cjson libgpiod2 lvgl libdrm libevdev alsa-lib jpeg libcurl libpng libnncase libmmz ffmpeg host-cmake host-python3

POCKETOS_SHELL_BUILD_DIR = $(@D)/ui/shell/build-k230

define POCKETOS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 POCKETCAM_JPEG=1 ZABBIX_CURL=1 BROWSER_CURL=1 BROWSER_IMAGES=1 POCKETVISION_KPU=1 MP3_FFMPEG=1 POCKETVIDEO_FFMPEG=1 ENABLE_MESHCORED=1 -C $(@D) all
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
#
# The Vision model (docs/apps/VISION.md, "The model") is the pinned SDK's
# yolov8n.kmodel, installed where pos-vision reads it. It is taken from the
# SDK's own copy in package/yolo/utils, which Buildroot's package tree carries
# whether or not the vendor yolo demo is selected (it is not). That way the
# image holds exactly one copy and the Doors repository holds none. The model
# is AGPL-3.0 and is in the image for internal use only (docs/LICENSING.md
# item 10), so the install refuses it without its notice. It also refuses any
# file other than the one tools/vision/yolov8n.kmodel.sha256 pins.
POCKETOS_VISION_MODEL_DIR = $(realpath $(TOPDIR))/package/yolo/utils

define POCKETOS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 POCKETCAM_JPEG=1 ZABBIX_CURL=1 BROWSER_CURL=1 BROWSER_IMAGES=1 POCKETVISION_KPU=1 MP3_FFMPEG=1 POCKETVIDEO_FFMPEG=1 ENABLE_MESHCORED=1 -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(POCKETOS_SHELL_BUILD_DIR)/pocketos-shell $(TARGET_DIR)/usr/bin/doors-shell
	rm -f $(TARGET_DIR)/usr/bin/pocketos-shell
	rm -f $(TARGET_DIR)/etc/init.d/S90pocketos-shell
	grep -q '^yolov8n-kmodel *|' $(@D)/third_party/notices/SOURCES || \
		{ echo "pocketos: the Vision model has no entry in third_party/notices/SOURCES (docs/LICENSING.md item 10)" >&2; exit 1; }
	cd $(POCKETOS_VISION_MODEL_DIR) && sha256sum -c $(@D)/tools/vision/yolov8n.kmodel.sha256
	$(INSTALL) -D -m 0644 $(POCKETOS_VISION_MODEL_DIR)/yolov8n.kmodel $(TARGET_DIR)/usr/share/doors/vision/yolov8n.kmodel
endef

$(eval $(generic-package))
