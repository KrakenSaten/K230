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
#   5. RIFT kept behind other screens (DS §51): the status cluster's mark in
#      shell.info across home, another app and RIFT again, one meshcored
#      connection throughout, and the session ended when the shell stops.
#      Needs pos (make all).
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
              RIFT_FAKE_MESHCORED="$(realpath "$FAKE" 2>/dev/null)" \
              RIFT_THEME="${RIFT_THEME:-carbon}" RIFT_MODE="${RIFT_MODE:-normal}" \
              "$BIN" 2>&1); rc=$?
        check "the screens were written to $SHOTS_DIR" \
            "$([ "$(ls "$SHOTS_DIR"/*.png 2>/dev/null | wc -l)" -ge 7 ] && echo 1 || echo 0)"
    else
        log=$(POCKETOS_RUNTIME_DIR="$EMPTY" RIFT_FAKE_MESHCORED="$(realpath "$FAKE" 2>/dev/null)" \
              "$BIN" 2>&1); rc=$?
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
# Where RIFT is on the launcher is the launcher's table (DS §31.2, §47): in
# ESSENTIALS after Terminal, in the package's mesh colour.
check "RIFT is on the launcher, in ESSENTIALS after Terminal" \
    "$(grep -q '{ "rift", HOME_GROUP_ESSENTIALS, HOME_HUE_MESH, HOME_FOLDER_NONE }' ui/shell/home_layout.c &&
       [ "$(grep -o '{ "[a-z]*", HOME_GROUP_ESSENTIALS' ui/shell/home_layout.c | sed -n 2p)" = '{ "rift", HOME_GROUP_ESSENTIALS' ] &&
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
            # Nor to change what it holds: joining, leaving, renaming and the
            # path hash size are a reader's presses on ACTIVITY, never opening.
            check "nor to change the node on its own ($rotation)" \
                "$(grep -qE '^mesh\.(channel_add|channel_remove|set_name|set_path_hash)$' \
                    "$OUT/methods-$rotation.txt" && echo 0 || echo 1)"
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

# ---- 5. kept behind other screens, in the real shell (DS §51) ---------------------
# RIFT opened, left for home and for another app, opened again: the status
# cluster's mark is RIFT's own state, the one meshcored connection lasts the
# whole time, and the shell stopping ends the session cleanly.
POS=${POS:-tools/pos/pos}
if [ -x "$FAKE" ] && [ -x "$POS" ]; then
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    export POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
        POCKETOS_STATE_DIR="$STATE"
    FAKE_MESHCORED_STATE=online FAKE_MESHCORED_REASON="receiving" FAKE_MESHCORED_NODES="$NODES" \
    FAKE_MESHCORED_METHODS="$OUT/methods-bg.txt" FAKE_MESHCORED_LIFE_MS=60000 "$FAKE" & FP=$!
    for _ in $(seq 1 60); do [ -S "$RUN/meshcored.sock" ] && break; sleep 0.05; done
    # Portrait at Large with the widest chip: where the launcher's centred
    # clock and the cluster with the mark come closest (DS §51.4).
    POCKETOS_TEST_RADIO_STATE=rx POCKETOS_TERMINAL_SHELL=/bin/sh \
    "$SHELL_BIN" --theme carbon --rotation portrait --text-size large >"$LOGD/out" 2>&1 & SP=$!
    for _ in $(seq 1 80); do [ -S "$RUN/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
    logged() { grep -rqF "$1" "$LOGD"; }
    # "<app>:<mark's text>,... <mark shown> <cluster width> <current>"
    bgstate() {
        "$POS" shell info 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(",".join(b["app"] + ":" + b["shown"] for b in d["background"]) or "-",
      d["background_mark"], d["chrome"]["cluster"]["w"], d["current"])' 2>/dev/null
    }
    set -- $(bgstate); w0=${3:-0}
    check "the shell starts with nothing behind its screens" \
        "$([ "${1:-}" = "-" ] && [ "${2:-}" = "False" ] && echo 1 || echo 0)"
    "$POS" app start rift >/dev/null 2>&1; sleep 0.8
    set -- $(bgstate)
    check "RIFT open: not in the background, no mark" \
        "$([ "${4:-}" = "rift" ] && [ "${1:-}" = "-" ] && echo 1 || echo 0)"
    "$POS" call shell shell.home >/dev/null 2>&1; sleep 0.5
    info=$("$POS" shell info 2>/dev/null)
    set -- $(bgstate)
    check "home: RIFT kept and marked RIFT, 'RIFT active in background'" \
        "$(printf '%s' "$info" | grep -q 'RIFT active in background' &&
           [ "${1:-}" = "rift:RIFT" ] && [ "${2:-}" = "True" ] && [ "${4:-}" = "home" ] &&
           echo 1 || echo 0)"
    check "and the cluster is wider by the mark (${w0} -> ${3:-?})" \
        "$([ "${3:-0}" -gt "$w0" ] && echo 1 || echo 0)"
    clear=$(printf '%s' "$info" | python3 -c '
import json, sys
d = json.load(sys.stdin)
k, t = d["chrome"]["cluster"], d["launcher"]["time"]
print(k["x"] - (t["x"] + t["w"]))' 2>/dev/null)
    check "the launcher's clock stays 16 px clear of it at Large (${clear:-?} px)" \
        "$([ "${clear:-0}" -ge 16 ] && echo 1 || echo 0)"
    "$POS" app start calculator >/dev/null 2>&1; sleep 0.5
    set -- $(bgstate)
    check "another app open: still marked" \
        "$([ "${4:-}" = "calculator" ] && [ "${2:-}" = "True" ] && echo 1 || echo 0)"
    "$POS" app start rift >/dev/null 2>&1; sleep 0.8
    set -- $(bgstate)
    check "RIFT again: the mark goes" \
        "$([ "${4:-}" = "rift" ] && [ "${1:-}" = "-" ] && [ "${2:-}" = "False" ] && echo 1 || echo 0)"
    check "and RIFT says it reopened the session it kept" \
        "$(logged 'rift: open again (session kept' && echo 1 || echo 0)"
    # The Terminal's kept shell: its own mark beside RIFT's, each set and
    # cleared by its own app only; two fit where one did (R and >_).
    "$POS" app start terminal >/dev/null 2>&1; sleep 0.8
    set -- $(bgstate)
    check "Terminal open: only RIFT marked (${1:-?})" "$([ "${1:-}" = "rift:RIFT" ] && echo 1 || echo 0)"
    "$POS" call shell shell.home >/dev/null 2>&1; sleep 0.5
    info=$("$POS" shell info 2>/dev/null)
    set -- $(bgstate)
    check "home: two marks, RIFT's and the Terminal's (${1:-?}, ${3:-?} px)" \
        "$([ "${1:-}" = "rift:R,terminal:>_" ] && [ "${2:-}" = "True" ] && echo 1 || echo 0)"
    clear=$(printf '%s' "$info" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(d["chrome"]["cluster"]["x"] - d["launcher"]["time"]["x"] - d["launcher"]["time"]["w"])' 2>/dev/null)
    check "the launcher's clock stays 16 px clear of both at Large (${clear:-?} px)" \
        "$([ "${clear:-0}" -ge 16 ] && echo 1 || echo 0)"
    "$POS" app start rift >/dev/null 2>&1; sleep 0.8
    set -- $(bgstate)
    check "RIFT open: only the Terminal marked, in full (${1:-?})" \
        "$([ "${1:-}" = "terminal:>_" ] && echo 1 || echo 0)"
    "$POS" call shell shell.home >/dev/null 2>&1; sleep 0.5
    tsh=$(ps -o pid= --ppid "$SP" 2>/dev/null | tr -d ' ' | head -n 1)
    [ -n "$tsh" ] && kill -KILL "$tsh" 2>/dev/null; sleep 0.8
    set -- $(bgstate)
    check "the Terminal's shell ended: its mark goes, RIFT's stays (${1:-?})" \
        "$([ -n "$tsh" ] && [ "${1:-}" = "rift:RIFT" ] && echo 1 || echo 0)"
    "$POS" call shell shell.home >/dev/null 2>&1; sleep 0.4
    kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
    check "the shell stopping ends the kept session" \
        "$(logged 'rift: session ended' && echo 1 || echo 0)"
    check "one connection for all of it, given back at the end" \
        "$([ "$(grep -c '^mesh.subscribe$' "$OUT/methods-bg.txt" 2>/dev/null)" = 1 ] &&
           [ "$(grep -c '^mesh.unsubscribe$' "$OUT/methods-bg.txt" 2>/dev/null)" = 1 ] &&
           echo 1 || echo 0)"
    check "nothing put on the air" \
        "$(grep -qE '^mesh\.(send|advert)$' "$OUT/methods-bg.txt" && echo 0 || echo 1)"
    check "and no fault" "$(grep -rqE ' ERROR |assert' "$LOGD" && echo 0 || echo 1)"
    kill "$FP" 2>/dev/null; wait "$FP" 2>/dev/null
    unset POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
else
    check "tests/fake-meshcored and pos present (make all) for the kept session" 0
fi

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
