echo "--- pass 3 setup: reduced_motion off (settings restored from backup), staging off via /etc/default/pocketos-shell"
/etc/init.d/S90pocketos-shell stop
cp -p /tmp/h1/settings.conf.orig /etc/pocketos/settings.conf
md5sum /tmp/h1/settings.conf.orig /etc/pocketos/settings.conf
printf 'ENABLE=1\nK230_LVGL_DRM_STAGING=0\n' > /etc/default/pocketos-shell
cat /etc/default/pocketos-shell
/etc/init.d/S90pocketos-shell start
sleep 5
pid=$(pidof pocketos-shell); echo "shell pid $pid env: $(tr '\0' '\n' < /proc/$pid/environ | grep K230_LVGL_DRM_STAGING)"
pos shell info | grep -E 'current|theme'
pos app start radar; sleep 1; pos shell info | grep current
pos logs shell -n 3
