# The Clock rotation-state fix: unit A gate

**The unit carries build `646dcbb`** (`Doors 0.0.10 (build 646dcbb)`), the tip
of `fix/clock-rotation-state` rebased onto origin/master `b9c4a47`. VERSION
stays 0.0.10. Deployed as userspace only; nothing was flashed.

**Result: PASS on unit A, 2026-09-18** — host and simulator revalidation after
the rebase, a userspace deployment with checksum and health verification, a
non-visual pre-flight of the mechanism over SSH, then the product owner's
physical check on the panel. **Not merged.**

Scope: Clock only. No other app, no rotation policy, no keyboard-presence
logic, no PocketUI or alarm-alert change, no layout change, no new Clock
feature, no VERSION change.

## What changes

A rotation restarts the shell in place (`restart_in_place()` in
`ui/shell/shell.c` is an `execv`, DS §21.2), which threw away the one clock
engine — a static in `apps/clock/clock_runtime.c` — and rebuilt it from
`clock.conf`, which holds the alarms and the last timer duration and nothing
else. A running stopwatch, a running countdown and every snooze ended there.

The outgoing shell now writes a restart handoff to
`/run/pocketos/clock/restart.state` and the incoming one takes and consumes
it. `/run` is a tmpfs that starts empty on every boot, so a monotonic instant
in it cannot outlive the boot it was measured in; `clock.conf` is unchanged in
format and in what it holds. Details: "Storage" in `docs/apps/POCKETCLOCK.md`.

## Host validation, after the rebase

From the working tree at `646dcbb` (origin/master `b9c4a47`).

| Check | Result |
| --- | --- |
| Rebase | `f93712b` → `646dcbb`, no conflicts; the commit's patch byte-identical apart from one blob-index line in `KNOWN_ISSUES.md`, which master had also edited |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,813 ok, 0 FAIL, 0 warnings |
| `clock_engine_test` / `clock_time_test` / `clock_store_test` | 210 / 39 / 76 checks, 0 failures |
| `clock_runtime_test` | 103 checks, 0 failures |
| `clock_handoff_test` | 212 checks, 0 failures |
| `clock_restart_test` | 98 checks across 8 real `execv` boundaries, 0 failures |
| `clock_lint.sh` | 47 checks, 0 failures |
| `clock_shell_test.sh` | 27 checks, 0 failures |
| `fleet_lint.sh` / `radar_lint.sh` and the Fleet and Radar tests | 0 failures: unaffected by the rebase |
| 21 shell and UI test scripts | 560 ok, 0 FAIL, every script rc 0 |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 0 first-party **and** 0 vendor warnings |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; 9 handoff symbols linked in |
| Mutations of the handoff logic | 18 of 18 caught |

Fleet §28 (Amendment L) and Radar §29 (Amendment M) are present and ACCEPTED
after the rebase, as are §20–§27.

## The build, and that it is a commit

Packaged from a fresh clone of `646dcbb` (`/home/dolby/work/clock-gate`) so
the worktree is clean and the build id carries no `-dirty` suffix. The
developer's own checkout has unrelated untracked files (CAD, card scripts),
which is why the clone was used rather than an override.

```
Packaged source : 646dcbb via git archive
Source worktree : clean
RadioLib        : 034126ef3b5305394d1e4e14a5049482ec10c1c4 (clean)
ggwave          : a38e38b7373f9adf45baf737a104206664b225a1 (clean)
BUILD_ID        : 646dcbb
```

## Deployment

`platforms/k230/scripts/deploy.sh 192.168.10.157`, userspace only, rc 0.

Unit A's SSH host key had changed when it was reflashed for v0.0.10, and
`StrictHostKeyChecking=no` does not bypass a *changed* key, only an unknown
host. The deployment therefore ran with a `ssh` shim first on `PATH` pointing
at a scratch `known_hosts` seeded by `ssh-keyscan`, so the developer's
`~/.ssh/known_hosts` was neither read nor written. The bench public key was
reinstated on the unit by the product owner on the serial console beforehand;
the image ships no `authorized_keys`.

| Before | After |
| --- | --- |
| `0.0.10`, `BUILD_ID=9f9c802` (the v0.0.10 release) | `0.0.10`, `BUILD_ID=646dcbb` |
| `doors-shell` `212c4e1b…` | `doors-shell` `efdbde9805b226aa93018c99d58f86b36dcb50498a6222ad68c962d172e6e7b3` |
| `handoff` strings in the shell: 0 | 6, and both log strings |
| `/run/pocketos/clock` absent | created on the first restart |

Every deployed checksum matches the build tree: `doors` `12324523…`, `radiod`
`84ed3a13…`, `sysd` `c06dc325…`, `netd` `b441c6cf…`. A rollback copy of the
replaced userspace is on the unit at
`/root/rollback-9f9c802/userspace.tar.gz` (`d736a028…`, 1,393,664 bytes).

Baseline before the deployment: 4 services running, 0 restarts, no crashloop
markers, no crash reports, 0 ERROR and 0 WARN in `shell.log`, `clock.conf`
holding `pocketclock 1` / `timer 71` and no alarms.

**`/run` is a tmpfs on the unit**, which is what the whole design rests on and
is the reason the handoff cannot outlive its boot:

```
tmpfs /run tmpfs rw,nosuid,nodev,relatime,mode=755 0 0
```

## Pre-flight over SSH, before the physical gate

Run so that the owner would not spend a manual gate on a build whose mechanism
was broken. No touch input: one **disabled** alarm was put in `clock.conf` (a
disabled alarm cannot ring, so nothing appeared on the panel) to give the run
something only it knows, the display was turned through the real
`shell.rotation` IPC, and the log was read on both sides of the `execv`.

```
17:08:20.644 shell INFO  clock: runtime state handed to the next shell
17:08:20.645 shell INFO  display: restarting in place (/usr/bin/doors-shell) to open the display at rotation 0
17:08:20.712 shell INFO  start version=0.0.10 build=646dcbb pid=12241
17:08:20.798 shell INFO  clock: 1 alarm(s) loaded
17:08:20.798 shell INFO  clock: runtime state taken from the shell before this one
```

The pid is the same on both sides, which is what makes it an `execv` and not a
restart; 154 ms from handed to taken; the handoff file was gone afterwards,
consumed as designed. `clock.conf` was then restored and the restoration
checked by sha256 against the backup taken first.

**One WARN in the log is from this pre-flight and not from the gate.**
Restoring `clock.conf` out of band removed the seeded alarm while the running
shell still held it, so the next start refused a handoff describing an alarm
list that no longer matched:

```
17:09:35.078 shell WARN  clock: the handoff from the shell before this one could not be used; the stored alarms stand on their own
```

That is the alarm-list guard working as designed — a snooze must never land on
a different alarm — and it is reachable only by editing `clock.conf` behind
the shell's back, which nothing in Doors does. It did not recur on the next
restart. The gate's own evidence begins after `shell.log` line 917.

## The physical gate

Product owner, unit A, 2026-09-18, on build `646dcbb`, panel starting at
Automatic/portrait.

| Step | Result |
| --- | --- |
| Stopwatch started, left running, display turned | **PASS** — "survives rotation and continues with the correct elapsed time" |
| Countdown of about 30 s started, display turned back | **PASS** — "survives rotation and continues with the expected remaining time" |
| Snooze across a rotation | **Not run** (optional; covered by `clock_handoff_test` and by `clock_restart_test` across a real `execv`) |
| Rotation returned to Automatic, Clock reopened | **PASS** — "Clock renders normally" |

What the log records of it, with the same pid `12828` across the seam:

```
17:14:58.730 rotation mode landscape stored: rotation 270
17:14:59.531 display: restarting in place ... rotation 270
17:14:59.591 start version=0.0.10 build=646dcbb pid=12828
17:15:09.205 open app clock
17:15:19.340 open app clock
17:15:46.421 rotation mode portrait stored: rotation 0
17:15:47.221 clock: runtime state handed to the next shell
17:15:47.221 display: restarting in place ... rotation 0
17:15:47.279 start version=0.0.10 build=646dcbb pid=12828
17:15:47.366 clock: runtime state taken from the shell before this one
17:15:50.860 open app clock
```

**Which rotation carried the state, precisely.** The turn at 17:15:47 wrote a
handoff and the next image took it, and the owner then opened Clock and found
both the stopwatch and the countdown continuing correctly. The earlier turn at
17:14:59 logged no handoff at all, which means nothing was running when it
happened — an idle clock writes nothing, by design. So the panel evidence is
one rotation carrying both timers rather than two rotations carrying one each;
that is a complete demonstration of the fix, and it is recorded here as what
the log shows rather than as what the steps assumed.

## After the gate

| Check | Result |
| --- | --- |
| `doors-shell` processes | 1 |
| `doors-shell`, `sysd`, `netd`, `radiod` | `running=1 crashloop=0 restarts=0` each |
| Crashloop markers | none |
| Crash reports | none |
| Kernel oops/panic/segfault in `dmesg` | 0 |
| ERROR or WARN during the gate | **0** (the one WARN in the log is the pre-flight's, above) |
| `doors-shell` checksum | `efdbde98…`, unchanged |
| Rotation | `display_rotation=automatic` |
| Alarms left behind | none |
| Handoff waiting | none (the last exit had nothing to hand on) |
| `clock.conf` | `pocketclock 1` / `timer 30` — the format is untouched and the duration is the owner's own 30 s countdown, persisted the way a duration always was |

Unit A is left on build `646dcbb`, Automatic orientation, theme Carbon,
display mode Normal, brightness 100, all four services healthy, with the
rollback copy of the v0.0.10 release userspace still in `/root`.

## Left alone

- **A shell that is killed or crashes hands nothing on.** A running stopwatch,
  countdown or snooze still ends there; only an orderly exit writes a handoff.
  By design, and now the only remaining case.
- **A power cycle clears the handoff**, which is the point: every instant in it
  is measured on a clock that starts again at the boot.
- The snooze leg of the physical gate was not run. It is covered on the host,
  including across a real `execv`, but not on the panel.
- `docs/hardware/CLOCK_LANDSCAPE_GATE.md` records the original observation that
  a rotation ended a running stopwatch, countdown and snooze. It stands as the
  history; this sheet is the fix.
