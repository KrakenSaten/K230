# Settings and System cleanup: unit B smoke (DS §52)

Branch `feat/settings-system-cleanup`, build **0a12874** (doors-shell md5
`7c5d96d3`, sysd md5 `34b3b5f5`; riscv64 DRM build from a clean clone, 0
first-party warnings, no test hooks in the binary). Unit B, 2026-10-02
20:28-20:40 UTC, remote: nobody at the unit. Unit A was not reachable (no
route to host). Captures and scripts: `out/settings-sys-gate/` (not in git).

## Before

Unit B ran doors-shell `d6ffdda` (md5 `38c69c67`) and the v0.3.0 sysd (md5
`b7021a7c`), landscape 1232 x 568 with the keyboard base attached, text size
Large, brightness 10 %, key light off, on RIFT, unlocked. settings.conf md5
`48752972`, no `timezone`, `screen_off_s`, `auto_lock_s` or `debug_overlay`
key. 0 crash reports, 0 ERROR lines in shell.log. Last input 18:51 UTC.

Hot swap of doors-shell and sysd only (`deploy.sh`), with
`/root/rollback-settings-sys/RESTORE.sh` (both binaries and settings.conf).

## Steps

| # | Step | Result |
| --- | --- | --- |
| 1 | Defaults after the swap: both timeouts never, lock at start on, `sleep: unavailable`, zone UTC not stored, overlay off with no object or timer | PASS |
| 2 | sysd `system.status`: `rx_bytes`/`tx_bytes` on eth0, wlan0, wlan1, sit0 | PASS |
| 3 | Settings opens on the list: nine categories in two panels side by side, every line filled from the device (10 % · Automatic · Large text, Doors · Normal, Volume 60 %, Keyboard base attached · light off, UTC, Connected to <SSID>, Debug overlay off); nothing scrolls at Large in landscape | PASS |
| 4 | Power & Sleep by touch: header title "Power & Sleep"; screen off 5 taps Never → 30 s, stored `screen_off_s=30`, its - disabled at 30 s | PASS |
| 5 | Screen off: off after 31 s idle, the panel black (kmsgrab); a tap on the "Lock when Doors starts" switch only woke it (`lock_screen` unchanged, page unchanged); off again; F1 (Home) only woke it, the next F1 went home | PASS |
| 6 | Lock after 1 min (screen never): unlocked at 50 s, locked at 63 s (`lock: engaged (idle)`) | PASS |
| 7 | Time & Region by touch: UTC shown with the local time; CHANGE TIME ZONE opens the list (two to a line, UTC marked); a tap on New York set `America/New_York`, stored, and the status clock read 16:33 at 20:33 UTC at once, no restart; back to UTC the same way | PASS |
| 8 | System from the list: OVERVIEW (CPU 2 %, 50.7 °C, 894/967 MB, load, uptime, clock synced, storage, Restart and Power off), NETWORK (eth0 and wlan0 with rates and totals, wlan1 down, Wi-Fi connected with signal, LoRa RX with 1165 received · 1 sent · 36 CRC errors and the last packet's RSSI/SNR, mesh online), SERVICES, ABOUT (0.3.0 · 0a12874, the card 2956eff, model, kernel, platform, SDK, CPUs) | PASS |
| 9 | Back: the slab from System to Settings to the launcher; from Display to the list; Home from a page to the launcher; Diagnostics' slab back to SERVICES, then to Settings | PASS |
| 10 | Developer: the switch turns the overlay on, stored `debug_overlay=1`; the line at the foot of the launcher and of Calculator, clear of the header and the status cluster: `CPU 1% · RAM 8% · 51°C · NET ↓1.3 ↑0.0 KB/s · LORA ↓1176 ↑1` | PASS |
| 11 | Overlay cost on the launcher, doors-shell over 30 s: off 1.7 % CPU, RSS 15132 kB, 14 fds; on 1.9 %, 15388 kB, 14 fds | measured |
| 12 | 21 more on/off cycles over IPC: screen objects 55 → 55, RSS 15388 → 15388 kB, fds 14 → 14, no refresh while off, same shell pid | PASS |
| 13 | No shell restart during steps 1-12 (pid 7519 throughout, supervisor restarts 0), 0 ERROR lines, 0 crash reports | PASS |

Seen, not changed: in Calculator (landscape) the overlay lies over the
bottom few pixels of the `0` and `.` keys; it takes no touch, so the keys
still work under it. System's NETWORK page at Large in landscape takes a
short page scroll for the mesh row (as tested on the host).

Not tested on hardware: portrait (unit B had the base attached), Small and
Medium on the panel (host tests cover all three sizes both ways up), system
sleep (not offered).

## Left

settings.conf put back byte-identical (md5 `48752972`) with the shell
stopped, then the shell started: UTC, no zone stored, brightness 10 %, Large,
key light off, overlay off, timeouts never. Unit B stays on doors-shell and
sysd **0a12874**, unlocked at the launcher; `RESTORE.sh` puts back
`d6ffdda` and the v0.3.0 sysd.
