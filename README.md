# DOORS

**Open. Explore. Connect.**

A complete Linux environment for the LILYGO T-Display K230.

DOORS turns the K230 into a small Linux platform for communication, local AI,
tools, games and experimentation — built around its touchscreen, keyboard,
LoRa radio, camera and hardware AI acceleration.

**[Get started](docs/GETTING_STARTED.md)**: download an image, write a
microSD card and boot DOORS, no development tools needed.

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
  GETTING_STARTED.md       Install a released image: download, write the card, first boot, Wi-Fi, RIFT
  ARCHITECTURE.md          How the layers, IPC, services and shell fit together
  ROADMAP.md               Phase 1 status table and later phases
  releases/                Release notes per version (v0.0.10 on) and HISTORY.md for the earlier releases
  KNOWN_ISSUES.md          Open hardware, licensing, build and software issues
  api/                     Public API contracts: pocketipc v0, radio.* v0, mesh.* v0, shell.* v0, system.* v0, wifi.* v0 (network.md)
  services/MESHCORED.md    The MeshCore service: ownership, persistence, safety and how to enable it on a unit
  design/                  Design System v0.1 (normative), themes.json, feasibility review, shots/
  BUILD_ENVIRONMENT.md     Host, toolchain, SDK commits, build/flash/test commands
  LICENSING.md             Licence register for vendor and third-party code; Doors' own licence is not decided, and redistribution is not authorised until it is
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted; ADR-003 Wi-Fi credentials: Accepted for the post-v0.0.9 milestone; ADR-004 audio ownership: Accepted for the audio milestone as a narrow exception for Wave; ADR-005 product name Doors: Accepted for Phases 1, 2 and 3; ADR-006 to ADR-012 and their status: docs/decisions/README.md)
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
(docs/LICENSING.md items 1 and 10). Release notes, with its fresh-flash
smoke: [docs/releases/v0.3.0.md](docs/releases/v0.3.0.md).

**On master since v0.3.0** (not yet released): RIFT colour emoji, the Public
channel as a standard channel, RIFT MAP and SYSTEM, room for 1000 nodes in
RIFT and meshcored, USB storage in Files, expanding the root filesystem over
the microSD card from System, a freshly flashed card booting straight into
Doors, and the vendor launcher removed from the image.

Earlier releases, from v0.2.1 back to the first PocketOS images:
[docs/releases/HISTORY.md](docs/releases/HISTORY.md).

The K230 SD image is built by platforms/k230 (see docs/BUILD_ENVIRONMENT.md
and docs/hardware/FIRST_BOOT.md).
