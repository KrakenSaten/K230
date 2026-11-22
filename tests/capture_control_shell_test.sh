#!/bin/bash
# capture.hold, capture.stop and the events capture.state and capture.stats,
# served by PacketShark on shell.sock (apps/packetshark/psk_app.c,
# psk_remote.h; docs/api/shell.md): in the real shell, on the simulator's
# synthetic traffic, asked over the socket.
#
# What is held: HOLD, RESUME and STOP by request do what the buttons do, and
# the buttons still work between them; they are refused while the app is
# closed, behind the lock, or in a state with no running capture, and with
# bad parameters; nothing but a tap starts a capture; the events come in the
# order things happened, figures never before their state, only to a client
# that subscribed; every event has the contract's keys and nothing that could
# be an address; leaving says so once and leaves no helper.
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
LQ=
cleanup() {
    [ -n "$LP" ] && kill "$LP" 2>/dev/null
    [ -n "$LQ" ] && kill "$LQ" 2>/dev/null
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
start_shell() { # $1: the capture source
    rm -f "$SOCK"
    POCKETOS_CAPTURE_SOURCE="$1" POCKETOS_CAPTURE_HELPER="$HELPER" "$SHELL_BIN" >"$OUT/shell.out" 2>&1 &
    SP=$!
    for _ in $(seq 1 80); do [ -S "$SOCK" ] && return 0; kill -0 $SP 2>/dev/null || return 1; sleep 0.1; done
    return 1
}
stop_shell() { [ -n "$SP" ] && kill $SP 2>/dev/null; wait $SP 2>/dev/null; SP=; }
wait_state() { # $1: the state, $2: tenths of a second
    for _ in $(seq 1 "$2"); do [ "$(state)" = "$1" ] && return 0; sleep 0.1; done
    return 1
}
helpers() { pgrep -f "$HELPER session" | wc -l | tr -d ' '; }
listen() { # $1: the method the listener opens with, $2: where its lines go
    rm -f "$OUT/ready"
    $CALL "$SOCK" --listen 90 "$1" "$OUT/ready" >"$2" 2>&1 &
    L=$!
    for _ in $(seq 1 40); do [ -e "$OUT/ready" ] && break; sleep 0.1; done
}
# What a file of "EVENT name json" lines holds, one question at a time.
events() { # $1: the file, $2: the question
    python3 - "$1" "$2" <<'PY'
import json, re, sys
TOP = ["available", "state", "reason", "iface", "keep", "elapsed_ms", "limit_s", "seen", "stored", "held",
       "kernel_drops", "iface_drops", "shed", "bytes", "pps", "bps", "fill_permille", "classes", "findings",
       "diag", "requested_by_companion", "request"]
STATE = ["state", "reason", "iface", "keep", "limit_s"]
STATES = ["idle", "capturing", "hold", "saving", "stopped", "ended"]
REASONS = ["none", "user", "limit", "link", "end", "error", "helper_crashed", "helper_hung", "left"]
ev = []
for line in open(sys.argv[1]):
    m = re.match(r"EVENT (capture\.[a-z_]+) (.*)$", line.rstrip("\n"))
    if m:
        ev.append((m.group(1), json.loads(m.group(2)), m.group(2)))
q = sys.argv[2]
def word(d): return d["state"] + ("" if d["reason"] == "none" else "/" + d["reason"])
if q == "states":
    print(",".join(word(d) for n, d, _ in ev if n == "capture.state"))
elif q == "names":
    print(",".join(sorted(set(n for n, _, _ in ev))))
elif q == "count":
    print(len(ev))
elif q == "stats":
    print(sum(1 for n, _, _ in ev if n == "capture.stats"))
elif q == "last":
    print(ev[-1][0] + " " + word(ev[-1][1]) if ev else "none")
elif q == "order":
    # Figures never come before their state, and carry the state last said.
    last = None
    bad = []
    for n, d, _ in ev:
        if n == "capture.state":
            last = d["state"]
        elif d.get("state") != last:
            bad.append("stats in %s after state %s" % (d.get("state"), last))
    print("ok" if not bad else bad[0])
elif q == "after":
    # After each state that has figures, figures follow before the next state.
    bad = []
    for i, (n, d, _) in enumerate(ev):
        if n == "capture.state" and d["state"] not in ("idle",) and d["reason"] != "left":
            nxt = ev[i + 1][0] if i + 1 < len(ev) else None
            if nxt != "capture.stats":
                bad.append("no figures after " + word(d))
    print("ok" if not bad else bad[0])
elif q == "stopped":
    print(sum(1 for n, d, _ in ev if n == "capture.stats" and d["state"] == "stopped"))
elif q == "final":
    # The figures after the stop are the capture's totals, with no rate.
    out = "no stopped figures"
    for n, d, _ in ev:
        if n == "capture.stats" and d["state"] == "stopped":
            out = "ok" if d["seen"] > 0 and d["pps"] == 0 and d["bps"] == 0 and d["reason"] == "user" else "bad totals"
    print(out)
elif q == "shape":
    try:
        for n, d, text in ev:
            assert not re.search(r"\d+\.\d+\.\d+|[0-9a-f]{2}:[0-9a-f]{2}:", text), "an address"
            assert d["state"] in STATES and d["reason"] in REASONS and d["keep"] in ("headers", "full"), "a word"
            assert re.fullmatch(r"((eth|wlan|usb|synth)[0-9]{1,2})?", d["iface"]), "iface"
            if n == "capture.state":
                assert sorted(d) == sorted(STATE), "state keys %s" % sorted(d)
                assert type(d["limit_s"]) is int
            else:
                assert n == "capture.stats", n
                assert sorted(d) == sorted(TOP), "stats keys %s" % sorted(d)
                assert d["available"] is True and d["requested_by_companion"] is False
                for k, v in d.items():
                    if k in ("classes", "diag"):
                        assert all(type(x) is int and x >= 0 for x in v.values()), k
                    elif k == "findings":
                        assert len(v) == 6 and all(type(x) is int and x >= 0 for x in v), k
                    elif k not in ("available", "requested_by_companion", "request", "state", "reason", "iface", "keep"):
                        assert type(v) is int and v >= 0, k
        print("ok")
    except Exception as e:
        print("bad:", e)
PY
}

# ---- 1. not in front: nothing is done ----------------------------------------------------
start_shell synth:200:1 || { echo "FAIL the shell did not start"; tail -5 "$OUT/shell.out"; exit 1; }
is "the shell starts locked" True "$(info 'd["lock"]["locked"]')"
check "locked: capture.hold is refused" '^ERR 3 PacketShark is not open$' "$(call capture.hold '{"on": true}')"
check "locked: capture.stop is refused" '^ERR 3 PacketShark is not open$' "$(call capture.stop)"
check "unlock, as a person at the unit" '"locked": false' "$(call shell.unlock)"
check "closed: capture.hold is refused" '^ERR 3 PacketShark is not open$' "$(call capture.hold '{"on": false}')"
check "closed: capture.stop is refused" '^ERR 3 PacketShark is not open$' "$(call capture.stop)"
is "and neither opened the app nor called it" "home 0" "$(info 'd["current"]') $(info 'd["app_ipc"]["calls"]')"

# Two clients from before the app opens: one subscribes, one does not.
listen shell.subscribe "$OUT/events"; LP=$L
listen shell.info "$OUT/unsubscribed"; LQ=$L
T0=$(date +%s)

# ---- 2. in front, with nothing capturing ---------------------------------------------------
check "open PacketShark" '"current": "packetshark"' "$(call shell.open '{"id": "packetshark"}')"
sleep 0.5
check "setup: capture.hold has nothing to hold" '^ERR 3 nothing is capturing$' "$(call capture.hold '{"on": true}')"
check "setup: capture.stop has nothing to stop" '^ERR 3 nothing is capturing$' "$(call capture.stop)"
is "and neither started a capture or a helper" "idle 0" "$(state) $(helpers)"

# ---- 3. hold and resume --------------------------------------------------------------------
check "START, tapped on the panel: the only way a capture starts" '"tapped": true' "$(call shell.tap '{"text": "START CAPTURE"}')"
wait_state capturing 50; is "capturing" 0 $?
sleep 2.2
for p in '' '{}' '{"on": "true"}' '{"on": 1}' '{"on": null}' '[true]' '{"hold": true}'; do
    check "capture.hold with ${p:-no parameters} is refused" '^ERR 2 on must be true or false$' "$(if [ -n "$p" ]; then call capture.hold "$p"; else call capture.hold; fi)"
done
is "and held nothing" capturing "$(state)"
is "capture.hold on: taken, answered with the state it was asked in" 'OK {"changing": true, "state": "capturing"}' "$(call capture.hold '{"on": true}')"
wait_state hold 30; is "hold" 0 $?
sleep 1.2
H=$(status)
is "held: frames pass uncounted into the list, as after a tap on HOLD" True "$(of "$H" 'd["held"] > 0 and d["reason"] == "none"')"
is "capture.hold on again: already held, nothing sent" 'OK {"changing": false, "state": "hold"}' "$(call capture.hold '{"on": true}')"
check "the screen offers RESUME, and a tap on it resumes" '"tapped": true' "$(call shell.tap '{"text": "RESUME"}')"
wait_state capturing 30; is "capturing again, by the button" 0 $?
sleep 0.5 # each state stands long enough to be raised: changes within 250 ms are raised as one
check "the screen offers HOLD again, and a tap on it holds" '"tapped": true' "$(call shell.tap '{"text": "HOLD"}')"
wait_state hold 30; is "hold, by the button" 0 $?
sleep 0.5
is "capture.hold off: taken" 'OK {"changing": true, "state": "hold"}' "$(call capture.hold '{"on": false}')"
wait_state capturing 30; is "capturing again, by request" 0 $?
is "capture.hold off again: already capturing, nothing sent" 'OK {"changing": false, "state": "capturing"}' "$(call capture.hold '{"on": false}')"
is "one helper through all of it" 1 "$(helpers)"

# ---- 4. the lock over a capture --------------------------------------------------------------
B=$(status)
check "lock the unit" '"locked": true' "$(call shell.lock)"
check "locked over a capture: capture.stop is refused" '^ERR 3 PacketShark is not open$' "$(call capture.stop)"
check "locked over a capture: capture.hold is refused" '^ERR 3 PacketShark is not open$' "$(call capture.hold '{"on": true}')"
sleep 1.3
check "unlock" '"locked": false' "$(call shell.unlock)"
A=$(status)
is "and the capture ran on untouched" "capturing True" "$(of "$A" 'd["state"]') $(python3 -c "import json,sys; print(json.loads(sys.argv[2])['seen'] > json.loads(sys.argv[1])['seen'])" "$B" "$A")"
sleep 1.3

# ---- 5. stop ---------------------------------------------------------------------------------
for m in capture.discard capture.save capture.new capture.start capture.resume; do
    check "$m is not served" '^ERR 1 unknown method$' "$(call $m '{"on": true, "iface": "synth0", "limit_s": 300}')"
done
is "and the capture runs on" capturing "$(state)"
is "capture.stop: taken" 'OK {"changing": true, "state": "capturing"}' "$(call capture.stop '{"anything": 1}')"
wait_state stopped 30; is "stopped" 0 $?
sleep 0.7
T=$(status)
is "stopped as by a tap on STOP: by the user, no rate, its packets kept in its helper" "user 0 1" "$(of "$T" 'd["reason"]') $(of "$T" 'd["pps"]') $(helpers)"
check "capture.stop on a stopped capture" '^ERR 3 nothing is capturing$' "$(call capture.stop)"
check "capture.hold on a stopped capture" '^ERR 3 nothing is capturing$' "$(call capture.hold '{"on": true}')"
check "capture.hold off on a stopped capture" '^ERR 3 nothing is capturing$' "$(call capture.hold '{"on": false}')"
check "the screen offers SAVE and DISCARD, as after a tap on STOP" '"tapped": true' "$(call shell.tap '{"text": "DISCARD"}')"
sleep 0.4
check "DISCARD asks first, and STAY keeps the capture" '"tapped": true' "$(call shell.tap '{"text": "STAY"}')"
sleep 0.4
is "still stopped" stopped "$(state)"
sleep 1.6

# ---- 6. leaving --------------------------------------------------------------------------------
call shell.home >/dev/null
sleep 0.8
if [ "$(info 'd["current"]')" != home ]; then
    call shell.tap '{"text": "LEAVE"}' >/dev/null; sleep 0.8
fi
is "home again" home "$(info 'd["current"]')"
sleep 0.8
is "left: no helper stays" 0 "$(helpers)"
check "left: capture.stop is refused" '^ERR 3 PacketShark is not open$' "$(call capture.stop)"
sleep 1.5
T1=$(date +%s)
kill $LP $LQ 2>/dev/null; wait $LP $LQ 2>/dev/null
LP=
LQ=

# ---- 7. the events ---------------------------------------------------------------------------
E="$OUT/events"
is "the states, in the order they happened, each said once" \
   "idle,capturing,hold,capturing,hold,capturing,stopped/user,ended/left" "$(events "$E" states)"
is "the app raised its two events and no other" "capture.state,capture.stats" "$(events "$E" names)"
is "figures never came before their state" ok "$(events "$E" order)"
is "each state of a capture was followed by its figures" ok "$(events "$E" after)"
is "the stopped capture's totals went out, with no rate" ok "$(events "$E" final)"
is "leaving was the last word" "capture.state ended/left" "$(events "$E" last)"
is "every event has the contract's keys, whole numbers, checked words, and nothing like an address" ok "$(events "$E" shape)"
N=$(events "$E" stats)
SPAN=$((T1 - T0))
echo "     $N capture.stats and $(events "$E" count) events in all over ${SPAN} s"
is "figures came about once a second while it ran" 1 "$([ "$N" -ge 5 ] && [ "$N" -le $((SPAN + 8)) ] && echo 1 || echo 0)"
is "and once for the stopped capture, in the three seconds and more it then stood" 1 "$(events "$E" stopped)"
check "the subscribed client also heard the shell's own events" '^EVENT shell\.app ' "$(cat "$E")"
is "a client that did not subscribe heard nothing" 0 "$(grep -c '^EVENT' "$OUT/unsubscribed")"
echo "     raised $(info 'd["app_ipc"]["events"]'), dropped $(info 'd["app_ipc"]["events_dropped"]'), handler calls $(info 'd["app_ipc"]["calls"]'), slow $(info 'd["app_ipc"]["slow"]'), worst $(info 'd["app_ipc"].get("worst_us")') us"
is "under the lock nothing was raised, and nothing made only to be dropped" 0 "$(info 'd["app_ipc"]["events_dropped"]')"
is "no log line holds an answer or an event" 0 "$(grep -c '"seen"' "$OUT/shell.out" "$POCKETOS_LOG_DIR"/*.log 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')"
is "the shell logged no error about it" 0 "$(grep -cE ' ERROR .*(app method|capture\.)' "$OUT/shell.out" "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')"
stop_shell

# ---- 8. a helper that crashes: said as it is, and nothing to stop --------------------------------
start_shell synth:300:1:crash=300 || { echo "FAIL the shell did not start"; exit 1; }
call shell.unlock >/dev/null
listen shell.subscribe "$OUT/events2"; LP=$L
call shell.open '{"id": "packetshark"}' >/dev/null
sleep 0.5
check "START, tapped" '"tapped": true' "$(call shell.tap '{"text": "START CAPTURE"}')"
for _ in $(seq 1 80); do [ "$(state)" = ended ] && break; sleep 0.1; done
is "the helper crashed: ended" ended "$(state)"
check "capture.stop on an ended capture" '^ERR 3 nothing is capturing$' "$(call capture.stop)"
check "capture.hold on an ended capture" '^ERR 3 nothing is capturing$' "$(call capture.hold '{"on": true}')"
sleep 1.0
check "NEW CAPTURE, tapped" '"tapped": true' "$(call shell.tap '{"text": "NEW CAPTURE"}')"
sleep 1.0
kill $LP 2>/dev/null; wait $LP 2>/dev/null
LP=
S2=$(events "$OUT/events2" states)
check "the crash is said as a state, then setup again" 'ended/helper_crashed,idle$' "$S2"
check "from setup" '^idle,' "$S2"
is "its events are in order and of the contract's shape" "ok ok" "$(events "$OUT/events2" order) $(events "$OUT/events2" shape)"
stop_shell
is "no helper is left" 0 "$(helpers)"

echo "capture_control_shell_test: $failed failure(s)"
exit $((failed > 0))
