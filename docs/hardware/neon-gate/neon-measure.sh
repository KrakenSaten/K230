#!/bin/sh
# Read-only. Sample the running doors-shell once a second for N seconds while
# the owner performs the interaction sequence; print CPU % (of one core,
# utime+stime) and memory, then a summary line appended to log/measure.txt.
#   neon-measure.sh <label> [seconds, default 30]
set -u
K=$(cd "$(dirname "$0")" && pwd)
L=${1:?label}; N=${2:-30}
P=$(pidof doors-shell | awk '{print $1}')
[ -n "$P" ] || { echo "no doors-shell" >&2; exit 1; }
HZ=100
theme=$(grep '^theme=' /etc/pocketos/settings.conf | cut -d= -f2)
t() { awk '{print $14+$15}' /proc/$P/stat; }
start=$(t); prev=$start; max=0; i=0
echo "pid $P exe $(readlink /proc/$P/exe) theme $theme label $L"
while [ $i -lt $N ]; do
	sleep 1; i=$((i+1)); now=$(t); d=$((now-prev)); prev=$now
	[ $d -gt $max ] && max=$d
	echo "t=$i cpu=$((d*100/HZ))% rss_kb=$(awk '/VmRSS/{print $2}' /proc/$P/status)"
done
tot=$((prev-start))
line="$(date -Iseconds) label=$L theme=$theme exe=$(readlink /proc/$P/exe) secs=$N cpu_avg=$((tot*100/HZ/N))% cpu_peak_1s=$((max*100/HZ))% rss_kb=$(awk '/VmRSS/{print $2}' /proc/$P/status) hwm_kb=$(awk '/VmHWM/{print $2}' /proc/$P/status) load=$(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p "$K/log"; echo "$line" | tee -a "$K/log/measure.txt"
