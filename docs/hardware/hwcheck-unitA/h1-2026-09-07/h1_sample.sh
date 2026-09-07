# H1 sampler, runs on the device: $1 = label, $2 = seconds (samples every 5 s).
label=$1; secs=${2:-60}; n=$((secs / 5))
pid=$(pidof pocketos-shell)
read c1 u1 n1 s1 i1 rest < /proc/stat
t1=$(cat /sys/class/thermal/thermal_zone0/temp); set -- $(cat /proc/$pid/stat); p1=$(( ${14} + ${15} ))
echo "=== $label start $(date +%T) shell pid $pid temp_mC $t1 loadavg $(cat /proc/loadavg | cut -d' ' -f1-3) rss_kB $(grep VmRSS /proc/$pid/status | awk '{print $2}')"
echo "conf: pocketos-shell=[$(tr '\n' ';' < /etc/default/pocketos-shell)] settings=[$(grep -v '^#' /etc/pocketos/settings.conf | tr '\n' ';')] staging_env=$(tr '\0' '\n' < /proc/$pid/environ | grep K230_LVGL_DRM_STAGING)"
i=0
while [ $i -lt $n ]; do
    sleep 5
    i=$((i + 1))
    printf 't+%3ds temp=%s load=%s ' $((i * 5)) "$(cat /sys/class/thermal/thermal_zone0/temp)" "$(cut -d' ' -f1 /proc/loadavg)"
    top -b -n 1 2>/dev/null | awk '/^CPU:/{cpu=$0} $NF=="/usr/bin/pocketos-shell"{sh=$0} $NF=="EU868"{rd=$0} END{gsub(/  +/," ",cpu); gsub(/  +/," ",sh); print cpu " | " sh}'
done
read c2 u2 n2 s2 i2 rest < /proc/stat
t2=$(cat /sys/class/thermal/thermal_zone0/temp); set -- $(cat /proc/$pid/stat); p2=$(( ${14} + ${15} ))
busy=$(( (u2 + n2 + s2) - (u1 + n1 + s1) )); idle=$(( i2 - i1 )); total=$((busy + idle))
echo "=== $label end $(date +%T) temp_mC $t2 loadavg $(cut -d' ' -f1-3 /proc/loadavg) rss_kB $(grep VmRSS /proc/$pid/status | awk '{print $2}') cpu_busy_percent_over_window $(( busy * 100 / total )) (jiffies busy=$busy idle=$idle) shell_cpu_percent $(( (p2 - p1) * 100 / total )) (shell jiffies $((p2 - p1)))"
