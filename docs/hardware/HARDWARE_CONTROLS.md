# Hardware controls: the keyboard base's keys, LEDs and light

Branch `feat/hardware-controls`, from master `749f4f1`. What the keyboard
base's own controls are, where each one reaches Linux, what the vendor
launcher did with them, and what Doors now does. DS §44 (PROPOSED; §44.1,
what the keys mean, ACCEPTED 2026-10-03) is the design side; `docs/api/shell.md` has `shell.action`, `shell.key` and
`shell.info.hardware`.

Evidence classes as in AGENTS.md: **VERIFIED** (measured on unit A or B),
**DOCUMENTED** (vendor or repository source, cited), **ASSUMED**.
"Vendor" below means `vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/`
(`ui_hardware.c` unless another file is named) and
`vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md`.

## 1. Where the controls reach Linux

**Nothing on the keyboard base is a kernel input device.** On both units
`/proc/bus/input/devices` lists exactly two devices, `K230 PMU Power Key`
(event0) and `goodix_ts` (event1), and `/dev/input` holds event0, event1,
mouse0 and mice (**VERIFIED**, units A and B, 2026-09-30). There is no
gpio-keys or adc-keys node in the vendor BSP either (DOCUMENTED). Every key
of the base, the function row included, arrives only through the TCA8418
matrix controller that the shell itself polls over bit-banged I2C on
GPIO46/47 (`KEYBOARD_BRINGUP_2026-09-10.md` §1, `KEYBOARD_DRIVER_DESIGN`).
evdev tooling (`evtest`, `/dev/input/event*`) therefore cannot see them, and
the only remote way to exercise them is to feed controller bytes into the
driver's own path: `shell.key` (§7).

Both units run the same image (0.2.1, `BUILD_ID=749f4f1`, doors-shell md5
`92c12be3`), the same kernel (`6.6.36 #2`), report the same two input
devices, the same keyboard driver start-up line
(`keyboard: TCA8418 ready, polling every 15 ms (INT-gated)`), the same GPIO
consumers (gpiochip1 lines 10, 14, 15), the same PWM chips (`pwmchip0` =
`pwm0_2`, `pwmchip3` = `pwm3_5`), the same video nodes and sound card, and
the same io52 and io0 pad words (**VERIFIED**, both units).

## 2. The keys on the matrix

`code = raw & 0x7F`, bit 7 press / release, `row/col = (code-1)/10,
(code-1)%10`. The codes are the vendor's table (`tca8418_key_name`), which
`ui/pocketui/pos_keymap.c` already carries; six of them were read off the
keycaps on unit A in 2026-09 (`KEYBOARD_BRINGUP` §5.1).

| Control | Code | Press / release | Repeat | Reached Doors before | Consumed elsewhere | Class |
| --- | --- | --- | --- | --- | --- | --- |
| F1 | 50 | 0xB2 / 0x32 | none: the TCA8418 reports one press and one release; the driver has no auto-repeat (design §0.4) | counted as "reserved", dropped | no | code DOCUMENTED; path VERIFIED by injection |
| F2 | 60 | 0xBC / 0x3C | as F1 | reserved | no | as F1 |
| F3 | 59 | 0xBB / 0x3B | as F1 | reserved | no | as F1 |
| F4 | 68 | 0xC4 / 0x44 | as F1 | reserved | no | as F1 |
| F5 | 67 | 0xC3 / 0x43 | as F1 | reserved | no | as F1 |
| F6 | 66 | 0xC2 / 0x42 | as F1 | reserved | no | as F1 |
| F7 | 65 | 0xC1 / 0x41 | as F1 | reserved | no | as F1 |
| F8 | 64 | 0xC0 / 0x40 | as F1 | reserved | no | as F1 |
| F9 | 63 | 0xBF / 0x3F | as F1 | reserved | no | as F1 |
| F10 | 62 | 0xBE / 0x3E | as F1 | reserved | no | as F1 |
| F11 | 61 | 0xBD / 0x3D | as F1 | reserved | no | as F1 |
| Orange microphone key | 11 (`MIC`) | 0x8B / 0x0B | as F1 | reserved | no | as F1 |
| LILYGO key | 8 (`LILYGO`) | 0x88 / 0x08 | as F1 | reserved | no | as F1 |
| Fn / Fn-R | 9 / 3 | 0x89 / 0x09, 0x83 / 0x03 | modifier | tracked, typed nothing | no | as F1 |
| Caps | 10 | 0x8A / 0x0A | toggles on press | toggles Caps | no | VERIFIED (typing, 2026-09-12) |

The owner had pressed three of these keys on unit A and two on unit B
before this work: the shell's exit lines count them (`3 reserved`,
`2 reserved`, 2026-09-29/30, **VERIFIED** from the logs) - the path from the
matrix to the driver was already live, the keys only had no meaning.

Positions 4, 30, 31, 69 and 70 carry no name in either table. The driver
now logs a press at any of them (`keyboard: press at unnamed matrix code N`,
the first 20), so a control wired there would identify itself.

## 3. Buttons that are not on the matrix; the top two-way control: UNIDENTIFIED / NOT IMPLEMENTED

| Control | Source | Level | Doors today | Class |
| --- | --- | --- | --- | --- |
| Power key | PMU INT0, kernel `k230-pmu-pwrkey`, `/dev/input/event0`, `KEY_POWER` (116) | idle low, pressed high | read by the shell since `feat/k230-power-key` (DS §56): short press screen off/wake, ~1 s hold the power menu; the kernel's own 5 s hold still powers off (`0064-input-k230-pmu-pwrkey.patch`) | DOCUMENTED; device VERIFIED present |
| BOOT0 button | GPIO0 (gpiochip0 line 0) | idle high, pressed low | not read. Its pad is not a GPIO on this image: iomux word `0x00000AC4` (function 1) on both units; the vendor forces `0x344` (GPIO input, pull-up) to read it | DOCUMENTED; pad word VERIFIED; with the pad switched for a 4 s test on unit A the line read high (idle), and the word was restored exactly |
| RESET | hardware reset, no software path | - | - | DOCUMENTED |

**No two-way (rocker) control exists in any vendor source or document**: no
gpio-keys, no ADC keys, no XL9555 input used as a button, and the vendor
launcher handles exactly two buttons outside the matrix - BOOT0 (screen
on/off) and the power key (hold to power off) (DOCUMENTED, vendor
`ui_hardware.c` BOOT0 poll, `main.c` power-key reader).

**The top two-way control is UNIDENTIFIED and NOT IMPLEMENTED.** The owner
pressed each side of it on both units on 2026-10-01 and got **no response
on either side** (owner, physical test). Its signal source is unknown: no
source this work could find reports it. Nothing in Doors is bound to it -
no Back, no Vision, nothing - and no binding is to be guessed. Identifying
it is new hardware reverse engineering and belongs to its own piece of
work, not to this branch.

What stays, and why it is not a claim about that control:

- `HW_ACTION_BACK` and `HW_ACTION_VISION` remain as semantic actions. They
  are reachable only through `shell.action back|vision` (the bench and the
  tests) and are exercised there (§8, §9). **No physical control carries
  either of them.**
- `tests/hw/hw_buttons_watch.sh`, the bench watcher (power key, BOOT0 with
  its pad restored, the keyboard matrix including unnamed positions), stays
  as a tool. It is what the owner's test used.
- The power key was not read by this branch. Since `feat/k230-power-key`
  the shell reads it (DS §56, ui/shell/shell_power_key.h), without grabbing
  it; the kernel's own 5 s hold-to-power-off is untouched.

## 4. Indicator LEDs

| LED | Where | Drive | Doors meaning | Class |
| --- | --- | --- | --- | --- |
| 1 | XL9555 at 0x20, port 0 pin P03 | active low | Caps Lock | pin DOCUMENTED (vendor index 0); behaviour VERIFIED (owner, 2026-10-01) |
| 2 | P04 | active low | microphone in use | pin DOCUMENTED (vendor index 1); behaviour VERIFIED (owner, 2026-10-01) |
| 3 | P05 | active low | camera in use | pin DOCUMENTED (vendor index 2); behaviour VERIFIED (owner, 2026-10-01) |

The vendor's LED test page drives each index alone, so the three are
independent (DOCUMENTED). The XL9555 is on the same two bit-banged lines as
the TCA8418; the vendor probed it as present on unit A (`XL:yes 0x20`,
KEYBOARD_BRINGUP §3, VERIFIED 2026-09-11). Doors reaches it through the same
bus object in the same process and thread - one bus owner still (design
§0.2). Only bits 3-5 of OUTPUT0/CONFIG0 are ever written; everything else on
the expander is read and written back as found. The register writes were
VERIFIED by read-back on both units (§9); the LEDs themselves were checked
by the owner on 2026-10-01: OK (§10).

How each is derived:

- **Caps**: the key map's own Caps state, so it goes out with an overflow
  or a controller recovery that drops the modifier state, and with the
  keyboard.
- **Microphone**: any ALSA capture substream that is open -
  `/proc/asound/card*/pcm*c/sub*/status` not `closed`. That is Wave's and
  Recorder's capture (pocketaudio), `arecord`, anything.
- **Camera**: any process holding a camera capture node open - the V4L2
  devices named `vvcam*` (`/dev/video1..3` on the K230; `video0` is the VPU
  and `video4` the 2-D engine, and neither is the camera), found through
  `/proc/<pid>/fd`. That is `pos-camera` (Camera) and `pos-vision` (Vision
  and DeskBuddy's vision provider), VERIFIED on unit A: each holds
  `/dev/video2` while open, the shell never does.

Both are recomputed from the kernel - the microphone every 500 ms, the
camera every second (§9, CPU) - and never from an app's say-so, so an app that closes, a helper that dies, a crash and an orphaned
helper all give the right answer on the next look (an orphan still holding
the camera keeps the LED lit, which is the truth). On a clean shell exit all
three go out; after a kill the next shell's bring-up writes all three from
scratch.

## 5. Keyboard light

GPIO52 as PWM4 = channel 1 of the pwmchip whose node is `pwm3_5`
(`pwmchip3` on both units, VERIFIED present). The vendor muxes io52 to
`0x1191` (PWM4) and writes enable 0, duty 0, period 20000 ns, polarity
`inversed`, duty = period x (100 - pct) / 100, enable 1 (DOCUMENTED); U-Boot
drives GPIO52 low at boot and the pad word is `0x18F` (GPIO) on both units
until something claims it (VERIFIED). Doors does exactly the vendor's
sequence (`ui/shell/kbd_light.c`), the mux write in the one file that maps
the iomux block (`kbd_bus_k230_light_mux()`, restored on a clean exit).
Levels 0 (off) to 100 in steps of 10, bounded, never wrapping, kept as
`keyboard_backlight` in settings.conf and applied at start; with nothing
stored the light is left as booted (dark). The PWM writes are VERIFIED on
both units (§9); the light itself was checked by the owner on 2026-10-01:
OK (§10).

## 6. The vendor launcher's own mapping (reference)

`keyboard_hotkey` table, defaults, settings `keyboard.hotkey.f1..f11`
(DOCUMENTED, vendor `ui_hardware.c`):

| Key | Vendor default | Doors |
| --- | --- | --- |
| F1 | Home | Home |
| F2 | Settings | Settings |
| F3 / F4 | keyboard backlight -/+ 10 % (0..100) | the same |
| F5 / F6 | volume -/+ (0..45 in steps of 3, `amixer`) | the system volume, -/+ 10 (10..100), the shell's own setting |
| F7 | screenshot to `/root/screenshots` | screenshot to `<state>/screenshots` |
| F8 | Terminal | Terminal |
| F9 | Meshtastic page | RIFT |
| F10 / F11 | display brightness -/+ max/12 | display brightness -/+ 10 % (10..100), the shell's own setting |
| MIC | push-to-talk on its Xiaozhi and Meshtastic pages, nothing elsewhere | Wave (press only) |
| LILYGO | nothing alone; with Fn a screenshot | Terminal |
| Fn+Space | pinyin toggle, LED P04 | nothing (P04 is the microphone LED) |
| Fn+B | backlight on/off | nothing |
| Esc | "back" when no app claims keys | unchanged: Esc is a key to the focused object, as before |
| BOOT0 | screen on/off | not bound (§3) |
| Power | hold 0.8 s: power-off overlay | not read (kernel hold 5 s) |

The vendor runs its F-key hotkeys before any app's key handler, so they work
in its terminal too; it has no raw F-key delivery.

## 7. What Doors does

```
TCA8418 FIFO byte ─> pos_keymap (RESERVED for F1-F11, MIC, LILYGO)
                      │
                      ├─ Fn held and the Terminal has the keys: POS_KEY_F(n) raw to it
                      └─ else hw_action_for_key(code)  ─> queued in shell_kbd
                                                         │ after the drain, bus released
shell.action {action} ──────────────────────────────────┤
                                                         v
                                   hw_action_run(host, action)   (ui/shell/hw_actions.c:
                                                         │        bounds, one instance,
                                                         │        the lock, Back)
                                                         v
            shell.c host: app_open() via the registry, the back slab's
            pocketos_shell_go_home(), app.h `back`, controls_close(),
            home_folder_close(), pocketos_shell_volume_set(),
            pocketos_shell_brightness_set(), kbd_light, the F7 capture
```

- **One instance.** An action whose app is on screen is a no-op: reopening
  would close it first (app_open) and lose its shell or camera session.
- **Back** is the header's back slab, after the app's own way out of a
  sub-page (`app.h` `back`: Settings' sheets, System's Diagnostics, Zabbix's
  host detail, RIFT's node detail and sections, DeskBuddy's panel); at home
  it closes Controls, a folder or the picker; on the launcher's own page it
  does nothing. No second navigation stack. **No key of the base carries
  Back** (nor Vision): the control they were meant for is unidentified
  (§3), so both are reachable through `shell.action` only.
- **The lock and alerts.** While the lock screen or an alert (DS §18.8) is
  up, actions that would open or leave an app are refused; volume,
  brightness, keyboard light and F7 still work.
- **The Terminal.** A bare function key is a Doors shortcut everywhere,
  the Terminal included, as on the vendor launcher. With **Fn held**, and
  only while the Terminal's grid has the keys, the function key goes to the
  program in it as xterm sends it (`ESC O P..S`, `ESC [ 15 ~` ...,
  `apps/terminal/term_keys.c`). The F-key codes are raw-only: pos_input
  drops them at delivery to anything but the raw target, so they can never
  be typed into a field. Ctrl, Alt, Tab, Esc and everything else reach the
  Terminal exactly as before.
- **Normal typing is untouched**: only the thirteen codes above carry an
  action, and they never typed anything.
- **Confirmation**: a 1.2 s flash on the top layer for level changes and F7
  (DS §44.3).
- **F7**: the vendor LVGL on the card has no LV_USE_SNAPSHOT, so
  `shell.screenshot` cannot work there (KNOWN_ISSUES). F7 runs the image's
  own `ffmpeg -f kmsgrab ... -vf hwdownload,format=rgb565le` as a child,
  reaped from the shell's tick (10 s limit, one at a time) - the capture
  every hardware gate has used, which yields the frame already rotated as
  the screen shows it (VERIFIED on unit B, landscape, 1232x568 PNG, 1.3 s).
  With LV_USE_SNAPSHOT (the simulator) it uses the existing snapshot path.
- **Future long press** on the microphone key: releases still pass through
  the driver's event path; a press-and-hold would be timed between the two.
  Not implemented.

## 8. Tests

| Suite | What | Checks |
| --- | --- | --- |
| `tests/hw_actions_test.c` (make test) | every mapping, no typing key mapped, apps and one instance, unavailable app, Back, the lock, every level bound without wrap, off-grid levels, F7 | see the gate log |
| `tests/kbd_leds_test.c` (make test) | XL9555 model: off before output, only three bits touched, independence, no traffic for an unchanged state, 100 cycles, failure, recovery, read-back, exit | |
| `tests/hw_activity_test.c` (make test) | fake /proc and /sys: capture open/closed/playback, camera held, released, holder vanished, VPU not the camera | |
| `tests/kbd_light_test.c` (make test) | fake PWM: found by node, vendor sequence, inverted duty, bounds, stored value, refused write | |
| `tests/term_keys_test.c` (make test) | F1-F12 as xterm sends them | |
| `tests/shell_kbd_test.c` (SDL, `kbd_shell_test.sh`) | the real driver glue: actions on press only, held key one action, Fn+F5 raw to the Terminal and not to a field, Ctrl+C unchanged, injection, Caps/mic/camera LEDs through the XL9555 model, overflow puts Caps out, destroy leaves all dark | |
| `tests/hw_actions_shell_test.sh` (SDL shell) | the whole path in the running shell over IPC | |

## 9. Hardware gate, 2026-09-30 (units A and B)

Both units were on master `749f4f1` in full (doors-shell md5 `92c12be3`).
Only `doors-shell` was replaced, hot, with `/etc/init.d/S90doors-shell`
(no flash, no reboot); the final build is **`0cc4b66`** (riscv64, stripped,
md5 `4cef0289`, 0 first-party warnings), built from the exact commit in a
clean clone. Rollback on each unit: `/root/rollback-hwctl/RESTORE.sh` (the
749f4f1 shell and the pre-gate settings.conf). Gate tooling:
`out/hwctl-gate/` (outside the repo): `g3_dev.sh` runs on the unit and
drives everything through `shell.key` - raw controller bytes through the
driver's own key path - `shell.action`, the touch injector and `arecord`.

| Part | Unit A | Unit B | What it covers |
| --- | --- | --- | --- |
| nav | 48 PASS, 0 FAIL | 48 PASS, 0 FAIL | every F-key, mic and LILYGO action (injected controller bytes); Back via `shell.action`; one instance (F8 x2 + LILYGO = one Terminal, mic in Wave = noop); Back in Wave, RIFT, Terminal, a folder, Controls, at the launcher (noop); Terminal -> Back -> Terminal; letters are no actions; the lock refuses F8 and Back; volume 90 -> 100 -> noop, 11 x F5 stops at 10; brightness steps and both bounds (10 floor, 100), restored; keyboard light up to 100 (duty 0), down to off (duty 20000), noop at off, io52 = PWM4 `0x1191`, pwm4 20000 ns inversed enabled, persisted; F7 PNG written, no ffmpeg left; Fn+F5 in the Terminal is no action and the volume does not move; bare F5 in the Terminal is the shortcut |
| leds | 12 PASS | 12 PASS | the XL9555 answers; its registers read back: P03-P05 outputs, all dark at rest (`0xFF`); Caps on -> P03 low (`0xF7`), off -> dark; `arecord` running -> microphone in use and P04 alone low (`0xEF`); done -> dark; a capture killed with -9 -> released |
| soak | 7 PASS (FDs 13 -> 13, RSS 15696 -> 15696 kB) | 7 PASS (FDs 12 -> 12, RSS +128 kB) | 20 cycles of Terminal/Home/Wave/Back/Settings/Home/light up/down: no FD growth, no leftover children, no restart, no crash |
| camera | 13 PASS | 13 PASS | Camera -> P05 lit, Back -> released and dark; Vision via its action, again = noop, exactly one pos-vision; Vision -> Terminal releases the camera, no pos-vision left; Terminal -> Back; DeskBuddy lights the camera (its vision provider) and Back releases it, all LEDs dark |

Every part also checked: shell pid unchanged, supervisor `restarts=0`, no
new crash report, no ERROR line in the shell log during the run.

Through the real apps (touch-injected, logical coordinates from an F7
capture of the screen):

- **Recorder, unit A**: RECORD -> microphone in use, P04 lit, `pos-record`
  running; STOP -> released, dark; RECORD again, then Back while recording
  -> home, helper gone, dark. (The two test recordings were deleted.)
- **Wave, unit B**: opened by the microphone key; LISTEN -> P04 lit,
  `pos-wave` running; stop -> dark; LISTEN again, the microphone key inside
  Wave = noop, Back while listening -> home, helper gone, dark.
- **RIFT, unit A** (the app `back` hook): NODES tab -> Back -> still RIFT, on
  ACTIVITY -> Back -> home. Mesh TX unchanged (nothing tapped transmits).
- **Terminal, unit B**: `cat -v` typed through injected keys (Shift+E gave
  `-`), then Fn+F5 -> the screen shows `^[[15~` (read off an F7 capture), so
  the program received ESC [ 15 ~; bare F5 in the Terminal stepped the
  volume instead.

Found and fixed on the hardware:

1. **`isp_media_server` holds the camera nodes for good.** The vendor ISP
   daemon (pid 148 on unit B) opens `/dev/video1..3` at boot, so "a process
   holding a capture node" lit the camera LED permanently. It is the ISP's
   broker, not a user, and is now left out (by its comm); pos-camera and
   pos-vision still count (VERIFIED both ways on both units).
2. **The /proc walk's cost.** Idle shell CPU on unit B was 1.2 % of a core
   with 749f4f1 and 2.2 % with the camera looked at every 500 ms; at once a
   second it is 1.7 % on both units.
3. The fresh shell starts behind the lock, where navigation is refused by
   design (the first gate run showed exactly that: every app action
   `refused`, the levels working) - the gate now unlocks first.

Both units were left at home, unlocked, LEDs dark, keyboard light off
(`keyboard_backlight=0`, as it boots), volume and brightness as found, no
screenshots left on the card; `settings.conf` differs from the pre-gate copy
only by `keyboard_backlight=0` (and, on unit B, `display_brightness=100`,
the level it booted at).

## 10. The owner's physical test, 2026-10-01

Run by the product owner with real presses and real eyes on the base, after
the gate in §9 had driven the same paths by injected controller bytes.

| Control | Result |
| --- | --- |
| F1-F11 | OK |
| Orange microphone key (Wave) | OK |
| LILYGO key (Terminal) | OK |
| Keyboard light (F3/F4) | OK |
| Indicator LEDs (Caps, microphone, camera) | OK |
| Fn+F-key in the Terminal | OK |
| Top two-way control | **NO RESPONSE on either side, on either unit** - UNIDENTIFIED / NOT IMPLEMENTED (§3) |

## 11. Known limitations

- **The top two-way control is UNIDENTIFIED / NOT IMPLEMENTED** (§3): it
  gave no response on either side, and nothing is bound to it. Back and
  Vision have no physical key; they are reachable through `shell.action`
  only.
- The camera LED follows node holders; a future consumer that reaches the
  ISP only through `isp_media_server` (not by opening a node) would not
  light it. No such consumer exists in Doors.
- Fn+F-key reaches only a raw key target (the Terminal); nowhere else has a
  use for a function key.
- F7 on the device depends on the image's ffmpeg with kmsgrab (present in
  0.2.1); `shell.screenshot` itself still needs LV_USE_SNAPSHOT.
- No long press, no remapping, no settings page for any of this (out of
  scope by design).
- Unit A has 79.8 MB and unit B 94.0 MB free on `/`; F7 PNGs are about
  300 KB each and are not pruned.
