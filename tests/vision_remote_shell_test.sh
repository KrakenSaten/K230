#!/bin/bash
# The vision.* methods the Vision app serves on shell.sock for a paired
# companion's gateway (apps/vision/vision_remote.h, docs/design/companion/
# VISION_PREVIEW.md section 5.2): in the real shell, on the fake camera and
# the real pos-vision, asked over the socket as gatewayd asks them.
#
# What is held: closed and locked (status says not available, everything else
# is refused, nothing opens the app); in front (the status's fields; FACE,
# RECOGNIZE and READ cannot be chosen; a setting only in its mode); and, when
# the helper can make a JPEG (a build with libjpeg: POCKETCAM_JPEG=1), the
# whole share: a real JPEG arrives through vision.pull, numbered upwards,
# inside the one larger answer the shell allows; a second viewer is refused;
# sharing ends with a private mode, with the viewer's word, and when the
# viewer goes quiet. With a helper that cannot (this host's default build)
# the share is refused and those checks are skipped, and said so.
#
# STOP is a tap on the unit's screen: tests/vision_remote_test.c holds the
# rule, the hardware validation the tap.
#
# Requires: SHELL_BIN (the CMake-built simulator), make's
# tools/vision/pos-vision (or VISION_HELPER), python3. Run from the
# repository root.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
HELPER=${VISION_HELPER:-$(pwd)/tools/vision/pos-vision}
[ -x "$HELPER" ] || { echo "FAIL build the helper first: make tools/vision/pos-vision"; exit 1; }
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
cleanup() {
    [ -n "$SP" ] && kill "$SP" 2>/dev/null
    rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR" "$OUT"
}
trap cleanup EXIT
check() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -4 | cut -c1-300; failed=$((failed + 1)); fi; }
is() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; failed=$((failed + 1)); fi; }
call() { $CALL "$SOCK" "$@" 2>&1; }
info() { call shell.info | sed 's/^OK //' | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
status() { call vision.status | sed 's/^OK //'; }
of() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($2)" "$1" 2>&1; }
wait_for() { # $1: an expression over the status (d), $2: tenths of a second
    for _ in $(seq 1 "$2"); do [ "$(of "$(status)" "$1")" = "True" ] && return 0; sleep 0.1; done
    return 1
}
VIEWER='"client": "0123456789abcdef", "name": "Test phone"'

rm -f "$SOCK"
POCKETOS_VISION_HELPER="$HELPER" POCKETOS_CAMERA_BACKEND=fake POCKETOS_CAMERA_FAKE=period=20 \
    POCKETOS_VISION_KPU_SCRIPT="box=2:800:100:120:120:80:3:0,box=0:700:400:40:60:160" \
    "$SHELL_BIN" >"$OUT/shell.out" 2>&1 &
SP=$!
for _ in $(seq 1 80); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "FAIL the shell did not start"; tail -5 "$OUT/shell.out"; exit 1; }

# ---- 1. not in front: behind the lock, and closed -------------------------------
is "locked: the status says not available, and nothing else" 'OK {"available": false}' "$(call vision.status)"
check "unlock, as a person at the unit" '"locked": false' "$(call shell.unlock)"
is "closed: the status says not available, and nothing else" 'OK {"available": false}' "$(call vision.status)"
for m in vision.share vision.pull vision.mode vision.set vision.reset vision.x; do
    check "closed: $m is refused" '^ERR 3 ' "$(call $m "{\"on\": true, $VIEWER, \"mode\": \"detect\"}")"
done
is "and none of them opened the app" home "$(info 'd["current"]')"
is "nor called it" 0 "$(info 'd["app_ipc"]["calls"]')"
is "the shell names the one larger answer it allows" "8192 90112" "$(info 'd["app_ipc"]["result_max"], d["app_ipc"]["vision_pull_max"]')"

# ---- 2. in front ----------------------------------------------------------------
check "open Vision" '"current": "vision"' "$(call shell.open '{"id": "vision"}')"
wait_for 'd.get("owner") == "vision"' 100 || { echo "FAIL Vision did not go live"; status | cut -c1-300; tail -5 "$OUT/shell.out"; exit 1; }
S=$(status)
is "the status has the contract's fields and no other" \
    "['available', 'labels', 'mode', 'model', 'modes', 'owner', 'pic', 'settings', 'share', 'stats']" "$(of "$S" 'sorted(d)')"
is "nothing is shared, and nobody is named" "off False" "$(of "$S" 'd["share"], "client" in d')"
is "the eight settings" 8 "$(of "$S" 'len(d["settings"])')"
check "it fits the ordinary answer" '^True$' "$(of "$S" 'len(json.dumps(d)) <= 8192')"
check "a pull without a share is refused" '^ERR 3 ' "$(call vision.pull '{"after_seq": 0}')"
for m in face recognize read; do
    check "$m cannot be chosen from outside" '^ERR 3 ' "$(call vision.mode "{\"mode\": \"$m\"}")"
done
check "a word that is no mode is refused" '^ERR 2 ' "$(call vision.mode '{"mode": "everything"}')"
check "TRACK is chosen" '^OK' "$(call vision.mode '{"mode": "track"}')"
wait_for 'd.get("mode") == "track"' 30
is "and is the mode" track "$(of "$(status)" 'd["mode"]')"
check "its line is set" '^OK' "$(call vision.set '{"key": "track.line", "value": "down"}')"
is "and stays set" down "$(of "$(status)" 'd["settings"]["track.line"]')"
check "another mode's setting is refused" '^ERR 3 ' "$(call vision.set '{"key": "traffic.range", "value": "far"}')"
check "a value it does not offer is refused" '^ERR 2 ' "$(call vision.set '{"key": "track.line", "value": "sideways"}')"
check "a setting that is not on the list is refused" '^ERR 2 ' "$(call vision.set '{"key": "detector", "value": "r0"}')"
check "the counters are reset" '^OK' "$(call vision.reset)"
check "a share without a viewer is refused" '^ERR 2 ' "$(call vision.share '{"on": true}')"
check "a method that is not served is unknown" '^ERR 1 ' "$(call vision.enrol)"

# ---- 3. the share ---------------------------------------------------------------
R=$(call vision.share "{\"on\": true, $VIEWER}")
if printf '%s' "$R" | grep -q '^ERR 3 '; then
    echo "SKIP the share itself: this pos-vision has no libjpeg ($R)"
    is "and nothing is shared" off "$(of "$(status)" 'd["share"]')"
else
    check "an authorised viewer is shown the picture at once" '"state": "live"' "$R"
    is "the status says so, and to whom" "live 0123456789abcdef" "$(of "$(status)" 'd["share"], d["client"]')"
    check "the same viewer asking again is still live" '"state": "live"' "$(call vision.share "{\"on\": true, $VIEWER}")"
    check "another viewer is refused as busy" '^ERR 5 ' "$(call vision.share '{"on": true, "client": "ffffffffffffffff", "name": "Other"}')"
    P=
    for _ in $(seq 1 50); do
        P=$(call vision.pull '{"after_seq": 0}' | sed 's/^OK //')
        [ "$(of "$P" '"jpeg" in d')" = "True" ] && break
        sleep 0.1
    done
    is "a picture arrives" True "$(of "$P" '"jpeg" in d')"
    PYOUT=$(python3 - "$P" "$OUT/first.jpg" <<'PY'
import base64, json, sys
d = json.loads(sys.argv[1])
raw = base64.b64decode(d["jpeg"], validate=True)
open(sys.argv[2], "wb").write(raw)
ok = (raw[:2] == b"\xff\xd8" and raw[-2:] == b"\xff\xd9" and len(raw) <= 65536 and 1 <= d["w"] <= 640 and 1 <= d["h"] <= 640
      and len(json.dumps(d)) <= 90112 and sorted(d) == ["age_ms", "boxes", "h", "jpeg", "seq", "w"]
      and len(d["boxes"]) <= 24 and all(len(b) == 9 for b in d["boxes"]))
# The frame header inside the JPEG says the same size as the answer does.
i = 2
size = None
while i + 9 < len(raw):
    if raw[i] != 0xFF:
        break
    marker = raw[i + 1]
    seg = (raw[i + 2] << 8) | raw[i + 3]
    if marker in (0xC0, 0xC1, 0xC2):
        size = ((raw[i + 7] << 8) | raw[i + 8], (raw[i + 5] << 8) | raw[i + 6])
        break
    i += 2 + seg
print("     first picture: seq %d, %d x %d, %d bytes of JPEG, %d boxes, answer %d bytes, age %d ms" %
      (d["seq"], d["w"], d["h"], len(raw), len(d["boxes"]), len(json.dumps(d)), d["age_ms"]))
print("ok   it is a JPEG within the bounds, of the size it says" if ok and size == (d["w"], d["h"]) else
      "FAIL the picture: ok=%s header=%s said=%s" % (ok, size, (d["w"], d["h"])))
PY
    )
    printf '%s
' "$PYOUT"
    printf '%s' "$PYOUT" | grep -q '^FAIL' && failed=$((failed + 1))
    [ -n "${KEEP_JPEG:-}" ] && cp "$OUT/first.jpg" "$KEEP_JPEG"
    SEQ1=$(of "$P" 'd["seq"]')
    check "the same number again brings no picture twice" '^False$' "$(of "$(call vision.pull "{\"after_seq\": $SEQ1}" | sed 's/^OK //')" '"jpeg" in d and d["seq"] == '"$SEQ1")"
    P2=
    for _ in $(seq 1 50); do
        P2=$(call vision.pull "{\"after_seq\": $SEQ1}" | sed 's/^OK //')
        [ "$(of "$P2" '"jpeg" in d')" = "True" ] && break
        sleep 0.1
    done
    check "the next picture has a higher number" '^True$' "$(of "$P2" 'd.get("seq", 0) > '"$SEQ1")"
    n=0
    last=$(of "$P2" 'd.get("seq", 0)')
    t0=$(date +%s%N)
    while [ $(( ($(date +%s%N) - t0) / 1000000 )) -lt 3000 ]; do
        Q=$(call vision.pull "{\"after_seq\": $last}" | sed 's/^OK //')
        if [ "$(of "$Q" '"jpeg" in d')" = "True" ]; then n=$((n + 1)); last=$(of "$Q" 'd["seq"]'); fi
    done
    echo "     $n pictures in 3 s of asking as fast as the socket allows"
    check "never more than five a second" '^True$' "$(python3 -c "print($n <= 16)")"
    check "and they do come" '^True$' "$(python3 -c "print($n >= 3)")"
    is "no answer was over its limit" 0 "$(info 'd["app_ipc"]["too_large"]')"
    echo "     handler calls $(info 'd["app_ipc"]["calls"]'), slow $(info 'd["app_ipc"]["slow"]'), worst $(info 'd["app_ipc"]["worst_us"]') us in $(info 'd["app_ipc"]["worst_method"]')"

    # What can be shown from outside the unit: the viewer's word ends it, and
    # a viewer that goes quiet loses it. (A private mode is chosen by a tap.)
    check "the viewer ends it" '"state": "off"' "$(call vision.share '{"on": false}')"
    is "the status says off" off "$(of "$(status)" 'd["share"]')"
    check "and a pull is refused again" '^ERR 3 ' "$(call vision.pull '{"after_seq": 0}')"
    check "shared again" '"state": "live"' "$(call vision.share "{\"on\": true, $VIEWER}")"
    sleep 9
    is "a viewer that asks for nothing for 8 s is no longer shown it" off "$(of "$(status)" 'd["share"]')"
fi

# ---- 4. leaving ------------------------------------------------------------------
call shell.home >/dev/null
is "Vision left: not available" 'OK {"available": false}' "$(call vision.status)"
check "and a share is refused" '^ERR 3 ' "$(call vision.share "{\"on\": true, $VIEWER}")"
sleep 1
is "no helper is left behind" 0 "$(pgrep -f "$HELPER session" | wc -l | tr -d ' ')"

if [ $failed -eq 0 ]; then echo "vision_remote_shell_test: all passed"; else echo "vision_remote_shell_test: $failed failed"; fi
exit $failed
