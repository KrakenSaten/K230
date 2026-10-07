# K230 main-board buttons: mapping before any role is assigned

Branch `research/k230-buttons`, from master `ee0c1f8`. Research only: no
code, no device tree, no persistent setting was changed. Evidence classes as
in AGENTS.md: **VERIFIED** (measured on our hardware), **DOCUMENTED** (vendor
schematic or source, cited), **ASSUMED**.

Sources: schematic `vendor/T-Display-K230_canmv_rt/schematic/T-Display
K230_V1.0_NEW.pdf` (sheet 2 "K230", sheet 6 "Peripherial"), LILYGO BSP
`vendor/T-Display-K230/k230_bsp/` (pin map, kernel patch
`0064-input-k230-pmu-pwrkey.patch`, U-Boot board files), vendor launcher
`k230_launcher/k230_phone_ui/src/{ui_hardware.c,main.c}`, CanMV FPIOA tables
(`canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/fpioa/rt_fpioa.c`),
and unit B (`192.168.10.187`, Doors 0.3.0, `BUILD_ID=6b26f06`, kernel
`6.6.36`) on 2026-10-06.

## 1. Button map

The board has three tactile switches, all `TS36CA-0.6 250gf` (schematic
sheet 6, top right).

| | SW1 | SW2 | SW3 |
| --- | --- | --- | --- |
| Silkscreen (owner, unit B) | RESET | "0" | "boot" |
| Net | `RSTN` | `INT0` | `BOOT0` |
| Switch connects net to | GND | `VDD_1V8_RTC` | GND |
| SoC ball / pin | B9 `RSTN` (chip reset) | C10 `GPIO64/INT0` (PMU input 0) | C8 `GPIO0/BOOT0` (pad IO0) |
| Bias | R7 100k to `VDD_1V8`, C5 100 nF to GND (RC) | no external resistor on the schematic; PMU input | R8 10k pull-up to `VDD_1V8`; R11 pull-down not fitted |
| Active level | low | **high** | **low** |
| Other controller involved | none; resets the SoC | the K230's own always-on **PMU** (RTC domain), which also drives `OUT0`, the enable of the main 5 V switch U10 (MT9700, `PRE_VDD_5V` -> `VDD_5V`) | none; strap pin read by the boot ROM |
| Class | DOCUMENTED | DOCUMENTED; levels VERIFIED | DOCUMENTED; levels VERIFIED |

All three go directly to the K230; there is no separate power-management IC.
The "PMU" is the K230's internal always-on block. The schematic note next to
the PMU (sheet 2) says, in translation: INT0 is designed so that a 3 s long
press drives OUT0 high (system power on); INT4 drives OUT0 high immediately on
a pull-up. The INT4 pull-up is R10 10k to `VDD_1V8_RTC` (DOCUMENTED).

## 2. Boot / recovery semantics

**SW3 "boot" (BOOT0): boot strap. Never hold it during reset or power-on.**

- The strap table (sheet 2, DOCUMENTED): BOOT0/BOOT1 = 0/0 SPI NOR, 0/1 eMMC,
  1/0 SPI NAND, 1/1 SD card. BOOT1 has R9 10k pull-up and no switch, so the
  board straps 1/1 = SD card. Holding SW3 at reset gives 0/1 = **eMMC**
  (SDIO0). This board has no eMMC: its MMC0/SDIO0 is wired to the RTL8189
  Wi-Fi module (sheet 6). The K230 boot ROM would therefore look for its
  first stage on the wrong medium.
- What the boot ROM does after that (stops, retries, or falls back to a USB
  download mode) is **not documented** in any vendor source in the repo. The
  LILYGO README flashes only by writing the SD card. Nothing in the
  repository documents a USB burn mode for this board. **UNKNOWN; not tested.**
  Holding the button during boot was deliberately not tried.
- U-Boot ignores the strap: LILYGO's `board/canaan/k230_canmv/board.c`
  overrides `sysctl_boot_get_boot_mode()` to return `SYSCTL_BOOT_SDIO1`
  unconditionally (DOCUMENTED). Linux does not read it. After the boot ROM has
  run, the button has no meaning to any firmware stage.
- At runtime it is only pad IO0. Pressing it on a running unit had no effect
  (§4).

**SW2 "0" (INT0): power-on key of the PMU.**

- Power on from off: an INT0 long press of about 3 s sets OUT0 and switches
  the main 5 V rail on (schematic note, DOCUMENTED). This is the power-on path
  after a PMU power-off.
- Linux power-off: the kernel driver's power-off handler re-arms "INT0 long
  press" as the PMU's wake source (`PMU_INT0_LONG_PRESS_TRIGGER_VAL` = 96000
  ticks of 32.768 kHz, 2.93 s), then asks the PMU to cut power
  (`0064-input-k230-pmu-pwrkey.patch`, DOCUMENTED). Hardware power-off was not
  exercised here.
- At runtime the driver routes only the edge event to the CPU and masks the
  PMU's own long-press and shutdown actions to OUT0
  (`PMU_INT0_TO_CTL_REGISTER` cleared). Pressing it does not cut power in
  hardware. The 5 s power-off is software (next item).
- **Kernel policy already bound to this button:** holding it for 5 s calls
  `orderly_poweroff(true)` (`PMU_SOFT_SHUTDOWN_MS`). It is not a boot-mode or
  recovery function, but it is active on every Doors image today.
- It plays no part in boot-mode selection, flashing or recovery (DOCUMENTED:
  no reference in U-Boot, the boot scripts or the LILYGO docs).

**SW1 RESET**: hardware reset of the SoC (`RSTN`). Not touched. It is the
only reset path. Pressing RESET while SW3 is held is exactly the strap hazard
described above.

## 3. Linux exposure on the running Doors kernel (unit B, VERIFIED)

| Item | Finding |
| --- | --- |
| Input devices | `/proc/bus/input/devices`: `K230 PMU Power Key` (event0, `kbd`) and `goodix_ts` (event1). Nothing else. |
| SW2 / INT0 | `k230-pmu-pwrkey` at `91000000.pmu-pwrkey`, IRQ 97 (PLIC 175, level), `/dev/input/event0`, `EV_KEY` `KEY_POWER` (116), wakeup source `enabled`. Log: `registered on IRQ 97, long-press=5000ms`. |
| SW3 / BOOT0 | **Not exposed.** No gpio-keys node. The device tree has no pinctrl group for IO0 (the iomux groups are AHT20, amp I2S, camera, keyboard backlight/IRQ, LoRa, UART1/3). gpiochip0 line 0 is unclaimed. |
| IO0 pad as booted | iomux word `0x00000AC4` on both units: function 1 (= `BOOT0`, per the FPIOA table `{GPIO0, BOOT0, TEST_PIN0}`), output enable 1, **input enable 0**, pull-up on, 1.8 V. This is left by the boot firmware, not set by Linux. With input disabled the GPIO block cannot see the button. The vendor launcher wrote `0x344` (GPIO, input, pull-up) through `/dev/mem` before reading it. |
| Kernel config | `CONFIG_INPUT_EVDEV=y`, **`CONFIG_KEYBOARD_GPIO=y`** (gpio-keys built in), `CONFIG_GPIO_CDEV=y` (libgpiod v2 tools present: `gpioget`, `gpiomon`), `CONFIG_GPIO_K230=y`, `CONFIG_GPIOLIB_IRQCHIP=y`, `CONFIG_PINCTRL_K230_IOMUX=y`, `CONFIG_INPUT_K230_PMU_PWRKEY=y`. Both GPIO ports (`9140b000`, `9140c000`) are `interrupt-controller`s. No sysfs GPIO class. `/sys/power/state` = `freeze mem`. |
| Doors today | The shell reads neither button. It opens only the touch node; its evdev discovery (`ui/shell/platform_drm.c`) accepts only ABS devices, so it never takes event0. No service reads event0 or IO0. |

## 4. Runtime test, unit B, 2026-10-06 (VERIFIED)

Method: a watcher run from `/tmp` over SSH. The power key was read from
`/dev/input/event0` and the driver's dmesg lines. For BOOT0, the IO0 pad was
switched for the run to `0x344` (the vendor launcher's value) and watched with
`gpiomon -c gpiochip0 0`, with no debounce. The pad was restored exactly
(`0xAC4` read back after every run). There was no reflash and no reboot.
Nothing was held during boot. The owner pressed the buttons in a given order:
the "boot" button first, then "0".

| | SW3 "boot" -> GPIO0 | SW2 "0" -> INT0 / event0 |
| --- | --- | --- |
| Released | `1` (pad DI bit set, `0x80000344`) | no event; driver idle |
| Round 1, one press | falling 6.725 s, rising 7.556 s (0.83 s) | press, release after 1.28 s |
| Round 3, 3 taps | 0.19 s, 0.22 s, 0.23 s low | 0.26 s, 0.24 s, 0.28 s |
| Round 3, hold | 1.59 s low | 1.93 s |
| Edges per press | exactly one falling, one rising | exactly one press, one release |
| Bounce | none visible (no debounce in the path) | none (PMU debounce 256 ticks, 7.8 ms, set by the driver) |
| Repeat events | none (GPIO edges only) | none (`value 2` never seen; the driver does not auto-repeat) |
| Kernel log | none | `INT0 press` / `INT0 release` per press |
| Unexpected behaviour | none: no reboot (uptime 2 d 7 h), shell pid unchanged, no screen change | none. The screen went off on its 5 min idle timer during the test: the presses did not count as activity, which confirms Doors ignores the key today |

The ordered round 3 ties each label to its signal: "boot" = GPIO0 = SW3, and
"0" = INT0 = SW2. The owner read the second label as "0". The exact
silkscreen text is not otherwise recorded.

Raw INT0 level: the vendor launcher also reads INT0 through the PMU iomux DI
bit (`0x91000080` bit 31). That bit stayed `0` through every press (5 ms
poll). It does not reflect the key on this unit, so the evdev node is the only
working path.

Untested here: pressing SW3 with the pad in its as-booted state (`0xAC4`,
output enable set, function BOOT0). The owner's earlier physical test
(HARDWARE_CONTROLS.md §10, "top two-way control, no response") was probably
these two buttons, but that is not established. With the pad as booted
nothing can see SW3. Whether the BOOT0 function drives the pad is not
documented.

## 5. Input ownership

| Path | SW2 / INT0 | SW3 / BOOT0 | Verdict |
| --- | --- | --- | --- |
| Kernel gpio-keys / evdev | already evdev (`event0`, `KEY_POWER`) | needs a device-tree change: a pinctrl group putting IO0 into GPIO input with pull-up (as `0x344`) plus a `gpio-keys` node on `gpio0_ports 0`, `GPIO_ACTIVE_LOW`, with a key code and `debounce-interval`. The driver is already built in (`CONFIG_KEYBOARD_GPIO=y`); no new driver. Needs a kernel/DTB rebuild and a flash. | **Preferred.** Both buttons become evdev keys. The kernel owns the pad, debounce and IRQ. |
| Shell / input layer (userspace pad write + libgpiod edges) | read event0 | the shell already maps the iomux block (`kbd_bus_k230_light_mux()` for io52). It could switch IO0 the same way and use libgpiod edge events (gpiochip0 is IRQ-capable). It is hot-deployable, but it makes the shell a second pinmux owner and rests on `/dev/mem`. | Fallback only. |
| Direct GPIO polling (vendor launcher) | n/a (the PMU DI bit does not move, §4) | polling `/dev/mem` plus libgpiod every frame | Rejected: polling cost, `/dev/mem`, and the INT0 half does not work. |

Can the Doors shell consume them cleanly? **Yes.** The shell is the one
process that hosts every app (apps are in-process LVGL screens), so a key it
reads is system-level, whatever app is in front. The pieces already exist:

- `hw_actions` (`ui/shell/hw_actions.[ch]`): `HW_ACTION_BACK` (the header's
  back slab after the app's own `back` hook) and Home
  (`pocketos_shell_go_home()`). Today they are reachable only through
  `shell.action`. They already respect the lock and alerts.
- `shell_power` (`ui/shell/shell_power.h`): screen off/wake
  (`shell_power_wake`, `shell_power_gate`), the lock hook, and
  `shell_power_activity()`.
- Missing: an evdev reader for key devices in the shell's loop. The current
  discovery deliberately ignores non-ABS devices. It would also need
  press-duration timing for short versus long press, since neither source
  repeats.

## 6. Proposed roles

| Role | Assessment | Reason |
| --- | --- | --- |
| Button A = SW2 "0" (INT0), short press: screen off / wake / lock | **SAFE** | It is already an evdev `KEY_POWER` with clean press/release (VERIFIED). Screen off in Doors is a cover, not suspend (DS §52.4), so waking needs no wake-source work. The shell has the off/wake/lock calls. It is the board's power key, so the meaning matches the hardware. |
| Button A, long press: power menu | **SAFE, with a fixed constraint** | The kernel's 5 s hold calls `orderly_poweroff` whatever the shell does. The shell's long-press threshold must be well under 5 s (for example about 1 s). The menu must treat the kernel's 5 s power-off as the forced fallback: it cannot cancel it, and must not try. Changing or removing the kernel's 5 s behaviour would be a separate, owner-approved kernel change. Power-on remains the PMU's ~3 s hold. |
| Button B = SW3 "boot" (BOOT0), short press: Back | **NEEDS MORE WORK** | The mapping is proven, but Linux cannot see the button on the shipped image: the IO0 pad is BOOT0 function with input disabled (`0xAC4`). It needs the DT pinctrl plus gpio-keys change (§5) and an image rebuild before any role can work. The strap hazard is also permanent: holding Back while pressing RESET or powering on selects eMMC boot, with an undocumented outcome. A Back button is pressed often, so this needs to be written down for users. |
| Button B, long press: Home / Launcher | **NEEDS MORE WORK** | The same input-path work. It also makes holding this button a learned habit, which raises the chance it is held across a RESET press. Nothing in software can override the strap (the boot ROM samples it), so this risk can only be documented. |
| RESET | unchanged | — |

Recommended mapping: as proposed. The power key (SW2 "0") carries screen
off/wake/lock and the power menu, matching its hardware role. The BOOT0
button (SW3 "boot") carries Back and Home, once the device tree exposes it as
a gpio-keys input. Do not assign the BOOT0 button anything a user would hold
while rebooting.

## 7. Risks

1. **BOOT0 strap.** SW3 held at reset or power-on selects eMMC (SDIO0 = the
   Wi-Fi module on this board). What the boot ROM does then is undocumented.
   Recovery would most likely be a release and a RESET press, but this is
   **untested**.
2. **Kernel 5 s power-off on SW2** coexists with any shell long-press role and
   cannot be vetoed from userspace.
3. **IO0 pad as booted** (`0xAC4`): output enable set with the BOOT0 function.
   Whether that function drives the pad, and so whether pressing SW3 in that
   state fights a driver, is undocumented. Today's presses were all made with
   the pad as a GPIO input. A gpio-keys pinctrl group removes the question from
   kernel probe onwards. The window from boot ROM to kernel is the same as on
   the vendor image.
4. **The PMU DI bit does not show INT0** on this unit (§4). Do not build on the
   vendor launcher's raw-read path.
5. **Neither source auto-repeats.** Long press must be timed by the consumer
   from press and release.

## 8. What was not done

No driver, no device-tree change, no shell change, no role, no change to
RESET, the bootloader, recovery or flashing. Nothing was held during boot.
Only unit B was pressed; unit A was read only (same kernel and the same input
devices, IO0 `0xAC4`).
