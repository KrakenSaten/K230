# System in landscape: unit A gate

Branch `feature/system-landscape` from origin/master `aaad9f4` (the shared
corner-clearance helper). Code `a6ec564`, docs `d824e36`. VERSION stays 0.0.10.

**Result: host and simulator validation PASS, 2026-09-17. Unit A NOT RUN.**
This work was done host and simulator only, by the product owner's
instruction: unit A was not accessed in any way (no serial console, no SSH, no
transfer, no restart of `doors-shell`, no rotation change, no flashing), and
the state another session left it in after the Settings gate is untouched.
The remote gate and the owner's physical check below are prepared, not run.
DS Amendment I (§25) is PROPOSED.

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

From a fresh clone of `d824e36`.

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

## Found on the way, left alone

On master too, and outside this scope:

- A mount point name longer than about twenty characters runs into its free
  space figure (the name has no length limit of its own). Not reachable with
  sysd as it is: `system.status` only reports `/`, `/boot` and `/data`.
- A poll that changes what the screen holds (a service appearing, the card row
  changing) rebuilds it, so the scroll position goes back to the top - in
  landscape both columns.

## Unresolved hardware questions

Only the panel and a finger can answer these; nothing here is claimed for
unit A.

- Whether two independently scrolling columns 343 px tall feel natural under a
  thumb, drags starting in the gap between panels included, and whether the
  scrollbars read clearly.
- Whether the freshness line at the top right of the landscape body is noticed
  as belonging to both columns.
- Capture against the simulator on the real panel (RGB565, within 13 per
  channel, as for Calculator, Notes and Settings). System's values are live, so
  the simulator must be fed the unit's own `system.status` and `system.info`
  taken at the moment of the capture.
- On the unit a rotation restarts the shell in place and comes back on the
  launcher (DS §21.2), so System is never open while the display turns there;
  the relayout under an open app is host evidence only.

## The gate to run later

Only in a session the owner allows to use unit A. Build the stripped
riscv64 DRM `doors-shell` from the branch tip as for the Settings gate (a
build of `d824e36` was 944,816 B, md5 `3afdda5b…`; the gate rebuilds it).
**Power off is never confirmed on the unit** - it cannot be undone remotely
(docs/hardware/V0.0.7_BLOCK2C_SMOKE.md). Every confirmation is closed with
Cancel.

**Remote** (serial console or SSH, as the owner allows):

1. Identity before (release file, `doors-shell` md5 and build, rotation mode,
   theme, restarts, crash reports, `shell.log` errors). Rollback copy of the
   installed `doors-shell`; install the build; restart only `S90doors-shell`;
   the shell answers the new build, supervised, restarts 0.
2. Portrait: open System; capture; compare with the simulator fed the unit's
   own `system.info`/`system.status` taken at the same moment.
3. Landscape (`doors call shell shell.rotation mode=landscape`, restart in
   place, back on the launcher): open System; capture and compare; drag the
   right column to Restart | Power off (the left column does not move), and
   the left column to its end; tap Restart: the confirmation centred, capture;
   Cancel; tap Power off: the confirmation centred, Cancel accented; Cancel.
4. Back to the mode found (Automatic); health after (`shell.log` 0 ERROR and
   WARN, no crash report, services running, restarts 0); the foot corner
   squares of every capture hold only background.

**Physical** (the owner, one batch):

1. **Portrait** (as the unit is): open System. It looks as it did; drag to the
   end and back; tap Restart, then Cancel; tap Power off, then Cancel.
2. **Landscape** (Settings > Rotation > LANDSCAPE; open System): it looks
   designed for landscape - two balanced columns, LIVE at the top right,
   nothing clipped, overlapping or cut by the rounded corners; drag each column
   with a thumb, once starting between two panels (each scrolls alone,
   naturally); drag the right column to the actions; tap Restart (the
   confirmation in the middle), Cancel; tap Power off, Cancel.
3. **Back**: Settings > Rotation > AUTOMATIC. System still looks right in
   portrait.

**Result: NOT RUN** - host and simulator only by instruction; the owner's
physical check and the remote gate are pending.
