# PocketCalculator

A simple calculator: the four operations with ordinary precedence, a decimal
point, +/−, backspace and clear. It computes and forgets - nothing is stored,
nothing is sent anywhere, and there is no clock in it.

Status: **in development on `feature/pocketcalculator`. Host-tested,
including taps under a real LVGL pointer device and keys through the real
logical key stream; not yet run on hardware.**

## What it is

One screen.

- **A display panel** with two lines, right-aligned: the expression being
  built above (`12 + 3 ×`), and the entry or the result below it, large.
- **A keypad** of nineteen keys, four columns by five rows:

  ```
  C    ⌫    ±    ÷
  7    8    9    ×
  4    5    6    −
  1    2    3    +
  0 (two wide)  .    =
  ```

That is the whole application. It is fully usable by touch alone and by
keyboard alone, and the two can be mixed within one calculation.

## What it deliberately is not

Not scientific. There is **no percent** (omitted by the product owner), no
memory keys, no parentheses, no square root or other functions, no history
or tape, no copy and paste, no unit or currency conversion, no repeated-equals
constant operation, and no settings. `tests/calculator_lint.sh` fails the
build if a percent action, a store, a clock, file or network I/O, a keyboard,
an animation or a timer appears under `apps/calculator/`.

## The rules

The model is **B: normal operator precedence**, decided by the lead engineer.
Every rule below is pinned by a test in `tests/calc_engine_test.c` unless
stated otherwise.

### Precedence

- × and ÷ bind tighter than + and −. Operators of equal precedence evaluate
  left to right: `2 + 3 × 4 = 14`, `10 − 4 ÷ 2 = 8`, `2 × 3 + 4 × 5 = 26`,
  `8 ÷ 4 ÷ 2 = 1`, `7 − 2 − 1 = 4`.
- Nothing is evaluated before `=`. The expression is kept whole and shown on
  the upper line, and `=` evaluates it in one pass.

### Operators

- An operator commits the current entry as an operand and appears on the
  expression line after it.
- **An operator straight after another operator replaces it**; there is never
  an empty operand. `5 + × 3 =` is `5 × 3 = 15`.
- **An operator pressed first, with nothing typed, uses 0** as the first
  operand: `− 5 =` is `0 − 5 = −5`.

### The entry

- **At most 12 digits.** A zero standing alone in front of the point (`0.5`)
  is not counted; every other digit is, zeros after the point included,
  because each takes a column of the display. Further digits are ignored.
  The longest entry is `-0.123456789012`, 15 characters.
- **Leading zeros collapse**: `0` then `0` stays `0`; `0` then `5` becomes `5`.
- **`.` on an empty entry gives `0.`**, and a second `.` in the same number
  is ignored.
- **Backspace** removes the last character. Removing the last digit returns
  to the empty entry, shown as `0` - which is where the user was before the
  first digit, so an operator pressed next replaces the previous operator as
  it would have then (`5 + 3 ⌫ × 2 =` is `5 × 2`). A minus sign left in
  front of nothing but zeros is dropped (`-0.05 ⌫` is `0.0`).
- **Backspace on an empty entry does nothing**; in particular it never
  deletes the operator before it.
- **+/−** toggles the sign of the entry. On the empty entry, on `0`, or on a
  number that is all zeros (`0.00`) it does nothing: there is no `-0`. To
  enter a negative number after an operator, type it and then press +/−
  (`3 × 2 ± =` is `3 × -2 = -6`).
- **Clear** resets everything. There is no separate clear-entry.

### Equals

- `=` evaluates the whole expression and shows the result; the expression line
  then shows the evaluated expression followed by ` =`.
- **An operator with nothing after it is dropped**: `5 + =` shows `5`, with
  `5 =` above it.
- **`=` with nothing typed at all does nothing.**
- **Repeated `=` is not supported**: a second `=` straight after a result
  changes nothing at all, not a byte of the engine's state (tested with
  `memcmp`).

### After a result

- **A digit or `.` starts a new calculation**; the expression line clears.
- **An operator continues with the result as the first operand**
  (`2 + 3 = × 4 =` is `5 × 4 = 20`). The result is carried at full double
  precision and shown on the expression line rounded, so `1 ÷ 3 = × 3 =` is
  `1`, with `0.333333333333 × 3 =` above it.
- **Backspace does nothing to a result.** (The alternative, clearing it, was
  rejected: a result is not typing, and one stray backspace should not lose
  it.)
- **+/− negates the result in place**, and the expression line clears,
  because the expression above no longer produced the number below it. On a
  zero result it does nothing.

### Arithmetic and what is shown

Numbers are IEEE 754 doubles. What reaches the display is decimal, by one
rule, in `calc_format_number()`:

- **Rounded to 12 significant digits, trailing zeros removed, never `-0`.**
  `0.1 + 0.2` shows `0.3`, `1 ÷ 3` shows `0.333333333333`, `2 ÷ 3` shows
  `0.666666666667`, `2.5 × 2` shows `5`.
- **Plain notation** while the rounded value is below 10^12 and its plain
  form, without a sign, is at most 15 characters: `123456789012`,
  `0.0000000000001`, `0.0333333333333`. The sign is left out of that test so
  a number and its negation are always written in the same notation.
- **Scientific notation** otherwise, as compactly as possible: mantissa, `e`,
  exponent, with no `+`, no leading zeros in the exponent, and no trailing
  zeros in the mantissa - `1e12`, `9.99999999998e23`, `3.33333333333e-3`,
  `1e-15`. The whole string is at most **16 characters including the sign**,
  so a negative mantissa gives up a digit and is rounded again to what fits:
  `-1.2345678901e14`, `-3.3333333333e-3`.
- **Cancellation.** `0.1 + 0.2` is 0.30000000000000004 in binary. Rounded for
  the display that is `0.3` - but `0.1 + 0.2 − 0.3` would leave 5.5e-17 and
  show it faithfully as noise. So every + and − is rounded to 15 significant
  digits at the scale of its larger operand, which is below anything the
  display shows and above the error a double carries; what is removed are
  exactly the digits a double never had. `0.1 + 0.2 − 0.3`, `0.3 − 0.1 − 0.2`
  and `1.1 × 1.1 − 1.21` are all `0`; `1 + 0.000000000001 − 1` is
  `0.000000000001`.
- **No locale.** Entries are read as an exact integer scaled by an exact power
  of ten, and results are decomposed from `%e` reading only the digits and the
  exponent. Neither `strtod` nor `%f`/`%g` is used (the lint checks), so a C
  library with a decimal comma cannot change the display.

### Errors

| State | Main line | Expression line |
| --- | --- | --- |
| Division by zero | `Can't divide by 0` | the expression, with ` =` |
| Overflow | `Overflow` | the expression, with ` =` |

- **Division by zero** is looked for before anything is computed, so it is
  reported even when an earlier part of the same expression would have
  overflowed: it is the mistake the user can see and fix.
- **Overflow** is any non-finite value on the way, or a result that rounds to
  1e100 or more. One edge follows from the width rule: `9.99999999999e99`
  fits in 16 characters, but its negation has room for 11 digits, rounds to
  1e100, and so +/− on it is an overflow.
- **Underflow**: a result that rounds below 1e-99 is shown as `0` and carried
  on as exactly 0, never as an invisible remainder.
- **In the error state every key leaves the error.** A digit or `.` then
  starts a new entry; an operator, `=`, +/− and backspace do not take effect,
  and the calculator is simply back at `0` (an operator is not applied to a 0
  the user never typed). Clear and Esc clear, as always.
- The message is drawn in the same hero role and colour as a number. Its words
  carry the meaning (DS §2), and Night mode's `status_error` is below the
  contrast the main readout needs (DS §13).

### Limits

- **32 operands** in one expression. An operator that would start a 33rd is
  ignored; the 32nd operand can still be typed and evaluated, and an operator
  straight after the 31st can still be replaced.
- The expression line's text is at most 32 numbers, 31 operators and ` =`
  (`CALC_EXPRESSION_MAX`, 704 bytes). When it is wider than the panel it is
  **cut from the left with a leading ellipsis**, at a space so it never begins
  with half a number (`calc_view_fit_left`), and it is fitted again when a
  theme or mode change changes its font.

## Keyboard

Through the one logical key stream (DS §17.4): every key the stream delivers
goes through `calc_view_action_for_key()` into the same `calc_apply()` a tap
reaches. Nothing in the app asks which source a key came from.

| Key | Action |
| --- | --- |
| `0`–`9` | digit |
| `.` or `,` | decimal point |
| `+` | add |
| `-` | subtract |
| `*`, `x` or `X` | multiply |
| `/` | divide |
| `=` or Enter (`LV_KEY_ENTER`) | equals |
| Backspace (`LV_KEY_BACKSPACE`) | backspace |
| Esc (`LV_KEY_ESC`), `c` or `C` | clear |
| **`n` or `N`** | **+/− (negate)** |

`n` was chosen for +/− because it names the operation (negate), sits on every
layout, and is not an arithmetic character that could be typed meaning
something else. Every other key is ignored silently - letters, the space, `%`,
the arrows, Home/End, Delete, and the typographic `×`, `÷` and `−` themselves.
Next and Prev (`LV_KEY_NEXT`, which the physical keyboard's Tab sends, and
`LV_KEY_PREV`) leave the focus where it is, because the display is the only
focusable object.

The view names LVGL's Enter, Esc and Backspace by number (10, 27, 8) so that
the whole map is tested without LVGL; `calc_app.c` asserts at compile time that
they still equal `LV_KEY_ENTER`, `LV_KEY_ESC` and `LV_KEY_BACKSPACE`.

**Focus.** The display panel is the single object in the shell's focus group
and is focused for the app's whole life. The keypad's buttons have
`LV_OBJ_FLAG_CLICK_FOCUSABLE` cleared, so a tap never moves focus off the
display and typing after tapping still arrives. No focus ring is drawn on the
display: a ring that is always on says nothing. No touch keyboard is shown or
requested - the keypad is the input.

Backspace on the keypad does not auto-repeat when held. A physical keyboard's
repeat, which belongs to its driver, arrives as repeated keys and works.

## Architecture

Three files, split so that everything that decides anything is pure and
tested, and what is left is a screen.

| File | What it is |
| --- | --- |
| `apps/calculator/calc_engine.c/.h` | The state machine (entry, result, error), precedence evaluation, the cancellation rounding and `calc_format_number()`. No LVGL, no I/O, no clock. |
| `apps/calculator/calc_view.c/.h` | Key code → action, the two display strings, the operator glyphs, the keypad table and its labels, and fitting a long line from the left. No LVGL. |
| `apps/calculator/calc_app.c` | LVGL: the panel, the nineteen keys, the key sink. `.tick` is `NULL`; `destroy` frees the private state. |

The launcher entry is `app_calculator` (`id` `calculator`, name
`Calculator`), registered in `ui/shell/shell.c` straight after Calendar. The
launcher grid gained a fifth row for it (below).

**Icon.** `LV_SYMBOL_PLUS`, a placeholder until the DS §11 icon set exists:
LVGL's symbol font has no calculator, and the plus sign is the nearest
arithmetic glyph in it. `LV_SYMBOL_KEYBOARD` was the other candidate and was
passed over because PocketOS has both a touch and a physical keyboard, and a
keyboard on the launcher would read as either.

## Look

- **Role styles only** (`tests/style_lint.sh`). The display is a
  `pocketui_card` panel. The expression line is `POS_STYLE_TEXT_SECONDARY`
  (body type, so 20 px in Outdoor); the number is `POS_STYLE_HERO_48`.
- **Keys.** Digits and the point are slabs (`POS_STYLE_SLAB`). The four
  operators are slabs with the glyph in `POS_STYLE_ACCENT_TEXT`, like the back
  chevron. C, ⌫ and ± edit rather than enter, and are secondary buttons
  (`POS_STYLE_BUTTON_SECONDARY`, a hairline apart). **`=` is the primary
  button** (`POS_STYLE_BUTTON_PRIMARY`), the one accent fill on the screen,
  which DS §1 allows. Pressed is `POS_STYLE_SLAB_PRESSED` or
  `POS_STYLE_BUTTON_PRIMARY_PRESSED`: the 2 px focus outline plus the fill
  change of DS §9, applied instantly. The keypad lets that outline draw past
  its own edge, so a pressed key in an outer column or the bottom row keeps
  all four sides of it.
- **Type on the keys.** DS §3 has no keycap size. Row-title (20 px) suits the
  52 px touch-keyboard keys and would be lost on a 126 × 128 key, so the keys
  use hero-40; ⌫ is `LV_SYMBOL_BACKSPACE` in the 32 px symbol role. The type
  role is added to the key before the fill role, so the fill role's text
  colour (`text_on_accent` on `=`) wins and the font stays.
- **Glyphs.** × (U+00D7), ÷ (U+00F7), − (U+2212), ± (U+00B1) and … (U+2026)
  are real glyphs: the product fonts carry them (`tools/design/gen_fonts.sh`
  converts Latin-1 and U+2013–U+2212). A negative number keeps the ASCII
  hyphen-minus, which is visibly shorter than the subtraction sign, so
  `5 − -3` reads unambiguously.
- **Motion: none.** A key press is a role style while the finger is down and a
  relabelled display when it lifts. There is nothing for the reduced-motion
  setting to switch off (DS §12); the lint fails on `lv_anim` or a timer, and
  `calc_app_test` checks that no animation is running during and after a
  press.

## Layout

The shell gives the app 528 × 1060 px (568 − 2 × 20; 1232 − 56 status bar −
72 header − 24 top padding − 20 bottom padding), a column with a 20 px gap.

| Block | Arithmetic | Size |
| --- | --- | --- |
| keypad columns | 4 × 126 + 3 × 8 | 528 wide; `0` is 2 × 126 + 8 = 260 |
| keypad rows | 5 × 128 + 4 × 8 | 672 tall, at the foot of the body |
| gap | | 20 |
| display panel | 1060 − 672 − 20 | 368 tall |
| panel content | 528 − 2 × 20 − 2 × hairline | 486 wide (484 Outdoor) |
| expression line | body line height | 21 (26 Outdoor) |
| gap | | 8 |
| number | hero-48 line height | 60 |

Every key is 126 × 128 or larger, twice the 64 px minimum of DS §7 in height.
The longest number, 16 characters of hero-48 at 28.8 px a digit with −1 px
tracking, is at most 445 px wide inside the panel's 484 px; `Can't divide by 0`
is about 352 px. Nothing scrolls, in Normal, Outdoor or Night.
`calc_app_test` checks all of that on the laid-out objects in all three
modes, with the widest strings the main line can be given.

**Launcher.** Nine apps in two columns of 150 px tiles take five rows:
5 × 150 + 4 × 20 = 830 px plus the 20 px padding, inside the 1176 px below the
status bar. The grid template in `home_create()` went from four
`LV_GRID_CONTENT` rows to five.

## Storage

**None.** A calculation is not worth keeping: opening the app lands on `0`
every time. There is no store to corrupt and no migration to write.
`calc_app_test` points `POCKETOS_STATE_DIR`, `POCKETOS_CONFIG_DIR` and
`POCKETOS_RUNTIME_DIR` at empty directories and checks they are still empty at
the end; `calculator_shell_test.sh` does the same for the real shell. The app
calls no shell service at all - no clock, no radio, no status hint, no
keyboard - which `calc_app_test` checks by defining each `app.h` entry point as
a counter.

## Tests

| Test | What it covers |
| --- | --- |
| `tests/calc_engine_test` (288 checks) | Add, subtract, multiply, divide; precedence (`2+3×4`, `10−4÷2`, `2×3+4×5`, `8÷4÷2`, `7−2−1` and more); decimal input and the rejected second point; leading zeros; negative numbers with +/− and an operator first using 0; chaining after a result, including full-precision carry; operator replacement; clear; backspace down to `0` and never across an operator; division by zero, its expression line and every way out of it, and its precedence over overflow; the 12-digit cap and `999999999999 × 999999999999`; formatting (`0.1+0.2`, `1/3`, `2/3`, trailing zeros, no `-0`, plain/scientific boundaries, the sign costing a digit); cancellation to exact 0; overflow from products, from infinity and from a carried result, and underflow to an exact 0; the 32-operand bound and the longest expression; repeated `=` as a byte-for-byte no-op; a fuzz run of 400 000 keys from a fixed seed (a uniform mix, one weighted to huge products, one to tiny quotients) checking every main line is a well-formed number of at most 16 characters or one of the two messages, every operand is well-formed, no `nan`/`inf`/`-0` ever appears, and that divide-by-zero, overflow, scientific, negative and small results were all actually reached; and 200 000 random doubles through the formatter, each checked for form, class and that it reads back as the value it rounds |
| `tests/calc_view_test` (197 checks) | Each of the 26 mapped keys to its action, and every one of the 19 actions reachable; every other 7-bit key, `%`, the arrows, Tab, Delete, `×` `÷` `−` as code points and out-of-range keys ignored; the two display strings through the key map, including Enter, Backspace and Esc by value; the messages; buffers too small cut rather than overrun; the operator glyphs; the keypad table (19 keys, every cell filled once, every action once, `=` bottom right, `0` two wide, operators in the right column) and the key labels matching the expression's glyphs; fitting from the left (fits untouched, cut at a space, a single long token, multi-byte glyphs never split, no room at all, empty and degenerate calls, the longest real expression) |
| `tests/calculator_lint.sh` (50 checks) | No LVGL in the engine or view; no file I/O, console output, clock, network, IPC, environment, processes or threads anywhere in the app; no store; no keyboard created, included or asked for; exactly one focus-group object; keys received as `LV_EVENT_KEY` and mapped only by the view; no branching on key source; keypad keys not click-focusable; the view's key numbers asserted against LVGL's; no `%f`/`%g`, no `strtod`/`setlocale`; no percent or scientific functions and exactly 19 actions; no animation, no timer, no tick; `=` the only primary button; registered once, after Calendar, built by CMake with its include path, tests run by `make test` |
| `calc_app_test` (171 checks) | The app under a real LVGL pointer device and the real key stream: it opens on `0` with the display focused; taps on the keypad by label give the right results and expression lines; focus stays on the display through every tap; the pressed state shows under a finger with no animation running; every mapped key pushed through `pos_input` does what the map says, unmapped keys change nothing, Next/Prev keep the focus; tapping and typing interleave into one calculation; overflow and a long expression are ellipsised and drawn whole; in Normal, Outdoor and Night every key is at least 64 × 64, wholly on screen, not overlapping another, with its face inside it, nothing scrolls, and the widest main-line strings are drawn whole; the long expression is refitted after each mode change; five open/close rounds leave nothing behind and a fresh open remembers nothing; no keyboard was requested, no shell service called, nothing written |
| `tests/calculator_shell_test.sh` (19 checks) | Runs `calc_app_test`, then the wiring: declared and registered after Calendar, a fifth launcher row, all three sources and the include path in CMake, the app test a host-only target, no keyboard, no store, no tick; then the real shell opens and closes `calculator` headless, writes its log, logs no fault in the log or the output, logs the app open and closed, and writes nothing to the state directory |

The first three run in `make test`. The last two need a display and the
CMake-built shell:

```sh
SHELL_BIN=~/work/pocketos-build/shell/pocketos-shell bash tests/calculator_shell_test.sh
```

## Hardware

**Not yet run on a board.** No physical test has been done. The tap path and
the key path are not untested, though: `calc_app_test` drives both through
real LVGL devices, so a board is the first test of the *panel*, not of
tapping or typing at all.

What only hardware can settle:

- Whether 126 × 128 keys with hero-40 faces are comfortable and accurate for a
  thumb, and whether the operator glyphs, which sit at x-height, read at arm's
  length.
- The body-16 expression line's legibility on the real panel in Normal and
  Night, and the hero-48 readout in Outdoor sunlight.
- Accent-on-slab operators and the accent `=` in Outdoor and Night.
- The physical keyboard's map for `*`, `/`, `=` and `n`, which is the
  keyboard driver's concern, not the calculator's: whatever code point it
  pushes is what the calculator sees.
