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
CONF=k230_pocketos_defconfig
failed=0

check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# This suite is a release gate, and it cannot be run without the git checkout
# it is about. Saying "0 failure(s)" here and exiting 0 was worse than useless:
# it made `make test` green while this whole file did nothing. That happened
# for real - a Windows git worktree seen from WSL, where .git is a file holding
# a path WSL cannot resolve - and the suite reported success through a release
# that had actually broken three of its checks (fixed in 5b6bd80).
#
# So a gate that cannot run is NOT RUN, not PASS. Exit 77 is the automake
# convention for "skipped", and it is non-zero, so `make test` stops on it.
# POCKETOS_ALLOW_SKIPPED_GATES=1 is the deliberate override, in the shape
# POCKETOS_ALLOW_DIRTY_BUILD already established: it says so loudly and it
# still does not claim the gate passed.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "NOT RUN package_sync_test: git could not read a checkout here, so the package sync this suite exists to prove cannot be exercised."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 set: continuing unverified."
        exit 0
    fi
    echo "        Run it from a checkout git can read, or set"
    echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 to accept an unverified gate."
    exit 77
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
         tools/wave/pos_wave.c core/pocketaudio/pocketaudio.c apps/wave/wave_modem.cpp \
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
for f in etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S65meshcored \
         etc/init.d/S90doors-shell etc/default/telnet etc/pocketos/settings.conf logo.xrgb; do
    check "overlay carries $f" $([ -e "$OVL/$f" ] && echo 1 || echo 0)
done
# The boot splash. The vendor post-image.sh copies rootfs_overlay/logo.xrgb to
# the boot partition, and U-Boot skips any size but 568 x 1232 x 4, so an
# archive that mangled it (an EOL filter on a binary) would lose the splash
# without an error anywhere. tests/boot_splash_test.sh checks its content.
check "overlay logo.xrgb is exactly 2,799,104 bytes as archived" \
      $([ "$(stat -c %s "$OVL/logo.xrgb" 2>/dev/null)" = "2799104" ] && echo 1 || echo 0)
check "overlay logo.xrgb is archived byte-for-byte" \
      $(cmp -s "$OVL/logo.xrgb" <(git show HEAD:platforms/k230/rootfs_overlay/logo.xrgb) && echo 1 || echo 0)
if [ "$MODES" -eq 1 ]; then
    # These five are what BusyBox rcS executes.
    for f in etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S65meshcored \
             etc/init.d/S90doors-shell; do
        check "overlay $f is executable" $([ -x "$OVL/$f" ] && echo 1 || echo 0)
    done
    for f in etc/default/telnet etc/pocketos/settings.conf logo.xrgb; do
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
#
# The stops are asserted by what they have to achieve, not by the shape of
# the source. They were three literal lines until a1d34c6 made them a loop
# with a failure branch, and the assertions that pinned the old shape then
# failed on a correct script - a red suite on pristine master, caused by the
# test rather than by the code. What matters is behavioural and is checked
# below: every service is stopped, the stop's result is tested, and a failure
# ends the deployment before a single file is unpacked over a running binary.
git archive --format=tar HEAD -- platforms/k230/scripts/deploy.sh | tar -x -C "$TMP"
DEPLOY="$TMP/platforms/k230/scripts/deploy.sh"
check "deploy.sh extracted" $([ -f "$DEPLOY" ] && echo 1 || echo 0)
# What deploy.sh runs on the board. The stop-then-unpack order lives here, so
# the order is read off this text rather than off the file as a whole: the
# service names also appear in the local tar's file list, which proves nothing
# about what the board does with them.
PAYLOAD="$TMP/remote-payload"
awk '/set -e$/,/^echo "Done\."/' "$DEPLOY" | sed '$d' > "$PAYLOAD"
check "the remote payload was extracted" \
      $([ -s "$PAYLOAD" ] && grep -q 'set -e' "$PAYLOAD" && echo 1 || echo 0)

# The line the new files land on. Everything that must happen first is above it.
UNPACK=$(grep -n '^tar -C / -xf -' "$PAYLOAD" | head -1 | cut -d: -f1)
check "the payload unpacks the archive" $([ -n "$UNPACK" ] && echo 1 || echo 0)
STOPS="$TMP/stop-stanza"
head -n "$(( ${UNPACK:-1} - 1 ))" "$PAYLOAD" > "$STOPS"

for f in "$OVL"/etc/init.d/S*; do
    [ -e "$f" ] || continue
    s=$(basename "$f")
    check "deploy.sh sends $s"  $(grep -q "etc/init.d/$s" "$DEPLOY" && echo 1 || echo 0)
    # Named in the stanza that runs before the unpack, whether the stops are a
    # loop over a list or a line each. A service missing from it is a service
    # whose running binary is replaced underneath it.
    check "deploy.sh stops $s before unpacking" \
          $(grep -qw -- "$s" "$STOPS" && echo 1 || echo 0)
    check "deploy.sh starts $s" $(grep -q "^/etc/init.d/$s start" "$DEPLOY" && echo 1 || echo 0)
done

# The guarantee itself, in three parts: a stop is attempted, its result is
# tested, and a failure ends the deployment before anything is written. The
# `|| true` check is the regression this replaced - it hid every failed stop
# and left boards running a mixture of old services and new files.
check "the stanza invokes the init scripts' stop" \
      $(grep -qE '/etc/init\.d/[^ ]+ stop' "$STOPS" && echo 1 || echo 0)
check "and tests whether the stop succeeded" \
      $(grep -qE 'if +! +[^|]*/etc/init\.d/[^ ]+ stop|/etc/init\.d/[^ ]+ stop[^|]*\|\|' "$STOPS" &&
        echo 1 || echo 0)
check "no stop has its failure discarded" \
      $(grep -qE '/etc/init\.d/[^ ]+ stop.*\|\|[[:space:]]*true' "$STOPS" && echo 0 || echo 1)
check "a failed stop aborts before the unpack" \
      $(grep -qE '^[[:space:]]*exit +[1-9]' "$STOPS" && echo 1 || echo 0)

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
# The package builds meshcored, and the script refuses before anything else
# when the notices lack its three entries - so the scratch repo carries the
# real list, or every guard below would be measured behind that refusal.
mkdir -p "$GUARD/repo/third_party/notices"
cp third_party/notices/SOURCES "$GUARD/repo/third_party/notices/"
# The script refuses to run if a first-party build input is missing from the
# snapshot, so the scratch repo has to carry them the way a real one does.
mkdir -p "$GUARD/repo/platforms/k230/configs" "$GUARD/repo/platforms/k230/package/pocketos"
cp "platforms/k230/configs/$CONF" "$GUARD/repo/platforms/k230/configs/"
cp platforms/k230/package/pocketos/Config.in platforms/k230/package/pocketos/pocketos.mk \
   platforms/k230/package/pocketos/pocketos.hash "$GUARD/repo/platforms/k230/package/pocketos/"
# RadioLib is pinned now, so the scratch repo needs a checkout and a pin that
# names it - the same two things a real build needs.
mkdir -p "$GUARD/repo/vendor/RadioLib"
printf 'stub
' > "$GUARD/repo/vendor/RadioLib/README.md"
git -C "$GUARD/repo/vendor/RadioLib" init -q
git -C "$GUARD/repo/vendor/RadioLib" add -A
git -C "$GUARD/repo/vendor/RadioLib" -c user.name=t -c user.email=t@t commit -qm stub
git -C "$GUARD/repo/vendor/RadioLib" rev-parse HEAD     > "$GUARD/repo/platforms/k230/vendor_radiolib_commit.txt"
# ggwave is pinned the same way (pos-wave), so it needs the same two things.
mkdir -p "$GUARD/repo/vendor/ggwave"
printf 'stub\n' > "$GUARD/repo/vendor/ggwave/README.md"
git -C "$GUARD/repo/vendor/ggwave" init -q
git -C "$GUARD/repo/vendor/ggwave" add -A
git -C "$GUARD/repo/vendor/ggwave" -c user.name=t -c user.email=t@t commit -qm stub
git -C "$GUARD/repo/vendor/ggwave" rev-parse HEAD > "$GUARD/repo/platforms/k230/vendor_ggwave_commit.txt"
# MeshCore (vendor/RIFT) and Crypto (vendor/Crypto) are pinned the same way
# (meshcored), by protocols/meshcore, so they need the same two things too.
mkdir -p "$GUARD/repo/protocols/meshcore"
for t in RIFT Crypto; do
    mkdir -p "$GUARD/repo/vendor/$t"
    printf 'stub\n' > "$GUARD/repo/vendor/$t/README.md"
    git -C "$GUARD/repo/vendor/$t" init -q
    git -C "$GUARD/repo/vendor/$t" add -A
    git -C "$GUARD/repo/vendor/$t" -c user.name=t -c user.email=t@t commit -qm stub
done
git -C "$GUARD/repo/vendor/RIFT" rev-parse HEAD > "$GUARD/repo/protocols/meshcore/vendor_rift_commit.txt"
git -C "$GUARD/repo/vendor/Crypto" rev-parse HEAD > "$GUARD/repo/protocols/meshcore/vendor_crypto_commit.txt"
printf 'committed\n' > "$GUARD/repo/tracked.txt"
# The real repository ignores /vendor/, so the scratch one must too: an
# embedded checkout showing as untracked would make the tree dirty, and the
# dirty-tree guard would then fire before any of the RadioLib cases below -
# which would pass, for the wrong reason.
printf '/vendor/\n' > "$GUARD/repo/.gitignore"
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

# The RadioLib pin, enforced by the real script rather than read off it. This
# is the dependency that is copied into the package rather than archived, so a
# wrong or edited checkout is compiled into radiod either way; the only
# question is whether the build says so.
#
# These cases cannot go through run_guard: it sets POCKETOS_ALLOW_PIN_DRIFT=1
# to get past the stub vendor's BSP pin, and that would wave the RadioLib pin
# through as well. So the scratch repo is given the stub vendor's own commits
# as its BSP and SDK pins - everything correct except the one thing under
# test - and the pin edits are committed, because an uncommitted one would
# trip the dirty-tree guard first and the case would pass for that reason.
git -C "$GUARD/repo" checkout -q -- tracked.txt 2>/dev/null
printf 'committed\n' > "$GUARD/repo/tracked.txt"
git -C "$GUARD/vendor" rev-parse HEAD > "$GUARD/repo/platforms/k230/vendor_bsp_commit.txt"
git -C "$GUARD/vendor/k230_linux_sdk" rev-parse HEAD \
    > "$GUARD/repo/platforms/k230/vendor_sdk_commit.txt"
RL_PIN="$GUARD/repo/platforms/k230/vendor_radiolib_commit.txt"
RL_REAL=$(git -C "$GUARD/repo/vendor/RadioLib" rev-parse HEAD)
guard_commit() { git -C "$GUARD/repo" add -A >/dev/null 2>&1
                 git -C "$GUARD/repo" -c user.name=t -c user.email=t@t \
                     commit -qm pins >/dev/null 2>&1; }

# Without the drift override, which run_guard hardcodes for the stub vendor's
# BSP pin and which would wave the RadioLib pin through with it.
run_strict() {
    # shellcheck disable=SC2086
    ( cd "$GUARD/repo" && env ${1:+"$1"} \
        bash platforms/k230/scripts/apply_to_sdk.sh "$GUARD/vendor" ) \
        > "$GUARD/out" 2>&1
    echo $?
}

printf '%s\n' 0000000000000000000000000000000000000000 > "$RL_PIN"
guard_commit
run_strict >/dev/null
check "a drifted RadioLib pin is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
# The error, not the warning of the same shape that the override prints.
check "and the refusal names RadioLib" $(said 'ERROR: RadioLib commit is')

printf '%s\n' "$RL_REAL" > "$RL_PIN"
guard_commit
run_strict >/dev/null
check "with the pin right, the apply gets past it" $(said '\[1/5\]')

printf 'edited\n' >> "$GUARD/repo/vendor/RadioLib/README.md"
run_strict >/dev/null
check "a dirty RadioLib checkout is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal says it would be compiled into radiod" \
      $(said 'compiled into radiod')
run_strict "POCKETOS_ALLOW_PIN_DRIFT=1" >/dev/null
check "the drift override lets a dirty dependency through" $(said '\[1/5\]')
check "but says its changes will be compiled in" $(said 'WILL be')
git -C "$GUARD/repo/vendor/RadioLib" checkout -q -- README.md 2>/dev/null

# ggwave, the same three cases: drifted, right, dirty.
GG_PIN="$GUARD/repo/platforms/k230/vendor_ggwave_commit.txt"
GG_REAL=$(git -C "$GUARD/repo/vendor/ggwave" rev-parse HEAD)
printf '%s\n' 0000000000000000000000000000000000000000 > "$GG_PIN"
guard_commit
run_strict >/dev/null
check "a drifted ggwave pin is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal names ggwave" $(said 'ERROR: ggwave commit is')
printf '%s\n' "$GG_REAL" > "$GG_PIN"
guard_commit
run_strict >/dev/null
check "with the ggwave pin right, the apply gets past it" $(said '\[1/5\]')
printf 'edited\n' >> "$GUARD/repo/vendor/ggwave/README.md"
run_strict >/dev/null
check "a dirty ggwave checkout is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal says it would be compiled into pos-wave" \
      $(said 'compiled into pos-wave')
git -C "$GUARD/repo/vendor/ggwave" checkout -q -- README.md 2>/dev/null

# MeshCore and Crypto: the protocol source meshcored is built from.
MC_PIN="$GUARD/repo/protocols/meshcore/vendor_rift_commit.txt"
MC_REAL=$(git -C "$GUARD/repo/vendor/RIFT" rev-parse HEAD)
printf '%s\n' 0000000000000000000000000000000000000000 > "$MC_PIN"
guard_commit
run_strict >/dev/null
check "a drifted MeshCore pin is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal names MeshCore" $(said 'ERROR: MeshCore (vendor/RIFT) commit is')
printf '%s\n' "$MC_REAL" > "$MC_PIN"
guard_commit
run_strict >/dev/null
check "with the MeshCore pin right, the apply gets past it" $(said '\[1/5\]')
printf 'edited\n' >> "$GUARD/repo/vendor/RIFT/README.md"
run_strict >/dev/null
check "a dirty MeshCore checkout is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal says it would be compiled into meshcored" \
      $(said 'compiled into meshcored')
git -C "$GUARD/repo/vendor/RIFT" checkout -q -- README.md 2>/dev/null
CR_PIN="$GUARD/repo/protocols/meshcore/vendor_crypto_commit.txt"
CR_REAL=$(git -C "$GUARD/repo/vendor/Crypto" rev-parse HEAD)
printf '%s\n' 0000000000000000000000000000000000000000 > "$CR_PIN"
guard_commit
run_strict >/dev/null
check "a drifted Crypto pin is refused" \
      $([ "$(said '\[1/5\]')" = "0" ] && echo 1 || echo 0)
check "and the refusal names Crypto" $(said 'ERROR: Crypto (vendor/Crypto) commit is')
printf '%s\n' "$CR_REAL" > "$CR_PIN"
guard_commit
# The provenance summary prints only on a run that finishes, which the stub
# vendor cannot reach, so it is checked where it is written instead. What
# matters about it is placement as much as content: a summary in the header
# would be the very thing it exists to survive.
sum_line=$(grep -n '^echo "Provenance"' "$APPLY" | cut -d: -f1)
pkg_line=$(grep -nE '^[[:space:]]*git [^|]*archive' "$APPLY" | head -1 | cut -d: -f1)
done_line=$(grep -n '^echo "Done\. Build with' "$APPLY" | cut -d: -f1)
check "apply_to_sdk.sh prints a provenance summary" \
      $([ -n "$sum_line" ] && echo 1 || echo 0)
check "the summary comes after packaging, not in the header" \
      $([ -n "$sum_line" ] && [ -n "$pkg_line" ] && [ "$sum_line" -gt "$pkg_line" ] && echo 1 || echo 0)
check "the summary is the last thing said before Done" \
      $([ -n "$sum_line" ] && [ -n "$done_line" ] && [ "$sum_line" -lt "$done_line" ] && echo 1 || echo 0)
check "the summary names the packaged commit" \
      $(grep -q 'Packaged source : ${REPO_COMMIT} (${SNAPSHOT_COMMIT})' "$APPLY" && echo 1 || echo 0)
check "the summary names the worktree state" \
      $(grep -q 'Source worktree : ' "$APPLY" && echo 1 || echo 0)
check "the summary calls out the override when it was used" \
      $(grep -q 'Dirty override  : POCKETOS_ALLOW_DIRTY_BUILD=1' "$APPLY" && echo 1 || echo 0)
check "the summary states that the worktree is never packaged" \
      $(grep -q 'working tree is never packaged' "$APPLY" && echo 1 || echo 0)

# The override must not widen what is packaged. Two halves: no archive in the
# script exports anything but the one snapshot object, and that object really
# does exclude the edit that made the tree dirty.
#
# What is asserted is the guarantee, not the spelling. This check used to
# require the literal string HEAD, which is how it would have failed the
# moment the archives were pointed at a resolved commit instead - the same
# mistake that made it demand deploy.sh's old three literal stop lines
# (5b6bd80). A moving reference is what is forbidden here, not a word.
loose=$(grep -nE '^[[:space:]]*git [^|]*archive' "$APPLY" \
        | grep -v 'SNAPSHOT_COMMIT' || true)
check "every git archive in apply_to_sdk.sh exports the one snapshot object" \
      $([ -z "$loose" ] && echo 1 || echo 0)
[ -n "$loose" ] && printf '%s\n' "$loose" | head -3
check "and that object is resolved once, from HEAD" \
      $(grep -c '^SNAPSHOT_COMMIT="\$(git -C "\${REPO_DIR}" rev-parse HEAD)"$' "$APPLY" \
        | grep -q '^1$' && echo 1 || echo 0)
got=$(git -C "$GUARD/repo" archive --format=tar HEAD -- tracked.txt | tar -xO)
check "an override build exports HEAD, not the uncommitted edit" \
      $([ "$got" = "committed" ] && echo 1 || echo 0)

# ---- every first-party build input, not just the archived ones -----------
#
# The defconfig, Config.in and pocketos.mk decide what goes into the image and
# how it is built, and all three used to be installed straight from the
# working tree while the script printed "the working tree is never packaged".
# An uncommitted edit to any of them changed the build and nothing said so.
wt=$(grep -nE '^[[:space:]]*(install|cp)[[:space:]]' "$APPLY" \
     | grep -E '\$\{(PLATFORM_DIR|REPO_DIR)\}' || true)
check "no first-party build input is installed from the working tree" \
      $([ -z "$wt" ] && echo 1 || echo 0)
[ -n "$wt" ] && printf '%s\n' "$wt" | head -5

for f in 'configs/${CONF}' package/pocketos/Config.in package/pocketos/pocketos.mk package/pocketos/pocketos.hash; do
    check "$(basename "$f") is installed from the snapshot" \
          $(grep -qF "install -m 0644 \"\${SNAPSHOT_DIR}/platforms/k230/$f\"" "$APPLY" \
            && echo 1 || echo 0)
done

# And the mechanism really does ignore the working tree. A throwaway worktree
# is dirtied in exactly the way that used to reach the image - an edit to the
# defconfig - and the snapshot the script would take must still yield the
# committed bytes.
SNAP="$TMP/snap"
mkdir -p "$SNAP/out"
if git worktree add --detach "$SNAP/wt" HEAD >/dev/null 2>&1; then
    conf_rel="platforms/k230/configs/$CONF"
    printf '\n# uncommitted edit that must never reach a build\n' >> "$SNAP/wt/$conf_rel"
    check "the throwaway worktree is dirty" \
          $([ -n "$(git -C "$SNAP/wt" status --porcelain)" ] && echo 1 || echo 0)
    snap_commit=$(git -C "$SNAP/wt" rev-parse HEAD)
    git -C "$SNAP/wt" archive --format=tar "$snap_commit" -- "$conf_rel" \
        | tar -xp -C "$SNAP/out"
    check "the snapshot of a dirty tree carries the committed defconfig" \
          $(cmp -s "$SNAP/out/$conf_rel" <(git show "HEAD:$conf_rel") && echo 1 || echo 0)
    check "and not the uncommitted edit" \
          $(grep -q 'uncommitted edit that must never reach a build' "$SNAP/out/$conf_rel" \
            && echo 0 || echo 1)
    git worktree remove --force "$SNAP/wt" >/dev/null 2>&1
else
    echo "FAIL could not create a throwaway worktree for the snapshot check"
    failed=$((failed + 1))
fi

# ---- the one vendored dependency that reaches the image -----------------
#
# RadioLib is compiled into radiod and is copied into the package rather than
# archived from a commit, so nothing about our own history fixes its version.
# cjson, lvgl, libgpiod, libdrm and libevdev are Buildroot packages, so the
# SDK pin already fixes theirs; RadioLib had no pin at all and the release
# build checked it by hand.
check "RadioLib has a pinned commit" \
      $([ -s platforms/k230/vendor_radiolib_commit.txt ] && echo 1 || echo 0)
check "the pin is a full commit id" \
      $(grep -qE '^[0-9a-f]{40}$' platforms/k230/vendor_radiolib_commit.txt && echo 1 || echo 0)
check "apply_to_sdk.sh enforces it with the same pin_check as the others" \
      $(grep -q 'pin_check "RadioLib commit"' "$APPLY" && echo 1 || echo 0)
check "and refuses a dirty RadioLib checkout" \
      $(grep -q 'the RadioLib checkout at ${RADIOLIB_DIR_SRC} is dirty' "$APPLY" && echo 1 || echo 0)
check "and refuses to guess when it is not a git checkout" \
      $(grep -q 'RADIOLIB_STATE="not-a-git-checkout"' "$APPLY" && echo 1 || echo 0)
check "the commit reaches the applied manifest" \
      $(grep -q '^radiolib_commit=' "$APPLY" && echo 1 || echo 0)
check "so does its clean/dirty state" \
      $(grep -q '^radiolib_state=' "$APPLY" && echo 1 || echo 0)
check "and BUILD_INFO reports it" \
      $(grep -q 'RadioLib  : $(m radiolib_commit)' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)

# ggwave: the second one, compiled into pos-wave.
check "ggwave has a pinned commit" \
      $([ -s platforms/k230/vendor_ggwave_commit.txt ] && echo 1 || echo 0)
check "the ggwave pin is a full commit id" \
      $(grep -qE '^[0-9a-f]{40}$' platforms/k230/vendor_ggwave_commit.txt && echo 1 || echo 0)
check "apply_to_sdk.sh enforces the ggwave pin with pin_check" \
      $(grep -q 'pin_check "ggwave commit"' "$APPLY" && echo 1 || echo 0)
check "and refuses a dirty ggwave checkout" \
      $(grep -q 'the ggwave checkout at ${GGWAVE_DIR_SRC} is dirty' "$APPLY" && echo 1 || echo 0)
check "and refuses to guess when ggwave is not a git checkout" \
      $(grep -q 'GGWAVE_STATE="not-a-git-checkout"' "$APPLY" && echo 1 || echo 0)
check "the ggwave commit and state reach the applied manifest" \
      $(grep -q '^ggwave_commit=' "$APPLY" && grep -q '^ggwave_state=' "$APPLY" && echo 1 || echo 0)
check "and BUILD_INFO reports ggwave" \
      $(grep -q 'ggwave    : $(m ggwave_commit)' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)
check "the package carries ggwave's licences with it" \
      $(grep -q 'ggwave}/LICENSE\|GGWAVE_DIR_SRC}/LICENSE' "$APPLY" &&
        grep -q 'reed-solomon/LICENSE' "$APPLY" && echo 1 || echo 0)
check "apply_to_sdk.sh enforces the MeshCore and Crypto pins with pin_check" \
      $(grep -q 'pin_check "${what} commit"' "$APPLY" &&
        grep -q 'meshcore_tree_check "MeshCore (vendor/RIFT)"' "$APPLY" &&
        grep -q 'meshcore_tree_check "Crypto (vendor/Crypto)"' "$APPLY" && echo 1 || echo 0)
check "their commits and states reach the applied manifest" \
      $(grep -q '^meshcore_commit=' "$APPLY" && grep -q '^meshcore_state=' "$APPLY" &&
        grep -q '^crypto_commit=' "$APPLY" && grep -q '^crypto_state=' "$APPLY" && echo 1 || echo 0)
check "and BUILD_INFO reports them" \
      $(grep -q 'MeshCore  : $(m meshcore_commit)' platforms/k230/scripts/build_image.sh &&
        grep -q 'Crypto    : $(m crypto_commit)' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)
check "an exported tree with uncommitted changes is recorded as <commit>-dirty, never as the pin" \
      $(grep -q "pin_record() { \[ \"\$2\" = \"dirty\" \] && printf '%s-dirty" "$APPLY" &&
        grep -q 'pin_record "${RIFT_COMMIT}" "${RIFT_STATE}"' "$APPLY" &&
        grep -q 'pin_record "${CRYPTO_COMMIT}" "${CRYPTO_STATE}"' "$APPLY" && echo 1 || echo 0)
check "the package carries the MeshCore and Crypto licences with them" \
      $(grep -q 'RIFT_DIR_SRC}/license.txt' "$APPLY" &&
        grep -q 'CRYPTO_DIR_SRC}/libraries/LICENSE.txt' "$APPLY" && echo 1 || echo 0)
check "the package build links alsa-lib for pos-wave" \
      $(grep -q 'alsa-lib' platforms/k230/package/pocketos/pocketos.mk &&
        grep -q 'BR2_PACKAGE_ALSA_LIB' platforms/k230/package/pocketos/Config.in && echo 1 || echo 0)
if git -C vendor/ggwave rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    here=$(git -C vendor/ggwave rev-parse HEAD)
    want=$(cat platforms/k230/vendor_ggwave_commit.txt)
    check "the ggwave checkout here is the pinned commit" \
          $([ "$here" = "$want" ] && echo 1 || echo 0)
    [ "$here" = "$want" ] || echo "     here $here, pinned $want"
else
    echo "note: no ggwave checkout here; the pin's value was not compared"
fi

# Every dependency the package build names must be either a Buildroot package
# (version fixed by the SDK pin) or pinned here. A new vendored tree copied
# into the package without a pin is the case this catches.
copied=$(grep -nE '^[[:space:]]*(rsync|cp)[[:space:]]' "$APPLY" \
         | grep -oE 'vendor/[A-Za-z0-9_.-]+' | sort -u)
for dep in $copied; do
    name=$(basename "$dep")
    check "vendored $name is pinned" \
          $([ -s "platforms/k230/vendor_$(echo "$name" | tr 'A-Z' 'a-z')_commit.txt" ] && echo 1 || echo 0)
done

# The pin has to name the checkout that is actually here, or the release build
# is against something else entirely.
if git -C vendor/RadioLib rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    here=$(git -C vendor/RadioLib rev-parse HEAD)
    want=$(cat platforms/k230/vendor_radiolib_commit.txt)
    check "the RadioLib checkout here is the pinned commit" \
          $([ "$here" = "$want" ] && echo 1 || echo 0)
    [ "$here" = "$want" ] || echo "     here $here, pinned $want"
else
    echo "note: no RadioLib checkout here; the pin's value was not compared"
fi

echo "package_sync_test: $failed failure(s)"
exit $((failed > 0))
