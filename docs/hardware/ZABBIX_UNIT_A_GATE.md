# Zabbix viewer: unit A gate (PASS on 7c725ba, 2026-09-25)

**Build under test:** `experiment/zabbix-dashboard` at **`7c725ba`**,
hand-installed over the flashed v0.0.13 development image of master
`ee39407`. `doors shell info` said `"build":"7c725ba"` throughout. The unit
was found in rotation mode Automatic, landscape, and was left that way.

**Status:** run 2026-09-25, **PASS**. One defect was found on the unit and
fixed: landscape STATUS could not be scrolled by finger (§ Findings). The
first install was `ae17c38`; `7c725ba` is the fix. Afterwards the unit was
rolled back and verified: `doors-shell` hash `e723ffab…` is the image's own,
and the helper, the mock, `zabbix.conf` and the secret directory are all
gone. Nothing was flashed, and the golden card was not touched.

Record, captures and scripts are in `C:\K230\out\zabbix-gate\`:

- `step*.log`;
- `caps/*.png`, the panel read back with kmsgrab;
- `install.sh`, `payload.sh` and `set_unit_secret.sh`.

Touches were injected into the Goodix evdev node (`rift_tap.py`). The owner
typed the real password once, through `set_unit_secret.sh`, which pipes it
over SSH; it was removed with `pos-zabbix clear-secret` right after step 7.

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

The SDK sysroot has the OpenSSL 3 headers, so the riscv64 mock is built with
`ZABBIX_MOCK_TLS=1` and serves HTTPS itself (the image has no `openssl`
tool). The certificate is a throwaway one made on the host with
`subjectAltName=IP:127.0.0.1`.

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

## Record (7c725ba)

Payload, from a clean clone of `7c725ba`:

- `make all` (riscv64, `ZABBIX_CURL=1 POCKETCAM_JPEG=1 ENABLE_SX1262=1
  ENABLE_MESHCORED=1`, `-Werror`) exits 0;
- both DRM shells build with 0 first-party warnings.

| File | sha256 |
| --- | --- |
| doors-shell (`POCKETOS_WITH_ZABBIX=ON`) | `683cd8f4437df9048e8ae55de14f2b535589382e429675e4c87b4d37bc5f2852` |
| pos-zabbix | `0d57ba664d301a4219fe263f875f2730ae2c1e2a77053d34e2c849ab22993793` |
| pos-zabbix-mock (TLS) | `bf0651ffd75db0481cd60ff7be593e8113f6777a70d2de7b56eb68216fd3fcd9` |

All three matched on the unit after the install. The rollback holds the
image's shell (`e723ffab5f8dd1f292f4e3131ad0f5a267d167a0891a070dbfcfcffe8d8376a3`).

| Step | Result | Evidence |
| --- | --- | --- |
| 1 identity | PASS | `"build":"7c725ba"`; `pos-zabbix transport` = `curl`; Zabbix in `doors app list` |
| 2 not set up | PASS | Launcher: Zabbix under MORE (eye icon). OVERVIEW "Not set up", TRY THE DEMO, no SIMULATED |
| 3 demo, both orientations | PASS | DISASTER, "9 open · 6 unacknowledged", "36 monitored · 2 down", SIMULATED. PROBLEMS and HOSTS scroll; a host opens and BACK returns. SCENARIO cycles demo → … → flap. `flap` brings "Server not reachable · retry in 3s · showing data from 2s ago" with the data kept, then recovers (backoff 5, 10, 20, 40 s). `large`: "1 200 open · 800 unacknowledged", "100 of 1 200". Portrait and landscape; after a rotation restart 0 helpers, then exactly 1 when opened |
| 4 transport on loopback | PASS | See the table below |
| 4 password path (mock) | PASS | See the table below |
| 5 network goes away (real server) | PASS | A blackhole route to the server's address on the unit (SSH on eth0 unaffected). Detected at the next refresh (timeout 15 s); retrying 5 → 10 → 20 → 40 s. Banner "Server not reachable · retry in 4s · showing data from 1m ago", data kept. Online 4 s after the route was removed, on the same session (no new login) |
| 6 clock unset | NOT RUN | Needs a reboot without NTP (owner's call). UNVERIFIED on the unit |
| 7 cost | PASS (see note) | 60 s on OVERVIEW. Helper: CPU 0.02 % (demo) and 0.15 % (large, 1 500 hosts); private memory 1.8 MB and 2.4 MB; PSS 2.4 MB and 3.0 MB; RSS 5.8 MB and 6.4 MB (peak 6.7 MB), of which about 4.0 MB is shared clean pages (libcurl, OpenSSL, libc). Shell: RSS 14.4–15.1 MB, CPU 1.4–1.6 %. **Note:** the budget in step 7 was written as "helper under 5 MB RSS"; RSS is above it only by the shared libraries, and the helper's own memory is under half of it |
| 7 open/close 20 times | PASS | 0 helpers after every close; 25 logins and 25 logouts on the mock, 0 sessions open; shell RSS 14 592 → 14 972 kB |
| 8 crash | PASS | `kill -SEGV`: "Zabbix helper stopped (crashed) · restarting in 2s · showing data from 26s ago"; a new helper pid; the shell logs `helper ended (139)` as WARN; no assert |
| 9 rollback | PASS | `RESTORE.sh`: `restored: … OK`, `"build":"ee39407"`, no helper, no configuration, no secret |

**Step 4, the real transport on loopback,** through the device's libcurl
8.12.1 with OpenSSL 3:

| Case | Result |
| --- | --- |
| HTTP + token | `pend 9`, `hend 36`, exit 0 |
| mock `auth` | `authfail/auth` |
| mock `timeout` | `retrying/timeout` after 5 s (= `timeout_s`) |
| mock `http500` | `retrying/http` |
| mock `malformed` | `retrying/malformed` |
| mock `apierror` | `retrying/api` |
| back to `demo` | `online` |
| HTTPS, untrusted certificate | `retrying/tls` |
| HTTPS with `ca_file` | online, API 7.0.31 |
| HTTPS, wrong name (`localhost`) | `retrying/tls` |
| `verify_tls=0` | online; STATUS warns |
| `http://` without `allow_insecure_http` | refused, exit 2 |
| nothing listening | `retrying/connect` |
| a name that does not resolve | `retrying/dns` |
| secret file | 0600, in a 0700 directory, root |

**Step 4, the password path,** against the HTTPS mock with the made-up
password `gate pässord 7` (user `demo`, any password):

| Case | Result |
| --- | --- |
| `pos-zabbix check` | `logged in`, `logged out`; mock: 1 login, 1 logout, 0 open |
| app open, then closed | helper gone; `logged out` 7 ms after `session end`; 0 sessions open |
| reopen | logs in again |
| renewal: `short` scenario (a session ends every 6 requests), 6 × REFRESH NOW | 8 silent re-logins, no `retrying` or `authfail`, 0 open after close |
| refused password (`user=nobody`) | banner "Access refused: check the user and password · REFRESH NOW to try again · no data yet", no countdown. 1 failed login on open, **0 more in 6.5 minutes**, exactly **1** more on REFRESH NOW |
| logs | the password is in no log; `zabbix.conf` holds no password |

**The real server, from the unit (step 7 of the owner's plan),** at
the owner's production server (Zabbix **7.4.15**, its URL and user kept out of the repository). TLS was verified against the
image's CA store (no `ca_file`). The owner's password was stored with
`set_unit_secret.sh`.

| Check | Result |
| --- | --- |
| `pos-zabbix check` | version 7.4.15; logged in; 32 open problems, 31 unacknowledged; by severity 0/0/28/4/0/0 (not classified … disaster); 173 monitored hosts, 0 down, 3 unknown, 0 in maintenance; logged out |
| the app, portrait and landscape | "AVERAGE · 4 at this severity, of 32 open", "32 open · 31 unacknowledged", "173 monitored · 0 down", "3 unknown · 0 in maintenance", "<label> · Zabbix 7.4.15"; PROBLEMS, HOSTS, STATUS ("User and password (session)") |
| the helper process | argv `/usr/bin/pos-zabbix session`; its environment holds no credential (only the shell's `POCKETOS_*`, `PATH`, `HOME`, `USER` and the SSH variables of the bench's restart) |
| rotation restart | the old session logged out, a new one logged in |
| close | `logged out`, no helper left |
| afterwards | `pos-zabbix clear-secret`; the user name is in no log on the unit |

**Stability soak** against the mock, on the password path:

- 20 opens and closes;
- 20 × REFRESH NOW;
- portrait, landscape, portrait, landscape with the app open;
- 10 lock/unlock cycles with the app open.

Results:

- 0 crash reports and 0 restarts of any service (`doors system status`);
- `dmesg` unchanged (431 lines);
- the shell kept the same pid, RSS 14 592 → 14 972 kB;
- 1 helper while open and 0 after closing;
- no WARN or ERROR in `shell.log` from this gate apart from the deliberate
  helper crash.

**UI responsiveness:** 60 `shell.info` calls over 30 s with the `large`
estate open took median 24 ms, p95 31 ms and max 36 ms. During tab switches
on the large estate the maximum was 41 ms.

## Findings

- **Fixed (7c725ba): a finger drag on text did not scroll the page.** In
  landscape the buttons at the foot of STATUS (REFRESH NOW, SCENARIO, LEAVE
  THE DEMO) sit below the fold. LVGL only finds clickable objects under a
  finger, and the pages were not clickable, so a drag that started on a
  status line scrolled nothing. The same held on the OVERVIEW cards and the
  host detail text. The simulator suite had missed it because it scrolled
  targets into view instead of dragging.
  - The pages are now clickable, and not click-focusable.
  - `zabbix_app_test` now drags by finger. The check failed before the fix
    and passes after it.
  - On the unit STATUS scrolls, and all three buttons were reached and used.
- **Gate tooling, not the product: an injected tap can lose an axis after a
  shell restart.**
  - The input core drops an ABS event whose value equals the device's
    current value, and LVGL's evdev driver starts at 0,0.
  - So the first injected tap after a restart that repeats either raw axis
    of the previous tap lands at 0 on that axis.
  - Reproduced three ways: the same point failed, and a point with the same
    x also failed; a point with both axes different worked.
  - It cost several apparently lost tab taps before it was understood.
  - Whether a real finger can meet it (an exact repeat of one raw axis) is
    a platform question, outside this branch. It is raised as its own task.
- **Not a defect: helper RSS vs the step 7 wording.** See step 7. The
  budget should be read as the helper's own memory (private or PSS).
- **Open: the web-UI cross-check.** The frontend's own Problems and Hosts
  numbers were not available to compare, so the counts are VERIFIED only
  as the API returns them (and consistently between the host run and the
  unit, allowing for live change). See docs/apps/ZABBIX.md §12.
