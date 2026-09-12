# Physical keyboard bring-up, discovery checkpoint, 2026-09-10

**Updated 2026-09-11, then 2026-09-12.** The keyboard base board is attached
to unit A. This records what the hardware is, what has been established about
it, what has not, and why no driver has been written yet.

**Status: PRESENCE VERIFIED. SIX KEY CODES VERIFIED AGAINST THE KEYCAPS, TWO
SYMBOLS CORRECTED. THE REST OF THE MAP STILL UNREAD. POWER PATH UNRESOLVED.
NO DRIVER.** The TCA8418 answers and the keyboard types correctly in the
vendor launcher on unit A (§0). The transport is documented from vendor
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
  worked throughout. Because a battery was fitted, this still does not show
  whether the base runs from the expansion connector alone.
- **Whether the base board runs from the K230's expansion connector alone,**
  or needs its own USB-C or a battery for some or all of its parts.
- **Whether the companions answer.** **Measured 2026-09-11** in the power
  setup above, from the launcher's own probe line:
  `[keyboard-base] Detected 6B:no 55:no 34:yes XL:yes 0x20 on I2C4 SDA47/SCL46`
  — so the **TCA8418 (0x34) and the XL9555 (0x20) answer**, while the
  **BQ25896 (0x6B) and BQ27220 (0x55) do not** in this configuration.
  Whether they answer in another power setup is still open.
- **Whether the GPIO42 interrupt line works** on the connected base.
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
