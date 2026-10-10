# Physical keyboard driver: design and preflight, 2026-09-12

**Status: IMPLEMENTED AND BROUGHT UP.** Approved by the product owner on
2026-09-12, implemented in commit `d3b7bb0`, deployed to unit A the same day,
and bring-up gate A passed (`KEYBOARD_BRINGUP_2026-09-10.md` §5.2). The
document was written before any code existed and is kept as written; where
the hardware has since answered a question it left open, the answer is marked
in place.

Scope: the minimal production driver that turns TCA8418 key events into the
one logical key stream of DS v0.1 §17.4, on unit A, for v0.0.9. It covers
the module layout, ownership, threading, the bus, the chip sequence, the
drain algorithm, recovery, tests, the hardware smoke and the risks.

What it deliberately does not cover: the F-row, Fn, the LILYGO key and the
mic key (their semantics are undecided), auto-repeat, and the DS §18.8 alert
work, which is a shipping gate recorded in §13 rather than a driver task.

Evidence classes follow AGENTS.md: **VERIFIED** (measured on unit A),
**DOCUMENTED** (read from vendor source), **ASSUMED** (stated, unmeasured).
Vendor citations are `vendor/T-Display-K230/k230_launcher/k230_phone_ui/
src/ui_hardware.c` unless another file is named.

## 0. Decisions taken before implementation

### 0.1 Deviation from ADR-002: the driver runs in the shell

ADR-002 fixed point 2 gives hardware to services exclusively, and
ARCHITECTURE.md says apps and the shell never open device nodes. **This
milestone deviates from that, with the product owner's approval
(2026-09-12).** The physical keyboard driver runs in-process in the PocketOS
shell for v0.0.9.

Reasons, recorded so the deviation can be re-examined rather than inherited:

- The physical keyboard is **panel-class input**, the same kind of device as
  the touch panel and the display, not a peripheral like the SX1262.
- `ui/shell/platform_*.c` is the **existing precedent and the sanctioned
  carve-out**: ARCHITECTURE.md places K230 specifics there, and
  `platform_drm.c` already opens `/dev/dri/card0` and evdev inside the shell.
- **`pos_input` is single-threaded and lives in the shell.** DS §17.4
  requires every source to push into it; `pos_input_push_key()` takes no
  lock. A driver in another process would need both a new daemon and a new
  IPC contract to deliver what the touch keyboard already delivers
  in-process.
- ADR-002 reserves the name `input.*`, but **no `docs/api/input.md` exists**,
  so there is no contract to conform to and none is invented here.

**No `inputd` and no new IPC contract in this milestone.**

### 0.2 The shared bus must be revisited later

The bit-banged bus on GPIO46/47 does not belong to the keyboard. The same
two lines reach the **XL9555 expander (0x20), the BQ25896 charger (0x6B) and
the BQ27220 gauge (0x55)** (DOCUMENTED; on unit A in the session power setup
0x34 and 0x20 answer and 0x6B and 0x55 do not — VERIFIED,
KEYBOARD_BRINGUP_2026-09-10.md §3).

**If charger, battery or XL9555 support is ever added, bus ownership must be
re-decided before that code is written.** Two processes must never bit-bang
these lines. The layout in §1 keeps the bus module free of both LVGL and
keyboard specifics precisely so it can move behind a service later without
touching the key map or the shell glue.

**Decided 2026-10-10 (product owner), for diagnostics only:** the read-only
battery probe runs inside this same owner - the shell, on the LVGL thread,
behind the same claim and release - so the lines still have one owner. It
reads the BQ27220 and the BQ25896 status register and writes nothing. Where
battery status is eventually published from is not decided by this
(BATTERY_PROBE.md).

### 0.3 DS §18.8 is a shipping gate, not an implementation blocker

The driver prototype and its hardware validation proceed now. **The physical
keyboard is not shippable or release-complete until DS §18.8 is resolved**
(§13.1). §18.8 is not touched during driver implementation.

### 0.4 Polling model

Approved: a **15 ms** timer that reads the **GPIO42 level first** and
performs I2C **only when INT is asserted**, with a runtime fallback to
unconditional polling at **20 ms** if GPIO42 proves unusable. **No IRQ
thread. No auto-repeat in driver v1.** The reasoning and the cost model are
in §14.

## 1. File and module layout

| File | Contents | LVGL | Built by |
| --- | --- | --- | --- |
| `ui/shell/kbd_bus.h` | The `struct kbd_bus` vtable and its contract | no | both |
| `ui/shell/kbd_bus_k230.c` | The only file naming `/dev/mem`, `gpiod_*`, io46/47/42/43 | no | CMake, `drm` only |
| `ui/shell/kbd_bus_absent.c` | Stub bus reporting "no device" | no | CMake, `sdl` |
| `ui/shell/kbd_tca8418.[ch]` | Chip state machine: init, presence, drain, overflow, retry | no | root `Makefile` and CMake |
| `ui/shell/shell_kbd.[ch]` | Shell glue: the timer, keymap call, push, logging | yes | CMake |
| `tests/kbd_tca8418_test.c` | Fake bus; the chip logic in full | no | root `Makefile`, in `make test` |
| `tests/kbd_lint.sh` | Boundary and ownership guards | — | `make test` |
| `tests/shell_kbd_test.c` | Fake chip through real `pos_input` | yes | CMake, `sdl` |
| `tests/kbd_shell_test.sh` | Runs that test, and the shell's no-keyboard path | — | with `SHELL_BIN` |

This split follows the convention already in the tree: LVGL-free logic is
built by the root `Makefile` and runs in `make test` in every configuration
(as `ui/pocketui/pos_theme.o` and `ui/shell/settings.o` already do), while
LVGL-dependent code is built only by CMake, whose test targets exist solely
under `POCKETOS_DISPLAY=sdl`.

The consequence is deliberate: **the chip logic is tested on every build;
the glue is tested only under SDL.** Everything that can be decided without
LVGL is therefore put in the chip layer rather than the glue.

## 2. Ownership and lifecycle

- **Created** in `main()` immediately after `pos_input_init()` — the stream
  must exist before a source can push into it — and before any app opens.
- **Presence is a return value, not an error.** Absent means no timer is
  created and the shell runs on touch alone.
- **Destroyed** on the exit path, after the main loop breaks. That path
  currently runs `app_close()`, `pocketipc_server_free()`,
  `shell_ipc_shutdown()` and `pocketlog_close()`, and does not deinitialise
  `pos_input`. **`shell_kbd_destroy()` must be added there**, because it is
  what restores the iomux (§8).
- **Exclusivity** is already guaranteed above this code: `S90pocketos-shell`
  refuses to start while the vendor launcher is enabled or running
  (platforms/k230/README.md), and only one program may drive the bit-banged
  bus (VERIFIED). As a second line of defence the driver refuses to claim
  the bus when a `k230_phone_ui` process exists, and logs why.

## 3. Threading model

**One `lv_timer` on the LVGL thread. No thread, no daemon.**

This is forced rather than preferred:

- `pos_input_push_key()` writes the queue with **no lock**
  (`ui/pocketui/pos_input.c`), so a second thread would have to introduce
  locking into the input path.
- The repository contains **no threads at all** in first-party code. The
  conventions are a single-threaded event loop, `volatile sig_atomic_t` stop
  flags, and kernel-mediated exclusion (`flock`, exclusive gpiod requests).
- The vendor runs an IRQ thread only because its UI thread is elsewhere
  (DOCUMENTED). Copying it would import a locking problem PocketOS does not
  have.

The price is a bounded blocking transaction on the UI thread. §14 quantifies
it, and the level-gate exists to keep it near zero when nothing is pressed.

## 4. Bus abstraction

```c
struct kbd_bus {
    int  (*claim)(void *ctx);                       /* mux to GPIO */
    void (*release)(void *ctx);                     /* mux to saved value */
    int  (*read_reg)(void *ctx, uint8_t r, uint8_t *v);
    int  (*write_reg)(void *ctx, uint8_t r, uint8_t v);
    int  (*reset_pulse)(void *ctx);                 /* GPIO43 */
    int  (*irq_level)(void *ctx);                   /* 0 low, 1 high, -1 n/a */
    void *ctx;
};
```

Transport facts (DOCUMENTED unless marked):

- `/dev/gpiochip1`, SCL = offset 14 (GPIO46), SDA = offset 15 (GPIO47),
  IRQ = offset 10 (GPIO42), reset = offset 11 (GPIO43); the offset is
  `pin - 32`.
- Both lines in **one** libgpiod v2 request, `GPIOD_LINE_DRIVE_OPEN_DRAIN`
  plus `GPIOD_LINE_BIAS_PULL_UP`, both starting high. Open drain with a
  pull-up means SDA is read without changing direction.
- 8 us after every SCL and SDA transition; about 50-60 kHz.
- `read_reg` is S, addr<<1, reg, Sr, addr<<1|1, one byte NACK, P.
  `write_reg` is S, addr<<1, reg, value, P. Address 0x34.

Three deliberate differences from the vendor:

1. **Session-scoped claim.** The vendor opens `/dev/mem`, mmaps, flips the
   mux, requests the lines, transfers, releases and restores **on every
   single register access**: a drain of N events performs N+3 complete mux
   flips. PocketOS maps `/dev/mem` once, holds the gpiod request for the
   shell's lifetime, and reduces claim and release to two register stores.
2. **libgpiod v2 with bias and drive.** The only existing libgpiod user,
   `services/radiod/hal_linux.cpp`, never calls `set_bias()` or
   `set_drive()`. This is the first PocketOS code that needs either, so it
   cannot reuse that wrapper; the wrapper stays private to radiod.
3. **The chip layer never sees a device path.** It talks only to this
   vtable, which is what makes §11 possible.

## 5. TCA8418 initialisation

The vendor's sequence is adopted unchanged, because it is the only sequence
VERIFIED to work on this hardware — but **every return value is checked**,
where the vendor ignores steps 4 to 8.

| # | Action | Value |
| --- | --- | --- |
| 1 | Save io42 mux, set GPIO input with pull-up | `0x344` |
| 2 | Reset pulse on GPIO43, active low | 3 ms low, 12 ms high |
| 3 | Read `0x03` KEY_LCK_EC — presence probe | failure means absent |
| 4 | `0x1D` KP_GPIO1 | `0x7F` (rows 0-6) |
| 5 | `0x1E` KP_GPIO2 | `0xFF` (cols 0-7) |
| 6 | `0x1F` KP_GPIO3 | `0x03` (cols 8-9) |
| 7 | `0x29`, `0x2A`, `0x2B` DEBOUNCE_DIS | `0x00`, debounce on |
| 8 | `0x01` CFG | `0x29` = KE_IEN \| OVR_FLOW_IEN \| OVR_FLOW_M |
| 9 | Flush: up to 16 reads of `0x04`, stop at a zero byte | then `0x02` = `0x09` |

Steps 4 to 6 give 7 rows and 10 columns, which is what makes
`code = row * 10 + col + 1` true; all six codes measured on unit A satisfy
it (VERIFIED, §15).

**One correction to the vendor.** Its reset helper requests the line, sets
the value, sleeps 1 ms and then **releases** it, so the pin floats for most
of the nominal 3 ms and 12 ms with `BIAS_AS_IS` (DOCUMENTED). This driver
holds the line for the whole pulse. That the base tolerates a held reset
line is ASSUMED and is checked in the smoke plan (§12).

## 6. FIFO drain

```text
if level gate trusted and irq_level() == high: return
claim()
int_stat = read(0x02)                 /* failure is non-fatal */
count    = read(0x03) & 0x0F          /* failure: not ready, bail out */
if int_stat & 0x08: overflow_recover()        /* then keep draining */
for i in 0 .. min(max(count, 1), 16):
    ev = read(0x04)                   /* failure: not ready, keep partial */
    if ev == 0: break                 /* FIFO empty */
    code = ev & 0x7F; pressed = ev & 0x80
    if 1 <= code <= 70: deliver(ev)
    else:               count_unknown(code)   /* rate-limited log */
if handled or (int_stat & 0x0D): write(0x02, 0x0D)
release()
```

Two deviations from the vendor, both defensive:

- The vendor's loop is capped at **32** and, when its event count reads 0,
  degenerates into a scan until a zero byte. The part's FIFO holds 10, so
  this driver caps at **16**, which bounds the worst-case time on the UI
  thread honestly.
- A **per-tick delivery cap** keeps a burst of key-mashing from stalling a
  frame. INT stays asserted while events remain, so the remainder is taken
  on the next tick.

A zero byte means "FIFO empty": it is not delivered and not counted.

## 7. Retry and recovery

| Condition | Behaviour |
| --- | --- |
| Absent at init | Log once, create no timer, shell continues |
| Transaction failure | Mark not ready, release the bus, re-init throttled at 2 s, then 5 s, then 30 s |
| Overflow, `INT_STAT & 0x08` | Clear modifier state through `pos_keymap_reset()`, drop any pending state, count it, and **continue** the drain. No chip reset, matching the vendor |
| Repeated failure | Log transitions only, as `radio_reachable` already does in the status bar |
| Stuck bus, SDA low when idle | P2, optional: nine clock pulses and a STOP before re-init. The vendor has no bus recovery at all, so a wedged slave needs a reboot |

The 2 s first retry is the vendor's own throttle (DOCUMENTED); the longer
steps are this design's, to avoid a permanent 2 s I2C cycle on a board with
no keyboard attached.

## 8. Mux save and restore

- `/dev/mem` opened `O_RDWR | O_SYNC`, `mmap` of `0x1000` at `0x91105000`,
  and **the fd is closed straight after the mmap**. Offsets are `pin * 4`:
  io46 `0xB8`, io47 `0xBC`, io42 `0xA8`.
- **The saved value is the value read, not a constant.** The vendor writes
  `0x000019D1` to restore, while the board reads back `0x800019D1`
  (VERIFIED) — bit 31 appears to be read-only. Writing back exactly what was
  read avoids having to decide which is correct.
- Bit-bang value is `0x000001D1` (function 0). io42's mux is saved and
  restored too; the vendor sets it and never restores it (DOCUMENTED).
- Restored on the clean exit, on SIGTERM (the handler sets `stop_requested`,
  the loop breaks, destroy runs) and on every initialisation failure path.
- **Not restored on SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT.**
  `pocketlog_install_crash_handler()` accepts no cleanup hooks, uses only
  async-signal-safe calls and re-raises. Rather than pretend otherwise,
  **initialisation is deterministic**: it always writes the mux it needs and
  never assumes a state, so an unclean death is repaired by the next start.
  Leaving io46/47 in function 0 harms nothing — no other PocketOS code
  touches those pins, and the vendor launcher re-muxes per transaction.

## 9. Missing device

Absent at boot is a normal outcome, handled the way the shell already
handles a missing touch keyboard or an unavailable IPC socket: log once,
continue, and never spin. A diagnostic reports absence as **null, not zero**,
following the `pocketsys` convention.

No new build flag is needed: `kbd_bus_absent.c` covers the simulator, and
`libgpiod2` is already a dependency of the Buildroot package.

## 10. How an event reaches a field

```text
FIFO byte -> kbd_tca8418 -> shell_kbd -> pos_keymap_event(&km, raw, &effect)
                                          |- MODIFIER: state kept, nothing pushed
                                          |- RESERVED: ignored (F-row, Fn,
                                          |            LILYGO, mic)
                                          `- KEY:      pos_input_push_key(key)
                                                        -> LVGL keypad indev
                                                        -> focus group
                                                        -> focused field
```

The raw byte is handed to `pos_keymap_event()` unchanged, press bit and all,
because that function already decodes bit 7 and owns the modifier state.

**Nothing is adopted as an LVGL source.** `pos_input_add_source()` loses
`LV_KEY_NEXT` and `LV_KEY_PREV` (KNOWN_ISSUES), so pushing is the only
correct path for a physical keyboard.

**Auto-repeat is not in v1.** `pos_input.c` states that repeat belongs to the
source that can see a key still held. When it is added, it should reuse the
touch keyboard's timings (`POS_KB_REPEAT_DELAY_MS` 400, `POS_KB_REPEAT_MS`
60) so the device has one repeat behaviour rather than two; the vendor's own
450 ms and 85 ms are recorded here only as a reference point.

## 11. Unit tests with a fake bus

`tests/kbd_tca8418_test.c` drives a scripted fake bus: a register file, a
queued FIFO, per-call failure injection and a settable clock.

- The initialisation writes exactly the nine steps of §5, in order, with the
  exact values; a failure in steps 4 to 6 leaves the chip not ready.
- A failed presence probe yields absent and **no** further register traffic.
- `count` masks to `0x0F`; the drain stops on a zero byte; the cap of 16
  holds even when the count register lies.
- Overflow clears modifier state and still drains.
- `INT_STAT` is cleared with `0x0D` exactly when the rule in §6 says.
- A mid-drain read failure keeps the partial result and marks not ready.
- The re-init throttle honours 2 s against the injected clock.
- Codes 0 and 71 to 127 are ignored without touching modifier state.

`tests/kbd_lint.sh` guards the boundaries the way `pos_keymap_test.sh`
already does: `kbd_tca8418.c` names no `/dev/`, `gpiod_`, `mmap` or LVGL
symbol; `kbd_bus_k230.c` is the only file naming `/dev/mem`; no app
references `kbd_` or `TCA8418`.

`tests/shell_kbd_test.c` feeds a fake chip and asserts that a key reaches a
real text field through the real `pos_input`, including **Shift+W giving
`_`**, which ties the hardware mapping of §15 to the stream.

For that test to drive the real glue rather than a copy of it, `shell_kbd.h`
carries one seam: `shell_kbd_attach()` takes a bus from the caller, where
`shell_kbd_create()` builds the board's. The test supplies a fake controller
through it and exercises the actual poll timer, key path and overflow
recovery. `tests/kbd_shell_test.sh` runs that binary and then starts the
shell itself to check the no-keyboard path: it must start, log the absence
exactly once, and report no fault.

## 12. Hardware smoke plan for unit A

Preconditions: vendor launcher stopped, `meshtastic.autostart=0`, PocketOS
shell running, SSH on the bench key.

1. The log says present or absent, exactly once.
2. `gpioinfo` shows io46, io47, io42 and io43 consumed by `pocketos-shell`.
3. Type in Notes: `a w q j z`, then with Shift `~ _ ' ` Z` — the six codes
   of §15.
4. Mash keys to force an overflow: recovery is logged, no modifier sticks,
   typing continues.
5. A 100-character sequence arrives with no drops and no duplicates.
6. SIGTERM, then read `0x911050B8` and `0x911050BC` with `devmem`: the saved
   values are back.
7. `kill -9`, then restart: the keyboard works again, proving deterministic
   initialisation.
8. Detach the base: the shell starts, logs absent once, and does not spin.
9. Time one drain on the device before trusting the cadence of §14.

**Outcome, 2026-09-12.** Items 1 to 8 were all run on unit A and all passed;
the results are recorded in `KEYBOARD_BRINGUP_2026-09-10.md` §5.2 and §5.3.
**Item 9 was not run**, so the cost model in §14 — about a millisecond per
register read, and therefore per poll — remains ASSUMED rather than measured.
Item 4's overflow was never provoked: a moderate burst produced none, and
forcing one was deliberately not attempted.

## 13. Risks and blockers

### 13.1 Shipping gate: DS §18.8

DS §18.8 says the alert's touch-only limitation "MUST be resolved before a
physical keyboard ships". The product owner has confirmed (2026-09-12) that
this does not block the driver prototype, and that the physical-keyboard
milestone is **not complete** until:

- alerts isolate keyboard input from the app hidden behind them;
- the alert's actions participate correctly in the focus group;
- focus is restored correctly when the alert closes;
- the touch keyboard cannot reappear above an active alert through a hidden
  field regaining focus.

**SATISFIED 2026-09-12**, commit `2afe7fe`, accepted on unit A the same day
(`KEYBOARD_BRINGUP_2026-09-10.md` §5.4). Against the four conditions above:
keys no longer reach the app behind an alert (printable characters and
Backspace both proved inert, on hardware and in `shell_alarm_test`); the
alert's actions participate through a **private** group handed to the stream
while the alert is shown, rather than joining the one app group — a
deliberate difference from the DS wording, recorded in DS §18.8; focus is
restored on acknowledgement, including when the object underneath was
destroyed meanwhile; and an app asking for the touch keyboard while an alert
holds the panel is refused by a suppression rule the shell asks rather than
duplicates.

The gate is closed. What remains open around it is smaller and listed in
docs/KNOWN_ISSUES.md: `lv_group_create()` failing at start-up is not
deterministically exercised, and the alert still writes no log line when it
is shown or dismissed, which is why the panel had to be the witness during
acceptance.

### 13.2 Other risks

- **Focus-group membership** (DS §17.2): only text fields and dialog buttons
  join, so Tab and the arrows do nothing on the launcher. The keyboard is
  useful in text contexts until membership is widened. A product decision,
  not a defect.
- **GPIO42 — answered 2026-09-12: it works.** On unit A the driver read the
  line as a level and kept the gate, so the earlier low reading really was
  the plastic film. The unconditional fallback stays in the code regardless:
  one board on one day is not every board.
- **A held reset line** is correct but ASSUMED safe on this base.
- **An unclean death inside the bus-claim window is untested.** The driver
  claims the bus for roughly 1 ms in every 15, and §8 accepts that a SIGKILL
  cannot restore the mux, relying instead on deterministic re-muxing at the
  next start. There is a subtler consequence: a kill landing *inside* that
  window leaves io46/io47 at the bit-bang word, and the next instance saves
  whatever it finds, so it would adopt that word as its restore value and a
  later clean exit would leave the bus in GPIO mode. The keyboard keeps
  working either way. The C2 test (2026-09-12) landed between polls and so
  did not exercise this; it shows the hazard did not occur, not that it
  cannot. Saving a known-good value rather than the observed one would
  remove it.
- **`LV_DEF_REFR_PERIOD` is unknown** — the shell's `lv_conf.h` is generated,
  and that period bounds end-to-end latency. Measure before tuning.
- **The rest of the key map is unread.** Only six codes are VERIFIED; every
  other legend still comes from a vendor table already proven wrong twice.
- **Root only.** `/dev/mem` needs root, which v0 has and the per-service-user
  follow-up will have to revisit.
- **Bus ownership** (§0.2) when charger or battery support arrives.

## 14. Why 15 ms with a level gate

From the vendor's timing, 8 us per line transition (DOCUMENTED), a byte
costs about 216 us and **a register read about 1 ms**. An unconditional I2C
poll therefore costs roughly 1 ms per tick on the one UI thread: **10 % at
10 ms and 5 % at 20 ms**, plus jitter injected into LVGL's frames.

The cadence is right; the transaction is not. Reading the **GPIO42 level**
costs microseconds and needs no I2C. The TCA8418 holds INT asserted until
`INT_STAT` is cleared, and the vendor's own safety net relies on exactly
that: on a 50 ms timeout with no edge delivered it reads the line level and
drains when it is low (DOCUMENTED). Checking the level on a timer is still
polling — no edge events, no epoll, no thread — and it drops the idle cost
by about two orders of magnitude.

- **15 ms** timer, I2C only when INT is asserted.
- **Runtime fallback** to unconditional I2C at **20 ms** when GPIO42 proves
  unusable; decided at runtime and logged, not a build flag. Implemented as
  specified since 2026-09-12: the timer's period changes on the edge and the
  transition is logged once, at WARN. A board whose line was never usable
  starts in the fallback mode rather than falling into it, so the period and
  the mode always agree. The gate is only ever lost, never regained, so the
  log carries at most one such line per session — and its **absence** is what
  says the gate was kept, which is the only way to tell after boot.
- **Distrusting the line takes evidence, not one coincidence.** A sweep
  drains whatever the line says, so between reading the level and reading the
  FIFO there is a window of one or two milliseconds in which a key genuinely
  pressed just then arrives and is found. That is not a lying line. The
  stuck-high verdict therefore requires the level to have read *idle* while
  the sweep found events, and requires it three times, cleared the moment the
  line is seen asserted. A line that is genuinely stuck never reads asserted
  and so reaches the verdict in about 1.5 s.
- Worst-case latency is about 15 ms of detection plus 1 to 3 ms of drain
  plus one LVGL input read, so roughly **50 ms** (the last term ASSUMED
  until `LV_DEF_REFR_PERIOD` is measured).
- Headroom: at ten keys per second an event arrives every 100 ms against a
  ten-deep FIFO, so an overflow needs about 1.5 s of total stall at 20 ms.
- **Do not go below 10 ms.** It buys latency that the LVGL read period hides
  anyway, and it doubles the UI-thread cost in the fallback mode.

Edge-driven input stays a later change that must justify itself with
measurements taken on the device.

**Outcome on unit A, 2026-09-12 (first run).** Typing was correct end to end
with no dropped keys, no overflow and no warning or error logged. The
acceptance also recorded that the driver "selected and kept the 15 ms
INT-gated mode"; **the second half of that was withdrawn the same day.** It
rested on the one line logged at start-up, which reports the mode chosen at
boot and nothing after it, and the driver as it then stood switched the gate
off on the first unconditional sweep that coincided with a keypress —
about half a second into any real typing — with nothing logged when it did.
So the run is evidence that the keyboard works, and was never evidence about
which mode it worked in. The code fix and the log line that makes the claim
checkable at all are above; the measurement is below.

**Outcome on unit A, 2026-09-12 (re-measured after the fix).** Build
`6d4b318-dirty`. The driver started INT-gated at 15 ms and **kept the gate**
through about 40 s of continuous typing — 69 keys delivered, 0 dropped, 0
`INT gate dropped` lines, 0 ERROR/WARN, the radio untouched and the mux words
back at rest. Roughly eight sweeps fell with the FIFO non-empty, each of
which was enough to end the gate under the old predicate. Full table:
`KEYBOARD_BRINGUP_2026-09-10.md` §5.5.

This is now a claim the log can support, which the first run's version of it
could not. The 20 ms fallback still has **not** been exercised on hardware:
no unit has yet presented a GPIO42 that misbehaves, so the fallback period
and its log line are VERIFIED on the host only.

The latency figure above is still ASSUMED: nothing has yet measured a key
press to a glyph on the device.

## 15. The hardware evidence this driver is built on

Measured on unit A on 2026-09-11 and 2026-09-12 through the vendor's key
test page, captured over SSH (KEYBOARD_BRINGUP_2026-09-10.md §5.1). Every
line satisfies `code = raw & 0x7F`, bit 7 = press, and
`row/col = (code-1)/10, (code-1)%10`.

| Key | Press | Release | Code | Row/col | Keycap | Class |
| --- | --- | --- | --- | --- | --- | --- |
| A | 0x9D | 0x1D | 29 | 2 / 8 | A + orange `~` | VERIFIED |
| W | 0xA7 | 0x27 | 39 | 3 / 8 | W + orange `_` | VERIFIED |
| Q | 0x94 | 0x14 | 20 | 1 / 9 | Q + orange `'` | VERIFIED |
| J | 0xA2 | 0x22 | 34 | 3 / 3 | J + orange `` ` `` | VERIFIED |
| Z | 0x92 | 0x12 | 18 | 1 / 7 | Z, no symbol | VERIFIED |
| Shift | 0x87 | 0x07 | 7 | 0 / 6 | orange arrow | VERIFIED |

Shift is an ordinary matrix key: it never changes the code of the key
pressed with it, and two held keys are both reported. Two vendor symbols
were wrong and are corrected in `pos_keymap.c` (commit 1172498).

## 16. Implementation order

Approved for implementation in this order, each step green before the next:

1. `kbd_bus` abstraction
2. K230 bit-banged bus
3. TCA8418 state machine
4. fake-bus tests
5. shell glue
6. SDL and end-to-end tests
7. K230 cross-build

DS §18.8 is not touched during this work. Nothing merges to master, and no
release image is flashed, until the milestone is complete.
