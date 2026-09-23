# K230 workspace (Doors)

Working folder for Doors development on the LILYGO T-Display K230.

Doors was previously known as PocketOS through v0.0.9. Release history,
hardware records and many identifiers (`pos_*`, the `pos-*` helpers,
`pocketui`, `/var/lib/pocketos`, `POCKETOS_*`) keep the old name on purpose; which ones, and when they may change, is in
docs/decisions/ADR-005-product-name-doors.md. The CLI is `doors`, with `pos`
as its permanent alias.

## Layout

```text
AGENTS.md                  Rules for AI agents working here
Makefile                   First-party build (called by the Buildroot package)
VERSION                    Doors version
THIRD_PARTY_NOTICES.txt    Notices for third-party material in Doors binaries and LVGL (generated; installed in /usr/share/doors/, linked from /usr/share/pocketos/)
third_party/notices/       Sources of those notices: the component list and verbatim licence texts
apps/                      In-process apps: radio (radiod client), system, fleet (PocketFleet), radar (PocketRadar), timber (PocketTimber), notes (PocketNotes), clock (PocketClock), calendar (PocketCalendar), calculator (PocketCalculator), settings (Wi-Fi, brightness, appearance), rift (mesh client for meshcored)
ui/pocketui/               Theme engine (pos_theme), shared role styles (pos_styles), widgets, fonts/
ui/shell/                  Shell: status bar, launcher, app host; SDL simulator or DRM target (CMake)
core/pocketipc/            IPC library and server helper: length-prefixed JSON over Unix sockets
core/pocketlog/            Structured logging, rotation and crash reports
core/pocketsys.c           System facts (identity, resources, storage, network) behind system.*
services/radiod/           Radio service: policy, stats, IPC; backends mock and sx1262 (RadioLib, untested on hardware)
services/sysd/             System service: system.info and system.status over pocketipc (read-only), plus the supervisor state reader
services/netd/             Network service: wifi.* over pocketipc (wpa_supplicant and udhcpc owned by netd, root-only credential store)
services/meshcored/        MeshCore protocol service: mesh.* over pocketipc, on top of radiod's radio.*; owns no radio (`make ENABLE_MESHCORED=1 meshcored`, disabled in the image)
protocols/meshcore/        The portable MeshCore protocol core, `libmeshcore.a` (`make meshcore-core`)
tests/                     Native unit tests (`make test`), shell tests (tests/*_shell_test.sh, need the CMake shell); tests/hw/ needs boards
docs/
  ARCHITECTURE.md          How the layers, IPC, services and shell fit together
  ROADMAP.md               Phase 1 status table and later phases
  KNOWN_ISSUES.md          Open hardware, licensing, build and software issues
  api/                     Public API contracts: pocketipc v0, radio.* v0, mesh.* v0, shell.* v0, system.* v0, wifi.* v0 (network.md)
  services/MESHCORED.md    The MeshCore service: ownership, persistence, safety and how to enable it on a unit
  design/                  Design System v0.1 (normative), themes.json, feasibility review, shots/
  BUILD_ENVIRONMENT.md     Host, toolchain, SDK commits, build/flash/test commands
  LICENSING.md             Licence register for vendor and third-party code; Doors' own licence is not decided, and redistribution is not authorised until it is
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted; ADR-003 Wi-Fi credentials: Accepted for the post-v0.0.9 milestone; ADR-004 audio ownership: Accepted for the audio milestone as a narrow exception for Wave; ADR-005 product name Doors: Accepted for Phases 1, 2 and 3)
  hardware/T-DISPLAY-K230.md  Hardware baseline with evidence classification
  hardware/FIRST_BOOT.md   Day-one runbook: flash, console, hwcheck, PocketOS image, link test
  hardware/BRINGUP_CHECKLIST.md  Bench checklist for the first physical session (image, hash, checksum, tests)
platforms/k230/            Defconfig, Buildroot package and apply/build scripts (ADR-001)
tools/pos/                 `doors` CLI (`pos` is the same binary under its old name): system, hardware, network, radio, wifi, logs, app, shell
tools/supervise/           `pos-supervise`: restart with backoff and crash-loop detection
tools/design/              Generators: theme table from themes.json, LVGL fonts from IBM Plex
tools/hwcheck/             `pos-hwcheck`: first-boot hardware inventory script
tools/legal/               `gen_notices.sh`: generates and verifies THIRD_PARTY_NOTICES.txt
tools/meshcore-frame/      `meshcore-frame`: host-side MeshCore wire frames, built and parsed with the real protocol and crypto (`make meshcore-frame`)
vendor/                    Read-only reference clones (git-ignored)
  T-Display-K230/          LILYGO BSP + LVGL launcher, pinned (see platforms/k230/vendor_bsp_commit.txt)
    k230_linux_sdk/        Kendryte K230 Linux SDK submodule, pinned
  T-Display-K230_canmv_rt/ LILYGO RT-Smart firmware, schematic, datasheets
  RIFT/                    MeshCore protocol source, pinned (protocols/meshcore, tools/meshcore-frame)
  Crypto/                  rweather/arduinolibs, the crypto MeshCore uses, pinned (same two)
```

`vendor/` is reference material only. Nothing in it is edited in place.

## Status

**Doors 0.0.11 is in preparation** (VERSION 0.0.11). It is not
released: no image has been built or flashed for it and no unit A gate has
been run on it. The draft release notes are docs/releases/v0.0.11.md, and
what still needs unit A is listed there.

**Doors 0.0.10** (tag `v0.0.10`, 2026-09-16) is the first release under the
Doors name: the release notes are docs/releases/v0.0.10.md. The history below
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
SX1262 transmit (docs/hardware/BRINGUP_SESSION_2026-09-07.md); hardware claims
are classified per statement in docs/hardware/T-DISPLAY-K230.md.

- **0.0.2** is a candidate image, built and checksummed but not yet flashed:
  the SIGPIPE fix, hwcheck `--lora`, no empty-password SSH, radiod at 2 dBm
  start-up power (docs/hardware/V0.0.2_INTEGRATION_REVIEW.md).
- **0.0.3** is platform-only and changes no application source: the image
  becomes a function of a commit (`git archive`, executable bits in git,
  pinned vendor commits enforced), every binary, log and crash report names
  its build, `core/pocketpaths` owns the filesystem roots, the shell's status
  poll no longer waits forever on a wedged service, the shell stops cleanly on
  SIGTERM, init-script `stop` confirms before returning, and radiod holds
  `/dev/spidev0.0` exclusively. Fleet and Radar are byte-for-byte unchanged.
  Validated on unit A on 2026-09-08 (docs/hardware/V0.0.3_OPERATOR_CHECKLIST.md)
  with three scoped defects.
- **0.0.4** fixes exactly those three: every app tick on the LVGL thread
  carries the 200 ms deadline (the Radio app's tick froze the panel while
  radiod was stopped), the supervisor leaves only after its child has (the
  shell stop was reported as forced), and `pos-hwcheck --lora` drives RST
  high and waits for BUSY low as radiod does (the probe read `ff`). Nothing
  else changes; the retest is docs/hardware/V0.0.4_FOCUSED_RETEST.md.
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
  Validated on unit A on 2026-09-09 (docs/hardware/V0.0.5_FOCUSED_RETEST.md):
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
  exit 0 (docs/hardware/V0.0.6_M7_BENCH.md). The earlier `a2 … 24 b4`
  readings were therefore state-dependent, not a framing or transport
  defect; the probe now says so when it happens again, with both
  transports' bytes as evidence. No further investigation unless it
  recurs on hardware.
- **0.0.7** is the core system layer. Its release image is built, flashed and
  validated (`4ab5a55`, docs/hardware/V0.0.7_RELEASE_SMOKE.md, PASS on all
  nine steps); it was merged and tagged `v0.0.7`.
  `core/pocketsys` turns /proc, /sys
  and the mount table into system facts with unknown as null and never as
  zero; `services/sysd` serves `system.info`, `system.status`,
  `system.reboot` and `system.poweroff` (docs/api/system.md); `pos-supervise`
  writes one documented state file per service and sysd is its only reader,
  with `running` requiring the pid to still be the same process; and the
  shell gains the System Status screen, whose presentation logic is pure C
  and host-tested. Both destructive actions reply before they act, go through
  init, and are confirmed at the panel. Validated on unit A across four bench
  sheets and then from the flashed image; the gate that run had to clear is
  docs/hardware/V0.0.7_PRE_RELEASE_CHECKPOINT.md. The image ships with the
  vendor launcher still owning the panel: `/etc/default/k230_phone_ui`
  `ENABLE=0` and `/etc/default/doors-shell` `ENABLE=1` hand it to the shell.

The K230 SD image is built by platforms/k230 (see docs/BUILD_ENVIRONMENT.md
and docs/hardware/FIRST_BOOT.md).
