# meshcore-frame: a host tool, built on its own.
#
# It is deliberately not part of `make all` or `make test` at the top of the
# repository. Nothing it produces is installed, nothing it produces reaches an
# image, and it needs two upstream checkouts a Doors build does not:
# vendor/RIFT (the MeshCore protocol code) and vendor/Crypto (the cipher, hash
# and signature library MeshCore itself uses). Making a Doors build depend on
# those would be a poor trade for a bench tool.
#
# From the top of the repository:
#
#   make meshcore-frame        build the tool
#   make meshcore-frame-test   build it and run its tests, plain and sanitised
#
# Prerequisites, as ignored checkouts at the pinned commits beside this file
# (the convention vendor/RadioLib and vendor/ggwave already follow) - see
# README.md for the two clone commands.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0

CC       ?= cc
CXX      ?= c++
CXXFLAGS ?= -O2
LDFLAGS  ?=

REPO       := ../..
RIFT_DIR   ?= $(REPO)/vendor/RIFT
CRYPTO_REPO ?= $(REPO)/vendor/Crypto
CRYPTO_DIR := $(CRYPTO_REPO)/libraries/Crypto

RIFT_COMMIT   := $(shell tr -d ' \t\r\n' < vendor_rift_commit.txt)
CRYPTO_COMMIT := $(shell tr -d ' \t\r\n' < vendor_crypto_commit.txt)

# ASAN=1 adds the address and undefined-behaviour sanitizers, into their own
# object and binary names so the two builds never share a stale object. The
# test target runs the suite both ways.
ASAN ?= 0
ifeq ($(ASAN),1)
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g
# orlp's ed25519, the implementation MeshCore signs with, does its field and
# scalar arithmetic by left-shifting signed 64-bit limbs that are often
# negative. That is formally undefined in C and universally an arithmetic
# shift in practice; every ref10-derived Ed25519 is written this way, and it
# is what runs on a real MeshCore node. UBSan's shift-base check is therefore
# switched off for those files alone - about forty sites in sc.c and fe.c,
# and nowhere else in this build. Everything else, ours and vendored, keeps
# the full address and undefined-behaviour checks.
ED25519_SAN_FLAGS := -fno-sanitize=shift-base
SUFFIX    := -asan
else
SAN_FLAGS :=
ED25519_SAN_FLAGS :=
SUFFIX    :=
endif

OBJDIR := build$(SUFFIX)

# -Wall -Wextra, as the rest of the repository builds. They are applied to our
# own sources only: the vendored trees are third-party code compiled as their
# authors wrote it, and silencing or "fixing" their warnings here would be an
# edit to somebody else's protocol implementation.
#
# The vendored trees are reached with -isystem rather than -I for the same
# reason. Their headers are included by our sources, and without it every one
# of our translation units would repeat their warnings - a wall of noise in
# which a real warning about our own code would not be noticed.
WARN_FLAGS := -Wall -Wextra
INCLUDES   := -I. -Icompat -isystem $(RIFT_DIR)/src -isystem $(RIFT_DIR)/lib/ed25519 \
              -isystem $(CRYPTO_DIR)
DEFINES    := -DMESHCORE_FRAME_RIFT_COMMIT=\"$(RIFT_COMMIT)\" \
              -DMESHCORE_FRAME_CRYPTO_COMMIT=\"$(CRYPTO_COMMIT)\"
ALL_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(SAN_FLAGS) $(INCLUDES) $(DEFINES) -MMD -MP
ALL_CFLAGS   := $(CXXFLAGS) -std=gnu11 $(SAN_FLAGS) $(INCLUDES) -MMD -MP

# ---- what is compiled from the vendored trees -----------------------------
# MeshCore's wire format and crypto, and nothing else from RIFT: no radio, no
# dispatcher, no mesh routing, no UI, no board support. Mesh.h and
# Dispatcher.h come along as headers, because AdvertDataHelpers.h includes
# Mesh.h; neither .cpp is built.
RIFT_CXX := Packet.cpp Utils.cpp Identity.cpp helpers/AdvertDataHelpers.cpp

# orlp's ed25519, as vendored by RIFT: the signing, the key pair and the
# X25519 agreement MeshCore uses. seed.c is left out (this tool takes its
# randomness from the host, see mcf_util.cpp), and so are verify.c, which
# MeshCore does not call, and add_scalar.c, which nothing here calls.
ED25519_C := keypair.c sign.c key_exchange.c fe.c ge.c sc.c sha512.c

# rweather/Crypto, the library MeshCore's own library.json depends on: AES128
# for the cipher, SHA256 for the hashes and the HMAC, Ed25519 for signature
# verification (Curve25519 and BigNumberUtil are what Ed25519 is built on).
CRYPTO_CXX := Crypto.cpp Hash.cpp BlockCipher.cpp AESCommon.cpp AES128.cpp \
              SHA256.cpp SHA512.cpp BigNumberUtil.cpp Curve25519.cpp Ed25519.cpp

OWN_CXX := mcf_util.cpp mcf_frame.cpp mcf_report.cpp compat/rng_host.cpp

# Object names carry the tree they came from. Two of these sources are called
# sha512 - ed25519's sha512.c and Crypto's SHA512.cpp - and this repository is
# built from a Windows filesystem, where those two names are one file. Without
# the prefixes the second object silently overwrites the first and the link
# fails a long way from the cause.
OWN_OBJS    := $(addprefix $(OBJDIR)/,$(notdir $(OWN_CXX:.cpp=.o)))
RIFT_OBJS   := $(addprefix $(OBJDIR)/rift_,$(notdir $(RIFT_CXX:.cpp=.o)))
ED25519_OBJS := $(addprefix $(OBJDIR)/ed25519_,$(ED25519_C:.c=.o))
CRYPTO_OBJS := $(addprefix $(OBJDIR)/crypto_,$(CRYPTO_CXX:.cpp=.o))
VENDOR_OBJS := $(RIFT_OBJS) $(ED25519_OBJS) $(CRYPTO_OBJS)
LIB_OBJS    := $(OWN_OBJS) $(VENDOR_OBJS)

BIN  := meshcore-frame$(SUFFIX)
TEST := $(REPO)/tests/meshcore_frame_test$(SUFFIX)

vpath %.cpp . compat $(RIFT_DIR)/src $(RIFT_DIR)/src/helpers $(CRYPTO_DIR) $(REPO)/tests
vpath %.c $(RIFT_DIR)/lib/ed25519

.PHONY: all test check-vendor clean

all: check-vendor $(BIN)

# The pinned checkouts, checked before anything is compiled. A tool whose
# whole purpose is to speak another project's wire format has to be able to
# say which revision of that project it was built from, and `meshcore-frame
# version` prints exactly what is checked here.
check-vendor:
	@missing=0; \
	for d in "$(RIFT_DIR)" "$(CRYPTO_DIR)"; do \
	    [ -d "$$d" ] || { echo "ERROR: $$d is missing."; missing=1; }; \
	done; \
	if [ $$missing = 1 ]; then \
	    echo "       meshcore-frame needs two ignored checkouts:"; \
	    echo "         git clone https://github.com/KrakenSaten/RIFT.git vendor/RIFT"; \
	    echo "         git -C vendor/RIFT checkout $(RIFT_COMMIT)"; \
	    echo "         git clone https://github.com/rweather/arduinolibs.git vendor/Crypto"; \
	    echo "         git -C vendor/Crypto checkout $(CRYPTO_COMMIT)"; \
	    exit 1; \
	fi
	@rift=$$(git -C "$(RIFT_DIR)" rev-parse HEAD 2>/dev/null); \
	 crypto=$$(git -C "$(CRYPTO_REPO)" rev-parse HEAD 2>/dev/null); \
	 bad=0; \
	 [ "$$rift" = "$(RIFT_COMMIT)" ] || { echo "ERROR: vendor/RIFT is at $${rift:-an unreadable commit}, pinned at $(RIFT_COMMIT)"; bad=1; }; \
	 [ "$$crypto" = "$(CRYPTO_COMMIT)" ] || { echo "ERROR: vendor/Crypto is at $${crypto:-an unreadable commit}, pinned at $(CRYPTO_COMMIT)"; bad=1; }; \
	 if [ $$bad = 1 ]; then \
	    echo "       The frames this tool builds are only as accurate as the protocol"; \
	    echo "       source they came from, so the pin is not advisory. Check the trees"; \
	    echo "       out at the commits above, or set MESHCORE_FRAME_ALLOW_UNPINNED=1"; \
	    echo "       to build against whatever is there."; \
	    [ "$(MESHCORE_FRAME_ALLOW_UNPINNED)" = "1" ] || exit 1; \
	    echo "       MESHCORE_FRAME_ALLOW_UNPINNED=1 set: building unpinned."; \
	 fi

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(BIN): $(LIB_OBJS) $(OBJDIR)/mcf_main.o
	$(CXX) $(ALL_CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST): $(LIB_OBJS) $(OBJDIR)/meshcore_frame_test.o
	$(CXX) $(ALL_CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Our own sources carry the warning flags; the vendored ones do not.
$(OWN_OBJS) $(OBJDIR)/mcf_main.o $(OBJDIR)/meshcore_frame_test.o: EXTRA_CXXFLAGS := $(WARN_FLAGS)

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) $(EXTRA_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/rift_%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/crypto_%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/ed25519_%.o: %.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) $(ED25519_SAN_FLAGS) -c -o $@ $<

test: all $(TEST)
	$(TEST)
	bash $(REPO)/tests/meshcore_frame_cli_test.sh ./$(BIN)

-include $(wildcard $(OBJDIR)/*.d)

clean:
	rm -rf build build-asan meshcore-frame meshcore-frame-asan \
	       $(REPO)/tests/meshcore_frame_test $(REPO)/tests/meshcore_frame_test-asan
