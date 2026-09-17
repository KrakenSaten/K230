# System in landscape: unit A gate

Branch `feature/system-landscape`, first from origin/master `aaad9f4` (code
`a6ec564`, docs `d824e36`), then **rebased onto origin/master `f5d81ec`**
(Settings in landscape, DS §24 accepted): code `5ad4b02`, docs `4cbeb0d`, this
sheet `4972860`. The build on unit A is `4972860`. VERSION stays 0.0.10.

**Result: PASS on unit A, 2026-09-17** - host and simulator validation before
and after the rebase, a userspace deployment of the rebased build with health
checks over the serial console, then the product owner's physical check (last
section). **The product owner ACCEPTED the work and DS Amendment I (§25) on
2026-09-17.** Not merged.

Scope: System only. No other app, no rotation policy, no keyboard presence
logic, no shell or PocketUI change, no sysd change, no boot splash, no
first-boot or vendor-launcher behaviour, no Phase 4, no new System feature.

## What changes

System arranges its screen from the size of the body it is given (DS §25.1):
tall in portrait, as before; wide in landscape, with the freshness line (and a
refused action's reason) across the top and the panels in two columns 22 px
apart that each scroll on their own - vitals, storage and network on the left;
services, radio, identity and Restart | Power off on the right - and a
confirmation or the panel a power action leaves at the portrait width (528 px),
centred. On master, landscape was the portrait screen stretched to 1192 px
(a mount's name a screen's width from its free space, a chip adrift mid-row),
and its foot drew into the rounded corners. In portrait the only change is the
corner clearance: the box the panels scroll in ends 10 px higher (1201).
Details: the "the layout" comment in `apps/system/system_app.c`.

## Host validation

First from a fresh clone of `d824e36` (on `aaad9f4`).

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,747 ok, 0 FAIL, 0 warnings (master `aaad9f4`: 3,733; the 14 new `system_lint` checks); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 21 shell and UI test scripts | 521 ok, 0 FAIL, every script rc 0 (master: 20 scripts, 494 ok; `system_shell_test.sh` new with 21, `system_brand_shell_test.sh` 33 -> 39) |
| `system_app_test` (new) | 833 checks, 0 failures, no LVGL warning |
| `system_view_test` / `system_lint.sh` | 0 failures / 14 checks, 0 failures |
| `system_brand_shell_test.sh` | 39 ok: the Doors mark in every theme and mode in portrait, and now in landscape in every mode, on the right column's panel content edge (x 648, 649 in Outdoor) |
| `settings_app_test`, `notes_app_test`, `calc_app_test`, `display_geometry_shell_test.sh` | 81 / 1,138 / 456 checks / 69 ok, 0 failures: the other apps unaffected |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build d824e36`; `system_app.c` and `system_view.c` compiled in; the app test is not a target there |
| `system_app_test` against `aaad9f4`'s `system_app.c` | 294 failures (the test sees the stretched layout and the corners) |
| Mutations of `system_app.c` | 29 of 30 caught; the miss (no scroll reset of the body when it turns wide) changes no behaviour, because LVGL clamps the scroll itself when the body is resized |

The mutations: the wide shape never chosen; always chosen; chosen without the
width floor; the floor off by one pixel; no foot inset; no size handler; no
arrangement after a rebuild; columns never scrollable; the body left
scrollable when wide; the body never scrollable again; the columns stacked
when wide; the columns box not grown; the columns not grown; the columns sized
to content; a column's scroll kept when tall; the body's scroll kept when wide
(missed, equivalent); a dialog across the whole body; a dialog left-aligned;
its track left-aligned; the freshness line full width over a dialog; the live
screen arranged as a dialog; the columns not clickable (a drag between panels
does nothing); a 20 px panel gap; a 20 px column gap; services in the left
column; network in the right; a 20 px body gap; the body outside the frame;
the corner inset applied sideways.

What `system_app_test` covers (sysd played by the test, which answers, refuses
or ignores and records every call; nothing is ever stopped): identity, vitals,
storage, network (a hidden interface), services and radio as reported; the
two-second poll, STALE after six seconds with every value kept, LIVE again; a
service appearing and going. Then, in portrait and landscape, Normal and
Outdoor, rounded and square corners: the live screen with every value as long
as it gets (a card that disagrees, a model and kernel wider than any row, a
crash loop, a long stopped service name, a volume in GB, the clock not set),
with every row the view keeps (6 mounts, 8 interfaces and 2 hidden, 12
services) and with sysd not answering; the restart and power-off
confirmations; a refused power-off with its reason; restarting; powering off.
For each: the arrangement, every scroller inside the body and the safe area,
every target at least 64 px wide and 56 px tall (DS §7 paired buttons), no
overlap, each target scrollable wholly into view, every label and row laid out
whole; fingers scrolling to the last control and each column on its own; a
poll keeping every scroll; each confirmation calling nothing, Cancel calling
nothing, a confirmed action calling sysd exactly once and polling stopping.
Rectangles pinned (portrait to master's own numbers); the width floor at 1077
and 1078 px; a drag that starts in the gap between two panels; the display
turned six times under the open app (one of everything, the same Restart and
Power off objects still opening the right confirmation), with a confirmation
open, with a refusal shown, while scrolled; a rebuild in landscape; close and
reopen both ways up; the keyboard never asked for.

## Simulator validation

The real SDL shell with a dev-only scripted finger (never committed), System
opened against a fake sysd and radiod speaking pocketipc, in both
orientations, with the reference panel's 30 px corners and with square
corners, Normal and Outdoor. Content: unit A's shape (two mounts, eth0 up,
wlan0/wlan1 down, sit0 hidden, four services); every value long; every row;
every field null; no sysd at all. Screens: the live screen, scrolled to the
end (each column separately in landscape), both confirmations, Cancel,
restarting, powering off, and a power-off refused with a long reason. 110
captures of the branch and the same 110 of master `aaad9f4`, every run free of
errors and warnings.

| Comparison | Result |
| --- | --- |
| Portrait, square corners, branch against master | 25 of 25 captures identical below the status bar: live (all content shapes), scrolled to the end, both confirmations, Cancel, refused, restarting, powering off; Normal and Outdoor |
| Portrait, 30 px corners, branch against master | 9 identical (the confirmations, restarting, powering off); 11 unscrolled live screens differ only in the 10 px strip y 1202..1211; the 5 scrolled to the end are master shifted up by exactly 10 px, identical to the pixel |
| Rounded corners, every 30 px capture | branch: the foot corner squares hold only background in 55 of 55; master: something is drawn into them in 37 of 55 |
| Landscape arrangement | two columns 20..604 and 627..1211, the gap empty; the freshness line and a refusal across the top; columns clipped at 537 above the corner squares; each column scrolled on its own to its end; the confirmations and the power-off panel at 352..879, in view whole in Outdoor |
| Column balance | content length left / right: unit A 771 / 828 px, every value long 843 / 956, every row 1,379 / 1,404 |

## The rebase onto `f5d81ec`

Settings in landscape was accepted and merged while System waited, so the
branch was rebased onto the new master before the unit A gate.

- **Conflicts:** only `docs/design/POCKETOS-DS-v0.1.md`, as expected. §21.3's
  list keeps "Settings: §24." and adds "System: §25." after it; §24 is kept
  exactly as accepted and §25 follows it. Against `f5d81ec` the DS diff is 91
  added lines and 0 removed, so nothing of §24 or any earlier section was
  changed. §25's opening no longer calls §24 pending, and §25.3 names §24.4's
  column rule as the one System follows. The DS commit's message was reworded
  to match; its tree is the plain rebase result.
- **Unchanged by the rebase:** every System file (`apps/system/`, the three
  System tests, the brand test, this sheet), `Makefile` and
  `ui/shell/CMakeLists.txt` are byte-identical to `de1295a`. The branch touches
  no other application file; it fast-forwards from `f5d81ec`.

Revalidated from a fresh clone of `4972860`:

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,753 ok, 0 FAIL, 0 warnings (master `f5d81ec`: 3,739, plus the 14 `system_lint` checks); clean checkout after |
| SDL simulator build | rc 0, 0 warnings |
| 21 shell and UI test scripts | 521 ok, 0 FAIL, every script rc 0; `system_app_test` 833 and `settings_app_test` 636, both 0 failures; `system_shell_test.sh` 21, `system_brand_shell_test.sh` 39, `settings_shell_test.sh` 9 |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 4972860`; stripped `doors-shell` 948,912 B, md5 `cf56ca1a7ae7d558c1912cb1b990888c` |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| Simulator smoke, 30 px corners | System portrait, landscape, the restart and power-off confirmations and Cancel, Normal and Outdoor: 16 captures identical below the status bar to the accepted captures of `de1295a`; every object's geometry identical (112 objects live, 50 with a confirmation; the status-bar clock text masked); the foot corner squares background only in 16 of 16 |

## Unit A: deployment

Over the USB serial console (COM9, 115200); SSH was not used. Userspace only:
the SD card was not flashed and only `S90doors-shell` was restarted. Helpers
in `/tmp/sysgate` were read-only apart from the install script; the capture
helper writes no input events, so nothing was pressed remotely, and neither
Restart nor Power off was used in any automation.

| Step | Evidence | Result |
| --- | --- | --- |
| Before | `/etc/doors-release` 0.0.10 / `9f9c802`; `doors-shell` build `8177aa7`, md5 `fa62288b…` (944,816 B), one process (pid 387) under `pos-supervise`, restarts 0; up 25 min; Automatic, portrait, keyboard absent; Slate, Normal, brightness 100; Wi-Fi off, 0 saved; sysd, netd, radiod running, restarts 0; 0 crash reports, no crashloop, no segfault/oops/panic; `shell.log`, `sysd.log`, `radiod.log` 0 ERROR and 0 WARN; `netd.log` 0 ERROR and the 3 WARN recorded in the Settings gate | recorded |
| Transfer | gzip + base64 over the console, 460,574 B in 95 s; gzip md5 `3f79f8ff…` equal on both ends | PASS |
| Install | rollback copy `/root/doors-shell.8177aa7`; installed 0755 root, md5 `cf56ca1a…` equal to the host artifact; the shell answers `build 4972860`; exactly one `doors-shell` (pid 862); supervised, running, crashloop 0, restarts 0 | PASS |
| System opened remotely | `doors app start system`: open, no new ERROR or WARN in any log; the panel captured: the portrait layout with the unit's own values (LIVE, eth0 192.168.10.157, `/` and `/boot`, the running services), the panels clipped above the rounded corners, the foot corner squares background only | PASS |
| Settings opened remotely | open with no fault; the panel captured: Display > Rotation with AUTOMATIC selected and PORTRAIT and LANDSCAPE offered; `shell.rotation` automatic, valid, not applying | PASS |
| Ready | back on the launcher; health as before, `shell.log` 14 new lines since the install, 0 ERROR, 0 WARN; VmRSS 12,800 kB | PASS |

The remote landscape captures, drags and simulator comparison planned earlier
were not run: the product owner was at the unit and did the landscape part by
hand (below). The two captures are portrait only.

## Unit A: the physical check

**Result: PASS** - the product owner at the panel, 2026-09-17, build
`4972860`. The batch, never confirming Restart or Power off:

| Steps | Check | Owner's finding |
| --- | --- | --- |
| 1-9 | Portrait: System looks correct; scroll to the bottom and back; Restart and its confirmation, Cancel; Power off and its confirmation, Cancel | PASS |
| 10-11 | Settings > Display > Rotation > LANDSCAPE; System reopened | PASS |
| 12-13 | Landscape looks designed rather than stretched; LIVE belongs to the whole screen | PASS |
| 14-17 | Each column dragged; one drag started in the gap between two panels scrolls the intended column naturally; both columns usable under a thumb | PASS |
| 18-23 | Restart: the dialog centred, Cancel; Power off: the dialog centred, Cancel | PASS |
| 24-26 | Settings > Display > Rotation > AUTOMATIC; System reopened; portrait correct | PASS (see below) |

**What the unit's logs recorded, read before the result was recorded** (times
UTC, `shell.log`):

- 16:00:52-16:01:01 System open in portrait.
- 16:01:10 LANDSCAPE stored from Settings; the shell restarted in place (the
  same pid 862, supervisor restarts 0) at rotation 270, 1232 x 568, touch
  calibration swapped for it; launcher 6 x 2.
- 16:01:24-16:02:21 System open in landscape.
- 16:02:27 AUTOMATIC stored from Settings; the shell restarted in place at
  rotation 0, 568 x 1232 (keyboard absent); launcher 2 x 6.
- **Steps 25-26.** The owner's first PASS was followed by no System open in
  portrait in the log; the owner was asked and replied "PASS 25-26", after
  which the log showed Notes opened and closed (16:22:38-16:22:40) and still no
  System. Asked again, the owner opened System (16:25:07-16:25:09, in portrait)
  and replied "PASS 25-26". The steps are recorded on that second check.
- `sysd.log` has no line during the check: neither a restart nor a power-off
  reached sysd. Taps inside System are not logged, so the confirmations
  themselves rest on the owner's finding.

Afterwards, over the console (16:25:20 UTC): unit A running build `4972860`
(md5 `cf56ca1a…`), one `doors-shell` (pid 862), restarts 0, no crashloop, 0
crash reports, no segfault, oops or panic; sysd, netd and radiod running,
restarts 0; since the install `shell.log` 51 new lines with 0 ERROR and 0 WARN,
and no new line in `netd.log`, `sysd.log` or `radiod.log`; Automatic
(portrait, keyboard absent), Slate, Normal, brightness 100, Wi-Fi off, on the
launcher; `doors-shell` VmRSS 12,672 kB. Nothing needed restoring.

Captures, logs and the bench helpers:
`out/system-landscape-4972860/hwgate-unitA/` (outside the repository).

**Rollback** (if wanted): stop `S90doors-shell`, copy
`/root/doors-shell.8177aa7` to `/usr/bin/doors-shell`, start it again.

## Found on the way, left alone

On master too, and outside this scope:

- A mount point name longer than about twenty characters runs into its free
  space figure (the name has no length limit of its own). Not reachable with
  sysd as it is: `system.status` only reports `/`, `/boot` and `/data`.
- A poll that changes what the screen holds (a service appearing, the card row
  changing) rebuilds it, so the scroll position goes back to the top - in
  landscape both columns.

## Hardware questions, as answered

- Two independently scrolling columns under a thumb, a drag that starts in the
  gap between panels included: natural and usable (owner, steps 14-17).
- The freshness line at the top right of the landscape body reads as belonging
  to the whole screen (owner, step 13).
- The confirmations in landscape are centred (owner, steps 18-23).
- Not done: a pixel comparison of the panel with the simulator fed the unit's
  own `system.info`/`system.status` (the physical check was done instead);
  the relayout with System open while the display turns remains host evidence
  only, because on the unit a rotation restarts the shell onto the launcher
  (DS §21.2), as the log shows.
