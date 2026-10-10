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
  charger, so the cells are not in series. Whether both are in circuit is
  open (§7.1). Their labels give 2 x 3000 mAh, about 6000 mAh nominal.
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

## 7. Configuration analysis (2026-10-10, no writes)

Sources: TI SLUUBD4A (BQ27220 TRM), SLUSCB7A (BQ27220 datasheet), SLUSC76C
(BQ25896 datasheet); LILYGO `k230_phone_ui/src/ui_hardware.c` (Linux
launcher) and the RT-Smart `ui_brookesia/.../battery/BQ27220/` sample.

### 7.1 Wiring

- LILYGO publishes no schematic for the keyboard/battery base: none in the
  pinned vendor trees, none in Xinyuan-LilyGO/T-Display-K230,
  T-Display-K230_canmv_rt, T-Display-K230-nRF9151 or T-Display-K230-nRF52840
  (checked 2026-10-10), none on the product page. The only schematic
  (`T-Display K230_V1.0_NEW.pdf`) is the main board, with no BQ parts.
- VERIFIED: pack voltage 3.78-3.79 V, so not series. DOCUMENTED: BQ25896 is a
  single-cell charger and the BQ27220 computes EDVs for n = 1 series cell.
- Not established: that both holders are connected. Pointing that way, but
  not proof: LILYGO's launcher treats the base pack as 6000 mAh
  (`BATTERY_CAPACITY_DEFAULT_MAH`, DOCUMENTED intent), and the learned FCC of
  3512 mAh says a past qualified discharge delivered at least 3512 mAh (§7.2),
  more than one 3000 mAh cell's rating - if the learning came from these
  cells, which is not known.
- Settling it needs a physical check by the owner (§7.5, P1).

### 7.2 What the readings mean

- Data memory is volatile RAM initialised from ROM; "the written data is not
  persistent, so a POR does resolve the fault" (SLUUBD4A §3.1). The learned
  FCC lives there too. A gauge power-on reset (cells out, or the gauge losing
  supply - whether the base switch does that is unknown) returns everything
  to the ROM defaults.
- DesignCapacity() 3000 mAh = ROM default *Design Capacity mAh* (0x929F).
  Nothing has written it since the gauge's last reset. TI: it should be the
  cell rating times the number of parallel cells (§1.1.12).
- FullChargeCapacity() 3512 mAh: initialised from *Learned Full Charge
  Capacity* (0x929D, default 3000) and updated only after a qualified
  discharge (from within *Near Full*, 200 mAh, of full down to EDV2 with no
  charge in between, §1.1.3). A single update may raise it by at most
  512 mAh: 3512 is exactly 3000 + 512, consistent with one capped update
  since the last reset (inference). The default *CEDV Gauging Configuration*
  0x102A has FCC_LIMIT clear, so FCC may exceed the design capacity.
- StateOfCharge() = RemainingCapacity() / FullChargeCapacity() (§2.21): 30 %
  is about 1.05 Ah out of 3.5 Ah, a percentage of the learned figure, not of
  the pack.
- OperationStatus()[VDQ] = 1: a qualified learning discharge is in progress.
  While it is, a capacity level reached before its voltage threshold is held
  (§1.1.4): on a pack larger than 3512 mAh the reading would fall to
  *Battery Low %* (7 %) early and stay there until the cell reaches EDV2.
- SEC = unsealed is TI's boot state (*Default Seal* = 0, §6.1 step 2), not a
  configuration somebody chose.

### 7.3 Configuration that does not match this pack

| Parameter | Now | For 2 x 3000 mAh in parallel | Basis |
|---|---|---|---|
| Design Capacity mAh (0x929F) | 3000 (read) | 6000 | TRM §1.1.12; LILYGO launcher writes 6000 |
| Learned Full Charge Capacity (0x929D) | 3512 (read via FCC) | 6000 | TRM §1.1.10: initialise to Design Capacity; LILYGO writes 6000 |
| EDV/CEDV profile (EMF, C0, R0, ..., Fixed EDV0-2, DOD table) | assumed ROM defaults (unread) | unchanged | TI's generic Li-ion 18650 values; fitting the Biltema cells needs discharge data for TI's tool that we do not have |
| Taper Current (0x9201) | assumed 100 mA (unread) | unchanged for now, see below | |
| Charging Voltage (0x91FD) | assumed 4200 mV | unchanged | charger default 4.208 V (SLUSC76C Table 2) |
| CC Gain / Board Offset | assumed defaults | unchanged, unverified | no reference current measured |

Charge-termination risk (to observe, not to change now): the gauge detects a
full charge when the average current stays below *Taper Current* (100 mA) for
two consecutive 40 s periods while capacity still changes by more than
0.25 mAh (TRM §4.4.1). The BQ25896 runs on its defaults under Doors (nothing
writes it; its watchdog returns it to defaults, SLUSC76C §9.3.1), so it
terminates at 256 mA. The current may therefore drop from about 256 mA to
zero without the two windows ever qualifying. If the gauge does not detect
termination, BatteryStatus()[FC] never sets and CSYNC never syncs
RemainingCapacity() to FCC. This can only be settled by watching one ordinary
full charge with the probe on.

LILYGO's two implementations: the Linux launcher writes only Design Capacity
and Learned FCC (6000/6000) through unseal, full access, CONFIG UPDATE and
EXIT_CFG_UPDATE_REINIT. It does this once, when its preferences file is
created, never after a gauge reset, so on volatile data memory it can lapse
silently. The RT-Smart sample carries a full CEDV profile, but labelled for
the T-Embed-CC1101 (1500 mAh), with its apply loop commented out. It is not a
reference for this pack.

### 7.4 Learning and calibration

- Learning: TI says the correct FCC needs a full charge followed by a full
  discharge (§1.1.1). FCC learns only from qualified discharges and moves at
  most -256/+512 mAh per cycle. From 3512 mAh, learning alone would take
  about five qualified full discharges to reach ~6000 mAh, and each is lost
  again on a gauge reset.
- Setting Design Capacity and Learned FCC to the pack's nominal capacity
  removes the need for a deliberate learning discharge. Ordinary deep
  discharges that happen to qualify then refine FCC within those limits.
- Calibration (CC Gain, Board Offset, voltage): no documented field procedure
  is needed for the percentage. Whether LILYGO calibrated the sense path is
  unknown. Current() looked plausible (2.2-2.6 W at 3.79 V) but has not been
  checked against a meter.
- Without a deliberate full discharge, a usable percentage is possible on
  these conditions: both cells are connected; Design Capacity and Learned FCC
  are written to match (and re-written after any gauge reset); and one
  ordinary full charge ends with the gauge detecting termination (FC set),
  so that RM is synced to FCC. Until that charge, the percentage after
  EXIT_CFG_UPDATE_REINIT is an OCV estimate taken under the unit's own load
  (§1.1.10) and may be off by several percent. Accuracy near empty rests on
  TI's generic EDV curve. Accuracy overall rests on the cells really holding
  about 3000 mAh each (labels; cell age unknown).

### 7.5 Proposed change (not done; needs the owner's go)

Preconditions
- P1 (physical, owner): superseded by §8.4, which needs no measurement on
  live battery-holder contacts.
- P2: the write runs in the shell's bus owner, as the probe does: the same
  claim and release, clock-stretch handling, at most two gauge commands per
  second (the CONFIG UPDATE waits are longer anyway), and the keyboard
  breaker. One-shot, triggered by the owner, never automatic in this step.

Backup
- B1: standard-command snapshot (as the probe logs it).
- B2: read the 32-byte data-memory block at 0x929D (Learned FCC, Design
  Capacity, Design Voltage, ... EMF) and the blocks holding Gauging
  Configuration (0x929B), Battery Low % (0x9251), Taper Current (0x9201) and
  Charging Voltage (0x91FD). Selecting a block writes its address to
  0x3E/0x3F; this is a command, not configuration. Verify each block against
  MACDataSum/MACDataLen. Store the bytes under
  `/var/lib/pocketos/battery/gauge-dm-<UTC>.bin` with sha256, plus a host copy.
  Data memory words are big-endian (TRM §6.1 writes 0x04 0xB0 for 1200 mAh);
  standard commands are little-endian.

Write (TRM §6.1, the sequence LILYGO's launcher uses)
- Full access keys 0xFFFF 0xFFFF (the gauge stays unsealed, as found; it is
  not sealed afterwards).
- ENTER_CFG_UPDATE 0x0090, then poll OperationStatus()[CFGUPDATE] for up to
  1.1 s.
- Read-modify-write the 0x929D block: Learned FCC 3512 -> 6000 (0x1770),
  Design Capacity 3000 -> 6000 (0x1770), checksum = 255 - (8-bit sum), then
  the length.
- EXIT_CFG_UPDATE_REINIT 0x0091, then poll [CFGUPDATE] clear. If it does not
  clear, the gauge leaves CONFIG UPDATE by itself after about 240 s.

Verification
- V1: the block read back equals the intended bytes; the checksum was
  accepted.
- V2: DesignCapacity() = 6000, FullChargeCapacity() = 6000, [CFGUPDATE] = 0,
  [INITCOMP] = 1, SEC unchanged.
- V3: no keyboard-controller drops; breaker not tripped.
- V4: one ordinary full charge with the probe on: [FC]/[TCA] set and SOC
  100 % at termination. If not, the Taper Current mismatch (§7.3) gets its
  own proposal.
- V5: after any gauge reset, DesignCapacity() reads 3000 again. That is how a
  later re-apply step would know to act.

Rollback
- The same sequence writing the B2 bytes back (3512/3000), verified the same
  way. A gauge power-on reset also reverts to the ROM defaults (3000/3000),
  but that needs the cells disconnected and loses the learned 3512.
- Nothing here touches the charger, seals the gauge or uses OTP. OTP is
  one-time and is not proposed.

## 8. Capacity operation (prepared 2026-10-10, not run)

`ui/shell/kbd_gauge_cfg.[ch]` implements §7.5 as an owner-triggered
operation in the shell's bus owner. Nothing in the shell starts it by
itself: it is never started because DesignCapacity() reads 3000, and there
is no default capacity.

### 8.1 Interface (on the unit)

```sh
echo "capacity 6000" > /run/pocketos/battery-gauge-request    # explicit mAh, 1..32767
echo "restore /var/lib/pocketos/battery/gauge-dm-<UTC>.txt" > /run/pocketos/battery-gauge-request
cat /run/pocketos/battery-gauge-result                          # running / ok / refused / failed-...
grep 'battery gauge' /var/lib/pocketos/log/shell.log
```

- The shell renames the request to `.taken` before reading it, so a request
  runs at most once, even across a restart.
- A request is refused unless the keyboard base answers.
- The probe stands aside while an operation runs.
- Each run first stores a backup, `/var/lib/pocketos/battery/gauge-dm-<UTC>.txt`.
  The file is written, fsync'd and renamed, then read back and compared. It
  holds the raw 36-byte block read (address echo, 32 data bytes, sum, length)
  plus the standard-command values before the change.
- A restore parses that file and checks the block's address, length and sum,
  and that the summary lines agree with the bytes. It then writes the two
  recorded values back with the same operation, backing up the current block
  first.

### 8.2 Sequence and sources

| Step | Bytes | Source | Class |
|---|---|---|---|
| Refuse if SEALED or already in CONFIG UPDATE | read 0x3A | TRM Table 2-7 (SEC bits 2:1, CFGUPDATE bit 10) | DOCUMENTED |
| FULL ACCESS | Control() 0x00 <- FF FF, twice | TRM §6.1 step 2 (the gauge "boots up in UNSEAL, but not in FULL ACCESS") | DOCUMENTED |
| ENTER_CFG_UPDATE | 0x00 <- 90 00, then poll 0x3A for CFGUPDATE, at most 3 s | TRM §6.1 steps 3-4 ("may take up to 1 second") | DOCUMENTED |
| Select block | 0x3E <- 9D 92 (address low byte first) | TRM §6.1 steps 5-6 | DOCUMENTED |
| Backup read | 0x3E..0x61 (36 bytes) in one read | TRM §2.29-2.31 | DOCUMENTED |
| Backup check | echo 9D 92; length 0x24; sum = 255 - (8-bit sum of address and 32 data bytes) | §2.30 (complement of the sum of ManufacturerAccessControl() and MACData()); §6.1 shows length 0x24 for this block; LILYGO's launcher uses the same sum | DOCUMENTED. §6.1's prose names only 0x40-0x5F for the sum, but its delta method gives the same result either way. If the gauge disagrees, this check fails before any write. |
| Write values | 0x40 <- FCC hi, lo, Design hi, lo | TRM §6.1 step 10 (big-endian: 0x04 0xB0 = 1200) | DOCUMENTED |
| Commit | 0x60 <- sum, 0x24, as one word | §2.31 ("written together as a word"); §6.1 step 13 (data reaches RAM only with correct sum and length) | DOCUMENTED |
| Verify | select again, read 36 bytes; all 32 data bytes must equal the intended block | - | design choice |
| EXIT_CFG_UPDATE_REINIT | 0x00 <- 91 00, poll CFGUPDATE clear, at most 3 s, up to 3 tries | TRM §6.1 steps 14-15 | DOCUMENTED |
| Check | DesignCapacity() 0x3C and FullChargeCapacity() 0x12 equal the values written | TRM §1.1.2 (FCC set from the learned value at initialisation), §2.28 | DOCUMENTED. That 0x929D is the active profile's learned FCC is ASSUMED (CEDV Profile 1 is the default; the table names it "Full Charge Capacity"); this check catches a wrong mapping. |

Addresses: Full Charge Capacity 0x929D and Design Capacity 0x929F, both
I2 mAh (TRM Table 3-2, CEDV Profile 1). Only these two words change. The
other 28 bytes of the block are written back exactly as read.

Reset effects (DOCUMENTED; TRM §3.1, §4.6, §1.1.10):
- Data memory is volatile RAM, so a gauge power-on reset (for example cells
  removed) returns both words, and any learned FCC, to the ROM defaults.
- EXIT_CFG_UPDATE_REINIT re-initialises: FCC is taken from the learned value,
  and the initial RM/SOC is estimated from OCV, here under the unit's own
  load.
- The gauge leaves CONFIG UPDATE by itself after about 240 s if it is not
  told to.
- After the operation the gauge stays in FULL ACCESS until its next reset.
  §6.1 re-seals only a gauge that was sealed, and this operation never seals.

### 8.3 Bounds and failures

- **Pacing:** one bus transaction per tick, at least 600 ms apart (the shell
  ticks once a second). Each transaction claims and releases the bus itself,
  so the keyboard keeps its turns.
- **Time limits:** waits are bounded (enter 3 s, exit 3 s with up to 3 tries,
  backup storage 10 s) and the whole operation has 45 s. A run is about
  20 transactions.
- **Failure before the commit:** leave with EXIT_CFG_UPDATE (0x0092, no
  re-initialisation). Result `failed-unchanged`, because RAM changes only on
  a correct commit.
- **Failure from the commit on:** leave with 0x0091 and read the standard
  commands. Result `failed-after-commit` with the values found, or
  `failed-unknown` if leaving or reading cannot be confirmed (the gauge then
  leaves CONFIG UPDATE by itself after about 240 s).
- **Keyboard failure:** any keyboard-controller failure during the operation
  stops it at once, with no further transaction.
- **Bus:** the bus primitives already wait for clock stretching (at most 8 ms
  per wait) and clock SDA free after a failed transaction.
- **Shell killed mid-way:** the gauge leaves CONFIG UPDATE by itself after
  about 240 s, and a new request is refused while it still reports CFGUPDATE.
- **Never touched:** charger registers, OTP, sealing.

Tests: `tests/kbd_gauge_cfg_test`, 57 checks, run against a model of the MAC
interface. They cover:
- success, with the rest of the block preserved byte for byte, the backup
  taken before any write, the sub-command order and the pacing;
- restoring that backup, byte for byte;
- refusals: range, sealed, already in CONFIG UPDATE, absent gauge, a bus that
  cannot be claimed;
- CONFIG UPDATE never entered, exit never confirmed, a backup never stored or
  not storable;
- a bad sum on the backup read, the data write failing, the sum write
  failing, and a commit that lands wrong;
- a keyboard failure part way;
- tampered backup files, and request parsing.

`tests/kbd_bus_k230_test` adds the block write: acknowledged, refused part
way, and against a held clock.

### 8.4 Physical confirmation (owner), before any write

No measurement on live battery-holder contacts. Each step starts from a
normal Doors shutdown:

1. Shut Doors down normally: hold the power button for about a second and
   choose **Power off** (or System > Power off). Wait until the panel is dark
   and the unit has stopped. Then unplug every USB cable and set the base
   battery switch OFF.
2. Take both cells out. Note whether the base has two holders, and their
   polarity markings.
3. Fit one cell in holder 1 only. Switch ON, start the unit, and let Doors
   boot: did it start from that cell alone? Then shut down normally again
   (step 1).
4. The same with one cell in holder 2 only.
5. Fit both cells, switch ON, and start normally.

If the unit starts from each holder alone, both holders feed the pack. The
pack voltage (3.78-3.79 V, VERIFIED) already rules out series, so the
holders are in parallel and 2 x 3000 mAh applies.

Consequence (DOCUMENTED): taking the cells out resets the gauge. The learned
3512 mAh is lost and the gauge restarts from its ROM defaults (3000/3000);
the probe will show that after step 5.

### 8.5 Execution plan (each step on the owner's go)

1. **Physical check** (§8.4), with a normal shutdown before each cell change.
2. **Deploy:** shell-only deploy of this branch onto unit A's integration
   build (v0.3.5 + vision A/B `36dc5b9` + this branch), with a verified
   backup of the running shell and a RESTORE script, as before. Restart only
   the shell.
3. **Probe on**, two samples. Expect DesignCapacity() 3000 (ROM default after
   the cell change) and no keyboard drops. Probe off.
4. **Write:** `echo "capacity 6000" > /run/pocketos/battery-gauge-request`,
   then wait for `ok` in the result file (about 20-30 s). Record the backup
   path. Check with the probe that DesignCapacity() = FullChargeCapacity() =
   6000, and that the keys still work.
5. **Observe one ordinary full charge** with the probe on. [FC]/[TCA] set and
   SOC at 100 % when the charge terminates means the percentage is usable. If
   FC never sets, the 100 mA taper vs 256 mA charger termination mismatch
   (§7.3) gets its own proposal. Nothing about charging is changed here.
6. **Rollback:** the gauge and the shell are rolled back separately (§8.7).

### 8.6 The exact fields and profile

| Field (TRM Table 3-2 name) | Data-memory address | Type, unit | ROM default | Unit A now |
|---|---|---|---|---|
| Gas Gauging > CEDV Profile 1 > **Full Charge Capacity** | 0x929D-0x929E | I2, mAh, big-endian | 3000 | not read directly; FullChargeCapacity() reads 3512 |
| Gas Gauging > CEDV Profile 1 > **Design Capacity** | 0x929F-0x92A0 | I2, mAh, big-endian | 3000 | DesignCapacity() reads 3000 |

- **The profile (DOCUMENTED):** "CEDV Profile 1" in Table 3-2 is the RAM
  section the gauge runs from. TI's OTP procedure fills "the CEDV Profile 1
  section in RAM" for every profile it programs (§8.3.2), and SET_PROFILE_1..6
  (0x0015-0x001A, §2.2.13) switch the gauge between its pre-programmed
  profiles. Writing 0x929D/0x929F therefore changes the working values,
  whichever profile was loaded.
- **Which profile is loaded on unit A (not read):** CONTROL_STATUS()
  [BATT_ID2:0] reports it (§2.2.1), and nothing has read it. Profile 1 is
  ASSUMED. A gauge reset, or a SET_PROFILE command, reloads the section
  from ROM/OTP and discards whatever was written.
- **The field name (ASSUMED mapping):** TRM §4.9.36 and §1.1.10 call the
  value FullChargeCapacity() is initialised from "Learned Full Charge
  Capacity". Table 3-2 lists it as "Full Charge Capacity" at 0x929D. That
  this entry is the learned value is supported by LILYGO's code: the Linux
  launcher writes it as `BQ27220_ROM_FULL_CHARGE_CAPACITY 0x929D`, and the
  RT-Smart sample's `...CEDVProfile1FullChargeCapacity` names the same
  address. The operation's last check (FullChargeCapacity() equal to the
  written value after EXIT_CFG_UPDATE_REINIT) would expose a wrong mapping
  as `failed-after-commit`.
- **Nothing else:** no other field is written. The other 28 bytes of the
  32-byte block starting at 0x929D are written back as read.

### 8.7 Rollback procedures

Shell and gauge are independent. Rolling back the shell does not change the
gauge, because the gauge keeps its RAM until it resets. Rolling back the
gauge does not change the shell. If both are needed, restore the gauge
first: only the new shell can do a gauge restore.

**Shell (unit A as it is now).** The running shell `00aa3e3` replaced the
Vision A/B shell `bddb56e`. To put that back:

```sh
ssh root@192.168.10.171 /root/rollback-battery-probe/RESTORE.sh
```

- The script first checks the stored copy (sha256 `bb012ede…f05a`) and
  refuses if it does not match.
- It removes the probe flag, stops only `doors-shell`, installs the copy,
  starts it again, and checks the installed file against the recorded hash.
- Verify with `doors shell info` (build `bddb56e`) and that sysd, netd,
  radiod and meshcored kept their pids.
- The script's header names the first probe build, `379a249`; the restore
  works by hash and is unaffected.
- It does not touch the gauge, the Vision models or any setting.

A future deploy of the capacity operation gets its own rollback directory
and script made the same way: verified copy of the shell it replaces, stop
and start of `doors-shell` only.

**Gauge configuration.** Nothing has been written to the gauge, so there is
nothing to roll back today. After a capacity write:

1. Find the backup the write stored. Its path is in
   `/run/pocketos/battery-gauge-result` (`backup=`) and in `shell.log`; the
   file is `/var/lib/pocketos/battery/gauge-dm-<UTC>.txt`.
2. Put it back:
   `echo "restore /var/lib/pocketos/battery/gauge-dm-<UTC>.txt" > /run/pocketos/battery-gauge-request`.
   The keyboard base must be answering, and the shell must be the one with
   the operation.
3. Wait for `ok` in `/run/pocketos/battery-gauge-result`. It shows
   `after-design` and `after-fcc` equal to the backup's
   `design-capacity-mah` and `full-charge-capacity-mah`. The restore stores
   its own backup first, so it can be undone the same way.
4. If the result is `failed-unchanged`, nothing changed and the restore can
   be retried. If it is `failed-after-commit` or `failed-unknown`, read the
   values with the probe before doing anything else.

The ROM defaults (3000/3000) also come back on any gauge reset. That needs
the cells disconnected, which is physical work and not part of this plan.

### 8.8 Status (2026-10-10)

The owner will not do the physical holder check (§8.4). The batteries and
the gauge configuration stay as they are. Nothing has been deployed and
nothing has been written to the gauge. The capacity operation stays
prepared on this branch, unused.
