# Settings

Status: host-tested (view model, LVGL app test, shell test, lint) and
validated on unit A on 2026-09-13 (end of this page). Added on `feature/post-v0.0.9-foundations`.
Landscape layout (DS §24, proposed) on `feature/settings-landscape`: remote
validation on unit A PASS 2026-09-17, physical check pending
(`docs/hardware/SETTINGS_LANDSCAPE_GATE.md`).

Settings is the launcher app for the OS-level controls that exist and work:
**Wi-Fi** (through netd, `docs/api/network.md`), **display brightness**
(through the shell, `docs/hardware/DISPLAY_BRIGHTNESS.md`) and **appearance**
(theme and display mode, the shell's `shell.theme` path). It has no store of
its own and owns no hardware; it is a client, like System Status.

## What it is not

Not a place for controls that do nothing. Time zone, date and time, reduced
motion, sound, keyboard and radio options are deliberately absent until each
is backed by real functionality and a decision (see "Settings fundamentals
before v0.1.0" in `docs/ROADMAP.md`). Reboot and power-off stay in System,
where they are.

## Files

```text
apps/settings/settings_view.[ch]  every decision, no LVGL: headlines, tones, list rows,
                                  what a tap on a network does, what a join sends,
                                  passphrase feedback, brightness stepping
apps/settings/settings_app.c      panels and taps
tests/settings_view_test.c        120 checks (Makefile)
tests/settings_app_test.c         636 checks under a real LVGL pointer device and the key stream,
                                  with netd and the shell scripted, hosted as the shell hosts it
                                  in portrait and landscape (CMake, host only)
tests/settings_shell_test.sh      the app test, registration, opening it in the real shell
tests/settings_lint.sh            21 boundary checks, the layout's among them (make test)
```

## Wi-Fi

One panel: an ON/OFF switch, a headline, address and signal when connected,
Scan and Disconnect, and the network list (strongest first, at most 12 rows,
64+ px each, with CONNECTED / SAVED badges). Polls `wifi.status` every
second and `wifi.networks` every third second, every second while a scan runs
or while the list is empty. Every call has the shell's 200 ms UI deadline.

Headlines, by state: "Wi-Fi is off", "Starting Wi-Fi...", "Not connected",
"Connecting to X...", "Getting an address from X...", "Connected to X", and
failures by reason: "Wrong passphrase for X", "X did not answer",
"X refused the connection", "Could not connect to X", "Connected to X, but got
no address" (warning, not error), "Wi-Fi could not be started", "No Wi-Fi
hardware found", "Wi-Fi is in use by another program", and "Wi-Fi service is
not running" when netd does not answer. Every failure is words, never colour
alone.

A tap on a network opens its sheet:

| Network | Sheet | What is sent |
| --- | --- | --- |
| new WPA/WPA2 | passphrase field (masked, SHOW/HIDE, 63 chars max), CANCEL / JOIN; the keyboard's Done also joins | `{ssid_hex, passphrase}` after the 8..63 printable-ASCII check passes locally |
| saved | CANCEL / FORGET / JOIN | `{ssid_hex}` - netd uses the saved passphrase |
| open | a warning that traffic is not encrypted; CANCEL (accented) / JOIN ANYWAY | `{ssid_hex, allow_open: true}` |
| connected | CANCEL / FORGET / DISCONNECT | `wifi.forget {ssid_hex}` or `wifi.disconnect` |
| WPA3-only, WEP, enterprise | an explanation; BACK | nothing |

SSIDs are sent as `ssid_hex`, the exact bytes netd reported, never the
display text. A refusal from netd is shown on the sheet, which stays open.

The passphrase lives in the text field and in the one `wifi.connect` request.
The field is cleared before the sheet closes and when the app is destroyed;
the app never logs (lint). `tests/settings_app_test.c` asserts that no other
request carries it.

Hidden networks are counted ("1 hidden network not shown") but cannot be
joined from Settings in this version; `pos wifi connect <ssid> --hidden wpa2`
can.

## Brightness

`Brightness  [-]  60 %  [+]`, steps of 10 between the 10 % floor and 100 %,
through `pocketos_shell_brightness_get/_set`. A level off the grid moves to
the neighbouring step; one below the floor (set by something else) goes to
the floor. The buttons disable at the ends. On a display without a backlight
device both are disabled and the panel says "This display has no brightness
control." Nothing is persisted by Settings; the shell persists what it
applied.

## Appearance

The five Design System themes as rows (the current one carries SELECTED),
and NORMAL / OUTDOOR / NIGHT as three buttons with the current mode accented.
A tap calls `pocketos_shell_set_appearance()`, the same function `shell.theme`
uses: the change is live, stored in `settings.conf` and announced as the
`shell.theme` event. Only the marks are repainted; everything else follows
through the shared styles, so the scroll position stays where it was.

## Layout

DS §24 (Amendment H, proposed), on the pattern of §22.3 and §23.4. Cards have
20 px padding; rows are 72 px and buttons 64 px tall; the toggle and the step
buttons are 120 and 96 px wide. The app puts one frame in the body the shell
gives it - exactly the body's content box - and the screen on show inside
that. The screen is shaped from the frame's size, never from the orientation,
when it is built and again whenever the frame changes size (in practice the
keyboard coming up or going down). A change of size moves nothing but flow,
sizes and which box scrolls: the values, the typed passphrase, the focus and
the keyboard are untouched by it.

| Shape | When | Main screen | Network sheet |
| --- | --- | --- | --- |
| **tall** | the frame is at least as tall as it is wide, or narrower than 1078 px | Wi-Fi, Display and Appearance in one column 22 px apart; the body scrolls (the v0.0.10 layout) | the network's text above the field and buttons |
| **wide** | wider than tall and at least 1078 px (two portrait bodies and the 22 px panel gap) | two columns of 585 px, 22 px apart: Wi-Fi on the left, Display and Appearance on the right, **each scrolling on its own** | one panel across the body in two halves 20 px apart: the network described on the left, the field, SHOW and the buttons on the right |

**Why columns, and why each scrolls.** A single 1192 px column is the portrait
screen stretched, with network names a screen's width from their badges. Two
columns keep every panel at least as wide as in portrait. They are unequal in
length - the network list alone can be longer than the screen - so each
scrolls itself, and scrolling the list never moves the display controls.

**Why the sheet has two halves.** With the keyboard up in landscape the app
has a 1192 x 100 px body. Under the network's text the passphrase field
opened out of sight (v0.0.10, where landscape was portrait stretched). Beside
the text it is at the top of the panel, in view with the network's name.

**Keeping the field in view.** When an error caption appears under the field
(a passphrase refused locally or by netd), or the body changes size while the
sheet is open, the sheet scrolls just far enough to show the field and its
caption; where they already show, nothing moves. Every message the sheet shows
today, netd's longest refusal included, fits one line across the landscape
field in Normal and Outdoor, so the two are seen together above the keyboard
(it wraps to two lines only in portrait Outdoor, where there is room). SHOW and
the buttons are one short scroll away above the landscape keyboard; the
keyboard's Done joins.

**Corners.** Panels scroll past the foot of the body, which on the reference
panel reaches 10 px into the 30 px rounded-corner squares (DS §21.1). The
frame pads its foot by however far a corner square reaches into the body,
from `pos_display_rect_insets()`, so the box the panels scroll in ends 10 px
higher in portrait (1201) and in landscape (537). With the keyboard up, or on
a panel with square corners, the pad is 0.

Rectangles on the reference panel (30 px corners), as `tests/settings_app_test.c`
pins them:

| | Portrait | Landscape |
| --- | --- | --- |
| body content box | 20..547 x 152..1211 | 20..1211 x 152..547 |
| main screen scrolls in | 20..547 x 152..1201 | Wi-Fi 20..604 x 152..537; Display and Appearance 627..1211 x 152..537 |
| Wi-Fi switch | 407..526 x 199..262 | 464..583 x 199..262 |
| sheet above the keyboard scrolls in | 20..547 x 152..915 | 20..1211 x 152..251 |
| passphrase field (sheet just opened) | 41..526 x 277..340 | 626..1190 x 173..236 |

**Portrait.** With square corners every screen is the v0.0.10 layout to the
pixel (the main screen, every network sheet, the error captions; Normal and
Outdoor), compared in the simulator. With the 30 px corners only the 10 px
strip above the foot changes on an unscrolled screen.

**Turning the display.** The shell restarts itself to rotate and comes back on
the launcher (DS §21.2), so on the device Settings is never open while the
display turns. The layout does not rely on that: `settings_app_test` turns the
display six times under the open main screen (every object once, every value
kept, reshaped each time), and under an open sheet with a passphrase half
typed and with an error shown (the text, the focus, the keyboard and the
caption kept, and what JOIN sends is what was typed across both turns).

Verified in Normal and Outdoor in the simulator against netd-testhooks and the
fake supplicant, and in `tests/settings_app_test.c` in portrait and landscape,
Normal and Outdoor, rounded and square corners.

## Launcher

Id `settings`, name "Settings", icon `LV_SYMBOL_EDIT` (the gear is System's
and the list glyph Timber's). Tenth tile, fifth row.

## Physical validation (unit A, 2026-09-13)

Build `3d4a6e7` on unit A, the product owner at the panel; details in the
unit A sections of `docs/hardware/WIFI_2026-09-12.md` and
`docs/hardware/DISPLAY_BRIGHTNESS.md`. PASS:

- Wi-Fi: scan list, join with the touch keyboard and with the physical
  keyboard, CONNECTED/SAVED badges, SSID, signal and address shown,
  Disconnect, saved rejoin without typing, Forget, "Wrong passphrase for ..."
  on a wrong passphrase, the ON/OFF switch with automatic rejoin, Scan.
- Brightness: `-` disabled at 10 %, `+` in steps of 10 with an immediate
  change, stored and restored after a shell restart and a reboot.
- Appearance: all five themes and Normal, Outdoor and Night applied live and
  persisted; every section readable in each mode, nothing clipped or
  overlapping, no control hard to hit.

## Landscape on unit A (2026-09-17)

Build `8177aa7`, installed over the serial console with only the shell
service restarted; `docs/hardware/SETTINGS_LANDSCAPE_GATE.md` has the evidence.
Remote validation PASS, nobody at the unit: the panel matches the simulator in
portrait and landscape; taps and drags injected into the touch device reached
brightness, the Wi-Fi switch, Scan, a network's sheet, the keyboard, SHOW,
Cancel, the display mode and the rotation modes; each landscape column
scrolled alone; the passphrase field and its error caption were in view above
the landscape keyboard; state was kept across closing, reopening and turning
the display; nothing reached the rounded corners; no fault, no restart. The
product owner's physical check is pending.
