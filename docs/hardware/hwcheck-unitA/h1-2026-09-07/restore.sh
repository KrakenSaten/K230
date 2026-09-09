echo "--- restore original configuration"
/etc/init.d/S90pocketos-shell stop
cp -p /tmp/h1/pocketos-shell.orig /etc/default/pocketos-shell
cp -p /tmp/h1/settings.conf.orig /etc/pocketos/settings.conf
md5sum /tmp/h1/pocketos-shell.orig /etc/default/pocketos-shell /tmp/h1/settings.conf.orig /etc/pocketos/settings.conf
ls -la --full-time /etc/default/pocketos-shell /etc/pocketos/settings.conf
cat /etc/default/pocketos-shell; cat /etc/pocketos/settings.conf
/etc/init.d/S90pocketos-shell start
sleep 5
pid=$(pidof pocketos-shell); echo "shell pid $pid env: $(tr '\0' '\n' < /proc/$pid/environ | grep K230_LVGL_DRM_STAGING) rss_kB $(grep VmRSS /proc/$pid/status | awk '{print $2}')"
pos shell info | grep -E 'current|theme|mode'
pos logs --crashes
ls -la --full-time /var/lib/pocketos/radar/record.v1
echo "--- collected pass files"; cat /tmp/h1/pass1.txt /tmp/h1/pass2.txt /tmp/h1/pass3.txt | grep '^==='
