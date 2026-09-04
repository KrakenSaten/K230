# PocketOS architecture

Status: reflects the code as of 2026-09-04 (PocketOS 0.0.1). Binding
decisions live in docs/decisions/; this file explains how the pieces fit.

## Layers

```text
apps/            In-process apps (radio, system). Talk to services over pocketipc only.
ui/shell         Shell: status bar, launcher, app host, display/input backend.
ui/pocketui      Design tokens and shared widgets on top of LVGL 9.
services/        Hardware-owning daemons: radiod (done, mock backend), netd (planned).
core/pocketipc   IPC library used by everything above.
tools/           pos CLI, pos-hwcheck.
platforms/k230   Buildroot integration on the pinned LILYGO BSP + Kendryte SDK.
vendor/          Read-only upstream trees (git-ignored): LILYGO BSP, K230 SDK, LVGL.
```

Rules (ADR-001, ADR-002):

- Core is small and boring. New capability goes into a service or an app.
- A service owns its hardware exclusively. Apps and the shell never open
  device nodes; they call `<service>.<method>` over pocketipc.
- Public APIs are the IPC contracts in docs/api/, named generically
  (`radio.*`, `network.*`), versioned by `api_version`.
- K230 specifics stay below the service backend boundary
  (`services/radiod/backend_*.c`, `ui/shell/platform_*.c`, `platforms/k230`).

## Process model on the device

```text
BusyBox init
 ├─ S40<conf>          vendor: Wi-Fi driver modprobe
 ├─ S60radiod          radiod --backend <mock|sx1262> --region EU868
 ├─ S99zz_k230_phone_ui vendor launcher (temporary, until the shell replaces it)
 └─ (planned) S90pocketos-shell
```

All PocketOS processes run as root in v0. Per-service users are a follow-up.
Runtime state lives in /run/pocketos (sockets), logs in /var/log.

## IPC

pocketipc (docs/api/pocketipc.md): Unix-domain sockets, 4-byte length +
JSON, request/response with integer ids, events for subscribed clients.
Services use a non-blocking poll loop with an incremental frame reader so a
stalled client cannot block the daemon. Clients use the blocking helper
`pocketipc_call`. cJSON is the only dependency.

## radiod

```text
pos radio / apps  ── pocketipc ──▶ radiod ──▶ backend ops ──▶ mock | sx1262 (todo)
                                     │
                                     ├─ region guard (EU868: 863-870 MHz, ≤14 dBm)
                                     ├─ profile validation (SF, BW set, CR, sync, preamble)
                                     ├─ airtime accounting (60 one-minute buckets)
                                     └─ events: radio.rx, radio.tx_done, radio.state
```

The backend interface is `services/radiod/radio_backend.h`. The sx1262
backend will use RadioLib (upstream, MIT) with a PocketOS HAL on spidev and
libgpiod v2. The LILYGO launcher's HAL cannot be reused (no licence).

## Shell

One LVGL process. The status bar polls radiod once per second. Apps
implement `struct pocketos_app` (create / tick / destroy) and are built into
the shell binary for v0.1; the same API is intended for out-of-process apps
later (ADR-002). Display backends: SDL (simulator, WSLg) and DRM + evdev
(K230, untested). `--screenshot` renders any screen headlessly to PNG.

## Build

The repository root Makefile builds the C tools and services; `ui/shell`
uses CMake because LVGL does. On the device, Buildroot builds everything via
`platforms/k230/package/pocketos` on top of the vendor defconfig. See
docs/BUILD_ENVIRONMENT.md.

## Not yet decided

- Service supervision and crash-loop handling (init script respawn for now).
- Update and rollback mechanism (partition layout must not be hard-coded).
- First-party licence.
- Out-of-process app hosting and DRM master handoff.
