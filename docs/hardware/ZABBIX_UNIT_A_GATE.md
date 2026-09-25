# Zabbix viewer: unit A gate (PREPARED, NOT RUN)

**Build under test:** the tip of `experiment/zabbix-dashboard`, hand-installed
over the flashed image the unit carries. Check `doors shell info` against the
build id before starting, and state it and the rotation mode at the top of
the record (docs: the gate-identity rule).

**Status:** prepared 2026-09-25, **not run**. Unit A did not answer on
192.168.10.171 or .157 ("No route to host"), and its console port (COM9) was
not present. Nothing on the unit was touched: no flash, no golden card, no
change of any kind.

## What changes on the unit

The gate is reversible and touches userspace only:

- `/usr/bin/doors-shell` is replaced by the branch's build, configured with
  `-DPOCKETOS_WITH_ZABBIX=ON`. The only change is the Zabbix app: registered,
  with no launcher table entry, so it shows under MORE.
- `/usr/bin/pos-zabbix` is added: the helper, linked to the image's own
  `libcurl.so.4` (8.12.1).
- `/usr/bin/pos-zabbix-mock` is added for the gate only, and removed at the
  end.
- `/etc/pocketos/zabbix.conf` and `/var/lib/pocketos/zabbix/` are added by
  the steps below, and removed by the rollback.

## Payload and rollback

Build from a clean clone of the commit (docs/BUILD_ENVIRONMENT.md,
riscv64 recipe), so every binary carries its build id:

```sh
make ZABBIX_CURL=1 POCKETCAM_JPEG=1 CC=<cross>gcc CXX=<cross>g++ \
     CFLAGS="--sysroot=$SYSROOT -O2 -Wall -Wextra -Werror" LDFLAGS="--sysroot=$SYSROOT" \
     tools/zabbix/pos-zabbix
make ZABBIX_MOCK_TLS=0 CC=<cross>gcc CFLAGS="--sysroot=$SYSROOT -O2" LDFLAGS="--sysroot=$SYSROOT" \
     tools/zabbix/pos-zabbix-mock
cmake -S ui/shell -B build-drm -DCMAKE_TOOLCHAIN_FILE=$SDKOUT/host/share/buildroot/toolchainfile.cmake \
      -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot -DPOCKETOS_WITH_ZABBIX=ON && cmake --build build-drm
```

The riscv64 mock has no TLS: its OpenSSL headers were not tried. The HTTPS
half of step 4 therefore uses `openssl s_server` in front of it, if the image
has the openssl tool (UNKNOWN),
or it is skipped. Either way, record which.

On the unit, before copying anything:

```sh
mkdir -p /root/rollback-zabbix && cp -a /usr/bin/doors-shell /root/rollback-zabbix/
printf '#!/bin/sh\n/etc/init.d/S90doors-shell stop\ncp -a /root/rollback-zabbix/doors-shell /usr/bin/\nrm -f /usr/bin/pos-zabbix /usr/bin/pos-zabbix-mock /etc/pocketos/zabbix.conf\nrm -rf /var/lib/pocketos/zabbix\n/etc/init.d/S90doors-shell start\n' > /root/rollback-zabbix/RESTORE.sh
chmod 755 /root/rollback-zabbix/RESTORE.sh
```

Then stop the shell, copy the three binaries (checking their sha256 against
the payload's), and start the shell again. The root filesystem has about
120 MB free and the payload is under 2 MB.

## Steps

1. **Identity.** `doors shell info` shows the branch's build, and
   `pos-zabbix transport` says `curl`.
2. **Not set up.** Open Zabbix (`doors app open zabbix`, or MORE on the
   launcher). OVERVIEW shows "Not set up" and TRY THE DEMO, and there is no
   SIMULATED hint.
3. **Demo, both orientations.** Tap TRY THE DEMO. OVERVIEW shows DISASTER,
   "9 open · 6 unacknowledged" and "36 monitored · 2 down", with the header
   saying SIMULATED. Then check:
   - PROBLEMS scrolls; a row opens its host; BACK returns.
   - HOSTS scrolls 36 rows.
   - STATUS switches the scenario: `large` gives "100 of 1 200", and `flap`
     brings and clears the offline banner while the data stays.
   - Repeat with the rotation mode set to landscape. The shell restarts, and
     `pgrep pos-zabbix` shows one helper, not two.
4. **The real transport, on loopback.** Run
   `pos-zabbix-mock --port 18080 &`, then write zabbix.conf with
   `url=http://127.0.0.1:18080/zabbix/`, `allow_insecure_http=1` and
   `label=Mock`. Store a token with
   `echo zbx-gate-token-7f3a | pos-zabbix set-secret token`
   and run `pos-zabbix check`: it exits 0 with `pend … 9`. Open the app:
   live data, no SIMULATED, and STATUS says "None: plain http://" in the
   error tone. Switch the mock's scenario and watch the banner:
   - `curl http://127.0.0.1:18080/__mock/scenario/auth` gives "Access
     refused";
   - `timeout` gives a timeout after `timeout_s`;
   - `demo` recovers.

   Then the password path, against the same mock (user `demo`, any
   password; a made-up one, never a real credential):
   - Add `auth=password` and `user=demo` to zabbix.conf, and run
     `printf '%s\n' 'gate pässord 7' | pos-zabbix set-secret password`.
     `pos-zabbix check` exits 0, and pos-zabbix.log says `logged in`, then
     `logged out`.
   - Open the app: live data. Close it (`doors shell home`): the log says
     `logged out` within 3 s, and `pgrep pos-zabbix` finds nothing after it.
   - Set `user=nobody` and open the app. The banner says "check the user and
     password · REFRESH NOW to try again", with no countdown. After 6
     minutes, the log shows no second login attempt.
   - Put the token setup back for the rest of the gate.
5. **The network goes away.** With the mock bound to the unit's LAN address,
   pull Ethernet (or switch Wi-Fi off in Settings). The banner must say the
   server is not reachable, with the retry countdown, and the data must stay.
   Restore the network and confirm recovery within one backoff step.
6. **Clock unset.** Only if the owner allows a reboot without NTP. Run an
   HTTPS configuration before NTP has answered. STATUS and the banner must
   say "device clock not set", and verification must not be turned off.
7. **Cost.** On OVERVIEW, left alone for 2 minutes, run
   `top -b -n 3 -d 10 | grep -E 'doors-shell|pos-zabbix'`. Record CPU% and
   RSS for each process, and for the shell compare with any other app
   (budget: helper under 5 MB RSS and near 0% CPU between refreshes; shell
   RSS growth under 1 MB). Then open and close the app 20 times with
   `doors app open zabbix` and `doors shell home`: RSS stays flat, and no
   helper is left.
8. **Crash.** `kill -SEGV $(pidof pos-zabbix)`. The banner says the helper
   stopped (crashed) and it restarts within about 2 s. The shell must not
   log an assert.
9. **Rollback.** `/root/rollback-zabbix/RESTORE.sh`, then `doors shell info`
   shows the original build.

## Pass criteria

- Steps 1-5 and 7-9 pass.
- No `ERROR` or `assert` in `shell.log` or `pos-zabbix.log`.
- No token or password in any log (`grep -rE 'zbx-gate-token|gate pässord'
  /var/lib/pocketos/log` finds nothing).
- The unit is left rolled back, with the result recorded.
