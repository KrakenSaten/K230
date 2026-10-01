#!/bin/bash
# Audio state after a pos-wave that was SIGKILLed: the regression test for the
# known issue that a killed helper left the route switched and the amplifier
# enabled (pocketaudio.h, "Recovery").
#
# Real pos-wave processes are killed mid-operation. Their sound card and GPIO
# are files (tests/pos-wave-testhooks, tests/fake_audio_backend.c), so what a
# killed process leaves behind can be read afterwards: "route" is the mixer
# switch, "amp" the amplifier enable line, and a released line keeps its
# value, as the kernel's does. The fake PCM runs in real time.
#
# Also checked here: the hardware gate still holds in the test build, the
# POCKETOS_AUDIO_ALLOW_UNVERIFIED direction words, and that the shipped
# pos-wave does not contain the hook.
#
# Requires: tests/pos-wave-testhooks and tools/wave/pos-wave (make test),
# flock (util-linux).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

H=$(pwd)/tests/pos-wave-testhooks
if [ ! -x "$H" ]; then
    echo "FAIL tests/pos-wave-testhooks is not built"
    echo "audio_recovery_test: 1 failure(s)"
    exit 1
fi
T=$(mktemp -d)
trap 'kill -KILL $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
export POS_WAVE_FAKE_AUDIO="$T"
export POCKETOS_RUNTIME_DIR="$T"
unset POCKETOS_AUDIO_ALLOW_UNVERIFIED

state() { cat "$T/$1" 2>/dev/null; }
reset_hw() { echo 1 > "$T/route"; echo 0 > "$T/amp"; rm -f "$T/log" "$T/audio.recovery"; }
wait_for() { # <file> <value> <ms>
    local n=0
    while [ "$(state "$1")" != "$2" ]; do
        n=$((n + 10)); [ "$n" -gt "$3" ] && return 1
        sleep 0.01
    done
}
wait_gone() { # <pid>
    local n=0
    while kill -0 "$1" 2>/dev/null; do
        n=$((n + 1)); [ "$n" -gt 300 ] && return 1
        sleep 0.01
    done
    wait "$1" 2>/dev/null
    return 0
}
lock_free() { flock -n "$T/audio.lock" true && echo 1 || echo 0; }
LONG=$(printf 'm%.0s' $(seq 1 64))

# ---- the test build still gates ----------------------------------------
reset_hw
"$H" info > "$T/info.out" 2>&1
check "the test build describes a K230-shaped board, gated" \
    "$(grep -q '^board fake-k230$' "$T/info.out" && grep -q '^playback not validated (gated)$' "$T/info.out" && echo 1 || echo 0)"
"$H" send --events --text DOORS > "$T/gate.out" 2>&1; rc=$?
check "the gate refuses playback without --allow-unverified, touching nothing" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_disabled' "$T/gate.out" && [ "$(state amp)" = 0 ] &&
       [ ! -e "$T/log" ] && [ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"

# ---- a send killed with the amplifier on -------------------------------
reset_hw
"$H" send --allow-unverified --protocol audible_normal --text "$LONG" > "$T/send1.out" 2>&1 &
pid=$!
wait_for amp 1 3000
check "send: the amplifier comes on" "$([ "$(state amp)" = 1 ] && echo 1 || echo 0)"
check "send: with a recovery record naming it" "$(grep -q '^amp_enabled 1$' "$T/audio.recovery" 2>/dev/null && echo 1 || echo 0)"
kill -KILL "$pid"
wait_gone "$pid" 2>/dev/null
check "SIGKILL: the process is gone" "$(kill -0 "$pid" 2>/dev/null && echo 0 || echo 1)"
check "SIGKILL: it left the amplifier on (the known issue, reproduced)" "$([ "$(state amp)" = 1 ] && echo 1 || echo 0)"
check "SIGKILL: its record survived it" "$([ -e "$T/audio.recovery" ] && echo 1 || echo 0)"
check "SIGKILL: the kernel released the lock" "$(lock_free)"

"$H" recover > "$T/recover1.out" 2>&1; rc=$?
check "recover: exits 0 and says it recovered" "$([ $rc -eq 0 ] && grep -q '^recovered$' "$T/recover1.out" && echo 1 || echo 0)"
check "recover: the amplifier is off" "$([ "$(state amp)" = 0 ] && echo 1 || echo 0)"
check "recover: the route is as the dead owner found it" "$([ "$(state route)" = 1 ] && echo 1 || echo 0)"
check "recover: and the record is gone" "$([ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"
lines=$(wc -l < "$T/log")
"$H" recover > "$T/recover2.out" 2>&1; rc=$?
check "recover again: nothing to do, nothing touched" \
    "$([ $rc -eq 0 ] && grep -q 'nothing to recover' "$T/recover2.out" && [ "$(wc -l < "$T/log")" = "$lines" ] && echo 1 || echo 0)"

# ---- a listen killed with the route on the microphone -----------------
reset_hw
"$H" listen --allow-unverified --seconds 60 > "$T/listen1.out" 2>&1 &
pid=$!
wait_for route 0 3000
check "listen: the route goes to the microphone" "$([ "$(state route)" = 0 ] && echo 1 || echo 0)"
kill -KILL "$pid"
wait_gone "$pid" 2>/dev/null
check "SIGKILL: the route is left on the microphone" "$([ "$(state route)" = 0 ] && echo 1 || echo 0)"
"$H" listen --allow-unverified --seconds 1 --events > "$T/listen2.out" 2>&1; rc=$?
check "next listen: restores first, and says so before it listens" \
    "$([ $rc -eq 0 ] && [ "$(grep -nm1 '^recovered$' "$T/listen2.out" | cut -d: -f1)" -lt \
       "$(grep -nm1 '^listening$' "$T/listen2.out" | cut -d: -f1)" ] && echo 1 || echo 0)"
check "next listen: ends with the route back and no record" \
    "$([ "$(state route)" = 1 ] && [ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"
check "next listen: the amplifier was never requested" "$(grep -q '^amp' "$T/log" && echo 0 || echo 1)"

# ---- a send killed, then the next send reconciles ---------------------
reset_hw
"$H" send --allow-unverified --protocol audible_normal --text "$LONG" > /dev/null 2>&1 &
pid=$!
wait_for amp 1 3000
kill -KILL "$pid"
wait_gone "$pid" 2>/dev/null
"$H" send --allow-unverified --events --protocol audible_fastest --text X > "$T/send2.out" 2>&1; rc=$?
check "next send: recovers, then sends" \
    "$([ $rc -eq 0 ] && grep -q '^recovered$' "$T/send2.out" && grep -q '^sent$' "$T/send2.out" && echo 1 || echo 0)"
check "next send: amplifier off at the end, no record" \
    "$([ "$(state amp)" = 0 ] && [ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"
check "next send: the stale amplifier went off before its own came on" \
    "$(grep '^amp' "$T/log" | tr '\n' '|' | grep -q '^amp request 1|amp request 0|amp release 0|amp request 1|amp set 0|amp release 0|$' && echo 1 || echo 0)"

# ---- no recovery over a live owner; a SIGTERM needs none ---------------
reset_hw
"$H" listen --allow-unverified --seconds 60 > "$T/listen3.out" 2>&1 &
pid=$!
wait_for route 0 3000
"$H" recover > "$T/recover3.out" 2>&1; rc=$?
check "recover while a live owner holds the audio: exit 3, audio_busy, route untouched" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_busy' "$T/recover3.out" && [ "$(state route)" = 0 ] && echo 1 || echo 0)"
kill -TERM "$pid"
wait_gone "$pid" 2>/dev/null
check "SIGTERM: the helper cleans up itself: route back, no record" \
    "$(grep -q '^stopped' "$T/listen3.out" && [ "$(state route)" = 1 ] && [ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"

# ---- a record this code did not write --------------------------------
reset_hw
printf 'pocketaudio-recovery 1\namp_chip /dev/sda\namp_line 2\namp_active_high 1\namp_enabled 1\n' > "$T/audio.recovery"
"$H" recover > "$T/recover4.out" 2>&1; rc=$?
check "a record naming something that is not a GPIO chip is discarded, not acted on" \
    "$([ $rc -eq 0 ] && grep -q 'discarded' "$T/recover4.out" && [ ! -e "$T/audio.recovery" ] &&
       [ ! -e "$T/log" ] && echo 1 || echo 0)"

# ---- the override names a direction -----------------------------------
reset_hw
POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture "$H" listen --events --seconds 1 > "$T/env1.out" 2>&1; rc=$?
check "POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture opens the microphone" "$([ $rc -eq 0 ] && grep -q '^listening$' "$T/env1.out" && echo 1 || echo 0)"
POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture "$H" send --events --text X > "$T/env2.out" 2>&1; rc=$?
check "and does not open the speaker" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_disabled' "$T/env2.out" && [ "$(state amp)" = 0 ] && echo 1 || echo 0)"
POCKETOS_AUDIO_ALLOW_UNVERIFIED=1 "$H" listen --events --seconds 1 > "$T/env3.out" 2>&1; rc=$?
check "=1 opens nothing" "$([ $rc -eq 3 ] && grep -q '^error audio_disabled' "$T/env3.out" && echo 1 || echo 0)"
POCKETOS_AUDIO_ALLOW_UNVERIFIED=captureplayback "$H" listen --events --seconds 1 > "$T/env4.out" 2>&1; rc=$?
check "a word that only contains a direction opens nothing" "$([ $rc -eq 3 ] && echo 1 || echo 0)"
POCKETOS_AUDIO_ALLOW_UNVERIFIED=playback,capture "$H" send --events --protocol audible_fastest --text X > "$T/env5.out" 2>&1; rc=$?
check "a list opens each direction it names" "$([ $rc -eq 0 ] && grep -q '^sent$' "$T/env5.out" && echo 1 || echo 0)"

# ---- the hook is not in the shipped binary ----------------------------
check "the test build carries the hook" "$(grep -aq POS_WAVE_FAKE_AUDIO "$H" && echo 1 || echo 0)"
check "the shipped pos-wave does not" \
    "$([ -x tools/wave/pos-wave ] && ! grep -aq POS_WAVE_FAKE_AUDIO tools/wave/pos-wave && echo 1 || echo 0)"

echo "audio_recovery_test: $failed failure(s)"
exit $((failed > 0))
