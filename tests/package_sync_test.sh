#!/bin/bash
# What apply_to_sdk.sh hands to Buildroot (M1).
#
# Two trees leave this repository for the SDK: the package source and the
# rootfs overlay. Both are produced with `git archive`, so both are a function
# of the commit rather than of whatever the build host's working tree and
# filesystem happen to hold. This test performs the same two extractions and
# checks what came out.
#
# The pathspec is read from apply_to_sdk.sh rather than repeated here, so the
# two cannot drift apart without this test failing.
#
# Mode checks need a filesystem that keeps modes; the extraction target is
# probed first, because a checkout under a Windows drive mount reports every
# file as executable and would pass regardless.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
APPLY=platforms/k230/scripts/apply_to_sdk.sh
failed=0

check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "note: not a git checkout, package sync cannot be exercised here"
    echo "package_sync_test: 0 failure(s)"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Does the extraction target keep modes at all?
probe="$TMP/.probe"
: > "$probe"; chmod 0644 "$probe"
MODES=0
if [ ! -x "$probe" ]; then chmod 0755 "$probe"; [ -x "$probe" ] && MODES=1; fi
rm -f "$probe"
[ "$MODES" -eq 1 ] || echo "note: $TMP does not keep file modes; mode checks skipped"

# ---- the package source -------------------------------------------------

PATHSPEC_LINE=$(grep -m1 '^POCKETOS_PKG_PATHSPEC=' "$APPLY")
check "apply_to_sdk.sh publishes its package pathspec" $([ -n "$PATHSPEC_LINE" ] && echo 1 || echo 0)
eval "$PATHSPEC_LINE"
SRC="$TMP/src"; mkdir -p "$SRC"
# shellcheck disable=SC2086
git archive --format=tar HEAD -- ${POCKETOS_PKG_PATHSPEC} | tar -x -C "$SRC"
check "package source extracted" $([ -f "$SRC/Makefile" ] && echo 1 || echo 0)

present() { [ -e "$SRC/$1" ] && echo 1 || echo 0; }
for f in Makefile VERSION core/pocketipc/pocketipc.c core/pocketlog/pocketlog.c \
         services/radiod/main.c ui/shell/shell.c ui/pocketui/pos_theme_table.h \
         apps/fleet/fleet_app.c tools/pos/pos.c tools/supervise/pos-supervise \
         tools/hwcheck/hwcheck.sh; do
    check "package carries $f" $(present "$f")
done

absent() { # <label> <find expression...>
    hits=$(find "$SRC" "${@:2}" 2>/dev/null | head -5)
    check "$1" $([ -z "$hits" ] && echo 1 || echo 0)
    [ -n "$hits" ] && echo "$hits" | sed "s#^$SRC/#     #"
}
absent "package carries no object files"        -name '*.o'
absent "package carries no dependency files"    -name '*.d'
absent "package carries no static libraries"    -name '*.a'
absent "package carries no docs tree"           -path '*/docs/*'
absent "package carries no platforms tree"      -path '*/platforms/*'
absent "package carries no vendor tree"         -path '*/vendor/*'
absent "package carries no out tree"            -path '*/out/*'
absent "package carries no repository metadata" -name '.git' -o -name '*.orig' -o -name '*.rej'
absent "package carries no CMake build tree"    -name 'CMakeCache.txt'
# Anything executable and not a script is a compiled artefact that escaped.
if [ "$MODES" -eq 1 ]; then
    bins=$(find "$SRC" -type f -perm -u+x ! -name '*.sh' ! -name 'pos-supervise' \
                ! -name '*.py' 2>/dev/null | head -5)
    check "package carries no stray executables" $([ -z "$bins" ] && echo 1 || echo 0)
    [ -n "$bins" ] && echo "$bins" | sed "s#^$SRC/#     #"
fi

# ---- the rootfs overlay -------------------------------------------------

OVL="$TMP/overlay"; mkdir -p "$OVL"
git archive --format=tar HEAD -- platforms/k230/rootfs_overlay |
    tar -x --strip-components=3 -C "$OVL"
check "overlay extracted at the rootfs root" $([ -d "$OVL/etc/init.d" ] && echo 1 || echo 0)
for f in etc/init.d/S60radiod etc/init.d/S90pocketos-shell etc/default/telnet \
         etc/pocketos/settings.conf; do
    check "overlay carries $f" $([ -e "$OVL/$f" ] && echo 1 || echo 0)
done
if [ "$MODES" -eq 1 ]; then
    # These two are what BusyBox rcS executes.
    for f in etc/init.d/S60radiod etc/init.d/S90pocketos-shell; do
        check "overlay $f is executable" $([ -x "$OVL/$f" ] && echo 1 || echo 0)
    done
    for f in etc/default/telnet etc/pocketos/settings.conf; do
        check "overlay $f is not executable" $([ -x "$OVL/$f" ] && echo 0 || echo 1)
    done
fi

echo "package_sync_test: $failed failure(s)"
exit $((failed > 0))
