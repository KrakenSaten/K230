#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Undo vn-start.sh: stop the test shell and its pos-vision, start the
# installed supervised doors-shell, and verify against the baseline that the
# installed files, /etc/pocketos, Vision's settings, DeskBuddy's preferences
# and any owner data are what they were, that no camera helper is left and
# that meshcored and radiod kept their processes. Safe to run more than once,
# and after a half-finished start.
set -u
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
rc=0

echo "== stop the test shell"
"$K/vn-stop-test.sh"
[ -z "$(pidof pos-vision)" ] || { echo "RESTORE: a pos-vision still runs: $(pidof pos-vision)" >&2; rc=1; }

echo "== start the installed shell"
if [ -z "$(pidof doors-shell pocketos-shell)" ] && [ -z "$("$K/vn-status.sh" panel-owners)" ]; then
	/etc/init.d/S90doors-shell start || rc=1
elif [ -n "$(pidof doors-shell)" ] && [ "$(readlink /proc/$(pidof doors-shell)/exe)" = /usr/bin/doors-shell ]; then
	echo "the installed shell already runs"
else
	echo "RESTORE: a shell or panel owner is still running; installed shell NOT started" >&2
	rc=1
fi
sleep 4

echo "== installed files"
if [ -s backup/installed.sha256 ]; then
	sha256sum -c backup/installed.sha256 || { echo "RESTORE: an installed file changed" >&2; rc=1; }
else
	echo "no baseline (start did not get that far)"
fi

echo "== preserved files"
if [ -s backup/preserved.txt ]; then
	for f in /etc/pocketos/settings.conf /var/lib/pocketos/vision/settings.v1 /var/lib/pocketos/vision/owner.v1 \
	         /var/lib/pocketos/deskbuddy/prefs.v1 /var/lib/pocketos/deskbuddy/guard.v1; do
		if [ -e "$f" ]; then echo "present $(sha256sum "$f")"; else echo "absent $f"; fi
	done > log/preserved-after.txt
	diff backup/preserved.txt log/preserved-after.txt && echo "preserved: identical (existence and content)" ||
		{ echo "RESTORE: a preserved file differs" >&2; rc=1; }
	find /etc/pocketos /var/lib/pocketos -type f | sort | xargs sha256sum > log/state-after.txt 2>/dev/null
	diff backup/state-before.txt log/state-after.txt > log/state-diff.txt && echo "state: identical to baseline" || {
		cat log/state-diff.txt
		# Services other than the shell write their own files (logs, radio
		# state); /etc/pocketos must match.
		grep -q ' /etc/pocketos/' log/state-diff.txt && { echo "RESTORE: /etc/pocketos differs" >&2; rc=1; }
	}
fi

echo "== services"
[ -s backup/services-before.txt ] && { [ "$(pidof meshcored) $(pidof radiod)" = "$(cat backup/services-before.txt)" ] &&
	echo "meshcored and radiod: the same processes" || { echo "RESTORE: meshcored or radiod changed" >&2; rc=1; }; }

echo "== the shell on the panel"
"$K/vn-status.sh"
n=0; for p in $(pidof doors-shell); do n=$((n+1)); [ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || rc=1; done
[ $n -eq 1 ] || { echo "expected exactly one doors-shell, found $n" >&2; rc=1; }
P=$(pidof doors-shell)
tr '\0' '\n' < /proc/$P/environ 2>/dev/null | grep -qE '^POCKETOS_(VISION|CONFIG|STATE)' && { echo "installed shell has test overrides" >&2; rc=1; }
[ "$("$K/vn-status.sh" panel-owners)" = "$P" ] || { echo "panel owner is not the installed shell" >&2; rc=1; }
ps | grep -q '[p]os-supervise doors-shell' || { echo "installed shell is not supervised" >&2; rc=1; }
[ -z "$(pidof pos-vision pos-camera)" ] || { echo "a camera helper is running: $(pidof pos-vision pos-camera)" >&2; rc=1; }
[ $rc -eq 0 ] && echo "RESTORE: OK - installed doors-shell supervised, installed files and preserved settings unchanged, no test shell or camera helper" ||
	echo "RESTORE: CHECK THE LINES ABOVE (rc=$rc)"
exit $rc
