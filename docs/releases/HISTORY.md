# Release history

The current release and what is on master since it are in the
[README](../../README.md#status). This page keeps the earlier releases, newest
first, as they were described at the time. The full notes of every release
from v0.0.10 on are next to this file; the earlier bench and gate records are
in [docs/hardware/](../hardware/).

**Doors 0.3.0** (tag `v0.3.0`, 2026-10-02): the
Terminal with a kept session, three text sizes and a CLI toolbox; Photo,
Video, MP3, DeskBuddy, Solitaire, Blackjack and 2048; launcher favourites
and folders; the system text size; the keyboard base's own keys; Fleet chat;
RIFT channel and node management. The image is for internal use only
(docs/LICENSING.md items 1 and 10). Release notes, with its fresh-flash
smoke: [v0.3.0.md](v0.3.0.md).

**Doors 0.2.1** (tag `v0.2.1`, 2026-09-28): v0.2.0
with Vision's model in the image, so Vision works right after a fresh flash.
The model is AGPL-3.0 and the image is for internal use only
([LICENSING.md](../LICENSING.md) item 10). The release notes, with its unit B fresh-flash
smoke, are [v0.2.1.md](v0.2.1.md).

**Doors 0.2.0** (tag `v0.2.0`, 2026-09-28): Vision
(a KPU detection prototype; the model is installed by hand), RIFT's landscape
COMMS console and traffic graph, Browser, Recorder, Camera's gallery, Wave's
one screen, the compact status cluster, and HDMI output with the first
Doors kernel patches. The release notes, with its unit B smoke, are
[v0.2.0.md](v0.2.0.md).

**Doors 0.1.0** (tag `v0.1.0`, 2026-09-26): Camera,
device controls and Diagnostics (SX1262 off by default), Zabbix (a read-only
viewer for an existing Zabbix server, in CONNECTIONS) and Fleet multiplayer
over the mesh. The release notes, with its unit A smoke, are
[v0.1.0.md](v0.1.0.md).

**Doors 0.0.12** (tag `v0.0.12`, 2026-09-25): Files, and fullscreen RIFT,
Notes, Wave, Fleet, Radar and Timber. The release notes are
[v0.0.12.md](v0.0.12.md) and its unit A gate is
[V0.0.12_RELEASE_SMOKE.md](../hardware/V0.0.12_RELEASE_SMOKE.md).

**Doors 0.0.11** (tag `v0.0.11`, 2026-09-23): release notes
[v0.0.11.md](v0.0.11.md), unit A gate [V0.0.11_RELEASE_SMOKE.md](../hardware/V0.0.11_RELEASE_SMOKE.md).

**Doors 0.0.10** (tag `v0.0.10`, 2026-09-16) is the first release under the
Doors name: the release notes are [v0.0.10.md](v0.0.10.md). The history below
was written under the PocketOS name and stops at 0.0.7.

PocketOS 0.0.6, the third focused release after the v0.0.3 and v0.0.4
hardware sessions. Implemented and host-tested:
`pos` CLI, `pos-hwcheck`, pocketipc, pocketlog, pocketpaths, `pos-supervise`,
radiod with the mock backend and the sx1262 backend (RadioLib on spidev +
libgpiod), the LVGL shell with Design System v0.1 (theme engine, five themes,
three modes, settings store), and two apps, PocketFleet and PocketRadar. The
shell runs as an SDL simulator on the PC with screenshots in out/sim/, and as
DRM + evdev on the board.

PocketOS 0.0.1 booted and ran on a physical K230 on 2026-09-07, including one
SX1262 transmit ([BRINGUP_SESSION_2026-09-07.md](../hardware/BRINGUP_SESSION_2026-09-07.md)); hardware claims
are classified per statement in [T-DISPLAY-K230.md](../hardware/T-DISPLAY-K230.md).

- **0.0.2** is a candidate image, built and checksummed but not yet flashed:
  the SIGPIPE fix, hwcheck `--lora`, no empty-password SSH, radiod at 2 dBm
  start-up power ([V0.0.2_INTEGRATION_REVIEW.md](../hardware/V0.0.2_INTEGRATION_REVIEW.md)).
- **0.0.3** is platform-only and changes no application source: the image
  becomes a function of a commit (`git archive`, executable bits in git,
  pinned vendor commits enforced), every binary, log and crash report names
  its build, `core/pocketpaths` owns the filesystem roots, the shell's status
  poll no longer waits forever on a wedged service, the shell stops cleanly on
  SIGTERM, init-script `stop` confirms before returning, and radiod holds
  `/dev/spidev0.0` exclusively. Fleet and Radar are byte-for-byte unchanged.
  Validated on unit A on 2026-09-08 ([V0.0.3_OPERATOR_CHECKLIST.md](../hardware/V0.0.3_OPERATOR_CHECKLIST.md))
  with three scoped defects.
- **0.0.4** fixes exactly those three: every app tick on the LVGL thread
  carries the 200 ms deadline (the Radio app's tick froze the panel while
  radiod was stopped), the supervisor leaves only after its child has (the
  shell stop was reported as forced), and `pos-hwcheck --lora` drives RST
  high and waits for BUSY low as radiod does (the probe read `ff`). Nothing
  else changes; the retest is [V0.0.4_FOCUSED_RETEST.md](../hardware/V0.0.4_FOCUSED_RETEST.md).
  On unit A (2026-09-08) M6 passed, M5 and M7 each exposed a second defect.
- **0.0.5** closes the M5 second cause: the UI deadline covers connecting
  as well (with radiod stopped, the shell's own abandoned connections
  filled radiod's listen backlog and the reconnect blocked with no
  deadline). `pos-hwcheck --lora` also gained a readiness gate (registers
  read only after the chip reports standby on two consecutive polls), which
  is correct but did not fix the probe's register read: on unit A the read
  still returns `24 b4` with the chip provably in standby, so that is a
  framing or decoding defect in the probe's hand-built ReadRegister, not in
  radiod and not in the SX1262, which radiod initialises correctly right
  afterwards. It is a diagnostic-tool defect, open, not a runtime blocker.
  Validated on unit A on 2026-09-09 ([V0.0.5_FOCUSED_RETEST.md](../hardware/V0.0.5_FOCUSED_RETEST.md)):
  M5 PASS, M6 PASS, M7 probe read FAIL with the radio PASS, smoke and
  reboot persistence PASS.
- **0.0.6** is diagnostic hardening of `pos-hwcheck --lora` only; radiod,
  RadioLib, the shell and the apps are byte-for-byte unchanged. The probe
  sends the SX1262 commands through a 120-line helper, `pos-spixfer`, that
  performs the transaction exactly as radiod's HAL does (one CS-framed
  `SPI_IOC_MESSAGE`, mode 0, 8 bits, 4 MHz, `O_RDWR`, `flock`), captures
  the register window at 6 and 8 bytes, and, when spi-pipe is installed,
  reads the window once more through it for comparison. It is not a fix
  for the `24 b4` read: on unit A on 2026-09-09 pos-spixfer at 4 MHz,
  spi-pipe at 1 MHz and spi-pipe at 4 MHz all returned `aa aa aa aa 14 24`
  after one power-and-reset cycle, and the probe reported VERIFIED with
  exit 0 ([V0.0.6_M7_BENCH.md](../hardware/V0.0.6_M7_BENCH.md)). The earlier `a2 … 24 b4`
  readings were therefore state-dependent, not a framing or transport
  defect; the probe now says so when it happens again, with both
  transports' bytes as evidence. No further investigation unless it
  recurs on hardware.
- **0.0.7** is the core system layer. Its release image is built, flashed and
  validated (`4ab5a55`, [V0.0.7_RELEASE_SMOKE.md](../hardware/V0.0.7_RELEASE_SMOKE.md), PASS on all
  nine steps); it was merged and tagged `v0.0.7`.
  `core/pocketsys` turns /proc, /sys
  and the mount table into system facts with unknown as null and never as
  zero; `services/sysd` serves `system.info`, `system.status`,
  `system.reboot` and `system.poweroff` ([system.md](../api/system.md)); `pos-supervise`
  writes one documented state file per service and sysd is its only reader,
  with `running` requiring the pid to still be the same process; and the
  shell gains the System Status screen, whose presentation logic is pure C
  and host-tested. Both destructive actions reply before they act, go through
  init, and are confirmed at the panel. Validated on unit A across four bench
  sheets and then from the flashed image; the gate that run had to clear is
  [V0.0.7_PRE_RELEASE_CHECKPOINT.md](../hardware/V0.0.7_PRE_RELEASE_CHECKPOINT.md). The image ships with the
  vendor launcher still owning the panel: `/etc/default/k230_phone_ui`
  `ENABLE=0` and `/etc/default/doors-shell` `ENABLE=1` hand it to the shell.
