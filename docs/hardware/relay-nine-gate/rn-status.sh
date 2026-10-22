#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Read-only. Who holds the panel and the audio device, which shells and
# effect helpers run, and with which Relay Nine overrides.
#   rn-status.sh               full report
#   rn-status.sh panel-owners  pids holding /dev/dri/card0
#   rn-status.sh audio-owners  pids holding a /dev/snd PCM
owners() {
	for f in /proc/[0-9]*/fd/*; do
		case "$(readlink "$f" 2>/dev/null)" in $1) p=${f#/proc/}; echo "${p%%/*}" ;; esac
	done | sort -u
}
case "${1:-}" in
panel-owners) owners /dev/dri/card0; exit 0 ;;
audio-owners) owners '/dev/snd/pcm*'; exit 0 ;;
esac
echo "-- panel owners (/dev/dri/card0)"
for p in $(owners /dev/dri/card0); do echo "$p $(readlink /proc/$p/exe)"; done
echo "-- audio owners (/dev/snd/pcm*)"
for p in $(owners '/dev/snd/pcm*'); do echo "$p $(readlink /proc/$p/exe)"; done
echo "-- shells"
for p in $(pidof doors-shell pocketos-shell); do
	echo "$p $(readlink /proc/$p/exe) ppid $(awk '{print $4}' /proc/$p/stat)"
	tr '\0' '\n' < /proc/$p/environ | grep -E '^(DOORS_|LD_LIBRARY_PATH)' | sed 's/^/   env /'
done
ps | grep '[p]os-supervise doors-shell' || echo "(no doors-shell supervisor)"
echo "-- effect helpers"
for p in $(pidof pos-audio-effects); do echo "$p $(readlink /proc/$p/exe) ppid $(awk '{print $4}' /proc/$p/stat)"; done
echo "-- $(ls /run/pocketos/audio.* 2>/dev/null | tr '\n' ' ')"
echo "-- pcm $(cat /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | head -1)"
echo "-- settings.conf $(sha256sum /etc/pocketos/settings.conf | cut -c1-16)"
