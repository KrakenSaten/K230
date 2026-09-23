# DOORS visual refresh: unit A visual gate

Branch `feat/doors-visual-refresh`, from master `84df75a`, merged to master
`36d216f`. DS Amendment O (§31) ACCEPTED by the owner 2026-09-23, on this
gate; follow-ups in DS §31.8.

**Build under test: `3c3d2b5`** (branch tip `6abde21` plus the memory fix
found by this gate). Unit A carries it in every deployed file
(`/etc/doors-release` `BUILD_ID=3c3d2b5`, `shell.info` `build`), deployed with
`deploy.sh`, no flash. **Left locked, landscape, rotation Automatic, keyboard
base attached.** Rollback of the `b41be37` userspace:
`/root/rollback-pre-dvr/RESTORE.sh`.

**Result: PASS on unit A, 2026-09-23** (remote checks and captures, and the
owner's physical checks: "all fine"). Not merged.

## Result

| Check | Result |
| --- | --- |
| Deploy | image `3c3d2b5` IMAGE GATE PASS; `deploy.sh` rc 0; 19 art files, 8,756,964 B, byte-identical to `ui/assets/doors`; all services running, 0 restarts, 0 crash reports |
| shell.info | `art.background` true, `icons_art` 12, `icons_fallback` 0; cold start `lock.locked` true |
| Lock (portrait, landscape) | frame captures match the simulator; bar clear of the 50 px corners; framebuffer shows dither, no contour bands at 4x contrast; owner: smooth, readable |
| Gestures (touch controller) | tap and a 40 px drag stay locked; a 220 px swipe opens, door sequence 925 ms end to end on the device (designed 840 ms); owner: smooth |
| Keyboard while locked | owner: typing reached nothing, Enter opened |
| Launcher | portrait 124 px cells, landscape 87 px, no scroll; portrait footer 20 px higher than in the simulator because the 50 px bottom-left corner override feeds the layout's inset, as designed; owner: icons sharp, labels readable, press mark shows |
| Controls | real radio (`Receiving`), Wi-Fi (`Off`), brightness 50 % from the backlight; owner: brightness moves the panel, Night darkens photograph and icons |
| Apps | Notes, Calculator, RIFT, Clock opened by touch and closed by the back slab; apps unchanged |
| Rotation | portrait and back to Automatic landscape in place (same pid), both times open, not locked |
| Timing on the device | launcher + Controls built in 20-25 ms, lock 1-3 ms, each background read 1-14 ms |
| Memory | see the fix below: `3c3d2b5` locked 7,552 kB RssAnon, open 6,196 kB, after 60 further rounds 6,204 kB, flat; `art.bytes_held` 1,731,328 open every time |

**Defect found and fixed (`3c3d2b5`).** On `6abde21`, after twenty rapid
lock/open rounds the shell's RssAnon stayed 1.3 MB up (7,508 kB) and flat for
forty more rounds, while `art.bytes_held` was correct: glibc kept one freed
1.4 MB background in the heap. The art pixels are now an anonymous `mmap`,
unmapped on release; re-measured on the unit as above.

Not part of this branch, seen again: the COMPACT bar clips the `RX` chip in
apps in landscape (shell chrome stage 1 follow-up).

The SDK tree `~/work/t-display-k230` now carries the `3c3d2b5` apply:
re-apply master before building anything else from it. Evidence:
`out/doors-visual-gate/` (scripts, logs, `caps/`), not committed.

## What is new on the panel

| | |
| --- | --- |
| Lock screen | cold start, Lock buttons, `shell.lock`; lock photograph, 96 px time, date, "Swipe up to open"; swipe up 140 px (100 landscape) or Enter opens; door sequence ~0.8 s |
| Launcher | home photograph behind a transparent status bar with no clock; time + date header; four glass panels (CONNECTIONS, WORKSPACE, PLAY, DEVICE) of 96 px portal icons; Lock and Controls at the foot |
| Controls | Radio, Wi-Fi, Rotation, Display tiles; Brightness slider; Settings / Mesh messages / About DOORS; Lock, Power |
| Apps | unchanged, including the status bar (§7, §30) |

What the simulator draws, at half size (clock and date are the capture's):
`docs/design/doors-visual-refresh/portrait.png` (lock, open door, launcher,
Controls, an app, launcher in Night) and `landscape-1.png`, `landscape-2.png`
(lock, open door, launcher, Controls). The panel is to be compared with these.

## Deploy (no flash)

The art is new under `/usr/share/doors/ui`, and `deploy.sh` now carries
that directory whole. From a clean clone at the tip: `apply_to_sdk.sh`, the
package rebuild, then `platforms/k230/scripts/deploy.sh <unit>` as for the
previous shell gates. Check before looking at the panel:

```sh
doors call shell shell.info
```

`art.dir` is `/usr/share/doors/ui`, `art.background` is `true`,
`launcher.icons_art` is 12 and `launcher.icons_fallback` 0, and
`lock.locked` is `true` right after the shell started.

## Checks on the panel

Portrait, then landscape (Controls → Rotation, or the keyboard base):

1. **Lock.** After a shell restart (`/etc/init.d/S90doors-shell restart`) the
   lock shows. Time and date correct; nothing clipped by the rounded corners;
   the status bar's wordmark and radio chip readable over the photograph.
   Dark areas: note any RGB565 banding on the doors (the art is dithered).
2. **Tap** on the lock: only the hint lifts; nothing opens. **Short drag** up:
   the time follows and springs back. **Swipe up**: the door sequence plays
   (closed → open door with "Open. Explore. Connect." → launcher), smooth
   enough not to stutter visibly; note the time it takes.
3. **Launcher.** All twelve apps, in their groups, readable labels, icons
   crisp (no scaling blur), the glass panels visible but not heavy. Hold an
   icon: the bracket and underline appear in the app's colour; release opens
   the app. Open and leave three apps; the launcher comes back identical.
4. **Controls.** Opens from the foot; Radio shows radiod's real state, Wi-Fi
   netd's; Brightness moves the panel; Display cycles Normal → Night →
   Outdoor and the photograph darkens in Night; Power opens System (nothing
   powers off). Back returns to the launcher.
5. **Lock from Controls**, then open with the keyboard's Enter (landscape,
   base attached): opens; typing while locked reached nothing.
6. **Alarm while locked** (set one a minute ahead in Clock, lock): the alert
   shows over the lock and Stop works without opening it.
7. **Landscape corners.** Unit A needs 50 px top corners in landscape
   (`POCKETOS_SAFE_CORNERS` in `/etc/default/doors-shell`); with that set, the
   launcher's footer buttons and the Controls tiles are clear of all four
   corners.
8. **Rotation** from Controls: the shell restarts in place and comes back
   *open* on the launcher (not the lock).

## Measure

- `grep -E 'art: |launcher:' /var/lib/pocketos/log/shell.log`: load time of
  each background and icon (the log line says ms), and the launcher line.
- Door sequence: frames it takes, by eye or `top -d 0.2` on the shell.
- `grep -E 'VmRSS|RssAnon' /proc/$(pidof doors-shell)/status` on the
  launcher, then with the lock up: expect about +1.4 MB while locked,
  back again after opening (`art.bytes_held` says the same from inside).

## Rollback

Deploy the previous build the same way; the art directory it leaves behind
is unused by older shells.
