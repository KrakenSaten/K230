# PocketOS architecture

Status: reflects the code as of 2026-09-08 (PocketOS 0.0.3, not yet built as
an image; 0.0.1 has run on hardware). Binding decisions live in
docs/decisions/; this file explains how the pieces fit.

## Layers

```text
apps/            In-process apps (radio, system, fleet, radar). Talk to services over pocketipc only;
                 app state under /var/lib/pocketos/<app>/ ($POCKETOS_STATE_DIR).
ui/shell         Shell: status bar, launcher, app host, display/input backend, settings store.
ui/pocketui      Design tokens, theme engine and shared role styles on top of LVGL 9.
services/        Hardware-owning daemons: radiod (mock and sx1262 backends), netd (planned);
                 sysd serves system.* (identity, resources, storage, network summary, service health).
core/pocketipc   IPC library used by everything above.
core/pocketlog   Logging, rotation and crash reports.
core/pocketsys   The system facts behind system.*, read from /proc, /sys, /etc and the runtime dir.
tools/           pos CLI, pos-hwcheck, pos-supervise.
platforms/k230   Buildroot integration on the pinned LILYGO BSP + Kendryte SDK.
vendor/          Read-only upstream trees (git-ignored): LILYGO BSP, K230 SDK, LVGL, RadioLib, libgpiod.
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
BusyBox init (rcS runs S?? scripts in order; rcK stops them in reverse)
 ├─ S40<conf>            vendor: Wi-Fi driver modprobe
 ├─ S60radiod            pos-supervise radiod --backend <mock|sx1262> --region EU868
 ├─ S90pocketos-shell    pos-supervise pocketos-shell (ENABLE=1 in /etc/default/pocketos-shell)
 └─ S99zz_k230_phone_ui  vendor launcher (ENABLE in /etc/default/k230_phone_ui, default 1)
```

Exactly one of the shell and the vendor launcher owns the panel: S90 refuses
to start while the launcher is enabled or running (platforms/k230/README.md,
"Panel ownership"). Both services run under `pos-supervise` (restart with
backoff, crash-loop marker in /run/pocketos after five restarts in a
minute; nothing displays the marker yet).

All PocketOS processes run as root in v0. Per-service users are a follow-up.
Runtime state lives in /run/pocketos (sockets, pid files, crash-loop
markers), settings in /etc/pocketos, app state in /var/lib/pocketos/<app>,
logs and crash reports in /var/lib/pocketos/log (persistent; /var/log is a
tmpfs on the image).

## IPC

pocketipc (docs/api/pocketipc.md): Unix-domain sockets, 4-byte length +
JSON, request/response with integer ids, events for subscribed clients.
Services use a non-blocking poll loop with an incremental frame reader so a
stalled client cannot block the daemon. Clients use the blocking helper
`pocketipc_call`, or `pocketipc_call_timeout` where waiting forever is the
wrong answer: from 0.0.3 the shell's once-a-second status poll carries a
200 ms deadline, so a service that is alive but not answering costs one frame
instead of the session. Calls whose completion is the point, `radio.send`
above all, still wait. cJSON is the only dependency.

## radiod

```text
pos radio / apps  ── pocketipc ──▶ radiod ──▶ backend ops ──▶ mock | sx1262 (untested on hardware)
                                     │
                                     ├─ region guard (EU868: 863-870 MHz, ≤14 dBm)
                                     ├─ profile validation (SF, BW set, CR, sync, preamble)
                                     ├─ airtime accounting (60 one-minute buckets)
                                     └─ events: radio.rx, radio.tx_done, radio.state
```

The backend interface is `services/radiod/radio_backend.h`. The sx1262
backend uses RadioLib (upstream, MIT) with a PocketOS HAL on spidev and
libgpiod v2 (`hal_linux.cpp`); it compiles for riscv64 and has not run on
hardware. The LILYGO launcher's HAL cannot be reused (no licence).

## sysd

```text
pos system status / pos call ── pocketipc ──▶ sysd ──▶ core/pocketsys ──▶ /proc, /sys, /etc, /run/pocketos
```

`sysd` (docs/api/system.md) answers `system.info` and `system.status`. It
is read-only in v0: no device node, no action. The facts come from
`core/pocketsys`, unit-tested against a fake root, and every source a board
may lack (thermal zone, power supply, release file, `/data`) reports `null`
rather than a guess. The supervised-service table is read from the pid files
and crash-loop markers `pos-supervise` already writes. Not yet started on the
device: the `S50sysd` init script and the shell's System Status screen are
the next two steps of v0.0.7.

## Theme engine (Design System v0.1)

`ui/pocketui/pos_theme.c` is pure C: five Normal-mode base tables generated
from `docs/design/themes.json`, derived tokens (§4), Outdoor and Night rules
(§6), invariants and fallback (§8). `pos_styles.c` turns the current tokens
into one shared LVGL style per role; a theme or mode change rewrites those
styles and calls `lv_obj_report_style_change`, so every widget follows
without being touched. Rule enforced by `tests/style_lint.sh`: no colour
literal, colour style call or font symbol outside `pos_theme.c`,
`pos_styles.c` and the generated fonts. Apps and the shell add role styles
only. Selection is persisted by the shell in `/etc/pocketos/settings.conf`
(`theme`, `display_mode`), read before the first frame; invalid stored values
fall back to `ice` + `normal`, are logged, and are left untouched.

The settings store is for non-secret preferences only. It is plain text,
world-readable and unauthenticated, and must never hold passwords, private
keys, Wi-Fi credentials or tokens. Credential storage is an open design item
(below); until it exists, no PocketOS component may persist a secret.

## Shell

One LVGL process. The status bar polls radiod once per second. Apps
implement `struct pocketos_app` (create / tick / destroy) and are built into
the shell binary for v0.1; the same API is intended for out-of-process apps
later (ADR-002). Display backends: SDL (simulator, WSLg) and DRM + evdev
(K230, untested; links the vendor-patched LVGL from the Buildroot package).
`--screenshot` renders any screen headlessly to PNG in the simulator only:
the target LVGL build has no snapshot support (docs/KNOWN_ISSUES.md).

## Build

The repository root Makefile builds the C tools and services; `ui/shell`
uses CMake because LVGL does. On the device, Buildroot builds everything via
`platforms/k230/package/pocketos` on top of the vendor defconfig. See
docs/BUILD_ENVIRONMENT.md.

## Not yet decided

- Surfacing service health in the shell. `system.status.services` now
  carries the pos-supervise crash-loop marker; the shell does not show it
  yet, and radiod state `error` is still only in the radio chip.
- Update and rollback mechanism (partition layout must not be hard-coded).
- First-party licence.
- Out-of-process app hosting and DRM master handoff.
- Secure credential storage (Wi-Fi passwords, keys): threat model, key
  storage and access control before any secret is persisted.
- Asynchronous radio transmit: `radio.send` blocks the radiod loop for the
  airtime in v0 (docs/api/radio.md).
