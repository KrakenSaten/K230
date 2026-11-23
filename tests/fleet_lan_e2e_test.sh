#!/bin/bash
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
# PocketFleet over the local network, end to end (M2 package 6): two whole
# peerd processes - the shipped binary - each in a network namespace of its
# own, and two Fleet players that play whole matches through them. See
# tests/fleet_lan_e2e.py.
#
#   FLEET_LAN_E2E_SCENARIOS="clean lossy crash restart address lookup outage blocked chat forfeit lastpacket endlost closemid" \
#       bash tests/fleet_lan_e2e_test.sh
#
# Each scenario runs in its own user, network, mount and process namespace
# (unshare -Urnm --pid --fork --kill-child), so the scenarios run side by
# side, need no root, and leave nothing behind: the interfaces, the routes
# and the loss exist only inside, and when a scenario's first process goes -
# finished, failed, or stopped with the test (Ctrl-C, SIGTERM, SIGHUP) - the
# kernel takes every peerd and player it started with it. Only a SIGKILL of
# this script itself skips that: its scenarios then run on to their own end
# (eight minutes at most) and the temporary directory stays. A host that does not allow unprivileged
# user namespaces cannot run this, and that is a failure here, not a skip.
#
# Not part of make test: it needs libsodium (peerd) and takes a few minutes.
# Run by `make ENABLE_PEERD=1 SODIUM_DIR=... fleet-lan-e2e`.
#
# No hardware, and nothing here reaches any network outside itself.
set -u
cd "$(dirname "$0")/.." || exit 1

PEERD=${PEERD:-services/peerd/peerd}
PLAYER=${PLAYER:-tests/fleet_lan_player}
SCENARIOS=${FLEET_LAN_E2E_SCENARIOS:-clean lossy crash restart address lookup outage blocked chat forfeit lastpacket endlost closemid}

for bin in "$PEERD" "$PLAYER"; do
    if [ ! -x "$bin" ]; then
        echo "FAIL $bin is not built; run 'make ENABLE_PEERD=1 SODIUM_DIR=... fleet-lan-e2e'"
        exit 1
    fi
done
for tool in unshare ip tc python3 ping strings pkill; do
    command -v "$tool" >/dev/null || { echo "FAIL $tool is needed and not installed"; exit 1; }
done
if strings "$PEERD" | grep -q "PEERD_TEST"; then
    echo "FAIL $PEERD contains a test hook: this test is of the shipped peerd"; exit 1
fi
echo "ok   the peerd under test is the shipped one, with no test hook"
NS="unshare -Urnm --pid --fork --kill-child --mount-proc"
if ! $NS true 2>/dev/null; then
    echo "FAIL this host does not allow unprivileged user namespaces ($NS)"; exit 1
fi

TMP=$(mktemp -d) && [ -n "$TMP" ] && [ -d "$TMP" ] || { echo "FAIL no temporary directory"; exit 1; }
pids=""
cleanup() {
    # Each pid is unshare's parent; its child, the scenario, is the first
    # process of a process namespace and dies with it (--kill-child), and
    # everything the scenario started dies with that. SIGKILL, because the
    # parent may have inherited SIGINT ignored as a background job.
    # (A scenario that is played again runs under a subshell of this script:
    # its unshare is that subshell's child, ended first.)
    for p in $pids; do
        pkill -KILL -P "$p" 2>/dev/null
        kill -KILL "$p" 2>/dev/null
    done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

run_one() {
    # Exit 5 is a scenario that could not stage its fault ("lastpacket", when
    # the endgame leaves nothing to lose): played again, from scratch.
    local sc=$1 try rc
    for try in 1 2 3; do
        rm -rf "${TMP:?}/$sc" && mkdir -p "$TMP/$sc"
        $NS python3 tests/fleet_lan_e2e.py "$PEERD" "$PLAYER" "$TMP/$sc" "$sc" \
            > "$TMP/$sc.try" 2>&1
        rc=$?
        cat "$TMP/$sc.try" >> "$TMP/$sc.out"
        [ "$rc" -ne 5 ] && return "$rc"
    done
    echo "FAIL [$sc] not staged in three tries" >> "$TMP/$sc.out"
    return 1
}
for sc in $SCENARIOS; do
    run_one "$sc" &
    pids="$pids $!"
done
failed=0
for p in $pids; do
    wait "$p" || failed=$((failed + 1))
done
for sc in $SCENARIOS; do
    cat "$TMP/$sc.out"
    if [ -n "${FLEET_LAN_E2E_KEEP:-}" ]; then
        mkdir -p "$FLEET_LAN_E2E_KEEP" && cp -r "$TMP/$sc" "$TMP/$sc.out" "$FLEET_LAN_E2E_KEEP/" 2>/dev/null
    fi
done
echo "fleet_lan_e2e_test: $failed scenario(s) failed"
exit $((failed > 0))
