#!/bin/bash
# End-to-end test: radiod (mock backend) and pos radio over pocketipc.
# Run from the repository root after `make all`.
set -u

RADIOD=${RADIOD:-services/radiod/radiod}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR
POCKETOS_RUNTIME_DIR=$(mktemp -d)
failed=0

check() { # check <name> <expected-substring> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -20
        failed=$((failed + 1))
    fi
}

"$RADIOD" --backend mock --region EU868 > "$POCKETOS_RUNTIME_DIR/radiod.log" 2>&1 &
RADIOD_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && break; sleep 0.1; done
[ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] || { echo "FAIL radiod did not start"; cat "$POCKETOS_RUNTIME_DIR/radiod.log"; exit 1; }

out=$("$POS" radio info)
check "info chip mock" '"chip":[[:space:]]*"mock"' "$out"
check "info api_version" '"api_version":[[:space:]]*0' "$out"
# A running service must be able to say which build it is; a bench report that
# cannot name the image is not evidence. The expected value is derived the same
# way the build derives it, so this also proves the plumbing rather than just
# the presence of a field.
EXPECTED_VERSION=$(cat VERSION)
EXPECTED_BUILD=$(cat BUILD_ID 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
check "info reports the version" "\"version\":[[:space:]]*\"${EXPECTED_VERSION}\"" "$out"
check "info reports the build" "\"build\":[[:space:]]*\"${EXPECTED_BUILD}\"" "$out"
check "info region" '"region":[[:space:]]*"EU868"' "$out"

out=$("$POS" radio status)
check "status state rx" '"state":[[:space:]]*"rx"' "$out"
check "status default frequency" '"frequency_mhz":[[:space:]]*869.525' "$out"
check "status default tx power is 2 dBm (bench-safe)" '"tx_power_dbm":[[:space:]]*2,' "$out"
check "status default sync word private" '"sync_word":[[:space:]]*18' "$out"

# TX power bounds through radio.configure: chip range (code 2) and region cap (code 3)
out=$("$POS" radio configure tx_power_dbm=-10 2>&1)
check "chip range rejects -10 dBm" 'code 2' "$out"
out=$("$POS" radio configure tx_power_dbm=15 2>&1)
check "region cap rejects 15 dBm" 'code 3' "$out"
out=$("$POS" radio status)
check "rejected power left the default" '"tx_power_dbm":[[:space:]]*2,' "$out"

out=$("$POS" radio configure frequency_mhz=868.1 spreading_factor=7 tx_power_dbm=10 crc=true)
check "configure applied sf7" '"spreading_factor":[[:space:]]*7' "$out"
check "configure applied freq" '"frequency_mhz":[[:space:]]*868.1' "$out"

out=$("$POS" radio configure frequency_mhz=915 2>&1)
check "region guard rejects 915 MHz" 'code 3' "$out"
out=$("$POS" radio configure tx_power_dbm=20 2>&1)
check "region guard rejects 20 dBm" 'code 3' "$out"
out=$("$POS" radio configure spreading_factor=13 2>&1)
check "range check rejects SF13" 'code 2' "$out"
out=$("$POS" radio configure bandwidth_khz=100 2>&1)
check "range check rejects BW100" 'code 2' "$out"
out=$("$POS" radio status)
check "rejected configure did not change profile" '"spreading_factor":[[:space:]]*7' "$out"

# Finding 1: integer fields reject fractional values instead of truncating
for kv in spreading_factor=7.9 coding_rate=5.5 tx_power_dbm=13.9 sync_word=18.5 preamble_length=8.5; do
    out=$("$POS" radio configure "$kv" 2>&1)
    check "fractional $kv rejected" 'code 2' "$out"
done
out=$("$POS" radio status)
check "fractional values did not change profile" '"tx_power_dbm":[[:space:]]*10' "$out"
out=$("$POS" radio configure spreading_factor=8.0 tx_power_dbm=12)
check "integral-valued numbers still accepted" '"spreading_factor":[[:space:]]*8' "$out"
"$POS" radio configure spreading_factor=7 tx_power_dbm=10 >/dev/null

# Finding 3: timeout_ms is not part of the contract and is refused explicitly.
# It was documented once and never implemented. The refusal used to explain
# that radio.send is synchronous; now that radio.send_async exists it points
# there instead, which is the answer the caller actually wanted - a deadline
# on a transmit whose completion is the point would report failure for a
# packet that went out.
out=$(python3 - "$POCKETOS_RUNTIME_DIR/radiod.sock" <<'PY'
import socket, sys, json, struct
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
body = json.dumps({"id": 1, "method": "radio.send", "params": {"payload_hex": "0102", "timeout_ms": 500}}).encode()
s.sendall(struct.pack(">I", len(body)) + body)
hdr = s.recv(4); n = struct.unpack(">I", hdr)[0]; print(s.recv(n).decode())
PY
)
check "timeout_ms refused with code 2" '"code":2' "$out"
check "timeout_ms refusal points at the asynchronous path" 'send_async' "$out"

# SF7 BW125 CR4/5, 10 bytes: 41.216 ms (tests/airtime_test.c)
out=$("$POS" radio send 00112233445566778899)
check "send airtime" '"airtime_ms":[[:space:]]*41.216' "$out"
out=$("$POS" radio send zz 2>&1)
check "send rejects bad hex" 'code 2' "$out"

# Cold review F17: each pair used to go through strtoul(), which skips leading
# whitespace and accepts a sign. So "-1" was transmitted as ff, "+a" as 0a and
# " a" as 0a - text that is not hexadecimal at all became bytes on the air,
# silently and differently from what was asked for. Exactly two hex digits per
# byte now, nothing else.
for bad in -1 +a "0 " " a" "0x" "g0" "0g" 0 000 --; do
    out=$("$POS" radio send "$bad" 2>&1)
    check "send rejects payload_hex '$bad'" 'code 2' "$out"
done

out=$("$POS" radio stats)
check "stats tx_packets" '"tx_packets":[[:space:]]*1' "$out"
check "stats last hour airtime" '"tx_airtime_last_hour_ms":[[:space:]]*41.216' "$out"

out=$("$POS" radio cad)
check "cad idle" '"activity":[[:space:]]*false' "$out"
out=$("$POS" radio rssi)
check "rssi idle" '"rssi_dbm":[[:space:]]*-115' "$out"

# Events: listener subscribes, another client injects a packet and sends.
"$POS" radio listen 3 > "$POCKETOS_RUNTIME_DIR/events.txt" 2>&1 &
LISTEN_PID=$!
sleep 0.5
out=$("$POS" radio inject 48656c6c6f -87.5 9.25)
check "inject accepted" '{' "$out"
out=$("$POS" radio send 0102)
wait $LISTEN_PID
events=$(cat "$POCKETOS_RUNTIME_DIR/events.txt")
check "event radio.rx payload" '"event":"radio.rx".*"payload_hex":"48656c6c6f"' "$events"
check "event radio.rx rssi" '"rssi_dbm":-87.5' "$events"
check "event radio.tx_done" '"event":"radio.tx_done"' "$events"
check "event radio.state tx" '"event":"radio.state","data":{"state":"tx"}' "$events"

out=$("$POS" radio stats)
check "stats rx_packets" '"rx_packets":[[:space:]]*1' "$out"
check "stats last rssi" '"last_rssi_dbm":[[:space:]]*-87.5' "$out"

# Finding 2: state follows the backend's real receive state, with recovery
out=$("$POS" radio mock rx_failing=1 2>&1); check "mock rx_failing set" '{' "$out"
out=$("$POS" radio status); check "state error when RX cannot be entered" '"state":[[:space:]]*"error"' "$out"
out=$("$POS" radio send 0102); check "send still transmits in error state" '"airtime_ms"' "$out"
out=$("$POS" radio status); check "state stays error after send" '"state":[[:space:]]*"error"' "$out"
"$POS" radio mock rx_failing=0 >/dev/null
sleep 1.5
out=$("$POS" radio status); check "state recovers to rx within a second" '"state":[[:space:]]*"rx"' "$out"
out=$("$POS" radio mock bogus=1 2>&1); check "unknown mock key rejected" 'code 2' "$out"

# Cold review F2: receive mode lost while a good packet was being handed over.
#
# The checks above reach the backend through mock.set, and that handler
# reconciles the daemon's state itself - so they passed whether or not the
# receive path did. The SX1262 re-enters receive inside sx_receive(), after
# readData(), and reports the result through is_receiving() alone: no control
# call arrives, the packet is returned as good, and the drain used to end
# without ever asking. The daemon then reported "rx" for the rest of the
# session while the transceiver was deaf, and recover_rx never ran because it
# only runs in "error".
"$POS" radio mock rx_fails_after_receive=1 >/dev/null
out=$("$POS" radio status)
check "arming the fault does not itself change the state" '"state":[[:space:]]*"rx"' "$out"
# The section above already logged recovery attempts, so only a new one counts.
recover_before=$(grep -c 'receive recovery failed' "$POCKETOS_RUNTIME_DIR/radiod.log")
out=$("$POS" radio inject 5445); check "a packet is injected" '{' "$out"
sleep 0.5
out=$("$POS" radio stats)
check "the packet was still received" '"rx_packets":[[:space:]]*2' "$out"
out=$("$POS" radio status)
check "and the daemon no longer reports rx" '"state":[[:space:]]*"error"' "$out"
sleep 1.5
out=$("$POS" radio status)
check "it stays error while the backend is deaf" '"state":[[:space:]]*"error"' "$out"
recover_after=$(grep -c 'receive recovery failed' "$POCKETOS_RUNTIME_DIR/radiod.log")
check "and the recovery loop is running against it" "yes" \
      "$([ "$recover_after" -gt "$recover_before" ] && echo yes || echo no)"
"$POS" radio mock rx_fails_after_receive=0 >/dev/null
sleep 0.3
out=$("$POS" radio status)
check "a backend that can receive again is reported as rx" '"state":[[:space:]]*"rx"' "$out"

# Cold review F3: a configure that changed the radio and then failed.
#
# Configuring a transceiver is several operations in order. On the SX1262
# begin() puts the whole radio configuration on the air, and setCRC() and
# startReceive() come after it - so a configure can fail with the chip already
# moved. Reporting the old profile afterwards puts the wrong frequency and
# spreading factor into every status and every bench report that follows.
#
# The mock models the same three stages so the failure can be injected between
# them. Stage 1 fails before the radio is touched, stages 2 and 3 after.
out=$("$POS" radio configure frequency_mhz=868.5 spreading_factor=9)
check "a known-good profile is applied" '"spreading_factor":[[:space:]]*9' "$out"

# Stage 1 with a transient fault: begin() failed without touching the radio,
# and the retry of the previous profile succeeds, so nothing is uncertain.
"$POS" radio mock configure_fail_once=1 >/dev/null
"$POS" radio mock configure_fail_stage=1 >/dev/null
out=$("$POS" radio configure spreading_factor=11 2>&1)
check "a configure that fails before touching the radio is refused" 'code' "$out"
out=$("$POS" radio status)
check "and the profile is the one that was working" '"spreading_factor":[[:space:]]*9' "$out"
check "with no uncertainty claimed" "0"       "$(printf '%s' "$out" | grep -c profile_uncertain)"

# Stage 2, also transient: the radio moved and then the configure failed, so
# the previous profile is asked for again - and this time the radio takes it.
"$POS" radio mock configure_fail_stage=2 >/dev/null
out=$("$POS" radio configure spreading_factor=12 2>&1)
check "a configure that fails after the radio moved is refused too" 'code' "$out"
out=$("$POS" radio status)
check "the previous profile was restored, not the requested one"       '"spreading_factor":[[:space:]]*9' "$out"
check "and the restore left nothing uncertain" "0"       "$(printf '%s' "$out" | grep -c profile_uncertain)"
check "the daemon logged the restore" 'the previous profile was restored'       "$(cat "$POCKETOS_RUNTIME_DIR/radiod.log")"

# Stage 3, and this time the fault stays: the radio moved, receive mode could
# not be entered, and the rollback fails the same way. Neither profile is on
# the chip and nothing can say what is.
"$POS" radio mock configure_fail_once=0 >/dev/null
"$POS" radio mock configure_fail_stage=3 >/dev/null
out=$("$POS" radio configure spreading_factor=10 2>&1)
check "a configure whose rollback also fails is refused" 'code' "$out"
out=$("$POS" radio status)
check "the daemon says the settings are unknown" '"profile_uncertain":[[:space:]]*true' "$out"
check "and does not report rx while they are" '"state":[[:space:]]*"error"' "$out"
check "the daemon logged that it could not restore them"       'could not be restored' "$(cat "$POCKETOS_RUNTIME_DIR/radiod.log")"

# A configure that succeeds is what clears it.
"$POS" radio mock configure_fail_stage=0 >/dev/null
out=$("$POS" radio configure spreading_factor=9 2>&1)
check "a configure that succeeds is accepted again" '"spreading_factor":[[:space:]]*9' "$out"
out=$("$POS" radio status)
check "and the uncertainty is gone" "0"       "$(printf '%s' "$out" | grep -c profile_uncertain)"
check "and the radio is receiving again" '"state":[[:space:]]*"rx"' "$out"

# Protocol robustness: garbage frame must not crash radiod.
python3 - "$POCKETOS_RUNTIME_DIR/radiod.sock" <<'PY' || true
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(b"\x00\x00\x00\x03abc")
s.settimeout(1)
try:
    s.recv(64)
except Exception:
    pass
s.close()
PY
out=$("$POS" radio info 2>&1)
check "radiod survives invalid JSON frame" '"chip"' "$out"

# F17, the other half: what was always valid still is. Placed here rather than
# beside the rejections because these do transmit, and the packet counts above
# are asserted exactly.
out=$("$POS" radio send AbCdEf)
check "upper and lower case hex is still accepted" '"bytes":[[:space:]]*3' "$out"
out=$("$POS" radio send ff00)
check "and so are the extremes of a byte" '"bytes":[[:space:]]*2' "$out"

kill $RADIOD_PID
wait $RADIOD_PID 2>/dev/null
[ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && { echo "FAIL socket not removed on exit"; failed=$((failed + 1)); } || echo "ok   socket removed on exit"

# Start-up defaults and restart behaviour (a restart must never raise the
# power back to the region maximum on its own).
"$RADIOD" > "$POCKETOS_RUNTIME_DIR/radiod_defaults.log" 2>&1 &   # no arguments at all
RADIOD_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && break; sleep 0.1; done
out=$("$POS" radio info)
check "default backend is mock" '"backend":[[:space:]]*"mock"' "$out"
check "default region is EU868" '"region":[[:space:]]*"EU868"' "$out"
out=$("$POS" radio status)
check "default tx power 2 dBm without arguments" '"tx_power_dbm":[[:space:]]*2,' "$out"
"$POS" radio configure tx_power_dbm=10 >/dev/null
out=$("$POS" radio status); check "operator raised power to 10" '"tx_power_dbm":[[:space:]]*10' "$out"
kill $RADIOD_PID; wait $RADIOD_PID 2>/dev/null
"$RADIOD" --tx-power-dbm 5 > "$POCKETOS_RUNTIME_DIR/radiod_restart.log" 2>&1 &
RADIOD_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && break; sleep 0.1; done
out=$("$POS" radio status)
check "restart returns to the configured start power, not 10 or 14" '"tx_power_dbm":[[:space:]]*5,' "$out"
kill $RADIOD_PID; wait $RADIOD_PID 2>/dev/null

# Invalid start-up configuration is refused before the radio is touched
"$RADIOD" --tx-power-dbm 15 > "$POCKETOS_RUNTIME_DIR/radiod_bad1.log" 2>&1; rc=$?
check "--tx-power-dbm 15 above the EU868 cap exits 2" '1' "$([ $rc -eq 2 ] && echo 1 || echo 0)"
check "the refusal names the region" 'region EU868' "$(cat "$POCKETOS_RUNTIME_DIR/radiod_bad1.log")"
"$RADIOD" --tx-power-dbm abc > "$POCKETOS_RUNTIME_DIR/radiod_bad2.log" 2>&1; rc=$?
check "--tx-power-dbm abc exits 2" '1' "$([ $rc -eq 2 ] && echo 1 || echo 0)"
"$RADIOD" --tx-power-dbm -10 > "$POCKETOS_RUNTIME_DIR/radiod_bad3.log" 2>&1; rc=$?
check "--tx-power-dbm -10 below the chip range exits 2" '1' "$([ $rc -eq 2 ] && echo 1 || echo 0)"
"$RADIOD" --backend bogus > "$POCKETOS_RUNTIME_DIR/radiod_bad4.log" 2>&1; rc=$?
check "--backend bogus exits 2" '1' "$([ $rc -eq 2 ] && echo 1 || echo 0)"
"$RADIOD" --region MARS > "$POCKETOS_RUNTIME_DIR/radiod_bad5.log" 2>&1; rc=$?
check "--region MARS exits 2" '1' "$([ $rc -eq 2 ] && echo 1 || echo 0)"
check "no socket left by refused starts" '1' "$([ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && echo 0 || echo 1)"
rm -rf "$POCKETOS_RUNTIME_DIR"
echo "radiod_mock_test: $failed failure(s)"
exit $((failed > 0))
