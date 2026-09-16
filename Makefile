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
RADIOD_OBJS := services/radiod/main.o services/radiod/backend_mock.o services/radiod/airtime.o $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
# Everything sysd is except the power actions, which exist twice: once as
# shipped and once with the test hook (see tests/sysd-testhooks below).
SYSD_BASE_OBJS := services/sysd/main.o services/sysd/sysd_services.o $(SYS_OBJS) $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
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

BINS := tools/pos/pos services/radiod/radiod services/sysd/sysd services/netd/netd tools/hwcheck/pos-spixfer \
        tools/wave/pos-wave

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
POCKETOS_ID_OBJS := core/pocketlog/pocketlog.o tools/pos/pos.o tests/pocketlog_test.o
$(POCKETOS_ID_OBJS): $(POCKETOS_BUILD_STAMP)

# Compile-only check of the sx1262 backend on a host without libgpiod v2
# (GPIOD_INCLUDE points at vendor/libgpiod/include).
sx1262-objs: ALL_CXXFLAGS += -DPOCKETOS_HAVE_SX1262 $(if $(GPIOD_INCLUDE),-I$(GPIOD_INCLUDE),)
sx1262-objs: $(SX1262_OBJS)

tests/airtime_test: tests/airtime_test.o services/radiod/airtime.o
	$(CC) $(ALL_CFLAGS) -Iservices/radiod -o $@ $^ $(LDFLAGS) -lm

tests/airtime_test.o: tests/airtime_test.c
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
	$(CC) $(ALL_CFLAGS) -I$(FLEET_DIR) -c -o $@ $<

tests/fleet_rng_test: tests/fleet_rng_test.o $(FLEET_DIR)/fleet_rng.o
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_rules_test: tests/fleet_rules_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_ai_test: tests/fleet_ai_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

tests/fleet_save_test: tests/fleet_save_test.o $(FLEET_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

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
               tests/clock_runtime_test

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

# Every binary `make test` builds on top of $(BINS). Each of them, and each of
# $(BINS), has to be git-ignored: apply_to_sdk.sh calls an image's BUILD_ID
# "<commit>-dirty" when `git status --porcelain` shows anything, so a single
# untracked test binary in the checkout an image is built from would make it
# claim changes it does not contain. tests/build_outputs_test.sh checks this
# list against .gitignore, so a test added here without an entry there fails.
TEST_BINS := tests/sysd-testhooks tests/netd-testhooks tests/fake_wpa_supplicant tests/wifi_parse_test \
             tests/wifi_store_test tests/airtime_test tests/pocketlog_test tests/pocketipc_test \
             tests/pocketsys_test tests/sysd_services_test tests/system_view_test tests/settings_view_test \
             tests/theme_test \
             tests/settings_test tests/brightness_test tests/display_geometry_test tests/orientation_test \
             tests/kbd_presence_test \
             tests/paths_test $(FLEET_TESTS) $(RADAR_TESTS) $(TIMBER_TESTS) \
             $(NOTES_TESTS) $(CLOCK_TESTS) $(CAL_TESTS) $(CALC_TESTS) tests/kbd_tca8418_test tests/kbd_bus_k230_test \
             $(WAVE_TESTS)

# Native tests only (they execute binaries).
test: all $(TEST_BINS)
	./tests/airtime_test
	./tests/pocketlog_test 2>/dev/null
	./tests/paths_test
	./tests/pocketipc_test
	./tests/pocketsys_test
	./tests/sysd_services_test
	./tests/wifi_parse_test
	./tests/wifi_store_test
	./tests/system_view_test
	./tests/settings_view_test
	./tests/theme_test docs/design/themes.json
	./tests/settings_test
	./tests/brightness_test
	./tests/display_geometry_test
	./tests/orientation_test
	./tests/kbd_presence_test
	./tests/fleet_rng_test
	./tests/fleet_rules_test
	./tests/fleet_ai_test
	./tests/fleet_save_test
	./tests/fleet_theme_test
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
	./tests/clock_engine_test
	TZ=UTC ./tests/clock_time_test
	./tests/clock_store_test
	TZ=UTC ./tests/cal_date_test
	./tests/cal_view_test
	./tests/calc_engine_test
	./tests/calc_view_test
	./tests/clock_runtime_test
	./tests/kbd_tca8418_test
	./tests/kbd_bus_k230_test
	./tests/pocketaudio_test
	./tests/wave_view_test
	./tests/wave_session_test tests/fake_pos_wave.sh tests/pos-wave-testhooks
	./tests/wave_modem_test
	bash tests/wave_tool_test.sh
	bash tests/audio_recovery_test.sh
	bash tests/capture_settle_test.sh
	bash tests/wave_lint.sh
	bash tests/kbd_lint.sh
	bash tests/radiod_mock_test.sh
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
	bash tests/identity_test.sh
	bash tests/style_lint.sh
	bash tests/build_deps_test.sh
	bash tests/build_outputs_test.sh
	bash tests/required_gates_test.sh
	bash tests/build_provenance_test.sh
	bash tests/image_contents_test.sh
	bash tests/splash_image_test.sh
	bash tests/hwcheck_test.sh
	bash tests/fleet_lint.sh
	bash tests/radar_lint.sh
	bash tests/timber_lint.sh
	bash tests/notes_lint.sh
	bash tests/clock_lint.sh
	bash tests/calendar_lint.sh
	bash tests/calculator_lint.sh
	bash tests/settings_lint.sh

install: all
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
	install -D -m 0755 services/radiod/radiod $(DESTDIR)$(PREFIX)/sbin/radiod
	install -D -m 0755 services/sysd/sysd $(DESTDIR)$(PREFIX)/sbin/sysd
	install -D -m 0755 services/netd/netd $(DESTDIR)$(PREFIX)/sbin/netd
	install -D -m 0755 tools/supervise/pos-supervise $(DESTDIR)$(PREFIX)/bin/pos-supervise
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
	rm -f $(DEPFILES) $(BINS) $(POS_OBJS) $(RADIOD_OBJS) $(SYSD_OBJS) $(NETD_OBJS) tests/netd_sys_hooks.o tests/netd-testhooks tests/fake_wpa_supplicant tests/fake_wpa_supplicant.o tests/wifi_parse_test tests/wifi_parse_test.o tests/wifi_store_test tests/wifi_store_test.otests/pocketsys_test tests/pocketsys_test.o tests/pocketsys_hooks.o tests/sysd_services_test tests/sysd_services_test.o tests/sysd-testhooks tests/sysd_power_hooks.o tests/system_view_test tests/system_view_test.o apps/system/system_view.o tests/settings_view_test tests/settings_view_test.o apps/settings/settings_view.o$(SX1262_OBJS) $(THEME_OBJS) $(FLEET_OBJS) $(FLEET_TESTS) $(FLEET_TESTS:=.o) $(RADAR_OBJS) $(RADAR_APP_OBJS) $(RADAR_TESTS) $(RADAR_TESTS:=.o) tests/airtime_test tests/airtime_test.o tests/pocketlog_test tests/pocketlog_test.o tests/pocketipc_test tests/pocketipc_test.o tests/theme_test tests/theme_test.o tests/settings_test tests/settings_test.o ui/shell/settings.o tests/brightness_test tests/brightness_test.o ui/shell/brightness.o tests/display_geometry_test tests/display_geometry_test.o ui/pocketui/pos_display.o tests/orientation_test tests/orientation_test.o ui/shell/orientation.o ui/shell/kbd_presence.o tests/kbd_presence_test tests/kbd_presence_test.o tests/paths_test tests/paths_test.o $(PATHS_OBJS) tools/hwcheck/spixfer.o $(TIMBER_OBJS) $(TIMBER_TESTS) $(TIMBER_TESTS:=.o) $(NOTES_OBJS) $(NOTES_TESTS) $(NOTES_TESTS:=.o) $(TIMBER_UI_OBJS) $(CLOCK_OBJS) $(CLOCK_TESTS) $(CLOCK_TESTS:=.o) $(CAL_OBJS) $(CAL_TESTS) $(CAL_TESTS:=.o) $(CALC_OBJS) $(CALC_TESTS) $(CALC_TESTS:=.o) $(POS_WAVE_OBJS) $(WAVE_OBJS) $(WAVE_TESTS) $(WAVE_TESTS:=.o) tests/pos_wave_hooks.o tests/fake_audio_backend.o $(POCKETOS_BUILD_STAMP)

# The files `make all` and `make test` produce, one to a line, for
# tests/build_outputs_test.sh.
print-build-outputs:
	@printf '%s\n' $(BINS) $(TEST_BINS) $(POCKETOS_BUILD_STAMP)

.PHONY: all test install clean sx1262-objs print-build-outputs
