#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Undo rn-start.sh: stop the test shell and any effect helper of this kit,
# let the kit's helper undo an audio owner's leftovers if one was left
# behind, put settings.conf back, move screenshots taken during the session
# into ./shots, compare the mixer and the state files with the baseline,
# start the installed supervised doors-shell and verify the result.
# Safe to run more than once, and after a half-finished start.
set -u
K=$(cd "$(dirname "$0")" && pwd)
. "$K/kit.env"
cd "$K" || exit 1
rc=0

echo "== stop the test shell"
stop_exe() { # stop_exe <exe>: every process running exactly that file
	for p in $(pidof "$(basename "$1")"); do
		[ "$(readlink /proc/$p/exe)" = "$1" ] || continue
		kill "$p"; i=0
		while kill -0 "$p" 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
		kill -0 "$p" 2>/dev/null && { echo "$p not gone after 10 s, SIGKILL"; kill -KILL "$p"; sleep 1; }
	done
}
stop_exe "$K/doors-shell"
rm -f test-shell.pid
stop_exe "$K/pos-audio-effects"
[ -z "$(pidof pos-audio-effects)" ] || { echo "RESTORE: an effect helper still runs: $(pidof pos-audio-effects)" >&2; rc=1; }

echo "== audio owner"
if [ -e "$RUN_DIR/audio.recovery" ] && grep -q '^audio.recovery absent' backup/audio-run-before.txt 2>/dev/null; then
	echo "an audio recovery record was left behind; undoing it"
	"$K/pos-audio-effects" recover || rc=1
fi
for f in audio.lock audio.recovery; do
	if [ -e "$RUN_DIR/$f" ]; then echo "$f present $(wc -c < "$RUN_DIR/$f") bytes"; else echo "$f absent"; fi
done > log/audio-run-after.txt
diff backup/audio-run-before.txt log/audio-run-after.txt || { echo "RESTORE: audio run files differ" >&2; rc=1; }
[ -z "$("$K/rn-status.sh" audio-owners)" ] || { echo "RESTORE: audio still held" >&2; rc=1; }
amixer contents > log/amixer-after.txt 2>&1
diff backup/amixer-before.txt log/amixer-after.txt > /dev/null && echo "mixer: identical to baseline" ||
	{ echo "RESTORE: the ALSA mixer differs from the baseline" >&2; rc=1; }

echo "== settings.conf"
/etc/init.d/S90doors-shell stop
if [ -s backup/settings.conf ]; then
	if echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c -; then
		cp -p backup/settings.conf /etc/pocketos/settings.conf.tmp && mv -f /etc/pocketos/settings.conf.tmp /etc/pocketos/settings.conf
		sync
	else
		echo "RESTORE: backup does not match; settings.conf NOT restored" >&2; rc=1
	fi
else
	echo "no backup (start did not get that far); settings.conf left as it is"
fi
echo "$ORIG_SETTINGS_SHA256  /etc/pocketos/settings.conf" | sha256sum -c - || rc=1

echo "== screenshots taken during the session"
if [ -f backup/screenshots-before.txt ]; then
	mkdir -p shots
	ls "$SHOTS_DIR" 2>/dev/null | while read -r f; do
		grep -qxF "$f" backup/screenshots-before.txt || { mv "$SHOTS_DIR/$f" shots/ && echo "moved $f"; }
	done
	ls "$SHOTS_DIR" 2>/dev/null | diff backup/screenshots-before.txt - > /dev/null || { echo "RESTORE: screenshot list differs" >&2; rc=1; }
fi

echo "== state files"
if [ -f backup/state-before.txt ]; then
	find /etc/pocketos /var/lib/pocketos -type f | sort | xargs sha256sum > log/state-after.txt 2>/dev/null
	diff backup/state-before.txt log/state-after.txt > log/state-diff.txt && echo "state: identical to baseline" || {
		cat log/state-diff.txt
		# Services other than the shell write their own files; /etc/pocketos must match.
		grep -q ' /etc/pocketos/' log/state-diff.txt && { echo "RESTORE: /etc/pocketos differs" >&2; rc=1; }
	}
fi

echo "== installed shell"
echo "$ORIG_SHELL_SHA256  /usr/bin/doors-shell" | sha256sum -c - || rc=1
echo "$ORIG_LVGL_SHA256  /usr/lib/liblvgl.so.9.5.0" | sha256sum -c - || rc=1
if [ -z "$(pidof doors-shell pocketos-shell)" ] && [ -z "$("$K/rn-status.sh" panel-owners)" ]; then
	/etc/init.d/S90doors-shell start || rc=1
else
	echo "RESTORE: a shell or panel owner is still running; installed shell NOT started" >&2
	rc=1
fi
sleep 3
"$K/rn-status.sh"
n=0; for p in $(pidof doors-shell); do n=$((n+1)); [ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || rc=1; done
[ $n -eq 1 ] || { echo "expected exactly one doors-shell, found $n" >&2; rc=1; }
P=$(pidof doors-shell)
tr '\0' '\n' < /proc/$P/environ 2>/dev/null | grep -qE '^(DOORS_|LD_LIBRARY_PATH)' && { echo "installed shell has test overrides" >&2; rc=1; }
[ "$("$K/rn-status.sh" panel-owners)" = "$P" ] || { echo "panel owner is not the installed shell" >&2; rc=1; }
ps | grep -q '[p]os-supervise doors-shell' || { echo "installed shell is not supervised" >&2; rc=1; }
[ $rc -eq 0 ] && echo "RESTORE: OK - installed doors-shell supervised, original settings, no test shell or helper" ||
	echo "RESTORE: CHECK THE LINES ABOVE (rc=$rc)"
exit $rc
