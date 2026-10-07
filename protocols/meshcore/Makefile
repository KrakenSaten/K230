# protocols/meshcore: the portable MeshCore protocol core.
#
# Like tools/meshcore-frame, this is deliberately not part of `make all` or
# `make test` at the top of the repository. It needs two upstream checkouts a
# Doors build does not - vendor/RIFT (the MeshCore protocol source) and
# vendor/Crypto (the cipher, hash and signature library MeshCore itself uses)
# - and nothing it produces is installed or reaches an image yet. When a
# MeshCore service exists and is part of a build, that changes; until then,
# making every Doors build depend on two extra clones would be a poor trade.
#
# From the top of the repository:
#
#   make meshcore-core         build libmeshcore.a
#   make meshcore-core-test    build it and run its tests, plain and sanitised
#
# Prerequisites, as ignored checkouts at the pinned commits beside this file
# (the same two tools/meshcore-frame uses, at the same commits) - see
# README.md for the two clone commands.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0

CC       ?= cc
CXX      ?= c++
CXXFLAGS ?= -O2
LDFLAGS  ?=
AR       ?= ar

REPO        := ../..
RIFT_DIR    ?= $(REPO)/vendor/RIFT
CRYPTO_REPO ?= $(REPO)/vendor/Crypto
CRYPTO_DIR  := $(CRYPTO_REPO)/libraries/Crypto

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
# the full address and undefined-behaviour checks. The same narrow exemption,
# for the same files and the same reason, as tools/meshcore-frame.
ED25519_SAN_FLAGS := -fno-sanitize=shift-base
SUFFIX    := -asan
else
SAN_FLAGS :=
ED25519_SAN_FLAGS :=
SUFFIX    :=
endif

OBJDIR := build$(SUFFIX)

# -Wall -Wextra, as the rest of the repository builds, applied to our own
# sources only. The vendored trees are third-party code compiled as their
# authors wrote it, and silencing or "fixing" their warnings here would be an
# edit to somebody else's protocol implementation. They are reached with
# -isystem rather than -I for the same reason: their headers are included by
# our sources, and without it every one of our translation units would repeat
# their warnings - a wall of noise in which a real warning about our own code
# would not be noticed.
WARN_FLAGS := -Wall -Wextra
INCLUDES   := -Iport -Icompat -Itest_support \
              -isystem $(RIFT_DIR)/src -isystem $(RIFT_DIR)/lib/ed25519 \
              -isystem $(CRYPTO_DIR)
DEFINES    := -DMESHCORE_RIFT_COMMIT=\"$(RIFT_COMMIT)\" \
              -DMESHCORE_CRYPTO_COMMIT=\"$(CRYPTO_COMMIT)\"
# -MD, not -MMD. The difference is the one that matters here: -MMD leaves
# system headers out of the dependency files, and every vendored MeshCore and
# Crypto header IS a system header to this build, because -isystem is how
# they are reached (above). With -MMD, editing vendor/RIFT/src/Mesh.h - or
# checking the tree out at a revision where it differs - left every object
# that includes it alone, and the next `make` linked a stale one into
# libmeshcore.a without a word. -MD lists them all. The cost is that the .d
# files also name libc's headers, which is noise; the benefit is that a
# header this library actually compiles against cannot change unnoticed.
# tests/meshcore_build_deps_test.sh is the regression check.
ALL_CXXFLAGS := $(CXXFLAGS) -std=gnu++17 $(SAN_FLAGS) $(INCLUDES) $(DEFINES) -MD -MP
ALL_CFLAGS   := $(CXXFLAGS) -std=gnu11 $(SAN_FLAGS) $(INCLUDES) -MD -MP

# ---- what is compiled from the vendored trees -----------------------------
# The MeshCore protocol runtime and nothing else: framing, crypto, identity,
# the dispatcher, the mesh layer, the packet pool, the chat mesh and the two
# app-data helpers. No radio driver, no board support, no display, no
# filesystem, no companion link. README.md lists what was left out and why.
RIFT_CXX := Packet.cpp Utils.cpp Identity.cpp Dispatcher.cpp Mesh.cpp \
            helpers/StaticPoolPacketManager.cpp \
            helpers/AdvertDataHelpers.cpp helpers/TxtDataHelpers.cpp \
            helpers/BaseChatMesh.cpp

# orlp's ed25519, as vendored by RIFT: the signing, the key pair and the
# X25519 agreement MeshCore uses. seed.c is left out (randomness comes from
# the host, port/mc_rng.cpp), and so are verify.c, which MeshCore does not
# call - it verifies through rweather's Ed25519::verify - and add_scalar.c,
# which nothing here calls.
ED25519_C := keypair.c sign.c key_exchange.c fe.c ge.c sc.c sha512.c

# rweather/Crypto, the library MeshCore's own library.json depends on: AES128
# for the cipher, SHA256 for the hashes and the HMAC, Ed25519 for signature
# verification (Curve25519 and BigNumberUtil are what Ed25519 is built on).
CRYPTO_CXX := Crypto.cpp Hash.cpp BlockCipher.cpp AESCommon.cpp AES128.cpp \
              SHA256.cpp SHA512.cpp BigNumberUtil.cpp Curve25519.cpp Ed25519.cpp

# ---- our own -------------------------------------------------------------
# The whole Linux adaptation layer. Four files.
OWN_CXX := port/mc_clock.cpp port/mc_log.cpp port/mc_rng.cpp compat/rng_host.cpp

# Object names carry the tree they came from. Two of these sources are called
# sha512 - ed25519's sha512.c and Crypto's SHA512.cpp - and this repository is
# built from a Windows filesystem, where those two names are one file. Without
# the prefixes the second object silently overwrites the first and the link
# fails a long way from the cause.
OWN_OBJS     := $(addprefix $(OBJDIR)/,$(notdir $(OWN_CXX:.cpp=.o)))
RIFT_OBJS    := $(addprefix $(OBJDIR)/rift_,$(notdir $(RIFT_CXX:.cpp=.o)))
ED25519_OBJS := $(addprefix $(OBJDIR)/ed25519_,$(ED25519_C:.c=.o))
CRYPTO_OBJS  := $(addprefix $(OBJDIR)/crypto_,$(CRYPTO_CXX:.cpp=.o))
VENDOR_OBJS  := $(RIFT_OBJS) $(ED25519_OBJS) $(CRYPTO_OBJS)
LIB_OBJS     := $(OWN_OBJS) $(VENDOR_OBJS)

LIB := libmeshcore$(SUFFIX).a

# The RNG failure path has no other way of being reached: the kernel pool does
# not fail on demand. port/mc_rng.cpp is therefore compiled a second time with
# the hook, into the test binaries only - the same arrangement sysd, netd and
# pos-wave use for theirs. The shipped object in $(LIB) does not contain it,
# and tests/meshcore_lint.sh checks that.
HOOK_OBJ := $(OBJDIR)/mc_rng_hooks.o

TEST_NAMES := meshcore_core_test meshcore_port_test meshcore_smoke_test
TEST_OBJS  := $(addsuffix .o,$(addprefix $(OBJDIR)/,$(TEST_NAMES)))
TESTS      := $(addprefix $(REPO)/tests/,$(addsuffix $(SUFFIX),$(TEST_NAMES)))

vpath %.cpp port compat $(RIFT_DIR)/src $(RIFT_DIR)/src/helpers $(CRYPTO_DIR) $(REPO)/tests
vpath %.c $(RIFT_DIR)/lib/ed25519

.PHONY: all test check-vendor clean FORCE

all: check-vendor $(LIB)

# ---- the vendor identity stamp -------------------------------------------
#
# One small file that does three jobs the build used to do badly or not at
# all.
#
# 1. It GATES compilation. `all: check-vendor $(LIB)` only orders the check
#    before the library under a serial make; under `make -j` the two start
#    together and a build against an unpinned tree can be most of the way
#    done before the check fails. Every object below has this stamp as a real
#    prerequisite, and its recipe is what validates the pins, so make cannot
#    compile anything until the validation has passed - in any -j order.
#
# 2. It INVALIDATES the objects when the source's identity changes. The pin
#    files and the commit each tree is checked out at are written into it. A
#    pin edit or a `git checkout` in either tree therefore rewrites the stamp
#    and rebuilds every object, so libmeshcore.a cannot end up holding
#    objects compiled from two different revisions. (Individual file changes
#    are caught by the dependency files; this is for the whole-tree move,
#    which no .d can see.)
#
# Where a tree's commit comes from: git, when the tree is its own repository
# (a checkout in vendor/), or .doors-pinned-commit, which apply_to_sdk.sh writes
# into the copy it exports to the Buildroot package after checking that very
# checkout against the pin - that copy has no .git. Git is asked only when the
# tree is its own top level: the package is built inside the SDK's checkout, and
# `git rev-parse HEAD` there answers with the SDK's commit, not MeshCore's. The
# stamp records which of the two it was (rift_source, crypto_source).
#
# 3. It RECORDS whether the pin was honoured. MESHCORE_ALLOW_UNPINNED=1
#    remains a deliberate escape hatch, but the build it produces is no
#    longer indistinguishable from a pinned one: the stamp says
#    `pinned=no`, and tests/meshcore_lint.sh refuses to call such a library
#    pinned.
#
# The recipe rewrites the file only when its content changes, which is why a
# FORCE prerequisite here does not mean recompiling on every make.
VENDOR_ID := $(OBJDIR)/vendor-id.stamp

FORCE:

check-vendor: $(VENDOR_ID)

$(VENDOR_ID): FORCE | $(OBJDIR)
	@missing=0; \
	for d in "$(RIFT_DIR)" "$(CRYPTO_DIR)"; do \
	    [ -d "$$d" ] || { echo "ERROR: $$d is missing."; missing=1; }; \
	done; \
	if [ $$missing = 1 ]; then \
	    echo "       protocols/meshcore needs two ignored checkouts:"; \
	    echo "         git clone https://github.com/KrakenSaten/RIFT.git vendor/RIFT"; \
	    echo "         git -C vendor/RIFT checkout $(RIFT_COMMIT)"; \
	    echo "         git clone https://github.com/rweather/arduinolibs.git vendor/Crypto"; \
	    echo "         git -C vendor/Crypto checkout $(CRYPTO_COMMIT)"; \
	    exit 1; \
	fi; \
	head_of() { \
	    if [ "$$(git -C "$$1" rev-parse --show-toplevel 2>/dev/null)" = "$$(cd "$$1" && pwd -P)" ]; then \
	        git -C "$$1" rev-parse HEAD 2>/dev/null; echo git; \
	    elif [ -f "$$1/.doors-pinned-commit" ]; then \
	        tr -d ' \t\r\n' < "$$1/.doors-pinned-commit"; echo; echo export; \
	    fi; \
	}; \
	rift_id=$$(head_of "$(RIFT_DIR)"); crypto_id=$$(head_of "$(CRYPTO_REPO)"); \
	rift=$$(echo "$$rift_id" | sed -n 1p); rift_src=$$(echo "$$rift_id" | sed -n 2p); \
	crypto=$$(echo "$$crypto_id" | sed -n 1p); crypto_src=$$(echo "$$crypto_id" | sed -n 2p); \
	bad=0; \
	[ "$$rift" = "$(RIFT_COMMIT)" ] || { echo "ERROR: vendor/RIFT is at $${rift:-an unreadable commit}, pinned at $(RIFT_COMMIT)"; bad=1; }; \
	[ "$$crypto" = "$(CRYPTO_COMMIT)" ] || { echo "ERROR: vendor/Crypto is at $${crypto:-an unreadable commit}, pinned at $(CRYPTO_COMMIT)"; bad=1; }; \
	pinned=yes; \
	if [ $$bad = 1 ]; then \
	    echo "       What goes on the air is decided by the protocol source this was"; \
	    echo "       built from, so the pin is not advisory. Check the trees out at the"; \
	    echo "       commits above, or set MESHCORE_ALLOW_UNPINNED=1 to build against"; \
	    echo "       whatever is there."; \
	    [ "$(MESHCORE_ALLOW_UNPINNED)" = "1" ] || exit 1; \
	    echo "       MESHCORE_ALLOW_UNPINNED=1 set: building unpinned."; \
	    pinned=no; \
	fi; \
	{ echo "rift_pin=$(RIFT_COMMIT)"; \
	  echo "crypto_pin=$(CRYPTO_COMMIT)"; \
	  echo "rift_head=$${rift:-unknown}"; \
	  echo "crypto_head=$${crypto:-unknown}"; \
	  echo "rift_source=$${rift_src:-none}"; \
	  echo "crypto_source=$${crypto_src:-none}"; \
	  echo "pinned=$$pinned"; } > $@.new; \
	if cmp -s $@.new $@ 2>/dev/null; then rm -f $@.new; else mv -f $@.new $@; fi

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(LIB): $(LIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

# Our own sources carry the warning flags; the vendored ones do not.
$(OWN_OBJS) $(HOOK_OBJ): EXTRA_CXXFLAGS := $(WARN_FLAGS)

# The test objects additionally see the RNG hook's declaration, because they
# link the hooked mc_rng.o below and call it.
$(TEST_OBJS): EXTRA_CXXFLAGS := $(WARN_FLAGS) -DMC_RNG_TEST_HOOKS=1

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) $(EXTRA_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/rift_%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/crypto_%.o: %.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) -c -o $@ $<

$(OBJDIR)/ed25519_%.o: %.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) $(ED25519_SAN_FLAGS) -c -o $@ $<

$(HOOK_OBJ): port/mc_rng.cpp | $(OBJDIR)
	$(CXX) $(ALL_CXXFLAGS) $(WARN_FLAGS) -DMC_RNG_TEST_HOOKS=1 -c -o $@ $<

# Nothing compiles before the pins have been validated, and everything is
# recompiled when the identity of either vendored tree changes. Both of those
# are this one line; see $(VENDOR_ID) above for why.
#
# It comes after the rules above, not before them: prerequisites named in a
# second rule are appended to the ones already there, and $(HOOK_OBJ) is
# built by an explicit rule whose $< must stay port/mc_rng.cpp. (The pattern
# rules are safe either way - $< is the prerequisite the pattern supplied.)
$(LIB_OBJS) $(HOOK_OBJ) $(TEST_OBJS): $(VENDOR_ID)

# The test binaries link the hooked mc_rng.o in place of the shipped one, so
# what they exercise is otherwise object-for-object what a service would get.
TEST_LIB_OBJS := $(HOOK_OBJ) $(filter-out $(OBJDIR)/mc_rng.o,$(LIB_OBJS))

$(REPO)/tests/meshcore_core_test$(SUFFIX): $(OBJDIR)/meshcore_core_test.o $(TEST_LIB_OBJS)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(REPO)/tests/meshcore_port_test$(SUFFIX): $(OBJDIR)/meshcore_port_test.o $(TEST_LIB_OBJS)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(REPO)/tests/meshcore_smoke_test$(SUFFIX): $(OBJDIR)/meshcore_smoke_test.o $(TEST_LIB_OBJS)
	$(CXX) $(ALL_CXXFLAGS) -o $@ $^ $(LDFLAGS)

# LSAN_OPTIONS points at lsan.supp, which suppresses exactly two vendored
# constructors and nothing else - see that file for why. It is set for both
# builds; without the sanitizers the variable is simply ignored, and setting
# it unconditionally keeps the two runs identical in every other respect.
LSAN_OPTS := suppressions=$(CURDIR)/lsan.supp

test: all $(TESTS)
	@set -e; for t in $(TESTS); do echo "--- $$t"; LSAN_OPTIONS=$(LSAN_OPTS) $$t; done
	MESHCORE_RIFT_DIR="$(abspath $(RIFT_DIR))" \
	MESHCORE_CRYPTO_REPO="$(abspath $(CRYPTO_REPO))" \
	    bash $(REPO)/tests/meshcore_lint.sh

-include $(wildcard $(OBJDIR)/*.d)

# build-riscv64 and libmeshcore-riscv64.a are named here rather than derived,
# because the cross target sets OBJDIR and LIB from the command line (see the
# top-level meshcore-core-riscv64 rule) and a plain `make clean` does not know
# about them.
clean:
	rm -rf build build-asan build-riscv64 \
	       libmeshcore.a libmeshcore-asan.a libmeshcore-riscv64.a \
	       $(addprefix $(REPO)/tests/,$(TEST_NAMES)) \
	       $(addprefix $(REPO)/tests/,$(addsuffix -asan,$(TEST_NAMES)))
