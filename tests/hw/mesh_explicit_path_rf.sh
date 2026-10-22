#!/bin/bash
# Physical RF acceptance test for explicit per-message DIRECT routing
# (mesh.path_preview, mesh.send with via; docs/api/mesh.md,
# docs/validation/RIFT_EXPLICIT_PATH_P0.md). Run from a build host with SSH
# as root (key authentication) to two Doors units running the meshcored
# under test, and jq on the host.
#
# Usage: mesh_explicit_path_rf.sh <phase> <host-A> <host-B> [repeater-key ...]
#
#   check    no TX. Capabilities, build, identities, mutual contacts, the
#            hash width, each repeater's full key and type, A's learned route.
#   preview  no TX. mesh.path_preview for [] and the given repeaters, and
#            invalid routes; checks no message and no transmission appeared.
#   refuse   no TX. mesh.send with invalid via, via with a channel, and
#            resend with any via; checks no message and no transmission.
#   send     TRANSMITS (needs RF_TX=yes). One explicit message A -> B over
#            [] or the given repeaters in that order, then delivery, ACK,
#            the unchanged learned route and RESEND refusal.
#   auto     TRANSMITS (needs RF_TX=yes). One ordinary AUTO message A -> B,
#            as the control that AUTO send, route learning and ACK still work.
#
# Repeater keys are FULL 64-hex public keys in forwarding order (A first).
# pos-ipc-json (make tests/hw/pos-ipc-json, cross-built) must be on both
# units at $HELPER. EXPECT_BUILD, when set, must equal mesh.info's build.
#
# What this can and cannot show: `route`, `path_hex` and `path_hops` on a
# message are the REQUESTED route. RF forwarding is OBSERVED only in the
# receive logs printed at the end of `send`: B's copy of the text packet and
# whatever A heard repeated. An ACK may come back by B's learned route or by
# flood; it need not retrace the selected hops.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
PHASE=${1:?phase: check|preview|refuse|send|auto}
A=${2:?host A (sender)}
B=${3:?host B (recipient)}
shift 3
RELAYS=("$@")
HELPER=${HELPER:-/tmp/doors-rf/pos-ipc-json}
LISTEN_S=${LISTEN_S:-45}
OUT=${OUT:-./rf-explicit-$(date +%Y%m%dT%H%M%S)}
# SSH may be replaced, e.g. by a stand-in that reaches a local meshcored, to
# rehearse the script without a unit.
SSH=${SSH:-ssh -o BatchMode=yes -o ConnectTimeout=8}
failed=0
command -v jq > /dev/null || { echo "jq is required on the host running this script"; exit 2; }
mkdir -p "$OUT"

ok() { if [ "$2" = 1 ]; then echo "ok   $1"; else echo "FAIL $1${3:+: $3}"; failed=$((failed + 1)); fi; }
# ipc <host> <method> [json] -> one JSON line; error responses come back as
# {"error":{...}}. Every exchange is also appended to $OUT/ipc.log.
ipc() {
    local h=$1 m=$2 p=${3:-}
    local out
    if [ -n "$p" ]; then
        out=$($SSH "root@$h" "$HELPER $m $(printf '%q' "$p")" 2>&1)
    else
        out=$($SSH "root@$h" "$HELPER $m" 2>&1)
    fi
    printf '%s %s %s %s\n-> %s\n' "$(date +%T)" "$h" "$m" "$p" "$out" >> "$OUT/ipc.log"
    printf '%s\n' "$out" | tail -1
}
q() { printf '%s' "$1" | jq -r "$2" 2>/dev/null; }
jlist() { local s='[' k; for k in "$@"; do s="$s\"$k\","; done; printf '%s]' "${s%,}"; }
txcount() { q "$(ipc "$1" mesh.status)" '.counters.tx_submitted'; }
msgcount() { q "$(ipc "$1" mesh.messages)" '.total'; }
route_of() { q "$(ipc "$A" mesh.node "{\"node\":\"$KB\"}")" '[.path_known, .hops, .path_hex] | @json'; }
need_tx() {
    if [ "${RF_TX:-}" != yes ]; then
        echo "refusing: phase $PHASE transmits; set RF_TX=yes once the owner has authorised it"
        exit 2
    fi
}

KA=$(q "$(ipc "$A" mesh.identity)" .public_key)
KB=$(q "$(ipc "$B" mesh.identity)" .public_key)
if [ ${#KA} -ne 64 ] || [ ${#KB} -ne 64 ]; then
    echo "cannot read both identities (A='$KA' B='$KB'); see $OUT/ipc.log"
    exit 1
fi
echo "A $A $KA"
echo "B $B $KB"
echo "relays: ${RELAYS[*]:-(none: zero-relay DIRECT)}"

case "$PHASE" in
check)
    for h in "$A" "$B"; do
        info=$(ipc "$h" mesh.info)
        ok "$h explicit-route capabilities are true" \
           "$(q "$info" '(.capabilities.manual_path_preview == true and .capabilities.explicit_direct_send == true) | if . then 1 else 0 end')" "$info"
        [ -n "${EXPECT_BUILD:-}" ] && ok "$h runs build $EXPECT_BUILD" \
           "$(q "$info" ".build == \"$EXPECT_BUILD\" | if . then 1 else 0 end")" "$(q "$info" .build)"
        st=$(ipc "$h" mesh.status)
        ok "$h meshcored is online" "$(q "$st" '.state == "online" | if . then 1 else 0 end')" "$(q "$st" .state)"
    done
    n=$(ipc "$A" mesh.node "{\"node\":\"$KB\"}")
    ok "A knows B by full key as a chat node" "$(q "$n" ".public_key == \"$KB\" and .type == 1 | if . then 1 else 0 end")" "$n"
    n=$(ipc "$B" mesh.node "{\"node\":\"$KA\"}")
    ok "B knows A by full key as a chat node" "$(q "$n" ".public_key == \"$KA\" and .type == 1 | if . then 1 else 0 end")" "$n"
    echo "A path hash: $(ipc "$A" mesh.path_hash)"
    echo "A learned route to B [path_known,hops,path_hex]: $(route_of)"
    for r in "${RELAYS[@]}"; do
        n=$(ipc "$A" mesh.node "{\"node\":\"$r\"}")
        ok "A knows repeater ${r:0:8} by full key as a repeater" \
           "$(q "$n" ".public_key == \"$r\" and .type == 2 | if . then 1 else 0 end")" "$n"
        echo "     ${r:0:8}: $(q "$n" '{name, advert_hops, last_rssi_dbm, last_snr_db} | @json')"
    done
    ;;
preview)
    tx0=$(txcount "$A"); m0=$(msgcount "$A")
    routes=('[]')
    [ ${#RELAYS[@]} -gt 0 ] && routes+=("$(jlist "${RELAYS[@]}")")
    for r in "${RELAYS[@]}"; do [ ${#RELAYS[@]} -gt 1 ] && routes+=("$(jlist "$r")"); done
    for v in "${routes[@]}"; do
        p=$(ipc "$A" mesh.path_preview "{\"to\":\"$KB\",\"via\":$v}")
        ok "preview via $v is valid and claims no link evidence" \
           "$(q "$p" '.valid == true and .route == "explicit" and .transmitted == false and .link_evidence_verified == false | if . then 1 else 0 end')" "$p"
        echo "     $(q "$p" '{relays, hash_bytes, path_bytes, path_hex} | @json')"
    done
    bad=("{\"to\":\"${KB:0:8}\",\"via\":[]}" "{\"to\":\"$KB\",\"via\":null}"
         "{\"to\":\"$KB\",\"via\":\"auto\"}" "{\"to\":\"$KB\",\"via\":[\"$KB\"]}"
         "{\"to\":\"$KB\",\"via\":[\"$KA\"]}"
         "{\"to\":\"$KB\",\"via\":[\"$(printf '%064d' 1)\"]}")
    [ ${#RELAYS[@]} -gt 0 ] && bad+=("{\"to\":\"$KB\",\"via\":$(jlist "${RELAYS[0]}" "${RELAYS[0]}")}")
    for p in "${bad[@]}"; do
        e=$(ipc "$A" mesh.path_preview "$p")
        ok "invalid preview $p is refused with error 2" "$(q "$e" '.error.code == 2 | if . then 1 else 0 end')" "$e"
    done
    ok "preview transmitted nothing" "$([ "$(txcount "$A")" = "$tx0" ] && echo 1 || echo 0)"
    ok "preview added no message" "$([ "$(msgcount "$A")" = "$m0" ] && echo 1 || echo 0)"
    ;;
refuse)
    tx0=$(txcount "$A"); m0=$(msgcount "$A")
    for v in null '"auto"' "[\"$KA\"]" "[\"$(printf '%064d' 1)\"]"; do
        e=$(ipc "$A" mesh.send "{\"to\":\"$KB\",\"text\":\"must not send\",\"via\":$v}")
        ok "send with via $v is refused with error 2" "$(q "$e" '.error.code == 2 | if . then 1 else 0 end')" "$e"
    done
    e=$(ipc "$A" mesh.send "{\"to\":\"${KB:0:8}\",\"text\":\"must not send\",\"via\":[]}")
    ok "send with via and a key prefix is refused" "$(q "$e" '.error.code == 2 | if . then 1 else 0 end')" "$e"
    e=$(ipc "$A" mesh.send '{"channel":0,"text":"must not send","via":[]}')
    ok "send with via and a channel is refused" "$(q "$e" '.error.code == 2 | if . then 1 else 0 end')" "$e"
    id=$(q "$(ipc "$A" mesh.messages)" '[.messages[] | select(.direction == "out" and .kind == "direct")] | last | .id // 1')
    for v in '[]' null '"auto"' "[\"${RELAYS[0]:-$KB}\"]"; do
        e=$(ipc "$A" mesh.send "{\"resend\":$id,\"via\":$v}")
        ok "resend $id with via $v is refused before routing" \
           "$(q "$e" '.error.code == 2 and .error.message == "resend takes a message id and nothing else" | if . then 1 else 0 end')" "$e"
    done
    ok "refusals transmitted nothing" "$([ "$(txcount "$A")" = "$tx0" ] && echo 1 || echo 0)"
    ok "refusals added no message" "$([ "$(msgcount "$A")" = "$m0" ] && echo 1 || echo 0)"
    ;;
send|auto)
    need_tx
    token="rfp0-$PHASE-$(date +%H%M%S)-$RANDOM"
    before=$(route_of)
    echo "A learned route to B before: $before"
    $SSH "root@$A" "$HELPER -l $LISTEN_S mesh.subscribe '{\"rx_log\":true}'" > "$OUT/A-rx.jsonl" 2>&1 &
    la=$!
    $SSH "root@$B" "$HELPER -l $LISTEN_S mesh.subscribe '{\"rx_log\":true}'" > "$OUT/B-rx.jsonl" 2>&1 &
    lb=$!
    sleep 2
    if [ "$PHASE" = send ]; then
        v=$(jlist "${RELAYS[@]}")
        p=$(ipc "$A" mesh.path_preview "{\"to\":\"$KB\",\"via\":$v}")
        s=$(ipc "$A" mesh.send "{\"to\":\"$KB\",\"text\":\"$token\",\"via\":$v}")
        ok "explicit send via $v is accepted as explicit" "$(q "$s" '.accepted == true and .route == "explicit" | if . then 1 else 0 end')" "$s"
    else
        s=$(ipc "$A" mesh.send "{\"to\":\"$KB\",\"text\":\"$token\"}")
        ok "AUTO send is accepted as flood or direct" "$(q "$s" '.accepted == true and (.route == "flood" or .route == "direct") | if . then 1 else 0 end')" "$s"
    fi
    id=$(q "$s" .message_id); wait_ms=$(q "$s" '.ack_timeout_ms // 10000')
    end=$(( $(date +%s) + wait_ms / 1000 + 10 ))
    m=""
    while [ "$(date +%s)" -lt "$end" ]; do
        m=$(q "$(ipc "$A" mesh.messages)" ".messages[] | select(.id == $id) | @json")
        case "$(q "$m" .state)" in acked|no_ack|failed) break ;; esac
        sleep 2
    done
    echo "A message: $m"
    ok "B's ACK is matched to this message on A" "$(q "$m" '.state == "acked" | if . then 1 else 0 end')" "$(q "$m" .state)"
    if [ "$PHASE" = send ]; then
        ok "the message records the requested route" \
           "$(q "$m" ".route == \"explicit\" and (.path_hex // \"\") == \"$(q "$p" .path_hex)\" and .path_hops == $(q "$p" .relays) | if . then 1 else 0 end")" "$m"
    fi
    r=$(q "$(ipc "$B" mesh.messages)" "[.messages[] | select(.direction == \"in\" and .text == \"$token\")] | @json")
    ok "B received the text exactly once, from A" \
       "$(q "$r" "length == 1 and .[0].peer_public_key == \"$KA\" | if . then 1 else 0 end")" "$r"
    after=$(route_of)
    echo "A learned route to B after: $after"
    if [ "$PHASE" = send ]; then
        ok "the explicit send left A's learned route to B unchanged" "$([ "$before" = "$after" ] && echo 1 || echo 0)" "$before -> $after"
        tx0=$(txcount "$A")
        e=$(ipc "$A" mesh.send "{\"resend\":$id}")
        ok "RESEND of the explicit message is refused" "$(q "$e" '.error.code == 2 | if . then 1 else 0 end')" "$e"
        echo "     refusal: $(q "$e" .error.message)"
        ok "the refused RESEND transmitted nothing" "$([ "$(txcount "$A")" = "$tx0" ] && echo 1 || echo 0)"
    fi
    echo "waiting for the receive logs (${LISTEN_S}s from the start)"
    wait $la $lb
    echo "OBSERVED on B (text packets, as received):"
    jq -c 'select(.event == "mesh.rx") | .data | select(.type == "text") | {seq, route, path_hash_size, path_hops, path_hex, rssi_dbm, snr_db, hash, dup}' "$OUT/B-rx.jsonl" 2>/dev/null
    echo "OBSERVED on A (own packets heard repeated, and ACK/PATH frames):"
    jq -c 'select(.event == "mesh.rx") | .data | select(.own == true or .type == "ack" or .type == "path") | {seq, type, route, own, path_hash_size, path_hops, path_hex, rssi_dbm, hash, dup}' "$OUT/A-rx.jsonl" 2>/dev/null
    ;;
*)
    echo "unknown phase $PHASE"; exit 2 ;;
esac
echo "mesh_explicit_path_rf $PHASE: $failed failure(s); exchanges in $OUT/ipc.log"
exit $((failed > 0))
