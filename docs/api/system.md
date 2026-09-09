# system.* API v0 (sysd)

Status: draft, api_version 0. Transport: pocketipc, socket `sysd.sock`.

Started on the device by `/etc/init.d/S50sysd` under `pos-supervise`, ahead of
`S60radiod`.

`sysd` serves the facts an operating system is expected to know about itself:
identity, uptime, load, memory, temperature, storage, the network interfaces,
the power supply and the health of the supervised services. It reads `/proc`,
`/sys`, `/etc` and the PocketOS runtime directory, opens no device node and
changes nothing (v0 is read-only; reboot, power-off and service restart are
later additions to this API, not to the shell). The facts themselves come
from `core/pocketsys`, which is unit-tested against a fake root so the
absence of every optional source is a tested case.

Rule for every field: a board that lacks the source gets JSON `null` (or an
empty array). Nothing is estimated, and no sentinel stands in for an absence:
a kernel without `MemAvailable` reports `null` for it, not `-1`. Every
documented key is always present, so a client distinguishes `null` from a
value and never has to distinguish a missing key as well. Clients must ignore
unknown fields (docs/api/pocketipc.md, Versioning).

## Methods

### system.info

Identity that does not change while the system runs.

| Field | Type | Source | K230 evidence |
| --- | --- | --- | --- |
| api_version | int | 0. Added by `sysd` (`SYSD_API_VERSION`), not by `core/pocketsys`: the number versions the served API, and the collector does not serve one. `system.status` carries no `api_version`; `system.info` is the method that answers it (docs/api/pocketipc.md, Versioning) | |
| version | string | compiled into sysd (`POCKETOS_VERSION`) | |
| build | string | compiled into sysd (`POCKETOS_BUILD_ID`) | |
| release_file | string or null | first line of `/etc/pocketos-release`, what the card carries; can differ from `version` after a single-binary bench deployment (v0.0.6 M7 bench) | VERIFIED |
| release_build | string or null | the `BUILD_ID=` line of `/etc/pocketos-release`, the build the card was flashed from. Line 1 of that file stays the bare version so that every first-line reader keeps working; the build identity is a `key=value` line below it, written by the Makefile `install` target from the same `BUILD_ID`/`git rev-parse` chain that is compiled into the binaries. A card flashed before v0.0.7 has no such line and reports `null`. Compare with `build` to see whether the running binary came from the flashed image | |
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
| memory | {total_kb, available_kb, free_kb} or null | `/proc/meminfo`. Null as a whole without `MemTotal`; a key the kernel does not carry (`MemAvailable` is absent before Linux 3.14) is null on its own. 512 MB of MemTotal is CMA reserved by the vendor DTS | VERIFIED |
| temperature_c | number or null | `/sys/class/thermal/thermal_zone0/temp` / 1000 | VERIFIED (`canaan_thermal_zone`, 48 to 54 C) |
| clock_set | bool | wall clock is after 2025-01-01. There is no RTC; the clock starts at 1970 every boot until NTP syncs, so a timestamp before then is not a time | VERIFIED (1970 until Ethernet, bench 2026-09-07) |
| storage | [{mount, total_bytes, avail_bytes}] | `statvfs` on each of `/`, `/boot`, `/data` that `/proc/mounts` lists; `avail_bytes` is what a writer can use. At most one row per mount point, whatever `/proc/mounts` does: an initramfs leaves `rootfs /` ahead of `/dev/root /`, and a bind or remount adds another line for the same place | VERIFIED layout (574 MB root, `/boot`; no `/data` yet) |
| network | [{name, operstate, carrier, mac, ipv4}] | `/sys/class/net` without `lo`, sorted; `carrier` is null while the interface is down (the kernel reports EINVAL), `ipv4` null without an address | VERIFIED (eth0 up with DHCP, wlan0/wlan1 down) |
| power | {source, supplies: [{name, type}]} | `/sys/class/power_supply`. `source` is `external` when no supply of type Battery exists and `unknown` when one does; battery state is not interpreted in v0 | VERIFIED empty on unit A (no gauge on the main board) |
| services | [{name, pid, running, crashloop, last_exit_code, restarts}] | pos-supervise's `<name>.pid` and `<name>.crashloop` in the runtime directory; `running` is `kill(pid, 0)` (true also on EPERM); the two crash-loop numbers are parsed from the marker. All six keys are always present: `last_exit_code` and `restarts` are `null` unless a crash-loop marker carries them, so one object shape is rendered whatever the supervisor wrote. See the stability note below | VERIFIED files (marker on PC, pid on unit A) |

### services: intentionally unstable within v0.0.7

`services` answers "what the supervisor has written down", not "what should
be running": a service that was never started is absent from the array
rather than reported as down, and `running` is only "a process with that pid
exists". Its source is `pos-supervise`'s pid files and crash-loop markers,
and the supervisor state file planned for the next block of v0.0.7 replaces
that source. This field may therefore change shape within v0.0.7 without an
`api_version` bump; the rest of `system.info` and `system.status` follows the
normal rule (docs/api/pocketipc.md, Versioning). The only client until then
is the System Status screen, which ships after the state file.

## Errors

| Code | Meaning |
| --- | --- |
| 1 | `POCKETIPC_ERR_UNKNOWN_METHOD`: no such method |
| 2 | `POCKETIPC_ERR_INVALID_PARAMS`: the request carried no `method`, or one that is not a string |

Neither method takes parameters; extra parameters are ignored. A frame that
is not valid JSON, or a request that is not a JSON object, is a protocol
violation rather than an error response: the connection is closed and the
service stays up (`tests/sysd_test.sh`).

## Clients

- `pos system status` prints `system.status`.
- `pos call <service> <method> [key=value ...]` calls any pocketipc method
  (`pos call sysd system.info`); numbers, `true` and `false` are typed, the
  rest are strings. A developer tool: it invokes methods without knowing what
  they are, so a service's own parameter checks are the only guard.
- `pos system info` deliberately does not go through sysd. It reads `/proc`,
  `/sys` and `/etc/pocketos-release` itself so that it still answers when
  sysd is not running, which is exactly when someone is looking. The two
  therefore report the same facts from two readers; `system.info` is the
  contract, `pos system info` is the offline path.
- The shell consumes `system.status` through `shell_ipc_call_timeout` with
  the UI deadline, like `radio.status` (a later release).

## Test hooks

The fake root is a build option, not an environment switch. An object
compiled with `-DPOCKETSYS_TEST_HOOKS=1` prefixes `$POCKETSYS_ROOT` to every
absolute path `core/pocketsys` reads, so `tests/pocketsys_test.c` runs
against a fake `/proc`, `/sys` and `/etc`. Only that test object is compiled
with it (Makefile, `tests/pocketsys_hooks.o`). **`sysd` is not, and cannot be
redirected by its environment**: a service whose whole job is to report what
the machine is must not be able to report something else because someone set
a variable. `tests/sysd_test.sh` starts a `sysd` with `POCKETSYS_ROOT` set
and checks that it ignores it.

What the fake root does not cover, in the test build either: `uname()`,
`sysconf()`, `time()` and the `SIOCGIFADDR` ioctl behind `ipv4` all answer
for the running kernel, and `statvfs` reports the host filesystem behind the
faked path. So `kernel`, `machine`, `hostname`, `cpus`, `clock_set` and
`ipv4` are host facts under the harness, and their failure branches are not
covered by it. `network[].ipv4` is null for an interface the running kernel
does not have, which is what the test asserts on.

`POCKETOS_RUNTIME_DIR` (pocketpaths.h) places the supervisor files. It is a
production override, not a test hook, and stays one.

## Not in v0

Reboot and power-off, service restart, events (a status subscription),
per-process CPU, network configuration of any kind, and a supervisor state
file with restart history (the marker only exists once supervision has
given up).
