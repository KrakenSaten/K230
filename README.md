# K230 workspace (PocketOS)

Working folder for PocketOS development on the LILYGO T-Display K230.

## Layout

```text
AGENTS.md                  Rules for AI agents working here
Makefile                   First-party build (called by the Buildroot package)
VERSION                    PocketOS version
apps/                      In-process apps: radio (radiod client), system, fleet (PocketFleet), radar (PocketRadar)
ui/pocketui/               Theme engine (pos_theme), shared role styles (pos_styles), widgets, fonts/
ui/shell/                  Shell: status bar, launcher, app host; SDL simulator or DRM target (CMake)
core/pocketipc/            IPC library and server helper: length-prefixed JSON over Unix sockets
core/pocketlog/            Structured logging, rotation and crash reports
services/radiod/           Radio service: policy, stats, IPC; backends mock and sx1262 (RadioLib, untested on hardware)
tests/                     Native unit tests (`make test`), shell tests (tests/*_shell_test.sh, need the CMake shell); tests/hw/ needs boards
docs/
  ARCHITECTURE.md          How the layers, IPC, services and shell fit together
  ROADMAP.md               Phase 1 status table and later phases
  KNOWN_ISSUES.md          Open hardware, licensing, build and software issues
  api/                     Public API contracts: pocketipc v0, radio.* v0, shell.* v0
  design/                  Design System v0.1 (normative), themes.json, feasibility review, shots/
  BUILD_ENVIRONMENT.md     Host, toolchain, SDK commits, build/flash/test commands
  LICENSING.md             Licence register for vendor and third-party code
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted)
  hardware/T-DISPLAY-K230.md  Hardware baseline with evidence classification
  hardware/FIRST_BOOT.md   Day-one runbook: flash, console, hwcheck, PocketOS image, link test
  hardware/BRINGUP_CHECKLIST.md  Bench checklist for the first physical session (image, hash, checksum, tests)
platforms/k230/            Defconfig, Buildroot package and apply/build scripts (ADR-001)
tools/pos/                 `pos` CLI: system, hardware, network, radio, logs, app, shell
tools/supervise/           `pos-supervise`: restart with backoff and crash-loop detection
tools/design/              Generators: theme table from themes.json, LVGL fonts from IBM Plex
tools/hwcheck/             `pos-hwcheck`: first-boot hardware inventory script
vendor/                    Read-only reference clones (git-ignored)
  T-Display-K230/          LILYGO BSP + LVGL launcher, pinned (see platforms/k230/vendor_bsp_commit.txt)
    k230_linux_sdk/        Kendryte K230 Linux SDK submodule, pinned
  T-Display-K230_canmv_rt/ LILYGO RT-Smart firmware, schematic, datasheets
```

`vendor/` is reference material only. Nothing in it is edited in place.

## Status

PocketOS 0.0.3, not yet built as an image. Implemented and host-tested:
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

The K230 SD image is built by platforms/k230 (see docs/BUILD_ENVIRONMENT.md
and docs/hardware/FIRST_BOOT.md).
