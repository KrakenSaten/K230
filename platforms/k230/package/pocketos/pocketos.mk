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
POCKETOS_LICENSE = Proprietary (license not yet decided, see docs/decisions)
POCKETOS_INSTALL_TARGET = YES
POCKETOS_DEPENDENCIES = cjson libgpiod2 lvgl libdrm libevdev host-cmake

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
	$(TARGET_MAKE_ENV) $(MAKE) ENABLE_SX1262=1 -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr install
	$(INSTALL) -D -m 0755 $(POCKETOS_SHELL_BUILD_DIR)/pocketos-shell $(TARGET_DIR)/usr/bin/pocketos-shell
endef

$(eval $(generic-package))
