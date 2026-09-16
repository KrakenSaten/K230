# Doors Phase 3 (shell service identity): hardware gate

Branch `rebrand/doors-3-shell`, from master `15b1b7e`.

**Result: PASS on unit A, 2026-09-16, at `a885842`.** Everything that can be
checked without a unit passed first (below), then all six steps ran on the
hardware. **Phase 3 was accepted by the product owner on this gate, 2026-09-16,
and merged to master.** VERSION stays 0.0.9; Phase 4 has not started and is not
approved.

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

## The gate, as run on unit A (2026-09-16)

The unit started on a master-era build (`15b1b7e`: `doors-shell` absent,
`pocketos-shell` present and running). **Unit A had no network for this
session** - Ethernet unplugged, USB only - so every step ran over the serial
console (COM9, 115200), with `deploy.sh`'s own payload and remote script
transferred as base64 and checked by MD5 on the unit (`8d56af35...` for the
Phase 3 payload, `c10eeb6f...` for the pre-Phase-3 one). The scripts are the
deployment's own; only the transport differs from SSH.

Two physical batches were asked of the operator - the keyboard base with the
power off, and the card swap - and one final answer.

| # | What was checked | Result |
| --- | --- | --- |
| A | `deploy.sh` over the master-era unit | **PASS**. Every service stopped cleanly, then `Starting doors-shell: (settings from /etc/default/pocketos-shell) OK` - the hand-edited PocketOS-era settings were used, not defaults. `deploy: shell processes=0 init scripts=1 supervisor states=1` (the count runs while the supervisor is still starting the shell, so 0 is a timing artefact of the count, not an absent shell; the check a moment later found exactly one). 15 of 16 scripted checks ok: one shell process `/usr/bin/doors-shell`, one init script, old binary and old init script gone, `name=doors-shell`, no `pocketos-shell` state or crash-loop marker, `restarts=0`, the socket answering, `system.status` listing `doors-shell` and no `pocketos-shell`, all four services up, no crash report, `Doors 0.0.9 (build a885842)`, landscape 1232x568 at rotation 270 with `keyboard: present` |
| B | Two reboots | **PASS** both times: one shell process, one init script, `name=doors-shell`, `restarts=0`, `crashloop=0`, no crash report, all four services, landscape with the base attached |
| C | Automatic rotation across power-off keyboard transitions | **PASS**. Base absent: `display: rotation mode automatic (stored), keyboard absent: rotation 0, 568x1232`, touch `rotation 0: swap 0`. Base attached: `keyboard present: rotation 270, 1232x568`, touch `rotation 270: swap 1`. The operator confirmed portrait, landscape and touch visually |
| D | Forced Portrait and forced Landscape | **PASS**, applied in place: pid 386 before and after each of `portrait`, `landscape` and back to `automatic`, `restarts=0`, `crashloop=0`, `running=1`, no crash report, and the touch transform following the display each time |
| D' | Settings precedence on the unit | **PASS**. Old file only: `(settings from /etc/default/pocketos-shell)`. Both files: `(settings from /etc/default/doors-shell; /etc/default/pocketos-shell ignored, not merged)` - and a `POCKETOS_DRM_ROTATION=0` placed **only in the old file** had no effect (`bench_override: false`, rotation 270), which is what "never merged" has to mean. The same key in the doors-shell file did apply (`bench_override: true`, rotation 0). Restored afterwards: the old file byte-identical to its backup, no `/etc/default/doors-shell` |
| E | `rollback_phase3.sh`, then a pre-Phase-3 `deploy.sh` | **PASS**. Dry run listed exactly what it would remove and changed nothing. The real run left `init scripts=0 binaries=0 supervisor states=0 shell processes=0` - deliberately no shell service at all - and kept `/etc/default/pocketos-shell` as it was. The master-era deploy then installed `pocketos-shell`, and 12 of 12 checks passed: one shell process and it is `/usr/bin/pocketos-shell`, no `doors-shell` binary, init script or runtime file anywhere, `system.status` listing `pocketos-shell` only, settings preserved, `Doors 0.0.9 (build 15b1b7e)`. A reboot gave the same 12 of 12 |
| F | A fresh Phase 3 image, flashed and booted | **PASS**. `flash-devcard.ps1` wrote the image (`e8eb5e75...`, 763,363,328 bytes) and read it back: only the 4-byte MBR disk identifier differed. `RESULT: FLASH PASS`. On the fresh card: `/usr/bin/doors-shell`, `/etc/init.d/S90doors-shell`, `doors`, `pos` and `pos-supervise` present; `/usr/bin/pocketos-shell`, `/etc/init.d/S90pocketos-shell` and **both** `/etc/default` settings files absent; neither shell name is a symlink; the shipped init script reads `NAME=doors-shell DAEMON=/usr/bin/doors-shell PIDFILE=/var/run/doors-shell-supervise.pid CHILD_PIDFILE=/run/pocketos/doors-shell.pid CONF=/etc/default/doors-shell`. With no settings file the service said `disabled (/etc/default/doors-shell)`; with `ENABLE=1` but the vendor launcher still owning the panel it refused with `not started: vendor launcher owns the panel`; after the documented hand-over it started once: 11 apps, landscape 270, `keyboard: present`, `restarts=0`, no crash report, **no ERROR and no WARN at all**. A reboot brought it up the same way |

**PASS**, answered by the operator after the last boot.

Two scripted `FAIL` lines in the transcript are the check's own assumptions,
not defects, and both were run down rather than waved through:

- *"no ERROR in any log"* on the migrated unit. The single ERROR is
  `shell.log:477`, `built without LODEPNG/SNAPSHOT, no screenshot`, written by
  a `pos shell screenshot` in an earlier session; this build's lines start at
  571 and the fresh image has no ERROR at all.
- *"the old settings file is preserved"* on the fresh image. Correct: the image
  deliberately ships neither settings file, so there is nothing to preserve.
  That check belongs to the migrated unit only.

One further check was wrong rather than the software: `supervise-doors-shell.log`
does not exist after a clean boot - `pos-supervise` writes that file on an
event, and no service had one. Stopping the service produced
`supervise-doors-shell.log` carrying `supervise doors-shell stopped`, and no
`supervise-pocketos-shell.log`, which is the name the rename was about.

What the gate proved, in one line each: never two shells, never two init
scripts, no stale PocketOS service row, no crash-loop regression, display and
touch rotation unchanged under the new name, settings chosen whole and never
merged, and a deterministic rollback.

**Unit A was left** on the freshly flashed Phase 3 image (`a885842`), the
keyboard base attached, the shell enabled through the canonical
`/etc/default/doors-shell` (`ENABLE=1`) with the vendor launcher disabled
(`/etc/default/k230_phone_ui` `ENABLE=0`), landscape, no
`/etc/default/pocketos-shell`, and the gate's scripts under `/root/p3gate`.
