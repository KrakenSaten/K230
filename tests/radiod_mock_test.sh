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
check "info region" '"region":[[:space:]]*"EU868"' "$out"

out=$("$POS" radio status)
check "status state rx" '"state":[[:space:]]*"rx"' "$out"
check "status default frequency" '"frequency_mhz":[[:space:]]*869.525' "$out"

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

# SF7 BW125 CR4/5, 10 bytes: 41.216 ms (tests/airtime_test.c)
out=$("$POS" radio send 00112233445566778899)
check "send airtime" '"airtime_ms":[[:space:]]*41.216' "$out"
out=$("$POS" radio send zz 2>&1)
check "send rejects bad hex" 'code 2' "$out"

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

kill $RADIOD_PID
wait $RADIOD_PID 2>/dev/null
[ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && { echo "FAIL socket not removed on exit"; failed=$((failed + 1)); } || echo "ok   socket removed on exit"
rm -rf "$POCKETOS_RUNTIME_DIR"
echo "radiod_mock_test: $failed failure(s)"
exit $((failed > 0))
