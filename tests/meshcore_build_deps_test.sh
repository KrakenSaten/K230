#!/bin/bash
# protocols/meshcore: the build's own integrity, checked by building.
#
# The test suites prove the protocol works. They say nothing about whether
# the objects they were linked from were compiled from the source the
# repository claims - and two ways of getting that wrong were live here:
#
#  1. Dependency generation used -MMD, which omits system headers, and every
#     vendored MeshCore and Crypto header IS a system header to this build
#     because -isystem is how they are reached. A header could change and the
#     objects that included it would not be rebuilt.
#
#  2. `all: check-vendor $(LIB)` only orders the pin check before the library
#     under a serial make. Under `make -j` the two start together, so an
#     unpinned build could be most of the way done before the check failed.
#
# Both are now the job of one file, protocols/meshcore/build/vendor-id.stamp,
# and this checks that it does that job - and that tests/meshcore_lint.sh
# actually fails on the things it claims to catch, in both vendored trees.
#
# Everything happens out of tree. Two throwaway shared clones of the real
# checkouts are made in a temporary directory and built into a temporary
# object directory; vendor/RIFT, vendor/Crypto and protocols/meshcore/build
# are never written to, and the mutations below happen only in the clones.
#
# Run by `make meshcore-core-test` from the top of the repository, once
# rather than once per sanitizer.

set -u
cd "$(dirname "$0")/.." || exit 1

# This script runs make itself. If it was reached from a parallel `make -j`,
# the inherited jobserver file descriptors belong to that make and not to the
# ones started below, so they are dropped rather than warned about.
unset MAKEFLAGS MFLAGS MAKELEVEL

LIB_DIR=protocols/meshcore
failed=0
checks=0
check() {
    checks=$((checks + 1))
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}
note() { printf '     %s\n' "$1"; }

for tool in git make; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "NOT RUN meshcore_build_deps: $tool is not available."
        exit 77
    }
done

RIFT_PIN=$(tr -d ' \t\r\n' < "$LIB_DIR/vendor_rift_commit.txt")
CRYPTO_PIN=$(tr -d ' \t\r\n' < "$LIB_DIR/vendor_crypto_commit.txt")

for d in vendor/RIFT vendor/Crypto; do
    git -C "$d" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        echo "NOT RUN meshcore_build_deps: $d is not a git checkout."
        echo "        See protocols/meshcore/README.md for the two clone commands."
        exit 77
    }
done

REPO=$PWD
WORK=$(mktemp -d "${TMPDIR:-/tmp}/meshcore-deps.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
JOBS=$(nproc 2>/dev/null || echo 4)

# Shared clones: the objects stay in the real repositories and only a working
# tree is written, so this costs a checkout rather than a copy.
git clone -q -s -n "$REPO/vendor/RIFT" "$WORK/RIFT" || exit 1
git clone -q -s -n "$REPO/vendor/Crypto" "$WORK/Crypto" || exit 1
git -C "$WORK/RIFT" checkout -q "$RIFT_PIN" || exit 1
git -C "$WORK/Crypto" checkout -q "$CRYPTO_PIN" || exit 1

RIFT_OTHER=$(git -C "$WORK/RIFT" rev-parse "$RIFT_PIN^")
CRYPTO_OTHER=$(git -C "$WORK/Crypto" rev-parse "$CRYPTO_PIN^")

OBJ="$WORK/obj"
STAMP="$OBJ/vendor-id.stamp"

mc_make() {
    make -C "$REPO/$LIB_DIR" \
         RIFT_DIR="$WORK/RIFT" CRYPTO_REPO="$WORK/Crypto" \
         OBJDIR="$OBJ" LIB="$WORK/libmeshcore.a" "$@" >"$WORK/make.log" 2>&1
}

mc_lint() {
    MESHCORE_RIFT_DIR="$WORK/RIFT" MESHCORE_CRYPTO_REPO="$WORK/Crypto" \
    MESHCORE_LIB="$WORK/libmeshcore.a" MESHCORE_BUILD_STAMP="$STAMP" \
        bash "$REPO/tests/meshcore_lint.sh" >"$WORK/lint.log" 2>&1
}

# Nanosecond resolution: a rebuild that lands in the same second as the one
# before it still has to be visible, or "was this recompiled?" is a coin toss.
mtime() { stat -c %y "$1" 2>/dev/null; }

objects_present() { ls "$OBJ"/*.o >/dev/null 2>&1 && echo 1 || echo 0; }

# ---- 1. a pinned build, and what its dependency files know ---------------

mc_make -j"$JOBS" all; rc=$?
check "a pinned out-of-tree build succeeds" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
if [ ! -f "$WORK/libmeshcore.a" ]; then
    tail -20 "$WORK/make.log"
    echo "meshcore_build_deps: $checks check(s), $((failed + 1)) failure(s)"
    exit 1
fi

check "the build recorded the commits it compiled" \
      "$([ -f "$STAMP" ] && echo 1 || echo 0)"
check "and recorded that the pin was honoured" \
      "$(grep -qx 'pinned=yes' "$STAMP" && echo 1 || echo 0)"

# The -MMD/-MD point, stated directly: a vendored header reached with
# -isystem has to appear as a prerequisite, including across the two trees -
# a MeshCore object that includes a Crypto header is exactly the case -MMD
# dropped.
check "a vendored MeshCore header appears in the dependency file that uses it" \
      "$(grep -q "$WORK/RIFT/src/Mesh.h" "$OBJ/rift_Mesh.d" 2>/dev/null && echo 1 || echo 0)"
check "and a vendored Crypto header does too, from a MeshCore object" \
      "$(grep -q "$WORK/Crypto/libraries/Crypto/SHA256.h" "$OBJ/rift_Utils.d" 2>/dev/null && echo 1 || echo 0)"

# ---- 2. a changed vendor header rebuilds what included it ----------------
#
# The header touched here is deliberately one in the OTHER tree: Utils.cpp is
# MeshCore's, and it includes rweather's <SHA256.h> for the hash and the MAC.
# A same-tree header would not settle anything, because Mesh.cpp includes
# "Mesh.h" in quotes and the compiler finds that beside the source rather
# than as a system header - so -MMD listed it anyway. <SHA256.h> is reached
# through -isystem, which is exactly what -MMD dropped and what a stale
# object after a Crypto update would have come from.
#
# ed25519_fe.o is the control: C, in a third tree, including neither.

m_utils=$(mtime "$OBJ/rift_Utils.o")
m_fe=$(mtime "$OBJ/ed25519_fe.o")
touch "$WORK/Crypto/libraries/Crypto/SHA256.h"
mc_make -j"$JOBS" all; rc=$?
check "the build still succeeds after a vendored header changes" \
      "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "a changed vendored Crypto header rebuilds the MeshCore object that includes it" \
      "$([ "$(mtime "$OBJ/rift_Utils.o")" != "$m_utils" ] && echo 1 || echo 0)"
check "and leaves the objects that do not include it alone" \
      "$([ "$(mtime "$OBJ/ed25519_fe.o")" = "$m_fe" ] && echo 1 || echo 0)"

# ---- 3. a changed checkout rebuilds everything ---------------------------
# The case no dependency file can see: the whole tree moves to another
# revision. If only the files that happen to differ were recompiled,
# libmeshcore.a would hold objects from two revisions of the protocol.

m_sha=$(mtime "$OBJ/crypto_SHA256.o")
m_mesh=$(mtime "$OBJ/rift_Mesh.o")
git -C "$WORK/RIFT" checkout -q "$RIFT_OTHER"

mc_make -j"$JOBS" all; rc=$?
check "a checkout that does not match the pin stops the build" \
      "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and it stops it before anything was recompiled" \
      "$([ "$(mtime "$OBJ/rift_Mesh.o")" = "$m_mesh" ] && echo 1 || echo 0)"

# Exported and then unset rather than written as a one-shot prefix: bash
# keeps a variable assigned in front of a FUNCTION call afterwards, which
# would quietly leave every later build in this file unpinned.
export MESHCORE_ALLOW_UNPINNED=1
mc_make -j"$JOBS" all; rc=$?
unset MESHCORE_ALLOW_UNPINNED
check "MESHCORE_ALLOW_UNPINNED=1 builds it anyway" \
      "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "moving the checkout rebuilds objects from the other tree too, so no archive is mixed-revision" \
      "$([ "$(mtime "$OBJ/crypto_SHA256.o")" != "$m_sha" ] && echo 1 || echo 0)"
check "the stamp records that the pin was bypassed" \
      "$(grep -qx 'pinned=no' "$STAMP" && echo 1 || echo 0)"

mc_lint; rc=$?
check "the lint refuses to call an unpinned build pinned" \
      "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and says which claim failed" \
      "$(grep -q 'FAIL the build did not bypass the pin' "$WORK/lint.log" && echo 1 || echo 0)"
check "and that the checkout is not at the pinned commit" \
      "$(grep -q 'FAIL the MeshCore protocol source is checked out at the pinned commit' "$WORK/lint.log" && echo 1 || echo 0)"

git -C "$WORK/RIFT" checkout -q "$RIFT_PIN"
mc_make -j"$JOBS" all; rc=$?
check "restoring the pin builds again" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "and the stamp says pinned once more" \
      "$(grep -qx 'pinned=yes' "$STAMP" && echo 1 || echo 0)"

# ---- 4. parallel make cannot start before the pins are validated ---------
# From an empty object directory, with a tree at the wrong revision: `make -j`
# must produce no object at all. Before the stamp gated compilation, the pin
# check and the first compiles were simply two prerequisites of `all` and
# started together.

for tree in RIFT Crypto; do
    rm -rf "$OBJ" "$WORK/libmeshcore.a"
    if [ "$tree" = RIFT ]; then
        git -C "$WORK/RIFT" checkout -q "$RIFT_OTHER"
    else
        git -C "$WORK/Crypto" checkout -q "$CRYPTO_OTHER"
    fi

    mc_make -j"$JOBS" all
    rc=$?
    check "make -j against an unpinned vendor/$tree fails" \
          "$([ $rc -ne 0 ] && echo 1 || echo 0)"
    check "and compiles nothing at all before failing" \
          "$([ "$(objects_present)" = "0" ] && echo 1 || echo 0)"
    [ "$(objects_present)" = "1" ] && note "objects were produced: $(ls "$OBJ"/*.o | head -3)"

    git -C "$WORK/RIFT" checkout -q "$RIFT_PIN"
    git -C "$WORK/Crypto" checkout -q "$CRYPTO_PIN"
done

# ---- 5. the lint's vendor checks, in both trees --------------------------
# Rebuilt clean and pinned first, so the only thing failing below is the
# mutation each case introduces.

rm -rf "$OBJ" "$WORK/libmeshcore.a"
mc_make -j"$JOBS" all; rc=$?
check "a clean pinned build for the lint cases" "$([ $rc -eq 0 ] && echo 1 || echo 0)"

mc_lint; rc=$?
check "the lint passes on a clean pinned build" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
[ -s "$WORK/lint.log" ] && grep -q '^FAIL' "$WORK/lint.log" && grep '^FAIL' "$WORK/lint.log"

lint_fails_with() {   # $1 = human name, $2 = expected FAIL line fragment
    mc_lint
    rc=$?
    check "$1" \
          "$([ $rc -ne 0 ] && grep -q "FAIL $2" "$WORK/lint.log" && echo 1 || echo 0)"
    [ $rc -eq 0 ] && note "the lint passed when it should not have"
}

# A tracked Crypto source, edited. This is the half the lint used to skip
# entirely: AES-128, SHA-256 and the HMAC all come from this tree, so an edit
# here changes what goes on the air as much as one to Mesh.cpp would.
printf '\n/* not upstream */\n' >> "$WORK/Crypto/libraries/Crypto/SHA256.cpp"
lint_fails_with "an edited vendored Crypto source is caught" \
                "the vendored Crypto source has no local edits"
git -C "$WORK/Crypto" checkout -q -- libraries/Crypto/SHA256.cpp

# A tracked MeshCore source, edited.
printf '\n/* not upstream */\n' >> "$WORK/RIFT/src/Mesh.cpp"
lint_fails_with "an edited vendored MeshCore source is caught" \
                "the vendored MeshCore protocol source has no local edits"
git -C "$WORK/RIFT" checkout -q -- src/Mesh.cpp

# An untracked file dropped into each compiled tree.
: > "$WORK/RIFT/src/Injected.cpp"
lint_fails_with "an untracked file added to the MeshCore source is caught" \
                "nothing has been added to the vendored MeshCore protocol source"
rm -f "$WORK/RIFT/src/Injected.cpp"

: > "$WORK/Crypto/libraries/Crypto/Injected.cpp"
lint_fails_with "an untracked file added to the Crypto source is caught" \
                "nothing has been added to the vendored Crypto source"
rm -f "$WORK/Crypto/libraries/Crypto/Injected.cpp"

# A tree at the wrong revision, with the library already built - the case
# where the Makefile is never re-run and only the lint can notice.
git -C "$WORK/Crypto" checkout -q "$CRYPTO_OTHER"
lint_fails_with "a Crypto checkout away from the pin is caught" \
                "the Crypto source is checked out at the pinned commit"
git -C "$WORK/Crypto" checkout -q "$CRYPTO_PIN"

git -C "$WORK/RIFT" checkout -q "$RIFT_OTHER"
lint_fails_with "a MeshCore checkout away from the pin is caught" \
                "the MeshCore protocol source is checked out at the pinned commit"
git -C "$WORK/RIFT" checkout -q "$RIFT_PIN"

# And clean again afterwards, so a stuck mutation cannot be mistaken for a
# passing suite.
mc_lint; rc=$?
check "the lint passes again once every mutation is reverted" \
      "$([ $rc -eq 0 ] && echo 1 || echo 0)"

# ---- 6. nothing of this reached the real trees ---------------------------

real_dirty=$(git -C vendor/RIFT status --porcelain --ignore-submodules -- src lib/ed25519 2>/dev/null | grep '^??')
check "vendor/RIFT gained no untracked file from this test" \
      "$([ -z "$real_dirty" ] && echo 1 || echo 0)"
real_dirty=$(git -C vendor/Crypto status --porcelain --ignore-submodules -- libraries/Crypto 2>/dev/null | grep '^??')
check "vendor/Crypto gained no untracked file from this test" \
      "$([ -z "$real_dirty" ] && echo 1 || echo 0)"
check "vendor/RIFT is still at the pinned commit" \
      "$([ "$(git -C vendor/RIFT rev-parse HEAD)" = "$RIFT_PIN" ] && echo 1 || echo 0)"
check "vendor/Crypto is still at the pinned commit" \
      "$([ "$(git -C vendor/Crypto rev-parse HEAD)" = "$CRYPTO_PIN" ] && echo 1 || echo 0)"

# ---- 6. an exported tree, inside somebody else's repository ---------------
# The image package builds MeshCore from copies apply_to_sdk.sh exports into
# third_party/, which have no .git, and the package is built inside the SDK's
# own checkout. There `git -C third_party/RIFT rev-parse HEAD` does not fail:
# it answers with the SDK's commit. So the pin check reads git only when the
# tree is its own top level, and otherwise the .doors-pinned-commit
# apply_to_sdk.sh wrote after checking the source checkout against the pin.
PKG="$WORK/sdk"
mkdir -p "$PKG/third_party/RIFT/lib" "$PKG/third_party/Crypto/libraries"
git -C "$PKG" init -q && git -C "$PKG" -c user.email=t@t -c user.name=t commit -q --allow-empty -m sdk
SDK_HEAD=$(git -C "$PKG" rev-parse HEAD)
git -C "$WORK/RIFT" checkout -q "$RIFT_PIN"; git -C "$WORK/Crypto" checkout -q "$CRYPTO_PIN"
cp -a "$WORK/RIFT/src" "$PKG/third_party/RIFT/"
cp -a "$WORK/RIFT/lib/ed25519" "$PKG/third_party/RIFT/lib/"
cp -a "$WORK/Crypto/libraries/Crypto" "$PKG/third_party/Crypto/libraries/"
EXP_OBJ="$WORK/obj-export"
exp_check() { # vendor check only: nothing needs compiling to answer this
    rm -rf "$EXP_OBJ"
    make -C "$REPO/$LIB_DIR" RIFT_DIR="$PKG/third_party/RIFT" CRYPTO_REPO="$PKG/third_party/Crypto" \
         OBJDIR="$EXP_OBJ" check-vendor >"$WORK/export.log" 2>&1
}
exp_check; rc=$?
check "an exported tree with no pin record is refused" "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and the enclosing repository's commit is not taken for MeshCore's" \
      "$(grep -q "$SDK_HEAD" "$WORK/export.log" "$EXP_OBJ/vendor-id.stamp" 2>/dev/null && echo 0 || echo 1)"
echo "$RIFT_PIN" > "$PKG/third_party/RIFT/.doors-pinned-commit"
echo "$CRYPTO_PIN" > "$PKG/third_party/Crypto/.doors-pinned-commit"
exp_check; rc=$?
check "an exported tree carrying the pinned commits passes" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "and the stamp says where each commit came from" \
      "$(grep -qx 'rift_source=export' "$EXP_OBJ/vendor-id.stamp" 2>/dev/null &&
         grep -qx 'crypto_source=export' "$EXP_OBJ/vendor-id.stamp" && echo 1 || echo 0)"
check "and that the pin was honoured" \
      "$(grep -qx 'pinned=yes' "$EXP_OBJ/vendor-id.stamp" 2>/dev/null && echo 1 || echo 0)"
echo "$RIFT_OTHER" > "$PKG/third_party/RIFT/.doors-pinned-commit"
exp_check; rc=$?
check "an exported tree recorded at another commit is refused" "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and says which commit it is at" \
      "$(grep -q "is at $RIFT_OTHER" "$WORK/export.log" && echo 1 || echo 0)"

echo "meshcore_build_deps: $checks check(s), $failed failure(s)"
exit $((failed > 0))
