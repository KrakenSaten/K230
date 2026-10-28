#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Undo install.sh: put back the doors-shell and pos-vision it backed up,
# restart only the shell service and check the unit against the baseline.
# Safe to run more than once, and after a half-finished install.
set -u
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
rc=0
[ -s backup/copies.sha256 ] || { echo "RESTORE: no backup here; nothing changed" >&2; exit 1; }
(cd backup && sha256sum -c copies.sha256) || { echo "RESTORE: the backup copies are damaged; nothing changed" >&2; exit 1; }

echo "== stop the shell"
/etc/init.d/S90doors-shell stop || rc=1
sleep 1
for p in $(pidof pos-vision); do kill "$p" 2>/dev/null; done
sleep 1

echo "== put back"
for b in doors-shell pos-vision; do
	if [ -e "backup/$b" ]; then
		cp "backup/$b" "/usr/bin/.$b.old" && chmod 0755 "/usr/bin/.$b.old" && mv "/usr/bin/.$b.old" "/usr/bin/$b" || rc=1
	elif grep -qx "/usr/bin/$b" backup/absent.txt; then
		rm -f "/usr/bin/$b"
	fi
done
sync

echo "== start the shell"
/etc/init.d/S90doors-shell start || rc=1
sleep 5

echo "== installed files"
sha256sum -c backup/installed.sha256 || { echo "RESTORE: an installed file differs from the baseline" >&2; rc=1; }

echo "== preserved files"
for f in /etc/pocketos/settings.conf /var/lib/pocketos/vision/settings.v1 /var/lib/pocketos/vision/owner.v1 \
         /var/lib/pocketos/deskbuddy/prefs.v1 /var/lib/pocketos/deskbuddy/guard.v1; do
	if [ -e "$f" ]; then echo "present $(sha256sum "$f")"; else echo "absent $f"; fi
done > log/preserved-after.txt
diff backup/preserved.txt log/preserved-after.txt && echo "preserved: identical (existence and content)" ||
	echo "RESTORE: note - a preserved file changed while the integration ran (the owner's own use?)"
find /etc/pocketos /etc/default /var/lib/pocketos -type f | sort | xargs sha256sum > log/state-after.txt 2>/dev/null
diff backup/state-before.txt log/state-after.txt > log/state-diff.txt && echo "state: identical to baseline" || cat log/state-diff.txt

echo "== services"
[ "$(pidof meshcored) $(pidof radiod) $(pidof netd) $(pidof sysd)" = "$(cat backup/services-before.txt)" ] &&
	echo "meshcored, radiod, netd, sysd: the same processes" || { echo "RESTORE: another service changed" >&2; rc=1; }
n=0; for p in $(pidof doors-shell); do n=$((n+1)); [ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || rc=1; done
[ $n -eq 1 ] || { echo "expected exactly one doors-shell, found $n" >&2; rc=1; }
ps | grep -q '[p]os-supervise doors-shell' || { echo "the shell is not supervised" >&2; rc=1; }
[ -z "$(pidof pos-vision pos-camera)" ] || { echo "a camera helper is running" >&2; rc=1; }
[ $rc -eq 0 ] && echo "RESTORE: OK - previous doors-shell and pos-vision back, supervised, installed files match the baseline" ||
	echo "RESTORE: CHECK THE LINES ABOVE (rc=$rc)"
exit $rc
