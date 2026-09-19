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
check "apps/rift does not exist: this phase implements no UI" \
    "$([ ! -e apps/rift ] && echo 1 || echo 0)"

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

# ---- 6. it is off by default ----------------------------------------------

check "the build switch defaults to off" \
    "$(grep -q '^ENABLE_MESHCORED ?= 0' Makefile && echo 1 || echo 0)"
check "so a default build does not produce it" \
    "$(make -s print-build-outputs 2>/dev/null | grep -q 'services/meshcored/meshcored' && echo 0 || echo 1)"

INIT=platforms/k230/rootfs_overlay/etc/init.d/S65meshcored
check "the init script exists" "$([ -f "$INIT" ] && echo 1 || echo 0)"
if [ -f "$INIT" ]; then
    check "and ships disabled, so no boot acquires the radio" \
        "$(grep -q '^MESHCORED_ENABLE=0' "$INIT" && echo 1 || echo 0)"
    check "with an operator switch in /etc/default/meshcored" \
        "$(grep -q '/etc/default/meshcored' "$INIT" && echo 1 || echo 0)"
    check "it is supervised like the other services" \
        "$(grep -q 'pos-supervise' "$INIT" && echo 1 || echo 0)"
    check "and its mode is 0755 in git, which is the mode in the image" \
        "$(git ls-files --stage -- "$INIT" 2>/dev/null | grep -q '^100755' && echo 1 || echo 0)"
fi

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
