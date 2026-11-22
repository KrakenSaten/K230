#!/bin/bash
# capture.request_start and capture.request_cancel, served by PacketShark on
# shell.sock (apps/packetshark/psk_app.c; docs/api/shell.md; ADR-016
# amendment 1 rules 1 and 1a): in the real shell, on the simulator's
# synthetic traffic, asked over the socket.
#
# What is held: a request opens PacketShark and raises a sheet, and starts
# nothing - no helper, no capture - however long it is left; a locked unit is
# not opened and shows nothing; REFUSE, a cancel, the lock, leaving and 60
# seconds each end it, and each is said once as capture.state; only a tap on
# the sheet's START starts a capture, the one the sheet named, HEADERS, with
# its limit, marked as asked for; a request cannot be made twice, over a
# capture, or for what the setup screen does not offer.
#
# The 60 seconds are waited for in full, once.
#
# Requires: SHELL_BIN (the CMake-built simulator, pocketos-shell), make's
# tools/capture/pos-capture, python3. Run from the repository root.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
HELPER=$(pwd)/tools/capture/pos-capture
[ -x "$HELPER" ] || { echo "FAIL build the helper first: make tools/capture/pos-capture"; exit 1; }
if ! grep -aq '/usr/bin/pos-capture' "$SHELL_BIN"; then
    echo "SKIP $SHELL_BIN was configured with -DPOCKETOS_WITH_PACKETSHARK=OFF"
    exit 0
fi
CALL="python3 tests/pocketipc_call.py"
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
OUT=$(mktemp -d)
SOCK="$POCKETOS_RUNTIME_DIR/shell.sock"
failed=0
SP=
LP=
cleanup() {
    [ -n "$LP" ] && kill "$LP" 2>/dev/null
    [ -n "$SP" ] && kill "$SP" 2>/dev/null
    rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR" "$OUT"
}
trap cleanup EXIT
check() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -6 | cut -c1-300; failed=$((failed + 1)); fi; }
is() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; failed=$((failed + 1)); fi; }
call() { $CALL "$SOCK" "$@" 2>&1; }
info() { call shell.info | sed 's/^OK //' | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
status() { call capture.status | sed 's/^OK //'; }
of() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($2)" "$1" 2>&1; }
state() { of "$(status)" 'd.get("state")'; }
start_shell() {
    rm -f "$SOCK"
    POCKETOS_CAPTURE_SOURCE=synth:200:1 POCKETOS_CAPTURE_HELPER="$HELPER" "$SHELL_BIN" >"$OUT/shell.out" 2>&1 &
    SP=$!
    for _ in $(seq 1 80); do [ -S "$SOCK" ] && return 0; kill -0 $SP 2>/dev/null || return 1; sleep 0.1; done
    return 1
}
stop_shell() { [ -n "$SP" ] && kill $SP 2>/dev/null; wait $SP 2>/dev/null; SP=; }
wait_state() { for _ in $(seq 1 "$2"); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
helpers() { pgrep -f "$HELPER session" | wc -l | tr -d ' '; }
# The capture.state events so far, as "idle,idle/refused,capturing".
states() {
    python3 - "$OUT/events" <<'PY'
import json, re, sys
out = []
for line in open(sys.argv[1]):
    m = re.match(r"EVENT capture\.state (.*)$", line.rstrip("\n"))
    if m:
        d = json.loads(m.group(1))
        assert sorted(d) == ["iface", "keep", "limit_s", "reason", "state"], sorted(d)
        out.append(d["state"] + ("" if d["reason"] == "none" else "/" + d["reason"]))
print(",".join(out))
PY
}
# The sheet is up when its own START can be found to tap; the setup screen's
# button reads START CAPTURE, which is another label.
REQ='{"iface": "synth0", "limit_s": 300, "client": "Flip8"}'
ask() { call capture.request_start "${1:-$REQ}"; }

# ---- 1. a locked unit is not opened, and is shown nothing ----------------------------------
start_shell || { echo "FAIL the shell did not start"; tail -5 "$OUT/shell.out"; exit 1; }
is "the shell starts locked" True "$(info 'd["lock"]["locked"]')"
check "locked: a request is refused" '^ERR 3 the unit is locked or showing an alarm$' "$(ask)"
is "and neither opened the lock nor the app" "True home" "$(info 'd["lock"]["locked"]') $(info 'd["current"]')"
check "locked: a cancel is refused" '^ERR 3 PacketShark is not open$' "$(call capture.request_cancel)"
check "unlock, as a person at the unit" '"locked": false' "$(call shell.unlock)"
$CALL "$SOCK" --listen 170 shell.subscribe "$OUT/ready" >"$OUT/events" 2>&1 &
LP=$!
for _ in $(seq 1 40); do [ -e "$OUT/ready" ] && break; sleep 0.1; done

# ---- 2. asked: the app opens, a sheet is up, nothing starts ----------------------------------
is "PacketShark is closed" home "$(info 'd["current"]')"
is "a request opens PacketShark and says its sheet is up" 'OK {"shown": true}' "$(ask)"
sleep 0.6
is "PacketShark is in front" packetshark "$(info 'd["current"]')"
is "asking started nothing: setup, no helper, no capture log" "idle 0 no" "$(state) $(helpers) $([ -e "$POCKETOS_LOG_DIR/pos-capture.log" ] && echo yes || echo no)"
check "a second request while one is shown" '^ERR 3 a request is already shown$' "$(ask)"
check "capture.stop has nothing to stop" '^ERR 3 nothing is capturing$' "$(call capture.stop)"
check "REFUSE, tapped" '"tapped": true' "$(call shell.tap '{"text": "REFUSE"}')"
sleep 0.7
check "the sheet is gone: its START is no longer there to tap" '^ERR 2 no such text' "$(call shell.tap '{"text": "START"}')"
is "said once: refused; and still nothing started" "idle,idle/refused idle 0" "$(states) $(state) $(helpers)"

# ---- 3. what cannot be asked ---------------------------------------------------------------------
check "an interface that is not offered" '^ERR 3 that interface is not offered$' "$(ask '{"iface": "eth7", "limit_s": 300}')"
check "no limit" '^ERR 3 that time limit is not offered$' "$(ask '{"iface": "synth0", "limit_s": 0}')"
check "a limit that is not of the list" '^ERR 3 that time limit is not offered$' "$(ask '{"iface": "synth0", "limit_s": 7200}')"
check "no parameters" '^ERR 2 ' "$(call capture.request_start)"
check "a cancel with nothing shown" '^ERR 3 no request is shown$' "$(call capture.request_cancel)"
check "no sheet came of any of it" '^ERR 2 no such text' "$(call shell.tap '{"text": "REFUSE"}')"

# ---- 4. cancelled, and locked over ------------------------------------------------------------------
is "asked again" 'OK {"shown": true}' "$(ask)"
is "capture.request_cancel takes the sheet down" 'OK {"shown": false}' "$(call capture.request_cancel)"
sleep 0.7
check "the sheet is gone" '^ERR 2 no such text' "$(call shell.tap '{"text": "START"}')"
is "said: refused" "idle,idle/refused,idle/refused" "$(states)"
is "asked again" 'OK {"shown": true}' "$(ask)"
check "the unit is locked with the question up" '"locked": true' "$(call shell.lock)"
sleep 0.5
check "unlocked again" '"locked": false' "$(call shell.unlock)"
sleep 0.7
check "the lock took the sheet down: nobody finds a START they did not see asked for" '^ERR 2 no such text' "$(call shell.tap '{"text": "START"}')"
is "said once the unit was open again: refused; and nothing started" "idle,idle/refused,idle/refused,idle/refused 0" "$(states) $(helpers)"

# ---- 5. sixty seconds ----------------------------------------------------------------------------------
is "asked again" 'OK {"shown": true}' "$(ask)"
sleep 50
check "after 50 seconds the sheet is still there (REFUSE is not tapped: its place is looked up)" 'a request is already shown' "$(ask)"
is "and still nothing has started" "idle 0" "$(state) $(helpers)"
# Waited for by its event, never by a tap on START (which would start a
# capture if the sheet were still there).
for _ in $(seq 1 50); do states | grep -q 'idle/timeout$' && break; sleep 0.5; done
check "after 60 the sheet has gone by itself" '^ERR 2 no such text' "$(call shell.tap '{"text": "REFUSE"}')"
is "said: timeout; and nothing started in all that time" "idle,idle/refused,idle/refused,idle/refused,idle/timeout idle 0 no" \
   "$(states) $(state) $(helpers) $([ -e "$POCKETOS_LOG_DIR/pos-capture.log" ] && echo yes || echo no)"

# ---- 6. START, tapped on the sheet -----------------------------------------------------------------------
is "asked again, with more than may be asked" 'OK {"shown": true}' "$(ask '{"iface": "synth0", "limit_s": 300, "client": "Flip8", "keep": "full", "promisc": true, "start": true}')"
check "START, tapped in the sheet's first moment" '"tapped": true' "$(call shell.tap '{"text": "START"}')"
sleep 0.6
check "is not an answer: the sheet is still up" 'a request is already shown' "$(ask)"
sleep 1.5
is "and nothing has started" "idle 0" "$(state) $(helpers)"
check "START, tapped on the sheet by a finger once it has been up a moment" '"tapped": true' "$(call shell.tap '{"text": "START"}')"
wait_state capturing 50; is "capturing" 0 $?
sleep 1.5
S=$(status)
is "the capture the sheet named: synth0, 5 minutes, HEADERS, marked as asked for" "synth0 300 headers True" \
   "$(of "$S" '" ".join([d["iface"], str(d["limit_s"]), d["keep"], str(d["requested_by_companion"])])')"
is "one helper, the app's own" 1 "$(helpers)"
check "a request over a running capture" '^ERR 3 a capture is running$' "$(ask)"
is "capture.stop stops it like any other" 'OK {"changing": true, "state": "capturing"}' "$(call capture.stop)"
wait_state stopped 30; is "stopped" 0 $?
check "a request over a stopped capture" '^ERR 3 a capture is running$' "$(ask)"
sleep 0.6

# ---- 7. leaving with the question unanswered -----------------------------------------------------------
call shell.home >/dev/null; sleep 0.6
[ "$(info 'd["current"]')" != home ] && { call shell.tap '{"text": "LEAVE"}' >/dev/null; sleep 0.8; }
is "home, the capture gone with the app" "home 0" "$(info 'd["current"]') $(helpers)"
is "asked from home: the app opens again" 'OK {"shown": true} packetshark' "$(ask) $(sleep 0.6; info 'd["current"]')"
call shell.home >/dev/null; sleep 0.8
is "left with the sheet up: home, and nothing started" "home 0" "$(info 'd["current"]') $(helpers)"
check "PacketShark is closed, so its sheet is too" '^ERR 3 PacketShark is not open$' "$(call capture.request_cancel)"
sleep 0.5
kill $LP 2>/dev/null; wait $LP 2>/dev/null
LP=
is "all of it, in order: each outcome once, the capture between, and leaving twice" \
   "idle,idle/refused,idle/refused,idle/refused,idle/timeout,capturing,stopped/user,ended/left,idle,idle/refused" "$(states)"
is "a request never reaches a log" 0 "$(grep -c -i 'flip8' "$OUT/shell.out" "$POCKETOS_LOG_DIR"/*.log 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')"
echo "     opened by request $(info 'd["app_ipc"]["opened"]'), refused to open $(info 'd["app_ipc"]["open_refused"]'), slow $(info 'd["app_ipc"]["slow"]'), worst $(info 'd["app_ipc"].get("worst_us")') us ($(info 'd["app_ipc"].get("worst_method")'))"
is "the shell opened the app for a request twice, and refused once (the lock)" "2 1" "$(info 'd["app_ipc"]["opened"]') $(info 'd["app_ipc"]["open_refused"]')"
stop_shell

echo "capture_request_shell_test: $failed failure(s)"
exit $((failed > 0))
