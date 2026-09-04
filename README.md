# K230 workspace (PocketOS)

Working folder for PocketOS development on the LILYGO T-Display K230.

## Layout

```text
AGENTS.md                  Rules for AI agents working here
Makefile                   First-party build (called by the Buildroot package)
VERSION                    PocketOS version
apps/                      In-process apps: radio (radiod client), system
ui/pocketui/               Design tokens and shared widgets (dark UI, cyan accent)
ui/shell/                  Shell: status bar, launcher, app host; SDL simulator or DRM target (CMake)
core/pocketipc/            IPC library and server helper: length-prefixed JSON over Unix sockets
core/pocketlog/            Structured logging, rotation and crash reports
services/radiod/           Radio service: policy, stats, IPC; backends mock (done) and sx1262 (todo)
tests/                     Native unit and end-to-end tests (`make test`); tests/hw/ needs boards
docs/
  ARCHITECTURE.md          How the layers, IPC, services and shell fit together
  ROADMAP.md               Phase 1 status table and later phases
  KNOWN_ISSUES.md          Open hardware, licensing, build and software issues
  api/                     Public API contracts: pocketipc v0, radio.* v0, shell.* v0
  design/POCKETUI.md       Design handoff: tokens, layout, components, modes, motion
  BUILD_ENVIRONMENT.md     Host, toolchain, SDK commits, build/flash/test commands
  LICENSING.md             Licence register for vendor and third-party code
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted)
  hardware/T-DISPLAY-K230.md  Hardware baseline with evidence classification
  hardware/FIRST_BOOT.md   Day-one runbook: flash, console, hwcheck, PocketOS image, link test
platforms/k230/            Defconfig, Buildroot package and apply/build scripts (ADR-001)
tools/pos/                 `pos` CLI: system, hardware, network, radio, logs, app, shell
tools/supervise/           `pos-supervise`: restart with backoff and crash-loop detection
tools/hwcheck/             `pos-hwcheck`: first-boot hardware inventory script
vendor/                    Read-only reference clones (git-ignored)
  T-Display-K230/          LILYGO BSP + LVGL launcher, pinned (see platforms/k230/vendor_bsp_commit.txt)
    k230_linux_sdk/        Kendryte K230 Linux SDK submodule, pinned
  T-Display-K230_canmv_rt/ LILYGO RT-Smart firmware, schematic, datasheets
```

`vendor/` is reference material only. Nothing in it is edited in place.

## Status

PocketOS 0.0.1: repository skeleton per ADR-001; `pos` CLI, `pos-hwcheck`,
pocketipc and radiod (mock backend) build natively and for riscv64, with
`make test` green. The LVGL shell runs as an SDL simulator on the PC with
screenshots in out/sim/; its DRM backend is written but untested. First PocketOS image build is pending the vendor baseline
build. No hardware has been tested yet; the sx1262 backend is not written.
