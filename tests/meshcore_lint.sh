#!/bin/bash
# protocols/meshcore: the boundary, checked statically.
#
# The value of a "portable protocol core" is entirely in what is NOT in it. A
# test suite that passes proves the protocol works; it does not prove that
# nobody has since reached into the library for a display driver, a RadioLib
# header or an Arduino runtime call. That is what this checks, by reading the
# built objects and the source rather than by trusting the Makefile.
#
# Run by protocols/meshcore/Makefile as part of `make meshcore-core-test`.
set -u
cd "$(dirname "$0")/.." || exit 1
LIB_DIR=protocols/meshcore
failed=0
checks=0
check() {
    checks=$((checks + 1))
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}

# Which trees and which build were checked. The defaults are what
# `make meshcore-core-test` uses; tests/meshcore_build_deps_test.sh overrides
# them to point this script at throwaway clones, which is how the negative
# cases below are exercised without touching the real vendored checkouts.
RIFT_DIR="${MESHCORE_RIFT_DIR:-vendor/RIFT}"
CRYPTO_REPO="${MESHCORE_CRYPTO_REPO:-vendor/Crypto}"
LIB="${MESHCORE_LIB:-$LIB_DIR/libmeshcore.a}"
BUILD_STAMP="${MESHCORE_BUILD_STAMP:-$LIB_DIR/build/vendor-id.stamp}"
if [ ! -f "$LIB" ]; then
    echo "NOT RUN meshcore_lint: $LIB has not been built."
    echo "        Build it with 'make meshcore-core' from the top of the repository."
    exit 77
fi

# ---- 1. what the library's objects are allowed to reference ---------------
# Undefined symbols in the archive are what it will demand from whoever links
# it. A MeshCore service should have to supply the C library, libm, libstdc++
# and its own radio adapter - and nothing else. Anything from LVGL, DRM,
# libgpiod, RadioLib or an Arduino core appearing here means the boundary has
# been crossed.
undef=$(nm --undefined-only --format=posix "$LIB" 2>/dev/null | awk '{print $1}' | sort -u)

forbidden_syms='^(lv_|drm|drmMode|gbm_|gpiod_|Radio(Lib)?_|SPI|Wire|esp_|xTask|vTask|pthread_create)'
hits=$(printf '%s\n' "$undef" | grep -E "$forbidden_syms" || true)
check "the library demands no LVGL, DRM, GPIO, RadioLib, SPI or RTOS symbol" \
      "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && printf '     unexpected: %s\n' "$hits"

# The Arduino core's timing and pin API specifically. MeshCore is written
# against it, so its absence is the thing that says the port really replaced
# it rather than shimming it in.
ard=$(printf '%s\n' "$undef" | grep -xE 'millis|micros|delay|delayMicroseconds|digitalWrite|digitalRead|pinMode|analogRead|yield' || true)
check "the library calls no Arduino timing or pin function" \
      "$([ -z "$ard" ] && echo 1 || echo 0)"
[ -n "$ard" ] && printf '     unexpected: %s\n' "$ard"

# The clock the port promises must be the one that is actually called.
check "the library uses clock_gettime for its clock" \
      "$(printf '%s\n' "$undef" | grep -qE '^(clock_gettime|__clock_gettime64)$' && echo 1 || echo 0)"

# ---- 2. the test hook is not in the shipped library -----------------------
# port/mc_rng.cpp is compiled twice: once plain into libmeshcore.a, once with
# MC_RNG_TEST_HOOKS into the test binaries. The same arrangement sysd, netd
# and pos-wave use. If the hook ever reached the library, a caller could
# switch the CSPRNG off.
# Every one of them, not just the first: port/mc_rng.cpp now carries several
# seams (force a failure, make getrandom behave as an older kernel, point the
# fallback somewhere else, count which source served the bytes), and a check
# that names only one of them would pass while the others shipped.
hook=$(nm --defined-only --format=posix "$LIB" 2>/dev/null | grep -c 'ForTest' || true)
check "the shipped library contains no RNG test hook" "$([ "$hook" = "0" ] && echo 1 || echo 0)"

# And the hook is compiled out by default, not merely unused.
check "the RNG hook is behind MC_RNG_TEST_HOOKS" \
      "$(grep -q '^#ifdef MC_RNG_TEST_HOOKS' $LIB_DIR/port/mc_rng.cpp && echo 1 || echo 0)"

# ---- 3. what the port's own sources include -------------------------------
# The port is the only first-party code here. It may include the C library,
# the vendored MeshCore headers and its own; anything else is scope creep.
bad_inc=$(grep -rhn '^#include' $LIB_DIR/port $LIB_DIR/compat 2>/dev/null \
          | grep -oE '<[^>]+>|"[^"]+"' \
          | grep -E 'lvgl|drm|gpiod|RadioLib|SX126|Arduino\.h|freertos|esp_' || true)
# compat/Arduino.h is this port's OWN stand-in, so a self-include of it is
# fine; a port source reaching for a real Arduino core is not.
bad_inc=$(printf '%s\n' "$bad_inc" | grep -v '^$' || true)
check "the port includes no display, radio driver or RTOS header" \
      "$([ -z "$bad_inc" ] && echo 1 || echo 0)"
[ -n "$bad_inc" ] && printf '     unexpected: %s\n' "$bad_inc"

# ---- 4. no vendored file was edited ---------------------------------------
# Upstream discipline: this library compiles vendor/RIFT and vendor/Crypto as
# their authors wrote them. Nothing under protocols/meshcore may be a copy of
# a vendored source, because a copy is an edit nobody will notice.
copies=$(find $LIB_DIR -name '*.cpp' -o -name '*.h' | while read -r f; do
    base=$(basename "$f")
    if [ -f "$RIFT_DIR/src/$base" ] || [ -f "$RIFT_DIR/src/helpers/$base" ] \
       || [ -f "$CRYPTO_REPO/libraries/Crypto/$base" ]; then
        echo "$f shadows a vendored source of the same name"
    fi
done)
check "no file here shadows a vendored MeshCore or Crypto source" \
      "$([ -z "$copies" ] && echo 1 || echo 0)"
[ -n "$copies" ] && printf '     %s\n' "$copies"

# And the protocol tree this library compiles from must be upstream's bytes.
#
# Scoped to src/ and lib/ed25519 - the protocol source - rather than the whole
# 909-file reference clone, because that is what is compiled and an edit
# anywhere else cannot reach this library.
#
# Compared with `git diff --numstat --ignore-cr-at-eol`, not with
# `git status --porcelain`, which is not usable here: this repository is a
# Windows checkout that is also built through WSL, and the two gits disagree
# about line endings. Windows git normalises with core.autocrlf and calls the
# tree clean; WSL git, with autocrlf off, sees a CRLF worktree against LF
# blobs and calls all 909 files modified. Neither answer is a signal.
# Ignoring the carriage return compares what the compiler will actually read,
# and gives the same verdict from either side.
# Both trees, to the same standard. vendor/Crypto is not "the support
# library": AES-128, SHA-256, the HMAC and Ed25519::verify all come from it,
# so an edit there changes what this library puts on the air exactly as much
# as an edit to Mesh.cpp would. Checking only RIFT, as this did, left half of
# the compiled bytes unchecked.
#
# The paths after each repository are the ones this library actually
# compiles, not the whole clone: an edit outside them cannot reach
# libmeshcore.a, and flagging it would be noise.
check_tree_clean() {
    label=$1; repo=$2; shift 2
    if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "skip $label edit check: $repo is not a git work tree"
        return
    fi
    edited=$(git -C "$repo" diff --numstat --ignore-cr-at-eol HEAD -- "$@" 2>/dev/null \
             | awk '$1 != 0 || $2 != 0 { print $3 }')
    check "the vendored $label source has no local edits" \
          "$([ -z "$edited" ] && echo 1 || echo 0)"
    [ -n "$edited" ] && printf '     edited: %s\n' "$edited"

    added=$(git -C "$repo" ls-files --others --exclude-standard -- "$@" 2>/dev/null)
    check "nothing has been added to the vendored $label source" \
          "$([ -z "$added" ] && echo 1 || echo 0)"
    [ -n "$added" ] && printf '     added: %s\n' "$added"
}

check_tree_clean "MeshCore protocol" "$RIFT_DIR" src lib/ed25519
check_tree_clean "Crypto" "$CRYPTO_REPO" libraries/Crypto

# ---- 5. the pins ---------------------------------------------------------
#
# Three separate claims, and the lint used to make only the weakest of them.
#
# a) The pin files agree with tools/meshcore-frame. That tool built the
#    frames that passed the accepted P0 on-air gate
#    (docs/hardware/MESHCORE_INTEROP_GATE.md); if this library were built
#    from a different revision, that evidence would stop carrying over.
#
# b) The trees on disk are ACTUALLY AT the pinned commits. Checking only (a)
#    compared one text file against another and said nothing at all about the
#    source the compiler read - vendor/RIFT could be at any revision and this
#    script would still have printed "ok".
#
# c) The build that produced $LIB honoured the pin. MESHCORE_ALLOW_UNPINNED=1
#    is a deliberate escape hatch, but a library built through it must not be
#    reported as pinned; the build stamp is what tells the two apart.
for f in vendor_rift_commit.txt vendor_crypto_commit.txt; do
    a=$(tr -d ' \t\r\n' < "$LIB_DIR/$f" 2>/dev/null)
    b=$(tr -d ' \t\r\n' < "tools/meshcore-frame/$f" 2>/dev/null)
    check "$f matches tools/meshcore-frame" \
          "$([ -n "$a" ] && [ "$a" = "$b" ] && echo 1 || echo 0)"
done

RIFT_PIN=$(tr -d ' \t\r\n' < "$LIB_DIR/vendor_rift_commit.txt" 2>/dev/null)
CRYPTO_PIN=$(tr -d ' \t\r\n' < "$LIB_DIR/vendor_crypto_commit.txt" 2>/dev/null)

check_tree_pinned() {
    label=$1; repo=$2; pin=$3
    head=$(git -C "$repo" rev-parse HEAD 2>/dev/null | tr -d ' \t\r\n')
    check "$label is checked out at the pinned commit" \
          "$([ -n "$pin" ] && [ "$head" = "$pin" ] && echo 1 || echo 0)"
    [ "$head" != "$pin" ] && printf '     %s is at %s, pinned at %s\n' \
        "$repo" "${head:-an unreadable commit}" "${pin:-nothing}"
}

check_tree_pinned "the MeshCore protocol source" "$RIFT_DIR" "$RIFT_PIN"
check_tree_pinned "the Crypto source" "$CRYPTO_REPO" "$CRYPTO_PIN"

# And what the build itself recorded. protocols/meshcore/Makefile writes this
# stamp before it compiles anything; `pinned=no` means MESHCORE_ALLOW_UNPINNED
# was used to build against whatever happened to be checked out.
if [ -f "$BUILD_STAMP" ]; then
    stamp_pinned=$(sed -n 's/^pinned=//p' "$BUILD_STAMP" | tr -d ' \t\r\n')
    stamp_rift=$(sed -n 's/^rift_head=//p' "$BUILD_STAMP" | tr -d ' \t\r\n')
    stamp_crypto=$(sed -n 's/^crypto_head=//p' "$BUILD_STAMP" | tr -d ' \t\r\n')
    check "the build did not bypass the pin with MESHCORE_ALLOW_UNPINNED" \
          "$([ "$stamp_pinned" = "yes" ] && echo 1 || echo 0)"
    [ "$stamp_pinned" != "yes" ] && printf '     %s says pinned=%s\n' \
        "$BUILD_STAMP" "${stamp_pinned:-unrecorded}"
    check "the library was built from the commits checked out now" \
          "$([ "$stamp_rift" = "$RIFT_PIN" ] && [ "$stamp_crypto" = "$CRYPTO_PIN" ] && echo 1 || echo 0)"
else
    check "the build recorded which commits it compiled" 0
    printf '     %s is missing - rebuild with `make meshcore-core`\n' "$BUILD_STAMP"
fi

# ---- 6. the library builds nothing into an image --------------------------
# Nothing here is installed yet, and the top-level Makefile must not have
# quietly started doing so.
check "no install rule mentions protocols/meshcore" \
      "$(grep -A 40 '^install:' Makefile | grep -q 'protocols/meshcore' && echo 0 || echo 1)"
check "protocols/meshcore is not in \$(BINS)" \
      "$(grep -E '^BINS[[:space:]]*:?=' -A 6 Makefile | grep -q 'protocols/meshcore' && echo 0 || echo 1)"

echo "meshcore_lint: $checks check(s), $failed failure(s)"
exit $((failed > 0))
