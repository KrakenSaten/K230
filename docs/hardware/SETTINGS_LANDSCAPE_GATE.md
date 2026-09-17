# Settings in landscape: unit A gate

Branch `feature/settings-landscape` from origin/master `aaad9f4` (the shared
corner-clearance helper). Code `c8e847b`, docs `8177aa7`; the build on unit A
is `8177aa7`. VERSION stays 0.0.10.

**Result: PASS on unit A, 2026-09-17** - remote validation, then the product
owner's physical check (last section), with the deviations there recorded as
the owner's deliberate actions. **The product owner ACCEPTED the work and DS
Amendment H (§24) on 2026-09-17.** Not merged.

Scope: Settings only. No other app, no rotation policy, no keyboard presence
logic, no shell keyboard change, no PocketUI change, no boot splash, no
first-boot or vendor-launcher behaviour, no Phase 4.

## What changes

Settings shapes the screen on show from the size of the body it is given
(DS §24.1): tall in portrait, as before; wide in landscape, with the main
screen's panels in two columns 22 px apart that each scroll on their own
(Wi-Fi | Display and Appearance), and the network sheet as one panel with the
network described on the left and the field, SHOW and the buttons on the
right. On master, landscape was the portrait screen stretched to 1192 px, and
opening a network that needs a passphrase put the field out of sight above the
keyboard (the 100 px body showed only the network's name). A field and its
error caption are kept in view (§24.2). In portrait the only change is the
corner clearance: the box the panels scroll in ends 10 px higher (1201).
Details: `docs/apps/SETTINGS.md` (Layout).

## Host validation

From a fresh clone of `8177aa7`.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,739 ok, 0 FAIL, 0 warnings (master `aaad9f4`: 3,733; the six new `settings_lint` checks) |
| SDL simulator build | rc 0, 0 warnings |
| 20 shell and UI test scripts | 494 ok, 0 FAIL, every script rc 0 (as master) |
| `settings_app_test` | 636 checks, 0 failures (was 81) |
| `settings_view_test` / `settings_lint.sh` | 120 checks / 21 checks, 0 failures |
| `notes_app_test`, `calc_app_test`, `display_geometry_shell_test.sh` | 1,138 / 456 checks / 69 ok, 0 failures: the other responsive apps unaffected |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build 8177aa7` |
| `settings_app_test` against `aaad9f4`'s `settings_app.c` | 85 failures (the test sees the old layout) |
| Mutations of `settings_app.c` | 16 of 17 caught; the miss (the wide body left scrollable while its columns fill it) changes no behaviour |
| Portrait against master, simulator | main screen, every network sheet (passphrase, connected, open, unsupported), the error captions, scrolled: every frame identical below the status bar with square corners, in Normal and Outdoor (18 screenshots); with 30 px corners the sheets are identical too, an unscrolled main screen differs only in the 10 px strip at the foot, and a screen scrolled to its end sits 10 px higher |

The mutations: the wide shape never chosen; always chosen; no foot inset; the
sheet not split; no reveal when an error appears; no reveal after a resize;
columns not scrollable; a 20 px column gap; no gap between the sheet's sides;
no resize handler; no gap between panels in a column; columns not grown; the
sheet's sides not grown; the wide body left scrollable (missed, equivalent); no
shape after a rebuild; Wi-Fi in the right column.

## Unit A: remote validation

Everything below ran with nobody at the unit, over the USB serial console
(COM9, 115200). The unit had Ethernet, but no SSH key is authorized on its
current card (the host key changed with the reflash and matches the console's
`ssh-keygen -lf`); installing one was not done, so the console was used, as
for Calculator and Notes. Touch is injected as input events into
`/dev/input/event1`, raw coordinates by inverting the calibration the shell
logged for the rotation in force; keyboard keys at the DS §17.3 geometry.
Panel captures by `ffmpeg -f kmsgrab`, compared with the simulator within 13
per channel (the panel is RGB565) below the status bar.

| Step | Evidence | Result |
| --- | --- | --- |
| Identity before | `/etc/doors-release` 0.0.10 / `9f9c802`; `doors-shell` md5 `b301c499…` (944,816 B), `build ebd5a01`; Automatic, portrait, keyboard absent; theme Slate, Normal; Wi-Fi off, 0 saved networks; restarts 0; 0 crash reports; `shell.log` 0 ERROR, 0 WARN | recorded |
| Transfer | stripped `doors-shell` from the riscv64 DRM build, gzip + base64 over the console: 459,573 B in 95 s; md5 `fa62288b…` equal on both ends | PASS |
| Install | rollback copy `/root/doors-shell.ebd5a01`; only `S90doors-shell` stopped and started; installed 0755 root; the shell answers `build 8177aa7`, supervised, running, crashloop 0, restarts 0 | PASS |
| Portrait on the panel | Settings, Wi-Fi off, against the simulator in the unit's state (Slate, 100 %): every pixel below the status bar within 13 per channel | PASS |
| Portrait touch | brightness `-` then `+`: backlight 254 -> 230 -> 255 of 255, stored `display_brightness=100` | PASS |
| **Landscape** | `shell.rotation mode=landscape`: restart in place (pid 2983 -> 2983, restarts 0), 1232 x 568; Settings, Wi-Fi off, against the simulator: every pixel within 13 per channel; brightness still 100 % | PASS |
| Columns scroll on their own | a finger dragged the right column to Appearance: the Wi-Fi column did not move; panels clipped at 537, above the corner squares | PASS |
| Wi-Fi switch and scan | OFF tapped: netd enabled, disconnected, and the list's empty state ("Tap Scan to look for networks."); SCAN tapped: three real networks listed, WPA2 and WPA2/3, strongest first; the Wi-Fi column dragged to its end, the other column still | PASS |
| **Sheet above the landscape keyboard** | the first network tapped: the keyboard up, the passphrase field at 626..1190 x 173..236 (the rectangle `settings_app_test` pins), focused, beside the network's name | PASS |
| Validation caption | `short` typed on the keyboard, Done: "At least 8 characters" read whole under the field above the keyboard, the field and its five masked characters in view, focus kept; netd logged no connect | PASS |
| Reaching the buttons | the sheet dragged above the keyboard: CANCEL and JOIN at 167..230; SHOW reached the same way and tapped: the typed text unmasked, focus and keyboard kept | PASS |
| Cancel and Back | CANCEL: the two columns back, keyboard down; a sheet with `abc` typed closed from the header's Back: the app closed, nothing sent (netd: 0 connect-related log lines from the first sheet to the end of the gate) | PASS |
| Close and reopen | Settings reopened in landscape: Wi-Fi on with its list, brightness 100 %, Landscape selected | PASS |
| Display mode selector | the right column dragged to its end, OUTDOOR tapped: live, `display_mode=outdoor`, larger type, scroll position kept, panels still clipped at 537; NORMAL tapped: `display_mode=normal` | PASS |
| **Rotation from Settings** | PORTRAIT tapped in landscape: stored, restart in place (pid 2983, restarts 0), portrait, on the launcher | PASS |
| Portrait with Wi-Fi on | the list; the body dragged to its end: the last panel stops at 1200; a WPA2 network's sheet with `short` and Done: the v0.0.10 sheet layout (field, caption, SHOW, CANCEL/JOIN, all above the keyboard); CANCEL | PASS |
| **Back to Automatic** | ON tapped: Wi-Fi off, 0 saved, store ok; AUTOMATIC tapped: stored, no restart needed (keyboard absent, portrait); the panel against the simulator: every pixel within 13 per channel | PASS |
| Rounded corners | the 30 px foot corner squares of every capture hold only background, but for the portrait capture with the keyboard up, where they hold the shell's full-width keyboard sheet (DS §17.3, not Settings) | PASS |
| Health after | 49 new `shell.log` lines, 0 ERROR, 0 WARN, no LVGL message; 0 crash reports; no crashloop marker; no segfault, oops or panic in `dmesg`; `netd.log` 0 ERROR, 0 WARN during the gate; sysd, netd and radiod running, restarts 0; `doors-shell` VmRSS 12,544 kB | PASS |

Covered on the host rather than on the panel, because the unit's
surroundings cannot produce them: the longest SSIDs and a twelve-network list
(the unit saw three short names), netd's refusals, the connected, open and
unsupported sheets, and the display turning under the open app with a
passphrase half typed and with an error shown (`settings_app_test` sections
13 to 16, which also count the objects to prove that no relayout duplicates
or loses a control).

Unit A was left running build `8177aa7`: Automatic (portrait, keyboard base
absent), theme Slate, Normal, brightness 100 % (now stored as
`display_brightness=100`; before the gate the key was absent and the backlight
read 254 of 255), Wi-Fi off with no saved networks, Settings closed, on the
launcher, no error state. Captures, simulator references, logs and the bench
scripts: `out/settings-landscape-8177aa7/hwgate-unitA/` (outside the
repository; the captures show the names of nearby networks).

**Rollback** (if wanted): stop `S90doors-shell`, copy
`/root/doors-shell.ebd5a01` to `/usr/bin/doors-shell`, start it again.

## Found on the way, left alone

On master too and outside this scope:

- Turning Wi-Fi on or off rebuilds the main screen, so its scroll position
  (both columns in landscape) goes back to the top.
- If the shell takes the keyboard away while the network sheet is open (an
  alarm alert), tapping the field focuses it but does not bring the keyboard
  back; Notes does that with its own field handler, Settings has none. Seen in
  the code; not exercised on the panel.
- In the single-line passphrase field the text sits near the top of the 64 px
  field rather than centred (`pocketui_text_field`), in both orientations.

## What only the panel and a finger can show

A capture is the framebuffer, not the glass, and injected events do not show
where a finger lands or how scrolling feels. The physical check, one batch:

1. **Portrait** (as the unit is): open Settings. It looks right; drag the
   screen up and down (scrolling feels natural); tap brightness `-` and `+`
   (the taps land).
2. **Landscape** (Settings > Rotation > LANDSCAPE; Doors comes back on the
   launcher; open Settings): it looks designed for landscape - two columns,
   their balance sensible, nothing clipped, overlapping or cut by the rounded
   corners; drag each column with a finger (each scrolls alone, naturally);
   tap Wi-Fi ON, then SCAN.
3. **Typing and an error, landscape**: tap a network that asks for a
   passphrase. The field is above the keyboard beside the network's name;
   type a few letters with a thumb (fewer than eight), tap Done: "At least 8
   characters" is readable under the field. Drag the sheet up, tap CANCEL.
4. **Back**: tap Wi-Fi OFF; Rotation > AUTOMATIC. Settings still looks right
   in portrait.

**Result: PASS** - the product owner at the panel, 2026-09-17, build
`8177aa7`, "all 21 steps look correct" (the batch as the owner received it:
portrait 1-4, landscape 5-18, return 19-21):

| Check | Owner's finding |
| --- | --- |
| Portrait | looks correct; scrolling natural; brightness taps land |
| Landscape | the two-column layout looks intentional and balanced |
| Columns | each scrolls on its own, naturally, under a finger |
| Wi-Fi ON, SCAN, a passphrase network | correct |
| Fewer than 8 characters, Done | the error message is clearly readable above the keyboard |
| Sheet dragged up, CANCEL | correct |
| Back in portrait | Settings still looks correct |

Before the check, over the console: the unit had not rebooted since the remote
gate (up 22 h), still build `8177aa7` with md5 `fa62288b…` equal to the tested
riscv64 artifact, one `doors-shell` under its supervisor and no vendor
launcher, restarts 0, no crashloop, 0 crash reports, sysd, netd and radiod
healthy, `shell.log` and `netd.log` 0 ERROR and 0 WARN, no segfault, oops or
panic; on the launcher in the gate's start state (Automatic, portrait, Slate,
Normal, brightness 255 of 255, Wi-Fi off, 0 saved networks). Nothing needed
restoring or redeploying. SSH was still unavailable (no authorized key).

**What the unit's logs recorded during the check, and the owner's ruling.**
Read afterwards over the console; the owner confirmed these as deliberate and
the result as PASS:

- **A reboot.** Settings opened in portrait and LANDSCAPE was chosen (stored,
  restart in place); Settings was opened and closed twice; then System was
  opened, and 4 s later sysd accepted a reboot and ran `/sbin/reboot` (clean:
  "stopping on signal" in `shell.log`, no ext4 recovery in `dmesg`). Doors came
  back in landscape, the stored mode, and the rest of the batch ran after it.
- **A real join attempt.** With Wi-Fi turned on, netd logged a join to one of
  the nearby networks (WPA2/WPA3) 25 s later and `auth_failed` 8 s after that;
  a join reaches netd only once the passphrase passes the 8..63 character check,
  so a passphrase of 8 or more characters was submitted after the short one.
  Nothing was stored (0 saved networks). These are the three `netd.log` WARN
  lines of the session: `STATUS: no answer from wpa_supplicant: Connection
  timed out` (the 300 ms control timeout already recorded after the post-v0.0.9
  Wi-Fi validation) and the two `auth_failed` lines. `shell.log` stayed at 0
  ERROR and 0 WARN.
- **The ending.** Wi-Fi was turned off, then PORTRAIT (not AUTOMATIC) was
  stored and Doors restarted in place into portrait; in portrait Settings was
  opened, Wi-Fi turned on again, and Settings closed. The unit was found in
  Portrait mode with Wi-Fi on.

Afterwards, over the console: `doors wifi off` and `shell.rotation
mode=automatic` (no restart needed: keyboard absent, already portrait). Unit A
left running build `8177aa7` (md5 `fa62288b…`), Automatic (portrait), theme
Slate, Normal, brightness 255 of 255, Wi-Fi off, 0 saved networks, store ok,
on the launcher; one `doors-shell`, restarts 0, no crashloop, 0 crash reports,
no segfault, oops or panic; sysd, netd and radiod running, restarts 0;
`doors-shell` VmRSS 12,416 kB.
