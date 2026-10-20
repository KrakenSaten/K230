#!/bin/sh
# Undo neon-start.sh: stop the test shell, put settings.conf back from the
# hash-checked backup, start the installed supervised doors-shell through its
# init script, and verify the result. Safe to run more than once, and safe to
# run when neon-start.sh failed half way.
set -u
K=$(cd "$(dirname "$0")" && pwd)
. "$K/kit.env"
cd "$K" || exit 1
rc=0

echo "== stop the test shell"
if [ -s test-shell.pid ]; then
	P=$(cat test-shell.pid)
	if kill -0 "$P" 2>/dev/null && [ "$(readlink /proc/$P/exe)" = "$K/doors-shell" ]; then
		kill "$P"
		i=0; while kill -0 "$P" 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
		kill -0 "$P" 2>/dev/null && { echo "not gone after 10 s, SIGKILL"; kill -KILL "$P"; sleep 1; }
	fi
	rm -f test-shell.pid
fi
for p in $(pidof doors-shell); do
	[ "$(readlink /proc/$p/exe)" = "$K/doors-shell" ] && { echo "stray test shell $p, SIGKILL"; kill -KILL "$p"; sleep 1; }
done

echo "== settings.conf"
# The file is written by the shell, so it is only replaced with no shell
# running - also after a reboot mid-session, when the installed one is back.
/etc/init.d/S90doors-shell stop
if [ -s backup/settings.conf ]; then
	echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c - || { echo "RESTORE: backup does not match; settings.conf NOT restored" >&2; rc=1; }
	if [ $rc -eq 0 ]; then
		cp -p backup/settings.conf /etc/pocketos/settings.conf.tmp && mv -f /etc/pocketos/settings.conf.tmp /etc/pocketos/settings.conf
		sync
	fi
else
	echo "no backup (start did not get that far); settings.conf left as it is"
fi
echo "$ORIG_SETTINGS_SHA256  /etc/pocketos/settings.conf" | sha256sum -c - || rc=1

echo "== installed shell"
echo "$ORIG_SHELL_SHA256  /usr/bin/doors-shell" | sha256sum -c - || rc=1
echo "$ORIG_LVGL_SHA256  /usr/lib/liblvgl.so.9.5.0" | sha256sum -c - || rc=1
# Never two owners of the panel: start only when no shell runs at all.
if [ -z "$(pidof doors-shell pocketos-shell)" ] && [ -z "$("$K/neon-status.sh" panel-owners)" ]; then
	/etc/init.d/S90doors-shell start || rc=1
else
	echo "RESTORE: a shell or panel owner is still running ($(pidof doors-shell pocketos-shell)); installed shell NOT started" >&2
	rc=1
fi
sleep 3
"$K/neon-status.sh"
n=0; for p in $(pidof doors-shell); do n=$((n+1)); [ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || rc=1; done
[ $n -eq 1 ] || { echo "expected exactly one doors-shell, found $n" >&2; rc=1; }
grep -q LD_LIBRARY_PATH /proc/$(pidof doors-shell)/environ 2>/dev/null && { echo "installed shell has LD_LIBRARY_PATH" >&2; rc=1; }
[ $rc -eq 0 ] && echo "RESTORE: OK - installed doors-shell supervised, original settings" || echo "RESTORE: CHECK THE LINES ABOVE (rc=$rc)"
exit $rc
