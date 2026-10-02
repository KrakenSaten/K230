# Zabbix connection settings — unit B hardware check (2026-10-02)

**Build on the unit:** unit B (K230-B, 192.168.10.187), Doors v0.3.0 image
(`2956eff`) with two binaries hot-swapped from branch
`feat/zabbix-connection-settings`:

| Binary | Built from | md5 |
| --- | --- | --- |
| `/usr/bin/doors-shell` | `25bde9d` (first pass `a49e107`) | `e2b9426b…707b` (`462031cc…4a14`) |
| `/usr/bin/pos-zabbix` | `6021a6c` (same helper code as the tip) | `c4862607…d95e` |

Everything else on the unit was the v0.3.0 image. Portrait 568 x 1232, the
owner's text size **Large**. The unit was restored afterwards (below).

**Server:** the development mock `pos-zabbix-mock` (riscv64, never
installed), over **HTTPS** on the unit's loopback (`127.0.0.1:18443`), with a
self-signed certificate trusted through `ca_file` in zabbix.conf. So the
device's own libcurl 8.12.1 and OpenSSL ran the whole path. The token and
passwords used are made-up gate values; no real server or credential was
involved.

## Steps and results

| # | What | Result |
| --- | --- | --- |
| 0 | Hot-swap with a rollback (`/root/rollback-zabbix-settings/RESTORE.sh`); the shell reports the branch build | PASS |
| 1 | An existing installation set up over SSH the documented way (zabbix.conf with url, label, ca_file, timeout; `set-secret token`); `pos-zabbix check` online | PASS |
| 2 | Zabbix opens: OVERVIEW (9 open, DISASTER), PROBLEMS, a host's detail, STATUS ("API token (never shown)", "HTTPS, certificate checked", Online) — existing functionality unchanged | PASS |
| 3 | STATUS → CONNECTION SETTINGS: the settings load (the stored address, API TOKEN marked, "A token is stored and never shown"), the token field empty | PASS |
| 4 | TEST CONNECTION with the stored token: **CONNECTED** ("Zabbix 7.0.31 answered and accepted the API token") | PASS (see finding 1) |
| 5 | PASSWORD: a tap on USER brings the touch keyboard, the field stays in view; `nobody` and a 6-letter password typed on the touch keyboard; the password shows six bullets from the first character and only "6 characters typed" | PASS |
| 6 | TEST CONNECTION: **AUTH FAILED** with the server's words; the mock counted exactly one failed login | PASS |
| 7 | SAVE with the same wrong user: AUTH FAILED, "Not saved: the previous settings stay in use."; zabbix.conf and the secret unchanged | PASS |
| 8 | USER corrected to `demo`, SAVE: **CONNECTED**, "Saved and in use"; zabbix.conf now has `auth=password` and `user=demo` with label, ca_file and timeout kept, its 0600 mode kept (finding 2); the secret 0600, password kind; the field wiped; a new helper logged in, the shell's pid unchanged (applied without a reboot) | PASS |
| 9 | The typed password in no log under /var/lib/pocketos/log, not in the helper's cmdline or environ | PASS |
| 10 | Header back slab: leaves the app (as from every screen); Home: no helper left | PASS |
| 11 | Zabbix opened again: STATUS "User and password (session)", Online; CONNECTION loads PASSWORD, `demo`, "A password is stored" — the save persisted after the app was reopened | PASS |
| 12 | The screen's own ‹ BACK returns to STATUS | PASS |
| 13 | Mock stopped, TEST CONNECTION: **UNREACHABLE** ("Server not reachable: Failed to connect …") | PASS |
| 14 | The address cleared on the touch keyboard, SAVE: **INVALID CONFIG** ("Type the server's address …"), not saved, the files unchanged | PASS |
| 15 | After the fix of finding 3, redeployed: switching TOKEN → PASSWORD takes the old CONNECTED off the card | PASS |
| 16 | Over the whole run: no crash report, no segfault in dmesg, no gate secret in any log | PASS |

Not tested on hardware: the keyboard base's hardware Back key (it reaches
the same `app.back` hook, which `zabbix_app_test` drives), landscape (host
only), and a real Zabbix server (the owner's server and credentials were not
used; the real-server path is unchanged from the unit A gate of
`docs/hardware/ZABBIX_UNIT_A_GATE.md`).

## Findings, all fixed on the branch

1. **The trial timed out beside a live session against the mock.** The mock
   serves one connection at a time and the app's session held a keep-alive
   connection (a separate `pos-zabbix check` timed out the same way). A real
   frontend serves both. The mock got `--close` (`ba5c9dd`), and
   `zabbix_http_test.sh` covers a trial beside a live session.
2. **A save loosened an administrator's 0600 zabbix.conf to 0644.** A save
   now keeps the file's mode; only a new file is 0644 (`6021a6c`).
3. **A stale result.** A CONNECTED for the token stayed on the card after
   switching to a password not yet tried. An edit now clears the result
   (`25bde9d`).

Also learned: the unit's BusyBox has no `pkill` (stop processes with
`kill $(pidof …)`), and a hung ssh session can use up sshd's slots for
connections not yet authenticated (kex resets) until it is killed.

## Restored

`RESTORE.sh` put back `doors-shell` `4f940c27…` and `pos-zabbix`
`c759f89b…` (the v0.3.0 image's, checked against the rollback MD5), removed
the gate's zabbix.conf and `/var/lib/pocketos/zabbix` (the unit had none
before), stopped the mock and removed `/tmp/zbxgate`. The shell reports
`2956eff`, showing DeskBuddy as before. The rollback directory
`/root/rollback-zabbix-settings` is left on the unit.

Kit (not committed): `C:\K230\out\zabbix-settings-gate` (deploy.sh,
s1_setup.sh … s9_final.sh, type.sh, caps/).
