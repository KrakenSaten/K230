#!/bin/bash
# pos-record from the command line: usage refusals, the naming rule, never
# overwriting, a bench recording with --seconds, the three ways a recording
# is stopped (SIGTERM, the "stop" command, the app's end of stdin closing),
# private files whatever the umask, playback refusals, recover, and that the
# shipped helper carries none of the test hooks.
#
# Run from the repository root after `make` (tools/recorder/pos-record and
# tests/pos-record-testhooks). No sound card is used: the test-hooks helper's
# is files (tests/fake_audio_backend.c).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

SHIP=tools/recorder/pos-record
HOOK=tests/pos-record-testhooks
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/rec" "$T/audio" "$T/run"
echo 1 > "$T/audio/route"
echo 10000000000 > "$T/free"
# Two seconds of a tone for the microphone.
python3 - "$T/audio/capture.raw" <<'PY'
import math, struct, sys
with open(sys.argv[1], "wb") as f:
    f.write(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000 * 8)))
PY
export POS_RECORD_FAKE_AUDIO="$T/audio" POS_RECORD_FREE_FILE="$T/free" POCKETOS_RUNTIME_DIR="$T/run"
export POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture,playback

# ---- the shipped helper -----------------------------------------------------
check "the shipped helper is built" "$([ -x "$SHIP" ] && echo 1 || echo 0)"
check "it carries none of the test hooks" \
    "$(grep -qaE 'POS_RECORD_FAKE_AUDIO|POS_RECORD_FREE_FILE|POS_RECORD_FAIL_AFTER' "$SHIP" && echo 0 || echo 1)"
out=$(POCKETOS_AUDIO_BOARD=generic "$SHIP" info); rc=$?
check "info opens nothing and says the formats and limits" \
    "$([ $rc = 0 ] && echo "$out" | grep -q '^formats voice=16000 standard=48000 mono s16le wav$' &&
       echo "$out" | grep -q '^max_seconds voice=67108 standard=22369$' && echo 1 || echo 0)"
"$SHIP" >/dev/null 2>&1; rc=$?
check "no command is a usage error" "$([ $rc = 2 ] && echo 1 || echo 0)"
out=$("$SHIP" record --events 2>&1); rc=$?
check "record without a file is refused" "$([ $rc = 2 ] && echo "$out" | grep -q '^error usage' && echo 1 || echo 0)"
out=$("$SHIP" record --rate 44100 "$T/rec/REC-0001.wav" 2>&1); rc=$?
check "a rate other than 16000 or 48000 is refused" "$([ $rc = 2 ] && echo 1 || echo 0)"
out=$("$SHIP" record --seconds 0 "$T/rec/REC-0001.wav" 2>&1); rc=$?
check "zero seconds is refused" "$([ $rc = 2 ] && echo 1 || echo 0)"
for bad in "$T/rec/notes.wav" "$T/rec/../REC-0001.txt" "$T/rec/REC-0001.wav.part" "$T/rec/.REC-0001.wav"; do
    "$SHIP" record "$bad" >/dev/null 2>&1; rc=$?
    check "a file that is not a recording name is refused ($(basename "$bad"))" "$([ $rc = 2 ] && echo 1 || echo 0)"
done
check "and nothing was created" "$([ -z "$(ls -A "$T/rec")" ] && echo 1 || echo 0)"

# ---- recording with the test hooks -----------------------------------------
echo "theirs" > "$T/rec/REC-0001.wav"
out=$("$HOOK" record "$T/rec/REC-0001.wav" 2>&1); rc=$?
check "an existing recording is never overwritten" \
    "$([ $rc = 4 ] && echo "$out" | grep -q 'exists' && [ "$(cat "$T/rec/REC-0001.wav")" = theirs ] && echo 1 || echo 0)"

start=$(date +%s%N)
out=$(umask 000; "$HOOK" record --seconds 2 "$T/rec/REC-0002.wav" < /dev/null 2>&1); rc=$?
ms=$(( ($(date +%s%N) - start) / 1000000 ))
check "a two-second bench recording stops by itself and is saved (${ms} ms)" \
    "$([ $rc = 0 ] && echo "$out" | grep -q '^limit time$' && echo "$out" | grep -q '^saved [0-9]* [0-9]* 0 REC-0002.wav$' &&
       [ $ms -lt 4000 ] && echo 1 || echo 0)"
check "without --events, stdin is not read (it was /dev/null)" "$(echo "$out" | grep -q '^saved' && echo 1 || echo 0)"
check "the file is 0600 even under umask 000" "$([ "$(stat -c %a "$T/rec/REC-0002.wav")" = 600 ] && echo 1 || echo 0)"
check "and its header is a 16 kHz mono WAV" \
    "$(python3 - "$T/rec/REC-0002.wav" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
ok = d[:4] == b"RIFF" and d[8:16] == b"WAVEfmt " and struct.unpack("<HHIIHH", d[20:36]) == (1, 1, 16000, 32000, 2, 16) \
    and d[36:40] == b"data" and struct.unpack("<I", d[40:44])[0] == len(d) - 44 \
    and struct.unpack("<I", d[4:8])[0] == len(d) - 8 and 16000 < (len(d) - 44) // 2 < 16000 * 3
print(1 if ok else 0)
PY
)"

# SIGTERM.
mkfifo "$T/in"
("$HOOK" record --events "$T/rec/REC-0003.wav" < "$T/in" > "$T/out3" 2>&1; echo "rc=$?" >> "$T/out3") &
exec 7> "$T/in"
sleep 1.2
pkill -TERM -f "record --events $T/rec/REC-0003.wav"
wait_for() { for _ in $(seq 1 100); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.05; done; return 1; }
wait_for "$T/out3" "^rc="
check "SIGTERM stops a recording: saved, stopped, exit 0" \
    "$(grep -q '^saved .* REC-0003.wav$' "$T/out3" && grep -q '^stopped$' "$T/out3" && grep -q '^rc=0$' "$T/out3" &&
       [ -f "$T/rec/REC-0003.wav" ] && [ ! -e "$T/rec/REC-0003.wav.part" ] && echo 1 || echo 0)"
exec 7>&-
rm -f "$T/in"

# The stop command.
mkfifo "$T/in"
("$HOOK" record --events "$T/rec/REC-0004.wav" < "$T/in" > "$T/out4" 2>&1; echo "rc=$?" >> "$T/out4") &
exec 7> "$T/in"
sleep 1.2
echo stop >&7
wait_for "$T/out4" "^rc="
check "the stop command stops it the same way" \
    "$(grep -q '^saved .* REC-0004.wav$' "$T/out4" && grep -q '^rc=0$' "$T/out4" && echo 1 || echo 0)"
exec 7>&-
rm -f "$T/in"

# The app's end closing (it crashed, or was killed).
mkfifo "$T/in"
("$HOOK" record --events "$T/rec/REC-0005.wav" < "$T/in" > "$T/out5" 2>&1; echo "rc=$?" >> "$T/out5") &
exec 7> "$T/in"
sleep 1.2
exec 7>&-
wait_for "$T/out5" "^rc="
check "a closed stdin (the app is gone) stops and saves it" \
    "$(grep -q '^saved .* REC-0005.wav$' "$T/out5" && grep -q '^rc=0$' "$T/out5" && echo 1 || echo 0)"
rm -f "$T/in"
check "the microphone is closed after all of it" "$([ "$(cat "$T/audio/pcm")" = closed ] && echo 1 || echo 0)"
check "no recording helper is left" "$(pgrep -f "pos-record-testhooks record" >/dev/null && echo 0 || echo 1)"

# ---- playback and recover ---------------------------------------------------
out=$("$HOOK" play "$T/rec/missing.wav" 2>&1); rc=$?
check "playing a missing file is a storage error, before any device" \
    "$([ $rc = 5 ] && echo "$out" | grep -q '^error storage' && [ "$(cat "$T/audio/amp" 2>/dev/null || echo 0)" = 0 ] &&
       echo 1 || echo 0)"
echo "not a wav" > "$T/rec/junk.wav"
out=$("$HOOK" play "$T/rec/junk.wav" 2>&1); rc=$?
check "playing a file that is not a WAV is a format error" "$([ $rc = 5 ] && echo "$out" | grep -q '^error format' && echo 1 || echo 0)"
out=$("$HOOK" play --volume-percent 30 "$T/rec/REC-0002.wav" 2>&1); rc=$?
check "a recording plays to its end at a system volume" \
    "$([ $rc = 0 ] && echo "$out" | grep -q '^playing [0-9]* 16000 1$' && echo "$out" | grep -q '^played$' && echo 1 || echo 0)"
out=$("$HOOK" recover 2>&1); rc=$?
check "recover with nothing to recover exits 0" "$([ $rc = 0 ] && echo 1 || echo 0)"
out=$("$HOOK" recover --events --dir "$T/nonexistent" 2>&1); rc=$?
check "recover of a folder that does not exist is not an error" "$([ $rc = 0 ] && echo 1 || echo 0)"

echo "rec_tool_test: $failed failure(s)"
exit $((failed > 0))
