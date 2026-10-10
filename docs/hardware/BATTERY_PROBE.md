# Battery probe on the keyboard base

**Status: DIAGNOSTIC ONLY.** An opt-in, read-only probe of the keyboard
base's BQ27220 fuel gauge and BQ25896 charger, logging to `shell.log`. No UI,
nothing published to sysd, no configuration written. It exists to find out
whether the gauge on unit A answers and what it says, before any battery
status is shown in Doors.

Evidence classes follow AGENTS.md: **VERIFIED** (measured on a unit),
**DOCUMENTED** (read from TI or vendor documents), **ASSUMED**.

## 1. Bus ownership

The gauge (0x55) and charger (0x6B) share the bit-banged GPIO46/47 bus with
the TCA8418 keyboard controller and the XL9555 expander (vendor pin map,
DOCUMENTED). The shell owns that bus (KEYBOARD_DRIVER_DESIGN_2026-09-12.md
§0.2). The product owner decided on 2026-10-10 that the diagnostic probe runs
inside that same owner: same process, same LVGL thread, the same per-access
claim and release, the same bit timing. No second process, no kernel driver,
no device-tree change.

## 2. What is read, and from which document

| Device | Register | Meaning | Units, sign | Source |
|---|---|---|---|---|
| BQ27220 0x55 | 0x2C StateOfCharge() | RemainingCapacity / FullChargeCapacity | %, unsigned, 0-100 | SLUUBD4A §2.21 |
| | 0x08 Voltage() | cell-pack voltage | mV, unsigned, 0-6000 | SLUUBD4A §2.6 |
| | 0x0C Current() | instantaneous current through the sense resistor, updated every second | mA, signed | SLUUBD4A §2.8 |
| | 0x0A BatteryStatus() | flags, Table 2-6 (DSG bit 0, BATTPRES bit 3, FC bit 9, FD bit 15, ...) | - | SLUUBD4A §2.7 |
| | 0x12 FullChargeCapacity() | compensated capacity when full | mAh, unsigned | SLUUBD4A §2.10 |
| | 0x3C DesignCapacity() | data flash "Design Capacity mAh" (TI default 3000) | mAh, unsigned | SLUUBD4A §2.28, data memory table |
| | 0x3A OperationStatus() | flags, Table 2-7 (SEC[1:0] bits 2:1, INITCOMP bit 5, CFGUPDATE bit 10) | - | SLUUBD4A §2.27 |
| BQ25896 0x6B | 0x0B REG0B | VBUS_STAT[7:5], CHRG_STAT[4:3], PG_STAT[2], reserved[1] reads 1, VSYS_STAT[0] | read-only | SLUSC76C Table 17 |

Gauge words are one incremental two-byte read, low byte first (SLUSCB7A
§7.3.1.1 (d)). The datasheet does not state Current()'s sign convention in
the register description; which sign is discharge is to be read off the
hardware against BatteryStatus()[DSG].

Not read on purpose: the charger's ADC registers (0x0E-0x12). They only
update after a conversion is started by writing REG02[CONV_START], which is a
configuration write and out of scope.

## 3. Pacing and limits

- SLUSCB7A §7.3.1.3: the host "must not issue any standard command more than
  two times per second", or the gauge's watchdog may reset it. The probe does
  one transaction per step with at least 600 ms between steps, from the
  keyboard's existing one-second watch timer. A sample (eight reads) takes
  about eight seconds and repeats every 60 s.
- A device that does not answer is asked once per sample, not once per
  register. When neither answers the period doubles up to 10 minutes; any
  answer restores 60 s. A missing device therefore costs one failed address
  byte per sample and one log line per sample.
- SLUSCB7A §7.3.1.4: the gauge may stretch SCL (about 100 us out of SLEEP, up
  to 4 ms otherwise). The gauge's block reads therefore read SCL back after
  releasing it and wait for it to go high, bounded at 8 ms; a read that fails
  clocks SDA free (up to nine pulses) before its stop. The K230 GPIO driver
  reads the pin level for an output line too (`gpio-k230.c` gives
  `bgpio_init` the EXT_PORT register as its data register, no flags;
  DOCUMENTED from the pinned SDK kernel), which is what SDA reads already rely
  on. The keyboard's own transactions do not read SCL and are unchanged.
  Ignoring the stretch wedged the bus on hardware (§6.1).
- Values are also range- and reserved-bit-checked and marked `?` with a
  `suspect=` mask in the log; a plausible value is not proof of a correct one.
- It runs only while the keyboard controller is answering, from the watch
  timer, never inside a key drain. A keyboard drop pauses it without
  resetting its pacing.
- Breaker: if the keyboard controller stops answering within 2 s of a probe
  read, the probe is off until the shell restarts (one WARN line).

## 4. Turning it on and off

On the unit, no restart needed:

```sh
touch /run/pocketos/battery-probe     # on
rm /run/pocketos/battery-probe        # off
grep 'battery probe' /var/lib/pocketos/log/shell.log
```

The flag lives in tmpfs and is gone after a reboot. Log lines:

```
battery probe: on (read-only; gauge 0x55, charger 0x6b; one register per 600 ms, a sample every 60 s)
battery probe: gauge 0x55: soc=..% voltage=..mV current=..mA fcc=..mAh design=..mAh status=0x....[...] op=0x....[sealed,...]; charger 0x6b: reg0b=0x.. vbus=... chrg=... pg=. vsys_min=.
battery probe: off after N samples (R reads, F failed)
```

## 5. Reading a percentage

StateOfCharge() is only as good as the gauge's configuration. The gauge is a
CEDV gauge whose data flash (design capacity, EDV thresholds, chemistry
profile) is set by whoever configured it; the vendor launcher wrote a
6000 mAh design capacity into it once (ui_hardware.c, DOCUMENTED), and
whether that ever ran on a given base is unknown. A DesignCapacity() that
matches the pack's rating does not by itself make the percentage accurate:
the EDV profile, a learned FullChargeCapacity() (OperationStatus()[VDQ], a
qualified discharge) and INITCOMP all matter.

## 6. Hardware results, unit A, 2026-10-10

Unit A before: Doors 0.3.5 userspace (`BUILD_ID=3d4ea6e`), shell `bddb56e`
= v0.3.5 + vision A/B `36dc5b9` (VISION_MODEL_AB_GATE.md; running binary
sha256 `bb012ede…f05a`, matching that record). Kernel still the 27 Sep build
(v0.3.5 boot files staged, not booted). Shell-only deploys, each built from
v0.3.5 + `36dc5b9` + this branch on local branch
`integration/unitA-v035-vision-ab-battery`, cross-built with the pinned
toolchain, 0 warnings, stripped. Vision's two A/B kmodels and `yolov8n.kmodel`
were not touched. Rollback: `/root/rollback-battery-probe/RESTORE.sh` puts the
`bddb56e` shell back (backup sha256-checked before the first swap).

Physical setup (owner): nothing plugged in, neither the K230's USB-C nor the
base's; base battery switch ON; two cells labelled "Biltema 3000 mAh 3,7 V
11,1 Wh".

### 6.1 First run: the bus ignored clock stretching (build `379a249`)

14:30:32-14:33:09 UTC. Every gauge read failed, and each time the keyboard
controller stopped answering 30-250 ms later and recovered about a second
later; the probe re-initialised on every recovery, so the cycle repeated every
two seconds for about 2.5 minutes (79 controller drops) until the flag was
removed. The keyboard recovered by itself once the probe was off. Consistent
with the gauge stretching SCL mid-packet while the master clocked on, leaving
the gauge part way through a byte and the next keyboard transaction failing.
Fixed in the next build (§3: SCL read-back, bus recovery, breaker, no reset
on keyboard drops).

### 6.2 Second run (build `00aa3e3`)

14:39:35-14:45:23 UTC: 6 samples, 48 reads, 0 failed, nothing marked
suspect; 0 keyboard controller drops; the owner typed on the keyboard during
the run and it worked normally (10 keys delivered). sysd, netd, radiod and
meshcored kept their pids throughout.

| Reading | Values (6 samples) | Class |
|---|---|---|
| StateOfCharge() | 30, 30, 29, 29, 29, 29 % | VERIFIED as read; accuracy NOT verified (§6.3) |
| Voltage() | 3782-3791 mV | VERIFIED |
| Current() | -568 to -697 mA, negative while BatteryStatus()[DSG] is set and no input is present: negative is discharge | VERIFIED |
| BatteryStatus() | 0x4029: DSG, BATTPRES, OCVGD, OCVCOMP | VERIFIED |
| FullChargeCapacity() | 3512 mAh | VERIFIED as read |
| DesignCapacity() | 3000 mAh, TI's data-flash default | VERIFIED as read |
| OperationStatus() | 0x00B4: SEC = unsealed, VDQ, INITCOMP, BTPINT | VERIFIED |
| BQ25896 REG0B | 0x02: VBUS no input, not charging, not power good, reserved bit reads 1 as documented, not in VSYSMIN regulation | VERIFIED |

Derived: about 2.2-2.6 W drawn from the pack (V x I); SOC falling 1 % in
about 5 minutes at about 0.6 A is consistent with coulomb counting against a
3512 mAh full charge (0.05 Ah is 1.4 % of it).

### 6.3 What the percentage is worth

- The pack voltage is single-cell (3.79 V), and the BQ25896 is a single-cell
  charger, so two cells must be in parallel (1S2P) if both are in circuit;
  that both are is ASSUMED. Their labels give 2 x 3000 mAh, about 6000 mAh
  nominal.
- The gauge still holds TI's default Design Capacity (3000 mAh): the vendor
  launcher's 6000 mAh write is not in effect on this gauge. Its EDV
  thresholds and other CEDV parameters are unread and presumably defaults
  too (ASSUMED).
- FullChargeCapacity() has been learned at 3512 mAh (VDQ set), well short of
  the ~6000 mAh nominal. StateOfCharge() is a percentage of that figure, so
  30 % here may not be 30 % of the pack.
- The gauge is unsealed, so its data flash is writable by anything on the bus.

So: the gauge answers and its voltage, current, flags and charger status are
usable as read. The percentage is self-consistent but rests on an
unconfigured gauge and is **not usable as an accurate battery level** until
the gauge's configuration matches the pack. A matching DesignCapacity() alone
would not settle that (§5).
