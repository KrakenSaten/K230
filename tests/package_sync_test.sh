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
# The PocketTimber art is the one part of docs/ the shell build consumes; it
# travels by its own pathspec, published the same way.
ART_LINE=$(grep -m1 '^POCKETOS_PKG_ART_PATHSPEC=' "$APPLY")
check "apply_to_sdk.sh publishes its art pathspec" $([ -n "$ART_LINE" ] && echo 1 || echo 0)
eval "$ART_LINE"
# shellcheck disable=SC2086
git archive --format=tar HEAD -- ${POCKETOS_PKG_ART_PATHSPEC} | tar -x -C "$SRC"

present() { [ -e "$SRC/$1" ] && echo 1 || echo 0; }
for f in Makefile VERSION core/pocketipc/pocketipc.c core/pocketlog/pocketlog.c \
         core/pocketsys.c services/radiod/main.c services/sysd/main.c \
         ui/shell/shell.c ui/pocketui/pos_theme_table.h \
         apps/fleet/fleet_app.c tools/pos/pos.c tools/supervise/pos-supervise \
         tools/hwcheck/hwcheck.sh tools/hwcheck/spixfer.c \
         apps/timber/timber_app.c apps/timber/engine/timber_rules.c \
         docs/design/timber-art/tools/png2lvgl.py docs/design/timber-art/tools/pngio.py \
         docs/design/timber-art/rendered/anchors.json; do
    check "package carries $f" $(present "$f")
done
check "package carries the rendered sprites" \
      $([ "$(find "$SRC/docs/design/timber-art/rendered" -name '*.png' 2>/dev/null | wc -l)" -ge 4 ] && echo 1 || echo 0)
# The art is exactly what ui/shell/CMakeLists.txt reads, at the path it reads
# it from, so the package build converts it rather than falling back to the
# placeholder blocks.
check "shell CMake reads the art from the exported path" \
      $(grep -q 'docs/design/timber-art/rendered' "$SRC/ui/shell/CMakeLists.txt" && echo 1 || echo 0)
check "the package build depends on Buildroot's host python3" \
      $(grep -q 'host-python3' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)

absent() { # <label> <find expression...>
    hits=$(find "$SRC" "${@:2}" 2>/dev/null | head -5)
    check "$1" $([ -z "$hits" ] && echo 1 || echo 0)
    [ -n "$hits" ] && echo "$hits" | sed "s#^$SRC/#     #"
}
absent "package carries no object files"        -name '*.o'
absent "package carries no dependency files"    -name '*.d'
absent "package carries no static libraries"    -name '*.a'
absent "package carries no docs tree beyond the timber art" -type f -path '*/docs/*' ! -path '*/docs/design/timber-art/*'
absent "the timber art carries no Blender sources or studies" -type f -path '*/docs/design/timber-art/*' \( -name '*.blend' -o -path '*/study/*' \)
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
for f in etc/init.d/S50sysd etc/init.d/S60radiod etc/init.d/S90pocketos-shell \
         etc/default/telnet etc/pocketos/settings.conf; do
    check "overlay carries $f" $([ -e "$OVL/$f" ] && echo 1 || echo 0)
done
if [ "$MODES" -eq 1 ]; then
    # These three are what BusyBox rcS executes.
    for f in etc/init.d/S50sysd etc/init.d/S60radiod etc/init.d/S90pocketos-shell; do
        check "overlay $f is executable" $([ -x "$OVL/$f" ] && echo 1 || echo 0)
    done
    for f in etc/default/telnet etc/pocketos/settings.conf; do
        check "overlay $f is not executable" $([ -x "$OVL/$f" ] && echo 0 || echo 1)
    done
fi

# ---- the bench-deploy path ----------------------------------------------
#
# deploy.sh pushes the same binaries and init scripts to a running board over
# SSH, without reflashing. A service that reaches the image but not deploy.sh
# leaves a bench deployment running the old one and says nothing: S50sysd was
# added to the overlay in v0.0.7 block 2a and to deploy.sh only afterwards.
# Every init script the overlay carries must be sent, stopped and started.
# Read from HEAD like everything else here, so this is a fact about the
# commit rather than about the working tree.
git archive --format=tar HEAD -- platforms/k230/scripts/deploy.sh | tar -x -C "$TMP"
DEPLOY="$TMP/platforms/k230/scripts/deploy.sh"
check "deploy.sh extracted" $([ -f "$DEPLOY" ] && echo 1 || echo 0)
for f in "$OVL"/etc/init.d/S*; do
    [ -e "$f" ] || continue
    s=$(basename "$f")
    check "deploy.sh sends $s"  $(grep -q "etc/init.d/$s" "$DEPLOY" && echo 1 || echo 0)
    check "deploy.sh stops $s"  $(grep -q "^/etc/init.d/$s stop" "$DEPLOY" && echo 1 || echo 0)
    check "deploy.sh starts $s" $(grep -q "^/etc/init.d/$s start" "$DEPLOY" && echo 1 || echo 0)
done

# Ownership: a bench deployment must land root-owned, like the flashed image.
# Before this was set, every file arrived owned by the build host's uid (1000,
# VERIFIED on unit A, docs/hardware/V0.0.7_BLOCK2A_SMOKE.md), which would let a
# uid-1000 process rewrite an init script that rcS runs as root. Both halves
# are checked: that deploy.sh asks for it, and that asking works with the tar
# on this host.
check "deploy.sh creates the archive as uid/gid 0" \
      $(grep -q -- '--owner=0 --group=0 --numeric-owner' "$DEPLOY" && echo 1 || echo 0)
printf 'x' > "$TMP/ownprobe"
ownline=$(tar -C "$TMP" --owner=0 --group=0 --numeric-owner -cf - ownprobe 2>/dev/null |
          tar -tvf - 2>/dev/null)
check "those flags produce uid/gid 0 with this tar" \
      $(printf '%s' "$ownline" | grep -qE '(^| )0/0( |$)' && echo 1 || echo 0)
[ -n "$ownline" ] || echo "     tar produced no listing"

# ---- the dirty-worktree guard -------------------------------------------
#
# The package is HEAD, so a dirty tree means the build is not what the
# developer is looking at - and the loop that breaks is edit, build, deploy,
# test on hardware, where the result reads as evidence about the edit and is
# evidence about HEAD. apply_to_sdk.sh refuses by default and takes
# POCKETOS_ALLOW_DIRTY_BUILD=1 as a deliberate override.
#
# Exercised against the real script in a scratch repository with a stub vendor
# tree, so nothing here can touch an SDK. The guard runs before any of the
# vendor tree is used and before "[1/5]" is echoed, so "did it get past the
# guard" is read off that marker; the run is expected to fail afterwards, on
# the stub, and that later failure is not what is being measured.

GUARD="$TMP/guard"
mkdir -p "$GUARD/repo/platforms/k230/scripts" "$GUARD/vendor/k230_bsp/scripts" \
         "$GUARD/vendor/k230_linux_sdk"
cp platforms/k230/scripts/apply_to_sdk.sh "$GUARD/repo/platforms/k230/scripts/"
cp platforms/k230/vendor_bsp_commit.txt platforms/k230/vendor_sdk_commit.txt \
   "$GUARD/repo/platforms/k230/"
cp VERSION "$GUARD/repo/"
printf 'committed\n' > "$GUARD/repo/tracked.txt"
git -C "$GUARD/repo" init -q
git -C "$GUARD/repo" add -A
git -C "$GUARD/repo" -c user.name=t -c user.email=t@t commit -qm scratch
printf '#!/bin/sh\nexit 0\n' > "$GUARD/vendor/k230_bsp/scripts/apply.sh"
chmod +x "$GUARD/vendor/k230_bsp/scripts/apply.sh"
for d in "$GUARD/vendor" "$GUARD/vendor/k230_linux_sdk"; do
    git -C "$d" init -q
    git -C "$d" -c user.name=t -c user.email=t@t commit -q --allow-empty -m stub
done

# Pin drift is allowed throughout: the stub vendor is not the pinned tree, and
# the pin is not what these cases are about.
run_guard() { # [env assignment]; leaves combined output in $GUARD/out
    # shellcheck disable=SC2086  # an empty $1 must expand to no argument
    ( cd "$GUARD/repo" && env ${1:+"$1"} POCKETOS_ALLOW_PIN_DRIFT=1 \
        bash platforms/k230/scripts/apply_to_sdk.sh "$GUARD/vendor" ) \
        > "$GUARD/out" 2>&1
    echo $?
}
said() { grep -q "$1" "$GUARD/out" && echo 1 || echo 0; }

rc=$(run_guard "")
check "a clean worktree is not refused" \
      $([ "$(said 'ERROR: the working tree is dirty')" = 0 ] && echo 1 || echo 0)
check "a clean worktree is not warned about" \
      $([ "$(said 'WARNING: the working tree is dirty')" = 0 ] && echo 1 || echo 0)
check "a clean worktree gets past the guard" $(said '\[1/5\]')

# Now dirty it, by modifying a tracked file rather than adding an ignored one.
printf 'uncommitted\n' > "$GUARD/repo/tracked.txt"

rc=$(run_guard "")
check "a dirty worktree fails" $([ "$rc" != "0" ] && echo 1 || echo 0)
check "the refusal names the packaging mechanism" $(said 'git archive HEAD')
check "the refusal says uncommitted changes are excluded" $(said 'would NOT be included')
check "the refusal names the override" $(said 'POCKETOS_ALLOW_DIRTY_BUILD=1')
check "the refusal happens before any packaging" \
      $([ "$(said '\[1/5\]')" = 0 ] && echo 1 || echo 0)
check "the refusal lists what is dirty" $(said 'tracked.txt')

rc=$(run_guard "POCKETOS_ALLOW_DIRTY_BUILD=1")
check "the override gets past the guard" $(said '\[1/5\]')
check "the override warns rather than passing silently" \
      $(said 'WARNING: the working tree is dirty')
check "the override states that uncommitted changes are excluded" $(said 'are NOT')
# The provenance summary prints only on a run that finishes, which the stub
# vendor cannot reach, so it is checked where it is written instead. What
# matters about it is placement as much as content: a summary in the header
# would be the very thing it exists to survive.
sum_line=$(grep -n '^echo "Provenance"' "$APPLY" | cut -d: -f1)
pkg_line=$(grep -n 'archive --format=tar HEAD' "$APPLY" | head -1 | cut -d: -f1)
done_line=$(grep -n '^echo "Done\. Build with' "$APPLY" | cut -d: -f1)
check "apply_to_sdk.sh prints a provenance summary" \
      $([ -n "$sum_line" ] && echo 1 || echo 0)
check "the summary comes after packaging, not in the header" \
      $([ -n "$sum_line" ] && [ -n "$pkg_line" ] && [ "$sum_line" -gt "$pkg_line" ] && echo 1 || echo 0)
check "the summary is the last thing said before Done" \
      $([ -n "$sum_line" ] && [ -n "$done_line" ] && [ "$sum_line" -lt "$done_line" ] && echo 1 || echo 0)
check "the summary names the packaged commit" \
      $(grep -q 'Packaged source : HEAD' "$APPLY" && echo 1 || echo 0)
check "the summary names the worktree state" \
      $(grep -q 'Source worktree : ' "$APPLY" && echo 1 || echo 0)
check "the summary calls out the override when it was used" \
      $(grep -q 'Dirty override  : POCKETOS_ALLOW_DIRTY_BUILD=1' "$APPLY" && echo 1 || echo 0)
check "the summary states that the worktree is never packaged" \
      $(grep -q 'working tree is never packaged' "$APPLY" && echo 1 || echo 0)

# The override must not widen what is packaged. Two halves: the script has no
# archive that exports anything but HEAD, and HEAD really does exclude the
# edit that made the tree dirty.
notHEAD=$(grep 'git .*archive' "$APPLY" | grep -vc 'HEAD' || true)
check "every git archive in apply_to_sdk.sh exports HEAD" \
      $([ "$notHEAD" = "0" ] && echo 1 || echo 0)
got=$(git -C "$GUARD/repo" archive --format=tar HEAD -- tracked.txt | tar -xO)
check "an override build exports HEAD, not the uncommitted edit" \
      $([ "$got" = "committed" ] && echo 1 || echo 0)

echo "package_sync_test: $failed failure(s)"
exit $((failed > 0))
