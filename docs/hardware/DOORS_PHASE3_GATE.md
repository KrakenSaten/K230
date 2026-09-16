# Doors Phase 3 (shell service identity): hardware gate

Branch `rebrand/doors-3-shell`, from master `15b1b7e`.

**Result: NOT RUN.** Everything that can be checked without a unit has passed
(below). Not merged. VERSION stays 0.0.9; Phase 4 has not started.

## What this phase changes

The shell service becomes `doors-shell` (ADR-005 Phase 3, "Phase 3 as
implemented" for the full table): `/usr/bin/doors-shell`,
`/etc/init.d/S90doors-shell`, supervised under that name, so
`/run/pocketos/doors-shell.{pid,state,crashloop}`,
`/var/run/doors-shell-supervise.pid` and `supervise-doors-shell.log`. The
socket, `shell.log`, `shell.stdio.log`, the crash reports and the
`/run/pocketos`, `/var/lib/pocketos` and `/etc/pocketos` directories are
unchanged. No internal identifier is renamed.

Settings move to `/etc/default/doors-shell`, with `/etc/default/pocketos-shell`
read as a whole file when the new one is absent - never merged, and the start
says which file it used.

## The three ways a unit could end up with two shells

Each one is closed, and each is tested:

1. **Both init scripts installed.** The image gate refuses a rootfs carrying
   both; `deploy.sh` removes the old one; the new service refuses to start
   while an installed old one is enabled, and names what to remove.
2. **An old shell still running.** `deploy.sh` refuses to unpack while any
   process of either name is alive - pid files and a `/proc` sweep, so a
   daemon whose supervisor was killed is caught too - and the new service
   refuses to start beside it.
3. **A compatibility symlink.** There is none, deliberately: a
   `pocketos-shell -> doors-shell` link would be a second name to start a
   second DRM owner from.

## The display milestone, under the new name

Automatic rotation applies an orientation by re-executing the shell in place:
same pid, so `pos-supervise` sees no exit and counts no restart. The service
rename does not touch that path - the supervisor's name is an argument, and
the shell re-executes `/proc/self/exe` - and `tests/initscript_test.sh` now
proves it under the new name with a daemon that re-executes itself: same pid,
`restarts=0`, `crashloop=0`, `running=1`.

## Validation without hardware (2026-09-16)

From fresh clones of `11db02c`, one clone per build (a host build and a cross
build in one tree leave objects of the wrong architecture, and build output
inside the clone makes `apply_to_sdk.sh` refuse it, as it should).

| Check | Result |
| --- | --- |
| `make all`, host, `-Werror` | rc 0, 0 warnings |
| `make test`, host | 3,414 ok, 0 FAIL, 0 warnings; `initscript_test`, `phase3_migration_test`, `package_sync_test`, `identity_test`, `notices_test`, `required_gates_test` and `build_outputs_test` all 0 failures |
| `tests/initscript_test.sh` | 177 checks: the identity, the settings fallback in all four combinations, both refusals (installed-and-enabled, and running - including a shell with no pid file, caught through /proc), stale old runtime files not blocking a start, and the in-place re-exec keeping one process with `restarts=0`, `crashloop=0`, `running=1` |
| `tests/phase3_migration_test.sh` | 44 checks: the order of deploy.sh's migration and of the rollback, then both run in a fake root |
| `tests/image_contents_test.sh` | 27 checks including the rootfs identity gate and its four negative controls |
| Shell/UI tests | 20 scripts, 0 with rc != 0, 470 ok, 0 FAIL - the display and keyboard suites unchanged by the rename (`auto_rotation_shell_test` 29 ok, `display_geometry_shell_test` 69 ok) |
| riscv64 `make all` (`ENABLE_SX1262=1`) | rc 0, 4 warnings, all in vendor ggwave - 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings |
| Full image build | rc 0 in 223 s; `Shell service: one identity in the target tree (doors-shell), no PocketOS-era leftovers`; 0 first-party warnings |
| `verify_image.sh` | **PASS** - boot-critical files present, and the root partition carries exactly one shell service |
| `verify_splash.sh` | **PASS** |
| The image itself, read with debugfs | present: `/usr/bin/doors-shell`, `/etc/init.d/S90doors-shell`, `/usr/bin/doors`, `/usr/bin/pos`, `/usr/bin/pos-supervise`; absent: `/usr/bin/pocketos-shell`, `/etc/init.d/S90pocketos-shell`, and both `/etc/default` settings files. The shipped init script reads `NAME=doors-shell`, `DAEMON=/usr/bin/doors-shell`, `PIDFILE=/var/run/doors-shell-supervise.pid`, `CHILD_PIDFILE=/run/pocketos/doors-shell.pid`, `CONF=/etc/default/doors-shell` |
| Phase 3 boundaries | VERSION 0.0.9; no `pocketos-shell` symlink anywhere; the shell still reads its five `POCKETOS_*` overrides; PocketUI, pocketipc, pocketlog and pocketaudio untouched; `/var/lib/pocketos`, `/run/pocketos` and `/etc/pocketos` unchanged; 0 `DOORS_*_DIR` references (Phase 4 not started) |

**One defect found and fixed by the new gates.** Buildroot syncs the rootfs
overlay into its own tree and builds the image from that copy, so removing the
old init script from the overlay this repository writes was not enough: the
first image build after the rename put `S90pocketos-shell` back, and
`build_image.sh` refused the build naming that exact file. Both the removal in
`apply_to_sdk.sh` and the gate now cover all three places.

**Deliberate breakages**, each reverted after the test it should fail did:

| Mutation | Caught by |
| --- | --- |
| the service starts beside a running PocketOS-era shell | `initscript_test` (2 FAIL) |
| an installed, enabled old service no longer blocks the start | `initscript_test` (3) |
| the /proc sweep no longer recognises the old shell | `initscript_test` |
| the two settings files are merged instead of chosen between | `initscript_test` |
| deploy removes the old binary before its init script | `phase3_migration_test` (5) |
| the old supervisor state is left behind (a second service row) | `phase3_migration_test` (2) |
| the old service is not stopped before the unpack | `phase3_migration_test` |
| a running shell no longer stops the deploy | `phase3_migration_test` |
| the rollback removes the binary before its init script | `phase3_migration_test` (3) |
| the rollback overwrites a hand-edited old settings file | `phase3_migration_test` (4) |
| the image gate stops looking for the PocketOS-era shell | `image_contents_test` (4) |
| the image gate stops requiring the Doors shell | `image_contents_test` (3) |

12 of 12 caught. One further mutation - the old settings file winning over the
new one - was not completed: breaking that guard leaves the harness's test
holding a supervised daemon, and the run had to be abandoned rather than left
hanging. The case itself is covered by the "with both files" and "ENABLE=0 in
the new file" checks in `initscript_test`.

## The gate

**Operator time: about ten minutes, two card swaps and one answer.** The
session does the rest over SSH (or the serial console when there is no
network), and every scripted check prints `ok` or `FAIL`.

The unit starts on a master-era build (`doors-shell` absent,
`pocketos-shell` present and running).

| # | What happens | Pass |
| --- | --- | --- |
| A | `deploy.sh` over the master-era unit | one shell process, one init script, one supervisor state; `system.status` lists `doors-shell` and no `pocketos-shell` row; the panel comes back with the launcher |
| B | Two reboots | the shell starts both times, no crash report, no crash-loop marker, `restarts=0` |
| C | Automatic rotation, keyboard attached and removed (power-off transitions) | landscape with the base attached, portrait without it, exactly as before the rename; touch follows the display |
| D | Forced Portrait and forced Landscape from Settings | applied in place: same pid, `restarts=0`, no supervisor restart |
| E | `rollback_phase3.sh`, then a pre-Phase-3 `deploy.sh` | the Doors identity is gone, the unit comes back on `pocketos-shell`, settings preserved, exactly one shell |
| F | A fresh Phase 3 image, flashed and booted | `/usr/bin/doors-shell` and `/etc/init.d/S90doors-shell` present, the PocketOS-era pair absent, the shell starts once |

One answer: PASS, or FAIL with the step letter. The operator's part is the
power and card handling in E and F; everything else is read from the unit.

What the gate is proving, in one line each: never two shells, never two init
scripts, no stale PocketOS service row, no crash-loop regression, display and
touch rotation unchanged, and a deterministic rollback.
