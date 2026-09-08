sh -n /tmp/h1/h1_sample.sh && echo sampler-ok
sh /tmp/h1/h1_sample.sh pass1b-default-shell-cpu 30 | grep '^===' | tee -a /tmp/h1/pass1.txt
echo "--- pass 2 setup: reduced_motion=1 (shell stopped for the edit)"
/etc/init.d/S90pocketos-shell stop
printf 'reduced_motion=1\n' >> /etc/pocketos/settings.conf
cat /etc/pocketos/settings.conf
/etc/init.d/S90pocketos-shell start
sleep 5
pos shell info | grep -E 'current|theme'
pos app start radar; sleep 1; pos shell info | grep current
pos logs shell -n 3
