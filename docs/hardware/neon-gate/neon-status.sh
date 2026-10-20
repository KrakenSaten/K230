#!/bin/sh
# Read-only. Who owns the panel, which shell and libraries are loaded, which
# settings are in effect.
#   neon-status.sh               full report
#   neon-status.sh panel-owners  only the pids holding /dev/dri/card0
K=$(cd "$(dirname "$0")" && pwd)
owners() {
	for f in /proc/[0-9]*/fd/*; do
		[ "$(readlink "$f" 2>/dev/null)" = /dev/dri/card0 ] && { p=${f#/proc/}; echo "${p%%/*}"; }
	done | sort -u
}
[ "${1:-}" = panel-owners ] && { owners; exit 0; }
echo "-- panel owners (/dev/dri/card0)"
for p in $(owners); do echo "$p $(readlink /proc/$p/exe)"; done
echo "-- shells"
for p in $(pidof doors-shell); do
	echo "$p $(readlink /proc/$p/exe) ppid $(awk '{print $4}' /proc/$p/stat)"
	grep -o '/[^ ]*liblvgl.so[^ ]*' /proc/$p/maps | sort -u
done
ps | grep '[p]os-supervise doors-shell' || echo "(no doors-shell supervisor)"
echo "-- settings.conf $(sha256sum /etc/pocketos/settings.conf | cut -c1-16)"
grep -v '^#' /etc/pocketos/settings.conf
