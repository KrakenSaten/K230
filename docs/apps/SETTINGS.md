# Settings

Status: host-tested (view model, LVGL app test, shell test, lint); not yet
run on hardware. Added on `feature/post-v0.0.9-foundations`.

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
tests/settings_view_test.c        111 checks (Makefile)
tests/settings_app_test.c         71 checks under a real LVGL pointer device and the key stream,
                                  with netd and the shell scripted (CMake, host only)
tests/settings_shell_test.sh      the app test, registration, opening it in the real shell
tests/settings_lint.sh            15 boundary checks (make test)
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

Body width 528 px, cards with 20 px padding (488 px inner). Rows 72 px, buttons
64 px tall; the toggle and the step buttons are 120 and 96 px wide. The body
scrolls when the list is long. Verified in Normal, Night and Outdoor in the
simulator against netd-testhooks and the fake supplicant.

## Launcher

Id `settings`, name "Settings", icon `LV_SYMBOL_EDIT` (the gear is System's
and the list glyph Timber's). Tenth tile, fifth row.

## Physical validation (not done)

Covered by step 8 of `docs/hardware/WIFI_2026-09-12.md` and step 6 of
`docs/hardware/DISPLAY_BRIGHTNESS.md`. In addition: type a passphrase with the
touch keyboard and with the physical keyboard (the SHOW toggle helps against
the known 1-in-10 mis-key rate of the 52 px keys), and check that no row or
button is hard to hit on glass.
