#!/bin/bash
# PocketFleet multiplayer end to end (P6, docs/apps/FLEET_MULTIPLAYER.md):
# two whole meshcored processes over stand-in radiods and a mock air, and two
# Fleet players - the real session, mesh link and match state machine - that
# play whole matches through them. See tests/fleet_mp_e2e.py.
#
#   FLEET_E2E_SCENARIOS="real lossy crash restart" bash tests/fleet_mp_e2e_test.sh
#
# The scenarios run side by side, each in its own directory with its own
# sockets and state. "real" plays at the product's own pacing and takes about
# ten minutes; the rest refill the airtime governor's token bucket and take
# under one, except "lossy", whose retries take several.
#
# No hardware, no radio, and nothing here transmits anything anywhere.
set -u
cd "$(dirname "$0")/.." || exit 1

MESHCORED=${MESHCORED:-services/meshcored/meshcored}
PLAYER=${PLAYER:-tests/fleet_mp_player}
SCENARIOS=${FLEET_E2E_SCENARIOS:-real lossy crash restart}

for bin in "$MESHCORED" "$PLAYER"; do
    if [ ! -x "$bin" ]; then
        echo "FAIL $bin is not built; run 'make ENABLE_MESHCORED=1 meshcored tests/fleet_mp_player'"
        exit 1
    fi
done

TMP=$(mktemp -d)
cleanup() {
    pkill -f "$TMP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

pids=""
for sc in $SCENARIOS; do
    mkdir -p "$TMP/$sc"
    python3 tests/fleet_mp_e2e.py "$MESHCORED" "$PLAYER" "$TMP/$sc" "$sc" > "$TMP/$sc.out" 2>&1 &
    pids="$pids $!"
done
failed=0
for p in $pids; do
    wait "$p" || failed=$((failed + 1))
done
for sc in $SCENARIOS; do
    cat "$TMP/$sc.out"
done
echo "fleet_mp_e2e_test: $failed scenario(s) failed"
exit $((failed > 0))
