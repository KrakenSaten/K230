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
POS_OBJS    := tools/pos/pos.o tools/pos/pos_radio.o tools/pos/pos_logs.o tools/pos/pos_app.o $(IPC_OBJS) $(PATHS_OBJS)
RADIOD_OBJS := services/radiod/main.o services/radiod/backend_mock.o services/radiod/airtime.o $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS) $(PATHS_OBJS)

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

BINS := tools/pos/pos services/radiod/radiod

all: $(BINS)

tools/pos/pos: $(POS_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

services/radiod/radiod: $(RADIOD_OBJS)
	$(RADIOD_LINK) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS) $(RADIOD_LIBS)

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
                tests/timber_replay_test tests/timber_view_test

$(TIMBER_DIR)/%.o: $(TIMBER_DIR)/%.c
	$(CC) $(ALL_CFLAGS) -I$(TIMBER_DIR) -c -o $@ $<

tests/timber_%_test.o: tests/timber_%_test.c
	$(CC) $(ALL_CFLAGS) -I$(TIMBER_DIR) -c -o $@ $<

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

# Native tests only (they execute binaries).
test: all tests/airtime_test tests/pocketlog_test tests/pocketipc_test tests/theme_test tests/settings_test tests/paths_test $(FLEET_TESTS) $(RADAR_TESTS) $(TIMBER_TESTS)
	./tests/airtime_test
	./tests/pocketlog_test 2>/dev/null
	./tests/paths_test
	./tests/pocketipc_test
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
	bash tests/radiod_mock_test.sh
	bash tests/supervise_test.sh
	bash tests/initscript_test.sh
	bash tests/package_sync_test.sh
	bash tests/style_lint.sh
	bash tests/build_deps_test.sh
	bash tests/hwcheck_test.sh
	bash tests/fleet_lint.sh
	bash tests/radar_lint.sh
	bash tests/timber_lint.sh

install: all
	install -D -m 0755 tools/pos/pos $(DESTDIR)$(PREFIX)/bin/pos
	install -D -m 0755 tools/hwcheck/hwcheck.sh $(DESTDIR)$(PREFIX)/bin/pos-hwcheck
	install -D -m 0755 services/radiod/radiod $(DESTDIR)$(PREFIX)/sbin/radiod
	install -D -m 0755 tools/supervise/pos-supervise $(DESTDIR)$(PREFIX)/bin/pos-supervise
	install -D -m 0644 VERSION $(DESTDIR)/etc/pocketos-release

DEPFILES := $(shell find apps core services tools ui tests $(RADIOLIB_DIR) -name '*.d' 2>/dev/null)
-include $(DEPFILES)

clean:
	rm -f $(DEPFILES) $(BINS) $(POS_OBJS) $(RADIOD_OBJS) $(SX1262_OBJS) $(THEME_OBJS) $(FLEET_OBJS) $(FLEET_TESTS) $(FLEET_TESTS:=.o) $(RADAR_OBJS) $(RADAR_APP_OBJS) $(RADAR_TESTS) $(RADAR_TESTS:=.o) tests/airtime_test tests/airtime_test.o tests/pocketlog_test tests/pocketlog_test.o tests/pocketipc_test tests/pocketipc_test.o tests/theme_test tests/theme_test.o tests/settings_test tests/settings_test.o ui/shell/settings.o tests/paths_test tests/paths_test.o $(PATHS_OBJS) $(TIMBER_OBJS) $(TIMBER_TESTS) $(TIMBER_TESTS:=.o) $(TIMBER_UI_OBJS)

.PHONY: all test install clean sx1262-objs
