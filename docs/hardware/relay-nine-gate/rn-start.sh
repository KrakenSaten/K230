#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# PR #104 Relay Nine device session, unit B: hand the panel from the
# installed, supervised doors-shell to this kit's shell, which launches this
# kit's pos-audio-effects (DOORS_AUDIO_EFFECTS_HELPER) and writes frame
# timing (DOORS_RELAY_STATS) into ./log.
#   rn-start.sh <label> [slice]   slice: DOORS_RELAY_SLICE=1 (stairs + combat)
#
# Changes nothing under /usr, /etc/init.d or /boot, and installs nothing.
# The first start records the baseline rn-restore.sh returns to:
# settings.conf (hash-checked copy), the screenshot list, the ALSA mixer,
# the audio lock/recovery files and every file under /etc/pocketos and
# /var/lib/pocketos (hashes).
set -u
K=$(cd "$(dirname "$0")" && pwd)
. "$K/kit.env"
cd "$K" || exit 1
LABEL=${1:?label}; SLICE=${2:-}
fail() { echo "START: $*; nothing further changed" >&2; exit 1; }

echo "== kit"
sha256sum -c SHA256SUMS || fail "kit files do not match SHA256SUMS"
[ -s test-shell.pid ] && kill -0 "$(cat test-shell.pid)" 2>/dev/null && fail "a test shell is already running"

echo "== installed state"
echo "$ORIG_SHELL_SHA256  /usr/bin/doors-shell" | sha256sum -c - || fail "installed doors-shell is not $ORIG_SHELL_SHA256"
echo "$ORIG_LVGL_SHA256  /usr/lib/liblvgl.so.9.5.0" | sha256sum -c - || fail "installed liblvgl is not $ORIG_LVGL_SHA256"
echo "$ORIG_SETTINGS_SHA256  /etc/pocketos/settings.conf" | sha256sum -c - || fail "settings.conf changed since the kit was prepared"
# Another session owning the panel or the speaker is not ours to interrupt.
for p in $("$K/rn-status.sh" panel-owners); do
	[ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || fail "panel held by $p $(readlink /proc/$p/exe)"
done
[ -z "$("$K/rn-status.sh" audio-owners)" ] || fail "audio device held by $("$K/rn-status.sh" audio-owners)"
[ -z "$(pidof pos-audio-effects)" ] || fail "an effect helper is running: $(pidof pos-audio-effects)"

echo "== baseline"
mkdir -p backup log shots
if [ -s backup/settings.conf ]; then
	echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c - || fail "backup/settings.conf does not match; inspect it"
else
	cp -p /etc/pocketos/settings.conf backup/settings.conf && sync
	echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c - || fail "backup copy does not match"
	ls "$SHOTS_DIR" > backup/screenshots-before.txt 2>/dev/null
	amixer contents > backup/amixer-before.txt 2>&1
	for f in audio.lock audio.recovery; do
		if [ -e "$RUN_DIR/$f" ]; then echo "$f present $(wc -c < "$RUN_DIR/$f") bytes"; else echo "$f absent"; fi
	done > backup/audio-run-before.txt
	find /etc/pocketos /var/lib/pocketos -type f | sort | xargs sha256sum > backup/state-before.txt 2>/dev/null
	sync
fi
cat backup/audio-run-before.txt

echo "== stop the installed shell"
/etc/init.d/S90doors-shell stop || fail "S90doors-shell stop failed"
sleep 1
[ -z "$(pidof doors-shell pocketos-shell)" ] || fail "a shell is still running: $(pidof doors-shell pocketos-shell)"
ps | grep '[p]os-supervise doors-shell' && fail "the doors-shell supervisor is still running"
[ -z "$("$K/rn-status.sh" panel-owners)" ] || fail "something still holds /dev/dri/card0"

echo "== start the test shell ($LABEL${SLICE:+, slice})"
# The installed service's environment (S90doors-shell), logs kept apart.
env K230_LVGL_DRM_STAGING=1 POCKETOS_LOG_DIR="$K/log" POCKETOS_LOG_STDERR=0 \
	DOORS_AUDIO_EFFECTS_HELPER="$K/pos-audio-effects" DOORS_RELAY_STATS="$K/log/stats-$LABEL.txt" \
	${SLICE:+DOORS_RELAY_SLICE=1} \
	setsid "$K/doors-shell" > "$K/log/stdio-$LABEL.log" 2>&1 < /dev/null &
sleep 3
P=""
for p in $(pidof doors-shell); do [ "$(readlink /proc/$p/exe)" = "$K/doors-shell" ] && P=$p; done
[ -n "$P" ] || { echo "START: the test shell exited; log follows"; tail -30 "$K/log/stdio-$LABEL.log"; echo "Run ./rn-restore.sh"; exit 1; }
echo "$P" > "$K/test-shell.pid"
[ "$("$K/rn-status.sh" panel-owners)" = "$P" ] || echo "WARNING: panel owners are not exactly the test shell: $("$K/rn-status.sh" panel-owners)"
"$K/rn-status.sh"
echo "START: test shell $P is on the panel. Restore with ./rn-restore.sh"
