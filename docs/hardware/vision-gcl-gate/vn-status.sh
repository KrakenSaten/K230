#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Read-only. Who holds the panel and the camera, which shells and camera
# helpers run, with which Vision overrides.
#   vn-status.sh               full report
#   vn-status.sh panel-owners  pids holding /dev/dri/card0
owners() {
	for f in /proc/[0-9]*/fd/*; do
		case "$(readlink "$f" 2>/dev/null)" in $1) p=${f#/proc/}; echo "${p%%/*}" ;; esac
	done | sort -u
}
case "${1:-}" in
panel-owners) owners /dev/dri/card0; exit 0 ;;
esac
echo "-- panel owners (/dev/dri/card0)"
for p in $(owners /dev/dri/card0); do echo "$p $(readlink /proc/$p/exe)"; done
echo "-- camera nodes (/dev/video*), besides the ISP server"
for p in $(owners '/dev/video*'); do
	e=$(readlink /proc/$p/exe); [ "$e" = /usr/bin/isp_media_server ] || echo "$p $e"
done
echo "-- shells"
for p in $(pidof doors-shell pocketos-shell); do
	echo "$p $(readlink /proc/$p/exe) ppid $(awk '{print $4}' /proc/$p/stat)"
	tr '\0' '\n' < /proc/$p/environ | grep -E '^POCKETOS_(VISION|CONFIG|STATE)' | sed 's/^/   env /'
done
ps | grep '[p]os-supervise doors-shell' || echo "(no doors-shell supervisor)"
echo "-- camera helpers"
for p in $(pidof pos-vision pos-camera); do echo "$p $(readlink /proc/$p/exe) ppid $(awk '{print $4}' /proc/$p/stat)"; done
echo "-- meshcored $(pidof meshcored) radiod $(pidof radiod)"
