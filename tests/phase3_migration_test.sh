#!/bin/bash
# The Phase 3 migration and its rollback (ADR-005 Phase 3).
#
# Two halves. The first reads deploy_unit.sh (what deploy.sh runs on the unit)
# and rollback_phase3.sh as text and checks the order of what they do: order is the whole safety
# argument here - a binary removed before its init script, or a shell started
# before the old one was stopped, is how a unit ends up with two shells or
# none, and neither is visible in a test that only looks at the end state.
# The second runs the file operations of both, in a fake root, and checks the
# end state: what was removed, what was kept, and that running them twice
# changes nothing the second time.
#
# The payloads are extracted rather than copied here, so this cannot pass
# against a script that no longer contains them.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
DEPLOY=platforms/k230/scripts/deploy.sh
UNIT=platforms/k230/scripts/deploy_unit.sh
ROLLBACK=platforms/k230/scripts/rollback_phase3.sh
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# Line number of the first line matching a fixed string, or empty.
at() { grep -nF -m1 -- "$2" "$1" | cut -d: -f1; }
before() { # <file> <earlier> <later>
    local a b
    a=$(at "$1" "$2"); b=$(at "$1" "$3")
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && echo 1 || echo 0
}

# ---- 1. deploy.sh: the order of the migration ---------------------------
UNPACK=$(at "$UNIT" 'tar -C / -xf "$PAYLOAD"')
check "deploy.sh still unpacks a payload" "$([ -n "$UNPACK" ] && echo 1 || echo 0)"
check "both shell services are stopped before anything is unpacked" \
    "$([ "$(before "$UNIT" 'for s in S90doors-shell S90pocketos-shell' 'tar -C / -xf "$PAYLOAD"')" = 1 ] && echo 1 || echo 0)"
check "a stop that fails ends the deployment" \
    "$(grep -q 'could not be stopped; nothing has been installed' "$UNIT" && echo 1 || echo 0)"
check "no shell of either name may still be running before the unpack" \
    "$([ "$(before "$UNIT" 'a shell is still running' 'tar -C / -xf "$PAYLOAD"')" = 1 ] && echo 1 || echo 0)"
check "that check reads both identities' pid files" \
    "$(grep -q '/var/run/doors-shell-supervise.pid /run/pocketos/doors-shell.pid' "$UNIT" &&
       grep -q '/var/run/pocketos-shell-supervise.pid /run/pocketos/pocketos-shell.pid' "$UNIT" && echo 1 || echo 0)"
check "and sweeps /proc for a daemon whose supervisor was killed" \
    "$(grep -q 'readlink "$d/exe"' "$UNIT" && echo 1 || echo 0)"
# The removals: init script first. A binary removed first leaves an init script
# that starts nothing; an init script removed first leaves a binary nothing
# runs, which is the harmless order.
check "the old init script is removed after the unpack" \
    "$([ "$(before "$UNIT" 'tar -C / -xf "$PAYLOAD"' 'rm -f /etc/init.d/S90pocketos-shell')" = 1 ] && echo 1 || echo 0)"
check "the old init script is removed BEFORE the old binary" \
    "$([ "$(before "$UNIT" 'rm -f /etc/init.d/S90pocketos-shell' 'rm -f /usr/bin/pocketos-shell')" = 1 ] && echo 1 || echo 0)"
check "the old supervisor state is removed too, so no stale service row is left" \
    "$(grep -q '/run/pocketos/pocketos-shell.state' "$UNIT" && echo 1 || echo 0)"
check "the new service is started after the removals" \
    "$([ "$(before "$UNIT" 'rm -f /usr/bin/pocketos-shell' '/etc/init.d/S90doors-shell start')" = 1 ] && echo 1 || echo 0)"
check "the deploy counts shells, init scripts and states at the end" \
    "$(grep -q 'shell processes=' "$UNIT" && grep -q 'more than one shell identity' "$UNIT" && echo 1 || echo 0)"
check "and fails when it finds more than one of any of them" \
    "$([ "$(before "$UNIT" 'more than one shell identity' 'doors version')" = 1 ] && echo 1 || echo 0)"
# Settings are not the deploy's to delete: they are what the new service falls
# back to, and what a rollback needs.
check "deploy.sh and deploy_unit.sh never remove a settings file" \
    "$(grep -hE 'rm[^#]*etc/default' "$DEPLOY" "$UNIT" | grep -q . && echo 0 || echo 1)"
check "deploy.sh ships the new binary and init script" \
    "$(grep -q 'usr/bin/doors-shell' "$DEPLOY" && grep -q 'etc/init.d/S90doors-shell' "$DEPLOY" && echo 1 || echo 0)"
check "and ships neither of the old ones" \
    "$(grep -E 'usr/bin/pocketos-shell|etc/init.d/S90pocketos-shell' "$DEPLOY" | grep -vE '^\s*#|rm -f|for s in|scripts=|ls ' | grep -q . && echo 0 || echo 1)"

# ---- 2. rollback_phase3.sh: the order of the undo -----------------------
check "the rollback stops the Doors shell first" \
    "$([ "$(before "$ROLLBACK" 'for s in S90doors-shell' 'rm -f /etc/init.d/S90doors-shell')" = 1 ] && echo 1 || echo 0)"
check "it refuses to remove anything while a shell is running" \
    "$([ "$(before "$ROLLBACK" 'nothing has been removed' 'run rm -f /etc/init.d/S90doors-shell')" = 1 ] && echo 1 || echo 0)"
check "settings are carried back before the identity is removed" \
    "$([ "$(before "$ROLLBACK" 'cp /etc/default/doors-shell /etc/default/pocketos-shell' 'run rm -f /etc/init.d/S90doors-shell')" = 1 ] && echo 1 || echo 0)"
check "and only when the old file is not there, so nothing is overwritten" \
    "$(grep -q '\[ ! -e /etc/default/pocketos-shell \]' "$ROLLBACK" && echo 1 || echo 0)"
check "the two settings files are never merged" \
    "$(grep -q 'not merged into it' "$ROLLBACK" && echo 1 || echo 0)"
check "the init script is removed BEFORE the binary" \
    "$([ "$(before "$ROLLBACK" 'run rm -f /etc/init.d/S90doors-shell' 'run rm -f /usr/bin/doors-shell')" = 1 ] && echo 1 || echo 0)"
check "the Doors runtime state is removed" \
    "$(grep -q '/run/pocketos/doors-shell.state' "$ROLLBACK" && echo 1 || echo 0)"
check "it ends by proving no shell identity is left" \
    "$(grep -q 'still carries a shell identity' "$ROLLBACK" && echo 1 || echo 0)"
check "it keeps the Phase 2 names and the shared directories" \
    "$(grep -E 'rm[^#]*(usr/bin/doors([^-]|$)|usr/bin/pos([^-]|$)|etc/doors-release|var/lib/pocketos|etc/pocketos([^/]|$))' "$ROLLBACK" | grep -q . && echo 0 || echo 1)"
check "it keeps the logs" \
    "$(grep -E 'rm[^#]*log' "$ROLLBACK" | grep -q . && echo 0 || echo 1)"
check "--dry-run removes nothing" \
    "$(grep -q 'if \[ -n "$DRY" \]; then echo "  would: \$\*"' "$ROLLBACK" && echo 1 || echo 0)"

# ---- 3. the file operations, in a fake root -----------------------------
#
# The stanzas are taken out of the scripts by their own markers and run with
# the paths rewritten, so what is exercised is the text that ships.
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT/etc/init.d" "$ROOT/etc/default" "$ROOT/usr/bin" "$ROOT/run/pocketos" "$ROOT/var/run" \
         "$ROOT/var/lib/pocketos/log"
rewrite_into() { # <file> : rewrite system paths into $ROOT
    sed -e "s#/etc/init.d/#$ROOT/etc/init.d/#g" \
        -e "s#/etc/default/#$ROOT/etc/default/#g" \
        -e "s#/usr/bin/#$ROOT/usr/bin/#g" \
        -e "s#/run/pocketos/#$ROOT/run/pocketos/#g" \
        -e "s#/var/run/#$ROOT/var/run/#g" "$1"
}

pocketos_era() { # a unit as Phase 2 left it
    rm -rf "${ROOT:?}/etc" "${ROOT:?}/usr" "${ROOT:?}/run" "${ROOT:?}/var"
    mkdir -p "$ROOT/etc/init.d" "$ROOT/etc/default" "$ROOT/usr/bin" "$ROOT/run/pocketos" "$ROOT/var/run" \
             "$ROOT/var/lib/pocketos/log"
    : > "$ROOT/etc/init.d/S90pocketos-shell"; chmod 0755 "$ROOT/etc/init.d/S90pocketos-shell"
    : > "$ROOT/usr/bin/pocketos-shell"; chmod 0755 "$ROOT/usr/bin/pocketos-shell"
    printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=33,33,33,33\n' > "$ROOT/etc/default/pocketos-shell"
    : > "$ROOT/run/pocketos/pocketos-shell.state"
    : > "$ROOT/run/pocketos/pocketos-shell.pid"
    : > "$ROOT/run/pocketos/pocketos-shell.crashloop"
    : > "$ROOT/var/run/pocketos-shell-supervise.pid"
    : > "$ROOT/var/lib/pocketos/log/supervise-pocketos-shell.log"
    # what the payload brings
    : > "$ROOT/etc/init.d/S90doors-shell"; chmod 0755 "$ROOT/etc/init.d/S90doors-shell"
    : > "$ROOT/usr/bin/doors-shell"; chmod 0755 "$ROOT/usr/bin/doors-shell"
}

# The migration stanza: everything deploy_unit.sh does between the unpack and the
# services being started again.
MIGRATE="$ROOT/migrate.sh"
sed -n '/^rm -f \/etc\/init.d\/S90pocketos-shell$/,/^rm -f \/run\/pocketos\/pocketos-shell.pid/p' "$UNIT" \
    | rewrite_into /dev/stdin > "$MIGRATE"
# the multi-line state removal ends on the next line; take it too
sed -n '/^rm -f \/run\/pocketos\/pocketos-shell.pid/,+1p' "$UNIT" | tail -1 | rewrite_into /dev/stdin >> "$MIGRATE"
check "the migration stanza was found in deploy_unit.sh" \
    "$([ -s "$MIGRATE" ] && grep -q 'S90pocketos-shell' "$MIGRATE" && echo 1 || echo 0)"

pocketos_era
sh "$MIGRATE"
check "migration: the old init script is gone" "$([ ! -e "$ROOT/etc/init.d/S90pocketos-shell" ] && echo 1 || echo 0)"
check "migration: the old binary is gone" "$([ ! -e "$ROOT/usr/bin/pocketos-shell" ] && echo 1 || echo 0)"
check "migration: the old supervisor state is gone" \
    "$([ ! -e "$ROOT/run/pocketos/pocketos-shell.state" ] && [ ! -e "$ROOT/run/pocketos/pocketos-shell.pid" ] &&
       [ ! -e "$ROOT/run/pocketos/pocketos-shell.crashloop" ] &&
       [ ! -e "$ROOT/var/run/pocketos-shell-supervise.pid" ] && echo 1 || echo 0)"
check "migration: the old settings file is kept" \
    "$(grep -q '33,33,33,33' "$ROOT/etc/default/pocketos-shell" && echo 1 || echo 0)"
check "migration: the old supervisor log is kept" \
    "$([ -e "$ROOT/var/lib/pocketos/log/supervise-pocketos-shell.log" ] && echo 1 || echo 0)"
check "migration: the new service is untouched" \
    "$([ -x "$ROOT/etc/init.d/S90doors-shell" ] && [ -x "$ROOT/usr/bin/doors-shell" ] && echo 1 || echo 0)"
sh "$MIGRATE"
check "migration: running it again changes nothing and fails nothing" \
    "$([ $? -eq 0 ] && [ ! -e "$ROOT/usr/bin/pocketos-shell" ] && [ -x "$ROOT/usr/bin/doors-shell" ] && echo 1 || echo 0)"

# A deploy interrupted between the two removals: the init script is gone, the
# binary is not. Nothing starts the old shell, and the next deploy finishes it.
pocketos_era
rm -f "$ROOT/etc/init.d/S90pocketos-shell"
sh "$MIGRATE"
check "an interrupted migration is finished by the next one" \
    "$([ ! -e "$ROOT/usr/bin/pocketos-shell" ] && [ -x "$ROOT/usr/bin/doors-shell" ] && echo 1 || echo 0)"

# ---- 4. the rollback's file operations ----------------------------------
ROLL="$ROOT/roll.sh"
{
    echo 'run() { "$@"; }'
    sed -n '/^if \[ -r \/etc\/default\/doors-shell \]/,/^run rm -f \/run\/pocketos\/doors-shell.pid/p' "$ROLLBACK" \
        | rewrite_into /dev/stdin
    sed -n '/^run rm -f \/run\/pocketos\/doors-shell.pid/,+1p' "$ROLLBACK" | tail -1 | rewrite_into /dev/stdin
} > "$ROLL"
check "the rollback stanza was found in rollback_phase3.sh" \
    "$([ -s "$ROLL" ] && grep -q 'doors-shell' "$ROLL" && echo 1 || echo 0)"

phase3_unit() { # a unit as Phase 3 leaves it
    rm -rf "${ROOT:?}/etc" "${ROOT:?}/usr" "${ROOT:?}/run" "${ROOT:?}/var"
    mkdir -p "$ROOT/etc/init.d" "$ROOT/etc/default" "$ROOT/usr/bin" "$ROOT/run/pocketos" "$ROOT/var/run" \
             "$ROOT/var/lib/pocketos/log"
    : > "$ROOT/etc/init.d/S90doors-shell"; chmod 0755 "$ROOT/etc/init.d/S90doors-shell"
    : > "$ROOT/usr/bin/doors-shell"; chmod 0755 "$ROOT/usr/bin/doors-shell"
    : > "$ROOT/usr/bin/doors"; : > "$ROOT/usr/bin/pos"
    : > "$ROOT/run/pocketos/doors-shell.state"; : > "$ROOT/run/pocketos/doors-shell.pid"
    : > "$ROOT/var/run/doors-shell-supervise.pid"
    : > "$ROOT/var/lib/pocketos/log/supervise-doors-shell.log"
}

# Only the new settings file: it must come back under the old name, whole.
phase3_unit
printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=44,44,44,44\n' > "$ROOT/etc/default/doors-shell"
sh "$ROLL" >/dev/null
check "rollback: the Doors init script and binary are gone" \
    "$([ ! -e "$ROOT/etc/init.d/S90doors-shell" ] && [ ! -e "$ROOT/usr/bin/doors-shell" ] && echo 1 || echo 0)"
check "rollback: the Doors runtime state is gone" \
    "$([ ! -e "$ROOT/run/pocketos/doors-shell.state" ] && [ ! -e "$ROOT/run/pocketos/doors-shell.pid" ] &&
       [ ! -e "$ROOT/var/run/doors-shell-supervise.pid" ] && echo 1 || echo 0)"
check "rollback: the settings come back under the old name, byte for byte" \
    "$(cmp -s "$ROOT/etc/default/doors-shell" "$ROOT/etc/default/pocketos-shell" && echo 1 || echo 0)"
check "rollback: the Phase 2 names are untouched" \
    "$([ -e "$ROOT/usr/bin/doors" ] && [ -e "$ROOT/usr/bin/pos" ] && echo 1 || echo 0)"
check "rollback: the logs are untouched" \
    "$([ -e "$ROOT/var/lib/pocketos/log/supervise-doors-shell.log" ] && echo 1 || echo 0)"

# Both files: the old one is what the restored service reads, and it is kept
# exactly as the operator left it.
phase3_unit
printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=44,44,44,44\n' > "$ROOT/etc/default/doors-shell"
printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=55,55,55,55\n' > "$ROOT/etc/default/pocketos-shell"
sh "$ROLL" >/dev/null
check "rollback: an existing old settings file is not overwritten" \
    "$(grep -q '55,55,55,55' "$ROOT/etc/default/pocketos-shell" && echo 1 || echo 0)"
check "rollback: and nothing from the new file is merged into it" \
    "$(grep -q '44,44,44,44' "$ROOT/etc/default/pocketos-shell" && echo 0 || echo 1)"
sh "$ROLL" >/dev/null
check "rollback: running it again changes nothing" \
    "$([ ! -e "$ROOT/usr/bin/doors-shell" ] && grep -q '55,55,55,55' "$ROOT/etc/default/pocketos-shell" && echo 1 || echo 0)"

echo "phase3_migration_test: $failed failure(s)"
exit $((failed > 0))
