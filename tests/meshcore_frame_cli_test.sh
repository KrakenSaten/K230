#!/bin/bash
# meshcore-frame from the outside: the command line, the identity file, the
# exit codes, and the one property the whole tool exists for - that the hex it
# prints is hex a MeshCore node and radiod will both accept, and that feeding
# it back in gives the same frame.
#
# Run by tools/meshcore-frame/Makefile:  make meshcore-frame-test
# Takes the binary to exercise as its argument.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
# The binary may be named relative to wherever this was invoked from (the tool
# Makefile runs it from its own directory), so resolve it before moving to the
# top of the repository, which everything else here is relative to.
INVOKED_FROM=$PWD
cd "$(dirname "$0")/.." || exit 1
failed=0
checks=0
check() {
    checks=$((checks + 1))
    if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}

MCF=${1:-tools/meshcore-frame/meshcore-frame}
case "$MCF" in
    /*) ;;
    *) [ $# -ge 1 ] && MCF="$INVOKED_FROM/$MCF" ;;
esac
if [ ! -x "$MCF" ]; then
    echo "NOT RUN meshcore_frame_cli_test: $MCF is not there."
    echo "        Build it with: make meshcore-frame"
    exit 77
fi
MCF=$(cd "$(dirname "$MCF")" && pwd)/$(basename "$MCF")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- version ---------------------------------------------------------------
# The pins in the checkout are what the binary reports, so a report that quotes
# `meshcore-frame version` is quoting the protocol source it really used.
out=$("$MCF" version)
check "version names the RIFT commit the tool was built from" \
    "$(printf '%s' "$out" | grep -q "meshcore_source: KrakenSaten/RIFT $(tr -d ' \t\r\n' < tools/meshcore-frame/vendor_rift_commit.txt)" && echo 1 || echo 0)"
check "version names the crypto commit" \
    "$(printf '%s' "$out" | grep -q "crypto_source: rweather/arduinolibs $(tr -d ' \t\r\n' < tools/meshcore-frame/vendor_crypto_commit.txt)" && echo 1 || echo 0)"
check "version states the MeshCore MTU and payload limit" \
    "$(printf '%s' "$out" | grep -q '^mtu_bytes: 255$' &&
       printf '%s' "$out" | grep -q '^max_payload_bytes: 184$' && echo 1 || echo 0)"

# ---- identities ------------------------------------------------------------
"$MCF" identity new "$TMP/a.id" > "$TMP/a.txt" 2>&1
check "identity new writes a key file" "$([ -f "$TMP/a.id" ] && echo 1 || echo 0)"
check "the identity file is a 96-byte MeshCore .id" \
    "$([ "$(wc -c < "$TMP/a.id")" = 96 ] && echo 1 || echo 0)"
check "the identity file is not readable by anyone else" \
    "$([ "$(stat -c '%a' "$TMP/a.id")" = 600 ] && echo 1 || echo 0)"

"$MCF" identity new "$TMP/a.id" > /dev/null 2>&1
check "identity new refuses to overwrite an existing key" "$([ $? -ne 0 ] && echo 1 || echo 0)"

A=$("$MCF" identity show "$TMP/a.id" | sed -n 's/^public_key: //p')
check "identity show reports a 32-byte public key" \
    "$(printf '%s' "$A" | grep -q '^[0-9a-f]\{64\}$' && echo 1 || echo 0)"
check "identity new and identity show agree" \
    "$(printf '%s' "$(sed -n 's/^public_key: //p' "$TMP/a.txt")" | grep -qx "$A" && echo 1 || echo 0)"
check "the node hash is the first byte of the public key" \
    "$([ "$("$MCF" identity show "$TMP/a.id" | sed -n 's/^node_hash: //p')" = "${A:0:2}" ] && echo 1 || echo 0)"

head -c 95 "$TMP/a.id" > "$TMP/short.id"
"$MCF" identity show "$TMP/short.id" > /dev/null 2>&1
check "a truncated identity file is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"

head -c 96 /dev/zero > "$TMP/zero.id"
"$MCF" identity show "$TMP/zero.id" > /dev/null 2>&1
check "an identity file that is not a key pair is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"

# A file whose two halves disagree: a valid private key, but not the public
# key it derives. Signatures made with it would be attributed to a node that
# never made them, so the file is refused rather than half-used.
{ head -c 31 "$TMP/a.id"; printf 'Z'; tail -c 64 "$TMP/a.id"; } > "$TMP/mixed.id"
"$MCF" identity show "$TMP/mixed.id" > /dev/null 2>&1
check "an identity whose public key is not the one its private key derives is refused" \
    "$([ $? -ne 0 ] && echo 1 || echo 0)"

"$MCF" identity show "$TMP/missing.id" > /dev/null 2>&1
check "a missing identity file is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"

"$MCF" identity new "$TMP/b.id" > /dev/null
B=$("$MCF" identity show "$TMP/b.id" | sed -n 's/^public_key: //p')

# ---- ADVERT ----------------------------------------------------------------
"$MCF" advert --key "$TMP/a.id" --name K230-A --timestamp 1709999616 > "$TMP/adv.txt"
check "advert exits 0" "$([ $? -eq 0 ] && echo 1 || echo 0)"
HEX=$(sed -n 's/^frame_hex: //p' "$TMP/adv.txt")

check "the advert hex is lower-case hex, two digits a byte" \
    "$(printf '%s' "$HEX" | grep -q '^\([0-9a-f][0-9a-f]\)\{1,255\}$' && echo 1 || echo 0)"
check "the advert is within the 255-byte MTU radio.send accepts" \
    "$([ "${#HEX}" -le 510 ] && echo 1 || echo 0)"
check "frame_bytes agrees with the hex it printed" \
    "$([ "$(sed -n 's/^frame_bytes: //p' "$TMP/adv.txt")" = "$((${#HEX} / 2))" ] && echo 1 || echo 0)"
check "the advert says it is an ADVERT on a flood route" \
    "$(grep -q '^payload_type: 4 (ADVERT)$' "$TMP/adv.txt" &&
       grep -q '^route_type: 1 (flood)$' "$TMP/adv.txt" && echo 1 || echo 0)"
check "the advert carries the identity that signed it" \
    "$(grep -qx "advert.public_key: $A" "$TMP/adv.txt" && echo 1 || echo 0)"
check "the advert signature verifies where it was built" \
    "$(grep -q '^advert.signature_valid: yes$' "$TMP/adv.txt" && echo 1 || echo 0)"
check "the advert name survives into the frame" \
    "$(grep -q '^advert.name: K230-A$' "$TMP/adv.txt" && echo 1 || echo 0)"
check "the advert timestamp is the one asked for" \
    "$(grep -q '^advert.timestamp: 1709999616 ' "$TMP/adv.txt" && echo 1 || echo 0)"

# ---- the round trip that matters -------------------------------------------
"$MCF" parse "$HEX" > "$TMP/parsed.txt"
check "parse exits 0 on the frame the builder printed" "$([ $? -eq 0 ] && echo 1 || echo 0)"
check "parse reports the same hex it was given, byte for byte" \
    "$([ "$(sed -n 's/^frame_hex: //p' "$TMP/parsed.txt")" = "$HEX" ] && echo 1 || echo 0)"
check "building and parsing describe the same frame" \
    "$(diff <(grep -v '^frame_hex: ' "$TMP/adv.txt") \
            <(grep -v '^frame_hex: ' "$TMP/parsed.txt") > /dev/null && echo 1 || echo 0)"
check "upper-case hex parses to the same frame" \
    "$([ "$("$MCF" parse "$(printf '%s' "$HEX" | tr 'a-f' 'A-F')" | sed -n 's/^frame_hex: //p')" = "$HEX" ] && echo 1 || echo 0)"

# ---- a tampered advert -----------------------------------------------------
# Flip one bit of the last signature byte. The frame is still well formed, so
# it parses; it is not authentic, so the command fails.
sig_end=$(( (2 + 32 + 4 + 64) * 2 ))
byte=${HEX:$((sig_end - 2)):2}
flipped=$(printf '%02x' $(( 0x$byte ^ 0x40 )))
BAD="${HEX:0:$((sig_end - 2))}$flipped${HEX:$sig_end}"
out=$("$MCF" parse "$BAD" 2>&1); rc=$?
check "a tampered advert signature is reported as invalid" \
    "$(printf '%s' "$out" | grep -q '^advert.signature_valid: no$' && echo 1 || echo 0)"
check "and the tampered advert exits non-zero" "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
check "the tampered frame still parses as a frame" \
    "$(printf '%s' "$out" | grep -q '^payload_type: 4 (ADVERT)$' && echo 1 || echo 0)"

# ---- hex and frames the parser must refuse ---------------------------------
refuse() { # <label> <hex>
    "$MCF" parse "$2" > /dev/null 2>&1
    check "$1" "$([ $? -ne 0 ] && echo 1 || echo 0)"
}
refuse "an odd number of hex digits is refused" "${HEX}a"
refuse "a non-hex character is refused" "${HEX%??}zz"
refuse "empty hex is refused" ""
refuse "an 0x prefix is refused" "0x$HEX"
refuse "whitespace in the hex is refused" "${HEX:0:8} ${HEX:8}"
refuse "a frame cut to its header is refused" "${HEX:0:2}"
refuse "a frame cut mid-payload is refused" "${HEX:0:40}"
refuse "a frame past the 255-byte MTU is refused" \
    "$(printf '11'; printf '00'; for _ in $(seq 1 256); do printf 'aa'; done)"
refuse "a payload past the 184-byte limit is refused" \
    "$(printf '0d'; printf '00'; for _ in $(seq 1 200); do printf 'aa'; done)"
refuse "the reserved path hash size is refused" "0ec1112233"
refuse "a path longer than the frame is refused" "0e3f11"
refuse "a path past MAX_PATH_SIZE is refused" "0ea011"
refuse "a frame with no payload is refused" "1100"

# ---- an advert whose app_data lies about itself ----------------------------
# An advert with neither a name nor a location carries one app_data byte: the
# flags. Set the location bit in it and that byte now announces eight bytes
# which are not in the frame. MeshCore parses the fields its flags name before
# it looks at the length it was given, so it reads them anyway, out of
# whatever follows in its own payload buffer. This tool has to say the data is
# not there rather than print a position nobody advertised. (Flipping the byte
# also invalidates the signature, which is reported separately and is not what
# this checks.)
BARE=$("$MCF" advert --key "$TMP/a.id" --timestamp 1709999616 |
       sed -n 's/^frame_hex: //p')
check "an advert with no name or location carries one app_data byte" \
    "$([ "${BARE: -2}" = "01" ] && echo 1 || echo 0)"
LIAR="${BARE%??}11"
out=$("$MCF" parse "$LIAR" 2>&1)
check "app_data too short for its own flags is called out, not decoded" \
    "$(printf '%s' "$out" |
       grep -q '^advert.app_data_valid: no (flags 0x11 need 9 bytes, 1 present)$' &&
       ! printf '%s' "$out" | grep -q '^advert.lat: ' && echo 1 || echo 0)"

# ---- text from the air cannot drive the terminal ---------------------------
# A node name and a message body are attacker-controlled, and this tool prints
# them to a terminal. Control bytes have to come out escaped.
esc=$(printf 'red\033[31malert\007')
out=$("$MCF" txtmsg --key "$TMP/a.id" --peer "$B" --text "$esc" \
      --timestamp 1709999616 2>&1)
check "control bytes in a message are escaped, not printed" \
    "$(printf '%s' "$out" |
       grep -q 'txt.text: red\\x1b\[31malert\\x07' && echo 1 || echo 0)"
check "and no raw escape byte reaches the output" \
    "$(printf '%s' "$out" | grep -q "$(printf '\033')" && echo 0 || echo 1)"

# ---- TXT_MSG and ACK -------------------------------------------------------
"$MCF" txtmsg --key "$TMP/a.id" --peer "$B" --text "hello from K230" \
       --timestamp 1709999616 > "$TMP/msg.txt"
check "txtmsg exits 0" "$([ $? -eq 0 ] && echo 1 || echo 0)"
MHEX=$(sed -n 's/^frame_hex: //p' "$TMP/msg.txt")
ACK=$(sed -n 's/^txt.ack_to_expect: //p' "$TMP/msg.txt")

check "the message is a TXT_MSG" \
    "$(grep -q '^payload_type: 2 (TXT_MSG)$' "$TMP/msg.txt" && echo 1 || echo 0)"
check "the destination hash is the recipient's node hash" \
    "$(grep -qx "txt.dest_hash: ${B:0:2}" "$TMP/msg.txt" && echo 1 || echo 0)"
check "the source hash is the sender's node hash" \
    "$(grep -qx "txt.src_hash: ${A:0:2}" "$TMP/msg.txt" && echo 1 || echo 0)"
check "txtmsg says which ACK to expect" \
    "$(printf '%s' "$ACK" | grep -q '^[0-9a-f]\{8\}$' && echo 1 || echo 0)"

"$MCF" parse "$MHEX" --key "$TMP/b.id" --peer "$A" > "$TMP/rx.txt"
check "the recipient decrypts the message" \
    "$([ $? -eq 0 ] && grep -q '^txt.decrypted: yes$' "$TMP/rx.txt" && echo 1 || echo 0)"
check "the text survives the round trip" \
    "$(grep -qx 'txt.text: hello from K230' "$TMP/rx.txt" && echo 1 || echo 0)"
check "the recipient works out the same ACK the sender expects" \
    "$(grep -qx "txt.ack_if_sent_by_peer: $ACK" "$TMP/rx.txt" && echo 1 || echo 0)"
check "the recipient sees --peer as the sender" \
    "$(grep -qx 'txt.sender: --peer' "$TMP/rx.txt" && echo 1 || echo 0)"

"$MCF" parse "$MHEX" > "$TMP/rx2.txt"
check "parsing without keys describes the message but does not open it" \
    "$([ $? -eq 0 ] && grep -q '^txt.decrypted: not attempted' "$TMP/rx2.txt" && echo 1 || echo 0)"

"$MCF" parse "$MHEX" --key "$TMP/b.id" --peer "$B" > "$TMP/rx3.txt" 2>&1
rc=$?
check "the wrong key pair fails the MAC" \
    "$(grep -q '^txt.decrypted: no ' "$TMP/rx3.txt" && echo 1 || echo 0)"
check "and exits non-zero" "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"

# One flipped ciphertext bit, checked against the key pair that should open it.
pos=$(( (2 + 1 + 1 + 2) * 2 ))
byte=${MHEX:$pos:2}
flipped=$(printf '%02x' $(( 0x$byte ^ 0x01 )))
MBAD="${MHEX:0:$pos}$flipped${MHEX:$((pos + 2))}"
"$MCF" parse "$MBAD" --key "$TMP/b.id" --peer "$A" > "$TMP/rx4.txt" 2>&1
rc=$?
check "a flipped ciphertext bit fails the MAC" \
    "$(grep -q '^txt.decrypted: no ' "$TMP/rx4.txt" && echo 1 || echo 0)"
check "and that exits non-zero too" "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"

"$MCF" ack --hash "$ACK" > "$TMP/ack.txt"
check "ack exits 0" "$([ $? -eq 0 ] && echo 1 || echo 0)"
AHEX=$(sed -n 's/^frame_hex: //p' "$TMP/ack.txt")
check "an ACK is a six-byte frame" \
    "$([ "$(sed -n 's/^frame_bytes: //p' "$TMP/ack.txt")" = 6 ] && echo 1 || echo 0)"
check "the ACK carries the hash it was given" \
    "$(grep -qx "ack.hex: $ACK" "$TMP/ack.txt" && echo 1 || echo 0)"
check "the ACK parses back to the same hash" \
    "$([ "$("$MCF" parse "$AHEX" | sed -n 's/^ack.hex: //p')" = "$ACK" ] && echo 1 || echo 0)"

# ---- routing ---------------------------------------------------------------
out=$("$MCF" ack --hash "$ACK" --route direct --path aabbcc)
check "a direct route carries its path" \
    "$(printf '%s' "$out" | grep -q '^route_type: 2 (direct)$' &&
       printf '%s' "$out" | grep -q '^path_hash_count: 3$' &&
       printf '%s' "$out" | grep -q '^path_hex: aabbcc$' && echo 1 || echo 0)"
check "the direct frame parses back the same way" \
    "$([ "$("$MCF" parse "$(printf '%s' "$out" | sed -n 's/^frame_hex: //p')" | sed -n 's/^path_hex: //p')" = "aabbcc" ] && echo 1 || echo 0)"

out=$("$MCF" ack --hash "$ACK" --route transport-flood --transport 258 772)
check "transport codes are written little endian and read back" \
    "$(printf '%s' "$out" | grep -q '^transport_codes: 258 772$' &&
       printf '%s' "$out" | grep -q '^frame_hex: 0c0201040300' && echo 1 || echo 0)"

"$MCF" ack --hash "$ACK" --path aabbcc > /dev/null 2>&1
check "a path on a flood route is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" ack --hash "$ACK" --route direct --path aabb --path-hash-size 3 > /dev/null 2>&1
check "a path that is not a whole number of hashes is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" ack --hash "$ACK" --path-hash-size 4 > /dev/null 2>&1
check "the reserved path hash size is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"

# ---- argument checking -----------------------------------------------------
"$MCF" advert --name K230-A > /dev/null 2>&1
check "advert without a key is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" advert --key "$TMP/a.id" --name "$(printf 'x%.0s' $(seq 1 40))" > /dev/null 2>&1
check "a name too long for the advert data is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" advert --key "$TMP/a.id" --lat 1.0 > /dev/null 2>&1
check "a latitude without a longitude is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" advert --key "$TMP/a.id" --lat 100 --lon 0 > /dev/null 2>&1
check "an out-of-range latitude is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" txtmsg --key "$TMP/a.id" --peer "${B:0:10}" --text hi > /dev/null 2>&1
check "a peer key of the wrong length is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" txtmsg --key "$TMP/a.id" --peer "$B" > /dev/null 2>&1
check "txtmsg without text is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"
"$MCF" nonsense > /dev/null 2>&1
check "an unknown command is refused" "$([ $? -ne 0 ] && echo 1 || echo 0)"

# ---- a located advert ------------------------------------------------------
out=$("$MCF" advert --key "$TMP/a.id" --name K230-A --lat 59.913869 --lon 10.752245 \
      --timestamp 1709999616)
check "a located advert verifies and reports its position" \
    "$(printf '%s' "$out" | grep -q '^advert.signature_valid: yes$' &&
       printf '%s' "$out" | grep -q '^advert.lat: 59.913869$' &&
       printf '%s' "$out" | grep -q '^advert.lon: 10.752245$' && echo 1 || echo 0)"
check "the located advert round-trips through parse" \
    "$("$MCF" parse "$(printf '%s' "$out" | sed -n 's/^frame_hex: //p')" |
       grep -q '^advert.lat: 59.913869$' && echo 1 || echo 0)"

echo "meshcore_frame_cli_test: $checks check(s), $failed failure(s)"
exit $((failed > 0))
