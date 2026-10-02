# Settings

Status: host-tested (view model, LVGL app test, shell tests, lint) and
validated on unit A on 2026-09-13 (end of this page). Added on `feature/post-v0.0.9-foundations`.
Landscape layout (DS §24, accepted) on `feature/settings-landscape`: unit A
gate PASS 2026-09-17, remote validation and the product owner's physical check
(`docs/hardware/SETTINGS_LANDSCAPE_GATE.md`). Reorganised into categories,
with Sound, Keyboard, Power & Sleep, Time & Region and Developer added, on
`feat/settings-system-cleanup` (DS §52, proposed;
`docs/hardware/SETTINGS_SYSTEM_SMOKE.md`).

Settings is the launcher app for the OS-level controls that exist and work.
Since DS §52 it opens on a short list of categories, each a page of its own:

| Category | What is on its page | Whose it is |
| --- | --- | --- |
| Display | brightness, rotation, text size | the shell (`docs/hardware/DISPLAY_BRIGHTNESS.md`, DS §21, §46) |
| Appearance | theme, display mode | the shell's `shell.theme` path |
| Sound | volume, mute | the shell (`volume.h`) |
| Keyboard | whether the keyboard base is attached, its key light | the shell (`kbd_presence.h`, `kbd_light.h`) |
| Power & Sleep | screen off after, lock after, lock when Doors starts; what sleep is not | the shell (`power_policy.h`, `shell_power.c`) |
| Time & Region | the time zone (one level further: the list of zones), whether the clock is set | the shell (`tz_zones.h`) |
| Network | Wi-Fi, its networks, the join sheet (one level further) | netd (`docs/api/network.md`) |
| System | opens System, Settings' page (DS §47) | sysd and the System app |
| Developer | the debug overlay | the shell (`shell_overlay.h`) |

Each line on the list says what is set ("60 % · Automatic · Small text",
"Screen off after 1 min · lock after 5 min", "Oslo, Stockholm, Berlin,
Paris", "Connected to Home"). Settings has no store of its own and owns no
hardware; it is a client, like System Status: every value is read from its
owner when a page is painted and changed through it, and the owner stores
it, under the keys it always used.

## Moving around

One level: the list, a category's page, and from two of them one page
further (a network's sheet, the list of time zones). The header's title names
the page. The header's back slab and Back go one level back each - a page to
the list, the zone list to Time & Region, a network's sheet to Network - and
from the list out of Settings, to the launcher (app.h `back_slab_in_app`);
Home goes home from anywhere. Settings opens on the list every time.

## What it is not

Not a place for controls that do nothing. Date and time setting, reduced
motion, a 12/24-hour choice (the clocks are 24-hour and there is no setting
for it anywhere yet) and radio options are absent until each is backed by
real functionality and a decision. Reboot and power-off stay in System,
where they are. System sleep is not offered: see Power & Sleep.

## Files

```text
apps/settings/settings_view.[ch]   every decision, no LVGL: headlines, tones, list rows,
                                   what a tap on a network does, what a join sends,
                                   passphrase feedback, brightness, volume and timer
                                   stepping, the categories' lines
apps/settings/settings_internal.h  the app's state, shared by the three files below
apps/settings/settings_app.c       the list of categories, moving between pages, the layout,
                                   the lifecycle
apps/settings/settings_pages.c     Display, Appearance, Sound, Keyboard, Power & Sleep,
                                   Time & Region (and its zone list), Developer
apps/settings/settings_wifi.c      Network: Wi-Fi, the list, the join sheet
tests/settings_view_test.c         143 checks (Makefile)
tests/settings_app_test.c          1432 checks under a real LVGL pointer device and the key
                                   stream, with netd and the shell scripted, hosted as the shell
                                   hosts it: the hierarchy and every way back, every page at
                                   Small, Medium and Large in portrait and landscape (CMake, host)
tests/settings_shell_test.sh       the app test, registration, opening it in the real shell
tests/settings_lint.sh             boundary checks, the layout's among them (make test)
tests/power_overlay_shell_test.sh  Power & Sleep, the time zone and the overlay in the real shell
```

## Power & Sleep

Three different things, and the page says which is which:

- **Screen off after** 30 s, 1, 2, 5 or 10 min, or Never (the default). The
  screen goes black and the first touch or key only wakes it; everything
  keeps running (apps, services, the radio, alarms). On the AMOLED a black
  pixel is a pixel that is off; the backlight level is not touched, because
  what level 0 shows on this panel is UNKNOWN (`DISPLAY_BRIGHTNESS.md`).
- **Lock after** 1, 2, 5, 10 or 30 min, or Never (the default): the existing
  lock screen comes down. **Lock when Doors starts** is the existing
  `lock_screen` setting, which had no control before.
- **Sleep: not available.** The kernel lists `freeze` and `mem` (s2idle and
  deep, unit B 2026-10-02), but which devices can wake the board from them is
  unverified, and there is no RTC to wake it on a timer. Doors does not
  suspend; nothing here pretends to.

Neither timer runs while an alarm rings or while Video, Camera, Vision or
DeskBuddy is in front (a table in `ui/shell/shell.c`). Stored as
`screen_off_s` and `auto_lock_s` in `settings.conf`, whole seconds, one of the
options and nothing else (a value this code never wrote is read as never and
logged).

## Time & Region

The zone in force (its places, its IANA name and standard offset), the local
time when the clock is set, and whether it is: "Set from the network" or "Not
set yet" (this board has no clock that runs while it is off). CHANGE TIME
ZONE opens the list of 32 zones (UTC first, then west to east), the current
one marked; a tap sets it and comes back. The shell applies it at once, as
libc expects (TZ, then `tzset()`), for every clock it shows and every helper
it starts afterwards, and stores it as `timezone=<IANA name>`
(`ui/shell/tz_zones.h`: the image has no zone database, so each zone carries
its POSIX rule; services keep logging in UTC). No 12/24-hour setting: the
clocks are 24-hour and nothing else exists to move.

## Developer

**Debug overlay**, off by default: one compact line at the foot of every
screen - `CPU 18% · RAM 42% · 51°C · NET ↓12 ↑2 KB/s · LORA ↓848 ↑1` - from
sysd's `system.status` and, while radiod answers, `radio.stats`, refreshed
every 2 s. It takes no touch, shows no address or name, and stays as it was
left across restarts (`debug_overlay=0|1`). See `ui/shell/shell_overlay.h`.

## Sound and Keyboard

Sound: `Volume [-] 60 % [+]` in the shell's steps of 10 and a Mute switch,
through the same entry points Controls uses; without a sound card the page
says nothing plays. Keyboard: ATTACHED / NOT ATTACHED for the base, and its
key light from Off to 100 % in steps of 10 (the F3/F4 keys' level), disabled
on a board without one.

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

DS §24 (Amendment H, accepted), on the pattern of §22.3 and §23.4. Cards have
20 px padding; rows are 72 px and buttons 64 px tall; the toggle and the step
buttons are 120 and 96 px wide. The app puts one frame in the body the shell
gives it - exactly the body's content box - and the screen on show inside
that. The screen is shaped from the frame's size, never from the orientation,
when it is built and again whenever the frame changes size (in practice the
keyboard coming up or going down). A change of size moves nothing but flow,
sizes and which box scrolls: the values, the typed passphrase, the focus and
the keyboard are untouched by it.

| Shape | When | A page | Network sheet |
| --- | --- | --- | --- |
| **tall** | the frame is at least as tall as it is wide, or narrower than 1078 px | its panels in one column 22 px apart | the network's text above the field and buttons |
| **wide** | wider than tall and at least 1078 px (two portrait bodies and the 22 px panel gap) | its two columns of 585 px side by side, 22 px apart; a page of one panel or a list keeps it across the body, its rows (themes, zones) two to a line | one panel across the body in two halves 20 px apart: the network described on the left, the field, SHOW and the buttons on the right |

**One box scrolls (DS §52.2).** In either shape the page itself is the only
box that scrolls, and only when what it holds is taller than the room: the
columns never scroll on their own. Until DS §52 the one screen held every
panel, scrolled as a whole in portrait and as two columns scrolling
separately in landscape. Now every page but the two lists (Network's
networks, the time zones) fits without scrolling at Small, Medium and Large in
both orientations on the reference panel (`tests/settings_app_test.c`
section 18), and the lists scroll as their page.

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
from `pos_display_rect_insets()` through the shared layout guard
(`pocketui_layout_begin()`, DS §22.4), so the box the panels scroll in ends 10 px
higher in portrait (1201) and in landscape (537). With the keyboard up, or on
a panel with square corners, the pad is 0.

Rectangles on the reference panel (30 px corners), as `tests/settings_app_test.c`
pins them (rows as numbered under the retired v0.0.10 bar):

| | Portrait | Landscape |
| --- | --- | --- |
| body content box | 20..547 x 152..1211 | 20..1211 x 152..547 |
| a page scrolls in | 20..547 x 152..1201 | 20..1211 x 152..537 |
| Network: Wi-Fi switch | 407..526 x 199..262 | 1071..1190 x 199..262 |
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
and the list glyph Timber's). On the launcher's page in ESSENTIALS, after
Terminal, RIFT and Browser (DS §47).

## System, Settings' page (DS §47)

On the list of categories, System is a row like the others, "System" over
"About, status, diagnostics, restart and power off", with a chevron. A tap
asks the shell to open System in Settings' place
(`pocketos_shell_open_app("system")`, run on the shell's next timer pass,
never inside the tap's own event). System is still its own app -
`apps/system/` - but the shell's `app_pages` table makes it Settings' page.
Since DS §52 System itself is four short pages under a row of tabs: OVERVIEW
(CPU, temperature, memory, load, uptime, clock; storage; Restart and Power
off), NETWORK (the interfaces with their addresses and their traffic -
the rate between two of sysd's answers and the totals - Wi-Fi, the LoRa radio
with its packets and last signal, the mesh), SERVICES (and Diagnostics) and
ABOUT (the build, the card, model, kernel, platform, vendor SDK, CPUs). Each
page asks only for what it shows: ABOUT nothing after the one `system.info`,
NETWORK one of `radio.stats`, `wifi.status` and `mesh.status` a second beside
`system.status` every other.

- it has no launcher cell (`HOME_GROUP_NONE` in `ui/shell/home_layout.c`) and
  the favorites' picker does not offer it; a favorite that already holds it
  still shows it and opens it;
- its way out is Settings: the header's back slab and Back
  (`shell.action back`, the keyboard base's Back when one carries it) come
  back to Settings' list, after they have closed Diagnostics (back to
  SERVICES); the tabs are one level, so from any of them it is Settings;
  Back on Settings' list goes home; Home goes to the launcher's page from
  anywhere;
- every other way in still opens it - Controls' "About DOORS" row and Power,
  `shell.open`, `--open system` - and every one of them comes back to
  Settings.

One level, from a table; there is no navigation stack. Settings is built
again when it comes back (it keeps nothing, so it shows what the shell and
netd hold), at the top of its page. `tests/settings_app_test.c` taps the row
in both orientations and modes; `tests/launcher_groups_shell_test.sh` drives
the whole path in the running shell by taps and by Back, at Small, Medium and
Large.

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
- Appearance: all six themes (five before DS §32) and Normal, Outdoor and Night applied live and
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
the display; nothing reached the rounded corners; no fault, no restart. Then
the product owner's physical check PASS: portrait and landscape look correct
and intentional, the two columns balanced and scrolling on their own under a
finger, taps land, and the passphrase error is readable above the landscape
keyboard.
