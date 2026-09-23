#!/bin/bash
# The meshcored boundary, checked statically.
#
# Three claims this service makes about itself are the kind that stop being
# true quietly, one edit at a time, and none of them is visible in a passing
# functional test:
#
#   1. radiod owns the radio. meshcored opens no device, drives no GPIO and
#      links no radio library. A single #include would undo that and nothing
#      would fail.
#   2. The protocol core does not know about IPC, and the daemon does not know
#      about MeshCore. The seam is services/meshcored/mesh_runtime.h and it is
#      one file wide.
#   3. Nothing here is a UI, and nothing here reaches into the RIFT design
#      package.
#
# It also checks the things that would be embarrassing rather than wrong: a
# private key on the wire, a default that starts the radio on every boot, a
# service that shells out.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
checks=0

check() { # <label> <0|1>
    checks=$((checks + 1))
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}

refuse() { # <label> <regex> [files...]
    local label="$1"; shift
    local pattern="$1"; shift
    local hits
    hits=$(grep -rnE "$pattern" "$@" 2>/dev/null)
    checks=$((checks + 1))
    if [ -n "$hits" ]; then
        echo "FAIL $label:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $label"
    fi
}

SRC=services/meshcored
C_HALF="$SRC/main.c $SRC/api.c $SRC/radio_link.c $SRC/tx_map.c $SRC/mcd_util.c $SRC/mcd.h $SRC/tx_map.h $SRC/mcd_util.h $SRC/radio_link.h"
CXX_HALF="$SRC/mesh_runtime.cpp $SRC/mesh_store.cpp $SRC/mesh_store.h"

check "the service directory exists" "$([ -d "$SRC" ] && echo 1 || echo 0)"

# ---- 1. radiod owns the radio --------------------------------------------

refuse "no SPI, GPIO or radio-library header is included" \
    '#include.*(spidev|gpiod|linux/spi|RadioLib|SX126|SX127|LLCC68|LR11)' \
    "$SRC"
refuse "no device path is named" \
    '/dev/(spidev|gpiochip|i2c|mem)' \
    "$SRC"
refuse "no GPIO or SPI call" \
    '\b(gpiod_[a-z_]+|SPI_IOC_MESSAGE|ioctl *\()' \
    "$SRC"
refuse "the sx1262 backend is not reached" \
    'radio_backend_sx1262|hal_linux|PocketRadioHal' \
    "$SRC"
# The one radiod header it may include is the airtime formula, which is
# arithmetic and touches nothing.
INCLUDED_RADIOD=$(grep -rhoE '#include "[a-z_]+\.h"' "$SRC" | sort -u | grep -E 'airtime' | wc -l)
check "the only radiod header it borrows is the airtime formula" \
    "$([ "$INCLUDED_RADIOD" -le 1 ] && echo 1 || echo 0)"

# Protocol traffic goes out asynchronously. The synchronous radio.send blocks
# radiod for the whole airtime and would block this service with it.
refuse "protocol traffic never uses the synchronous radio.send" \
    '"radio\.send"' \
    "$SRC"
check "it does use radio.send_async" \
    "$(grep -rq '"radio.send_async"' "$SRC" && echo 1 || echo 0)"

# ---- 2. the seam ----------------------------------------------------------

refuse "the C daemon includes no MeshCore header" \
    '#include *[<"](Mesh|Packet|Identity|Utils|Dispatcher|MeshCore|helpers/|mc_port)' \
    $C_HALF
refuse "the C++ protocol half includes no cJSON or pocketipc" \
    '#include.*(cjson|cJSON|pocketipc)' \
    $CXX_HALF
refuse "and names no JSON field or IPC method" \
    '(cJSON_|pocketipc_|"mesh\.[a-z]|"radio\.[a-z])' \
    $CXX_HALF
refuse "the C++ half does not depend on pocketlog either" \
    '#include.*pocketlog' \
    $CXX_HALF
check "the seam is one header" \
    "$([ -f "$SRC/mesh_runtime.h" ] && echo 1 || echo 0)"
# Symbols and includes, not prose: the header's own comment explains what it
# keeps apart, and naming the two sides is how it does that.
refuse "and the seam header itself is free of both worlds" \
    '(cJSON_|pocketipc_|#include.*(cjson|pocketipc)|#include *[<"](Mesh|Packet|Identity|Dispatcher|helpers/))' \
    "$SRC/mesh_runtime.h"

# ---- 3. no UI, and the design package is not touched ----------------------

refuse "nothing here draws anything" \
    '(lv_[a-z_]+\(|#include.*lvgl|pos_style|pos_theme|LV_SYMBOL)' \
    "$SRC"
# This file is left out of its own scan: it has to name those paths to check
# for them.
refuse "nothing here reaches into the RIFT design package or a RIFT app" \
    '(docs/design/rift|apps/rift)' \
    "$SRC" tests/meshcored_service_test.sh tests/meshcored_harness_test.sh \
    tests/meshcored_runtime_test.cpp tests/meshcored_store_test.cpp \
    tests/meshcored_util_test.c tests/meshcored_txmap_test.c
# apps/rift exists now (RIFT phase 1), and its existence was never the point:
# what this file is for is the boundary. meshcored owns the MeshCore protocol
# and runtime, RIFT is a UI above it, and the service must not know that the
# UI is there - not by including it, not by linking it, and not by having a
# build of its own that needs it. The `refuse` above already checks that no
# source here names apps/rift; these check the other direction, that nothing
# in the service's build reaches for it.
check "the service's objects come from services/meshcored and nowhere else" \
    "$(sed -n '/^MESHCORED_C_OBJS/,/^MESHCORED_OBJS/p' Makefile |
       grep -oE '[a-z_/]+\.o' | grep -vE '^(services/meshcored|services/radiod|core)/' |
       grep -q . && echo 0 || echo 1)"
check "and no part of the app is built into it" \
    "$(sed -n '/^MESHCORED_C_OBJS/,/^MESHCORED_OBJS/p' Makefile | grep -q 'apps/' &&
       echo 0 || echo 1)"
check "the service builds with no include path into the app" \
    "$(grep -E '^MESHCORED_CXXFLAGS|^\$\(MESHCORED_C_OBJS\)' Makefile | grep -q 'apps/rift' &&
       echo 0 || echo 1)"

# ---- 4. secrets and side effects ------------------------------------------

# The private key is loaded, used to sign, and never reported. The store is
# the only file that may name it.
refuse "no method result or event can carry the private key" \
    '(prv_key|private_key|PRV_KEY)' \
    "$SRC/api.c" "$SRC/main.c" "$SRC/radio_link.c"
refuse "the service never shells out" \
    '\b(system|popen|execl|execle|execlp|execv|execvp|execve|fork|posix_spawn) *\(' \
    "$SRC"
refuse "and never builds a path out of anything a client sent" \
    'sprintf *\(' \
    "$SRC"

# ---- 5. the profile is the one the gate proved ----------------------------

check "the default frequency is the MeshCore one" \
    "$(grep -q 'MCD_DEFAULT_FREQUENCY_MHZ 869.618' "$SRC/mcd.h" && echo 1 || echo 0)"
check "the default bandwidth" \
    "$(grep -q 'MCD_DEFAULT_BANDWIDTH_KHZ 62.5' "$SRC/mcd.h" && echo 1 || echo 0)"
check "the default spreading factor" \
    "$(grep -q 'MCD_DEFAULT_SPREADING_FACTOR 8' "$SRC/mcd.h" && echo 1 || echo 0)"
check "the default sync word" \
    "$(grep -q 'MCD_DEFAULT_SYNC_WORD 0x12' "$SRC/mcd.h" && echo 1 || echo 0)"
check "the default preamble" \
    "$(grep -q 'MCD_DEFAULT_PREAMBLE 32' "$SRC/mcd.h" && echo 1 || echo 0)"
# The power is the bench-safe value the gate transmitted at, not a regional
# maximum this service has no business choosing.
check "the default transmit power is the tested 2 dBm" \
    "$(grep -q 'MCD_DEFAULT_TX_POWER_DBM 2$' "$SRC/mcd.h" && echo 1 || echo 0)"

# ---- 5b. remote text, and the test seam -----------------------------------

# An advert name and a message body are chosen by whoever is on the air. Every
# one of them leaves through the sanitiser; a raw cJSON_AddStringToObject on
# one of those fields would put an escape sequence, or a byte sequence that is
# not UTF-8, straight into an IPC frame.
refuse "no remote string reaches a client unsanitised" \
    'cJSON_AddStringToObject\([^,]*, *"(name|peer_name|text)"' \
    "$SRC/api.c"
check "remote strings go through the sanitiser" \
    "$(grep -q 'add_remote_text' "$SRC/api.c" && echo 1 || echo 0)"
check "which is the one in mcd_util" \
    "$(grep -q 'mcd_text_sanitize' "$SRC/api.c" && echo 1 || echo 0)"

# The directory-flush hook exists so a failure that a working filesystem will
# not produce on request can be exercised. It must not be in the service.
if [ -f "$SRC/meshcored" ]; then
    check "the shipped binary carries no test hook" \
        "$(strings "$SRC/meshcored" 2>/dev/null | grep -q 'failNextDirSyncForTest' && echo 0 || echo 1)"
fi
check "and the hook is declared only inside its guard" \
    "$(awk '/#ifdef MCD_STORE_TEST_HOOKS/{g=1} /#endif/{g=0}
            /failNextDirSyncForTest/{ if (!g) bad=1 }
            END{exit bad ? 1 : 0}' "$SRC/mesh_store.h" && echo 1 || echo 0)"
check "and defined only inside it" \
    "$(awk '/#ifdef MCD_STORE_TEST_HOOKS/{g=1} /#endif/{g=0}
            /^void failNextDirSyncForTest/{ if (!g) bad=1 }
            END{exit bad ? 1 : 0}' "$SRC/mesh_store.cpp" && echo 1 || echo 0)"

# ---- 5c. the lease is not held by a service that has given up -------------

check "a permanently refused profile releases the radio" \
    "$(grep -q 'link_fail_permanently' "$SRC/radio_link.c" && echo 1 || echo 0)"
check "by asking, and by closing the connection" \
    "$(grep -A 12 'static void link_fail_permanently' "$SRC/radio_link.c" |
       grep -q 'radio.release' && echo 1 || echo 0)"
check "and it does not reconnect afterwards" \
    "$(grep -q 'l->permanent' "$SRC/radio_link.c" && echo 1 || echo 0)"

# ---- 5d. a node the table did not keep is not a node ----------------------

check "discovery is reported only for contacts the table kept" \
    "$(grep -q 'isRetained' "$SRC/mesh_runtime.cpp" && echo 1 || echo 0)"
check "and an unstored one does not mark the state dirty" \
    "$(awk '/void onDiscoveredContact/,/^    }/' "$SRC/mesh_runtime.cpp" |
       awk '/isRetained/{seen=1} seen && /_dirty = true/{after=1} END{exit !after}' &&
       echo 1 || echo 0)"

# ---- 6. how it ships ------------------------------------------------------
#
# In every image, disabled per unit. An ordinary host build still leaves it out
# (it needs vendor/RIFT and vendor/Crypto); the image package builds and
# installs it, and installing it is still gated on the notices - which
# tests/notices_test.sh executes, both ways. This only checks the wiring.

check "the host build switch defaults to off" \
    "$(grep -q '^ENABLE_MESHCORED ?= 0' Makefile && echo 1 || echo 0)"
# What make builds when nobody has asked for anything. This lint runs inside
# `make meshcored-test`, and a variable on an enclosing make's command line
# (`make ENABLE_MESHCORED=1 meshcored-test`) reaches every make below it
# through MAKEFLAGS - a bare `make` here then answered for the caller's
# build, not the default one, and this check failed whenever meshcored was
# being tested with its own switch on. So the question is asked with nothing
# inherited: no MAKEFLAGS and relatives, and no switch in the environment.
MCD_BIN=services/meshcored/meshcored
outputs() { # [make args...]: the build outputs, as make reports them with nothing inherited
    env -u MAKEFLAGS -u MFLAGS -u GNUMAKEFLAGS -u MAKELEVEL -u MAKEOVERRIDES -u ENABLE_MESHCORED \
        make -s "$@" print-build-outputs 2>/dev/null
}
check "so a default host build does not produce it" \
    "$(outputs | grep -q "$MCD_BIN" && echo 0 || echo 1)"
check "and the check can see it: switched on, the same question lists it" \
    "$(outputs ENABLE_MESHCORED=1 | grep -q "$MCD_BIN" && echo 1 || echo 0)"
# The caller's switch, the way each kind of caller passes it on. Each is shown
# to switch a bare make on - so the environment below is a real threat - and
# then not to reach the question above.
hostile_ok=1
hostile_seen=1
for how in "MAKEFLAGS= -- ENABLE_MESHCORED=1" "GNUMAKEFLAGS=ENABLE_MESHCORED=1" "ENABLE_MESHCORED=1"; do
    var=${how%%=*}
    val=${how#*=}
    ( export "$var=$val"; make -s print-build-outputs 2>/dev/null ) | grep -q "$MCD_BIN" || hostile_seen=0
    ( export "$var=$val"; outputs ) | grep -q "$MCD_BIN" && hostile_ok=0
done
check "a caller's switch (MAKEFLAGS, GNUMAKEFLAGS, the environment) does switch a bare make on" \
    "$hostile_seen"
check "and none of them changes the default-build answer" "$hostile_ok"
check "installing it is gated on the notices" \
    "$(grep -q '^install: all meshcored-shipping-check$' Makefile && echo 1 || echo 0)"
check "the image package builds and installs it" \
    "$([ "$(grep -c 'ENABLE_MESHCORED=1 -C' platforms/k230/package/pocketos/pocketos.mk)" -eq 2 ] && echo 1 || echo 0)"
check "the package path checks its notices before assembling anything" \
    "$(grep -q 'for id in meshcore ed25519 arduinolibs-crypto' platforms/k230/scripts/apply_to_sdk.sh && echo 1 || echo 0)"
check "and exports the MeshCore and Crypto trees it compiles, pin-checked" \
    "$(grep -q 'doors-pinned-commit' platforms/k230/scripts/apply_to_sdk.sh &&
       grep -q 'doors-pinned-commit' protocols/meshcore/Makefile && echo 1 || echo 0)"

INIT=platforms/k230/rootfs_overlay/etc/init.d/S65meshcored
check "the init script exists" "$([ -f "$INIT" ] && echo 1 || echo 0)"
if [ -f "$INIT" ]; then
    check "and ships disabled, so no boot acquires the radio" \
        "$(grep -q '^MESHCORED_ENABLE=0' "$INIT" && echo 1 || echo 0)"
    check "with an operator switch in /etc/default/meshcored" \
        "$(grep -q '/etc/default/meshcored' "$INIT" && echo 1 || echo 0)"
    check "it is supervised like the other services" \
        "$(grep -q 'pos-supervise' "$INIT" && echo 1 || echo 0)"
    check "enabled and not installed, it fails loudly rather than quietly" \
        "$(grep -q 'FAILED: enabled, but' "$INIT" && echo 1 || echo 0)"
    check "it will not start beside a meshcored it did not start, and stop ends one" \
        "$(grep -q 'meshcored_pids' "$INIT" && grep -q 'stop_unsupervised' "$INIT" && echo 1 || echo 0)"
    check "and its mode is 0755 in git, which is the mode in the image" \
        "$(git ls-files --stage -- "$INIT" 2>/dev/null | grep -q '^100755' && echo 1 || echo 0)"
fi
# One process per node, whoever started it: the daemon locks its state
# directory before it reads the identity or takes the socket
# (tests/meshcored_service_test.sh, section 1b, executes the refusal).
check "meshcored locks its state directory before it reads the identity" \
    "$(awk '/mcd_runtime_lock_state_dir\(/{l=NR} /mcd_runtime_create\(/{c=NR} END{exit !(l && c && l < c)}' \
       "$SRC/main.c" && echo 1 || echo 0)"

# ---- 7. no periodic transmit ----------------------------------------------
#
# The service transmits when a client asks it to, and when the protocol owes
# somebody an answer. It does not advertise on a timer. On a handheld that is
# the difference between a radio that is quiet until used and one that keys up
# unattended.
refuse "nothing advertises on a timer" \
    '(advert_interval|periodic_advert|ADVERT_INTERVAL)' \
    "$SRC"

# ---- 8. documented -------------------------------------------------------

check "the IPC surface is documented" "$([ -f docs/api/mesh.md ] && echo 1 || echo 0)"
check "and the service itself" "$([ -f docs/services/MESHCORED.md ] && echo 1 || echo 0)"
for m in mesh.info mesh.status mesh.identity mesh.nodes mesh.node mesh.messages \
         mesh.send mesh.advert mesh.subscribe mesh.unsubscribe; do
    check "docs/api/mesh.md describes $m" \
        "$(grep -q "$m" docs/api/mesh.md 2>/dev/null && echo 1 || echo 0)"
    check "and $m is implemented" \
        "$(grep -q "\"$m\"" "$SRC/api.c" && echo 1 || echo 0)"
done
for s in starting waiting_for_radiod waiting_for_lease configuring online degraded error; do
    check "the state '$s' is documented" \
        "$(grep -q "$s" docs/api/mesh.md 2>/dev/null && echo 1 || echo 0)"
done

echo "meshcored_lint: $checks check(s), $failed failure(s)"
exit $((failed > 0))
