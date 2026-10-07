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
# Doors' own code (PocketOS through v0.0.9) is Apache-2.0 (ADR-013): LICENSE
# and NOTICE. What the package contains from others is listed, and reproduced
# in full, in THIRD_PARTY_NOTICES.txt. legal-info collects all three (checked
# against pocketos.hash) and the image installs them in /usr/share/doors, with
# a link to the notices at the old /usr/share/pocketos path.
# REDISTRIBUTE stays NO, so legal-info does not export this package's source,
# until docs/licensing/APACHE_2_READINESS.md clears the source repository for
# publication: the owner has chosen the licence, not yet published anything.
POCKETOS_LICENSE = Apache-2.0 (Doors), MIT (RadioLib, ggwave, Reed-Solomon, MeshCore, Arduino Cryptography Library), Zlib (Ed25519, in MeshCore), BSD-2-Clause (Canaan K230 SDK code in pos-vision), Ooura FFT licence (ggwave FFT), OFL-1.1 (IBM Plex font bitmaps)
POCKETOS_LICENSE_FILES = LICENSE NOTICE THIRD_PARTY_NOTICES.txt
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
# The pocketos package selects them (Config.in), with gsl-lite, which the
# nncase headers include; up to 0.3.0 the vendor face_detect demo did. The
# runtime's Python wheel, which libnncase also unpacks into the target, is
# not used by anything and is removed (below). The detector model is not
# installed (below).
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
POCKETOS_DEPENDENCIES = cjson libgpiod2 lvgl libdrm libevdev alsa-lib jpeg libcurl libpng libnncase libmmz gsl-lite ffmpeg host-cmake host-python3

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
# No Vision detector model ships (Doors 0.3.5 on; docs/apps/VISION.md "The
# model", MODEL_LICENSES.md). The pinned SDK's yolov8n.kmodel, which images
# up to 0.3.0 carried for internal use, is a compiled Ultralytics YOLOv8n
# (AGPL-3.0); a DOORS-trained replacement is not ready. pos-vision runs
# without it: COLOR, EDGE and LINE TRACE, and the modes whose own models are
# installed by hand; DETECT, TRACK and TRAFFIC are not offered, and the app
# says why. Buildroot never deletes from $(TARGET_DIR), so the install
# removes the copy an earlier build put there, and a final check over the
# whole target refuses any of the SDK's Ultralytics YOLO kmodels - by name,
# and by the hashes in tools/vision/refused-models.sha256 whatever the file
# is called - and the vendor yolo demo package, which would install them.
ifeq ($(BR2_PACKAGE_YOLO),y)
$(error pocketos: BR2_PACKAGE_YOLO installs Ultralytics YOLO models (AGPL-3.0), which no Doors image ships (docs/apps/VISION.md, "The model"))
endif

define POCKETOS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) ENABLE_SX1262=1 POCKETCAM_JPEG=1 ZABBIX_CURL=1 BROWSER_CURL=1 BROWSER_IMAGES=1 POCKETVISION_KPU=1 MP3_FFMPEG=1 POCKETVIDEO_FFMPEG=1 ENABLE_MESHCORED=1 -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(POCKETOS_SHELL_BUILD_DIR)/pocketos-shell $(TARGET_DIR)/usr/bin/doors-shell
	rm -f $(TARGET_DIR)/usr/bin/pocketos-shell
	rm -f $(TARGET_DIR)/etc/init.d/S90pocketos-shell
	rm -f $(TARGET_DIR)/usr/share/doors/vision/yolov8n.kmodel
	rmdir $(TARGET_DIR)/usr/share/doors/vision 2>/dev/null || true
endef

# After every package has installed: no refused model anywhere in the target.
define POCKETOS_REFUSE_DETECTOR_MODELS
	refused=$$(find $(TARGET_DIR) -type f -iname 'yolo*.kmodel' 2>/dev/null); \
	for f in $$(find $(TARGET_DIR) -type f -iname '*.kmodel' 2>/dev/null); do \
		h=$$(sha256sum "$$f" | cut -c1-64); \
		grep -q "^$$h " $(POCKETOS_DIR)/tools/vision/refused-models.sha256 && refused="$$refused $$f"; \
	done; \
	if [ -n "$$refused" ]; then \
		echo "pocketos: refused Vision model(s) in the target:$$refused (docs/apps/VISION.md, The model)" >&2; \
		exit 1; \
	fi
endef
POCKETOS_TARGET_FINALIZE_HOOKS += POCKETOS_REFUSE_DETECTOR_MODELS

# Packages the Doors fragment turns off (configs/k230_pocketos.fragment): a
# target tree that once had them keeps their files, as Buildroot never
# deletes from $(TARGET_DIR). Buildroot's own record of what each package
# installed (packages-file-list.txt) names them; the shared module indexes
# (modules.*) are left alone, and the kernel's depmod hook, which runs after
# this one (linux/linux.mk is included after the packages), rebuilds them
# without the removed modules. A fresh build has nothing to remove.
POCKETOS_DROPPED_PACKAGES = rtl8723ds:$(BR2_PACKAGE_RTL8723DS) rtl8723ds-bt:$(BR2_PACKAGE_RTL8723DS_BT) aic8800:$(BR2_PACKAGE_AIC8800) \
	face_detect:$(BR2_PACKAGE_FACE_DETECT) ai2d_kpu:$(BR2_PACKAGE_AI2D_KPU)
define POCKETOS_REMOVE_DROPPED_PACKAGES
	for e in $(POCKETOS_DROPPED_PACKAGES); do \
		p=$${e%%:*}; [ "$${e#*:}" = y ] && continue; \
		grep "^$$p,\./" $(BUILD_DIR)/packages-file-list.txt 2>/dev/null | cut -d, -f2- | \
			grep -v -E '/lib/modules/[^/]+/modules\.[a-z.]+$$' | \
			while IFS= read -r f; do rm -f "$(TARGET_DIR)/$$f"; done; \
	done; \
	for d in lib/firmware/aic8800 lib/firmware/aic8800D80 lib/firmware/aic8800D80X2 lib/firmware/aic8800DC \
		lib/firmware/rtlbt lib/firmware/rtl_bt root/app/face_detect root/app/ai2d_kpu; do \
		rmdir "$(TARGET_DIR)/$$d" 2>/dev/null || true; done; \
	find $(TARGET_DIR)/lib/modules -type d -empty -delete 2>/dev/null || true
endef
POCKETOS_TARGET_FINALIZE_HOOKS += POCKETOS_REMOVE_DROPPED_PACKAGES

# libnncase (kept: pos-vision links its runtime statically) also unpacks the
# runtime's Python wheel, nncaseruntime_k230, into site-packages whenever
# Python is in the image. Nothing in the image imports it, and its K230
# modules state no licence (docs/licensing/APACHE_2_READINESS.md B5), so it
# is removed after every build; nothing installs it again later.
define POCKETOS_REMOVE_NNCASE_WHEEL
	rm -rf $(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/nncaseruntime \
		$(TARGET_DIR)/usr/lib/python$(PYTHON3_VERSION_MAJOR)/site-packages/nncaseruntime_k230-*.dist-info
endef
POCKETOS_TARGET_FINALIZE_HOOKS += POCKETOS_REMOVE_NNCASE_WHEEL

$(eval $(generic-package))
