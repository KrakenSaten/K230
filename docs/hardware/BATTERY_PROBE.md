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
  to 4 ms otherwise). This bus never reads SCL back - neither did the vendor
  launcher's - so a stretched read can return wrong bits rather than fail.
  Values are therefore range- and reserved-bit-checked and marked `?` with a
  `suspect=` mask in the log; a plausible value is not proof of a correct one.
- It runs only while the keyboard controller is answering, from the watch
  timer, never inside a key drain.

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

## 6. Hardware results

Not yet measured.
