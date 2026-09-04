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
COMMON_FLAGS := -Wall -Wextra -Icore -DPOCKETOS_VERSION=\"$(POCKETOS_VERSION)\"
ALL_CFLAGS := $(CFLAGS) -std=gnu11 $(COMMON_FLAGS)
ALL_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(COMMON_FLAGS) -Iservices/radiod -I$(RADIOLIB_DIR) -DRADIOLIB_LOW_LEVEL=1

IPC_OBJS    := core/pocketipc/pocketipc.o
LOG_OBJS    := core/pocketlog/pocketlog.o
POS_OBJS    := tools/pos/pos.o tools/pos/pos_radio.o tools/pos/pos_logs.o tools/pos/pos_app.o $(IPC_OBJS)
RADIOD_OBJS := services/radiod/main.o services/radiod/backend_mock.o services/radiod/airtime.o $(IPC_OBJS) core/pocketipc/server.o $(LOG_OBJS)

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

tests/pocketlog_test: tests/pocketlog_test.o $(LOG_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(LDFLAGS)

# Native tests only (they execute binaries).
test: all tests/airtime_test tests/pocketlog_test
	./tests/airtime_test
	./tests/pocketlog_test 2>/dev/null
	bash tests/radiod_mock_test.sh
	bash tests/supervise_test.sh

install: all
	install -D -m 0755 tools/pos/pos $(DESTDIR)$(PREFIX)/bin/pos
	install -D -m 0755 tools/hwcheck/hwcheck.sh $(DESTDIR)$(PREFIX)/bin/pos-hwcheck
	install -D -m 0755 services/radiod/radiod $(DESTDIR)$(PREFIX)/sbin/radiod
	install -D -m 0755 tools/supervise/pos-supervise $(DESTDIR)$(PREFIX)/bin/pos-supervise
	install -D -m 0644 VERSION $(DESTDIR)/etc/pocketos-release

clean:
	rm -f $(BINS) $(POS_OBJS) $(RADIOD_OBJS) $(SX1262_OBJS) tests/airtime_test tests/airtime_test.o tests/pocketlog_test tests/pocketlog_test.o

.PHONY: all test install clean sx1262-objs
