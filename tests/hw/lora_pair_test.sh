#!/bin/bash
# Two-unit LoRa link test over SSH. Unit A listens, unit B transmits, and the
# script checks that A reported the packet with RSSI and SNR.
#
# Usage: lora_pair_test.sh <host-A> <host-B> [payload-hex] [profile key=value ...]
# Both units need PocketOS radiod (sx1262 backend) and pos installed, and SSH
# as root with key authentication. Uses the EU868 default profile unless
# key=value overrides are given (applied to both units).
set -u
A=${1:?host A}
B=${2:?host B}
PAYLOAD=${3:-506f636b65744f53}   # "PocketOS"
shift 3 2>/dev/null || shift $#
PROFILE=("$@")
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no"
failed=0
check() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -8; failed=$((failed + 1)); fi; }

for h in "$A" "$B"; do
    out=$($SSH "root@$h" pos radio info 2>&1)
    check "$h radiod reachable" '"chip"' "$out"
    check "$h backend sx1262" '"backend":[[:space:]]*"sx1262"' "$out"
    if [ ${#PROFILE[@]} -gt 0 ]; then
        out=$($SSH "root@$h" pos radio configure "${PROFILE[@]}" 2>&1)
        check "$h profile applied" '"frequency_mhz"' "$out"
    fi
done
[ $failed -eq 0 ] || { echo "lora_pair_test: preconditions failed"; exit 1; }

echo "A=$A listening 8 s, B=$B sends after 2 s"
$SSH "root@$A" pos radio listen 8 > /tmp/lora_pair_A.txt 2>&1 &
LPID=$!
sleep 2
out=$($SSH "root@$B" pos radio send "$PAYLOAD" 2>&1)
check "B send accepted" '"airtime_ms"' "$out"
wait $LPID
events=$(cat /tmp/lora_pair_A.txt)
check "A received the payload" "\"payload_hex\":\"$PAYLOAD\"" "$events"
check "A reported RSSI" '"rssi_dbm":-?[0-9]' "$events"
check "A reported SNR" '"snr_db":-?[0-9]' "$events"
rssi=$(printf '%s' "$events" | grep -o '"rssi_dbm":-\?[0-9.]*' | head -1)
snr=$(printf '%s' "$events" | grep -o '"snr_db":-\?[0-9.]*' | head -1)
echo "link: $rssi $snr"

out=$($SSH "root@$A" pos radio stats 2>&1)
check "A stats rx_packets >= 1" '"rx_packets":[[:space:]]*[1-9]' "$out"
echo "lora_pair_test: $failed failure(s)"
exit $((failed > 0))
