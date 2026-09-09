# system.* API v0 (sysd)

Status: draft, api_version 0. Transport: pocketipc, socket `sysd.sock`.

`sysd` serves the facts an operating system is expected to know about itself:
identity, uptime, load, memory, temperature, storage, the network interfaces,
the power supply and the health of the supervised services. It reads `/proc`,
`/sys`, `/etc` and the PocketOS runtime directory, opens no device node and
changes nothing (v0 is read-only; reboot, power-off and service restart are
later additions to this API, not to the shell). The facts themselves come
from `core/pocketsys`, which is unit-tested against a fake root so the
absence of every optional source is a tested case.

Rule for every field: a board that lacks the source gets JSON `null` (or an
empty array). Nothing is estimated. Clients must treat every field as
optional and ignore unknown ones (docs/api/pocketipc.md, Versioning).

## Methods

### system.info

Identity that does not change while the system runs.

| Field | Type | Source | K230 evidence |
| --- | --- | --- | --- |
| api_version | int | 0 | |
| version | string | compiled into sysd (`POCKETOS_VERSION`) | |
| build | string | compiled into sysd (`POCKETOS_BUILD_ID`) | |
| release_file | string or null | first line of `/etc/pocketos-release`, what the card carries; can differ from `version` after a single-binary bench deployment (v0.0.6 M7 bench) | VERIFIED |
| model | string or null | `/proc/device-tree/model` | VERIFIED ("Canaan CanMV-K230 with RM69A10 OLED") |
| kernel, machine, hostname | string | `uname` | VERIFIED (6.6.36 riscv64) |
| cpus | int or null | online CPUs | VERIFIED (1 hart visible) |
| vendor_sdk | string or null | the `sdk:` line of `/etc/version/release_version` | VERIFIED |
| os | string or null | `PRETTY_NAME` of `/etc/os-release` | VERIFIED (Buildroot 2025.02.1) |

### system.status

The live view. sysd samples `/proc/stat` once a second for `cpu_percent`.

| Field | Type | Source | K230 evidence |
| --- | --- | --- | --- |
| uptime_s | int or null | `/proc/uptime` | VERIFIED |
| load | [1, 5, 15 min] or null | `/proc/loadavg` | VERIFIED |
| cpu_percent | number or null | busy share of the last one-second interval of `/proc/stat`; null until two samples exist | VERIFIED source, share unmeasured against the bench method |
| memory | {total_kb, available_kb, free_kb} or null | `/proc/meminfo`. 512 MB of MemTotal is CMA reserved by the vendor DTS | VERIFIED |
| temperature_c | number or null | `/sys/class/thermal/thermal_zone0/temp` / 1000 | VERIFIED (`canaan_thermal_zone`, 48 to 54 C) |
| clock_set | bool | wall clock is after 2025-01-01. There is no RTC; the clock starts at 1970 every boot until NTP syncs, so a timestamp before then is not a time | VERIFIED (1970 until Ethernet, bench 2026-09-07) |
| storage | [{mount, total_bytes, avail_bytes}] | `statvfs` on each of `/`, `/boot`, `/data` that `/proc/mounts` lists; `avail_bytes` is what a writer can use | VERIFIED layout (574 MB root, `/boot`; no `/data` yet) |
| network | [{name, operstate, carrier, mac, ipv4}] | `/sys/class/net` without `lo`, sorted; `carrier` is null while the interface is down (the kernel reports EINVAL), `ipv4` null without an address | VERIFIED (eth0 up with DHCP, wlan0/wlan1 down) |
| power | {source, supplies: [{name, type}]} | `/sys/class/power_supply`. `source` is `external` when no supply of type Battery exists and `unknown` when one does; battery state is not interpreted in v0 | VERIFIED empty on unit A (no gauge on the main board) |
| services | [{name, pid, running, crashloop, last_exit_code?, restarts?}] | pos-supervise's `<name>.pid` and `<name>.crashloop` in the runtime directory; `running` is `kill(pid, 0)`; the two crash-loop numbers are parsed from the marker | VERIFIED files (marker on PC, pid on unit A) |

## Errors

Unknown methods yield code 1. Neither method takes parameters; extra
parameters are ignored.

## Clients

- `pos system status` prints `system.status`.
- `pos call <service> <method> [key=value ...]` calls any pocketipc method
  (`pos call sysd system.info`); numbers, `true` and `false` are typed, the
  rest are strings.
- The shell consumes `system.status` through `shell_ipc_call_timeout` with
  the UI deadline, like `radio.status` (a later release).

## Test hooks

`POCKETSYS_ROOT` prefixes every absolute path pocketsys reads, so
`tests/pocketsys_test.c` runs against a fake `/proc`, `/sys` and `/etc`.
`POCKETOS_RUNTIME_DIR` (pocketpaths.h) places the supervisor files.
Production leaves both unset.

## Not in v0

Reboot and power-off, service restart, events (a status subscription),
per-process CPU, network configuration of any kind, and a supervisor state
file with restart history (the marker only exists once supervision has
given up).
