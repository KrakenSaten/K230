# ADR-002: Application and service model

Status: Accepted (product owner, 2026-09-04)
Date: 2026-09-04
Deciders: product owner (final), AI engineering partner (author)

## Context

PocketOS must run a launcher, status bar, settings and third-party-style
applications (RIFT first) on a 568x1232 AMOLED driven through DRM, with
touch via evdev. Hardware such as the SX1262, Wi-Fi, BLE, camera and NPU
must be owned by services so that apps use generic APIs and never open
`/dev/spidev0.0` or `/dev/gpiochip*` themselves.

The vendor launcher is one process, `k230_phone_ui`, with every app linked
in (254 source files, an 8900-line LoRa app, RadioLib instantiated inside
the UI). It demonstrates what to avoid: a radio bug takes the launcher down,
and hardware access is spread across UI files. DOCUMENTED.

Display constraint: only one process can be DRM master at a time. Two
processes cannot both draw without a compositor. There is no Wayland
compositor in the vendor rootfs; adding one (weston) is a large dependency.

## Options

### A. Monolithic shell: all apps linked into one LVGL process

- Pro: simplest, no IPC for UI, matches the vendor-proven stack, fastest
  app switching, lowest RAM.
- Con: no crash isolation; no way to sandbox or update apps independently;
  the binary grows with every app; violates "Core small and boring" as
  soon as RIFT lands.

### B. Fully multi-process with a compositor

Each app is a process; a compositor owns the display; services are daemons.

- Pro: strongest isolation; standard desktop-Linux model.
- Con: a compositor plus a client toolkit is a large dependency and RAM
  cost on this device; LVGL has no first-class Wayland client path that is
  proven on K230. Premature for v0.1.

### C. Shell plus out-of-process apps with DRM master handoff (recommended)

- The PocketOS shell (launcher, status bar, settings, notifications) is one
  LVGL process and normally holds DRM master.
- Hardware-owning services are separate daemons from day one: `radiod`
  (SX1262 via RadioLib), `netd` (Wi-Fi/Ethernet/BLE), later `camerad`,
  `aid`. Apps and the shell talk to them over Unix-domain sockets with a
  small versioned message protocol (`pocketipc`). The same protocol is
  reused by the CLI (`pos radio status`).
- First-party v0.1 apps run in-process in the shell behind a defined App
  API (create, resume, pause, destroy, input, tick). This keeps v0.1 small.
- Large or third-party apps (RIFT, games, Labs) run as separate processes:
  the shell launches them, drops DRM master, the app takes master and draws
  full-screen with the shared `libpocketui`, and the shell regains master
  when the app exits or is suspended. Status-bar content is provided to
  such apps through `libpocketui`, not by overlaying a second process.
- Pro: hardware isolation from the start (the part that is expensive to
  retrofit); UI isolation where it matters (RIFT, games); no compositor.
- Con: two app hosting modes to maintain; DRM master handoff must be robust
  (app crash must return the display to the shell); app switching between
  out-of-process apps is a full handoff, not instantaneous.

## Decision

Option C, with these fixed points:

1. Public APIs are IPC contracts, not C headers into service internals.
   Names follow the charter: `radio.*`, `network.*`, `display.*`,
   `input.*`, `storage.*`, `camera.*`, `ai.*`, `serial.*`. Each API has a
   version and a documented message set in `docs/api/`.
2. Services own hardware exclusively. `radiod` is the only process that
   opens the SX1262. A debugging escape hatch (developer mode) may stop a
   service so a Labs tool can take the hardware, and must restore it.
3. The App API is the same for in-process and out-of-process apps, so an
   app can be moved out of process without a rewrite.
4. `libpocketui` (LVGL plus the PocketUI design system, ADR later) is the
   only UI toolkit for first-party apps.
5. Crash policy: a crashed out-of-process app returns control to the shell
   with a user-visible notice and a crash report; a crashed service is
   restarted by a supervisor with backoff and crash-loop detection (the
   supervisor design is a later ADR; v0.1 may use a simple init-script
   respawn).
6. Out-of-process apps are a defined path, not a v0.1 deliverable. The
   DRM master handoff prototype is scheduled only when the first large
   app needs it. RIFT is not on the current priority list (product owner,
   2026-09-04).

## Consequences

Needed now (v0.1): shell process with App API; `radiod` and `netd` with
`pocketipc`; `pos` CLI over the same IPC.

Useful soon: DRM master handoff prototype for out-of-process apps; app manifest (name, version, permissions, entry point),
installation directory layout, structured logging shared by shell and
services, crash reports.

Future: app sandboxing (separate users, seccomp), package format and updates
(ties into the update ADR), a compositor if overlapping windows are ever
required.

Risks: DRM master handoff on the Canaan DRM driver is unproven. The vendor
BSP patches the driver for boot-splash handoff and rotation, which suggests
it is not a vanilla driver. This must be prototyped early; if handoff is
unreliable, fallback is a shell-owned framebuffer that apps render into via
shared memory, which costs one extra copy per frame.

Migration cost if the owner prefers Option A for v0.1: none for services
and IPC; only the out-of-process app path and DRM handoff are dropped.

## Evidence

- Vendor monolith structure and hardware access from UI code: DOCUMENTED
  (launcher source, CMakeLists.txt, ui_lora.cpp).
- DRM single-master constraint: DOCUMENTED (Linux DRM semantics).
- Handoff behaviour on the Canaan DRM driver: ASSUMED, must be VERIFIED on
  hardware before the shell design is frozen.
- RAM budget for two LVGL processes: ASSUMED acceptable, unmeasured.
