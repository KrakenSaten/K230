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

| Check | Result |
| --- | --- |
| `make test`, host | to be filled in from the fresh-clone run |
| Shell/UI tests | to be filled in |
| `tests/initscript_test.sh` | 176 checks: the identity, the settings fallback in all four combinations, both refusals, stale old runtime files, and the in-place re-exec keeping one process with no restart |
| `tests/phase3_migration_test.sh` | 44 checks: the order of deploy.sh's migration and of the rollback, then both run in a fake root - migration, interrupted migration, repeat runs, settings preserved in both directions |
| `tests/image_contents_test.sh` | 27 checks including the rootfs identity gate and its negative controls (two init scripts, a leftover old binary, no shell at all, a settings file shipped in the image) |
| riscv64 and DRM/sysroot builds | to be filled in |
| Full image build, `verify_image.sh`, `verify_splash.sh` | to be filled in |

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
