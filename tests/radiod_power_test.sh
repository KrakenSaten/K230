#!/bin/bash
# radiod's radio on/off choice (radio.set_enabled, docs/api/radio.md "Radio on
# and off"): the default with nothing stored, what is refused while off, the
# profile kept across off and on, the choice kept across restarts, and the
# two ways storing it can fail. Mock backend; `--radio-default off` stands in
# for the sx1262 default so the policy is exercised without hardware.
# Run from the repository root after `make all`.
set -u

RADIOD=${RADIOD:-services/radiod/radiod}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR POCKETOS_STATE_DIR POCKETOS_LOG_DIR
TMP=$(mktemp -d)
POCKETOS_RUNTIME_DIR=$TMP/run
POCKETOS_STATE_DIR=$TMP/state
POCKETOS_LOG_DIR=$TMP/log
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_STATE_DIR" "$POCKETOS_LOG_DIR"
CONF=$POCKETOS_STATE_DIR/radiod/radio.conf
failed=0
RADIOD_PID=""

check() { # check <name> <expected-substring> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -20
        failed=$((failed + 1))
    fi
}
check_not() { # check_not <name> <unexpected-substring> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: did not expect '$2' in:"; printf '%s\n' "$3" | head -20
        failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

start() { # start [radiod args...]
    rm -f "$POCKETOS_RUNTIME_DIR/radiod.sock"
    "$RADIOD" --backend mock "$@" >> "$TMP/radiod.out" 2>&1 &
    RADIOD_PID=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && return 0; sleep 0.1; done
    echo "FAIL radiod did not start ($*)"; cat "$TMP/radiod.out"; failed=$((failed + 1))
    return 1
}
stop() {
    [ -n "$RADIOD_PID" ] && kill "$RADIOD_PID" 2>/dev/null && wait "$RADIOD_PID" 2>/dev/null
    RADIOD_PID=""
}
trap 'stop; rm -rf "$TMP"' EXIT

# ---- the mock's own default, nothing stored -------------------------------
start
out=$("$POS" radio status)
check "mock with nothing stored starts on" '"enabled":[[:space:]]*true' "$out"
check "mock with nothing stored receives" '"state":[[:space:]]*"rx"' "$out"
check "nothing stored: no file written at start" "absent" "$([ -e "$CONF" ] && echo present || echo absent)"
stop

# ---- the sx1262 policy: off when nothing is stored -------------------------
start --radio-default off
out=$("$POS" radio status)
check "fresh default off: state off" '"state":[[:space:]]*"off"' "$out"
check "fresh default off: enabled false" '"enabled":[[:space:]]*false' "$out"
check "a fresh start does not store a choice" "absent" "$([ -e "$CONF" ] && echo present || echo absent)"
check "the start is logged as parked" "transceiver parked" "$(cat "$POCKETOS_LOG_DIR/radiod.log")"

out=$("$POS" radio send 01020304 2>&1)
check "send refused while off (code 3)" 'code 3' "$out"
check "the refusal says why" 'switched off' "$out"
out=$("$POS" radio send-async 0102 2>&1)
check "send_async refused while off" 'code 3' "$out"
out=$("$POS" radio cad 2>&1)
check "cad refused while off" 'code 3' "$out"
out=$("$POS" radio rssi 2>&1)
check "rssi refused while off" 'code 3' "$out"
out=$("$POS" radio inject 0102 2>&1)
check "mock injection refused while off" 'code 3' "$out"
out=$("$POS" radio mock rx_failing=1 2>&1)
check "mock knobs refused while off" 'code 3' "$out"
out=$("$POS" radio channel)
check "channel answers while off, not receiving" '"receiving":[[:space:]]*false' "$out"
check "channel: rssi unknown while off" '"rssi_known":[[:space:]]*false' "$out"
out=$("$POS" radio stats)
check "stats answer while off" '"tx_packets":[[:space:]]*0' "$out"
out=$("$POS" radio info)
check "info answers while off" '"backend":[[:space:]]*"mock"' "$out"

# A protocol daemon reconnecting while the radio is off configures it: the
# profile is validated and kept, not refused (a refusal is final for it).
out=$("$POS" radio configure frequency_mhz=869.618 bandwidth_khz=62.5 spreading_factor=8 2>&1)
check "configure accepted while off" '"spreading_factor":[[:space:]]*8' "$out"
out=$("$POS" radio configure frequency_mhz=915 2>&1)
check "region guard still applies while off" 'code 3' "$out"
out=$("$POS" radio status)
check "profile kept while off" '"frequency_mhz":[[:space:]]*869.618' "$out"
check "still off after configure" '"state":[[:space:]]*"off"' "$out"

out=$("$POS" call radiod radio.set_enabled enabled=yes 2>&1)
check "set_enabled needs a boolean (code 2)" 'code 2' "$out"

# Events: a subscriber sees the switch.
"$POS" radio listen 3 > "$TMP/events.txt" 2>&1 &
LISTEN=$!
sleep 0.5

# ---- on ------------------------------------------------------------------
out=$("$POS" radio on)
check "on: state rx" '"state":[[:space:]]*"rx"' "$out"
check "on: enabled true" '"enabled":[[:space:]]*true' "$out"
check "on: the profile configured while off is on the radio" '"frequency_mhz":[[:space:]]*869.618' "$out"
check "on: stored" 'enabled=1' "$(cat "$CONF" 2>/dev/null)"
out=$("$POS" radio on)
check "on again: idempotent" '"state":[[:space:]]*"rx"' "$out"
out=$("$POS" radio send 0102)
check "send works once on" '"bytes":[[:space:]]*2' "$out"

# A transmit on the air is finished, not cut: off is refused until it is done.
"$POS" radio mock tx_delay_ms=1500 > /dev/null
"$POS" radio send-async 0a0b > /dev/null
out=$("$POS" radio off 2>&1)
check "off during a transmit is refused as busy (code 5)" 'code 5' "$out"
sleep 2
"$POS" radio mock tx_delay_ms=0 > /dev/null

# ---- off -----------------------------------------------------------------
out=$("$POS" radio off)
check "off: state off" '"state":[[:space:]]*"off"' "$out"
check "off: profile retained" '"spreading_factor":[[:space:]]*8' "$out"
check "off: stored" 'enabled=0' "$(cat "$CONF" 2>/dev/null)"
out=$("$POS" radio off)
check "off again: idempotent" '"enabled":[[:space:]]*false' "$out"
wait "$LISTEN" 2>/dev/null
ev=$(cat "$TMP/events.txt")
check "event: radio.state rx on switch-on" '"state":[[:space:]]*"rx"' "$ev"
check "event: radio.state off on switch-off" '"state":[[:space:]]*"off"' "$ev"

# The lease does not guard the switch: the owner wins over the lease holder.
"$POS" radio on > /dev/null
timeout 3 "$POS" radio acquire holder > /dev/null 2>&1 &
HOLDER=$!
sleep 0.5
out=$("$POS" radio lease)
check "a daemon holds the lease" '"held":[[:space:]]*true' "$out"
out=$("$POS" radio off)
check "off wins over the lease holder" '"state":[[:space:]]*"off"' "$out"
out=$("$POS" radio lease)
check "the lease survives off" '"held":[[:space:]]*true' "$out"
wait "$HOLDER" 2>/dev/null
stop

# ---- restarts keep the choice ---------------------------------------------
start --radio-default off
out=$("$POS" radio status)
check "restart while off: still off" '"state":[[:space:]]*"off"' "$out"
"$POS" radio on > /dev/null
stop
start --radio-default off
out=$("$POS" radio status)
check "restart after on: stored on wins over the default" '"state":[[:space:]]*"rx"' "$out"
stop
start --radio-default on
"$POS" radio off > /dev/null
stop
start --radio-default on
out=$("$POS" radio status)
check "restart after off: stored off wins over --radio-default on" '"state":[[:space:]]*"off"' "$out"
stop
start
out=$("$POS" radio status)
check "stored off wins over the mock default too" '"state":[[:space:]]*"off"' "$out"
stop

# SIGKILL while on: the supervisor's restart finds the stored choice.
"$RADIOD" --backend mock --radio-default off >> "$TMP/radiod.out" 2>&1 &
RADIOD_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && break; sleep 0.1; done
"$POS" radio on > /dev/null
kill -KILL "$RADIOD_PID"; wait "$RADIOD_PID" 2>/dev/null; RADIOD_PID=""
start --radio-default off
out=$("$POS" radio status)
check "after SIGKILL: the stored on is found" '"state":[[:space:]]*"rx"' "$out"
stop

# ---- a file that says neither -----------------------------------------------
printf 'garbage\nenabled=maybe\n' > "$CONF"
start --radio-default off
out=$("$POS" radio status)
check "unreadable choice: the default applies" '"state":[[:space:]]*"off"' "$out"
check "unreadable choice: a warning is logged" "says neither on nor off" "$(cat "$POCKETOS_LOG_DIR/radiod.log")"
stop
: > "$CONF"
start --radio-default on
out=$("$POS" radio status)
check "empty choice file: the default applies" '"state":[[:space:]]*"rx"' "$out"
stop

# ---- storing fails -------------------------------------------------------------
rm -rf "$POCKETOS_STATE_DIR/radiod"
printf 'not a directory\n' > "$POCKETOS_STATE_DIR/radiod"
start --radio-default off
out=$("$POS" radio on 2>&1)
check "on that cannot be stored is refused (code 4)" 'code 4' "$out"
check "the refusal says it stays off" 'stays off' "$out"
out=$("$POS" radio status)
check "an unstorable on leaves the radio off" '"state":[[:space:]]*"off"' "$out"
stop
start --radio-default on
out=$("$POS" radio off 2>&1)
check "off that cannot be stored is reported (code 4)" 'code 4' "$out"
out=$("$POS" radio status)
check "but the radio is off all the same" '"state":[[:space:]]*"off"' "$out"
stop

# ---- the switch is not a lease-only operation and --radio-default is checked --
out=$("$RADIOD" --backend mock --radio-default maybe 2>&1); rc=$?
check "--radio-default takes on or off only" "2" "$rc"

if [ "$failed" -ne 0 ]; then
    echo "radiod_power_test: $failed failure(s)"
    cat "$TMP/radiod.out" | tail -20
    exit 1
fi
echo "radiod_power_test: all passed"
