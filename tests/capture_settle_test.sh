#!/bin/bash
# The K230 codec/capture startup transient, end to end through a real
# pos-wave (pocketaudio.h, "Clean capture"): what the microphone delivers in
# the first 500 ms of a capture never reaches the ggwave decoder, the level
# meter or a recording; the first sample that does is the one after the
# window; and a SIGTERM (the app's STOP), a SIGKILL or a device failure during
# the discard leaves the audio as it was found.
#
# The microphone is a file (tests/fake_audio_backend.c, capture.raw) paced in
# real time, and the test board declares the same 500 ms discard as the K230.
# It is not a ggwave delay: the decoder is never told about it.
#
# Requires: tests/pos-wave-testhooks (make test), perl, flock.
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
    echo "capture_settle_test: 1 failure(s)"
    exit 1
fi
T=$(mktemp -d)
trap 'kill -KILL $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
export POS_WAVE_FAKE_AUDIO="$T"
export POCKETOS_RUNTIME_DIR="$T"
unset POCKETOS_AUDIO_ALLOW_UNVERIFIED

state() { cat "$T/$1" 2>/dev/null; }
reset_hw() { echo 1 > "$T/route"; echo 0 > "$T/amp"; rm -f "$T/log" "$T/audio.recovery" "$T/capture.raw" "$T/capture_fail_after"; }
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
        n=$((n + 1)); [ "$n" -gt 500 ] && return 1
        sleep 0.01
    done
    wait "$1" 2>/dev/null
    return 0
}
lock_free() { flock -n "$T/audio.lock" true && echo 1 || echo 0; }
clean() { # the audio exactly as a capture must leave it
    [ "$(state route)" = 1 ] && [ ! -e "$T/audio.recovery" ] && [ "$(state pcm)" = closed ] &&
        [ "$(lock_free)" = 1 ] && [ "$(state amp)" = 0 ] && echo 1 || echo 0
}
samples() { perl -e 'print pack("s", $ARGV[0]) x $ARGV[1]' -- "$1" "$2"; } # <value> <count>
ms_now() { date +%s%3N; }
SETTLE=24000

"$H" info > "$T/info.out" 2>&1
check "the test board declares the K230's 500 ms capture discard" \
    "$(grep -q '^capture_settle_ms 500$' "$T/info.out" && echo 1 || echo 0)"
"$H" encode --text DOORS "$T/doors.wav" > /dev/null 2>&1
tail -c +45 "$T/doors.wav" > "$T/doors.raw"
check "a DOORS transmission to feed the microphone" "$([ -s "$T/doors.raw" ] && echo 1 || echo 0)"
check "and a 500 ms full-scale transient to put in front of it" \
    "$([ "$(samples -32768 $SETTLE | wc -c)" = $((SETTLE * 2)) ] && echo 1 || echo 0)"

# ---- the transient is dropped, the message after it decoded ------------
reset_hw
{ samples -32768 $SETTLE; cat "$T/doors.raw"; samples 0 48000; } > "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 4 > "$T/l1.out" 2>&1; rc=$?
check "a message after a full-scale transient is decoded" \
    "$([ $rc -eq 0 ] && grep -q '^received 444f4f5253$' "$T/l1.out" && echo 1 || echo 0)"
max=$(sed -n 's/^level //p' "$T/l1.out" | sort -n | tail -1)
check "no level ever reports the transient (loudest $max)" "$([ -n "$max" ] && [ "$max" -lt 20 ] && echo 1 || echo 0)"
check "and the capture ends clean" "$(clean)"

# ---- the first sample passed on is the first after the window ---------
reset_hw
{ samples -32768 $SETTLE; samples 1000 480; samples 0 24000; } > "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 1 > "$T/l2.out" 2>&1; rc=$?
first=$(sed -n 's/^level //p' "$T/l2.out" | head -1)
check "the first level is the 10 ms right after the window: not the transient, not skipped past (level $first)" \
    "$([ $rc -eq 0 ] && [ "$first" = 3 ] && echo 1 || echo 0)"

# ---- a message inside the window is never decoded ---------------------
reset_hw
{ cat "$T/doors.raw"; samples 0 48000; } > "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 3 > "$T/l3.out" 2>&1; rc=$?
check "a message that starts inside the window never reaches the decoder" \
    "$([ $rc -eq 0 ] && ! grep -q '^received' "$T/l3.out" && echo 1 || echo 0)"

# ---- a recording starts after the window too ---------------------------
reset_hw
{ samples -32768 $SETTLE; samples 1000 48000; } > "$T/capture.raw"
"$H" record --allow-unverified --seconds 1 "$T/rec.wav" > "$T/rec.out" 2>&1; rc=$?
check "a recording holds no transient sample: one second, peak 1000" \
    "$([ $rc -eq 0 ] && grep -q '^samples 48000$' "$T/rec.out" && grep -q '^peak 1000$' "$T/rec.out" && echo 1 || echo 0)"
check "and ends clean" "$(clean)"

# ---- SIGTERM (the app's STOP) during the discard -----------------------
reset_hw
samples -32768 144000 > "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 30 > "$T/l4.out" 2>&1 &
pid=$!
wait_for pcm capture 3000
t0=$(ms_now)
kill -TERM "$pid"
wait_gone "$pid"
took=$(( $(ms_now) - t0 ))
check "SIGTERM during the discard: stopped within one wait (${took} ms)" \
    "$(grep -q '^stopped$' "$T/l4.out" && [ "$took" -lt 700 ] && echo 1 || echo 0)"
check "SIGTERM during the discard: nothing was heard" "$(grep -qE '^(level|received)' "$T/l4.out" && echo 0 || echo 1)"
check "SIGTERM during the discard: route back, record gone, PCM closed, lock free" "$(clean)"

# ---- SIGKILL during the discard ------------------------------------------
reset_hw
samples -32768 144000 > "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 30 > "$T/l5.out" 2>&1 &
pid=$!
wait_for pcm capture 3000
kill -KILL "$pid"
wait_gone "$pid"
check "SIGKILL during the discard: the route is left on the microphone, with its record" \
    "$([ "$(state route)" = 0 ] && [ -e "$T/audio.recovery" ] && echo 1 || echo 0)"
check "SIGKILL during the discard: the kernel released the lock" "$(lock_free)"
"$H" recover > "$T/r5.out" 2>&1; rc=$?
check "recover after it: route back and no record" \
    "$([ $rc -eq 0 ] && grep -q '^recovered$' "$T/r5.out" && [ "$(state route)" = 1 ] && [ ! -e "$T/audio.recovery" ] && echo 1 || echo 0)"
rm -f "$T/capture.raw"
"$H" listen --allow-unverified --events --seconds 1 > "$T/l5b.out" 2>&1; rc=$?
check "and the next capture runs normally" "$([ $rc -eq 0 ] && grep -q '^listening$' "$T/l5b.out" && ! grep -q '^recovered' "$T/l5b.out" && echo 1 || echo 0)"
check "and ends clean" "$(clean)"

# ---- a device failure during the discard --------------------------------
reset_hw
echo 4800 > "$T/capture_fail_after"
"$H" listen --allow-unverified --events --seconds 5 > "$T/l6.out" 2>&1; rc=$?
check "a device failure during the discard: exit 3 with an audio error" \
    "$([ $rc -eq 3 ] && grep -q '^error audio capture failed' "$T/l6.out" && echo 1 || echo 0)"
check "a device failure during the discard: nothing was heard" "$(grep -qE '^(level|received)' "$T/l6.out" && echo 0 || echo 1)"
check "a device failure during the discard: cleaned up like any other close" "$(clean)"

check "no helper is left running" "$([ -z "$(jobs -pr)" ] && echo 1 || echo 0)"

echo "capture_settle_test: $failed failure(s)"
exit $((failed > 0))
