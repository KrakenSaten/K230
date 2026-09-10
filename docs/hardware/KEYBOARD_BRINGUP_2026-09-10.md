# Physical keyboard bring-up, discovery checkpoint, 2026-09-10

The keyboard base board is attached to unit A. This records what the hardware
is, what was measured, what was not, and why implementation stopped where it
did.

**Status: DISCOVERY COMPLETE, PRESENCE UNVERIFIED.** The transport is fully
documented from vendor sources and confirmed against the running device tree.
Whether the keyboard is powered and answering could not be established
tonight: probing it needs a program on the board, and copying one there was
refused by the sandbox. No mapping has been invented, and no driver has been
written. What exists is the translation layer, which is testable without the
hardware and is tested.

## 1. What the keyboard is

| Fact | Value | Class |
| --- | --- | --- |
| Controller | TI **TCA8418** keypad matrix controller | DOCUMENTED |
| Board | nRF9151 cellular / GNSS / keyboard base board | DOCUMENTED |
| Matrix | 7 rows × 10 columns, key codes 1–70 | DOCUMENTED |
| I2C address | **0x34** (7-bit) | DOCUMENTED |
| Bus | **bit-banged** on GPIO46 (SCL) / GPIO47 (SDA) | DOCUMENTED |
| Reset | **GPIO43**, active low | DOCUMENTED |
| IRQ | **GPIO42** | DOCUMENTED |
| Backlight | **GPIO52 / PWM4** | DOCUMENTED |
| Event format | one byte: bit 7 press/release, bits 0–6 the key code | DOCUMENTED |
| Registers | CFG 0x01, INT_STAT 0x02, KEY_LCK_EC 0x03, KEY_EVENT_A 0x04, KP_GPIO 0x1D–0x1F, DEBOUNCE_DIS 0x29–0x2B | DOCUMENTED |
| Companions on the same bus | BQ25896 charger 0x6B, BQ27220 gauge 0x55, XL9555 expander 0x20–0x27 | DOCUMENTED |

Sources: `vendor/T-Display-K230/k230_bsp/docs/HARDWARE_PINMAP.md` and the
vendor launcher's own driver,
`vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/ui_hardware.c`.

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
running board: io46 and io47 currently read `0x800019D1`, which is exactly the
vendor's own restore value (function select 3), and the vendor's bit-bang
value is `0x800001D1` (function select 0).

Consequences for PocketOS: a physical-keyboard driver needs `/dev/mem`, must
own the mux while it holds the bus, and cannot use `i2cdetect`, `i2cget` or
any `/dev/i2c-*` path. None of the standard I2C tooling on the card can see
this keyboard, which is why the scans below prove nothing about it.

## 2. What was measured on unit A

Runtime `0.0.8` build `0b16f0e`. All read-only except where noted.

| Measurement | Result | Class |
| --- | --- | --- |
| `/proc/bus/input/devices` | only the PMU power key and `goodix_ts`. **No keyboard input device** | VERIFIED |
| USB | only the Realtek Ethernet and two root hubs; `usbhid` loaded but bound to nothing | VERIFIED |
| `/dev/input` | `event0`, `event1`, `mouse0`, `mice` — nothing new | VERIFIED |
| Hardware I2C scan, `i2c-0` | only 0x37 (camera) | VERIFIED |
| Hardware I2C scan, `i2c-1` | 0x3b (unidentified) and 0x5d (touch, driver bound) | VERIFIED |
| Device tree | `keyboard_irq_gpio42_pins` and `keyboard_backlight_pwm4_pins` exist, and **nothing references either**. There is no keyboard node | VERIFIED |
| io46 / io47 mux | `0x800019D1` — function 3, not GPIO | VERIFIED |
| GPIO42 (IRQ) | reads low under every bias setting | VERIFIED |
| dmesg | no keypad, matrix, TCA or HID device probe of any kind | VERIFIED |

One test was attempted and is reported as **inconclusive rather than
evidence**: reading GPIO46/47 against an internal pull-down, to see whether
the base board's I2C pull-ups hold them high. Both read high — but so did a
control pin with nothing attached (GPIO44), so `gpioget --bias` is not
overriding the pad configuration on this SoC and the reading discriminates
nothing. It is recorded here so nobody repeats it expecting an answer.

## 3. Power: the questions asked, and what can honestly be said

The instruction was not to assume the keyboard's own USB-C is required, nor
that power passes through the expansion connector. Neither assumption has been
made, and **neither question has been answered.**

What can be said: the keyboard has never appeared as an input device, on any
bus, in any log, with no battery fitted and no separate USB-C connected. That
is *consistent* with the base board being unpowered, but it is equally
consistent with a powered board that simply has no driver — because nothing in
this build ever talks to GPIO46/47, and the kernel has no TCA8418 driver
bound. **The two cannot be told apart without probing the bit-banged bus.**

The discriminating measurement is the BQ25896 charger at 0x6B and the BQ27220
gauge at 0x55. Neither has a reset line from the K230, so if the base board
has power, both answer. That probe is section 6.

## 4. Why implementation stopped where it did

A probe was written (bit-banged I2C over GPIO46/47, the vendor's exact
sequence, with the mux saved and restored) and cross-compiled for riscv64.
Copying it to the board was refused by the sandbox, and that refusal was not
worked around. The board has `devmem`, `gpioset` and `gpioget`, but a shell
cannot bit-bang I2C: `gpioset` releases its lines when it exits, and reading
the ACK needs the same lines the driving process holds.

So the honest position is the one the brief asked for: stop at a documented
checkpoint, do not invent a mapping, and leave a diagnostic ready.

## 5. What was built

**`ui/pocketui/pos_keymap.[ch]` — the translation layer, and nothing else.**

It turns a raw TCA8418 FIFO byte into the one logical vocabulary of DS §17.4:
a Unicode code point for a printable character, an `LV_KEY_*` constant for
anything else. It is pure — no I/O, no LVGL objects, no hardware — which is
what makes it testable tonight with no keyboard attached.

`tests/pos_keymap_test.c` covers it in 78 checks: every letter, every digit,
Shift, Caps, the two together, the keycap symbols, space, enter, backspace,
escape, tab, all four arrows, the modifiers, releases delivering nothing,
one press giving exactly one key, unknown and out-of-range codes ignored
without touching the modifier state, and the function row reported as reserved
rather than guessed at.

**The layout is BlackBerry-style, and this is the thing most worth checking
against the real keycaps.** Shift reaches the *symbol printed on the key*, not
a capital: Shift+A is `~`, Shift+K is `/`, Shift+L is `?`. Capitals come from
**Caps**. Only Z, X, C and V carry no symbol, so only those four give a capital
under Shift. That is the vendor's map reproduced faithfully; it has not been
read off the hardware.

Two symbols appear on two keys each in the vendor map — `~` on both W and A,
`` ` `` on both Q and J. Reproduced as-is rather than "corrected", because a
guess about which is wrong would be worse than the duplication.

The function row (F1–F11), the Fn keys, the LILYGO key and the mic key are
deliberately **not mapped**. They are reported as reserved. The vendor binds
them to its own launcher's hotkeys, which is not a contract PocketOS has, and
the brief said not to over-design them.

### Architecture, once the hardware is confirmed

```
TCA8418 (bit-banged I2C on GPIO46/47)
        |
        v
 transport + poll        <- not written yet: needs hardware evidence first
        |
        v
 pos_keymap_event()      <- WRITTEN AND TESTED (78 checks)
        |
        v
 pos_input_push_key()    <- unchanged, from M3
        |
        v
 queue -> LVGL keypad indev -> focus group -> focused field
```

Nothing above `pos_input_push_key()` changes, which is the point of §17.4:
Notes cannot tell a typed character from a tapped one, and no second text
path is created. The touch keyboard, the SDL keyboard and Notes are untouched
by this commit.

## 6. What the operator can do tomorrow

**No new code is needed for the decisive test.** The vendor launcher is
already installed at `/root/app/k230_phone_ui/k230_phone_ui` and contains
exactly the probe that could not be run tonight: `keyboard_base_probe()`
checks 0x6B, 0x55, 0x34 and the XL9555 range over the bit-banged bus and
reports a line of the form

```
Detected 6B:yes 55:yes 34:yes XL:yes 0x20
```

or `Keyboard base not detected`.

Step 1 — hand the panel to the vendor launcher and read that line:

```bash
ssh root@192.168.10.157 'sed -i s/ENABLE=1/ENABLE=0/ /etc/default/pocketos-shell;
                         sed -i s/ENABLE=0/ENABLE=1/ /etc/default/k230_phone_ui; reboot'
```

Then on the panel: **Settings → the keyboard section**, and read the
keyboard-base status line. That single string answers, in one go, whether the
board is powered and whether the TCA8418 is present.

Step 2 — repeat it with the keyboard's own USB-C connected, and again with a
battery if one is available. The difference between those readings is the
answer to "does it need its own power", which nothing in software can settle.

Step 3 — if the TCA8418 answers, press these keys with the vendor launcher's
key grid open, and note what it shows for each:

- **A**, then **Shift+A** — this is the BlackBerry-layout question. Expected
  `a` and `~`.
- **Z**, then **Shift+Z** — expected `z` and `Z`.
- **Q** and **Shift+Q** — expected `q` and `` ` ``.
- **Caps**, then **A** — expected `A`.
- **Space**, **Enter**, **Del**, **Tab**, **Esc**, and all four **arrows**.
- One **F-key** and the **LILYGO** key, to confirm they are distinct codes.

Put the panel back afterwards by reversing step 1.

To restore PocketOS:

```bash
ssh root@192.168.10.157 'sed -i s/ENABLE=1/ENABLE=0/ /etc/default/k230_phone_ui;
                         sed -i s/ENABLE=0/ENABLE=1/ /etc/default/pocketos-shell; reboot'
```

## 7. Unknowns

- Whether the base board is attached, powered and answering. **The whole of
  section 3.**
- Whether its USB-C or a battery is required.
- Whether the keycap legends match the vendor's symbol map, including the two
  duplicated symbols.
- What the function row, Fn, LILYGO and mic keys should do in PocketOS.
- Whether the TCA8418 should be polled or driven from the GPIO42 interrupt.
  The vendor supports both; polling is simpler and is what a first driver
  should do.
- Whether re-muxing io46/47 from a running PocketOS shell disturbs anything
  else. Nothing in the device tree claims those pins, so probably not — but
  "probably" is not a measurement.
