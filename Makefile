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
POCKETOS_VERSION := $(shell cat VERSION)
# Build identity. In the Buildroot package the source tree has no git history,
# so apply_to_sdk.sh writes BUILD_ID beside VERSION when it exports the tree;
# in a working checkout it comes from git. A build that has neither says so.
POCKETOS_BUILD_ID := $(shell cat BUILD_ID 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
COMMON_FLAGS := -Wall -Wextra -Icore -DPOCKETOS_VERSION=\"$(POCKETOS_VERSION)\" \
                -DPOCKETOS_BUILD_ID=\"$(POCKETOS_BUILD_ID)\"
# Compiler-generated header dependencies (.d next to each .o) so a changed
# header rebuilds every object that includes it (PocketFleet finding 1).
DEPFLAGS := -MMD -MP
ALL_CFLAGS := $(CFLAGS) -std=gnu11 $(COMMON_FLAGS) $(DEPFLAGS)
ALL_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(COMMON_FLAGS) -Iservices/radiod -I$(RADIOLIB_DIR) -DRADIOLIB_LOW_LEVEL=1 $(DEPFLAGS)

PATHS_OBJS  := core/pocketpaths.o
IPC_OBJS    := core/pocketipc/pocketipc.o
LOG_OBJS    := core/pocketlog/pocketlog.o
SYS_OBJS    := core/pocketsys.o
POS_OBJS    := tools/pos/pos.o tools/pos/pos_radio.o tools/pos/pos_logs.o tools/pos/pos_app.o tools/pos/pos_system.o $(IPC_OBJS) $(PATHS_OBJS)
RADIOD_OBJS := services/radiod/main.o services/radiod/backend_mock.o services/radiod/airtime.o $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
# Everything sysd is except the power actions, which exist twice: once as
# shipped and once with the test hook (see tests/sysd-testhooks below).
SYSD_BASE_OBJS := services/sysd/main.o services/sysd/sysd_services.o $(SYS_OBJS) $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)
SYSD_OBJS   := $(SYSD_BASE_OBJS) services/sysd/sysd_power.o

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

BINS := tools/pos/pos services/radiod/radiod services/sysd/sysd tools/hwcheck/pos-spixfer

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

tools/pos/pos.o: VERSION

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

# Every binary `make test` builds on top of $(BINS). Each of them, and each of
# $(BINS), has to be git-ignored: apply_to_sdk.sh calls an image's BUILD_ID
# "<commit>-dirty" when `git status --porcelain` shows anything, so a single
# untracked test binary in the checkout an image is built from would make it
# claim changes it does not contain. tests/build_outputs_test.sh checks this
# list against .gitignore, so a test added here without an entry there fails.
TEST_BINS := tests/sysd-testhooks tests/airtime_test tests/pocketlog_test tests/pocketipc_test \
             tests/pocketsys_test tests/sysd_services_test tests/system_view_test tests/theme_test \
             tests/settings_test tests/paths_test $(FLEET_TESTS) $(RADAR_TESTS) $(TIMBER_TESTS) \
             $(NOTES_TESTS) $(CLOCK_TESTS)

# Native tests only (they execute binaries).
test: all $(TEST_BINS)
	./tests/airtime_test
	./tests/pocketlog_test 2>/dev/null
	./tests/paths_test
	./tests/pocketipc_test
	./tests/pocketsys_test
	./tests/sysd_services_test
	./tests/system_view_test
	./tests/theme_test docs/design/themes.json
	./tests/settings_test
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
	./tests/clock_runtime_test
	bash tests/radiod_mock_test.sh
	bash tests/sysd_test.sh
	bash tests/supervise_test.sh
	bash tests/initscript_test.sh
	bash tests/package_sync_test.sh
	bash tests/style_lint.sh
	bash tests/build_deps_test.sh
	bash tests/build_outputs_test.sh
	bash tests/hwcheck_test.sh
	bash tests/fleet_lint.sh
	bash tests/radar_lint.sh
	bash tests/timber_lint.sh
	bash tests/notes_lint.sh
	bash tests/clock_lint.sh

install: all
	install -D -m 0755 tools/pos/pos $(DESTDIR)$(PREFIX)/bin/pos
	install -D -m 0755 tools/hwcheck/hwcheck.sh $(DESTDIR)$(PREFIX)/bin/pos-hwcheck
	install -D -m 0755 tools/hwcheck/pos-spixfer $(DESTDIR)$(PREFIX)/bin/pos-spixfer
	install -D -m 0755 services/radiod/radiod $(DESTDIR)$(PREFIX)/sbin/radiod
	install -D -m 0755 services/sysd/sysd $(DESTDIR)$(PREFIX)/sbin/sysd
	install -D -m 0755 tools/supervise/pos-supervise $(DESTDIR)$(PREFIX)/bin/pos-supervise
# /etc/pocketos-release: line 1 stays the bare version, so every reader that
# takes the first line keeps working, and the build identity follows as a
# key=value line (system.info release_file and release_build, `pos system
# info`). The build id is the same one compiled into the binaries above.
	install -d -m 0755 $(DESTDIR)/etc
	printf '%s\nBUILD_ID=%s\n' '$(POCKETOS_VERSION)' '$(POCKETOS_BUILD_ID)' \
		> $(DESTDIR)/etc/pocketos-release
	chmod 0644 $(DESTDIR)/etc/pocketos-release

DEPFILES := $(shell find apps core services tools ui tests $(RADIOLIB_DIR) -name '*.d' 2>/dev/null)
-include $(DEPFILES)

clean:
	rm -f $(DEPFILES) $(BINS) $(POS_OBJS) $(RADIOD_OBJS) $(SYSD_OBJS) tests/pocketsys_test tests/pocketsys_test.o tests/pocketsys_hooks.o tests/sysd_services_test tests/sysd_services_test.o tests/sysd-testhooks tests/sysd_power_hooks.o tests/system_view_test tests/system_view_test.o apps/system/system_view.o $(SX1262_OBJS) $(THEME_OBJS) $(FLEET_OBJS) $(FLEET_TESTS) $(FLEET_TESTS:=.o) $(RADAR_OBJS) $(RADAR_APP_OBJS) $(RADAR_TESTS) $(RADAR_TESTS:=.o) tests/airtime_test tests/airtime_test.o tests/pocketlog_test tests/pocketlog_test.o tests/pocketipc_test tests/pocketipc_test.o tests/theme_test tests/theme_test.o tests/settings_test tests/settings_test.o ui/shell/settings.o tests/paths_test tests/paths_test.o $(PATHS_OBJS) tools/hwcheck/spixfer.o $(TIMBER_OBJS) $(TIMBER_TESTS) $(TIMBER_TESTS:=.o) $(NOTES_OBJS) $(NOTES_TESTS) $(NOTES_TESTS:=.o) $(TIMBER_UI_OBJS) $(CLOCK_OBJS) $(CLOCK_TESTS) $(CLOCK_TESTS:=.o)

# The files `make all` and `make test` produce, one to a line, for
# tests/build_outputs_test.sh.
print-build-outputs:
	@printf '%s\n' $(BINS) $(TEST_BINS)

.PHONY: all test install clean sx1262-objs print-build-outputs
