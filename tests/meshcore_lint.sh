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

LIB="$LIB_DIR/libmeshcore.a"
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
hook=$(nm --defined-only --format=posix "$LIB" 2>/dev/null | grep -c 'randomForceFailureForTest' || true)
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
    if [ -f "vendor/RIFT/src/$base" ] || [ -f "vendor/RIFT/src/helpers/$base" ]; then
        echo "$f shadows a vendored source of the same name"
    fi
done)
check "no file here shadows a vendored MeshCore source" \
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
if git -C vendor/RIFT rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    edited=$(git -C vendor/RIFT diff --numstat --ignore-cr-at-eol HEAD -- src lib/ed25519 2>/dev/null \
             | awk '$1 != 0 || $2 != 0 { print $3 }')
    check "the vendored MeshCore protocol source has no local edits" \
          "$([ -z "$edited" ] && echo 1 || echo 0)"
    [ -n "$edited" ] && printf '     edited: %s\n' "$edited"

    added=$(git -C vendor/RIFT ls-files --others --exclude-standard -- src lib/ed25519 2>/dev/null)
    check "nothing has been added to the vendored protocol source" \
          "$([ -z "$added" ] && echo 1 || echo 0)"
    [ -n "$added" ] && printf '     added: %s\n' "$added"
else
    echo "skip vendored-checkout edit check: vendor/RIFT is not a git work tree"
fi

# ---- 5. the pins are the ones the frame tool already proved on air --------
# tools/meshcore-frame built the frames that passed the accepted P0 gate
# (docs/hardware/MESHCORE_INTEROP_GATE.md). If this library were built from a
# different revision of the protocol, that evidence would no longer carry
# over to it, and nothing would say so.
for f in vendor_rift_commit.txt vendor_crypto_commit.txt; do
    a=$(tr -d ' \t\r\n' < "$LIB_DIR/$f" 2>/dev/null)
    b=$(tr -d ' \t\r\n' < "tools/meshcore-frame/$f" 2>/dev/null)
    check "$f matches tools/meshcore-frame" \
          "$([ -n "$a" ] && [ "$a" = "$b" ] && echo 1 || echo 0)"
done

# ---- 6. the library builds nothing into an image --------------------------
# Nothing here is installed yet, and the top-level Makefile must not have
# quietly started doing so.
check "no install rule mentions protocols/meshcore" \
      "$(grep -A 40 '^install:' Makefile | grep -q 'protocols/meshcore' && echo 0 || echo 1)"
check "protocols/meshcore is not in \$(BINS)" \
      "$(grep -E '^BINS[[:space:]]*:?=' -A 6 Makefile | grep -q 'protocols/meshcore' && echo 0 || echo 1)"

echo "meshcore_lint: $checks check(s), $failed failure(s)"
exit $((failed > 0))
