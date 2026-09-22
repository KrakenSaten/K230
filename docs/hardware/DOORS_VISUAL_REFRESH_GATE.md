# DOORS visual refresh: unit A visual gate

Branch `feat/doors-visual-refresh`, from master `84df75a`. DS Amendment O
(§31), PROPOSED. **Not run yet.** Nothing was deployed or flashed while the
branch was built; unit A still carries whatever it carried before.

**Build under test:** record here, before anything else, the build unit A
shows in `doors system info` (and `doors call shell shell.info` → `build`).
It must be the branch tip named in the gate's result line, not an earlier
build.

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
