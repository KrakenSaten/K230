# RIFT UI next — unit A integration gate

**Unit A carries `feat/rift-ui-next` build `d512ba9`** (the branch rebased
onto master 140843e: Doors 0.1.0 + the compact status cluster + this RIFT
work), deployed 2026-09-26 19:19 UTC as the shell binary alone, and is left
on it: rotation mode **Automatic** (keyboard base present, so landscape),
**home**, **locked**, radio `rx`, meshcored online as `Mstr_k230`.
Rollback: `/root/rollback-rift-next/RESTORE.sh` puts back master's shell
`2161e0a` (md5 `4daad5d5…`); `/root/rollback-rift-next/state-before.tar`
holds `/var/lib/pocketos` (logs excluded) as it was before the gate.

**Result: PASS** as a short integration gate, with the limits listed at the
foot. No radio or protocol code changed on this branch, so meshcored and
radiod were not replaced and no RF test beyond one received DM was run.

## What was deployed

Only `/usr/bin/doors-shell` (RIFT is in-process in it): the riscv64 DRM
build of a clean clone of `d512ba9`, stripped, 1,321,664 bytes, md5
`f33b118b…` (sha256 `d6de4ab9…`), copy verified on the unit before it
replaced the old one, restarted through `/etc/init.d/S90doors-shell`,
logged `start version=0.1.0 build=d512ba9`. meshcored (`1368695`), radiod,
netd, sysd, CLI, init scripts, art, settings, the MeshCore identity, radio
and Wi-Fi configuration were not touched; the SD card was not flashed. The
one new file is RIFT's own `/var/lib/pocketos/rift/prefs.v1`, written when
the DM sound switch was first used and left at the default `dm_sound=1`.

## How

Over SSH from WSL with the bench key; scripts, JSON and captures in
`out/rift-ui-next-gate/` (not committed): `g1_deploy.sh`, `g2_open.sh`,
`g3_scroll.sh`, `g4_notify.sh`, `g5_rotate.sh`, `g6_dm.sh`, `g7_health.sh`,
`caps/` (kmsgrab captures of the panel). Taps were injected as the GT9895
reports a finger (`tests/hw/touch_slot0_tap.py`), swipes with the RIFT
gates' `rift_tap.py`; no finger was on the glass.

## Results

The mesh on the unit: **241 nodes** held by meshcored (3 with a learned
route, all direct), 21 messages in 4 conversations (2 channels, `test`, one
direct peer), then 22 after the DM below.

| Check | Landscape (Automatic) | Portrait (stored) |
| --- | --- | --- |
| RIFT opens, `chrome: none, content from y 0, cluster hidden` | ok | ok |
| Header and tabs clear of the corners; no overlap, clipping or stale top padding | ok | ok |
| ACTIVITY: radio, NOTIFY, this device, recently heard, mesh activity (`LAST 5 MIN · RX 1 · TX 0`) | ok | ok |
| NODES: 241 rows, flung end to end to the never-heard nodes | ok | ok |
| NODES: a row selects (expands in place in portrait), selection held after 20 s of traffic | ok | ok |
| Node detail (side pane / pushed), link state, path panel, identity | ok | ok |
| Activity dots after the age read as a pulse (NOW / RECENT / QUIET / STALE), not as signal bars | ok | ok |
| COMMS: list, a channel thread with the newest above the composer, a direct thread | ok | ok |
| A thread scrolled into its history stays there across repaints | ok (8 s) | ok (6 s) |
| DM sound switch: OFF stored (`dm_sound=0`), still OFF after closing and reopening RIFT, back ON | ok | - |

Rotation both ways through `shell.rotation`: Automatic (landscape) →
Portrait → Automatic (landscape), each applied in place (pid kept, "lock:
not engaged at start (rotation restart)"), RIFT reopened and drawn whole
after each.

**One real DM**, unit B (`K230-B`) → unit A over the air, RIFT open on
COMMS: `mesh.send` accepted `direct`, on unit A **exactly one** new
incoming message (id 22, −43 dBm, SNR 12.5) within 1 s; unit B's copy
`acked`. RIFT put `K230-B` at the top of the list with unread **1** and
`COMMS [1]` on the tab; opening it showed one row and cleared the unread.
No WARN from RIFT's notify/sound path, the preference stayed `dm_sound=1`,
and nothing was heard, as the NOTIFY caption says ("No system notification
sound in this build of Doors"). The policy's own counters are not exposed on
the device; one-sound-per-arrival, dedupe and cooldown are host-tested
(`tests/rift_notify_test.c`, `tests/rift_app_test.c`).

**Performance** (injected swipes, `top` sampled at 1 s): `doors-shell` at
up to ~14 % CPU while flinging 241 rows in landscape, up to ~31 % in
portrait (more rows on screen), low single digits between swipes; RSS
17 MB after the gate. No capture showed a half-drawn frame.

**Health** at the end: 0 crash reports; radiod 271, meshcored 391, sysd 235,
netd 984 and doors-shell 3944 kept their pids from deploy to finish; no
supervisor restarts; no meshcored WARN or ERROR; meshcored `online`,
`tx_submitted 4 / tx_ok 4 / tx_failed 0` (the fourth is the ACK for the DM),
`rx_rejected 0 / rx_dropped 0`. One shell WARN, `radio.status poll failed:
timed out after 200 ms`, at the moment that ACK went out: the known
chrome-poll/radiod interaction recorded in MESHCORED_HARDWARE_GATE.md, not
RIFT's.

## Not covered

- A finger on the glass and the owner's eyes: every touch was injected and
  "smooth" is read from CPU samples and captures, not from watching the
  panel.
- A multi-hop path on hardware: no node on this mesh has a learned route of
  more than 0 hops, so the up-to-63-hop path display is host-tested only.
- The DM sound itself: Doors has no notification-sound API (known
  limitation; docs/apps/RIFT.md, "The DM sound").
- 256 nodes: the real mesh gave 241.
