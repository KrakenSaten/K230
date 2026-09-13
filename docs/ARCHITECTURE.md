# PocketOS architecture

Status: reflects the code as of 2026-09-08 (PocketOS 0.0.3, not yet built as
an image; 0.0.1 has run on hardware). Binding decisions live in
docs/decisions/; this file explains how the pieces fit.

## Layers

```text
apps/            In-process apps (radio, system, fleet, radar, timber). Talk to services over pocketipc only;
                 app state under /var/lib/pocketos/<app>/ ($POCKETOS_STATE_DIR).
ui/shell         Shell: status bar, launcher, app host, display/input backend, settings store.
ui/pocketui      Design tokens, theme engine and shared role styles on top of LVGL 9.
services/        Hardware-owning daemons: radiod (mock and sx1262 backends), netd (Wi-Fi: wifi.*);
                 sysd serves system.* (identity, resources, storage, network summary, service health).
core/pocketipc   IPC library used by everything above.
core/pocketlog   Logging, rotation and crash reports.
core/pocketsys   The system facts behind system.*, read from /proc, /sys and /etc.
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
 ├─ S40network           vendor: ifup eth0 (or the vendor wlan0 stanza without Ethernet)
 ├─ S50sysd              pos-supervise sysd
 ├─ S55netd              pos-supervise netd --interface wlan0 (Wi-Fi off until turned on)
 ├─ S60radiod            pos-supervise radiod --backend <mock|sx1262> --region EU868
 ├─ S90pocketos-shell    pos-supervise pocketos-shell (ENABLE=1 in /etc/default/pocketos-shell)
 └─ S99zz_k230_phone_ui  vendor launcher (ENABLE in /etc/default/k230_phone_ui, default 1)
```

Exactly one of the shell and the vendor launcher owns the panel: S90 refuses
to start while the launcher is enabled or running (platforms/k230/README.md,
"Panel ownership"). Both services run under `pos-supervise` (restart with
backoff, one state file per service in /run/pocketos and a crash-loop
marker after five restarts in a minute; nothing displays either yet).

All PocketOS processes run as root in v0. Per-service users are a follow-up.
Runtime state lives in /run/pocketos (sockets, supervisor state files, pid
files, crash-loop markers), settings in /etc/pocketos, app state in /var/lib/pocketos/<app>,
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
pos system status / pos call ── pocketipc ──▶ sysd ──┬─ core/pocketsys ────▶ /proc, /sys, /etc
                                                     └─ sysd_services.c ──▶ /run/pocketos/<name>.state
```

`sysd` (docs/api/system.md) answers `system.info`, `system.status`,
`system.reboot` and `system.poweroff`. It opens no device node. Everything it
reports is read-only; the two actions it can take, it does not take itself.
The facts come from
`core/pocketsys`, unit-tested against a fake root, and every source a board
may lack (thermal zone, power supply, release file, `/data`) reports `null`
rather than a guess. The fake root is a build option and not an environment
switch, so the shipped service reads the real machine whatever its
environment says.

The supervised-service table is not core's. `pos-supervise` writes one
`key=value` state file per service into the runtime directory, atomically, at
every transition it makes; `services/sysd/sysd_services.c` reads those files
and sysd joins them onto the status object, the way it adds `api_version`.
Before v0.0.7 block 2b, core parsed the supervisor's pid files and the free
text of its crash-loop marker, which made a shell script's private layout the
source of a public API field two layers below it.

Validated on unit A from `792f754`: the three services carry the documented
format with distinct supervisor and child pids, 59 reads taken while a
throwaway service died and restarted four times found no partial file, sysd
never exposed a half-formed entry, a stopped service keeps its entry with
`running` false, and a reboot clears the runtime directory so restart counters
start again at zero (docs/hardware/V0.0.7_BLOCK2B_SMOKE.md).

`system.reboot` and `system.poweroff` run `/sbin/reboot` and `/sbin/poweroff`,
BusyBox applets that signal init; init runs `rcK`, which stops S90, S60 and
S50 in reverse order, syncs and remounts the root read-only. sysd never calls
`reboot(2)`, which would skip all of that on a card mounted rw. The reply is
written before the action is recorded and the action runs from the main loop
200 ms later, so a client always sees its answer before the machine goes; the
reply means accepted, not completed, because afterwards there is nothing left
to answer on. One action at a time, refused with code 5 otherwise. There is no
authorization beyond the socket permissions in v0, and docs/api/system.md
says why that is currently sufficient and when it stops being.

`/etc/init.d/S50sysd` starts it under `pos-supervise`, ahead of `S60radiod`,
with S60's stop discipline (the supervise pid and the daemon pid are two
different facts). Validated on unit A from `3a56804`: sysd comes up under
supervision at boot, `system.info` and `system.status` answer with the
board's real values, stop and start are clean without escalation, and a
planted stale pid file was gone after a reboot, which is what makes
`services[].running` trustworthy (docs/hardware/V0.0.7_BLOCK2A_SMOKE.md). The
shell's System Status screen is the client, and it is done: validated on unit
A from `b9203c8` (docs/hardware/V0.0.7_SYSTEM_STATUS_SMOKE.md).

## System Status screen

```text
apps/system/system_app.c  ── LVGL panels, taps
apps/system/system_view.c ── every decision, no LVGL: strings, states, phases
```

The split is what makes it testable. `system_view` turns `system.info` and
`system.status` into the exact strings the panels show, holds the confirm and
terminal phases behind the two destructive actions, and is the only place that
decides what unknown looks like; it has no LVGL in it, so the host suite covers
the parts of a status screen that go wrong - a null read as a zero, a dropped
poll blanking the numbers, an action fired before anyone confirmed it - without
a display.

`system.info` once at create, `system.status` every two seconds, both through
`shell_ipc_call_timeout` with the UI deadline. The radio row reuses the state
the status bar already polls (`pocketos_shell_radio_state`) rather than asking
radiod a second time. The screen reads nothing from `/proc`, `/sys` or `/run`:
it is a client like any other.

Null is the only unknown and renders as a muted em dash; a number, zero
included, is a number. A failed poll changes the freshness line and nothing
else. Services are RUNNING, CRASH LOOP or STOPPED, and a restart count is
always written with its 60-second window. Pseudo interfaces are hidden from the
human view by the six-octet-MAC rule and counted in a caption, never filtered
out of `system.status`. Both power actions take two taps.

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
keys, Wi-Fi credentials or tokens. The one exception is netd's Wi-Fi store,
under docs/decisions/ADR-003-wifi-credentials.md (accepted 2026-09-13):
a root-only file, protected by Unix file permissions and not encrypted, with
no secret in logs, results, command lines or the environment. No other
PocketOS component may persist a secret.

## netd

```text
pos wifi / Settings ── pocketipc ──▶ netd ──┬─ wpa_supplicant (child, control socket, no network in its config)
                                            ├─ udhcpc -f -R (child, one per association)
                                            └─ /var/lib/pocketos/netd/wifi.conf (0600, ADR-003)
```

`netd` (docs/api/network.md) serves `wifi.*` for one wireless interface. It
starts and owns wpa_supplicant and the DHCP client for it and never builds a
shell command: children get argv arrays, networks and passphrases go over the
supplicant's control socket. Every request answers at once and the work is
followed through `wifi.status`, which is the asynchronous service shape the
shell's 200 ms UI deadline needs. The manager reconciles each step against
the supplicant's STATUS rather than trusting events alone, so a missed event
costs one step. Ethernet stays with the vendor's ifupdown. Hardware facts:
docs/hardware/WIFI_2026-09-12.md. Host-tested against a scenario-driven fake
wpa_supplicant (`tests/netd_test.sh`); not yet run on hardware.

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
- Secure credential storage beyond Wi-Fi: ADR-003 (accepted) covers Wi-Fi
  passphrases with file permissions only; encryption at rest waits for a
  device-bound key store, and per-service users are still open.
- Asynchronous radio transmit: `radio.send` blocks the radiod loop for the
  airtime in v0 (docs/api/radio.md).
