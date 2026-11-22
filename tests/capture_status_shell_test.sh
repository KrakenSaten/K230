#!/bin/bash
# capture.status, served by PacketShark on shell.sock (apps/packetshark/
# psk_remote.h, docs/api/shell.md "Methods served by an app"): in the real
# shell, on the simulator's synthetic traffic, asked over the socket as
# gatewayd would one day ask it.
#
# What is held: closed, locked and in front; setup, capturing, hold, stopped,
# a helper that crashes and one that is missing; that every answer has the
# contract's keys and no other and no text but four checked words; that
# asking starts nothing, sends the helper nothing and changes nothing; that
# no other capture.* name does anything, in front or not, and none opens the
# app; that nothing is raised as an event.
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
# The status answer's JSON, and one expression over it (d).
status() { call capture.status | sed 's/^OK //'; }
of() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($2)" "$1" 2>&1; }
# "ok", or what is wrong with the shape of an answer.
shape() {
    python3 - "$1" <<'PY'
import json, sys
TOP = ["available", "state", "reason", "iface", "keep", "elapsed_ms", "limit_s", "seen", "stored", "held",
       "kernel_drops", "iface_drops", "shed", "bytes", "pps", "bps", "fill_permille", "classes", "findings",
       "diag", "requested_by_companion"]
CLASSES = ["tcp", "udp", "dns", "dhcp", "arp", "icmp", "ipv6", "other"]
DIAG = ["dns_queries", "dns_retries", "dns_answered", "dns_late", "dns_noreply", "dns_waiting", "dns_nxdomain",
        "dns_servfail", "dns_refused", "dns_rtt_sum_us", "dns_rtt_max_us", "tcp_attempts", "tcp_connected",
        "tcp_timed", "tcp_hs_sum_us", "tcp_hs_max_us", "tcp_refused", "tcp_aborted"]
STATES = ["idle", "capturing", "hold", "saving", "stopped", "ended"]
REASONS = ["none", "user", "limit", "link", "end", "error", "helper_crashed", "helper_hung"]
import re
def whole(v): return type(v) is int and v >= 0
try:
    d = json.loads(sys.argv[1])
    assert sorted(d) == sorted(TOP), "keys: %s" % sorted(d)
    assert d["available"] is True and d["requested_by_companion"] is False
    assert d["state"] in STATES and d["reason"] in REASONS and d["keep"] in ("headers", "full")
    assert re.fullmatch(r"((eth|wlan|usb|synth)[0-9]{1,2})?", d["iface"]), "iface"
    for k in TOP:
        if k not in ("available", "state", "reason", "iface", "keep", "classes", "findings", "diag",
                     "requested_by_companion"):
            assert whole(d[k]), k
    assert sorted(d["classes"]) == sorted(CLASSES) and all(whole(v) for v in d["classes"].values()), "classes"
    assert len(d["findings"]) == 6 and all(whole(v) for v in d["findings"]), "findings"
    assert sorted(d["diag"]) == sorted(DIAG) and all(whole(v) for v in d["diag"].values()), "diag"
    # Nothing in the text that could be an address: no dotted or coloned run of digits.
    assert not re.search(r"\d+\.\d+\.\d+|[0-9a-f]{2}:[0-9a-f]{2}:", sys.argv[1]), "an address"
    print("ok")
except Exception as e:
    print("bad:", e)
PY
}
start_shell() { # $1: the capture source, $2: the helper
    rm -f "$SOCK"
    POCKETOS_CAPTURE_SOURCE="$1" POCKETOS_CAPTURE_HELPER="$2" "$SHELL_BIN" >"$OUT/shell.out" 2>&1 &
    SP=$!
    for _ in $(seq 1 80); do [ -S "$SOCK" ] && return 0; kill -0 $SP 2>/dev/null || return 1; sleep 0.1; done
    return 1
}
stop_shell() { [ -n "$SP" ] && kill $SP 2>/dev/null; wait $SP 2>/dev/null; SP=; }
wait_state() { # $1: the state, $2: tenths of a second
    for _ in $(seq 1 "$2"); do [ "$(of "$(status)" 'd.get("state")')" = "$1" ] && return 0; sleep 0.1; done
    return 1
}
helpers() { pgrep -f "$HELPER session" | wc -l | tr -d ' '; }

# ---- 1. not in front: behind the lock, and closed ------------------------------
start_shell synth:200:1 "$HELPER" || { echo "FAIL the shell did not start"; tail -5 "$OUT/shell.out"; exit 1; }
is "the shell starts locked" True "$(info 'd["lock"]["locked"]')"
is "locked: the status says not available, and nothing else" 'OK {"available": false}' "$(call capture.status)"
check "unlock, as a person at the unit" '"locked": false' "$(call shell.unlock)"
is "home" home "$(info 'd["current"]')"
is "closed: the status says not available, and nothing else" 'OK {"available": false}' "$(call capture.status)"
for m in capture.hold capture.stop capture.request_start capture.discard capture.save capture.rows capture.x; do
    check "closed: $m is refused as unavailable" '^ERR 3 PacketShark is not open$' "$(call $m '{"on": true, "iface": "synth0", "limit_s": 300}')"
done
is "and none of them opened the app" home "$(info 'd["current"]')"
is "nor called it" 0 "$(info 'd["app_ipc"]["calls"]')"
is "nor started a helper" 0 "$(helpers)"
is "PacketShark declares no method that opens it" 0 "$(grep -cE '\.ipc_opens[[:space:]]*=' apps/packetshark/psk_app.c)"
is "and one status method" '"capture.status", NULL' \
   "$(grep -E 'psk_ipc_status\[\] *=' apps/packetshark/psk_app.c | sed 's/.*{ *//; s/ *}.*//')"

# ---- 2. in front, on setup -------------------------------------------------------
# A listener from before the app opens until the capture has stopped: it must
# see the shell's own events, and none of the app's.
$CALL "$SOCK" --listen 60 shell.subscribe "$OUT/ready" >"$OUT/events" 2>&1 &
LP=$!
for _ in $(seq 1 40); do [ -e "$OUT/ready" ] && break; sleep 0.1; done
is "a listener is subscribed" yes "$([ -e "$OUT/ready" ] && echo yes || echo no)"
check "open PacketShark" '"current": "packetshark"' "$(call shell.open '{"id": "packetshark"}')"
sleep 0.5
S=$(status)
is "setup: the contract's shape" ok "$(shape "$S")"
is "setup: idle, no reason, no interface, HEADERS" "idle none [] headers" "$(of "$S" '" ".join([d["state"], d["reason"], "[" + d["iface"] + "]", d["keep"]])')"
is "setup: every figure zero" 0 "$(of "$S" 'sum(v for k, v in d.items() if type(v) is int) + sum(d["classes"].values()) + sum(d["findings"]) + sum(d["diag"].values())')"
is "asking started no helper" 0 "$(helpers)"
is "and wrote no capture log" no "$([ -e "$POCKETOS_LOG_DIR/pos-capture.log" ] && echo yes || echo no)"
check "in front: a method that is not served is unknown" '^ERR 1 unknown method$' "$(call capture.request_start '{"iface": "synth0", "limit_s": 300}')"
sleep 0.3
is "and it started nothing" "idle 0" "$(of "$(status)" 'd["state"]') $(helpers)"

# ---- 3. capturing: only a tap on the unit starts it ------------------------------
check "START, tapped on the panel" '"tapped": true' "$(call shell.tap '{"text": "START CAPTURE"}')"
wait_state capturing 50; is "capturing" 0 $?
sleep 1.6
A=$(status)
is "capturing: the contract's shape" ok "$(shape "$A")"
is "capturing: the state, the interface, HEADERS, the 15 minutes" "capturing none synth0 headers 900" \
   "$(of "$A" '" ".join([d["state"], d["reason"], d["iface"], d["keep"], str(d["limit_s"])])')"
is "capturing: packets are counted, and a rate" True "$(of "$A" 'd["seen"] > 100 and d["bytes"] > d["seen"] and d["pps"] > 0 and d["elapsed_ms"] > 1000')"
is "capturing: the classes add up to the packets seen" True "$(of "$A" 'sum(d["classes"].values()) == d["seen"]')"
sleep 1.2
B=$(status)
is "capturing: the figures move" True "$(python3 -c "import json,sys; a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2]); print(b['seen'] > a['seen'] and b['elapsed_ms'] > a['elapsed_ms'] and b['bytes'] > a['bytes'])" "$A" "$B")"
is "one helper, the app's own" 1 "$(helpers)"
for m in capture.hold capture.stop capture.discard; do
    check "capturing: $m is not served" '^ERR 1 unknown method$' "$(call $m '{"on": true}')"
done
sleep 0.4
is "and the capture runs on" capturing "$(of "$(status)" 'd["state"]')"

# ---- 4. the lock over a capture ----------------------------------------------------
check "lock the unit" '"locked": true' "$(call shell.lock)"
is "locked over a capture: not available, and nothing else" 'OK {"available": false}' "$(call capture.status)"
check "locked: other methods are refused" '^ERR 3 PacketShark is not open$' "$(call capture.stop)"
check "unlock" '"locked": false' "$(call shell.unlock)"
C=$(status)
is "unlocked: PacketShark is in front again" packetshark "$(info 'd["current"]')"
is "and its capture is as the screen has it" ok "$(shape "$C")"
echo "     after the lock: $(of "$C" 'd["state"], d["seen"]')"

# ---- 5. hold and stop, tapped ------------------------------------------------------
if [ "$(of "$C" 'd["state"]')" = capturing ]; then
    check "HOLD, tapped" '"tapped": true' "$(call shell.tap '{"text": "HOLD"}')"
    wait_state hold 30; is "hold" 0 $?
    sleep 1.3
    H=$(status)
    is "hold: shape, no reason, frames counted as held" "ok none True" "$(shape "$H") $(of "$H" 'd["reason"]') $(of "$H" 'd["held"] > 0')"
    check "STOP, tapped" '"tapped": true' "$(call shell.tap '{"text": "STOP"}')"
    wait_state stopped 30; is "stopped" 0 $?
    sleep 0.6
    T=$(status)
    is "stopped: shape, by the user, no rate" "ok user 0 0" "$(shape "$T") $(of "$T" 'd["reason"]') $(of "$T" 'd["pps"]') $(of "$T" 'd["bps"]')"
    sleep 0.6
    is "stopped: the totals stand still" "$(of "$T" 'd["seen"], d["bytes"], d["stored"]')" "$(of "$(status)" 'd["seen"], d["bytes"], d["stored"]')"
else
    echo "FAIL the capture did not outlast the lock: hold and stop not tested"; failed=$((failed + 1))
fi
sleep 0.3
kill $LP 2>/dev/null; wait $LP 2>/dev/null
LP=
check "the listener heard the shell's own events through all of it" '^EVENT shell\.app ' "$(cat "$OUT/events")"
is "and no capture.* event (events are a later step)" 0 "$(grep -c '^EVENT capture\.' "$OUT/events")"
# A handler over its budget is logged as a warning by design and reported
# below; on a loaded host that is not a fault of the answer.
is "the shell logged no error about it" 0 "$(grep -cE ' ERROR .*(app method|capture\.)' "$OUT/shell.out" "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')"
is "no log line holds an answer" 0 "$(grep -c '"seen"' "$OUT/shell.out" "$POCKETOS_LOG_DIR"/*.log 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')"

# ---- 6. left: nothing to read, no helper ---------------------------------------------
call shell.home >/dev/null
sleep 0.8
if [ "$(info 'd["current"]')" != home ]; then
    # Back asks before a capture's packets go: answer as a person would.
    call shell.tap '{"text": "LEAVE"}' >/dev/null; sleep 0.8
fi
is "home again" home "$(info 'd["current"]')"
is "left: not available, and nothing else" 'OK {"available": false}' "$(call capture.status)"
sleep 0.6
is "left: no helper stays" 0 "$(helpers)"
echo "     handler calls $(info 'd["app_ipc"]["calls"]'), slow $(info 'd["app_ipc"]["slow"]'), worst $(info 'd["app_ipc"].get("worst_us")') us of $(info 'd["app_ipc"]["budget_us"]')"
is "no answer was over the limit" 0 "$(info 'd["app_ipc"]["too_large"]')"
stop_shell

# ---- 7. a helper that crashes ----------------------------------------------------------
start_shell synth:300:1:crash=300 "$HELPER" || { echo "FAIL the shell did not start"; exit 1; }
call shell.open '{"id": "packetshark"}' >/dev/null
sleep 0.5
call shell.tap '{"text": "START CAPTURE"}' >/dev/null
wait_state ended 80; is "a crashed helper: ended" 0 $?
E=$(status)
is "ended: shape, the reason, no rate" "ok helper_crashed 0" "$(shape "$E") $(of "$E" 'd["reason"]') $(of "$E" 'd["pps"]')"
check "NEW CAPTURE, tapped" '"tapped": true' "$(call shell.tap '{"text": "NEW CAPTURE"}')"
wait_state idle 30; is "setup again: idle" 0 $?
is "and its figures are gone" 0 "$(of "$(status)" 'd["seen"] + d["bytes"] + d["elapsed_ms"]')"
stop_shell

# ---- 8. no helper at all ------------------------------------------------------------------
start_shell synth:200:1 /nonexistent/pos-capture || { echo "FAIL the shell did not start"; exit 1; }
call shell.open '{"id": "packetshark"}' >/dev/null
sleep 0.5
check "a missing helper: START, tapped" '"tapped": true' "$(call shell.tap '{"text": "START CAPTURE"}')"
sleep 1.5
N=$(status)
is "a missing helper: still setup, idle, the shape, no helper" "ok idle 0" "$(shape "$N") $(of "$N" 'd["state"]') $(helpers)"
check "and the screen is back on setup, START there to tap again" '"tapped": true' "$(call shell.tap '{"text": "START CAPTURE"}')"
stop_shell

echo "capture_status_shell_test: $failed failure(s)"
exit $((failed > 0))
