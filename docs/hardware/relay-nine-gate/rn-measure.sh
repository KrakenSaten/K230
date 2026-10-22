#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Read-only. Sample the test shell and its effect helper every 0.5 s for N
# seconds: CPU % of one core (utime+stime), shell RSS, and the playback PCM
# state; count playback stream starts (a new trigger_time after the first
# means an underrun was recovered). Summary appended to log/measure.txt.
#   rn-measure.sh <label> [seconds, default 30]
set -u
K=$(cd "$(dirname "$0")" && pwd)
L=${1:?label}; N=${2:-30}
P=$(cat "$K/test-shell.pid" 2>/dev/null)
[ -n "$P" ] && kill -0 "$P" 2>/dev/null || { echo "no test shell" >&2; exit 1; }
HZ=100
ticks() { [ -n "$1" ] && awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0; }
helper() { for h in $(pidof pos-audio-effects); do [ "$(awk '{print $4}' /proc/$h/stat)" = "$P" ] && echo $h; done | head -1; }
s0=$(ticks "$P"); prev=$s0; peak=0; hs=0; i=0; trig=""; starts=0; hmax=0
while [ $i -lt $((N*2)) ]; do
	sleep 0.5; i=$((i+1))
	now=$(ticks "$P"); d=$((now-prev)); prev=$now; [ $d -gt $peak ] && peak=$d
	H=$(helper); hc=0
	if [ -n "$H" ]; then
		[ "$H" != "${HP:-}" ] && { HP=$H; hprev=$(ticks "$H"); }
		hn=$(ticks "$H"); hc=$((hn-hprev)); hprev=$hn; hs=$((hs+hc)); [ $hc -gt $hmax ] && hmax=$hc
	fi
	t=$(grep trigger_time /proc/asound/card0/pcm0p/sub0/status 2>/dev/null | cut -d: -f2)
	[ -n "$t" ] && [ "$t" != "$trig" ] && { starts=$((starts+1)); trig=$t; }
	echo "t=$i shell_cpu=$((d*200/HZ))% helper=${H:--} helper_cpu=$((hc*200/HZ))% rss_kb=$(awk '/VmRSS/{print $2}' /proc/$P/status) pcm=$(head -1 /proc/asound/card0/pcm0p/sub0/status 2>/dev/null)"
done
tot=$((prev-s0))
line="$(date -Iseconds) label=$L secs=$N shell_cpu_avg=$((tot*100/HZ/N))% shell_cpu_peak_0.5s=$((peak*200/HZ))% helper_cpu_avg=$((hs*100/HZ/N))% helper_cpu_peak_0.5s=$((hmax*200/HZ))% pcm_stream_starts=$starts rss_kb=$(awk '/VmRSS/{print $2}' /proc/$P/status) hwm_kb=$(awk '/VmHWM/{print $2}' /proc/$P/status) load=$(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p "$K/log"; echo "$line" | tee -a "$K/log/measure.txt"
