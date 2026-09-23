#!/bin/bash
# End-to-end test: netd, pos wifi and the credential store, with the radio
# replaced by tests/fake_wpa_supplicant (a scenario-driven stand-in for
# wpa_supplicant's control interface) and a fake udhcpc. The netd under test
# is tests/netd-testhooks: the shipped services/netd/netd with only its
# machine-facing paths redirected into a test tree (services/netd/netd_sys.h).
#
# Run from the repository root after `make all tests/netd-testhooks
# tests/fake_wpa_supplicant`. Uses python3 only to send exact JSON frames.
#
# Covers: absent interface, off by default, turning on, scan parsing with
# duplicate and hidden SSIDs, malformed and oversized input, policy refusals,
# authentication failure, a join with DHCP, the passphrase never leaving the
# store (logs, command lines, CLI output, the fake's own record), restrictive
# permissions, disconnect, rejoining a saved network, DHCP failure and
# recovery, an open network only when asked, forget, restart with automatic
# reconnect, a supplicant crash, a foreign supplicant, a damaged store, the
# interface disappearing, and protocol robustness.
#
# A failed run keeps its evidence in out/test-failures/ (or
# $TEST_EVIDENCE_DIR; not a NETD_TEST_ name, which initscript_test.sh rightly
# refuses to see reach netd): the whole test tree (netd's log and stdio, the fake
# supplicant's record, the store, the interface files, the DHCP log), every
# request sent to netd with its response and times, and for each failed check
# the time, the output it judged and netd's status at that moment, plus the
# exit status and the processes still running. A passing run keeps nothing.
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
NETD=${NETD:-$REPO/tests/netd-testhooks}
SHIPPED=${SHIPPED:-$REPO/services/netd/netd}
POS=${POS:-$REPO/tools/pos/pos}
FAKE=${FAKE:-$REPO/tests/fake_wpa_supplicant}
failed=0
checks=0

check() { # <name> <0|1>
    checks=$((checks + 1))
    if [ "$2" = "1" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        failed=$((failed + 1))
        note_failure "$1"
    fi
}
# Times in netd's own log format, so the two line up.
stamp() { date -u +%Y-%m-%dT%H:%M:%S.%3NZ; }
note_failure() { # <check name>
    {
        echo "== $(stamp) check failed: $1"
        echo "-- last request to netd (rpc.log has them all):"
        tail -n 2 "$DIAG/rpc.log" 2>/dev/null
        echo "-- last captured \$out (the check may have judged a newer command): ${out-}"
        echo "-- wifi.status now: $(st | tr -d '\n')"
    } >> "$DIAG/failures.txt"
}
has() { printf '%s' "$1" | grep -q -- "$2" && echo 1 || echo 0; }
hasnt() { printf '%s' "$1" | grep -q -- "$2" && echo 0 || echo 1; }
# "11", "111": every part held
all() { case "$1" in *0*|"") echo 0 ;; *) echo 1 ;; esac; }

for b in "$NETD" "$SHIPPED" "$POS" "$FAKE"; do
    [ -x "$b" ] || { echo "FAIL missing $b (run make all tests/netd-testhooks tests/fake_wpa_supplicant)"; exit 1; }
done

TMP=$(mktemp -d)
# Diagnostics live outside $TMP: the checks below search that tree for the
# passphrase and must find it only where netd put it.
DIAG=$(mktemp -d)
NP=""
cleanup() {
    local status=$? keep=""
    if [ "$failed" -gt 0 ] || [ "$status" -ne 0 ]; then
        keep=${TEST_EVIDENCE_DIR:-$REPO/out/test-failures}/netd_test-$(date -u +%Y%m%dT%H%M%SZ)-$$
        {
            echo "exit status $status, $checks checks, $failed failure(s), ended $(stamp)"
            echo "-- wifi.status at exit: $(st 2>&1 | tr -d '\n')"
            echo "-- processes of this run:"
            ps -eo pid,ppid,stat,etimes,args | grep -F -e "$TMP" -e "$FAKE" -e "$NETD" | grep -v 'grep -F'
        } > "$DIAG/exit.txt" 2>&1
    fi
    [ -n "$NP" ] && kill "$NP" 2>/dev/null
    sleep 0.3
    pkill -f "$TMP/" 2>/dev/null
    pkill -f "$FAKE" 2>/dev/null
    if [ -n "$keep" ] && mkdir -p "$keep/tree"; then
        # Sockets cannot be archived; everything else is.
        tar -C "$TMP" -cf - . 2>/dev/null | tar -C "$keep/tree" -xf - 2>/dev/null
        cp -r "$DIAG/." "$keep/"
        echo "netd_test: evidence from this run kept in $keep"
    fi
    rm -rf "$TMP" "$DIAG"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

export POCKETOS_RUNTIME_DIR=$TMP/run POCKETOS_STATE_DIR=$TMP/state POCKETOS_LOG_DIR=$TMP/log
export NETD_TEST_ROOT=$TMP/root NETD_TEST_WPA_SUPPLICANT=$FAKE NETD_TEST_UDHCPC=$TMP/udhcpc
export FAKE_WPA_SCENARIO=$TMP/scenario FAKE_WPA_RECORD=$TMP/record
export POCKETOS_LOG_STDERR=0
IFDIR=$NETD_TEST_ROOT/sys/class/net/wlan0
STORE=$POCKETOS_STATE_DIR/netd/wifi.conf
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$NETD_TEST_ROOT/proc" "$NETD_TEST_ROOT/sys/class/net"
: > "$NETD_TEST_ROOT/default.script"

# The passphrase that works. A space, both quote kinds, a backslash, '#', '='
# and '%': everything that could break a command, a config line or a format.
PASS='Pa"ss wo\rd#=%s 1'"'"'x'
PASS_HEX=$(printf '%s' "$PASS" | od -An -tx1 | tr -d ' \n')
# SSIDs as hex: Home, Guest, Corp, W3only, Old, and "Café ø" in UTF-8.
HOME_HEX=486f6d65; GUEST_HEX=4775657374; CAFE_HEX=436166c3a920c3b8

cat > "$FAKE_WPA_SCENARIO" <<EOF
bss aa:bb:cc:00:00:01 2412 -71 [WPA2-PSK-CCMP][ESS] $HOME_HEX
bss aa:bb:cc:00:00:02 5180 -40 [WPA2-PSK-CCMP][ESS] $HOME_HEX
bss aa:bb:cc:00:00:03 2437 -60 [ESS] $GUEST_HEX
bss aa:bb:cc:00:00:04 2462 -65 [WPA2-EAP-CCMP][ESS] 436f7270
bss aa:bb:cc:00:00:05 2412 -66 [WPA2-SAE-CCMP][ESS] 57336f6e6c79
bss aa:bb:cc:00:00:06 2412 -80 [WEP][ESS] 4f6c64
bss aa:bb:cc:00:00:07 2412 -50 [WPA2-PSK-CCMP][ESS] $CAFE_HEX
hidden aa:bb:cc:00:00:08 2412 -55 [WPA2-PSK-CCMP][ESS] 536563726574
key $HOME_HEX $PASS_HEX
key 536563726574 $PASS_HEX
key $CAFE_HEX $PASS_HEX
EOF

# udhcpc stand-in: the address appears where netd-testhooks reads it, and
# goes away on SIGTERM as `udhcpc -R` would release it.
cat > "$TMP/udhcpc" <<EOF
#!/bin/sh
echo "udhcpc \$*" >> "$TMP/dhcp.log"
trap 'rm -f "$IFDIR/ipv4"; echo released >> "$TMP/dhcp.log"; exit 0' TERM INT
if [ "\$(cat "$TMP/dhcp_mode" 2>/dev/null || echo ok)" = ok ]; then
    sleep 0.3
    echo 192.168.50.23 > "$IFDIR/ipv4"
fi
while :; do sleep 0.2; done
EOF
chmod 0755 "$TMP/udhcpc"

start_netd() {
    "$NETD" --dhcp-timeout-s 2 --connect-timeout-s 8 --verbose >>"$TMP/netd.stdio" 2>&1 & NP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/netd.sock" ] && return 0; sleep 0.1; done
    return 1
}
stop_netd() {
    [ -n "$NP" ] && kill "$NP" 2>/dev/null && wait "$NP" 2>/dev/null
    NP=""
}
st() { "$POS" call netd wifi.status 2>&1; }
field() { # <json> <key> -> raw value
    printf '%s' "$1" | tr -d '\n' | sed -n "s/.*\"$2\":[[:space:]]*\(\"[^\"]*\"\|[^,}]*\).*/\1/p" | tr -d '"' | tr -d ' \t'
}
wait_state() { # <state> [seconds]
    _n=0
    while [ $_n -lt $(( ${2:-8} * 10 )) ]; do
        [ "$(field "$(st)" state)" = "$1" ] && return 0
        sleep 0.1; _n=$((_n + 1))
    done
    return 1
}
rpc() { # <method> <json params> -> response JSON (result or error); logged to $DIAG/rpc.log
    local t0 resp
    t0=$(stamp)
    resp=$(python3 - "$POCKETOS_RUNTIME_DIR/netd.sock" "$1" "$2" 2>>"$DIAG/rpc.log" <<'PY'
import json, socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
req = {"id": 7, "method": sys.argv[2]}
if sys.argv[3] != "null":
    req["params"] = json.loads(sys.argv[3])
body = json.dumps(req).encode()
s.sendall(struct.pack(">I", len(body)) + body)
def read(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit("closed")
        b += c
    return b
n = struct.unpack(">I", read(4))[0]
print(read(n).decode())
PY
)
    printf '%s .. %s %s %.200s\n    -> %.400s\n' "$t0" "$(stamp)" "$1" "$2" "$resp" >> "$DIAG/rpc.log"
    printf '%s\n' "$resp"
}
code_of() { printf '%s' "$1" | sed -n 's/.*"code":[[:space:]]*\([0-9]*\).*/\1/p'; }

# ---- the shipped binary cannot be redirected ------------------------------------
check "the shipped netd carries no test hook names" \
    "$([ "$(strings "$SHIPPED" | grep -c 'NETD_TEST_')" = "0" ] && echo 1 || echo 0)"
check "the test build does (so the check above means something)" \
    "$([ "$(strings "$NETD" | grep -c 'NETD_TEST_')" -ge 1 ] && echo 1 || echo 0)"

# ---- no interface -----------------------------------------------------------------
start_netd; check "netd starts with no wireless interface" "$([ $? = 0 ] && echo 1 || echo 0)"
wait_state unavailable 3
out=$(st)
check "no interface: unavailable" "$(has "$out" '"state":[[:space:]]*"unavailable"')"
check "no interface: reason no_interface" "$(has "$out" '"reason":[[:space:]]*"no_interface"')"
check "no interface: available false" "$(has "$out" '"available":[[:space:]]*false')"
out=$(rpc wifi.scan null); check "no interface: scan is unsupported (6)" "$([ "$(code_of "$out")" = 6 ] && echo 1 || echo 0)"
out=$(rpc wifi.connect '{"ssid":"Home","passphrase":"whatever1"}')
check "no interface: connect is unsupported (6)" "$([ "$(code_of "$out")" = 6 ] && echo 1 || echo 0)"
out=$("$POS" wifi status 2>&1); check "pos wifi status says unavailable" "$(has "$out" 'unavailable (no_interface)')"
check "no interface: no supplicant was started" "$([ ! -e "$FAKE_WPA_RECORD" ] && echo 1 || echo 0)"

# ---- the interface appears; Wi-Fi is off until turned on ------------------------------
mkdir -p "$IFDIR/wireless"
check "interface appears: off by default" "$(wait_state off 3 && echo 1 || echo 0)"
out=$(rpc wifi.scan null); check "off: scan is refused by policy (3)" "$([ "$(code_of "$out")" = 3 ] && echo 1 || echo 0)"
check "off: still no supplicant" "$([ ! -e "$FAKE_WPA_RECORD" ] && echo 1 || echo 0)"
check "off: no store written just by starting" "$([ ! -e "$STORE" ] && echo 1 || echo 0)"
out=$(rpc wifi.set_enabled '{"enabled":"yes"}'); check "set_enabled with a string is invalid (2)" "$([ "$(code_of "$out")" = 2 ] && echo 1 || echo 0)"
out=$(rpc wifi.set_enabled '{"enabled":true,"extra":1}'); check "set_enabled with an unknown key is invalid (2)" "$([ "$(code_of "$out")" = 2 ] && echo 1 || echo 0)"

out=$("$POS" wifi on 2>&1); check "pos wifi on" "$(has "$out" 'Wi-Fi on')"
check "on: disconnected once the supplicant answers" "$(wait_state disconnected 5 && echo 1 || echo 0)"
check "on: the supplicant was started for wlan0" "$(grep -q 'started iface=wlan0' "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "on: remembered in the store" "$(grep -qx 'enabled=1' "$STORE" && echo 1 || echo 0)"
check "store directory is 0700" "$([ "$(stat -c %a "$POCKETOS_STATE_DIR/netd")" = 700 ] && echo 1 || echo 0)"
check "store file is 0600" "$([ "$(stat -c %a "$STORE")" = 600 ] && echo 1 || echo 0)"
check "the generated supplicant config holds no network" \
    "$(grep -qE 'network=|psk' "$POCKETOS_RUNTIME_DIR/netd/wpa_supplicant.conf" && echo 0 || echo 1)"
check "the generated supplicant config is 0600" \
    "$([ "$(stat -c %a "$POCKETOS_RUNTIME_DIR/netd/wpa_supplicant.conf")" = 600 ] && echo 1 || echo 0)"
out=$(st); check "status reports WPA3 not supported by this driver" "$(has "$out" '"wpa3_supported":[[:space:]]*false')"

# ---- scan ---------------------------------------------------------------------------------
out=$("$POS" wifi scan 2>&1)
check "pos wifi scan lists Home" "$(has "$out" ' Home$')"
check "duplicate SSIDs are one entry" "$([ "$(printf '%s\n' "$out" | grep -c ' Home$')" = 1 ] && echo 1 || echo 0)"
check "the strongest BSS is shown" "$(printf '%s\n' "$out" | grep ' Home$' | grep -q '^-40 ' && echo 1 || echo 0)"
check "a UTF-8 SSID is shown as text" "$(has "$out" 'Café ø')"
check "enterprise is listed as not supported" "$(printf '%s\n' "$out" | grep ' Corp$' | grep -q 'X' && echo 1 || echo 0)"
check "hidden networks are counted, not listed" "$(has "$out" '1 hidden')"
out=$(rpc wifi.networks null)
check "networks: strongest first" "$(printf '%s' "$out" | python3 -c 'import json,sys; n=json.load(sys.stdin)["result"]["networks"]; print(1 if n[0]["ssid"]=="Home" and n[0]["signal_dbm"]==-40 and n[0]["frequency_mhz"]==5180 else 0)')"
check "networks: exact bytes as hex" "$(has "$out" "\"ssid_hex\":[[:space:]]*\"$CAFE_HEX\"")"
check "networks: WPA3-only not supported without SAE" "$(printf '%s' "$out" | python3 -c 'import json,sys; n={x["ssid"]:x for x in json.load(sys.stdin)["result"]["networks"]}; print(1 if n["W3only"]["supported"] is False and n["W3only"]["security"]=="wpa3" else 0)')"
check "networks: open network needs no passphrase" "$(printf '%s' "$out" | python3 -c 'import json,sys; n={x["ssid"]:x for x in json.load(sys.stdin)["result"]["networks"]}; print(1 if n["Guest"]["needs_passphrase"] is False and n["Guest"]["security"]=="open" else 0)')"
check "networks: hidden_count 1" "$(has "$out" '"hidden_count":[[:space:]]*1')"

# ---- malformed input -----------------------------------------------------------------------
c() { code_of "$(rpc wifi.connect "$1")"; }
check "33-byte ssid is invalid" "$([ "$(c '{"ssid":"0123456789abcdef0123456789abcdefX","passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "empty ssid is invalid" "$([ "$(c '{"ssid":"","passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "ssid and ssid_hex together are invalid" "$([ "$(c '{"ssid":"Home","ssid_hex":"486f6d65"}')" = 2 ] && echo 1 || echo 0)"
check "no ssid at all is invalid" "$([ "$(c '{"passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "bad ssid_hex is invalid" "$([ "$(c '{"ssid_hex":"zz"}')" = 2 ] && echo 1 || echo 0)"
check "all-zero ssid_hex is invalid" "$([ "$(c '{"ssid_hex":"0000"}')" = 2 ] && echo 1 || echo 0)"
check "a number for ssid is invalid" "$([ "$(c '{"ssid":42}')" = 2 ] && echo 1 || echo 0)"
check "an unknown parameter is invalid" "$([ "$(c '{"ssid":"Home","passphrase":"abcdefgh","psk":"x"}')" = 2 ] && echo 1 || echo 0)"
check "a 7-character passphrase is invalid" "$([ "$(c '{"ssid":"Home","passphrase":"abcdefg"}')" = 2 ] && echo 1 || echo 0)"
check "a 64-character passphrase is invalid" "$([ "$(c "{\"ssid\":\"Home\",\"passphrase\":\"$(printf 'a%.0s' $(seq 1 64))\"}")" = 2 ] && echo 1 || echo 0)"
check "a 100 kB passphrase is invalid" "$([ "$(c "{\"ssid\":\"Home\",\"passphrase\":\"$(head -c 100000 /dev/zero | tr '\0' a)\"}")" = 2 ] && echo 1 || echo 0)"
check "a passphrase with a newline is invalid" "$([ "$(c '{"ssid":"Home","passphrase":"abcd\nSET_NETWORK 0 ssid 41"}')" = 2 ] && echo 1 || echo 0)"
check "a non-ASCII passphrase is invalid" "$([ "$(c '{"ssid":"Home","passphrase":"blåbærsyltetøy"}')" = 2 ] && echo 1 || echo 0)"
check "a passphrase that is not a string is invalid" "$([ "$(c '{"ssid":"Home","passphrase":12345678}')" = 2 ] && echo 1 || echo 0)"
check "a new WPA2 network without a passphrase is invalid" "$([ "$(c '{"ssid":"Home"}')" = 2 ] && echo 1 || echo 0)"
check "hidden without security is invalid" "$([ "$(c '{"ssid":"Secret","hidden":true,"passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "security without hidden is invalid" "$([ "$(c '{"ssid":"Home","security":"wpa2","passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "hidden with a string flag is invalid" "$([ "$(c '{"ssid":"Secret","hidden":"true","security":"wpa2"}')" = 2 ] && echo 1 || echo 0)"
check "a network not in the scan is refused" "$([ "$(c '{"ssid":"Nowhere","passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "enterprise is unsupported (6)" "$([ "$(c '{"ssid":"Corp","passphrase":"abcdefgh"}')" = 6 ] && echo 1 || echo 0)"
check "WPA3-only is unsupported on this driver (6)" "$([ "$(c '{"ssid":"W3only","passphrase":"abcdefgh"}')" = 6 ] && echo 1 || echo 0)"
check "WEP is unsupported (6)" "$([ "$(c '{"ssid":"Old","passphrase":"abcdefgh"}')" = 6 ] && echo 1 || echo 0)"
check "an open network is not joined silently (3)" "$([ "$(c '{"ssid":"Guest"}')" = 3 ] && echo 1 || echo 0)"
check "an open network takes no passphrase" "$([ "$(c '{"ssid":"Guest","allow_open":true,"passphrase":"abcdefgh"}')" = 2 ] && echo 1 || echo 0)"
check "forget of an unknown network is invalid" "$([ "$(code_of "$(rpc wifi.forget '{"ssid":"Nowhere"}')")" = 2 ] && echo 1 || echo 0)"
check "an unknown method is code 1" "$([ "$(code_of "$(rpc wifi.explode null)")" = 1 ] && echo 1 || echo 0)"
check "params that are not an object are invalid" "$([ "$(code_of "$(rpc wifi.scan '[1]')")" = 2 ] && echo 1 || echo 0)"
check "none of that reached the supplicant as a network" "$(grep -q 'cmd ADD_NETWORK' "$FAKE_WPA_RECORD" && echo 0 || echo 1)"
check "and nothing was stored" "$(grep -q '^network=' "$STORE" && echo 0 || echo 1)"
out=$(printf 'short\n' | "$POS" wifi connect Home 2>&1); rc=$?
check "pos wifi connect with a short passphrase fails" "$([ $rc -ne 0 ] && echo 1 || echo 0)"
check "and says why without echoing it" "$(all "$(has "$out" '8..63 printable ASCII')$(hasnt "$out" 'short')")"
out=$("$POS" wifi connect 2>&1); check "pos wifi connect without an SSID is a usage error" "$([ $? = 2 ] && echo 1 || echo 0)"

# ---- authentication failure ------------------------------------------------------------------
out=$(printf '%s\n' 'wrong-passphrase' | "$POS" wifi connect Home 2>&1); rc=$?
check "a wrong passphrase fails" "$([ $rc = 1 ] && echo 1 || echo 0)"
check "reported as auth_failed" "$(has "$out" 'failed: auth_failed')"
out=$(st)
check "status: failed auth_failed" "$(all "$(has "$out" '"state":[[:space:]]*"failed"')$(has "$out" '"reason":[[:space:]]*"auth_failed"')")"
check "the supplicant saw a wrong passphrase" "$(grep -q "psk_bad $HOME_HEX" "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "a failed join is not remembered" "$(grep -q "network=$HOME_HEX" "$STORE" && echo 0 || echo 1)"
check "and the attempt was removed from the supplicant" "$(grep -q 'cmd REMOVE_NETWORK' "$FAKE_WPA_RECORD" && echo 1 || echo 0)"

# ---- a join, with DHCP -------------------------------------------------------------------------
out=$(printf '%s\n' "$PASS" | "$POS" wifi connect Home 2>&1); rc=$?
check "the right passphrase joins" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "pos reports the address" "$(has "$out" 'connected to Home, ipv4 192.168.50.23')"
check "every awkward character reached the supplicant exactly" "$(grep -q "psk_ok $HOME_HEX key_mgmt=WPA-PSK" "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "the SSID was sent as hex" "$(grep -q "set ssid $HOME_HEX" "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
out=$(st)
check "status: connected" "$(has "$out" '"state":[[:space:]]*"connected"')"
check "status: ssid" "$(has "$out" '"ssid":[[:space:]]*"Home"')"
check "status: ipv4" "$(has "$out" '"ipv4":[[:space:]]*"192.168.50.23"')"
check "status: signal of the joined BSS" "$(all "$(has "$out" '"signal_dbm":[[:space:]]*-40')$(has "$out" '"signal_bars":[[:space:]]*4')")"
check "status: frequency" "$(has "$out" '"frequency_mhz":[[:space:]]*5180')"
check "status: security from the store" "$(has "$out" '"security":[[:space:]]*"wpa2"')"
check "the DHCP client runs on wlan0 in the foreground and releases on exit" \
    "$(grep -q 'udhcpc -f -R -i wlan0' "$TMP/dhcp.log" && echo 1 || echo 0)"
check "the joined network is remembered" "$(grep -q "^network=$HOME_HEX wpa2 0 $PASS_HEX\$" "$STORE" && echo 1 || echo 0)"
check "the store is still 0600" "$([ "$(stat -c %a "$STORE")" = 600 ] && echo 1 || echo 0)"
out=$("$POS" wifi saved 2>&1); check "pos wifi saved lists it" "$(has "$out" 'wpa2        Home')"
out=$(rpc wifi.saved null); check "wifi.saved carries no passphrase" "$(all "$(hasnt "$out" 'passphrase')$(hasnt "$out" "$PASS_HEX")")"
out=$(rpc wifi.networks null)
check "networks: Home connected and saved" "$(printf '%s' "$out" | python3 -c 'import json,sys; n={x["ssid"]:x for x in json.load(sys.stdin)["result"]["networks"]}; print(1 if n["Home"]["connected"] and n["Home"]["saved"] else 0)')"

# ---- the passphrase goes nowhere else ------------------------------------------------------------
check "no command line of any child carries the passphrase" \
    "$(for p in $(pgrep -f "$FAKE") $(pgrep -f "$TMP/udhcpc"); do tr '\0' ' ' < /proc/$p/cmdline; done | grep -qF "$PASS" && echo 0 || echo 1)"
check "the netd log never contains the passphrase" "$(grep -rqF "$PASS" "$POCKETOS_LOG_DIR" "$TMP/netd.stdio" && echo 0 || echo 1)"
check "nor its hex form" "$(grep -rqF "$PASS_HEX" "$POCKETOS_LOG_DIR" "$TMP/netd.stdio" && echo 0 || echo 1)"
check "the runtime directory never contains it" "$(grep -rqF "$PASS" "$POCKETOS_RUNTIME_DIR" 2>/dev/null && echo 0 || echo 1)"
check "the debug log records the psk field name only" "$(grep -q 'SET_NETWORK [0-9]* psk -> OK' "$POCKETOS_LOG_DIR/netd.log" && echo 1 || echo 0)"
check "the hex form is in exactly one file: the store" \
    "$([ "$(grep -rlF "$PASS_HEX" "$TMP" 2>/dev/null | grep -v '/scenario$' | wc -l)" = 1 ] && echo 1 || echo 0)"

# ---- a late reply is never taken for the next command's -------------------------------------------
# wpa_supplicant's replies carry no request id. A SCAN_RESULTS answered after netd stopped waiting
# (300 ms) used to arrive while netd waited for its next command and was read as that answer: a scan
# list has no wpa_state, so netd took the association as gone and stopped the DHCP client, dropping
# a working lease. late_scan makes the fake hold the answer and send it just before it answers the
# next command - that ordering every time, not when the timing happens to fall that way.
rel_before=$(grep -c released "$TMP/dhcp.log")
log_before=$(wc -l < "$POCKETOS_LOG_DIR/netd.log")
since() { tail -n +$((log_before + 1)) "$POCKETOS_LOG_DIR/netd.log"; }
echo late_scan >> "$FAKE_WPA_SCENARIO"
"$POS" wifi scan >/dev/null 2>&1
for _ in $(seq 1 60); do grep -q 'late SCAN_RESULTS sent before' "$FAKE_WPA_RECORD" && break; sleep 0.1; done
sed -i '/^late_scan$/d' "$FAKE_WPA_SCENARIO"
# Status polls after the late answer, so a misread has had its chance to act.
for _ in $(seq 1 50); do [ "$(since | grep -c 'STATUS -> ')" -ge 3 ] && break; sleep 0.1; done
check "the fake held a SCAN_RESULTS answer past netd's deadline" \
    "$(grep -q 'late SCAN_RESULTS held' "$FAKE_WPA_RECORD" && since | grep -q 'SCAN_RESULTS: no answer' && echo 1 || echo 0)"
check "and sent it while netd waited for its next command" \
    "$(grep -q 'late SCAN_RESULTS sent before' "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "to an address netd no longer listens on" \
    "$(grep -qE 'late SCAN_RESULTS sent before [A-Z_]+: undeliverable' "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "so no STATUS was answered with a scan list" \
    "$(since | grep -q 'STATUS -> bssid / frequency' && echo 0 || echo 1)"
check "netd kept polling the supplicant afterwards" "$([ "$(since | grep -c 'STATUS -> ')" -ge 3 ] && echo 1 || echo 0)"
out=$(st)
check "the association held through it" \
    "$(all "$(has "$out" '"state":[[:space:]]*"connected"')$(has "$out" '"ipv4":[[:space:]]*"192.168.50.23"')")"
check "and the lease was never released" "$([ "$(grep -c released "$TMP/dhcp.log")" = "$rel_before" ] && echo 1 || echo 0)"

# ---- disconnect ----------------------------------------------------------------------------------
out=$("$POS" wifi disconnect 2>&1); check "pos wifi disconnect" "$(has "$out" 'disconnected')"
check "status: disconnected" "$(wait_state disconnected 3 && echo 1 || echo 0)"
# The client is stopped asynchronously, so give it a moment to run its trap.
for _ in $(seq 1 30); do grep -q released "$TMP/dhcp.log" && break; sleep 0.1; done
check "the lease was released" "$(grep -q released "$TMP/dhcp.log" && echo 1 || echo 0)"
check "no address is reported" "$(has "$(st)" '"ipv4":[[:space:]]*null')"
sleep 2
check "a disconnect is not undone by auto-connect" "$([ "$(field "$(st)" state)" = disconnected ] && echo 1 || echo 0)"

# ---- rejoin a saved network without typing again ----------------------------------------------------
out=$(printf '\n' | "$POS" wifi connect Home 2>&1); rc=$?
check "an empty passphrase rejoins a saved network" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "with the saved passphrase" "$([ "$(grep -c "psk_ok $HOME_HEX" "$FAKE_WPA_RECORD")" -ge 2 ] && echo 1 || echo 0)"

# ---- DHCP failure and recovery -------------------------------------------------------------------------
echo fail > "$TMP/dhcp_mode"
"$POS" wifi disconnect >/dev/null 2>&1
out=$(printf '\n' | "$POS" wifi connect Home 2>&1); rc=$?
check "no lease: the join reports failure" "$([ $rc = 1 ] && echo 1 || echo 0)"
check "as dhcp_failed" "$(has "$out" 'failed: dhcp_failed')"
out=$(st)
check "dhcp_failed keeps the association visible" "$(all "$(has "$out" '"ssid":[[:space:]]*"Home"')$(has "$out" '"reason":[[:space:]]*"dhcp_failed"')")"
echo ok > "$TMP/dhcp_mode"
"$POS" wifi disconnect >/dev/null 2>&1
out=$(printf '\n' | "$POS" wifi connect Home 2>&1); rc=$?
check "a later join gets its address again" "$([ $rc = 0 ] && echo 1 || echo 0)"

# ---- open network, only when asked ---------------------------------------------------------------------
out=$("$POS" wifi connect Guest --open 2>&1); rc=$?
check "an open network joins with --open" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "as an open network" "$(grep -q "open_ok $GUEST_HEX" "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "remembered without a passphrase" "$(grep -q "^network=$GUEST_HEX open 0 -\$" "$STORE" && echo 1 || echo 0)"
check "newest first" "$(grep '^network=' "$STORE" | head -1 | grep -q "$GUEST_HEX" && echo 1 || echo 0)"

# ---- hidden network ---------------------------------------------------------------------------------------
out=$(printf '%s\n' "$PASS" | "$POS" wifi connect Secret --hidden wpa2 2>&1); rc=$?
check "a hidden network joins with --hidden" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "with scan_ssid set" "$(grep -q 'set scan_ssid 1' "$FAKE_WPA_RECORD" && echo 1 || echo 0)"
check "remembered as hidden" "$(grep -q '^network=536563726574 wpa2 1 ' "$STORE" && echo 1 || echo 0)"

# ---- forget ---------------------------------------------------------------------------------------------
out=$("$POS" wifi forget Guest 2>&1); check "pos wifi forget" "$(has "$out" 'forgot Guest')"
check "gone from the store" "$(grep -q "$GUEST_HEX" "$STORE" && echo 0 || echo 1)"
check "saved_count follows" "$(has "$(st)" '"saved_count":[[:space:]]*2')"

# ---- a UTF-8 SSID end to end ------------------------------------------------------------------------------
out=$(printf '%s\n' "$PASS" | "$POS" wifi connect 'Café ø' 2>&1); rc=$?
check "a UTF-8 SSID joins" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "and is stored as its exact bytes" "$(grep -q "^network=$CAFE_HEX wpa2 0 " "$STORE" && echo 1 || echo 0)"
out=$(printf '\n' | "$POS" wifi connect "$CAFE_HEX" --ssid-hex 2>&1); rc=$?
check "and can be named by hex" "$([ $rc = 0 ] && echo 1 || echo 0)"

# ---- restart: netd comes back on, and reconnects by itself --------------------------------------------------
stop_netd
sleep 0.5
check "stopping netd stops its supplicant" "$(pgrep -f "$FAKE" >/dev/null && echo 0 || echo 1)"
check "and its DHCP client" "$(pgrep -f "$TMP/udhcpc" >/dev/null && echo 0 || echo 1)"
check "the store survives" "$(grep -qx 'enabled=1' "$STORE" && echo 1 || echo 0)"
start_netd
check "after a restart netd reconnects to a saved network on its own" "$(wait_state connected 10 && echo 1 || echo 0)"
check "without a passphrase being entered" "$([ "$(grep -c 'set psk accepted' "$FAKE_WPA_RECORD")" -ge 1 ] && echo 1 || echo 0)"

# ---- the supplicant crashes ---------------------------------------------------------------------------------
before=$(grep -c 'started iface' "$FAKE_WPA_RECORD")
pkill -KILL -f "$FAKE"
sleep 0.5
check "a crashed supplicant is noticed" "$(grep -q 'wpa_supplicant was killed' "$POCKETOS_LOG_DIR/netd.log" && echo 1 || echo 0)"
check "and restarted, then reconnects" "$(wait_state connected 12 && echo 1 || echo 0)"
check "exactly one new supplicant" "$([ "$(grep -c 'started iface' "$FAKE_WPA_RECORD")" = $((before + 1)) ] && echo 1 || echo 0)"

# ---- the interface disappears and returns ---------------------------------------------------------------------
mv "$IFDIR" "$TMP/wlan0.away"
check "interface gone: unavailable" "$(wait_state unavailable 3 && echo 1 || echo 0)"
sleep 0.5
check "interface gone: the supplicant is stopped" "$(pgrep -f "$FAKE" >/dev/null && echo 0 || echo 1)"
mv "$TMP/wlan0.away" "$IFDIR"
check "interface back: reconnects" "$(wait_state connected 12 && echo 1 || echo 0)"

# ---- turning off --------------------------------------------------------------------------------------------------
out=$("$POS" wifi off 2>&1); check "pos wifi off" "$(has "$out" 'Wi-Fi off')"
check "off: state off" "$(wait_state off 3 && echo 1 || echo 0)"
sleep 0.5
check "off: supplicant gone" "$(pgrep -f "$FAKE" >/dev/null && echo 0 || echo 1)"
check "off: interface brought down" "$(grep -qx 0 "$IFDIR/netd_up" && echo 1 || echo 0)"
check "off: remembered" "$(grep -qx 'enabled=0' "$STORE" && echo 1 || echo 0)"
check "off: saved networks kept" "$(grep -c '^network=' "$STORE" | grep -qx 3 && echo 1 || echo 0)"

# ---- a wpa_supplicant netd did not start -------------------------------------------------------------------------
stop_netd
mkdir -p "$NETD_TEST_ROOT/proc/4242"
printf 'wpa_supplicant\0-B\0-i\0wlan0\0-c\0/tmp/wpa_supplicant.conf\0' > "$NETD_TEST_ROOT/proc/4242/cmdline"
sed -i 's/^enabled=0$/enabled=1/' "$STORE"
start_netd
check "a foreign supplicant on wlan0 makes Wi-Fi unavailable" "$(wait_state unavailable 4 && echo 1 || echo 0)"
check "reason interface_busy" "$(has "$(st)" '"reason":[[:space:]]*"interface_busy"')"
check "and netd does not start a second one" "$(pgrep -f "$FAKE" >/dev/null && echo 0 || echo 1)"
rm -rf "$NETD_TEST_ROOT/proc/4242"
check "once it is gone netd takes over" "$(wait_state connected 12 && echo 1 || echo 0)"

# ---- permissions found too open are corrected -------------------------------------------------------------------
stop_netd
chmod 0644 "$STORE"
start_netd
sleep 0.5
check "a store found readable by others is reported" "$(grep -q 'was readable by others' "$POCKETOS_LOG_DIR/netd.log" && echo 1 || echo 0)"
check "and corrected to 0600" "$([ "$(stat -c %a "$STORE")" = 600 ] && echo 1 || echo 0)"

# ---- a damaged store is set aside, never overwritten ----------------------------------------------------------------
stop_netd
printf 'version=1\nenabled=1\nnetwork=not-hex wpa2 0 zz\n' > "$STORE"
start_netd
check "damaged store: Wi-Fi starts off" "$(wait_state off 3 && echo 1 || echo 0)"
check "damaged store: reported" "$(has "$(st)" '"store":[[:space:]]*"damaged"')"
check "damaged store: file untouched until a change" "$(grep -q 'not-hex' "$STORE" && echo 1 || echo 0)"
"$POS" wifi on >/dev/null 2>&1
check "damaged store: kept aside on the first change" "$(grep -q 'not-hex' "$POCKETOS_STATE_DIR/netd/wifi.conf.damaged" && echo 1 || echo 0)"
check "damaged store: a valid store replaces it" "$(grep -qx 'version=1' "$STORE" && grep -qx 'enabled=1' "$STORE" && echo 1 || echo 0)"
check "damaged store: the copy aside is 0600" "$([ "$(stat -c %a "$POCKETOS_STATE_DIR/netd/wifi.conf.damaged")" = 600 ] && echo 1 || echo 0)"

# ---- a supplicant that cannot start ------------------------------------------------------------------------------------
stop_netd
FAKE_WPA_FAIL_START=1 start_netd
sleep 2.5
check "a supplicant that exits at start is retried with backoff" "$(grep -q 'wpa_supplicant exited (failure 1 of 5)' "$POCKETOS_LOG_DIR/netd.log" && echo 1 || echo 0)"
check "and netd itself keeps answering" "$(has "$(st)" '"api_version":[[:space:]]*0')"
stop_netd

# ---- protocol robustness ----------------------------------------------------------------------------------------------
start_netd
python3 - "$POCKETOS_RUNTIME_DIR/netd.sock" <<'PY' || true
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
body = b"{not json"
s.sendall(struct.pack(">I", len(body)) + body)
try:
    s.recv(10)
except Exception:
    pass
PY
check "a garbage frame does not take netd down" "$(has "$(st)" '"api_version":[[:space:]]*0')"
check "netd logged no crash" "$(ls "$POCKETOS_LOG_DIR"/crash-netd-* >/dev/null 2>&1 && echo 0 || echo 1)"
stop_netd

echo "netd_test: $checks checks, $failed failure(s)"
exit $((failed > 0))
