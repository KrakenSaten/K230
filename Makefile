# PocketOS first-party build. Called by the Buildroot package (platforms/k230)
# with CC/CXX/CFLAGS/LDFLAGS set for the target, or natively for host tests.
#
# ENABLE_SX1262=1 compiles the real radio backend (C++, RadioLib, libgpiod v2).
# RADIOLIB_DIR points at RadioLib's src/ (vendor/RadioLib/src by default, or
# third_party/RadioLib/src when synced into the Buildroot package).
PREFIX  ?= /usr
DESTDIR ?=
CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2
CXXFLAGS ?= $(CFLAGS)
LDLIBS  += -lcjson -lm
ENABLE_SX1262 ?= 0
RADIOLIB_DIR ?= $(if $(wildcard third_party/RadioLib/src),third_party/RadioLib/src,vendor/RadioLib/src)
# ggwave (MIT, the acoustic modem compiled into pos-wave): vendor/ggwave at the
# commit in platforms/k230/vendor_ggwave_commit.txt, or third_party/ggwave when
# synced into the Buildroot package. Unlike RadioLib it is always built.
GGWAVE_DIR ?= $(if $(wildcard third_party/ggwave/src/ggwave.cpp),third_party/ggwave,vendor/ggwave)
POCKETOS_VERSION := $(shell cat VERSION)
# Build identity. In the Buildroot package the source tree has no git history,
# so apply_to_sdk.sh writes BUILD_ID beside VERSION when it exports the tree;
# in a working checkout it comes from git. A build that has neither says so.
POCKETOS_BUILD_ID := $(shell cat BUILD_ID 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
COMMON_FLAGS := -Wall -Wextra -Icore -DPOCKETOS_VERSION=\"$(POCKETOS_VERSION)\" \
                -DPOCKETOS_BUILD_ID=\"$(POCKETOS_BUILD_ID)\"
# The identity as a file, because a -D is invisible to make: an object built
# at one commit kept that commit's identity for as long as its source was
# untouched, and a `make` after a commit produced binaries reporting the
# build before it. The stamp is written here, at parse time, so it exists
# before the first compile, and replaced only when the identity actually
# changes, so an unchanged identity still rebuilds nothing.
POCKETOS_BUILD_STAMP := .build-identity
$(shell printf '%s %s\n' '$(POCKETOS_VERSION)' '$(POCKETOS_BUILD_ID)' > $(POCKETOS_BUILD_STAMP).tmp; \
        cmp -s $(POCKETOS_BUILD_STAMP).tmp $(POCKETOS_BUILD_STAMP) \
          && rm -f $(POCKETOS_BUILD_STAMP).tmp \
          || mv -f $(POCKETOS_BUILD_STAMP).tmp $(POCKETOS_BUILD_STAMP))
# Compiler-generated header dependencies (.d next to each .o) so a changed
# header rebuilds every object that includes it (PocketFleet finding 1).
DEPFLAGS := -MMD -MP
ALL_CFLAGS := $(CFLAGS) -std=gnu11 $(COMMON_FLAGS) $(DEPFLAGS)
ALL_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(COMMON_FLAGS) -Iservices/radiod -I$(RADIOLIB_DIR) -DRADIOLIB_LOW_LEVEL=1 $(DEPFLAGS)

PATHS_OBJS  := core/pocketpaths.o
IPC_OBJS    := core/pocketipc/pocketipc.o
LOG_OBJS    := core/pocketlog/pocketlog.o
SYS_OBJS    := core/pocketsys.o
POS_OBJS    := tools/pos/pos.o tools/pos/pos_radio.o tools/pos/pos_logs.o tools/pos/pos_app.o tools/pos/pos_system.o \
               tools/pos/pos_wifi.o $(IPC_OBJS) $(PATHS_OBJS)
# The transmit state machine, the lease and the two clocks are their own
# objects so tests/radiod_tx_test.c can link them without main.c.
RADIOD_CORE_OBJS := services/radiod/tx.o services/radiod/lease.o \
                    services/radiod/radio_time.o services/radiod/backend_mock.o \
                    services/radiod/airtime.o
RADIOD_OBJS := services/radiod/main.o services/radiod/rf_state.o $(RADIOD_CORE_OBJS) $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
# Everything sysd is except the power actions, which exist twice: once as
# shipped and once with the test hook (see tests/sysd-testhooks below).
SYSD_BASE_OBJS := services/sysd/main.o services/sysd/sysd_services.o services/sysd/sysd_logs.o $(SYS_OBJS) $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
SYSD_OBJS   := $(SYSD_BASE_OBJS) services/sysd/sysd_power.o
# netd: wifi.* (docs/api/network.md). As with sysd, the one object that touches
# the machine (netd_sys) exists twice: as shipped, and with the test hooks
# tests/netd_test.sh needs to run it against a fake supplicant.
NETD_WIFI_OBJS := services/netd/wifi_parse.o services/netd/wifi_store.o services/netd/wpa_ctrl.o
NETD_BASE_OBJS := services/netd/main.o services/netd/wifi_mgr.o $(NETD_WIFI_OBJS) $(IPC_OBJS) \
                  core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
NETD_OBJS   := $(NETD_BASE_OBJS) services/netd/netd_sys.o

RADIOLIB_SRCS := $(RADIOLIB_DIR)/Hal.cpp $(RADIOLIB_DIR)/Module.cpp \
                 $(wildcard $(RADIOLIB_DIR)/modules/SX126x/*.cpp) \
                 $(RADIOLIB_DIR)/protocols/PhysicalLayer/PhysicalLayer.cpp \
                 $(wildcard $(RADIOLIB_DIR)/utils/*.cpp)
SX1262_OBJS := services/radiod/hal_linux.o services/radiod/backend_sx1262.o $(RADIOLIB_SRCS:.cpp=.o)

ifeq ($(ENABLE_SX1262),1)
RADIOD_OBJS += $(SX1262_OBJS)
RADIOD_LINK := $(CXX)
RADIOD_LIBS := $(LDLIBS) -lgpiod
ALL_CFLAGS += -DPOCKETOS_HAVE_SX1262
ALL_CXXFLAGS += -DPOCKETOS_HAVE_SX1262
else
RADIOD_LINK := $(CC)
RADIOD_LIBS := $(LDLIBS)
endif

# ---- meshcored: the MeshCore protocol service ----------------------------
#
# ENABLE_MESHCORED=1 builds and installs it, the way ENABLE_SX1262=1 adds the
# real radio backend. The image package turns it on (pocketos.mk), so every
# image carries meshcored and its init script, disabled per unit. An ordinary
# host build - `make all`, `make test` - leaves it off.
#
# The reason is a prerequisite, not a doubt about the service: meshcored
# links protocols/meshcore, which needs two upstream checkouts an ordinary
# Doors build does not have (vendor/RIFT and vendor/Crypto, the same two
# tools/meshcore-frame needs). apply_to_sdk.sh exports them, pin-checked, into
# the package's third_party/, which the two variables below prefer. See
# docs/services/MESHCORED.md for what enabling it means on hardware.
ENABLE_MESHCORED ?= 0
AR ?= ar

MESHCORE_DIR := protocols/meshcore
MESHCORE_LIB := $(MESHCORE_DIR)/libmeshcore.a
MESHCORE_RIFT_DIR ?= $(if $(wildcard third_party/RIFT/src),third_party/RIFT,vendor/RIFT)
MESHCORE_CRYPTO_REPO ?= $(if $(wildcard third_party/Crypto/libraries/Crypto),third_party/Crypto,vendor/Crypto)
# The vendored trees are reached with -isystem, as protocols/meshcore reaches
# them: they are third-party code compiled as their authors wrote it, and
# without it every translation unit here would repeat their warnings.
MESHCORE_INCLUDES := -I$(MESHCORE_DIR)/port -I$(MESHCORE_DIR)/compat \
                     -isystem $(MESHCORE_RIFT_DIR)/src \
                     -isystem $(MESHCORE_RIFT_DIR)/lib/ed25519 \
                     -isystem $(MESHCORE_CRYPTO_REPO)/libraries/Crypto
MESHCORE_DEFINES := -DMESHCORE_RIFT_COMMIT=\"$(shell tr -d ' \t\r\n' < $(MESHCORE_DIR)/vendor_rift_commit.txt 2>/dev/null)\" \
                    -DMESHCORE_CRYPTO_COMMIT=\"$(shell tr -d ' \t\r\n' < $(MESHCORE_DIR)/vendor_crypto_commit.txt 2>/dev/null)\"
# The two trees, named once, as absolute paths, and forwarded to every build
# that touches them. This is not tidiness: meshcored's own objects reach the
# MeshCore headers with -isystem from the variables above, and
# libmeshcore.a is built by a sub-make that had defaults of its own. Left
# unforwarded, `make MESHCORE_RIFT_DIR=<somewhere-else>` compiled our
# translation units against one checkout and linked them against a library
# built from another - one binary, two revisions of the wire format, and
# nothing to say so. The pin check runs in the sub-make, so it would have
# validated the tree that was NOT supplying the headers.
MESHCORE_TREES := RIFT_DIR="$(abspath $(MESHCORE_RIFT_DIR))" \
                  CRYPTO_REPO="$(abspath $(MESHCORE_CRYPTO_REPO))"
# -MD, not -MMD: every vendored MeshCore and Crypto header is a system header
# to this build because -isystem is how they are reached, and -MMD would
# leave them out of the dependency files. Same reason, same fix, as
# protocols/meshcore/Makefile.
MESHCORED_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(COMMON_FLAGS) -Iservices/meshcored \
                      -Iservices/radiod $(MESHCORE_INCLUDES) $(MESHCORE_DEFINES) -MD -MP
MESHCORED_C_OBJS := services/meshcored/main.o services/meshcored/api.o \
                    services/meshcored/radio_link.o services/meshcored/tx_map.o \
                    services/meshcored/mcd_util.o
MESHCORED_CXX_OBJS := services/meshcored/mesh_runtime.o services/meshcored/mesh_store.o
# radiod's airtime formula, linked rather than copied: the time-on-air the
# protocol core budgets with has to be the one the radio will really take.
MESHCORED_OBJS := $(MESHCORED_C_OBJS) $(MESHCORED_CXX_OBJS) services/radiod/airtime.o \
                  $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)

# ---- the shipping gate ----------------------------------------------------
#
# meshcored may be BUILT and TESTED with ENABLE_MESHCORED=1 at any time.
# Installing it puts MeshCore, orlp's ed25519 and rweather's Crypto into
# something distributable, so it may travel only while the notices carry an
# entry for each of the three. They have since 2026-09-22 (docs/LICENSING.md
# item 9), which is what lets the image package install it; the check stays,
# so an entry removed later stops the install again.
#
# The check is on the notices themselves rather than on a flag somebody could
# also set, so it answers the real question - do the notices cover what this
# binary contains - and it stops demanding anything the moment they do. It is
# a prerequisite of `install`, not a line inside it: under `make -j` a
# prerequisite completes before the recipe starts, so nothing is installed
# before the refusal.
MESHCORE_NOTICE_IDS := meshcore ed25519 arduinolibs-crypto
MESHCORE_NOTICES_OK := $(shell ok=1; for i in $(MESHCORE_NOTICE_IDS); do \
        grep -q "^$$i *|" third_party/notices/SOURCES 2>/dev/null || ok=0; done; echo $$ok)

.PHONY: meshcored-shipping-check
meshcored-shipping-check:
ifeq ($(ENABLE_MESHCORED),1)
	@if [ "$(MESHCORE_NOTICES_OK)" != "1" ]; then \
	    echo "ERROR: ENABLE_MESHCORED=1 would install meshcored, and meshcored contains" >&2; \
	    echo "       MeshCore, orlp's ed25519 and rweather's Crypto. The third-party" >&2; \
	    echo "       notices carry no entry for them, so this would distribute other" >&2; \
	    echo "       people's work with nothing said about it." >&2; \
	    echo "       Missing notices entries:" >&2; \
	    for i in $(MESHCORE_NOTICE_IDS); do \
	        grep -q "^$$i *|" third_party/notices/SOURCES 2>/dev/null || echo "         $$i" >&2; \
	    done; \
	    echo "       Building and testing meshcored is unaffected:" >&2; \
	    echo "         make ENABLE_MESHCORED=1 meshcored   and   make meshcored-test" >&2; \
	    echo "       See docs/LICENSING.md open item 9 and docs/services/MESHCORED.md." >&2; \
	    echo "       There is deliberately no override: this is a licensing decision," >&2; \
	    echo "       not a build preference." >&2; \
	    exit 1; \
	fi
	@echo "meshcored: the notices cover what it contains; installing it is allowed"
endif

BINS := tools/pos/pos services/radiod/radiod services/sysd/sysd services/netd/netd tools/hwcheck/pos-spixfer \
        tools/wave/pos-wave tools/camera/pos-camera tools/zabbix/pos-zabbix
ifeq ($(ENABLE_MESHCORED),1)
BINS += services/meshcored/meshcored
endif

all: $(BINS)

tools/pos/pos: $(POS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# pos-hwcheck's SPI transport: one CS-framed SPI_IOC_MESSAGE per command,
# the transaction radiod's HAL performs (no libraries).
tools/hwcheck/pos-spixfer: tools/hwcheck/spixfer.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

services/radiod/radiod: $(RADIOD_OBJS)
	$(RADIOD_LINK) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(RADIOD_LIBS)

# sysd: system.* from core/pocketsys (docs/api/system.md). C only, cJSON only.
services/sysd/sysd: $(SYSD_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

services/netd/netd: $(NETD_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# meshcored: mesh.* over pocketipc on top of radiod's radio.* (docs/api/mesh.md).
# The C half speaks JSON and knows no MeshCore; the two C++ objects are the
# protocol runtime and its persistence and know no JSON. Its own pattern rule,
# so the vendored include paths stay off every other object in the tree.
.PHONY: meshcore-lib
meshcore-lib:
	$(MAKE) -C $(MESHCORE_DIR) CC="$(CC)" CXX="$(CXX)" AR="$(AR)" CXXFLAGS="$(CXXFLAGS)" \
	        $(MESHCORE_TREES)

services/meshcored/%.o: services/meshcored/%.cpp
	$(CXX) $(MESHCORED_CXXFLAGS) -c -o $@ $<

$(MESHCORED_C_OBJS): ALL_CFLAGS += -Iservices/meshcored

services/meshcored/meshcored: meshcore-lib $(MESHCORED_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(MESHCORED_OBJS) $(MESHCORE_LIB) $(LDFLAGS) $(LDLIBS)

# A netd whose paths can be pointed at a test tree and a fake wpa_supplicant
# (services/netd/netd_sys.h). Only this object carries the hooks; the shipped
# services/netd/netd does not contain the variable names, which the test checks.
tests/netd_sys_hooks.o: services/netd/netd_sys.c services/netd/netd_sys.h
	$(CC) $(ALL_CFLAGS) -DNETD_TEST_HOOKS=1 -c -o $@ $<

tests/netd-testhooks: $(NETD_BASE_OBJS) tests/netd_sys_hooks.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# A stand-in for wpa_supplicant's control interface, driven by a scenario
# file (tests/fake_wpa_supplicant.c). Test helper, never installed.
tests/fake_wpa_supplicant: tests/fake_wpa_supplicant.o services/netd/wifi_parse.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fake_wpa_supplicant.o tests/wifi_parse_test.o tests/wifi_store_test.o: ALL_CFLAGS += -Iservices/netd

tests/wifi_parse_test: tests/wifi_parse_test.o services/netd/wifi_parse.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/wifi_store_test: tests/wifi_store_test.o services/netd/wifi_store.o services/netd/wifi_parse.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The fake root ($POCKETSYS_ROOT) is a test-only build option, so it gets its
# own object: this one honours the variable, the core/pocketsys.o that goes
# into sysd does not and cannot be pointed at a fake /proc by its environment.
tests/pocketsys_hooks.o: core/pocketsys.c
	$(CC) $(ALL_CFLAGS) -DPOCKETSYS_TEST_HOOKS=1 -c -o $@ $<

tests/pocketsys_test: tests/pocketsys_test.o tests/pocketsys_hooks.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The supervised-service table lives in sysd, not in core: it reads
# pos-supervise's state file (docs/api/system.md, services).
tests/sysd_services_test.o: tests/sysd_services_test.c services/sysd/sysd_services.h
	$(CC) $(ALL_CFLAGS) -Iservices/sysd -c -o $@ $<

tests/sysd_services_test: tests/sysd_services_test.o services/sysd/sysd_services.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# system.logs and system.crashes: the bounded log and crash-report reader,
# against a temporary log directory.
tests/sysd_logs_test.o: tests/sysd_logs_test.c services/sysd/sysd_logs.h
	$(CC) $(ALL_CFLAGS) -Iservices/sysd -c -o $@ $<

tests/sysd_logs_test: tests/sysd_logs_test.o services/sysd/sysd_logs.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# A sysd whose power actions can be pointed at a recorder instead of
# /sbin/reboot, so tests/sysd_test.sh can watch an action run without
# restarting the build host. Only this object is compiled with the hook; the
# shipped services/sysd/sysd links the plain one and does not contain the
# variable names at all, which the test checks.
tests/sysd_power_hooks.o: services/sysd/sysd_power.c services/sysd/sysd_power.h
	$(CC) $(ALL_CFLAGS) -Iservices/sysd -DSYSD_TEST_HOOKS=1 -c -o $@ $<

tests/sysd-testhooks: $(SYSD_BASE_OBJS) tests/sysd_power_hooks.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(ALL_CXXFLAGS) -c -o $@ $<

# The objects that compile the identity in, named so make rebuilds exactly
# those when it changes. VERSION is in the stamp too, so this replaces the
# lone `tools/pos/pos.o: VERSION` that used to cover one of the three.
# tests/build_deps_test.sh fails if a source names the macros and its object
# is missing here.
# pos-zabbix's transport names POCKETOS_VERSION in its User-Agent; it is one
# source built under two names (ZABBIX_CURL), so both are listed.
POCKETOS_ID_OBJS := core/pocketlog/pocketlog.o tools/pos/pos.o tests/pocketlog_test.o \
                    core/zabbix/zbx_http_curl.o core/zabbix/zbx_http_none.o
$(POCKETOS_ID_OBJS): $(POCKETOS_BUILD_STAMP)

# Compile-only check of the sx1262 backend on a host without libgpiod v2
# (GPIOD_INCLUDE points at vendor/libgpiod/include).
sx1262-objs: ALL_CXXFLAGS += -DPOCKETOS_HAVE_SX1262 $(if $(GPIOD_INCLUDE),-I$(GPIOD_INCLUDE),)
sx1262-objs: $(SX1262_OBJS)

tests/airtime_test: tests/airtime_test.o services/radiod/airtime.o
	$(CC) $(ALL_CFLAGS) -Iservices/radiod -o $@ $^ $(LDFLAGS) -lm

tests/airtime_test.o: tests/airtime_test.c
	$(CC) $(ALL_CFLAGS) -Iservices/radiod -c -o $@ $<

# radiod's transmit state machine and lease against the mock backend, with no
# socket, no daemon and no poll loop around them.
tests/radiod_tx_test: tests/radiod_tx_test.o $(RADIOD_CORE_OBJS)
	$(CC) $(ALL_CFLAGS) -Iservices/radiod -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/radiod_tx_test.o: tests/radiod_tx_test.c
	$(CC) $(ALL_CFLAGS) -Iservices/radiod -c -o $@ $<

tests/pocketlog_test: tests/pocketlog_test.o $(LOG_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/pocketipc_test: tests/pocketipc_test.o $(IPC_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Theme engine (pure C, no LVGL) and its test against docs/design/themes.json.
THEME_OBJS := ui/pocketui/pos_theme.o

# The generated table is committed; it is only regenerated where the design
# sources exist (they are not synced into the Buildroot package).
ui/pocketui/pos_theme_table.h: $(wildcard docs/design/themes.json) $(wildcard tools/design/gen_theme_table.py)
	$(if $(wildcard docs/design/themes.json),python3 tools/design/gen_theme_table.py docs/design/themes.json $@,@true)

ui/pocketui/pos_theme.o: ui/pocketui/pos_theme.c ui/pocketui/pos_theme.h ui/pocketui/pos_theme_table.h
	$(CC) $(ALL_CFLAGS) -Iui/pocketui -c -o $@ $<

tests/theme_test: tests/theme_test.o $(THEME_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/theme_test.o: tests/theme_test.c ui/pocketui/pos_theme.h
	$(CC) $(ALL_CFLAGS) -Iui/pocketui -c -o $@ $<

# Shell settings store (pure C) and its test.
ui/shell/settings.o: ui/shell/settings.c ui/shell/settings.h core/pocketpaths.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/settings_test: tests/settings_test.o ui/shell/settings.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/settings_test.o: tests/settings_test.c ui/shell/settings.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

# Display brightness over the backlight class (pure C, sysfs root passed in),
# tested against a fake sysfs tree. The shell links the same source (CMake).
ui/shell/brightness.o: ui/shell/brightness.c ui/shell/brightness.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

# DOORS Controls' decisions (pure C): tile texts, the antenna question and
# the layout in both orientations. controls.c (LVGL) draws them.
ui/shell/controls_model.o: ui/shell/controls_model.c ui/shell/controls_model.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/controls_model_test.o: tests/controls_model_test.c ui/shell/controls_model.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/controls_model_test: tests/controls_model_test.o ui/shell/controls_model.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# System volume (pure C): the stored values, mute, the sound-card check and
# the round trip through the settings store.
ui/shell/volume.o: ui/shell/volume.c ui/shell/volume.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/volume_test.o: tests/volume_test.c ui/shell/volume.h ui/shell/settings.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/volume_test: tests/volume_test.o ui/shell/volume.o ui/shell/settings.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/brightness_test.o: tests/brightness_test.c ui/shell/brightness.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/brightness_test: tests/brightness_test.o ui/shell/brightness.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# Display geometry and safe area (pure C, no LVGL): the one mapping between
# panel and logical pixels, the inset transform and the touch calibration
# derived from it. The shell and the widgets link the same source (CMake).
ui/pocketui/pos_display.o: ui/pocketui/pos_display.c ui/pocketui/pos_display.h
	$(CC) $(ALL_CFLAGS) -Iui/pocketui -c -o $@ $<

tests/display_geometry_test.o: tests/display_geometry_test.c ui/pocketui/pos_display.h
	$(CC) $(ALL_CFLAGS) -Iui/pocketui -c -o $@ $<

tests/display_geometry_test: tests/display_geometry_test.o ui/pocketui/pos_display.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The rotation policy and the keyboard-presence state it reads (pure C),
# tested with the settings store they persist through. The shell links the
# same sources (CMake).
ORIENTATION_OBJS := ui/shell/orientation.o ui/shell/kbd_presence.o ui/pocketui/pos_display.o

ui/shell/orientation.o: ui/shell/orientation.c ui/shell/orientation.h ui/shell/kbd_presence.h ui/pocketui/pos_display.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -Iui/pocketui -c -o $@ $<

ui/shell/kbd_presence.o: ui/shell/kbd_presence.c ui/shell/kbd_presence.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/orientation_test.o: tests/orientation_test.c ui/shell/orientation.h ui/shell/kbd_presence.h ui/shell/settings.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -Iui/pocketui -c -o $@ $<

tests/orientation_test: tests/orientation_test.o $(ORIENTATION_OBJS) ui/shell/settings.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The status chrome policy (pure C, DS §30): what an app's declaration
# resolves to in each orientation and on the launcher, the bar's height under
# it, and the content box below it with and without the keyboard. The shell
# and every app test link the same source (CMake).
ui/shell/chrome.o: ui/shell/chrome.c ui/shell/chrome.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/chrome_test.o: tests/chrome_test.c ui/shell/chrome.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/chrome_test: tests/chrome_test.o ui/shell/chrome.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The DOORS launcher's geometry and groups (pure C, DS §31.2-31.3) and the
# rules for the shell's runtime art files (ui/shell/art_format.h). The shell
# links the same sources (CMake); tests/doors_ui_assets_test.sh checks the
# committed art itself.
ui/shell/home_layout.o: ui/shell/home_layout.c ui/shell/home_layout.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

ui/shell/art_format.o: ui/shell/art_format.c ui/shell/art_format.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/home_layout_test.o: tests/home_layout_test.c ui/shell/home_layout.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/home_layout_test: tests/home_layout_test.o ui/shell/home_layout.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/art_format_test.o: tests/art_format_test.c ui/shell/art_format.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/art_format_test: tests/art_format_test.o ui/shell/art_format.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The debounce every keyboard-presence provider goes through: what a base
# board being mated, unmated or bouncing does to the published state, and so
# to the orientation. The provider itself (ui/shell/shell_kbd.c) needs LVGL
# and is exercised by tests/shell_kbd_test.c and the shell tests.
tests/kbd_presence_test.o: tests/kbd_presence_test.c ui/shell/kbd_presence.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/kbd_presence_test: tests/kbd_presence_test.o ui/shell/kbd_presence.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The physical keyboard's controller logic (docs/hardware/
# KEYBOARD_DRIVER_DESIGN_2026-09-12.md). It is deliberately free of LVGL,
# /dev/mem and libgpiod so the init sequence, the FIFO drain, overflow
# recovery and the retry throttle are tested here against a fake bus, with
# no keyboard attached. The bus backend and the shell glue need LVGL or the
# board, and are built by ui/shell (CMake).
ui/shell/kbd_tca8418.o: ui/shell/kbd_tca8418.c ui/shell/kbd_tca8418.h ui/shell/kbd_bus.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/kbd_tca8418_test.o: tests/kbd_tca8418_test.c ui/shell/kbd_tca8418.h ui/shell/kbd_bus.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -c -o $@ $<

tests/kbd_tca8418_test: tests/kbd_tca8418_test.o ui/shell/kbd_tca8418.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The bit-banged I2C one layer below that, against a fake libgpiod so a GPIO
# access that fails can be injected where it actually happens (cold review
# F6). The translation unit is included by the test, so there is no separate
# object for it and no libgpiod on the host.
tests/kbd_bus_k230_test.o: tests/kbd_bus_k230_test.c ui/shell/kbd_bus_k230.c ui/shell/kbd_bus_k230.h ui/shell/kbd_bus.h tests/fake/gpiod.h
	$(CC) $(ALL_CFLAGS) -Iui/shell -Itests/fake -c -o $@ $<

tests/kbd_bus_k230_test: tests/kbd_bus_k230_test.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/paths_test: tests/paths_test.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# System Status presentation (pure C, no LVGL). It lives beside its app in
# apps/system but is built here so the decisions that go wrong in a status
# screen - null read as zero, a dropped poll blanking it, an action fired
# before it was confirmed - are unit-tested; the app itself is built by
# ui/shell (CMake).
apps/system/system_view.o: apps/system/system_view.c apps/system/system_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/system -c -o $@ $<

tests/system_view_test.o: tests/system_view_test.c apps/system/system_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/system -c -o $@ $<

tests/system_view_test: tests/system_view_test.o apps/system/system_view.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# System > Diagnostics (pure C): the summary, crash reports and the log list,
# and the staged refresh; the page itself is apps/system/system_app.c.
apps/system/diag_view.o: apps/system/diag_view.c apps/system/diag_view.h apps/system/system_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/system -c -o $@ $<

tests/diag_view_test.o: tests/diag_view_test.c apps/system/diag_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/system -c -o $@ $<

tests/diag_view_test: tests/diag_view_test.o apps/system/diag_view.o apps/system/system_view.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Settings presentation (pure C, no LVGL), arranged like System Status: what
# a Wi-Fi state or a tap on a network means is decided here and unit-tested;
# the screen is built by ui/shell (CMake).
apps/settings/settings_view.o: apps/settings/settings_view.c apps/settings/settings_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/settings -c -o $@ $<

tests/settings_view_test.o: tests/settings_view_test.c apps/settings/settings_view.h
	$(CC) $(ALL_CFLAGS) -Iapps/settings -c -o $@ $<

tests/settings_view_test: tests/settings_view_test.o apps/settings/settings_view.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# PocketFleet game engine (pure C, no LVGL). It lives beside its app in
# apps/fleet/engine but is built here so it is unit-tested with the rest of
# the tree; the app itself is built by ui/shell (CMake).
FLEET_DIR := apps/fleet/engine
FLEET_OBJS := $(FLEET_DIR)/fleet_types.o $(FLEET_DIR)/fleet_rng.o $(FLEET_DIR)/fleet_ai.o \
              $(FLEET_DIR)/fleet_rules.o $(FLEET_DIR)/fleet_save.o $(FLEET_DIR)/fleet_store.o
FLEET_TESTS := tests/fleet_rng_test tests/fleet_rules_test tests/fleet_ai_test \
               tests/fleet_save_test tests/fleet_theme_test

$(FLEET_DIR)/%.o: $(FLEET_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(FLEET_DIR) -c -o $@ $<

tests/fleet_%_test.o: tests/fleet_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(FLEET_DIR) -Iapps/fleet/net -c -o $@ $<

tests/fleet_rng_test: tests/fleet_rng_test.o $(FLEET_DIR)/fleet_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_rules_test: tests/fleet_rules_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_ai_test: tests/fleet_ai_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_save_test: tests/fleet_save_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# PocketFleet multiplayer (docs/apps/FLEET_MULTIPLAYER.md): the protocol, the
# match state machine and its save codec. Pure C like the engine - no LVGL,
# no IPC, no filesystem (tests/fleet_lint.sh) - so thousands of matches run
# natively over a fake network.
FLEET_NET_DIR := apps/fleet/net
FLEET_NET_OBJS := $(FLEET_NET_DIR)/fleet_sha256.o $(FLEET_NET_DIR)/fleet_proto.o \
                  $(FLEET_NET_DIR)/fleet_match.o $(FLEET_NET_DIR)/fleet_match_save.o
FLEET_NET_TESTS := tests/fleet_sha256_test tests/fleet_proto_test tests/fleet_match_test \
                   tests/fleet_mp_sim_test tests/fleet_session_test tests/fleet_view_mp_test
# The session and the virtual opponent (apps/fleet/link) and the multiplayer
# view model, also pure C: the shell builds them into the app, and they are
# tested here natively.
FLEET_LINK_OBJS := apps/fleet/link/fleet_link.o apps/fleet/link/fleet_link_loop.o \
                   apps/fleet/link/fleet_session.o
FLEET_VIEW_MP_OBJS := apps/fleet/ui/fleet_view_mp.o apps/fleet/ui/fleet_view.o
FLEET_TESTS += $(FLEET_NET_TESTS)

$(FLEET_NET_DIR)/%.o: $(FLEET_NET_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(FLEET_NET_DIR) -I$(FLEET_DIR) -c -o $@ $<

tests/fleet_sha256_test: tests/fleet_sha256_test.o $(FLEET_NET_DIR)/fleet_sha256.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_proto_test: tests/fleet_proto_test.o $(FLEET_NET_DIR)/fleet_proto.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_match_test: tests/fleet_match_test.o $(FLEET_NET_OBJS) $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_mp_sim_test: tests/fleet_mp_sim_test.o $(FLEET_NET_OBJS) $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

apps/fleet/link/%.o: apps/fleet/link/%.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

apps/fleet/ui/fleet_view_mp.o apps/fleet/ui/fleet_view.o: apps/fleet/ui/%.o: apps/fleet/ui/%.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

tests/fleet_session_test.o tests/fleet_view_mp_test.o: ALL_CFLAGS += -Iapps/fleet/link -Iapps/fleet/ui

tests/fleet_session_test: tests/fleet_session_test.o $(FLEET_LINK_OBJS) $(FLEET_NET_OBJS) $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_view_mp_test: tests/fleet_view_mp_test.o $(FLEET_VIEW_MP_OBJS) $(FLEET_LINK_OBJS) \
                          $(FLEET_NET_OBJS) $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The mesh link: the one part of Fleet that talks to a service, against a real
# socket and the scripted meshcored RIFT's client is tested against.
FLEET_MESH_LINK_OBJS := apps/fleet/link/fleet_link_mesh.o apps/fleet/link/fleet_link.o
tests/fleet_link_test.o: ALL_CFLAGS += -Iapps/fleet/link
tests/fleet_link_test: tests/fleet_link_test.o $(FLEET_MESH_LINK_OBJS) tests/fake_meshcored.o \
                       $(IPC_OBJS) core/pocketipc/server.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)
FLEET_TESTS += tests/fleet_link_test

# The simulator at scale: FLEET_SIM_MATCHES matches per fault profile, each
# checked against the same match on a perfect network. make test runs 300.
FLEET_SOAK_MATCHES ?= 5000
.PHONY: fleet-mp-soak
fleet-mp-soak: tests/fleet_mp_sim_test
	FLEET_SIM_MATCHES=$(FLEET_SOAK_MATCHES) ./tests/fleet_mp_sim_test

# The multiplayer suites again under the address and undefined-behaviour
# sanitizers, in their own build tree so the ordinary objects are untouched.
FLEET_MP_SAN_DIR := out/fleet-mp-san
.PHONY: fleet-mp-san-test
fleet-mp-san-test:
	rm -rf $(FLEET_MP_SAN_DIR) && mkdir -p $(FLEET_MP_SAN_DIR)
	git ls-files --cached --others --exclude-standard core apps/fleet tests/fleet_* \
	    tests/fake_meshcored.* Makefile VERSION \
	    | tar -cf - -T - | tar -xf - -C $(FLEET_MP_SAN_DIR)
	$(MAKE) -C $(FLEET_MP_SAN_DIR) CC="$(CC)" POCKETOS_BUILD_ID=$(POCKETOS_BUILD_ID) \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	    LDFLAGS="-fsanitize=address,undefined" $(FLEET_NET_TESTS) tests/fleet_link_test
	cd $(FLEET_MP_SAN_DIR) && ./tests/fleet_sha256_test && ./tests/fleet_proto_test && \
	    ./tests/fleet_match_test && FLEET_SIM_MATCHES=40 ./tests/fleet_mp_sim_test && \
	    ./tests/fleet_session_test && ./tests/fleet_view_mp_test && ./tests/fleet_link_test

# The app's colour contract, checked against the Design System theme tables.
tests/fleet_theme_test: tests/fleet_theme_test.o $(THEME_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

tests/fleet_theme_test.o: tests/fleet_theme_test.c ui/pocketui/pos_theme.h
	$(CC) $(ALL_CFLAGS) -Iui/pocketui -c -o $@ $<

# PocketRadar game engine (pure C, no LVGL and no I/O). Arranged like
# PocketFleet: the engine lives beside its app in apps/radar/engine and is
# built here so it is unit-tested with the rest of the tree; the app itself
# is built by ui/shell (CMake). PocketRadar keeps its store outside the
# engine, so RADAR_OBJS is pure computation (tests/radar_lint.sh).
RADAR_DIR := apps/radar/engine
RADAR_OBJS := $(RADAR_DIR)/radar_types.o $(RADAR_DIR)/radar_rng.o $(RADAR_DIR)/radar_rules.o $(RADAR_DIR)/radar_score.o
RADAR_TESTS := tests/radar_rng_test tests/radar_types_test tests/radar_rules_test tests/radar_score_test tests/radar_store_test
# The store is the app's only door to the filesystem, so it sits beside the
# app rather than inside the engine (tests/radar_lint.sh).
RADAR_APP_OBJS := apps/radar/radar_store.o

$(RADAR_DIR)/%.o: $(RADAR_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(RADAR_DIR) -c -o $@ $<

apps/radar/radar_store.o: apps/radar/radar_store.c
	$(CC) $(ALL_CFLAGS) -I$(RADAR_DIR) -Iapps/radar -c -o $@ $<

tests/radar_%_test.o: tests/radar_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(RADAR_DIR) -Iapps/radar -c -o $@ $<

tests/radar_rng_test: tests/radar_rng_test.o $(RADAR_DIR)/radar_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/radar_types_test: tests/radar_types_test.o $(RADAR_DIR)/radar_types.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/radar_rules_test: tests/radar_rules_test.o $(RADAR_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/radar_score_test: tests/radar_score_test.o $(RADAR_DIR)/radar_score.o $(RADAR_DIR)/radar_types.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/radar_store_test: tests/radar_store_test.o $(RADAR_APP_OBJS) $(RADAR_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# PocketNotes: the text rules and the store. Both are LVGL-free and do no
# hidden I/O, so they are unit-tested here; the app itself needs a display
# and is built by ui/shell (tests/notes_shell_test.sh).
NOTES_DIR := apps/notes
NOTES_OBJS := $(NOTES_DIR)/notes_view.o $(NOTES_DIR)/notes_store.o
NOTES_TESTS := tests/notes_view_test tests/notes_store_test

$(NOTES_DIR)/%.o: $(NOTES_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(NOTES_DIR) -c -o $@ $<

tests/notes_%_test.o: tests/notes_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(NOTES_DIR) -c -o $@ $<

tests/notes_view_test: tests/notes_view_test.o $(NOTES_DIR)/notes_view.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/notes_store_test: tests/notes_store_test.o $(NOTES_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# Files: the filesystem layer with its write policy, the worker that runs
# copy, move and delete, and the on-screen text. All LVGL-free
# (tests/files_lint.sh), so navigation, the policy and every operation -
# including what each one leaves behind when it fails - are unit-tested here
# against a real temporary tree. The app itself needs a display and is built
# by ui/shell (tests/files_shell_test.sh).
FILES_DIR := apps/files
FILES_OBJS := $(FILES_DIR)/files_fs.o $(FILES_DIR)/files_job.o $(FILES_DIR)/files_view.o
FILES_TESTS := tests/files_fs_test tests/files_view_test

$(FILES_DIR)/%.o: $(FILES_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(FILES_DIR) -c -o $@ $<

tests/files_%_test.o: tests/files_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(FILES_DIR) -c -o $@ $<

tests/files_fs_test: tests/files_fs_test.o $(FILES_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -pthread

tests/files_view_test: tests/files_view_test.o $(FILES_DIR)/files_view.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# PocketClock: the timekeeping, the alert seam, the clock reader and the
# store. All four are LVGL-free (tests/clock_lint.sh), so the decisions that
# go wrong in a clock - an alarm firing on every poll, a countdown measured
# against a wall clock somebody has just corrected, a board with no RTC
# showing 1970 as though it meant something - are unit-tested here, with both
# clocks injected and nothing sleeping. The app itself needs a display and is
# built by ui/shell (tests/clock_shell_test.sh).
CLOCK_DIR := apps/clock
CLOCK_OBJS := $(CLOCK_DIR)/clock_engine.o $(CLOCK_DIR)/clock_alert.o \
              $(CLOCK_DIR)/clock_time.o $(CLOCK_DIR)/clock_store.o \
              $(CLOCK_DIR)/clock_runtime.o
CLOCK_TESTS := tests/clock_engine_test tests/clock_time_test tests/clock_store_test \
               tests/clock_runtime_test tests/clock_handoff_test \
               tests/clock_restart_test

$(CLOCK_DIR)/%.o: $(CLOCK_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(CLOCK_DIR) -c -o $@ $<

tests/clock_%_test.o: tests/clock_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(CLOCK_DIR) -c -o $@ $<

tests/clock_engine_test: tests/clock_engine_test.o $(CLOCK_DIR)/clock_engine.o $(CLOCK_DIR)/clock_alert.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/clock_time_test: tests/clock_time_test.o $(CLOCK_DIR)/clock_time.o $(CLOCK_DIR)/clock_engine.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/clock_store_test: tests/clock_store_test.o $(CLOCK_DIR)/clock_store.o $(CLOCK_DIR)/clock_engine.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The runtime the shell owns: one engine, loaded, stepped and saved. Its
# clock is injectable, so an alarm can be reached without waiting for it.
tests/clock_runtime_test: tests/clock_runtime_test.o $(CLOCK_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The restart handoff: the codec and the rules, with both clocks injected, so
# a countdown can end in the middle of a restart without one passing.
tests/clock_handoff_test: tests/clock_handoff_test.o $(CLOCK_DIR)/clock_store.o \
                          $(CLOCK_DIR)/clock_engine.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# And the seam itself. This one execs itself, so the second half of every
# check below runs in a process image that has never seen the first: the same
# thing restart_in_place() does to the shell, and the only way to prove that
# what survives it is the file and not a static somebody forgot about.
tests/clock_restart_test: tests/clock_restart_test.o $(CLOCK_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# PocketCalendar: the date arithmetic and the view model. Both are LVGL-free,
# do no I/O and read no clock (tests/calendar_lint.sh), so the things that go
# wrong in a calendar - a February that is the wrong length, a month that
# starts in the wrong column, December not becoming January, and a board with
# no RTC marking 1 January 1970 as today - are unit-tested here with the
# system date handed in. The app itself needs a display and is built by
# ui/shell (CMake). There is no store: a calendar with no events has nothing
# to write.
CAL_DIR := apps/calendar
CAL_OBJS := $(CAL_DIR)/cal_date.o $(CAL_DIR)/cal_view.o
CAL_TESTS := tests/cal_date_test tests/cal_view_test

$(CAL_DIR)/%.o: $(CAL_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(CAL_DIR) -c -o $@ $<

# The date test links PocketClock's clock reader as well: it checks that the
# calendar's weekday and PocketClock's agree, and that the fallback month is
# the month of CLOCK_WALL_VALID_FROM.
tests/cal_%_test.o: tests/cal_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(CAL_DIR) -I$(CLOCK_DIR) -c -o $@ $<

tests/cal_date_test: tests/cal_date_test.o $(CAL_DIR)/cal_date.o \
                     $(CLOCK_DIR)/clock_time.o $(CLOCK_DIR)/clock_engine.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/cal_view_test: tests/cal_view_test.o $(CAL_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# PocketCalculator: the engine and the view. Both are LVGL-free, do no I/O,
# read no clock and store nothing (tests/calculator_lint.sh), so precedence,
# the entry rules, the error states and the formatting that keeps binary
# floating-point noise off the display are unit-tested here, including a
# fuzz run over hundreds of thousands of keys. The app itself needs a display
# and is built by ui/shell (CMake). The engine test links the view too: what
# it checks is the text a key sequence puts on the glass.
CALC_DIR := apps/calculator
CALC_OBJS := $(CALC_DIR)/calc_engine.o $(CALC_DIR)/calc_view.o
CALC_TESTS := tests/calc_engine_test tests/calc_view_test

$(CALC_DIR)/%.o: $(CALC_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(CALC_DIR) -c -o $@ $<

tests/calc_%_test.o: tests/calc_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(CALC_DIR) -c -o $@ $<

tests/calc_engine_test: tests/calc_engine_test.o $(CALC_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

tests/calc_view_test: tests/calc_view_test.o $(CALC_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# RIFT: the three halves of the app that are not LVGL. The model (what is
# known and how sure it is), the formatting (every string the screens print)
# and the meshcored client (the connection, the framing and the reconnect)
# have no display in them, so they are unit-tested here, the client against a
# real socket and a scripted service. The screens themselves need LVGL and
# are built by ui/shell (CMake), run by tests/rift_shell_test.sh.
RIFT_DIR := apps/rift
RIFT_OBJS := $(RIFT_DIR)/rift_model.o $(RIFT_DIR)/rift_clock.o $(RIFT_DIR)/rift_messages.o \
             $(RIFT_DIR)/rift_channels.o $(RIFT_DIR)/rift_actions.o \
             $(RIFT_DIR)/rift_order.o $(RIFT_DIR)/rift_format.o $(RIFT_DIR)/rift_format_msg.o \
             $(RIFT_DIR)/rift_ipc.o
# The model is several translation units over one struct: rift_model.c
# dispatches mesh.message and mesh.channel events into rift_messages.c and
# rift_channels.c, and the service going away settles the requests
# rift_actions.c holds, so anything linking one links them all.
RIFT_MODEL_OBJS := $(RIFT_DIR)/rift_model.o $(RIFT_DIR)/rift_clock.o $(RIFT_DIR)/rift_messages.o \
                   $(RIFT_DIR)/rift_channels.o $(RIFT_DIR)/rift_actions.o \
                   $(RIFT_DIR)/rift_order.o $(RIFT_DIR)/rift_format.o \
                   $(RIFT_DIR)/rift_format_msg.o
RIFT_TESTS := tests/rift_format_test tests/rift_model_test tests/rift_comms_test \
              tests/rift_ipc_test tests/fake-meshcored

$(RIFT_DIR)/%.o: $(RIFT_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(RIFT_DIR) -c -o $@ $<

tests/rift_%_test.o: tests/rift_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(RIFT_DIR) -c -o $@ $<

tests/fake_meshcored.o: tests/fake_meshcored.c tests/fake_meshcored.h
	$(CC) $(ALL_CFLAGS) -I$(RIFT_DIR) -c -o $@ $<

tests/rift_format_test: tests/rift_format_test.o $(RIFT_MODEL_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/rift_model_test: tests/rift_model_test.o $(RIFT_MODEL_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/rift_comms_test: tests/rift_comms_test.o $(RIFT_MODEL_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/rift_ipc_test: tests/rift_ipc_test.o tests/fake_meshcored.o $(RIFT_OBJS) $(IPC_OBJS) \
                     core/pocketipc/server.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The same scripted service as a program, so tests/rift_shell_test.sh can run
# one beside the real shell and the app can be photographed with real data
# on the other end of a real socket.
tests/fake_meshcored_main.o: tests/fake_meshcored_main.c tests/fake_meshcored.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

tests/fake-meshcored: tests/fake_meshcored_main.o tests/fake_meshcored.o $(IPC_OBJS) \
                      core/pocketipc/server.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# PocketTimber game engine (pure C, no LVGL, no I/O, no floating point).
# Arranged like PocketRadar: the engine lives beside its app in
# apps/timber/engine and is built here so it is unit-tested with the rest
# of the tree; the app itself will be built by ui/shell (CMake) from P7.
TIMBER_DIR := apps/timber/engine
TIMBER_OBJS := $(TIMBER_DIR)/timber_types.o $(TIMBER_DIR)/timber_rng.o $(TIMBER_DIR)/timber_tower.o \
               $(TIMBER_DIR)/timber_pull.o $(TIMBER_DIR)/timber_stability.o $(TIMBER_DIR)/timber_score.o \
               $(TIMBER_DIR)/timber_collapse.o $(TIMBER_DIR)/timber_rules.o $(TIMBER_DIR)/timber_replay.o
TIMBER_TESTS := tests/timber_rng_test tests/timber_types_test tests/timber_tower_test tests/timber_pull_test \
                tests/timber_stability_test tests/timber_score_test tests/timber_collapse_test tests/timber_rules_test \
                tests/timber_replay_test tests/timber_view_test tests/timber_store_test
# The store is the app's only door to the filesystem, so it sits beside the
# app rather than inside the engine (tests/timber_lint.sh).
TIMBER_APP_OBJS := apps/timber/timber_store.o

$(TIMBER_DIR)/%.o: $(TIMBER_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(TIMBER_DIR) -c -o $@ $<

apps/timber/timber_store.o: apps/timber/timber_store.c
	$(CC) $(ALL_CFLAGS) -I$(TIMBER_DIR) -Iapps/timber -c -o $@ $<

tests/timber_%_test.o: tests/timber_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(TIMBER_DIR) -Iapps/timber -Iapps/timber/ui -c -o $@ $<

tests/timber_rng_test: tests/timber_rng_test.o $(TIMBER_DIR)/timber_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_types_test: tests/timber_types_test.o $(TIMBER_DIR)/timber_types.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_tower_test: tests/timber_tower_test.o $(TIMBER_DIR)/timber_tower.o $(TIMBER_DIR)/timber_types.o $(TIMBER_DIR)/timber_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_pull_test: tests/timber_pull_test.o $(TIMBER_DIR)/timber_pull.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_stability_test: tests/timber_stability_test.o $(TIMBER_DIR)/timber_stability.o $(TIMBER_DIR)/timber_tower.o \
                             $(TIMBER_DIR)/timber_types.o $(TIMBER_DIR)/timber_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_score_test: tests/timber_score_test.o $(TIMBER_DIR)/timber_score.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_collapse_test: tests/timber_collapse_test.o $(TIMBER_DIR)/timber_collapse.o $(TIMBER_DIR)/timber_tower.o $(TIMBER_DIR)/timber_types.o $(TIMBER_DIR)/timber_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_rules_test: tests/timber_rules_test.o $(TIMBER_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_replay_test: tests/timber_replay_test.o $(TIMBER_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The view model is LVGL-free (tests/timber_lint.sh), so it is tested here
# too; the widget and screens that use it are built only by ui/shell.
TIMBER_UI_OBJS := apps/timber/ui/timber_view.o

tests/timber_view_test: tests/timber_view_test.o $(TIMBER_UI_OBJS) $(TIMBER_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/timber_store_test: tests/timber_store_test.o $(TIMBER_APP_OBJS) $(TIMBER_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# Audio and Wave (docs/apps/WAVE.md, docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md).
#
# core/pocketaudio is the audio layer: its policy (pocketaudio.o) is pure C
# over a backend seam and is tested against a fake; the ALSA and GPIO backend
# (pocketaudio_alsa.o) is linked only into pos-wave. apps/wave holds the view
# model, the helper client and the text rule (pure C, tested here), the ggwave
# wrapper (C++), and the LVGL screen, which is built by ui/shell (CMake).
# pos-wave is the helper: the one program that opens audio for Wave.
#
# ggwave is compiled into our own tree (tools/wave/ggwave.o), never beside its
# sources, so nothing built lands in the vendor checkout; its logging is
# compiled out because it prints every decoded payload to stderr.
AUDIO_OBJS := core/pocketaudio/pocketaudio.o
AUDIO_ALSA_OBJS := core/pocketaudio/pocketaudio_alsa.o
WAVE_DIR := apps/wave
WAVE_OBJS := $(WAVE_DIR)/wave_view.o $(WAVE_DIR)/wave_session.o $(WAVE_DIR)/wave_text.o
WAVE_MODEM_OBJS := $(WAVE_DIR)/wave_modem.o tools/wave/ggwave.o
POS_WAVE_OBJS := tools/wave/pos_wave.o tools/wave/wave_wav.o $(WAVE_DIR)/wave_text.o $(WAVE_MODEM_OBJS) \
                 $(AUDIO_OBJS) $(AUDIO_ALSA_OBJS) $(PATHS_OBJS)
WAVE_TESTS := tests/pocketaudio_test tests/wave_view_test tests/wave_session_test tests/wave_modem_test \
              tests/pos-wave-testhooks
GGWAVE_CXXFLAGS := $(CXXFLAGS) -std=c++11 -DNDEBUG -DGGWAVE_DISABLE_LOG -fno-exceptions -fno-rtti \
                   -I$(GGWAVE_DIR)/include $(DEPFLAGS)
WAVE_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(COMMON_FLAGS) -DGGWAVE_DISABLE_LOG -fno-exceptions -fno-rtti \
                 -I$(GGWAVE_DIR)/include -I$(WAVE_DIR) $(DEPFLAGS)

$(GGWAVE_DIR)/src/ggwave.cpp:
	@echo "ggwave is missing at $(GGWAVE_DIR): clone it into vendor/ggwave at the commit in" >&2
	@echo "platforms/k230/vendor_ggwave_commit.txt (docs/BUILD_ENVIRONMENT.md)." >&2
	@exit 1

tools/wave/ggwave.o: $(GGWAVE_DIR)/src/ggwave.cpp
	$(CXX) $(GGWAVE_CXXFLAGS) -c -o $@ $<

$(WAVE_DIR)/wave_modem.o: $(WAVE_DIR)/wave_modem.cpp $(GGWAVE_DIR)/src/ggwave.cpp
	$(CXX) $(WAVE_CXXFLAGS) -c -o $@ $<

$(WAVE_DIR)/%.o: $(WAVE_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(WAVE_DIR) -c -o $@ $<

tools/wave/pos_wave.o tools/wave/wave_wav.o: ALL_CFLAGS += -I$(WAVE_DIR) -Itools/wave

tools/wave/pos-wave: $(POS_WAVE_OBJS)
	$(CXX) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lasound -lm

tests/pocketaudio_test: tests/pocketaudio_test.o $(AUDIO_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# A pos-wave whose sound card and GPIO can be replaced by files
# (POS_WAVE_FAKE_AUDIO, tests/fake_audio_backend.c), so tests/
# audio_recovery_test.sh can SIGKILL a real one mid-operation and read what it
# left. As with netd and sysd, only this object carries the hook; the shipped
# tools/wave/pos-wave does not contain it, which the test checks.
tests/pos_wave_hooks.o: tools/wave/pos_wave.c
	$(CC) $(ALL_CFLAGS) -I$(WAVE_DIR) -Itools/wave -Itests -DPOS_WAVE_TEST_HOOKS=1 -c -o $@ $<

tests/pos-wave-testhooks: tests/pos_wave_hooks.o tests/fake_audio_backend.o \
                          $(filter-out tools/wave/pos_wave.o,$(POS_WAVE_OBJS))
	$(CXX) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lasound -lm

tests/wave_%_test.o: tests/wave_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(WAVE_DIR) -c -o $@ $<

tests/wave_view_test: tests/wave_view_test.o $(WAVE_DIR)/wave_view.o $(WAVE_DIR)/wave_text.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/wave_session_test: tests/wave_session_test.o $(WAVE_DIR)/wave_session.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/wave_modem_test: tests/wave_modem_test.o $(WAVE_MODEM_OBJS) $(AUDIO_OBJS) $(PATHS_OBJS)
	$(CXX) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Camera (docs/apps/CAMERA.md, ADR-006).
#
# core/pocketcam is the camera layer: the backend seam with the fake backend
# and the real V4L2 backend, the pixel conversion, the photo store and the
# still encoder - all pure C. pos-camera is the helper, the only program that
# opens the camera; the app's state machine, layout and helper client in
# apps/camera are LVGL-free and tested here, against the real helper on the
# fake backend. The V4L2 backend compiles here and runs only on the unit
# (docs/hardware/CAMERA_GATE.md). The screen is built by ui/shell
# (tests/camera_shell_test.sh).
#
# POCKETCAM_JPEG=1 encodes photos with libjpeg (in the K230 image and its
# sysroot; the Buildroot package sets it and depends on jpeg). Without it they
# are PPM, because this host has no libjpeg headers.
POCKETCAM_JPEG ?= 0
CAM_DIR := core/pocketcam
CAM_OBJS := $(CAM_DIR)/pocketcam.o $(CAM_DIR)/pocketcam_fake.o $(CAM_DIR)/pocketcam_v4l2.o \
            $(CAM_DIR)/pocketcam_convert.o $(CAM_DIR)/pocketcam_store.o $(CAM_DIR)/pocketcam_codec.o
CAM_LIBS :=
ifeq ($(POCKETCAM_JPEG),1)
CAM_LIBS := -ljpeg
$(CAM_DIR)/pocketcam_codec.o: ALL_CFLAGS += -DPOCKETCAM_HAVE_JPEG
endif
CAMERA_DIR := apps/camera
CAMERA_OBJS := $(CAMERA_DIR)/camera_state.o $(CAMERA_DIR)/camera_layout.o $(CAMERA_DIR)/camera_session.o
POS_CAMERA_OBJS := tools/camera/pos_camera.o $(CAM_OBJS) $(PATHS_OBJS)
CAMERA_TESTS := tests/pocketcam_test tests/camera_state_test tests/camera_layout_test \
                tests/camera_session_test tests/pos-camera-testhooks

$(CAMERA_DIR)/%.o: $(CAMERA_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(CAMERA_DIR) -c -o $@ $<

tools/camera/pos-camera: $(POS_CAMERA_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(CAM_LIBS)

# A pos-camera that reads a pretend free space and a pretend full disk from
# its environment (POCKETCAM_TEST_FREE_BYTES, POCKETCAM_TEST_FAIL_AFTER), so
# the session test can fill the store. Only this object carries the hook.
tests/pos_camera_hooks.o: tools/camera/pos_camera.c
	$(CC) $(ALL_CFLAGS) -DPOS_CAMERA_TEST_HOOKS=1 -c -o $@ $<

tests/pos-camera-testhooks: tests/pos_camera_hooks.o $(CAM_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(CAM_LIBS)

tests/camera_%_test.o: tests/camera_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(CAMERA_DIR) -c -o $@ $<

tests/pocketcam_test: tests/pocketcam_test.o $(CAM_OBJS) $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(CAM_LIBS)

tests/camera_state_test: tests/camera_state_test.o $(CAMERA_DIR)/camera_state.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/camera_layout_test: tests/camera_layout_test.o $(CAMERA_DIR)/camera_layout.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/camera_session_test: tests/camera_session_test.o $(CAMERA_DIR)/camera_session.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The camera suites again under the address and undefined-behaviour
# sanitizers, in a separate build tree so the ordinary objects are untouched.
# The helper is built with them too: the session test drives it.
CAMERA_SAN_DIR := out/camera-san
camera-san-test:
	rm -rf $(CAMERA_SAN_DIR) && mkdir -p $(CAMERA_SAN_DIR)
	git ls-files --cached --others --exclude-standard core apps/camera tools/camera tests/camera_* tests/pocketcam_test.c Makefile VERSION \
	    | tar -cf - -T - | tar -xf - -C $(CAMERA_SAN_DIR)
	$(MAKE) -C $(CAMERA_SAN_DIR) CC="$(CC)" POCKETOS_BUILD_ID=$(POCKETOS_BUILD_ID) \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	    LDFLAGS="-fsanitize=address,undefined" $(CAMERA_TESTS)
	cd $(CAMERA_SAN_DIR) && ./tests/pocketcam_test && ./tests/camera_state_test && \
	    ./tests/camera_layout_test && ./tests/camera_session_test tests/pos-camera-testhooks

# Zabbix (docs/apps/ZABBIX.md, ADR-007 PROPOSED).
#
# core/zabbix is the Zabbix viewer's client layer, all pure C: the bounded
# model and the app/helper line protocol (the only two parts the shell links),
# and, for the helper alone, the JSON-RPC requests and their parsing (cJSON),
# the configuration and its secret, the connection state machine, the
# deterministic fake server and the libcurl transport. pos-zabbix is the
# helper, the only program that opens a connection to a Zabbix server; the
# app's helper client and presentation logic in apps/zabbix are LVGL-free and
# tested here, against the real helper on the fake backend. The screen is
# built by ui/shell (tests/zabbix_shell_test.sh).
#
# ZABBIX_CURL=1 builds the real transport with libcurl (in the K230 image with
# OpenSSL, and in its sysroot; the Buildroot package sets it and depends on
# libcurl). Without it pos-zabbix has only the fake backend and says so,
# because this host has no libcurl headers. ZABBIX_CURL_CFLAGS and
# ZABBIX_CURL_LIBS point a host build at headers and a library of its own
# (tests/zabbix_http_test.sh, docs/apps/ZABBIX.md "Validation").
ZABBIX_CURL ?= 0
ZABBIX_CURL_CFLAGS ?=
ZABBIX_CURL_LIBS ?= -lcurl
# The mock server speaks TLS when this host has the OpenSSL headers.
ZABBIX_MOCK_TLS ?= $(shell printf '\043include <openssl/ssl.h>\n' | $(CC) -E - >/dev/null 2>&1 && echo 1 || echo 0)
ZBX_DIR := core/zabbix
ZBX_APP_OBJS := $(ZBX_DIR)/zbx_model.o $(ZBX_DIR)/zbx_proto.o
ZBX_LIBS := -lcjson -lm
# The transport is one source built two ways, under two object names, so a
# tree that has been built both ways can never link the wrong one.
ifeq ($(ZABBIX_CURL),1)
ZBX_HTTP_OBJ := $(ZBX_DIR)/zbx_http_curl.o
$(ZBX_HTTP_OBJ): ALL_CFLAGS += -DZBX_HAVE_CURL $(ZABBIX_CURL_CFLAGS)
ZBX_LIBS += $(ZABBIX_CURL_LIBS)
else
ZBX_HTTP_OBJ := $(ZBX_DIR)/zbx_http_none.o
endif
ZBX_OBJS := $(ZBX_APP_OBJS) $(ZBX_DIR)/zbx_api.o $(ZBX_DIR)/zbx_fake.o $(ZBX_DIR)/zbx_config.o \
            $(ZBX_DIR)/zbx_client.o $(ZBX_HTTP_OBJ)

$(ZBX_DIR)/zbx_http_none.o: $(ZBX_DIR)/zbx_http_curl.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
ZBX_MOCK_OBJS := tools/zabbix/pos_zabbix_mock.o $(ZBX_DIR)/zbx_fake.o $(ZBX_DIR)/zbx_api.o \
                 $(ZBX_APP_OBJS)
ZBX_MOCK_LIBS := -lcjson -lm
ifeq ($(ZABBIX_MOCK_TLS),1)
tools/zabbix/pos_zabbix_mock.o: ALL_CFLAGS += -DZBX_MOCK_TLS
ZBX_MOCK_LIBS += -lssl -lcrypto
endif
ZABBIX_DIR := apps/zabbix
ZABBIX_OBJS := $(ZABBIX_DIR)/zabbix_session.o $(ZABBIX_DIR)/zabbix_view.o
POS_ZABBIX_OBJS := tools/zabbix/pos_zabbix.o $(ZBX_OBJS) $(PATHS_OBJS) $(LOG_OBJS)
ZABBIX_TESTS := tests/zbx_model_test tests/zbx_proto_test tests/zbx_api_test tests/zbx_config_test \
                tests/zbx_client_test tests/zabbix_view_test tests/zabbix_session_test \
                tools/zabbix/pos-zabbix-mock

$(ZABBIX_DIR)/%.o: $(ZABBIX_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(ZABBIX_DIR) -c -o $@ $<

tools/zabbix/pos-zabbix: $(POS_ZABBIX_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(ZBX_LIBS)

tools/zabbix/pos-zabbix-mock: $(ZBX_MOCK_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(ZBX_MOCK_LIBS)

tests/zbx_%_test.o: tests/zbx_%_test.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

tests/zabbix_%_test.o: tests/zabbix_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(ZABBIX_DIR) -c -o $@ $<

tests/zbx_model_test: tests/zbx_model_test.o $(ZBX_APP_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/zbx_proto_test: tests/zbx_proto_test.o $(ZBX_APP_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/zbx_api_test: tests/zbx_api_test.o $(ZBX_APP_OBJS) $(ZBX_DIR)/zbx_api.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lcjson -lm

tests/zbx_config_test: tests/zbx_config_test.o $(ZBX_APP_OBJS) $(ZBX_DIR)/zbx_config.o \
                       $(ZBX_DIR)/zbx_fake.o $(ZBX_DIR)/zbx_api.o $(PATHS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) -lcjson -lm

tests/zbx_client_test: tests/zbx_client_test.o $(ZBX_OBJS) $(PATHS_OBJS) $(LOG_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(ZBX_LIBS)

tests/zabbix_view_test: tests/zabbix_view_test.o $(ZABBIX_DIR)/zabbix_view.o $(ZBX_APP_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/zabbix_session_test: tests/zabbix_session_test.o $(ZABBIX_OBJS) $(ZBX_APP_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# The Zabbix suites again under the address and undefined-behaviour
# sanitizers, in a separate build tree so the ordinary objects are untouched.
# The helper is built with them too: the session test drives it.
ZABBIX_SAN_DIR := out/zabbix-san
zabbix-san-test:
	rm -rf $(ZABBIX_SAN_DIR) && mkdir -p $(ZABBIX_SAN_DIR)
	git ls-files --cached --others --exclude-standard core apps/zabbix tools/zabbix tests/zbx_* tests/zabbix_* Makefile VERSION \
	    | tar -cf - -T - | tar -xf - -C $(ZABBIX_SAN_DIR)
	$(MAKE) -C $(ZABBIX_SAN_DIR) CC="$(CC)" POCKETOS_BUILD_ID=$(POCKETOS_BUILD_ID) \
	    ZABBIX_CURL=$(ZABBIX_CURL) ZABBIX_CURL_CFLAGS="$(ZABBIX_CURL_CFLAGS)" ZABBIX_CURL_LIBS="$(ZABBIX_CURL_LIBS)" \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	    LDFLAGS="-fsanitize=address,undefined" $(ZABBIX_TESTS) tools/zabbix/pos-zabbix
	cd $(ZABBIX_SAN_DIR) && ./tests/zbx_model_test && ./tests/zbx_proto_test && ./tests/zbx_api_test && \
	    ./tests/zbx_config_test && ./tests/zbx_client_test && TZ=UTC ./tests/zabbix_view_test && \
	    ./tests/zabbix_session_test tools/zabbix/pos-zabbix

# Every binary `make test` builds on top of $(BINS). Each of them, and each of
# $(BINS), has to be git-ignored: apply_to_sdk.sh calls an image's BUILD_ID
# "<commit>-dirty" when `git status --porcelain` shows anything, so a single
# untracked test binary in the checkout an image is built from would make it
# claim changes it does not contain. tests/build_outputs_test.sh checks this
# list against .gitignore, so a test added here without an entry there fails.
TEST_BINS := tests/sysd-testhooks tests/netd-testhooks tests/fake_wpa_supplicant tests/wifi_parse_test \
             tests/wifi_store_test tests/airtime_test tests/radiod_tx_test \
             tests/pocketlog_test tests/pocketipc_test \
             tests/pocketsys_test tests/sysd_services_test tests/sysd_logs_test tests/system_view_test tests/diag_view_test tests/settings_view_test \
             tests/theme_test \
             tests/settings_test tests/brightness_test tests/volume_test tests/controls_model_test tests/display_geometry_test tests/orientation_test \
             tests/kbd_presence_test tests/chrome_test tests/home_layout_test tests/art_format_test \
             tests/paths_test $(FLEET_TESTS) $(RADAR_TESTS) $(TIMBER_TESTS) \
             $(NOTES_TESTS) $(FILES_TESTS) $(CLOCK_TESTS) $(CAL_TESTS) $(CALC_TESTS) tests/kbd_tca8418_test tests/kbd_bus_k230_test \
             $(WAVE_TESTS) $(RIFT_TESTS) $(CAMERA_TESTS) $(ZABBIX_TESTS)

# Native tests only (they execute binaries).
test: all $(TEST_BINS)
	./tests/airtime_test
	./tests/radiod_tx_test
	./tests/pocketlog_test 2>/dev/null
	./tests/paths_test
	./tests/pocketipc_test
	./tests/pocketsys_test
	./tests/sysd_services_test
	./tests/sysd_logs_test
	./tests/wifi_parse_test
	./tests/wifi_store_test
	./tests/system_view_test
	./tests/diag_view_test
	./tests/settings_view_test
	./tests/theme_test docs/design/themes.json
	./tests/settings_test
	./tests/brightness_test
	./tests/volume_test
	./tests/controls_model_test
	./tests/display_geometry_test
	./tests/orientation_test
	./tests/kbd_presence_test
	./tests/chrome_test
	./tests/home_layout_test
	./tests/art_format_test ui/assets/doors
	./tests/fleet_rng_test
	./tests/fleet_rules_test
	./tests/fleet_ai_test
	./tests/fleet_save_test
	./tests/fleet_theme_test
	./tests/fleet_sha256_test
	./tests/fleet_proto_test
	./tests/fleet_match_test
	./tests/fleet_mp_sim_test
	./tests/fleet_session_test
	./tests/fleet_view_mp_test
	./tests/fleet_link_test
	./tests/radar_rng_test
	./tests/radar_types_test
	./tests/radar_rules_test
	./tests/radar_score_test
	./tests/radar_store_test
	./tests/timber_rng_test
	./tests/timber_types_test
	./tests/timber_tower_test
	./tests/timber_pull_test
	./tests/timber_stability_test
	./tests/timber_score_test
	./tests/timber_collapse_test
	./tests/timber_rules_test
	./tests/timber_replay_test
	./tests/timber_view_test
	./tests/timber_store_test
	./tests/notes_view_test
	./tests/notes_store_test
	./tests/files_fs_test
	TZ=UTC ./tests/files_view_test
	./tests/clock_engine_test
	TZ=UTC ./tests/clock_time_test
	./tests/clock_store_test
	TZ=UTC ./tests/cal_date_test
	./tests/cal_view_test
	./tests/calc_engine_test
	./tests/calc_view_test
	./tests/clock_runtime_test
	./tests/clock_handoff_test
	./tests/clock_restart_test
	./tests/kbd_tca8418_test
	./tests/kbd_bus_k230_test
	./tests/pocketaudio_test
	./tests/wave_view_test
	./tests/wave_session_test tests/fake_pos_wave.sh tests/pos-wave-testhooks
	./tests/wave_modem_test
	./tests/rift_format_test
	./tests/rift_model_test
	./tests/rift_comms_test
	./tests/rift_ipc_test
	./tests/pocketcam_test
	./tests/camera_state_test
	./tests/camera_layout_test
	./tests/camera_session_test tests/pos-camera-testhooks
	./tests/zbx_model_test
	./tests/zbx_proto_test
	./tests/zbx_api_test
	./tests/zbx_config_test
	./tests/zbx_client_test
	TZ=UTC ./tests/zabbix_view_test
	./tests/zabbix_session_test tools/zabbix/pos-zabbix
	bash tests/zabbix_http_test.sh
	bash tests/wave_tool_test.sh
	bash tests/audio_recovery_test.sh
	bash tests/capture_settle_test.sh
	bash tests/wave_lint.sh
	bash tests/kbd_lint.sh
	bash tests/radiod_mock_test.sh
	bash tests/radiod_async_test.sh
	bash tests/radiod_power_test.sh
	bash tests/sysd_test.sh
	bash tests/netd_test.sh
	bash tests/supervise_test.sh
	bash tests/initscript_test.sh
	bash tests/phase3_migration_test.sh
	bash tests/package_sync_test.sh
	bash tests/notices_test.sh
	bash tests/boot_splash_test.sh
	bash tests/brand_mark_test.sh
	bash tests/app_icons_test.sh
	bash tests/doors_ui_assets_test.sh
	bash tests/identity_test.sh
	bash tests/style_lint.sh
	bash tests/build_deps_test.sh
	bash tests/build_outputs_test.sh
	bash tests/required_gates_test.sh
	bash tests/build_provenance_test.sh
	bash tests/provenance_state_test.sh
	bash tests/image_contents_test.sh
	bash tests/deploy_staging_test.sh
	bash tests/splash_image_test.sh
	bash tests/hwcheck_test.sh
	bash tests/fleet_lint.sh
	bash tests/radar_lint.sh
	bash tests/timber_lint.sh
	bash tests/notes_lint.sh
	bash tests/files_lint.sh
	bash tests/clock_lint.sh
	bash tests/calendar_lint.sh
	bash tests/calculator_lint.sh
	bash tests/settings_lint.sh
	bash tests/system_lint.sh
	bash tests/rift_lint.sh
	bash tests/camera_lint.sh
	bash tests/zabbix_lint.sh

install: all meshcored-shipping-check
# The command-line tool is installed as doors, and pos is a symlink to it: one
# binary under two names, pos a permanent alias (ADR-005 decision 5). Invoked
# as pos it prints what pos always printed (tools/pos/pos_cli.h). The build
# output keeps its name, tools/pos/pos, like the pos-* helpers keep theirs.
# ln -f replaces the regular file an earlier install left under the old name.
	install -D -m 0755 tools/pos/pos $(DESTDIR)$(PREFIX)/bin/doors
	ln -sfn doors $(DESTDIR)$(PREFIX)/bin/pos
	install -D -m 0755 tools/hwcheck/hwcheck.sh $(DESTDIR)$(PREFIX)/bin/pos-hwcheck
	install -D -m 0755 tools/hwcheck/pos-spixfer $(DESTDIR)$(PREFIX)/bin/pos-spixfer
	install -D -m 0755 tools/wave/pos-wave $(DESTDIR)$(PREFIX)/bin/pos-wave
	install -D -m 0755 tools/camera/pos-camera $(DESTDIR)$(PREFIX)/bin/pos-camera
	install -D -m 0755 tools/zabbix/pos-zabbix $(DESTDIR)$(PREFIX)/bin/pos-zabbix
	install -D -m 0755 services/radiod/radiod $(DESTDIR)$(PREFIX)/sbin/radiod
	install -D -m 0755 services/sysd/sysd $(DESTDIR)$(PREFIX)/sbin/sysd
	install -D -m 0755 services/netd/netd $(DESTDIR)$(PREFIX)/sbin/netd
	install -D -m 0755 tools/supervise/pos-supervise $(DESTDIR)$(PREFIX)/bin/pos-supervise
# meshcored only when it was built. Installing it does not start it: the init
# script ships disabled and has to be switched on per unit, because starting
# it acquires the radio (docs/services/MESHCORED.md).
ifeq ($(ENABLE_MESHCORED),1)
	install -D -m 0755 services/meshcored/meshcored $(DESTDIR)$(PREFIX)/sbin/meshcored
endif
# The third-party notices travel with the binaries that need them. Under
# share/doors rather than share/doc, which Buildroot strips from the target.
# share/pocketos, where they were through v0.0.9, stays a directory holding a
# symlink to them (ADR-005 Phase 2): the old path still reads the same file,
# and a bench deploy over a PocketOS-era unit replaces the copy it had there
# rather than leaving it to go stale. The directory itself is not turned into
# a link, which BusyBox tar could not unpack over the existing directory.
	install -D -m 0644 THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/doors/THIRD_PARTY_NOTICES.txt
	install -d -m 0755 $(DESTDIR)$(PREFIX)/share/pocketos
	ln -sfn ../doors/THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/pocketos/THIRD_PARTY_NOTICES.txt
# The DOORS shell's runtime art (ui/shell/art.h): backgrounds and launcher
# icons, read by the shell from files so only the screen in front is in
# memory. The design sources stay in docs/ and never reach the image.
	install -d -m 0755 $(DESTDIR)$(PREFIX)/share/doors/ui
	install -m 0644 ui/assets/doors/*.bin $(DESTDIR)$(PREFIX)/share/doors/ui/
# /etc/doors-release: line 1 stays the bare version, so every reader that
# takes the first line keeps working, and the build identity follows as a
# key=value line (system.info release_file and release_build, `pos system
# info`). The build id is the same one compiled into the binaries above.
# /etc/pocketos-release, the name every release up to v0.0.9 used, is a
# symlink to it (ADR-005 Phase 2): one file, so the two names cannot disagree.
# Same directory, so the link resolves inside the Buildroot target tree and
# BusyBox tar creates it on the spot during a bench deploy. ln -f replaces the
# regular file an earlier install left there.
	install -d -m 0755 $(DESTDIR)/etc
	printf '%s\nBUILD_ID=%s\n' '$(POCKETOS_VERSION)' '$(POCKETOS_BUILD_ID)' \
		> $(DESTDIR)/etc/doors-release
	chmod 0644 $(DESTDIR)/etc/doors-release
	ln -sfn doors-release $(DESTDIR)/etc/pocketos-release

DEPFILES := $(shell find apps core services tools ui tests $(RADIOLIB_DIR) -name '*.d' 2>/dev/null)
-include $(DEPFILES)

clean:
	$(MAKE) -C tools/meshcore-frame clean
	rm -f $(DEPFILES) $(BINS) $(POS_OBJS) $(RADIOD_OBJS) $(SYSD_OBJS) $(NETD_OBJS) tests/netd_sys_hooks.o tests/netd-testhooks tests/fake_wpa_supplicant tests/fake_wpa_supplicant.o tests/wifi_parse_test tests/wifi_parse_test.o tests/wifi_store_test tests/wifi_store_test.otests/pocketsys_test tests/pocketsys_test.o tests/pocketsys_hooks.o tests/sysd_services_test tests/sysd_services_test.o tests/sysd_logs_test tests/sysd_logs_test.o tests/sysd-testhooks tests/sysd_power_hooks.o tests/system_view_test tests/system_view_test.o apps/system/system_view.o tests/settings_view_test tests/settings_view_test.o apps/settings/settings_view.o$(SX1262_OBJS) $(THEME_OBJS) $(FLEET_OBJS) $(FLEET_NET_OBJS) $(FLEET_LINK_OBJS) apps/fleet/link/fleet_link_mesh.o $(FLEET_VIEW_MP_OBJS) $(FLEET_TESTS) $(FLEET_TESTS:=.o) $(RADAR_OBJS) $(RADAR_APP_OBJS) $(RADAR_TESTS) $(RADAR_TESTS:=.o) tests/airtime_test tests/airtime_test.o tests/pocketlog_test tests/pocketlog_test.o tests/pocketipc_test tests/pocketipc_test.o tests/theme_test tests/theme_test.o tests/settings_test tests/settings_test.o ui/shell/settings.o tests/brightness_test tests/brightness_test.o ui/shell/brightness.o tests/display_geometry_test tests/display_geometry_test.o ui/pocketui/pos_display.o tests/orientation_test tests/orientation_test.o ui/shell/orientation.o ui/shell/kbd_presence.o tests/kbd_presence_test tests/kbd_presence_test.o tests/paths_test tests/paths_test.o $(PATHS_OBJS) tools/hwcheck/spixfer.o $(TIMBER_OBJS) $(TIMBER_TESTS) $(TIMBER_TESTS:=.o) $(NOTES_OBJS) $(NOTES_TESTS) $(NOTES_TESTS:=.o) $(FILES_OBJS) $(FILES_TESTS) $(FILES_TESTS:=.o) $(TIMBER_UI_OBJS) $(CLOCK_OBJS) $(CLOCK_TESTS) $(CLOCK_TESTS:=.o) $(CAL_OBJS) $(CAL_TESTS) $(CAL_TESTS:=.o) $(CALC_OBJS) $(CALC_TESTS) $(CALC_TESTS:=.o) $(POS_WAVE_OBJS) $(WAVE_OBJS) $(WAVE_TESTS) $(WAVE_TESTS:=.o) tests/pos_wave_hooks.o tests/fake_audio_backend.o $(RIFT_OBJS) $(RIFT_TESTS) $(RIFT_TESTS:=.o) tests/fake_meshcored.o tests/fake_meshcored_main.o $(CAM_OBJS) $(CAMERA_OBJS) $(CAMERA_TESTS) $(CAMERA_TESTS:=.o) tests/pos_camera_hooks.o tools/camera/pos_camera.o tests/volume_test tests/volume_test.o ui/shell/volume.o tests/controls_model_test tests/controls_model_test.o ui/shell/controls_model.o apps/system/diag_view.o tests/diag_view_test tests/diag_view_test.o $(ZBX_OBJS) core/zabbix/zbx_http_curl.o core/zabbix/zbx_http_none.o $(ZABBIX_OBJS) $(ZABBIX_TESTS) $(ZABBIX_TESTS:=.o) tools/zabbix/pos_zabbix.o tools/zabbix/pos_zabbix_mock.o $(POCKETOS_BUILD_STAMP)

# The files `make all` and `make test` produce, one to a line, for
# tests/build_outputs_test.sh.
print-build-outputs:
	@printf '%s\n' $(BINS) $(TEST_BINS) $(POCKETOS_BUILD_STAMP)

# meshcore-frame (tools/meshcore-frame): a host-side tool that builds and
# parses MeshCore wire frames from the real MeshCore protocol and crypto
# sources, for proving K230-to-MeshCore interoperability over radiod. It has
# its own Makefile and its own prerequisites - two ignored upstream checkouts
# a Doors build does not need - so it is reached by name and stays out of
# all, test and install. Nothing it produces is installed or reaches an
# image, and its build outputs are git-ignored like every other one here.
meshcore-frame:
	$(MAKE) -C tools/meshcore-frame

# The suite plain, then the same suite under the address and
# undefined-behaviour sanitizers.
meshcore-frame-test:
	$(MAKE) -C tools/meshcore-frame test
	$(MAKE) -C tools/meshcore-frame ASAN=1 test

# protocols/meshcore: the portable MeshCore protocol core (P1A). Reached by
# name for the same reasons meshcore-frame is - it needs the same two ignored
# upstream checkouts a Doors build does not - and, additionally, because
# nothing links it yet. There is no MeshCore service; when there is, it will
# depend on this library and this library will join `all`.
#
# It does not touch radiod, does not define a service, and is not installed.
meshcore-core:
	$(MAKE) -C protocols/meshcore $(MESHCORE_TREES)

# The three suites plain, then the same three under the address and
# undefined-behaviour sanitizers. Both runs end with tests/meshcore_lint.sh,
# which is what keeps the portable boundary from quietly widening.
#
# Then the build-integrity check, once rather than twice: it drives its own
# out-of-tree builds against throwaway clones of the two vendored trees, so
# it is a nested make and belongs here rather than inside the library's own
# `test` target. Nothing it does touches vendor/RIFT, vendor/Crypto or
# protocols/meshcore/build.
meshcore-core-test:
	$(MAKE) -C protocols/meshcore $(MESHCORE_TREES) test
	$(MAKE) -C protocols/meshcore $(MESHCORE_TREES) ASAN=1 test
	bash tests/meshcore_build_deps_test.sh

# The cross-compile check. The portable core has to build for the board it is
# eventually going to run on, and riscv64 is where `unsigned long` being
# 64-bit stops being an x86-64 coincidence and starts being the thing the
# protocol clock depends on. Object-only: there is nothing to link yet, and
# the tests cannot run here.
#
#   make meshcore-core-riscv64 CROSS=/opt/toolchain/.../bin/riscv64-unknown-linux-gnu-
CROSS ?= riscv64-unknown-linux-gnu-
meshcore-core-riscv64:
	$(MAKE) -C protocols/meshcore $(MESHCORE_TREES) \
	        CC=$(CROSS)gcc CXX=$(CROSS)g++ AR=$(CROSS)ar \
	        CXXFLAGS="-O2 -mcpu=c908v -mtune=c908" OBJDIR=build-riscv64 \
	        LIB=libmeshcore-riscv64.a

# ---- meshcored's own suite ------------------------------------------------
#
# Reached by name for the same reason the service is built by name: it needs
# the two upstream checkouts. Nothing here is in `make all` or `make test`.
#
#   make meshcored             build the service
#   make meshcored-test        its tests, plain then sanitised, then the lint
#                              and the two host integration suites
#
# The riscv64 check is the ordinary cross-build with the switch on, in a copy
# of the tree (docs/BUILD_ENVIRONMENT.md; objects land beside their sources,
# so a cross-build in the working checkout would clobber the host ones):
#
#   make ENABLE_MESHCORED=1 CC=<cross>gcc CXX=<cross>g++ AR=<cross>ar \
#        CFLAGS="--sysroot=$$SYSROOT -O2 -Wall -Wextra" \
#        CXXFLAGS="--sysroot=$$SYSROOT -O2" LDFLAGS="--sysroot=$$SYSROOT" all
#
# meshcore-lib is a prerequisite of the binary and forwards CC/CXX/AR, so
# that one line builds libmeshcore.a for the target too.
meshcored: meshcore-lib services/meshcored/meshcored

MESHCORED_SAN := -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -fno-sanitize-recover=all -g
MESHCORE_LIB_ASAN := $(MESHCORE_DIR)/libmeshcore-asan.a
MESHCORED_TESTS := tests/meshcored_util_test tests/meshcored_txmap_test \
                   tests/meshcored_store_test tests/meshcored_runtime_test
MESHCORED_TESTS_ASAN := $(addsuffix -asan,$(MESHCORED_TESTS))

.PHONY: meshcore-lib-asan
meshcore-lib-asan:
	$(MAKE) -C $(MESHCORE_DIR) ASAN=1 CC="$(CC)" CXX="$(CXX)" AR="$(AR)" CXXFLAGS="$(CXXFLAGS)" \
	        $(MESHCORE_TREES)

tests/meshcored_util_test: tests/meshcored_util_test.o services/meshcored/mcd_util.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/meshcored_txmap_test: tests/meshcored_txmap_test.o services/meshcored/tx_map.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/meshcored_util_test.o tests/meshcored_txmap_test.o: ALL_CFLAGS += -Iservices/meshcored

# The store's own suite links a SECOND build of mesh_store.cpp, the one with
# the directory-fsync hook. A directory flush does not fail on a working
# filesystem, so without it that error path would never be executed and its
# handling would be a guess. The shipped object does not carry the hook and
# tests/meshcored_lint.sh checks that - the same arrangement sysd, netd,
# pos-wave and protocols/meshcore's RNG already use.
tests/meshcored_store_hooks.o: services/meshcored/mesh_store.cpp
	$(CXX) $(MESHCORED_CXXFLAGS) -DMCD_STORE_TEST_HOOKS=1 -c -o $@ $<

tests/meshcored_store_test: meshcore-lib tests/meshcored_store_test.o tests/meshcored_store_hooks.o
	$(CXX) $(CXXFLAGS) -o $@ tests/meshcored_store_test.o tests/meshcored_store_hooks.o \
	       $(MESHCORE_LIB) $(LDFLAGS)

tests/meshcored_runtime_test: meshcore-lib tests/meshcored_runtime_test.o \
                              services/meshcored/mesh_runtime.o services/meshcored/mesh_store.o \
                              services/meshcored/mcd_util.o services/radiod/airtime.o
	$(CXX) $(CXXFLAGS) -o $@ tests/meshcored_runtime_test.o \
	       services/meshcored/mesh_runtime.o services/meshcored/mesh_store.o \
	       services/meshcored/mcd_util.o services/radiod/airtime.o \
	       $(MESHCORE_LIB) $(LDFLAGS)

tests/meshcored_store_test.o: tests/meshcored_store_test.cpp
	$(CXX) $(MESHCORED_CXXFLAGS) -DMCD_STORE_TEST_HOOKS=1 -c -o $@ $<

tests/meshcored_runtime_test.o: tests/meshcored_runtime_test.cpp
	$(CXX) $(MESHCORED_CXXFLAGS) -c -o $@ $<

# The sanitised builds compile from source into their own binaries rather
# than reusing the objects above, so the two builds can never share a stale
# one. ed25519's signed-limb shifts keep their narrow UBSan exemption inside
# protocols/meshcore, which builds its own sanitised library.
tests/meshcored_util_test-asan: tests/meshcored_util_test.c services/meshcored/mcd_util.c
	$(CC) $(ALL_CFLAGS) $(MESHCORED_SAN) -Iservices/meshcored -o $@ $^ $(LDFLAGS)

tests/meshcored_txmap_test-asan: tests/meshcored_txmap_test.c services/meshcored/tx_map.c
	$(CC) $(ALL_CFLAGS) $(MESHCORED_SAN) -Iservices/meshcored -o $@ $^ $(LDFLAGS)

tests/meshcored_airtime_asan.o: services/radiod/airtime.c
	$(CC) $(ALL_CFLAGS) $(MESHCORED_SAN) -Iservices/radiod -c -o $@ $<

tests/meshcored_util_asan.o: services/meshcored/mcd_util.c
	$(CC) $(ALL_CFLAGS) $(MESHCORED_SAN) -Iservices/meshcored -c -o $@ $<

tests/meshcored_store_test-asan: meshcore-lib-asan tests/meshcored_store_test.cpp \
                                 services/meshcored/mesh_store.cpp
	$(CXX) $(MESHCORED_CXXFLAGS) -DMCD_STORE_TEST_HOOKS=1 $(MESHCORED_SAN) -o $@ \
	       tests/meshcored_store_test.cpp services/meshcored/mesh_store.cpp \
	       $(MESHCORE_LIB_ASAN) $(LDFLAGS)

tests/meshcored_runtime_test-asan: meshcore-lib-asan tests/meshcored_runtime_test.cpp \
                                   services/meshcored/mesh_runtime.cpp \
                                   services/meshcored/mesh_store.cpp \
                                   tests/meshcored_airtime_asan.o \
                                   tests/meshcored_util_asan.o
	$(CXX) $(MESHCORED_CXXFLAGS) $(MESHCORED_SAN) -o $@ \
	       tests/meshcored_runtime_test.cpp services/meshcored/mesh_runtime.cpp \
	       services/meshcored/mesh_store.cpp tests/meshcored_airtime_asan.o \
	       tests/meshcored_util_asan.o $(MESHCORE_LIB_ASAN) $(LDFLAGS)

# LSAN_OPTIONS points at protocols/meshcore's suppression file, which
# suppresses exactly two vendored constructors: StaticPoolPacketManager and
# PacketQueue allocate with `new` and never free, which is harmless for a
# pool that lives as long as the process and is recorded as upstream debt.
MESHCORED_LSAN := suppressions=$(CURDIR)/$(MESHCORE_DIR)/lsan.supp

meshcored-test: meshcored $(MESHCORED_TESTS) $(MESHCORED_TESTS_ASAN)
	./tests/meshcored_util_test
	./tests/meshcored_txmap_test
	./tests/meshcored_store_test
	./tests/meshcored_runtime_test
	LSAN_OPTIONS=$(MESHCORED_LSAN) ./tests/meshcored_util_test-asan
	LSAN_OPTIONS=$(MESHCORED_LSAN) ./tests/meshcored_txmap_test-asan
	LSAN_OPTIONS=$(MESHCORED_LSAN) ./tests/meshcored_store_test-asan
	LSAN_OPTIONS=$(MESHCORED_LSAN) ./tests/meshcored_runtime_test-asan
	bash tests/meshcored_lint.sh
	bash tests/meshcored_source_identity_test.sh
	bash tests/meshcored_service_test.sh
	bash tests/meshcored_harness_test.sh

meshcored-clean:
	rm -f $(MESHCORED_C_OBJS) $(MESHCORED_CXX_OBJS) services/meshcored/meshcored \
	      $(MESHCORED_TESTS) $(MESHCORED_TESTS_ASAN) $(MESHCORED_TESTS:=.o) \
	      tests/meshcored_airtime_asan.o tests/meshcored_util_asan.o \
	      tests/meshcored_store_hooks.o \
	      services/meshcored/*.d tests/meshcored_*.d

.PHONY: all test install clean sx1262-objs print-build-outputs \
        meshcore-frame meshcore-frame-test \
        meshcore-core meshcore-core-test meshcore-core-riscv64 \
        meshcored meshcored-test meshcored-clean meshcored-shipping-check
