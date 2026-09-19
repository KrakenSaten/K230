#!/bin/bash
# One MeshCore checkout, or none.
#
# meshcored is compiled in two halves that must agree byte for byte about the
# protocol: its own C++ objects, which reach the MeshCore and Crypto headers
# with -isystem from the top-level Makefile, and libmeshcore.a, which a
# sub-make builds from those same trees. Both paths were selectable, and only
# one of them was forwarded - so
#
#     make MESHCORE_RIFT_DIR=<somewhere-else> meshcored
#
# compiled our translation units against one checkout and linked them against
# a library built from another. One binary, two revisions of the wire format,
# and nothing to say so. Worse, the pin check runs inside the sub-make, so it
# would have validated the tree that was NOT supplying the headers: a build
# that reported "pinned" while its headers came from anywhere at all.
#
# This holds the two together. It is cheap: the first three cases read what
# make would do, and only the last one builds anything.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
checks=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() { # <label> <0|1>
    checks=$((checks + 1))
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}

# What make WOULD do, without doing it. The two facts wanted from the plan:
# the -isystem the C++ objects are compiled with, and the RIFT_DIR/CRYPTO_REPO
# the sub-make is handed.
plan() { # plan [VAR=value ...]
    make -n ENABLE_MESHCORED=1 "$@" services/meshcored/mesh_runtime.o meshcore-lib 2>/dev/null
}
isystem_rift() { sed -n 's/.*-isystem \([^ ]*\)\/src .*/\1/p' "$1" | head -1; }
isystem_crypto() { sed -n 's/.*-isystem \([^ ]*\)\/libraries\/Crypto.*/\1/p' "$1" | head -1; }
submake_rift() { sed -n 's/.*RIFT_DIR="\([^"]*\)".*/\1/p' "$1" | head -1; }
submake_crypto() { sed -n 's/.*CRYPTO_REPO="\([^"]*\)".*/\1/p' "$1" | head -1; }
same_tree() { # <a> <b>
    a=$(readlink -f "$1" 2>/dev/null)
    b=$(readlink -f "$2" 2>/dev/null)
    [ -n "$a" ] && [ "$a" = "$b" ] && echo 1 || echo 0
}

# ---- 1. the default build ------------------------------------------------

plan > "$TMP/default.plan"
D_HDR_R=$(isystem_rift "$TMP/default.plan")
D_HDR_C=$(isystem_crypto "$TMP/default.plan")
D_LIB_R=$(submake_rift "$TMP/default.plan")
D_LIB_C=$(submake_crypto "$TMP/default.plan")

check "the plan compiles meshcored against a MeshCore checkout" \
    "$([ -n "$D_HDR_R" ] && echo 1 || echo 0)"
check "and hands the library build one too" \
    "$([ -n "$D_LIB_R" ] && echo 1 || echo 0)"
check "the headers and the library come from the same MeshCore checkout" \
    "$(same_tree "$D_HDR_R" "$D_LIB_R")"
check "and from the same Crypto checkout" \
    "$(same_tree "$D_HDR_C" "$D_LIB_C")"

# ---- 2. an explicit override, which is where they used to diverge --------

plan MESHCORE_RIFT_DIR=/tmp/other-rift MESHCORE_CRYPTO_REPO=/tmp/other-crypto \
    > "$TMP/override.plan"
O_HDR_R=$(isystem_rift "$TMP/override.plan")
O_HDR_C=$(isystem_crypto "$TMP/override.plan")
O_LIB_R=$(submake_rift "$TMP/override.plan")
O_LIB_C=$(submake_crypto "$TMP/override.plan")

check "an override moves the headers" \
    "$([ "$O_HDR_R" = "/tmp/other-rift" ] && echo 1 || echo 0)"
check "and it moves the library build with them" \
    "$([ "$O_LIB_R" = "/tmp/other-rift" ] && echo 1 || echo 0)"
check "the same for Crypto: the headers" \
    "$([ "$O_HDR_C" = "/tmp/other-crypto" ] && echo 1 || echo 0)"
check "and the library build" \
    "$([ "$O_LIB_C" = "/tmp/other-crypto" ] && echo 1 || echo 0)"
check "so an override cannot split the two" \
    "$([ "$O_HDR_R" = "$O_LIB_R" ] && [ "$O_HDR_C" = "$O_LIB_C" ] && echo 1 || echo 0)"

# Every sub-make of protocols/meshcore is handed the trees, not just the one
# the service links: a suite built against a different checkout from the
# library it exercises would be testing the wrong thing.
for target in meshcore-core meshcore-core-test meshcore-lib-asan; do
    make -n "$target" 2>/dev/null > "$TMP/t.plan"
    check "$target is handed the same checkout" \
        "$([ -n "$(submake_rift "$TMP/t.plan")" ] && echo 1 || echo 0)"
done

# ---- 3. a checkout whose revision cannot be established ------------------
#
# The build, for real. A copy of the MeshCore tree with its history removed
# is a tree nobody can name a commit for, and the pin check refuses it. That
# is the point: the tree it refuses is now the same tree the headers come
# from. Before these were forwarded, the sub-make validated vendor/RIFT - the
# pinned one - while the headers came from here, and the build succeeded.
RIFT_SRC=${MESHCORE_RIFT_DIR:-vendor/RIFT}
if [ ! -d "$RIFT_SRC/src" ]; then
    echo "NOT RUN meshcored_source_identity_test: $RIFT_SRC is missing."
    echo "        This is a build-integrity gate; not running it is not a pass."
    exit 77
fi
cp -a "$RIFT_SRC" "$TMP/unnamed-rift"
rm -rf "$TMP/unnamed-rift/.git"

make ENABLE_MESHCORED=1 MESHCORE_RIFT_DIR="$TMP/unnamed-rift" meshcored \
    > "$TMP/mixed.log" 2>&1
rc=$?
check "a build against a checkout with no readable revision fails" \
    "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and the refusal names the tree the headers came from" \
    "$(grep -q "$TMP/unnamed-rift" "$TMP/mixed.log" && echo 1 || echo 0)"
check "and says the pin could not be checked" \
    "$(grep -q 'unreadable commit' "$TMP/mixed.log" && echo 1 || echo 0)"
# Nothing was linked: a refusal that still produced a binary would be no
# refusal at all.
check "and no binary came out of it" \
    "$(grep -q 'meshcored$' <<< "$(sed -n 's/.*-o \(services\/meshcored\/meshcored\)$/\1/p' "$TMP/mixed.log")" && echo 0 || echo 1)"

# The ordinary build still works afterwards, so the case above failed for the
# reason claimed rather than leaving the tree broken.
make ENABLE_MESHCORED=1 meshcored > "$TMP/after.log" 2>&1
check "the ordinary build is unaffected" "$([ $? -eq 0 ] && echo 1 || echo 0)"

echo "meshcored_source_identity_test: $checks check(s), $failed failure(s)"
exit $((failed > 0))
