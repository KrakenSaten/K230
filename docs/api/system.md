# system.* API v0 (sysd)

Status: draft, api_version 0. Transport: pocketipc, socket `sysd.sock`.

Started on the device by `/etc/init.d/S50sysd` under `pos-supervise`, ahead of
`S60radiod`.

`sysd` serves the facts an operating system is expected to know about itself:
identity, uptime, load, memory, temperature, storage, the network interfaces,
the power supply and the health of the supervised services. It reads `/proc`,
`/sys`, `/etc` and the PocketOS runtime directory and opens no device node.
The facts themselves come from `core/pocketsys`, which is unit-tested against
a fake root so the absence of every optional source is a tested case.

Everything sysd *reports* is read-only. The two things it can *do* are
`system.reboot` and `system.poweroff`, and it does neither itself: it asks
init. Service restart through the API is still a later addition.

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
| services | [{name, pid, running, crashloop, last_exit_code, restarts}] | one entry per `<name>.state` file pos-supervise writes in the runtime directory, sorted by name. All six keys are always present; `null` means the supervisor did not know, never a sentinel. `restarts` is the count **within the current 60-second restart window**, not lifetime restarts — see below. `running` requires both that the supervisor had a live child at its last update **and** that `kill(pid, 0)` still finds it (true also on EPERM); the residual case it does not cover is below. See the source and stability notes | VERIFIED on unit A |

### services: where it comes from

`pos-supervise` writes `$POCKETOS_RUNTIME_DIR/<name>.state`, a `key=value`
file, at every transition of a service it watches: supervisor start, child
start, child exit, restart with backoff, crash loop, and its own shutdown.
Writes go to a temp file in the same directory and are renamed over the real
one, so a reader sees the whole previous state or the whole new one and never
a half-written file. The format is specified in the header of
`tools/supervise/pos-supervise`, which is its only writer;
`services/sysd/sysd_services.c` is its only reader, and a file whose
`state_version` is not 1 is reported as a service whose state is unknown
rather than guessed at.

The file lives in a tmpfs, so everything in it is boot-scoped: restart counts
and exit codes start again from nothing at each boot, and nothing here
pretends to be history. A service that has been stopped keeps its entry, with
`running` false, until the next boot; before v0.0.7 block 2b it vanished from
the array instead, which could not tell "stopped" from "never existed".

This is also why `core/pocketsys` does not produce this field. It reads
`/proc`, `/sys` and `/etc`; the supervisor's files are a platform detail two
layers above it, and sysd joins the two in the response the same way it adds
`api_version`.

`<name>.pid` and `<name>.crashloop` are still written by the supervisor. The
init scripts need the pid file to stop the daemon rather than only the
supervisor, and the bring-up checklist still names the crash-loop marker; both
are compatibility, neither is read by `system.*` any more, and the state file
is authoritative. The pid file exists only while that exact child is alive: it
is removed the moment the child exits, so it never names a pid the kernel may
since have handed to something else.

Two more things the state file records that are worth knowing when reading an
entry. `last_exit_code` is the last exit the supervisor *observed*, which
survives a restart — a service reporting `running: true` alongside
`last_exit_code: 7` is running now and exited 7 the time before, not failing.
And `supervisor_pid`, which the API does not expose, is write-time provenance:
it names whichever supervisor wrote that file, including one that has since
exited, and is not a liveness handle.

### services: what `restarts` counts

`restarts` is how many times the supervisor has restarted the service **in the
current 60-second window**, not since boot and not for the lifetime of the
service. The supervisor resets it whenever a child has run for a full 60
seconds or the window has elapsed, because its purpose is crash-loop
detection: the sixth restart inside one minute is what makes it give up. A
service that has restarted fifty times over an hour, none of them close
together, correctly reports `restarts: 0`. Render it as "restarts in the last
minute", never as a lifetime total; PocketOS does not keep one.

### services: what `running` does and does not prove

`running` is true only when the supervisor recorded a live child **and** the
kernel still has that pid. That closes the two cases that used to produce a
false "up": a supervisor that recorded giving up, and a state file left behind
by a service that has been stopped — neither can report `running: true`,
because `running=0` in the file ends the question before the pid is consulted.

One case remains open. If a supervisor is SIGKILLed while its child is alive
(the init scripts' escalation path), its state file keeps saying `running=1`
with that child's pid; if the child then dies and the kernel hands the same
pid to something else, `running` will be true for a service that is not there.
Closing it needs more than a pid — comparing `started_uptime_s` against the
process's start time in `/proc/<pid>/stat` would do it — and v0 does not.
Treat `running: true` as "the supervisor last saw this child alive and a
process with its pid still exists", which is what it is.

### services: a service that was stopped on purpose

Stopping a service through its init script leaves the entry in place, with:

```json
{"name":"radiod","pid":null,"running":false,"crashloop":false,"last_exit_code":null,"restarts":0}
```

`last_exit_code` is `null` here, not a number, and that is a decision rather
than a limitation. The init script stops the supervisor with SIGTERM; POSIX
says a trapped signal makes `wait` return *before* the child has gone, so the
first status available is the interrupted `wait` (128 + 15) and not the
child's. The supervisor then drains — it waits again until the child has
actually left — and the child's real result **does** become obtainable at that
point. It is deliberately not recorded: an operator asked for the service to
stop, and reporting the exit status of a service that did as it was told
presents a successful stop as a failure. `null` keeps its one meaning
throughout this API: nobody is claiming anything.

The consequence for a client is that "stopped on purpose" and "supervisor
started, no child yet" look the same. Both are genuinely "not running, with no
exit status observed"; distinguishing intent would need a field this API does
not have yet. VERIFIED on unit A (docs/hardware/V0.0.7_BLOCK2B_SMOKE.md).

### services: intentionally unstable within v0.0.7

`services` still answers "what the supervisor has written down since this
boot", not "what should be running": a service the supervisor has never been
asked to watch is absent from the array rather than reported as down, because
nothing on the system holds a list of services that ought to exist. The state
file made the rest of the field explicit rather than inferred, and that is as
far as v0.0.7 takes it.

The boot-scoped part is not an accident of the implementation, it is what
makes the field safe: the runtime directory is on a tmpfs, so no state, pid
or counter can outlive the boot that produced it. VERIFIED on unit A: `/run`
is `tmpfs rw,nosuid,nodev,relatime,mode=755`, and a planted `ghost.pid`
(reported `running: false` while it existed) was gone after a reboot
(docs/hardware/V0.0.7_BLOCK2A_SMOKE.md). A board that puts the runtime
directory on persistent storage breaks this field.

The six keys and their meanings are expected to hold from here. What may
still move within v0.0.7 without an `api_version` bump is what the array
contains — which services appear and when they leave it — as the System
Status screen shows this field for the first time and says what it needs. The
rest of `system.info` and `system.status` follows the normal rule
(docs/api/pocketipc.md, Versioning).

### system.reboot

Restart the machine. Takes no parameters. Replies, then acts. VERIFIED on
unit A (docs/hardware/V0.0.7_BLOCK2C_SMOKE.md).

```
$ pos call sysd system.reboot
{ "action": "reboot" }
```

### system.poweroff

Power the machine off. Takes no parameters. Replies, then acts. Validated on
the host; physical execution on unit A is still pending
(docs/hardware/V0.0.7_BLOCK2C_SMOKE.md).

```
$ pos call sysd system.poweroff
{ "action": "poweroff" }
```

Both are the same contract, and it is worth reading once.

**They go through init.** sysd runs `/sbin/reboot` or `/sbin/poweroff`, which
on this image are BusyBox applets that signal init. init runs `rcK`, which
stops `S90pocketos-shell`, `S60radiod` and `S50sysd` in reverse order, then
syncs and remounts the root filesystem read-only. sysd never calls `reboot(2)`
itself: that would skip all of it — the shell would never release the panel,
nothing would be flushed, and the SD card would be cut off mid-write.

**The reply comes first, and means accepted, not done.** The response is
written to the socket before the action is so much as recorded, and the action
runs from sysd's main loop 200 ms later — the same budget the transport gives
a write to reach its peer (`POCKETIPC_SEND_TIMEOUT_MS`). A client therefore
always sees its answer before the machine goes. What it does not get is a
report of the outcome: after `{"action":"reboot"}` there is no connection left
to answer on. If the command fails, sysd logs an error and keeps serving; the
client will notice by the machine still being there.

**One at a time.** A second power request while one is pending is refused with
code 5 (`POCKETIPC_ERR_BUSY`) and a message naming what is already pending,
whichever order the two arrive in. The first request wins; the second changes
nothing.

**Nothing acts on a request that was not understood or not delivered.**
Unlike `system.info` and `system.status`, which ignore extra parameters, these
two take *no* parameters and reject any with code 2 — a method that stops the
machine does not act on a request it does not fully understand. An unknown
method is code 1 as always. And if the reply could not be written — a client
that asked and immediately disconnected — the action is dropped: nobody was
told the machine was about to go, so it does not go.

**A stop request outranks a pending action.** If sysd is asked to shut down
inside the 200 ms window, the action is dropped rather than carried out on the
way past.

**Confirmation is not this API's job.** "Are you sure?" belongs to whatever UI
a person is touching. The API does what it is told, once.

### Trust model

There is no authorization layer in v0, and the socket permissions are the
whole of it. `sysd.sock` is mode 0660 in a runtime directory that is 0770, and
every PocketOS process on this image runs as root. **Anything that can open
the sysd socket is already root-equivalent on this machine** and could have
run `/sbin/reboot` for itself; `system.reboot` gives it no capability it did
not have. That is why these methods carry no token, no caller check and no
confirmation.

This is a statement about the image as it is, not a claim that it is the end
state. The moment PocketOS runs a service as anything other than root, or
exposes pocketipc beyond the local filesystem, this stops being sufficient and
the methods that change the machine will need a real answer.

## Errors

| Code | Meaning |
| --- | --- |
| 1 | `POCKETIPC_ERR_UNKNOWN_METHOD`: no such method |
| 2 | `POCKETIPC_ERR_INVALID_PARAMS`: the request carried no `method`, or one that is not a string, or parameters on `system.reboot` / `system.poweroff`, which take none |
| 5 | `POCKETIPC_ERR_BUSY`: a power action is already pending |

`system.info` and `system.status` take no parameters and ignore any that are
sent. `system.reboot` and `system.poweroff` take none and refuse any, because
they act on the machine.

A frame that is not valid JSON, or a request that is not a JSON object, is a
protocol violation rather than an error response: the connection is closed and
the service stays up (`tests/sysd_test.sh`).

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

Service restart through the API, events (a status subscription), per-process
CPU, network configuration of any kind, and any authorization beyond the
socket permissions (see Trust model). Reboot, power-off and the supervisor
state file arrived in v0.0.7 and are documented above.
