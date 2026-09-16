# Physical keyboard bring-up, discovery checkpoint, 2026-09-10

**Updated 2026-09-11, then twice on 2026-09-12: the key map, then the driver
bring-up.** The keyboard base board is attached to unit A. This records what
the hardware is, what has been established about it, what has not, and where
the driver now stands.

**Status: PRESENCE VERIFIED. SIX KEY CODES VERIFIED AGAINST THE KEYCAPS, TWO
SYMBOLS CORRECTED. THE REST OF THE MAP STILL UNREAD. POWER PATH UNRESOLVED.
DRIVER RUNNING ON UNIT A, TYPING VERIFIED END TO END (§5.2).** The TCA8418
answers and the keyboard types correctly both in the vendor launcher on unit
A (§0) and now in PocketOS itself (§5.2). The transport is documented from vendor
sources and the running device tree (§1). The translation layer (§5) has now
been compared with the physical keys for six codes — Shift 7, Z 18, Q 20,
A 29, J 34, W 39 — and two of the vendor's shifted symbols were wrong and are
corrected (§5.1). Every other entry is still the vendor's table, unread.
Which supply the base board needs is still an open question, and nothing
below asserts one (§3).

## 0. 2026-09-11 correction: the actual cause

**The "TCA8418 not detected" result was caused by protective plastic film
covering the connector contacts.** With the film removed and the keyboard
base reseated, the physical keyboard works correctly in the LILYGO vendor
launcher on unit A.

| Fact | Class |
| --- | --- |
| The keyboard base answered as not present while the film covered the contacts | VERIFIED (operator, vendor launcher) |
| With the film removed and the base reseated, the TCA8418 is present and the keyboard works in the vendor launcher | **VERIFIED** (operator, vendor launcher, unit A) |
| Which power configuration that working test used | **UNRESOLVED** — not recorded (§3) |

This withdraws, as evidence about the hardware, everything the earlier
investigation read off the covered connector:

- The vendor launcher's "not detected" readings, with and without the
  keyboard's own USB-C connected, measured the film, not the power path.
  They say nothing about whether that USB-C is needed.
- The theories tested before the cause was found — the reset line held low,
  a missing supply on the keyboard's USB-C, the base needing its own power,
  a difference in the board without the LTE modem — were not the cause.
  None of them is established as true or as false by that work.
- The GPIO42 reading in §2 was taken with the contacts covered and does not
  describe the connected base.

## 1. What the keyboard is

| Fact | Value | Class |
| --- | --- | --- |
| Controller present and answering | TCA8418 matrix controller; the keyboard works in the vendor launcher after the reseat | **VERIFIED** (§0) |
| Controller part | TI **TCA8418**. The vendor's `APP_DEVELOPMENT_RULES.md` writes "TCA8418/TCA8414" | DOCUMENTED |
| Board | nRF9151 cellular / GNSS / keyboard base board | DOCUMENTED |
| Matrix | 7 rows × 10 columns, key codes 1–70; 65 positions used | DOCUMENTED |
| I2C address | **0x34** (7-bit) | DOCUMENTED |
| Bus | **bit-banged** on GPIO46 (SCL) / GPIO47 (SDA) | DOCUMENTED |
| Reset | **GPIO43**, active low | DOCUMENTED |
| IRQ | **GPIO42** | DOCUMENTED |
| Backlight | **GPIO52 / PWM4** | DOCUMENTED |
| Event format | one byte: bit 7 press (1) / release (0), bits 0–6 the key code | DOCUMENTED |
| Registers | CFG 0x01, INT_STAT 0x02, KEY_LCK_EC 0x03, KEY_EVENT_A 0x04, KP_GPIO 0x1D–0x1F, DEBOUNCE_DIS 0x29–0x2B | DOCUMENTED |
| Vendor initialisation | reset GPIO43 low 3 ms then high 12 ms; KP_GPIO1/2/3 = 0x7F/0xFF/0x03 (7 rows, 10 columns); DEBOUNCE_DIS1–3 = 0x00; CFG = KE_IEN \| OVR_FLOW_IEN \| OVR_FLOW_M; drain the FIFO | DOCUMENTED |
| Companions on the same bus | BQ25896 charger 0x6B, BQ27220 gauge 0x55, XL9555 expander 0x20–0x27 | DOCUMENTED; presence **UNRESOLVED** (§3) |

Sources: `vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md` and the
vendor launcher's own driver,
`vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/ui_hardware.c`
(`tca8418_init_device`, `tca8418_read_fifo_events`). That the launcher binary
on the card was built from exactly this source is ASSUMED.

### The transport is not a hardware I2C controller

This is the finding that matters most, and it is not what the pin map's
wording suggests. The pin map calls GPIO46/47 "I2C4 SCL/SDA", but in this
kernel's device tree **I2C4 is muxed elsewhere**:

```
cam_i2c4_pins   pins=io7 io8   function=alt2   <- I2C4 goes to the camera
```

VERIFIED on the running board: `i2c@91409000` (I2C4, Linux `i2c-0`) uses
pinctrl phandle 0x35, which is `cam_i2c4_pins`. Its only child is the GC2093
camera at 0x37, and a scan of both hardware I2C buses finds only 0x37, 0x3b
and the touch controller at 0x5d. **No hardware I2C bus reaches the keyboard.**

The vendor gets there by not using a controller at all. It re-muxes io46 and
io47 to plain GPIO by writing the iomux block at `0x91105000` through
`/dev/mem`, requests the two lines open-drain with pull-ups through libgpiod,
and bit-bangs I2C on them; afterwards it puts the mux back. VERIFIED on the
running board: io46 and io47 read `0x800019D1`, which is exactly the vendor's
own restore value (function select 3), and the vendor's bit-bang value is
`0x800001D1` (function select 0).

Consequences for PocketOS: a physical-keyboard driver needs `/dev/mem`, must
own the mux while it holds the bus, and cannot use `i2cdetect`, `i2cget` or
any `/dev/i2c-*` path. None of the standard I2C tooling on the card can see
this keyboard. Only one program may drive the bus at a time: the vendor
launcher and a PocketOS driver must never run together.

## 2. What was measured on unit A, 2026-09-10

Runtime `0.0.8` build `0b16f0e`. All read-only. **Taken with the connector
contacts still covered (§0)**: the rows about software and the device tree
still hold; the GPIO42 row does not describe the connected base.

| Measurement | Result | Class |
| --- | --- | --- |
| `/proc/bus/input/devices` | only the PMU power key and `goodix_ts`. **No keyboard input device** — there is no kernel driver for it | VERIFIED |
| USB | only the Realtek Ethernet and two root hubs; `usbhid` loaded but bound to nothing | VERIFIED |
| `/dev/input` | `event0`, `event1`, `mouse0`, `mice` — nothing new | VERIFIED |
| Hardware I2C scan, `i2c-0` | only 0x37 (camera) | VERIFIED |
| Hardware I2C scan, `i2c-1` | 0x3b (unidentified) and 0x5d (touch, driver bound) | VERIFIED |
| Device tree | `keyboard_irq_gpio42_pins` and `keyboard_backlight_pwm4_pins` exist, and **nothing references either**. There is no keyboard node | VERIFIED |
| io46 / io47 mux | `0x800019D1` — function 3, not GPIO | VERIFIED |
| GPIO42 (IRQ) | read low under every bias setting — **with the contacts covered; not valid for the connected base** | superseded |
| dmesg | no keypad, matrix, TCA or HID device probe of any kind | VERIFIED |

One test was attempted and is reported as **inconclusive rather than
evidence**: reading GPIO46/47 against an internal pull-down, to see whether
the base board's I2C pull-ups hold them high. Both read high — but so did a
control pin with nothing attached (GPIO44), so `gpioget --bias` is not
overriding the pad configuration on this SoC and the reading discriminates
nothing. It is recorded here so nobody repeats it expecting an answer.

## 3. Power and routing: confirmed, and still open

**Confirmed.**

- The TCA8418 answers and the keyboard works in the vendor launcher on unit
  A once the film is gone and the base is reseated. VERIFIED (§0).
- The launcher reaches it over the route in §1. DOCUMENTED from its source;
  the installed binary matching that source is ASSUMED.
- A PocketOS driver cannot use any hardware I2C bus. VERIFIED (§1).

**Open — UNRESOLVED, and not to be assumed.**

- **Which power configuration the working test used.** Whether the
  keyboard's own USB-C was connected, whether a battery was fitted, and
  whether the K230's USB-C alone was the supply, was not recorded.
  **Partly answered 2026-09-11/12:** the key-map session ran with the K230's
  USB-C in a PC port, the **keyboard base USB-C not connected**, a **battery
  fitted** in the base, and a USB-C Ethernet adapter attached. The keyboard
  worked throughout. **Answered 2026-09-12:** the base's own power switch was
  OFF for all of that work (next entry), so its battery was isolated and the
  base ran from the K230 expansion connector alone.
- **The base has a physical ON/OFF switch** (operator, 2026-09-12). It
  **disconnects battery power to the base**, and it was **OFF during the
  detach test** of that day, which is what made the base safe to unmate once
  the K230's own USB-C was pulled: with the switch off and no base USB-C,
  the board has no source of its own. **The operator confirms the same switch
  was OFF during the Block B and C1/C2 typing tests**, so the battery was
  isolated throughout them.
- **Whether the base board runs from the K230's expansion connector alone.**
  **Answered 2026-09-12: for the keyboard, it does.** With the base's USB-C
  disconnected and its battery switch OFF, the TCA8418 initialised and
  delivered 142 keys across Block B and the C1/C2 restart tests. What the
  rest of the base needs — nRF9151, charger, gauge — is still open.
- **Whether the companions answer.** **Measured 2026-09-11** in the power
  setup above, from the launcher's own probe line:
  `[keyboard-base] Detected 6B:no 55:no 34:yes XL:yes 0x20 on I2C4 SDA47/SCL46`
  — so the **TCA8418 (0x34) and the XL9555 (0x20) answer**, while the
  **BQ25896 (0x6B) and BQ27220 (0x55) do not** in this configuration.
  Whether they answer in another power setup is still open.
- **Whether the GPIO42 interrupt line works. Answered 2026-09-12: it does.**
  The PocketOS driver reads it as a level and kept it as its gate on unit A
  (§5.2), so the earlier low reading really was the plastic film. The line is
  held as `pocketos-shell-kbd-irq`, input with a pull-up, gpiochip1 line 10.
- **Whether the keyboard backlight (GPIO52 / PWM4) and a Caps LED** — the
  vendor driver toggles XL9555 output 0 on Caps — exist and work.

**How to settle it without new code.** The vendor launcher probes all four
devices and writes one line to its standard error, which its init script
appends to `/var/log/k230_phone_ui.log`:

```
Detected 6B:yes 55:yes 34:yes XL:yes 0x20
```

or `Keyboard base not detected`. Record that line together with the exact
physical power setup, once for each configuration, starting with **only the
K230's normal USB-C and no battery**. The vendor Keyboard page also shows an
`irq=` counter, which answers the GPIO42 question for each key pressed.

## 4. Why no driver has been written

The earlier stop point was the presence question, and that is answered
(§0). A bit-banged probe had been written and cross-compiled to settle it;
copying it to the board was refused by the sandbox, it was never run, and it
is no longer needed for presence. It lives only in a session scratchpad and
is not part of the tree.

The driver is still deliberately not started. The translation layer in §5 was
copied from the vendor's tables, and those tables did carry two wrong
entries: the keycap reading on 2026-09-12 found `_` on W and `'` on Q where
the vendor had a second `~` and a second `` ` `` (§5.1). That is exactly the
failure a driver would otherwise have inherited — typing the wrong characters
with full confidence. The six codes in §5.1 are now hardware evidence; the
rest of the map is still the vendor's and unread, and the transport, the
interrupt and the power path are unresolved (§3), so the driver waits.

## 5. What was built

**`ui/pocketui/pos_keymap.[ch]` — the translation layer, and nothing else.**

It turns a raw TCA8418 FIFO byte into the one logical vocabulary of DS §17.4:
a Unicode code point for a printable character, an `LV_KEY_*` constant for
anything else. It is pure — no I/O, no LVGL objects, no hardware — which is
what makes it testable on the host.

`tests/pos_keymap_test.c` covers it in 79 checks: every letter, every digit,
Shift, Caps, the two together, the keycap symbols, space, enter, backspace,
escape, tab, all four arrows, the modifiers, releases delivering nothing,
one press giving exactly one key, unknown and out-of-range codes ignored
without touching the modifier state, and the function row reported as reserved
rather than guessed at.

**The layout is BlackBerry-style**, which the keycaps confirm: Shift reaches
the *symbol printed on the key*, not a capital — Shift+A is `~`, Shift+K is
`/`, Shift+L is `?`. Capitals come from **Caps**. Only Z, X, C and V carry no
symbol, so only those four give a capital under Shift; Z's bare keycap was
read on unit A and agrees. The rest is the vendor's map reproduced
faithfully. **Six codes have been read off the physical keyboard (§5.1); no
other key's code or legend has.**

Entries that looked wrong in the vendor's own tables. The first two are now
settled by hardware (§5.1); the rest are still unread:

- `~` appeared on both **W** and **A**, and `` ` `` on both **Q** and **J**.
  **Resolved 2026-09-12:** the keycaps carry `_` on W and `'` on Q, so the
  vendor's table duplicated `~` and `` ` `` and omitted `_` and `'`
  altogether. A keeps `~` and J keeps `` ` ``.
- The apostrophe `'` and the underscore `_` appeared on no key at all.
  **Resolved:** they are the Shift legends of Q and W.
- `,` sits on **H** and `.` on **B**; `<` on **N** and `>` on **M**. Still
  unread.
- There are two space codes, 5 and 14. Still unread.

The function row (F1–F11), the Fn keys, the LILYGO key and the mic key are
deliberately **not mapped**. They are reported as reserved. The vendor binds
them to its own launcher's hotkeys, which is not a contract PocketOS has.

### 5.1 Verified against the hardware, 2026-09-12 (unit A)

Read on the vendor launcher's Keyboard test page with the panel captured over
SSH (§6). Every line satisfies `code = raw & 0x7F`, bit 7 = press, and
`row/col = (code-1)/10, (code-1)%10`.

| Key | Press | Release | Code | Row/col | Keycap legend | Class |
| --- | --- | --- | --- | --- | --- | --- |
| A | 0x9D | 0x1D | 29 | 2 / 8 | A + orange `~` | **VERIFIED** |
| W | 0xA7 | 0x27 | 39 | 3 / 8 | W + orange `_` | **VERIFIED** |
| Q | 0x94 | 0x14 | 20 | 1 / 9 | Q + orange `'` | **VERIFIED** |
| J | 0xA2 | 0x22 | 34 | 3 / 3 | J + orange `` ` `` | **VERIFIED** |
| Z | 0x92 | 0x12 | 18 | 1 / 7 | Z only, no orange symbol | **VERIFIED** |
| Shift | 0x87 | 0x07 | 7 | 0 / 6 | orange arrow | **VERIFIED** |

- **Shift is the orange symbol layer.** Its keycap arrow is the same orange as
  the secondary legends; Alt, Fn and Caps Lock do not carry that colour.
- **Shift is an ordinary matrix key.** It is reported as its own event and
  never changes the code of the key pressed with it: A and W give identical
  bytes with and without Shift. Two held keys are both reported. Modifiers do
  not auto-repeat; ordinary keys do, and the launcher logs
  `[extension-keyboard] repeat code=<n>` for them.
- **The vendor's on-screen grid shows the same wrong table** as the vendor
  driver: it labels W with `~` and Q with `` ` ``, so `~` appears on two keys
  and `` ` `` on two keys, while neither `'` nor `_` appears anywhere on the
  grid. pos_keymap inherited that table, which is why both carried the same
  two errors.
- Consequently `pos_keymap.c` now returns `'_'` for code 39 and `'\''` for
  code 20; codes 29, 34 and 18 were already right.
- Event counters on the page read `ovr=0 err=0` throughout, so the controller
  neither overflowed nor lost events during the readings.

### 5.2 Driver bring-up on unit A, 2026-09-12 — gate A: PASS

The driver designed in `KEYBOARD_DRIVER_DESIGN_2026-09-12.md` (commit
d3b7bb0) was deployed with `platforms/k230/scripts/deploy.sh`. **The card was
not reflashed.** The unit reports `0.0.8 BUILD_ID=d3b7bb0`.

| Fact | Result | Class |
| --- | --- | --- |
| Driver start-up | `keyboard: TCA8418 ready, polling every 15 ms (INT-gated)` | **VERIFIED** |
| GPIO42 as a level gate | reads as a level and was chosen as the gate at start-up | **VERIFIED** |
| Whether the gate was *kept* for the session | **WITHDRAWN 2026-09-12** — see the note below §5.2; not measurable by this run | — |
| Line ownership | gpiochip1 line 10 `pocketos-shell-kbd-irq` input pull-up; lines 14 and 15 `pocketos-shell-kbd` output open-drain pull-up; line 11 (reset) held only for the pulse | **VERIFIED** |
| Typing, plain | `awqjz` | **VERIFIED** |
| Typing, shifted | `~_'` `` ` `` `Z` | **VERIFIED** |
| Warnings, errors, dropped keys, overflow | none during bring-up | **VERIFIED** |
| Services | sysd, radiod and pocketos-shell `running=1`, `crashloop=0`, `restarts=0` | **VERIFIED** |
| Radio | no Meshtastic process, 0 spidev holders, `meshtastic.autostart=0`, TX counters unchanged at 28 start / 14 done | **VERIFIED** |

**Shift+W is `_` and Shift+Q is `'` in PocketOS itself**, not only on the
vendor's key test page. The ten characters were read back off the panel's
framebuffer rather than transcribed, which is what makes them evidence: the
two entries the vendor table had wrong (§5.1) are now confirmed end to end,
from the matrix code to a character in a focused field.

The polling model the design chose is the one the hardware selected at
start-up: a 15 ms timer that reads the INT line and touches the bus only when
it is asserted.

**Correction, 2026-09-12 (cold merge review).** This section originally added
that "the driver kept the gate and never fell back" and that "the
unconditional 20 ms fallback exists in the driver and was not needed". Both
are withdrawn. The only evidence either claim ever had was the single
start-up line above, which reports the mode chosen at boot and says nothing
about the rest of the session — and the driver as it stood at `d3b7bb0`
switched the gate off at the first unconditional sweep that coincided with a
key being pressed, roughly half a second into any real typing, logging
nothing when it did. The 20 ms fallback period was not implemented either;
the timer stayed at 15 ms in both modes.

So this run is good evidence that the keyboard works — the typing, the
characters read off the framebuffer, the absence of drops and overflow all
stand. It is not, and never was, evidence about which polling mode it worked
in. The gate predicate and the missing log line are fixed on the v0.0.9
branch; the re-measurement is §5.5.

### 5.3 Hardware robustness, 2026-09-12 — blocks B and C: PASS

All of it on unit A against commit `d3b7bb0`, the card never reflashed. Every
character below was read off the panel's framebuffer over SSH rather than
transcribed.

**Block B — typing under load.**

| Test | Result |
| --- | --- |
| 90 consecutive keystrokes, nine `abcdefghij` groups | every group intact: zero drops, zero duplicates |
| Modifier recovery, `a~a_w'q` `` ` `` `jZz` | all five shifted legends correct; every key following a shifted one came out plain; Shift never stuck across five press/release cycles |
| Burst `asdfjkl` x3 then `~_Za` | no drops, no duplicates, no overflow, no stuck Shift |

The burst's first group read `asfdfjkl`, one extra `f`. It is **not** a driver
fault: a duplicated event repeats the same code adjacently and debounce
(enabled, `DEBOUNCE_DIS=0x00`) would do the same, but here the two `f`s are
separated by a `d`. The FIFO is strictly ordered, so the key was physically
pressed twice. 127 keystrokes across the three tests with nothing lost,
reordered or duplicated by the driver.

**C1 — clean SIGTERM: PASS.** Stopped through `S90pocketos-shell stop`.

- The destroy path ran: `keyboard: 142 key(s) delivered, 0 dropped, 0
  reserved`. That counter is the driver's own accounting and is only emitted
  on a clean exit, which is what makes it the proof the path ran.
- GPIO released while down (no `pocketos-shell-kbd` consumer on any line),
  and reacquired identically on restart.
- Mux restore verified: io46/io47 read `0x800019D1` before, during the stop
  and after — never left at the bit-bang word `0x…01D1`.
- Re-init 3.4 s later to `TCA8418 ready, polling every 15 ms (INT-gated)`.
- Typing after the restart: **`a~a`**.
- A supervised stop/start yields a **fresh supervisor at `restarts=0`**, which
  is the expected accounting for an intentional restart.

**C2 — SIGKILL recovery: PASS.** `kill -9` to the shell child only.

- The **same supervisor** (pid 1889) detected the death and restarted the
  shell 2 s later: pid 1900 -> 2818, `restarts=1`, `last_exit_code=137`
  (128+9), `crashloop=0`, no restart loop.
- No reboot: uptime ran 52164 -> 52170 s continuously, and sysd and radiod
  kept their pids.
- Keyboard re-initialised deterministically to 15 ms INT-gated. **No
  destroy/accounting line**, correctly, because cleanup cannot run on this
  path.
- GPIO reacquired: the kernel releases the lines when the process dies, which
  is the mechanism the design relies on when cleanup is impossible.
- Typing after recovery: **`a_w`**.
- **Untested hazard.** The driver claims the bus for roughly 1 ms in every
  15, and this kill landed between polls, so io46/io47 were never left in
  bit-bang mode. A kill landing *inside* the claim window would leave them at
  `0x…01D1`, and the next instance saves whatever it finds — it would then
  adopt the bit-bang word as its restore value, and a later clean exit would
  leave the bus in GPIO mode. The keyboard would still work. This run shows
  the hazard did not occur, **not** that it cannot.

**C3 — base absent and reconnected: PASS.** Powered off, base unmated,
booted; then powered off, remated, booted. Never hot-plugged: no vendor
evidence supports that, so every mate and unmate was done with the SoC
halted, the K230 USB-C out, the base USB-C disconnected and the base's
battery switch OFF.

- Absence is **normal, not an error**: exactly one line, at INFO —
  `keyboard: the controller did not answer; touch only`.
- **No retry timer and no cost.** A failed probe creates no poll timer at
  all, so there is nothing to throttle: 0.30 s of CPU in 85 s of uptime, and
  0.68 s after opening and closing an app; `top` showed 0% CPU, 100% idle.
- **No GPIO consumer retained** — requested, probe failed, released.
- Touch-only operation works: the launcher rendered with all seven tiles and
  the log recorded `open app notes` then `close app notes` 0.9 s apart.
- Reconnecting restores `TCA8418 ready, polling every 15 ms (INT-gated)`,
  GPIO ownership and the mux words. Typing after reconnect: **`a~a`**.

**Power, answered.** The base was remated and the controller initialised with
the **base USB-C disconnected, a battery fitted, and the base's battery
switch OFF**. The switch isolates battery power, so the keyboard path runs
from **K230 expansion-connector power alone** (§3).

**The iomux words, now fully observed.**

| Register | Pin | Cold boot, no driver | After driver init |
| --- | --- | --- | --- |
| `0x911050B8` | io46 SCL | `0x800019D1` | `0x800019D1` between polls |
| `0x911050BC` | io47 SDA | `0x800019D1` | `0x800019D1` between polls |
| `0x911050A8` | io42 IRQ | **`0x000001D1`** | **`0x80000344`** |

io42's cold-boot value had never been seen before: every earlier reading was
taken after the vendor launcher or the driver had already written `0x344`. It
became visible only on the keyboard-less boot, because the driver restored
what it had saved.

**Across all of B, C1, C2 and C3:** `meshtastic.autostart=0` throughout, no
Meshtastic process and no vendor launcher at any check, **0 spidev holders**,
and **no ERROR or WARN** in any shell log for any of the five shell
generations that ran that day. The Meshtastic TX counters stopped being a
usable before/after measure at the first power cycle, because `/tmp` is a
tmpfs and the daemon's log does not survive a reboot; process absence and
zero spidev holders are the evidence from that point on.

### 5.4 DS §18.8 alert isolation, 2026-09-12 — hardware acceptance: PASS

The shipping gate of DS §18.8, accepted on unit A against commit `2afe7fe`
(deployed as `BUILD_ID=52b8302` from a staging clone whose source was
verified byte-identical to the commit, 556 of 556 tracked files). The card
was not reflashed. Every character below was read off the panel's
framebuffer, never transcribed.

**Test A — an alarm over a focused Notes field.** The note held `TEST` with
the touch keyboard up. The alarm fired at 08:52 and the alert took the panel:

| Check | Result |
| --- | --- |
| Alert foreground, whole panel | title `Alarm`, detail `08:52`, Notes fully covered |
| Touch keyboard | **gone** — it was fully up one minute earlier (§18.5) |
| `x`, `y`, `z`, then Backspace | **nothing reached the note**: three captures spanning the four keystrokes are byte-identical by MD5 (`83d0e379…`) |
| Tab then Enter | the alert closed and the alarm was **snoozed** — it stayed enabled, where Stop would have switched a one-shot alarm off |
| After acknowledgement | note revealed still reading exactly `TEST`; Backspace had not even eaten the final `T` |
| Focus | restored to the same field, caret live |
| Touch keyboard | **not** restored — §18.5 gives focus back, not the keyboard |
| Typing afterwards | `OK` appended normally, giving `TESTOK` |

**Test B — Stop, and where focus starts.** A fresh 09:02 alarm over the same
focused field. **Enter alone**, with no Tab, closed the alert, and the stored
alarm flipped from `alarm 1 9 2 0` to `alarm 0 9 2 0`. A one-shot alarm
disables itself only on acknowledgement, so Enter reached **Stop** — which is
what proves focus opened on the dominant action (§18.4). The accent alone
could not prove it: the accent is style, not a focus ring.

**Test C — the timer variant.** `Timer finished`, detail `01:00`, and
**Snooze absent** with Stop spanning the full row. Tab then Enter dismissed
it. That is the visibility-versus-membership hazard settled on hardware: had
Snooze stayed in the group while hidden, Tab would have landed on a control
the owner cannot see and Enter would have done nothing.

**Throughout all three tests:** shell pid 1109 unchanged, all services
`running=1`, `crashloop=0`, `restarts=0`, **0 ERROR/WARN**, the keyboard
still `TCA8418 ready, polling every 15 ms (INT-gated)`, GPIO consumers held
on lines 10/14/15, and the radio untouched — no Meshtastic process, no vendor
launcher, 0 spidev holders, `meshtastic.autostart=0`.

Two notes on method, because they nearly produced wrong answers. A caret
missing from one still was the blink phase, not lost focus, and was settled by
sampling the field over four seconds rather than by judging a single frame.
And `ps | grep -c` run from an inline SSH command counts the command's own
text: every "Meshtastic present" reading in this session came from that
artefact, and each was disproved by feeding the script on stdin instead.

### Architecture, once the key map is confirmed

```
TCA8418 (bit-banged I2C on GPIO46/47)
        |
        v
 transport + poll        <- not written: waits for the key-map check
        |
        v
 pos_keymap_event()      <- WRITTEN AND TESTED (79 checks); six codes read
        |                   off the keycaps (§5.1), the rest still unread
        v
 pos_input_push_key()    <- unchanged, from M3
        |
        v
 queue -> LVGL keypad indev -> focus group -> focused field
```

Nothing above `pos_input_push_key()` changes, which is the point of §17.4:
Notes cannot tell a typed character from a tapped one, and no second text
path is created.

### 5.5 INT gate kept under sustained typing, 2026-09-12 — re-measurement: PASS

The claim §5.2 withdrew, measured properly this time. Deployed with
`deploy.sh` from the pre-merge fix pass; the unit reports `0.0.8
BUILD_ID=6d4b318-dirty`. **The card was not reflashed.**

What makes this measurable at all is the log line the fix adds: the gate can
only be lost, never regained, so a session that never logs
`keyboard: INT gate dropped` kept it throughout. Before the fix there was no
such line, which is why §5.2's version of this claim rested on nothing.

| Fact | Result | Class |
| --- | --- | --- |
| Start-up mode | `keyboard: TCA8418 ready, polling every 15 ms (INT-gated)` | **VERIFIED** |
| Typing | Notes open 10:16:34 → 10:18:15Z, about 40 s of continuous typing | **VERIFIED** |
| Keys delivered | `69 key(s) delivered, 0 dropped, 0 reserved` | **VERIFIED** |
| Text reached a focused field | `note-00000027.txt`, 66 bytes, read back off the store | **VERIFIED** |
| `INT gate dropped` lines | **0**, across the whole session | **VERIFIED** |
| ERROR/WARN in any log since start-up | **0** | **VERIFIED** |
| Services | 6 processes alive; supervise logged only the two deliberate stops, no crash restart, 0 crash reports | **VERIFIED** |
| Radio | 0 Meshtastic processes, 0 spidev holders, backend mock, region EU868 | **VERIFIED** |
| io46/io47 mux at rest | `0x800019D1` both — the saved word, not the bit-bang `0x…01D1` | **VERIFIED** |

The session ran about 101 s, so roughly 200 unconditional sweeps, of which
about 80 fell inside the typing. At 69 keys the raw event rate is near seven
per second counting releases, and an event waits at most one 15 ms poll, so
something like eight of those sweeps landed with the FIFO non-empty — the
exact condition that used to end the gate. Under the old predicate one such
sweep was enough and it was permanent; here there were none.

This is the hardware half of the evidence. The deterministic half is
`tests/kbd_tca8418_test.c`, which crosses twelve sweep boundaries with events
pending on every one of them, and fails when the fix is reverted.

## 6. Reading the key map against the keycaps

**No new code is needed to capture the raw event byte for every key.** The
vendor launcher's **Keyboard** page shows the most recent FIFO event:

```
<NAME> raw=0x<byte> code=<nn> row=<r> col=<c> press|release
```

where `raw` is the byte read from KEY_EVENT_A and `code` is its low seven
bits. `NAME` comes from the vendor's table, so it is what is being checked,
not evidence; the operator's reading of the physical keycap is the evidence.
The page also lights the key in its on-screen grid and counts interrupts
(`irq=`).

**The page is reached as** Settings → Keyboard settings (the first panel is
titled "Extension keyboard") → **drag upward** → Keyboard test. The launcher
runs at `display.rotation=270`, logical 1232x568 landscape, which is why the
row is below the fold and the drag is upward. The page itself shows
`TCA8418 Ready`, the Last key line, the event counters
(`fifo/total/irq/safe/ovr/err/last`), the key grid, a keyboard-backlight
slider and a **Clear keys** control.

**Radio precaution — do this before handing the panel over.** Starting the
vendor launcher also starts its Meshtastic daemon
(`k230_meshtastic_probe --daemon`), which detaches to pid 1, holds
`/dev/spidev0.0` and **transmits on LoRa at power 22** within minutes of
launch. On 2026-09-11 that produced 14 unapproved transmissions between 18:06
and 18:48 before it was noticed. Set

```
meshtastic.autostart=0
```

in `/root/.config/k230_phone_ui/settings.conf` **before** starting the
launcher, and keep that line when the vendor settings are restored
afterwards — the pre-session backup does not contain it, so restoring the
backup verbatim turns transmission back on.

Before pressing the function row or Esc on that page, the vendor's global
bindings must be switched off, or the page navigates away mid-test and F9
opens the Meshtastic page:

- **Hotkeys**: `keyboard.hotkey.f1` … `f11` = 0 in the same settings file.
- **Keyboard settings**: `keyboard.esc_back_enabled=0`.

Both are the vendor launcher's own settings on the card, not PocketOS, and
are put back afterwards.

**Reading the events without the operator transcribing them.** The launcher
logs no per-key line — `/var/log/k230_phone_ui.log` carries only
`[extension-keyboard] repeat code=<n>` for held keys — there is no kernel
input device for the keyboard, and `/tmp/k230_keyboard_click.log` holds only
audio errors. The page can be read directly instead: there is no `/dev/fb0`,
but `/dev/dri/card0` plus the vendor's own ffmpeg support `kmsgrab`
(plane 34, 1232x568, DRM format 36314752 = `rgb565le`):

```bash
# one frame
ffmpeg -f kmsgrab -i - -frames:v 1 -vf hwdownload,format=rgb565le -y /tmp/shot.png
# a window of a whole action, cropped to the Last key panel
ffmpeg -f kmsgrab -framerate 2 -i - -t 90 \
       -vf hwdownload,format=rgb565le,crop=290:155:45:172 -y /tmp/kbfr_%04d.png
```

Then de-duplicate the frames by checksum and tile them into one image. Crop
away the counters first (`crop=290:78:0:0`): the `last=<n>ms` field changes
every frame and defeats de-duplication. `mpdecimate` is not in this ffmpeg
build. At 2 fps a state shorter than ~0.5 s can be missed — the controller
does not lose it (`ovr=0 err=0`), only the sampling does.

Each command below flips an existing `ENABLE=` line. Check first with
`cat /etc/default/pocketos-shell /etc/default/k230_phone_ui`: where a file or
the line is missing, the defaults apply (vendor launcher on, PocketOS off).

No reboot is needed: flip the flags and move the two init scripts. To hand
the panel to the vendor launcher (after setting `meshtastic.autostart=0`):

```bash
sed -i s/ENABLE=1/ENABLE=0/ /etc/default/pocketos-shell
sed -i s/ENABLE=0/ENABLE=1/ /etc/default/k230_phone_ui
/etc/init.d/S90pocketos-shell stop
/etc/init.d/S99zz_k230_phone_ui start
```

To restore PocketOS:

```bash
/etc/init.d/S99zz_k230_phone_ui stop
sed -i s/ENABLE=1/ENABLE=0/ /etc/default/k230_phone_ui
sed -i s/ENABLE=0/ENABLE=1/ /etc/default/pocketos-shell
/etc/init.d/S90pocketos-shell start
```

Only one program may drive the bit-banged bus (§1), so the launcher and a
PocketOS driver must never run at the same time.

## 7. Unknowns

- **The power path** — the whole of §3's open list. Nothing in software
  settles it; the vendor base line per configuration does.
- Whether the **remaining** keycap legends match the vendor's map: `,` on H,
  `.` on B, `<` on N, `>` on M, and every key outside §5.1. The duplicated
  `~` and `` ` `` and the missing `'` and `_` are settled (§5.1).
- Whether the two space codes are one bar or two keys, and what the keys
  named FN-R, TAB, CTRL and ALT actually say on their caps.
- What the function row, Fn, LILYGO and mic keys should do in PocketOS.
- Whether the TCA8418 should be polled or driven from the GPIO42 interrupt.
  The vendor uses the interrupt by default and polls as a fallback; polling
  is simpler and is what a first driver should do.
- Whether re-muxing io46/47 from a running PocketOS shell disturbs anything
  else. Nothing in the device tree claims those pins, so probably not — but
  "probably" is not a measurement.

### 5.6 Presence as a published signal, 2026-09-16

The driver now says whether the base is there, for automatic rotation
(`ui/shell/kbd_presence.h`, provider in `ui/shell/shell_kbd.c`). Nothing new
was measured for it: the signal is the probe this document already verified in
both directions - §0 (the controller answering once the film was gone) and
§5.3 C3 (unmated: "the controller did not answer; touch only"; remated:
"TCA8418 ready").

| Fact | Class |
| --- | --- |
| The TCA8418 answering its probe means the base is attached | **VERIFIED** (§0, §5.3 C3) |
| It not answering means the base is not attached | **VERIFIED** (§5.3 C3) |
| A bus that cannot be claimed (mux, GPIO, /dev/mem) says nothing either way; the shell publishes "unknown" and stays portrait | by construction |
| The probe runs before the display is opened, so a boot with the base attached opens landscape directly | **VERIFIED** on unit A 2026-09-16 (build `1bd7cbf`) |
| The controller held in reset from the bench (`gpioset -c gpiochip1 11=0`) makes the shell publish unknown within seconds and turn the display portrait; releasing it restores present and landscape | **VERIFIED** on unit A 2026-09-16 |
| Mating or unmating the base **while the board is powered** | **UNRESOLVED** - never done; every attach and detach on record was with the SoC halted and all supplies removed (§5.3 C3). No vendor source supports hot-plug |

Cost of watching: one probe a second while nothing is attached (a reset pulse
and one register read), and no bus traffic at all while a keyboard is polled -
the watch reads the driver's own state then. Three consecutive agreeing
readings are needed before the published state changes.

### 5.7 Boot-time presence on the panel, 2026-09-16 — VERIFIED

Four power-off boots on unit A with the product owner (build `1bd7cbf`,
deployed, card not reflashed), over the USB serial console with no network.
The base was attached and detached only with the board powered down and USB
power removed.

| Boot | Base | Stored mode | The shell's account | Result |
| --- | --- | --- | --- | --- |
| 1 | attached | automatic | `keyboard: TCA8418 ready`; `keyboard present: rotation 270, 1232x568` | landscape at start, **VERIFIED** |
| 2 | detached | automatic | `the controller did not answer; touch only`; `keyboard absent: rotation 0, 568x1232` | portrait at start, **VERIFIED** |
| 3 | attached | portrait | `TCA8418 ready`; `rotation mode portrait (stored), keyboard present: rotation 0` | portrait, held 14 min, **VERIFIED** |
| 4 | detached | landscape | `did not answer`; `rotation mode landscape (stored), keyboard absent: rotation 270` | landscape, **VERIFIED** |

The panel itself was captured on the device from the DRM plane
(`ffmpeg -f kmsgrab`, since this build has `LV_USE_SNAPSHOT 0`), carried over
the serial line as base64 and checked in pixels: in both orientations the
wordmark and the clock sit inside the 30 px corner squares and every tile is
where the grid puts it. Services 4/4 and no crash report in every boot.

## 8. The expansion connector, and hot-plug

Asked because automatic rotation would be more useful if the base could be
attached while Doors runs. **The documents do not support it**, and this
section is what they do say, so the question is not re-opened from memory.

| Fact | Source | Class |
| --- | --- | --- |
| The interconnect is `JP1`, symbol `HEADER_20X2_H` - a 2x20 header | main-board schematic (`vendor/T-Display-K230_canmv_rt/schematic/T-Display K230_V1.0_NEW.pdf`) | DOCUMENTED |
| Its power pins are `USB-IN-5V`, `GND`, `3V3`, `5V`; **pin 3 (3V3) sits in the same net as the K230's VDDIO bank supply** | schematic netlist | DOCUMENTED |
| Signals go **straight from the header to K230 balls**: no series resistor, no TVS/ESD part, no buffer or level shifter on any JP1 net (ESD parts exist, but on USB, microSD and the antenna) | schematic netlist | DOCUMENTED |
| There is **no board-detect or ID pin**; all 40 pins are ADC/GPIO/power | pin map + netlist | DOCUMENTED |
| The vendor detects a base **in software**, by probing 0x6B/0x55/0x34/0x20 and printing `Detected 6B:… 55:… 34:… XL:…` | `k230_phone_ui/src/ui_hardware.c:2990` | DOCUMENTED |
| No connector part number, pitch, gender, stack height or mating specification anywhere - so **whether ground mates first is unknown** | absence | — |
| **No schematic, PCB, BOM or layout of any base board** exists in any vendor repository | absence (also AUDIO_HARDWARE_MAP §3) | — |
| **No vendor statement about hot-plug**, permissive or prohibitive; no assembly or removal instruction of any kind | absence | — |
| The nRF52840 base is documented as feeding 5 V **back into the host** from its own cell, so current can flow either way across JP1 depending on the base | external firmware README (AUDIO_HARDWARE_MAP §4) | DOCUMENTED |

What that leaves open is an electrical question, not a software one, and two
measurements answer it. Both are made on the **detached** base, with a meter,
nothing powered:

1. **Bulk capacitance on the base's 3V3 pin to GND** (and on 5V). This is the
   inrush the main board's 3V3 rail - the same rail as the SoC's VDDIO banks -
   would have to absorb at the instant of contact. With no load switch or
   series element anywhere on JP1, a large value means a brown-out risk on the
   I/O supply; a small one (tens of nF) means the risk is negligible.
2. **Whether the 3V3 pin reaches the TCA8418's supply directly**, or through
   something: resistance from the base's 3V3 pin to the controller's VDD, and
   whether the base's own battery/boost can back-feed that pin with the base's
   switch off (resistance and diode drop from the 5V pin to the cell).

A third, mechanical, would settle the sequencing: whether the base's socket
has any longer (first-mate) ground contact. That needs the connector's part
number or a look at the physical part, not a document.

Until those exist, mate and unmate only with the board powered down and USB
power removed, as every test on record has done. I2C recovering after a
dropout is **not** evidence about this: the bus coming back says nothing about
what the supply and the SoC's I/O rail did during the event.
