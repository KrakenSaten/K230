#!/bin/bash
# pos-wave, Wave's audio helper, on a host with no audio hardware.
#
# The modem path end to end through files (encode -> WAV -> decode), every
# input the WAV reader must refuse, the message rules, and the audio path
# against ALSA's null device: the event sequence, the hardware gate on the
# K230 board description, a missing device, a held lock, a stop by SIGTERM,
# and a parent that goes away. Nothing here can make a sound or open a
# microphone: the only PCM used is "null", and the K230 cases fail before any
# device is opened, which is what they check.
#
# Requires: tools/wave/pos-wave (make all), python3 (to write test WAVs),
# flock (util-linux).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${POS_WAVE:-tools/wave/pos-wave}
if [ ! -x "$BIN" ]; then
    echo "FAIL pos-wave is not built: $BIN"
    echo "wave_tool_test: 1 failure(s)"
    exit 1
fi
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
RUN="$T/run"
mkdir -p "$RUN"
export POCKETOS_RUNTIME_DIR="$RUN"
unset POCKETOS_AUDIO_ALLOW_UNVERIFIED
# The null device and the generic board unless a case says otherwise.
export POCKETOS_AUDIO_BOARD=generic
export POCKETOS_AUDIO_PCM=null

has() { grep -q -- "$2" "$1" && echo 1 || echo 0; }

# ---- files: encode and decode ------------------------------------------

echo DOORS | "$BIN" encode "$T/doors.wav" > "$T/enc.out" 2> "$T/enc.err"; rc=$?
check "encode: DOORS from stdin exits 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "encode: 1.194 s of audio" "$(has "$T/enc.out" '^duration_ms 1194$')"
check "encode: peak 3192 at the default volume" "$(has "$T/enc.out" '^peak 3192$')"
check "encode: the WAV is a 44-byte header and 57344 mono S16 samples" \
    "$([ "$(stat -c %s "$T/doors.wav" 2>/dev/null)" = "$((44 + 57344 * 2))" ] && echo 1 || echo 0)"
"$BIN" encode --text DOORS "$T/doors2.wav" > /dev/null 2>&1
check "encode: --text gives the same file as stdin" "$(cmp -s "$T/doors.wav" "$T/doors2.wav" && echo 1 || echo 0)"

"$BIN" decode "$T/doors.wav" > "$T/dec.out" 2> "$T/dec.err"; rc=$?
check "decode: exits 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "decode: the message comes back as hex" "$(has "$T/dec.out" '^received 444f4f5253$')"
check "decode: and as text" "$(has "$T/dec.out" '^text DOORS$')"
check "decode: exactly one message" "$([ "$(grep -c '^received' "$T/dec.out")" = 1 ] && echo 1 || echo 0)"
check "decode: nothing on stderr, so no payload can reach a log" "$([ ! -s "$T/dec.err" ] && echo 1 || echo 0)"

for p in audible_normal audible_fastest; do
    "$BIN" encode --protocol $p --text "profile $p" "$T/$p.wav" > /dev/null 2>&1
    "$BIN" decode "$T/$p.wav" > "$T/$p.out" 2>&1
    check "decode: $p round trip" "$(has "$T/$p.out" "^text profile $p\$")"
done

max=$(printf 'm%.0s' $(seq 1 64))
"$BIN" encode --protocol audible_normal --text "$max" "$T/max.wav" > "$T/max.enc" 2>&1; rc=$?
"$BIN" decode "$T/max.wav" > "$T/max.out" 2>&1
check "limit: 64 bytes encode and decode" "$([ $rc -eq 0 ] && grep -q "^text $max\$" "$T/max.out" && echo 1 || echo 0)"
check "limit: and take 6.634 s on the slowest profile" "$(has "$T/max.enc" '^duration_ms 6634$')"
"$BIN" encode --text "${max}x" "$T/over.wav" > "$T/over.out" 2>&1; rc=$?
check "limit: 65 bytes are refused (exit 2, too_long)" \
    "$([ $rc -eq 2 ] && grep -q '^error too_long' "$T/over.out" && [ ! -e "$T/over.wav" ] && echo 1 || echo 0)"
printf '%s\n' "${max}x" | "$BIN" encode "$T/over2.wav" > "$T/over2.out" 2>&1; rc=$?
check "limit: 65 bytes on stdin are refused, not cut" "$([ $rc -eq 2 ] && grep -q '^error too_long' "$T/over2.out" && echo 1 || echo 0)"
printf '' | "$BIN" encode "$T/e.wav" > "$T/empty.out" 2>&1; rc=$?
check "text: an empty message is refused" "$([ $rc -eq 2 ] && grep -q '^error invalid_text' "$T/empty.out" && echo 1 || echo 0)"
printf 'DOORS\n\n' | "$BIN" encode "$T/e.wav" > "$T/nl.out" 2>&1; rc=$?
check "text: only one trailing newline is forgiven" "$([ $rc -eq 2 ] && grep -q '^error invalid_text' "$T/nl.out" && echo 1 || echo 0)"
printf 'A\tB' | "$BIN" encode "$T/e.wav" > "$T/ctl.out" 2>&1; rc=$?
check "text: a control character is refused" "$([ $rc -eq 2 ] && echo 1 || echo 0)"
printf 'A\xc3' | "$BIN" encode "$T/e.wav" > "$T/utf.out" 2>&1; rc=$?
check "text: broken UTF-8 is refused" "$([ $rc -eq 2 ] && grep -q '^error invalid_text' "$T/utf.out" && echo 1 || echo 0)"
"$BIN" encode --volume 26 --text x "$T/e.wav" > "$T/vol.out" 2>&1; rc=$?
check "volume above 25 is refused" "$([ $rc -eq 2 ] && grep -q '^error usage' "$T/vol.out" && echo 1 || echo 0)"
"$BIN" encode --protocol ultrasound_fast --text x "$T/e.wav" > "$T/pro.out" 2>&1; rc=$?
check "an ultrasound protocol is not offered" "$([ $rc -eq 2 ] && echo 1 || echo 0)"

# ---- files the reader must refuse or survive ---------------------------

python3 - "$T" <<'PY'
import struct, sys, wave
t = sys.argv[1]
def w(name, rate, width, ch, frames):
    f = wave.open(f"{t}/{name}", "wb")
    f.setnchannels(ch); f.setsampwidth(width); f.setframerate(rate)
    f.writeframes(frames); f.close()
w("silence.wav", 48000, 2, 1, b"\0\0" * 48000)
w("u8.wav", 48000, 1, 1, b"\x80" * 4800)
w("rate44k.wav", 44100, 2, 1, b"\0\0" * 4410)
# IEEE float: the wave module cannot write it, so by hand.
data = struct.pack("<4800f", *([0.0] * 4800))
hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, 48000, 192000, 4, 32) + b"data" + struct.pack("<I", len(data))
open(f"{t}/float.wav", "wb").write(hdr + data)
open(f"{t}/notwav.wav", "wb").write(b"this is not a wave file at all" * 10)
# The encoded message on the right channel only, silence on the left.
m = wave.open(f"{t}/doors.wav", "rb"); mono = m.readframes(m.getnframes()); m.close()
st = bytearray()
for i in range(0, len(mono), 2):
    st += b"\0\0" + mono[i:i+2]
w("stereo_right.wav", 48000, 2, 2, bytes(st))
# Cut off at 60 %, header left claiming the whole message.
raw = open(f"{t}/doors.wav", "rb").read()
open(f"{t}/truncated.wav", "wb").write(raw[: 44 + int((len(raw) - 44) * 0.6) // 2 * 2])
PY

"$BIN" decode "$T/silence.wav" > "$T/sil.out" 2>&1; rc=$?
check "decode: silence gives no message (exit 1)" "$([ $rc -eq 1 ] && ! grep -q '^received' "$T/sil.out" && echo 1 || echo 0)"
"$BIN" decode "$T/u8.wav" > "$T/u8.out" 2>&1; rc=$?
check "decode: 8-bit PCM is an unsupported sample format (exit 2)" \
    "$([ $rc -eq 2 ] && grep -q 'unsupported sample format (8-bit' "$T/u8.out" && echo 1 || echo 0)"
"$BIN" decode "$T/float.wav" > "$T/fl.out" 2>&1; rc=$?
check "decode: IEEE float is an unsupported sample format (exit 2)" \
    "$([ $rc -eq 2 ] && grep -q 'unsupported sample format (format code 3' "$T/fl.out" && echo 1 || echo 0)"
"$BIN" decode "$T/rate44k.wav" > "$T/r44.out" 2>&1; rc=$?
check "decode: 44.1 kHz is refused, not resampled (exit 2)" \
    "$([ $rc -eq 2 ] && grep -q 'unsupported sample rate' "$T/r44.out" && echo 1 || echo 0)"
"$BIN" decode "$T/notwav.wav" > "$T/nw.out" 2>&1; rc=$?
check "decode: a file that is not a WAV is refused (exit 2)" \
    "$([ $rc -eq 2 ] && grep -q 'not a RIFF/WAVE' "$T/nw.out" && echo 1 || echo 0)"
"$BIN" decode "$T/does-not-exist.wav" > "$T/nx.out" 2>&1; rc=$?
check "decode: a missing file is refused (exit 2)" "$([ $rc -eq 2 ] && echo 1 || echo 0)"
"$BIN" decode "$T/truncated.wav" > "$T/tr.out" 2>&1; rc=$?
check "decode: truncated PCM decodes nothing and does not crash (exit 1)" \
    "$([ $rc -eq 1 ] && ! grep -q '^received' "$T/tr.out" && echo 1 || echo 0)"
check "decode: and says the file was cut short" "$(has "$T/tr.out" 'ended before its declared size')"
"$BIN" decode --channel 1 "$T/stereo_right.wav" > "$T/sr1.out" 2>&1; rc=$?
check "decode: a stereo capture's right channel (the K230 mic slot) decodes" \
    "$([ $rc -eq 0 ] && grep -q '^text DOORS$' "$T/sr1.out" && echo 1 || echo 0)"
"$BIN" decode --channel 0 "$T/stereo_right.wav" > "$T/sr0.out" 2>&1; rc=$?
check "decode: its silent left channel does not" "$([ $rc -eq 1 ] && echo 1 || echo 0)"
"$BIN" decode --channel 1 "$T/doors.wav" > "$T/ch.out" 2>&1; rc=$?
check "decode: a channel the file does not have is refused" "$([ $rc -eq 2 ] && echo 1 || echo 0)"

# ---- the audio path, against ALSA's null device ------------------------

"$BIN" info > "$T/info.out" 2>&1; rc=$?
check "info: exits 0 and opens nothing" "$([ $rc -eq 0 ] && [ ! -e "$RUN/audio.lock" ] && echo 1 || echo 0)"
check "info: names the board and the ceiling" \
    "$(grep -q '^board generic$' "$T/info.out" && grep -q '^peak_ceiling 8192$' "$T/info.out" &&
       grep -q '^capture_settle_ms 0$' "$T/info.out" && echo 1 || echo 0)"
POCKETOS_AUDIO_BOARD=k230 "$BIN" info > "$T/info2.out" 2>&1
check "info: both K230 paths are validated" \
    "$(grep -q '^playback validated$' "$T/info2.out" && grep -q '^capture validated$' "$T/info2.out" &&
       grep -q '^amplifier /dev/gpiochip1 line 2 active-high$' "$T/info2.out" && echo 1 || echo 0)"
check "info: the K230 capture discards its 500 ms startup transient" \
    "$(grep -q '^capture_settle_ms 500$' "$T/info2.out" && echo 1 || echo 0)"

printf 'DOORS' | "$BIN" send --events > "$T/send.out" 2>&1; rc=$?
check "send (null): exits 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "send (null): ready, sending 1444 ms, sent, in that order" \
    "$([ "$(tr '\n' '|' < "$T/send.out")" = "ready generic|sending 1444|sent|" ] && echo 1 || echo 0)"
check "send (null): the lock is released afterwards" \
    "$(flock -n "$RUN/audio.lock" true && echo 1 || echo 0)"

"$BIN" listen --events --seconds 2 > "$T/listen.out" 2>&1; rc=$?
check "listen (null): exits 0 after its seconds" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "listen (null): says it is listening, hears nothing" \
    "$(grep -q '^listening$' "$T/listen.out" && ! grep -q '^received' "$T/listen.out" && echo 1 || echo 0)"

"$BIN" record --seconds 1 "$T/rec.wav" > "$T/rec.out" 2>&1; rc=$?
check "record (null): one second of mono S16 at 48 kHz" \
    "$([ $rc -eq 0 ] && [ "$(stat -c %s "$T/rec.wav" 2>/dev/null)" = "$((44 + 48000 * 2))" ] && echo 1 || echo 0)"
"$BIN" record --seconds 31 "$T/rec31.wav" > "$T/rec31.out" 2>&1; rc=$?
check "record: more than 30 seconds is refused before the device is opened" \
    "$([ $rc -eq 2 ] && [ ! -e "$T/rec31.wav" ] && ! grep -q '^ready' "$T/rec31.out" && echo 1 || echo 0)"
"$BIN" record "$T/rec5.wav" > /dev/null 2>&1
check "record: 5 seconds by default" "$([ "$(stat -c %s "$T/rec5.wav" 2>/dev/null)" = "$((44 + 5 * 48000 * 2))" ] && echo 1 || echo 0)"

rm -f "$RUN/audio.lock"
# Both K230 paths are validated, so without any override they get past the
# gate and stop at the device, which this host does not have. The gate itself
# is tested on a K230-shaped unvalidated board in tests/audio_recovery_test.sh.
POCKETOS_AUDIO_BOARD=k230 "$BIN" send --events --text DOORS > "$T/gate.out" 2>&1; rc=$?
check "gate: the validated K230 speaker is not refused - with no K230 card here it is no device" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_nodev' "$T/gate.out" && ! grep -q 'audio_disabled' "$T/gate.out" && echo 1 || echo 0)"
check "gate: and nothing was announced as started" "$(grep -qE '^(ready|sending)' "$T/gate.out" && echo 0 || echo 1)"
POCKETOS_AUDIO_BOARD=k230 "$BIN" record --seconds 1 "$T/gated.wav" > "$T/gate0.out" 2>&1; rc=$?
check "gate: the validated K230 microphone is not refused - with no K230 card here it is no device" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_nodev' "$T/gate0.out" && ! grep -q 'audio_disabled' "$T/gate0.out" &&
       [ ! -e "$T/gated.wav" ] && echo 1 || echo 0)"
POCKETOS_AUDIO_BOARD=k230 "$BIN" listen --events --seconds 1 > "$T/gate2.out" 2>&1; rc=$?
check "gate: and a K230 listen the same, never listening" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_nodev' "$T/gate2.out" && ! grep -q '^listening' "$T/gate2.out" && echo 1 || echo 0)"
POCKETOS_AUDIO_BOARD=k230 POCKETOS_AUDIO_PCM= "$BIN" send --events --allow-unverified --text DOORS > "$T/nocard.out" 2>&1; rc=$?
check "gate: allowed, but no K230 card on this host, is no device (exit 3)" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_nodev' "$T/nocard.out" && echo 1 || echo 0)"

POCKETOS_AUDIO_PCM="hw:CARD=NoSuchCard,DEV=0" "$BIN" listen --events --seconds 1 > "$T/nodev.out" 2>&1; rc=$?
check "missing device: exit 3, audio_nodev, never listening" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_nodev' "$T/nodev.out" && ! grep -q '^listening' "$T/nodev.out" && echo 1 || echo 0)"

# The holder is waited for, not killed: killing flock would leave its child
# holding the lock for the cases after this one.
flock -n "$RUN/audio.lock" sleep 1 &
holder=$!
sleep 0.3
printf 'DOORS' | "$BIN" send --events > "$T/busy.out" 2>&1; rc=$?
check "busy: another owner of the audio lock means exit 3, audio_busy" \
    "$([ $rc -eq 3 ] && grep -q '^error audio_busy' "$T/busy.out" && echo 1 || echo 0)"
wait "$holder" 2>/dev/null
check "busy: the lock is free once its owner has gone" "$(flock -n "$RUN/audio.lock" true && echo 1 || echo 0)"

"$BIN" listen --events --seconds 3600 > "$T/stop.out" 2>&1 &
pid=$!
sleep 0.3
start=$(date +%s%N)
kill -TERM "$pid"
wait "$pid"; rc=$?
ms=$(( ($(date +%s%N) - start) / 1000000 ))
check "stop: SIGTERM ends a listen, exit 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "stop: and it says stopped" "$(has "$T/stop.out" '^stopped$')"
check "stop: within one second ($ms ms)" "$([ "$ms" -lt 1000 ] && echo 1 || echo 0)"
check "stop: the lock is free again" "$(flock -n "$RUN/audio.lock" true && echo 1 || echo 0)"

start=$(date +%s)
timeout 10 bash -c "\"$BIN\" listen --events --seconds 3600 | head -n 1 > \"$T/gone.out\""
rc=$?
check "parent gone: a listen whose reader went away ends by itself" "$([ $rc -ne 124 ] && echo 1 || echo 0)"
check "parent gone: and the lock is free" "$(flock -n "$RUN/audio.lock" true && echo 1 || echo 0)"

echo "wave_tool_test: $failed failure(s)"
exit $((failed > 0))
