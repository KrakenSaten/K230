#!/bin/bash
# RIFT in the running app and in the running shell.
#
#   1. rift_app_test: the app under a real LVGL pointer device - the chrome,
#      both sections, the 36 px row that only selects, the pushed detail, the
#      landscape split, and open/leave/open again. It writes the screenshots
#      when $SHOTS_DIR is set.
#   2. The real shell, upright and turned, opening RIFT with a scripted
#      meshcored on the other end of a real socket: the whole stack, chrome
#      included, and the app's own client doing the talking.
#   3. The same shell with no meshcored at all, which is what a unit that has
#      not enabled the service looks like. Opening RIFT must not be a fault.
#   4. The screens are reproducible: the same fixtures twice give the same
#      pixels.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell; rift_app_test is built
# beside it) and tests/fake-meshcored (make all / make test).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
FAKE=${FAKE_MESHCORED:-tests/fake-meshcored}

# The mesh these screens are drawn from. Every last_heard_mono_ms is
# negative, which the fake reads as "this long ago": a fixed monotonic stamp
# would be an age of however long this host has been up, and the ages on
# screen would differ every run.
NODES='[
 {"public_key":"a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1",
  "node_hash":"a1","name":"OSLO-01","type":1,"path_known":true,"hops":0,"direct":true,
  "last_heard_mono_ms":-90000,"last_rssi_dbm":-71.0,"last_snr_db":9.5},
 {"public_key":"b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2",
  "node_hash":"b2","name":"HYTTA","type":2,"path_known":true,"hops":8,"direct":false,
  "path_hex":"a1c2d34a5b6c7d8e","last_heard_mono_ms":-700000,"last_rssi_dbm":-88.0},
 {"public_key":"c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3",
  "node_hash":"c3","name":"NO-3241 FO","type":1,"path_known":false,
  "last_heard_mono_ms":-14400000}
]'

# ---- 1. the app itself ---------------------------------------------------------
BIN=${RIFT_APP_TEST:-$(dirname "$SHELL_BIN")/rift_app_test}
if [ -x "$BIN" ]; then
    EMPTY=$(mktemp -d)
    if [ -n "${SHOTS_DIR:-}" ]; then
        mkdir -p "$SHOTS_DIR"
        log=$(POCKETOS_RUNTIME_DIR="$EMPTY" RIFT_SHOTS_DIR="$SHOTS_DIR" \
              RIFT_THEME="${RIFT_THEME:-carbon}" RIFT_MODE="${RIFT_MODE:-normal}" \
              "$BIN" 2>&1); rc=$?
        check "the screens were written to $SHOTS_DIR" \
            "$([ "$(ls "$SHOTS_DIR"/*.png 2>/dev/null | wc -l)" -ge 7 ] && echo 1 || echo 0)"
    else
        log=$(POCKETOS_RUNTIME_DIR="$EMPTY" "$BIN" 2>&1); rc=$?
    fi
    printf '%s\n' "$log" | grep -E '^FAIL|rift_app_test:'
    check "RIFT end to end, tapped on the panel, in both orientations" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    rm -rf "$EMPTY"
else
    check "rift_app_test binary present ($BIN)" 0
fi

# ---- the app is the shell's, and is built by it ---------------------------------
check "the shell knows about RIFT" \
    "$(grep -q 'app_rift' ui/shell/shell.c && echo 1 || echo 0)"
check "and builds every part of it" \
    "$(grep -q 'apps/rift/rift_app.c' ui/shell/CMakeLists.txt &&
       grep -q 'apps/rift/ui/rift_nodes.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
# Where RIFT is on the launcher is the launcher's table (DS §31.2): first in
# CONNECTIONS, in the package's mesh colour.
check "RIFT is on the launcher, first in CONNECTIONS" \
    "$(grep -q '{ "rift", HOME_GROUP_CONNECT, HOME_HUE_MESH[ ,}]' ui/shell/home_layout.c &&
       [ "$(grep -o '{ "[a-z]*", HOME_GROUP_CONNECT' ui/shell/home_layout.c | head -1)" = '{ "rift", HOME_GROUP_CONNECT' ] &&
       echo 1 || echo 0)"

# ---- 2. the real shell, with a scripted meshcored -------------------------------
if [ -x "$FAKE" ]; then
    for rotation in portrait landscape; do
        RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
        POCKETOS_RUNTIME_DIR="$RUN" FAKE_MESHCORED_STATE=online \
        FAKE_MESHCORED_REASON="receiving" FAKE_MESHCORED_NODES="$NODES" \
        FAKE_MESHCORED_METHODS="$OUT/methods-$rotation.txt" \
        FAKE_MESHCORED_LIFE_MS=6000 "$FAKE" & FP=$!
        for _ in $(seq 1 60); do [ -S "$RUN/meshcored.sock" ] && break; sleep 0.05; done
        check "a scripted meshcored is listening ($rotation)" \
            "$([ -S "$RUN/meshcored.sock" ] && echo 1 || echo 0)"
        POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
        POCKETOS_STATE_DIR="$STATE" \
            "$SHELL_BIN" --rotation "$rotation" --open rift --theme carbon \
            --screenshot "$OUT/shell-$rotation.png" --exit-after-ms 2000 \
            >"$LOGD/out" 2>&1
        rc=$?
        check "the shell opens RIFT in $rotation" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
        check "and logs no fault" \
            "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
        check "RIFT reports itself open in $rotation" \
            "$(grep -q 'open app rift' "$LOGD/log/shell.log" 2>/dev/null ||
               grep -q 'open app rift' "$LOGD/out" && echo 1 || echo 0)"
        check "and says it reached meshcored" \
            "$(grep -q 'rift: open, meshcored connected' "$LOGD/log/shell.log" 2>/dev/null ||
               grep -q 'rift: open, meshcored connected' "$LOGD/out" && echo 1 || echo 0)"
        check "a screenshot of the whole stack in $rotation" \
            "$([ -s "$OUT/shell-$rotation.png" ] && echo 1 || echo 0)"
        # The whole point of the boundary: a client that only reads.
        if [ -s "$OUT/methods-$rotation.txt" ]; then
            check "the app asked meshcored for what it draws ($rotation)" \
                "$(grep -q 'mesh.nodes' "$OUT/methods-$rotation.txt" && echo 1 || echo 0)"
            check "and never asked it to transmit ($rotation)" \
                "$(grep -qE '^mesh\.(send|advert)$' "$OUT/methods-$rotation.txt" &&
                   echo 0 || echo 1)"
        else
            check "meshcored recorded what it was asked for ($rotation)" 0
        fi
        # Opening an app must not write anything: RIFT has no store.
        check "opening RIFT writes nothing to the store ($rotation)" \
            "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
        kill "$FP" 2>/dev/null; wait "$FP" 2>/dev/null
        if [ -n "${SHOTS_DIR:-}" ]; then
            cp "$OUT/shell-$rotation.png" "$SHOTS_DIR/" 2>/dev/null
        fi
        rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
    done
else
    check "tests/fake-meshcored present (make all)" 0
fi

# ---- 3. no meshcored at all ------------------------------------------------------
# What a unit that has not enabled the service looks like. meshcored ships
# disabled (docs/services/MESHCORED.md), so this is the ordinary case.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open rift --theme carbon --screenshot "$OUT/shell-no-service.png" \
    --exit-after-ms 1500 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens RIFT with no meshcored at all" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and that is not a fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "RIFT says the service is not answering" \
    "$(grep -q 'rift: open, meshcored not answering' "$LOGD/log/shell.log" 2>/dev/null ||
       grep -q 'rift: open, meshcored not answering' "$LOGD/out" && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

# ---- 4. the screens are reproducible ----------------------------------------------
if [ -x "$BIN" ]; then
    A=$(mktemp -d); B=$(mktemp -d); EMPTY=$(mktemp -d)
    POCKETOS_RUNTIME_DIR="$EMPTY" RIFT_SHOTS_DIR="$A" RIFT_THEME=carbon RIFT_MODE=normal \
        "$BIN" >/dev/null 2>&1
    POCKETOS_RUNTIME_DIR="$EMPTY" RIFT_SHOTS_DIR="$B" RIFT_THEME=carbon RIFT_MODE=normal \
        "$BIN" >/dev/null 2>&1
    same=1
    for f in "$A"/*.png; do
        cmp -s "$f" "$B/$(basename "$f")" || { same=0; echo "     differs: $(basename "$f")"; }
    done
    check "the same fixtures twice give the same pixels" \
        "$([ "$same" = "1" ] && [ -n "$(ls -A "$A")" ] && echo 1 || echo 0)"
    rm -rf "$A" "$B" "$EMPTY"
fi

[ "$failed" -eq 0 ] && rm -rf "$OUT" || echo "output kept in $OUT"
echo "rift_shell_test: $failed failure(s)"
exit $((failed > 0))
