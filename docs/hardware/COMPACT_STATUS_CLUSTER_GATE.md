# Compact status cluster — unit A chrome/layout smoke

**Unit A carries `feat/compact-status-cluster` build `64416ab`** (Doors 0.1.0
+ DS §36), deployed 2026-09-26 as the shell binary alone, and is left on it:
rotation mode **Automatic** (keyboard base present, so landscape), **locked**,
radio `rx`. Rollback: `/root/rollback-cluster/RESTORE.sh` puts back the
v0.1.0 `doors-shell` (1368695, md5 `ab2eb9e2…`).

**Result: PASS** as a chrome/layout smoke (DS §36.7), with the limits listed
at the foot. Not a feature gate: no app was exercised beyond opening it.

## What was deployed

Only `/usr/bin/doors-shell` (every app is in-process in it): the riscv64 DRM
build of the clean clone of `64416ab`, stripped, 1,305,272 bytes, md5
`54856d62…`, copy verified on the unit before it replaced the old one.
Services, CLI, init scripts, runtime art and settings were not touched; the
SD card was not flashed. The shell was restarted through
`/etc/init.d/S90doors-shell` and logged `start version=0.1.0 build=64416ab`.

## How

Over SSH from WSL with the bench key; scripts and captures in
`out/compact-status-cluster/gate/` (not committed): `g1_deploy.sh`,
`g2_smoke.sh <orientation>`, `g3_rotate.sh <mode>`, `g4_leave.sh`,
`frame.py`, and `caps/` (kmsgrab captures of the panel). Touch was injected
as the GT9895 reports a finger (`tests/hw/touch_slot0_tap.py`, and a
vertical swipe for the lock); no finger was on the glass.

Every screen was measured from `doors shell info` against the same rules as
`tests/chrome_shell_test.sh` (`frame.py`): the content area from the top
edge over the whole display; the cluster shown or hidden as the policy
says, in the top-right corner, its right edge 30 px (portrait) / 50 px
(landscape) from the edge, inside the screen, as wide as its content, the
chip's text given its whole line; every app header at row 0, 72 px, the
body straight under it at row 72, both ends clear of the corners, title and
hint ending before the cluster; on the launcher the time, the date and
every cell clear of the cluster.

## Results

| Step | Landscape (Automatic) | Portrait (stored) |
| --- | --- | --- |
| Lock: cluster shown, chip only (no clock) | ok | ok |
| Swipe up (injected) opens the lock | ok | ok |
| Launcher: time/date/cells clear of the cluster | ok | ok |
| Controls: nothing under the cluster; a tap on the cluster does nothing | ok | ok |
| System opened by a tap on its cell, closed by a tap on the back slab | ok | ok |
| Settings, Clock, Calendar, Calculator, Files, Radio: cluster with clock, header/body at 0/72, nothing under the cluster | ok | ok |
| RIFT, Wave, Camera, Fleet, Zabbix, Notes, Radar, Timber: fullscreen, no cluster | ok | ok |
| Lock over RIFT shows the cluster; unlocked, it goes | ok | ok |
| `frame.py` rules | 140 ok, 0 FAIL | 140 ok, 0 FAIL |
| Service pids (radiod 271, meshcored 391, sysd 235, netd 984, doors-shell 1569) | unchanged | unchanged |
| Crash reports / shell WARN or ERROR | 0 / 0 | 0 / 0 |

Rotation both ways through `shell.rotation`: Automatic (landscape) →
Portrait → Automatic (landscape), each applied in place (the shell kept pid
1569, "lock: not engaged at start (rotation restart)"), 8 of 8 rules on the
launcher after the return. The radio chip read `RX` in the radio's colour
throughout (radiod `rx`); the clock was the wall clock (NTP-synced).

## Not covered

- A finger on the glass: every touch was injected. The owner's own look and
  feel of the panel is the part of §36.7 still to do.
- The chip in TX and OFF on hardware: nothing was transmitted and the radio
  was not switched off for this smoke. Both states are drawn and measured in
  the simulator (`tests/chrome_shell_test.sh`, section 4), and the chip code
  is v0.1.0's.
- Wi-Fi, Bluetooth and Ethernet are not in the cluster (owner's decision,
  DS §36.6); nothing to check.
