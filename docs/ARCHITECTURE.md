# Doors architecture

Status: reflects the code as of 2026-09-08 (PocketOS 0.0.3, not yet built as
an image; 0.0.1 has run on hardware). Binding decisions live in
docs/decisions/; this file explains how the pieces fit.

Doors was previously known as PocketOS through v0.0.9. The paths, services
and C identifiers below keep their PocketOS-era names
(docs/decisions/ADR-005-product-name-doors.md); the CLI is `doors`, with `pos`
as its alias.

## Layers

```text
apps/            In-process apps (radio, system, fleet, radar, timber). Talk to services over pocketipc only;
                 app state under /var/lib/pocketos/<app>/ ($POCKETOS_STATE_DIR).
ui/shell         Shell: status cluster, launcher, app host, display/input backend, settings store.
ui/pocketui      Design tokens, theme engine and shared role styles on top of LVGL 9.
services/        Hardware-owning daemons: radiod (mock and sx1262 backends), netd (Wi-Fi: wifi.*);
                 sysd serves system.* (identity, resources, storage, network summary, service health).
                 meshcored is the first daemon that owns no hardware: it owns the MeshCore protocol
                 runtime and reaches the radio only through radiod (docs/services/MESHCORED.md).
protocols/       Portable protocol cores with no service, no IPC and no radio: meshcore.
core/pocketipc   IPC library used by everything above.
core/pocketlog   Logging, rotation and crash reports.
core/pocketsys   The system facts behind system.*, read from /proc, /sys and /etc.
tools/           doors CLI (pos is its alias), pos-hwcheck, pos-supervise.
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
 ├─ S65meshcored         pos-supervise meshcored (MESHCORED_ENABLE in /etc/default/meshcored, default 0)
 └─ S90doors-shell       pos-supervise doors-shell (ENABLE in /etc/default/doors-shell, default 1)
```

The shell owns the panel; the vendor launcher is not in the image. On a unit
still carrying one from an older image, S90 refuses to start while it is
installed and enabled, or running (platforms/k230/README.md, "Panel
ownership"). The services run under `pos-supervise` (restart with
backoff, one state file per service in /run/pocketos and a crash-loop
marker after five restarts in a minute; nothing displays either yet).

All Doors processes run as root in v0. Per-service users are a follow-up.
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
backend uses RadioLib (upstream, MIT) with a Doors HAL on spidev and
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
the status cluster already polls (`pocketos_shell_radio_state`) rather than asking
radiod a second time. The screen reads nothing from `/proc`, `/sys` or `/run`:
it is a client like any other.

**Diagnostics** (a page behind a button above the power actions;
`apps/system/diag_view.c` decides, `system_app.c` draws): version and
build, uptime, memory, storage, battery, a services summary with restarts
and crash loops, the LoRa radio (`radio.status`), the mesh (`mesh.status`),
Bluetooth, the newest crash reports (`system.crashes`) and the newest log
lines (`system.logs`) with All / Warnings / Errors. It refreshes one bounded
call per tick, so however many services are down a refresh never holds the
LVGL thread for more than one UI deadline, and it holds at most 40 log lines,
replaced whole. The log reading itself is sysd's and bounded there
(docs/api/system.md): the page never opens a file. No terminal and no command
of any kind is reachable from it.

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
fall back to `doors` + `normal` (DS §32; `ice` before it), are logged, and are
left untouched.

The settings store is for non-secret preferences only. It is plain text,
world-readable and unauthenticated, and must never hold passwords, private
keys, Wi-Fi credentials or tokens. The one exception is netd's Wi-Fi store,
under docs/decisions/ADR-003-wifi-credentials.md (accepted 2026-09-13):
a root-only file, protected by Unix file permissions and not encrypted, with
no secret in logs, results, command lines or the environment. Two later
ADRs each add one key under the same protection: peerd's identity
(docs/decisions/ADR-017-peer-datagrams.md) and the Companion Gateway's
private key (docs/decisions/ADR-019-companion-gateway.md, accepted
2026-11-21; not implemented). No other Doors component may persist a
secret.

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
wpa_supplicant (`tests/netd_test.sh`) and validated on unit A on 2026-09-13,
where the vendor `ifup wlan0` path was confirmed not to compete for wlan0.

## Shell

One LVGL process. The status cluster (DS §36) polls radiod once per second. Apps
implement `struct pocketos_app` (create / tick / destroy, and an optional
`shutdown` for the one app whose work outlives its screen - the Terminal's
kept session, docs/apps/TERMINAL.md - called once as the shell exits or
re-executes) and are built into
the shell binary for v0.1; the same API is intended for out-of-process apps
later (ADR-002). The status chrome above an app - the 56 px bar, a 32 px
compact one, or none - is the shell's (DS §30, `ui/shell/chrome.h`): an app
declares a policy in its `struct pocketos_app`, the shell resolves it
against the orientation before the app is created (FULL everywhere in
portrait and on the launcher, COMPACT under an app in landscape by default),
and the content area, the launcher grid and the keyboard reserve all follow
from that one resolved height. An app never reads or sets it. Display backends: SDL (simulator, WSLg) and DRM + evdev
(K230, untested; links the vendor-patched LVGL from the Buildroot package).
`--screenshot` renders any screen headlessly to PNG in the simulator only:
the target LVGL build has no snapshot support (docs/KNOWN_ISSUES.md).
`--text-size small|medium|large` runs the shell at a text size for that run
only (DS §46), and `--audit F.json` writes the layout audit
(`ui/pocketui/pocketui_audit.h`, also `shell.audit`) of the screen the run
ended on.

## Repository layout

Where everything lives, from the repository root. The apps each have a
guide in docs/apps/; the IPC contracts are in docs/api/.

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
apps/                      In-process apps: radio (radiod client), system, fleet (PocketFleet), radar (PocketRadar), timber (PocketTimber), notes (PocketNotes), clock (PocketClock), calendar (PocketCalendar), calculator (PocketCalculator), settings (Wi-Fi, brightness, appearance), rift (mesh client for meshcored), files (file explorer), solitaire, blackjack and 2048 (Pocket Games, docs/apps/PG*.md), poker, rotorterror (RotorTerror), neon-outpost (Relay Nine; the internal id keeps its first name), deskbuddy (DeskBuddy, docs/apps/DESKBUDDY.md) and deskbuddy_vision (its Vision provider), terminal, vision, browser, camera, photo, video, mp3, recorder, wave, zabbix, library (opt-in prototype, not in the image), packetshark (PacketShark) (docs/apps/)
ui/pocketui/               Theme engine (pos_theme), shared role styles (pos_styles), widgets, fonts/
ui/shell/                  Shell: status cluster, launcher, app host; SDL simulator or DRM target (CMake)
ui/cards/                  Card games' shared drawing: the composed card atlas, cloth and chips (Lamplight); core/cards/ holds the cards, RNG, shoe, cloth and motion
core/pocketipc/            IPC library and server helper: length-prefixed JSON over Unix sockets
core/pocketlog/            Structured logging, rotation and crash reports
core/pocketsys.c           System facts (identity, resources, storage, network) behind system.*
core/pocketpcap/           PacketShark's core: capture sources, layer-by-layer decoder, statistics, signals, .pcap files (docs/apps/PACKETSHARK.md)
core/pocketvision/         What Vision's helper does with a frame after the KPU: decoding, tracking, line counting, picture sharing (docs/apps/VISION.md)
services/radiod/           Radio service: policy, stats, IPC; backends mock and sx1262 (RadioLib)
services/sysd/             System service: system.info and system.status over pocketipc (read-only), plus the supervisor state reader
services/netd/             Network service: wifi.* over pocketipc (wpa_supplicant and udhcpc owned by netd, root-only credential store)
services/meshcored/        MeshCore protocol service: mesh.* over pocketipc, on top of radiod's radio.*; owns no radio (`make ENABLE_MESHCORED=1 meshcored`, disabled in the image)
services/gatewayd/         Companion Gateway: authenticated TLS 1.3 to paired phones (ADR-019, docs/api/gateway.md); not in the image
services/peerd/            Authenticated datagrams between Doors devices on an IP network, for Fleet over the LAN (ADR-017); not in the image
protocols/meshcore/        The portable MeshCore protocol core, `libmeshcore.a` (`make meshcore-core`)
hardware/stand/            3D-printable desk stand (k230-desk-stand.stl)
android/doors-companion/   DOORS Companion, the Android app (Kotlin, Gradle; builds on its own, not by the firmware's make)
tests/                     Native unit tests (`make test`), shell tests (tests/*_shell_test.sh, need the CMake shell); tests/hw/ needs boards
docs/
  GETTING_STARTED.md       Install a released image: download, write the card, first boot, Wi-Fi, RIFT
  ARCHITECTURE.md          How the layers, IPC, services and shell fit together
  apps/                    One guide per app (Vision, RIFT, Relay Nine, RotorTerror, DeskBuddy, ...) with its captures
  ROADMAP.md               Current priorities, ongoing and planned work; ROADMAP_HISTORY.md for earlier phases
  releases/                Release notes per version (v0.0.10 on) and HISTORY.md for the earlier releases
  KNOWN_ISSUES.md          Open hardware, licensing, build and software issues
  api/                     Public API contracts: pocketipc v0, radio.* v0, mesh.* v0, shell.* v0, system.* v0, wifi.* v0 (network.md)
  services/MESHCORED.md    The MeshCore service: ownership, persistence, safety and how to enable it on a unit
  design/                  Design System v0.1 (normative), themes.json, feasibility review, shots/
  BUILD_ENVIRONMENT.md     Host, toolchain, SDK commits, build/flash/test commands
  LICENSING.md             Licence register for vendor and third-party code (Doors itself: Apache-2.0)
  licensing/                Apache-2.0 readiness audit and its blockers, B1 artwork questions, asset inventory, public-source exclusions
  decisions/               ADRs (ADR-001 base platform: Accepted; ADR-002 app model: Accepted; ADR-003 Wi-Fi credentials: Accepted for the post-v0.0.9 milestone; ADR-004 audio ownership: Accepted for the audio milestone as a narrow exception for Wave; ADR-005 product name Doors: Accepted for Phases 1, 2 and 3; ADR-013 licence, Apache-2.0: Accepted; all fifteen and their status: docs/decisions/README.md)
  hardware/T-DISPLAY-K230.md  Hardware baseline with evidence classification
  hardware/FIRST_BOOT.md   Day-one runbook: flash, console, hwcheck, PocketOS image, link test
  hardware/BRINGUP_CHECKLIST.md  Bench checklist for the first physical session (image, hash, checksum, tests)
platforms/k230/            Defconfig, Buildroot package and apply/build scripts (ADR-001)
tools/pos/                 `doors` CLI (`pos` is the same binary under its old name): system, hardware, network, radio, wifi, logs, app, shell
tools/supervise/           `pos-supervise`: restart with backoff and crash-loop detection
tools/capture/             `pos-capture`: PacketShark's capture helper, the only program that opens a capture socket (ADR-016)
tools/vision/              `pos-vision`: Vision's helper, the one process that opens the camera and the detector
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
