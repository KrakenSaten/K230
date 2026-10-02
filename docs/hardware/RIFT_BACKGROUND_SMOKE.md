# RIFT kept in the background - unit B smoke (2026-10-02)

**Unit under test: unit B (192.168.10.187), Doors v0.3.0 image `2956eff`
with `/usr/bin/doors-shell` hot-swapped to build `3353659`** (branch
`feat/rift-background-lifecycle`, riscv64 DRM shell, md5
`4478dba35833c6207d396895c919f730`). Nothing else on the unit was changed:
meshcored, radiod and every helper are v0.3.0's. Landscape 1232 x 568, text
size Large (the owner's setting on this unit), meshcored online, sx1262 in
rx. Rollback: `/root/rollback-rift-bg/RESTORE.sh` (the v0.3.0 shell, md5
`4f940c27b5385e4f509efb1f81fd02f4`, and settings.conf as found).

A short smoke, not a full gate (DS §51). Driven over SSH: `doors app start`,
`shell.action back`, `shell.home`, and slot-0 finger taps and drags on the
touch controller (tests/hw/touch_slot0_tap.py, out/rift-gate/swipe.py);
panel captures by kmsgrab. Kit: `C:\K230\out\rift-bg-gate` (not in the repo).

| Step | Result |
| --- | --- |
| Open RIFT | `rift: open, meshcored connected`; no mark (RIFT on screen) |
| Leave with Back (at ACTIVITY Back is the shell's: home) | launcher; `shell.info` `background` = `rift / RIFT / RIFT active in background`, `background_mark` true; the cluster reads `RX` then `RIFT` (capture) |
| Another app (Settings) | mark still up |
| Reopen | `open again (session kept, open 2), meshcored connected`; mark gone; same ACTIVITY; the activity feed kept counting across the leaves (4, then 12, then 13 frames "since RIFT opened") |
| Tabs by finger | 4 px under the strip's top: NODES; in the gap after COMMS's pill: COMMS (the nearer tab); the strip's foot: COMMS; ACTIVITY |
| Back slab by finger | the strip's top row (y 50) and its foot row at the slab's left reach (x 22, y 105): home, session kept. Measured on the panel: strip rows 50-105, slab 52-103 (52 px), 64 px wide. A tap at y 49, above the strip in the corner clearance, does nothing - as designed |
| CLOSE RIFT | SESSION panel whole at Large; the press disables it and shows *Close RIFT?* with CANCEL accented; confirmed: launcher, `rift: session ended after 7 open(s)`, `background` empty, mark gone |
| meshcored after CLOSE | same pid (397), `online` - untouched |
| Open after CLOSE | `rift: open, meshcored connected` (a new session); Back: home, mark up again |
| Process | doors-shell pid 4876 from deploy to the end (no crash, no restart); RSS 17.0-17.2 MB, 13 fds, 1 thread; 0 ERROR/assert lines (two keyboard-controller WARNs from before the deploy) |

Not covered here: portrait on the panel (host-tested in both orientations),
a direct message arriving while RIFT is left on air (host-tested against the
scripted meshcored; a channel message did arrive during the run), a long
soak, and how the larger targets feel to a finger - the owner's.

**Unit B was left on doors-shell `3353659`** with the RIFT session running in
the background. Note: that shell carries master's Zabbix connection settings
(a24e86c), whose helper the unit's v0.3.0 `pos-zabbix` predates, so Zabbix's
CONNECTION screen is not expected to work on this unit until it is restored or
updated. Restore with `/root/rollback-rift-bg/RESTORE.sh`.

## Visible navigation controls (2026-10-02, doors-shell `0600641`)

The owner found the first pass's navigation unchanged to the eye (only the row
and the targets had grown). **Unit B now runs doors-shell `0600641`** (md5
`05381d601999ad090c68b771713ef932`), same rollback. Landscape, Large:

| | master | first pass (`3353659`) | now (`0600641`) |
| --- | --- | --- | --- |
| Row | 36 px | 56 px | 64 px |
| Back | 56 x 32 slab | 64 x 52 slab | 72 x 56 face |
| Tabs | caption words | caption words, 56 px targets | 56 px faces, button type |

Captured on the panel: the five faces in one look on ACTIVITY, NODES, COMMS
and NET; a finger tap on each tab switched to it; Back went home with the RIFT
mark up; reopening was the kept session (`open again ... open 2`). doors-shell
pid unchanged, 0 ERROR/assert lines. Seen, not changed: NET's ring column
headings overlap at Large in landscape (NET's own layout; not compared with
master here).
