# DOORS

**Open. Explore. Connect.**

A complete Linux environment for the LILYGO T-Display K230.

DOORS turns the K230 into a small Linux platform for communication, local AI,
tools, games and experimentation — built around its touchscreen, keyboard,
LoRa radio, camera and hardware AI acceleration.

![DOORS for the LILYGO T-Display K230](docs/images/doors-poster.jpg)

## What is DOORS?

DOORS is the user environment for the T-Display K230: a shell with a
launcher, lock screen and status bar, apps that run inside it, and system
services that own the radio, network and hardware. It runs on the K230's
Linux (Buildroot, based on LILYGO's BSP, ADR-001) and is an independent
project: not LILYGO software and not affiliated with LILYGO.

The aim is to make the hardware useful beyond vendor demos — for mesh
communication, local AI, everyday tools, games and further development.

## Highlights

- **RIFT** — MeshCore messaging: direct messages with delivery acknowledgement,
  channels (Public included), nodes, a map and the radio's own status.
- **MeshCore integration** — `meshcored`, the MeshCore protocol service on the
  SX1262 radio; disabled in the image and enabled per unit
  (docs/services/MESHCORED.md).
- **Fleet** — naval battle, solo against the device or multiplayer between two
  DOORS devices over LoRa, with chat between shots.
- **Wave** — short text messages sent and received as sound.
- **Vision** — camera and KPU: DETECT, TRACK, TRAFFIC (vehicles counted across a
  line), COLOR, EDGE and LINE TRACE; FACE, READ and RECOGNIZE when their models
  are installed (docs/apps/VISION.md).
- **DeskBuddy** — a desk companion with BUDDY, GUARD and NIGHT modes.
- **Terminal** — a real Linux shell with a kept session.
- **Files** — the card and USB drives.
- **Camera and Photo**, **MP3**, **Video** and **Recorder**.
- **Browser**, a read-only **Zabbix** viewer, **Radio**, **Clock**, **Calendar**,
  **Calculator** and **Notes**.
- **Settings** — Wi-Fi, display, six themes, three text sizes and rotation;
  **System** status lives there too.
- **Games** — Fleet, Radar, Timber, Solitaire, Blackjack and 2048.
- Landscape and portrait (Timber is portrait-only).

Feature status per release is under [Status](#status) below.

## Hardware

Built for the **LILYGO T-Display K230** (docs/hardware/T-DISPLAY-K230.md):

- Kendryte K230, RISC-V, Linux; 1 GB RAM
- 4.1" AMOLED, 568 × 1232, touchscreen
- physical keyboard with F-keys (keyboard base)
- GC2093 camera
- SX1262 LoRa radio
- K230 KPU (AI accelerator)
- Wi-Fi, USB-C, microSD storage

> Hardware by LILYGO. DOORS is an independent project built for the T-Display K230.
> The product photos and the LILYGO logo in the poster are LILYGO's.

**Thank you, LILYGO ♥**

## Development

DOORS is meant to be explored and built on. The Terminal is a real Linux
shell with root access on the device. Apps are C modules hosted by the shell
(`apps/`), and services talk over `pocketipc` with documented APIs
(`docs/api/`). The shell also runs as an SDL simulator on a PC, and
`make test` runs the native test suite. Host, toolchain and build commands:
docs/BUILD_ENVIRONMENT.md.

> Root access gives you full control of the system — including the ability to
> break it. Know what a command does before running it.

## Licence

DOORS' own licence is not decided yet: no licence is granted for its code,
and redistribution is not authorised until it is (docs/LICENSING.md).
Third-party components keep their own licences (THIRD_PARTY_NOTICES.txt).

## Repository

Doors was previously known as PocketOS through v0.0.9. Release history,
hardware records and many identifiers (`pos_*`, the `pos-*` helpers,
`pocketui`, `/var/lib/pocketos`, `POCKETOS_*`) keep the old name on purpose; which ones, and when they may change, is in
docs/decisions/ADR-005-product-name-doors.md. The CLI is `doors`, with `pos`
as its permanent alias.

### Layout

```text
AGENTS.md                  Rules for AI agents working here
Makefile                   First-party build (called by the Buildroot package)
VERSION                    Doors version
LICENSE                    Apache License 2.0, the licence of Doors' own code (ADR-013); NOTICE beside it
THIRD_PARTY_LICENSES.md    Inventory of third-party material and how each licence sits with Apache-2.0
MODEL_LICENSES.md          Licence status of every Vision model
CONTRIBUTING.md            Contributions: Apache-2.0, DCO sign-off, source headers
THIRD_PARTY_NOTICES.txt    Notices for third-party material in Doors binaries and LVGL (generated; installed in /usr/share/doors/, linked from /usr/share/pocketos/)
third_party/notices/       Sources of those notices: the component list and verbatim licence texts
apps/                      In-process apps: radio (radiod client), system, fleet (PocketFleet), radar (PocketRadar), timber (PocketTimber), notes (PocketNotes), clock (PocketClock), calendar (PocketCalendar), calculator (PocketCalculator), settings (Wi-Fi, brightness, appearance), rift (mesh client for meshcored), files (file explorer), solitaire, blackjack and 2048 (Pocket Games, docs/apps/PG*.md), deskbuddy (DeskBuddy, docs/apps/DESKBUDDY.md), terminal, vision, browser, camera, photo, video, mp3, recorder, wave, zabbix (docs/apps/)
ui/pocketui/               Theme engine (pos_theme), shared role styles (pos_styles), widgets, fonts/
ui/shell/                  Shell: status cluster, launcher, app host; SDL simulator or DRM target (CMake)
core/pocketipc/            IPC library and server helper: length-prefixed JSON over Unix sockets
core/pocketlog/            Structured logging, rotation and crash reports
core/pocketsys.c           System facts (identity, resources, storage, network) behind system.*
services/radiod/           Radio service: policy, stats, IPC; backends mock and sx1262 (RadioLib)
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
  LICENSING.md             Licence register for vendor and third-party code (Doors itself: Apache-2.0)
  licensing/                Apache-2.0 readiness audit and its blockers, B1 artwork questions, asset inventory, public-source exclusions
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted; ADR-003 Wi-Fi credentials: Accepted for the post-v0.0.9 milestone; ADR-004 audio ownership: Accepted for the audio milestone as a narrow exception for Wave; ADR-005 product name Doors: Accepted for Phases 1, 2 and 3; ADR-006 to ADR-013 and their status: docs/decisions/README.md; ADR-013 licence, Apache-2.0: Accepted)
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

**Doors 0.3.0** (tag `v0.3.0`, 2026-10-02) is the current release: the
Terminal with a kept session, three text sizes and a CLI toolbox; Photo,
Video, MP3, DeskBuddy, Solitaire, Blackjack and 2048; launcher favourites
and folders; the system text size; the keyboard base's own keys; Fleet chat;
RIFT channel and node management. The image is for internal use only
(docs/LICENSING.md items 1 and 10). The release notes, with its fresh-flash
smoke, are docs/releases/v0.3.0.md.

**On master since v0.3.0** (not yet released): RIFT colour emoji, the Public
channel as a standard channel, RIFT MAP and SYSTEM, room for 1000 nodes in
RIFT and meshcored, USB storage in Files, expanding the root filesystem over
the microSD card from System, and the vendor launcher removed from the image.

**Doors 0.2.1** (tag `v0.2.1`, 2026-09-28): v0.2.0
with Vision's model in the image, so Vision works right after a fresh flash.
The model is AGPL-3.0 and the image is for internal use only
(docs/LICENSING.md item 10). The release notes, with its unit B fresh-flash
smoke, are docs/releases/v0.2.1.md.

**Doors 0.2.0** (tag `v0.2.0`, 2026-09-28): Vision
(a KPU detection prototype; the model is installed by hand), RIFT's landscape
COMMS console and traffic graph, Browser, Recorder, Camera's gallery, Wave's
one screen, the compact status cluster, and HDMI output with the first
Doors kernel patches. The release notes, with its unit B smoke, are
docs/releases/v0.2.0.md.

**Doors 0.1.0** (tag `v0.1.0`, 2026-09-26): Camera,
device controls and Diagnostics (SX1262 off by default), Zabbix (a read-only
viewer for an existing Zabbix server, in CONNECTIONS) and Fleet multiplayer
over the mesh. The release notes, with its unit A smoke, are
docs/releases/v0.1.0.md.

**Doors 0.0.12** (tag `v0.0.12`, 2026-09-25): Files, and fullscreen RIFT,
Notes, Wave, Fleet, Radar and Timber. The release notes are
docs/releases/v0.0.12.md and its unit A gate is
docs/hardware/V0.0.12_RELEASE_SMOKE.md.

**Doors 0.0.11** (tag `v0.0.11`, 2026-09-23): release notes
docs/releases/v0.0.11.md, unit A gate docs/hardware/V0.0.11_RELEASE_SMOKE.md.

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

## Licence

Doors is licensed under the Apache License, Version 2.0: see LICENSE and
NOTICE. Third-party components keep their own licences
(THIRD_PARTY_LICENSES.md; the texts the image ships are in
THIRD_PARTY_NOTICES.txt), and the Vision models are documented separately in
MODEL_LICENSES.md. The Doors mark, lockups and boot splash are not
Apache-2.0: they may be redistributed unmodified as part of Doors
(docs/licensing/BRAND.md). Not everything in this repository is Apache-2.0,
and the repository is not yet cleared for publication:
docs/licensing/APACHE_2_READINESS.md says what remains.
