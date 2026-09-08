# Bring-up session record, 2026-09-07

First physical session with a T-Display K230. This file records **only what was
observed on this host and this board during this session**. It is a checkpoint,
not a conclusion: no source, image, runtime configuration or golden artifact was
changed while writing it, and no evidence class anywhere else in the repository
has been updated on the strength of it.

Where an observation would justify promoting a row in
[T-DISPLAY-K230.md](T-DISPLAY-K230.md), that is stated as a *proposed* change and
left for a separate, deliberate edit.

## Evidence classes

| Class | Meaning here |
| --- | --- |
| VERIFIED | Directly observed this session, on this hardware or host. |
| DOCUMENTED | Stated by vendor material or existing repo docs; not independently confirmed this session. |
| ASSUMED | Inferred from what was observed; plausible, not established. |
| UNRESOLVED | Open. Not observed, or observed ambiguously. |

## 1. Host PC context

| Item | Observation | Class |
| --- | --- | --- |
| Host | `THINXPAD`, user `Dolby` | VERIFIED |
| OS | Microsoft Windows 11 Pro 10.0.26200 | VERIFIED |
| Serial terminal | PuTTY at `C:\Program Files\PuTTY\putty.exe`, already installed | VERIFIED |
| Flash tool | Rufus 4.15 portable present in `Downloads`; **not used** | VERIFIED |
| Card reader | USB, `VID_14CD&PID_1212`, serial `121220160204` | VERIFIED |
| Elevation | Raw disk writes required an elevated PowerShell; the agent session is not elevated and could not perform them | VERIFIED |

### 1.1 Card reader instability (host defect, resolved)

While the laptop ran on battery, the reader repeatedly dropped off the USB bus
with the card inserted. Observed directly: `Get-Disk` returned no USB disk across
six polls over nine seconds, and the PnP device reported `Present: False`, with
**no USB or disk error logged in the System event log**.

| Item | Observation | Class |
| --- | --- | --- |
| Power state during failures | On battery, Balanced scheme | VERIFIED |
| USB selective suspend | Enabled for both AC and DC (`Current AC/DC Power Setting Index: 0x1`) | VERIFIED |
| Root hub power-off permission | `MSPower_DeviceEnable` = `True` on both USB root hubs | VERIFIED |
| Thunderbolt controller | `nhi` entered and exited RTD3 in the same window | VERIFIED |
| Behaviour after AC power + different USB port | 20 of 20 presence polls over 60 s, no drop | VERIFIED |
| Cause is USB power management suspending a reader that does not resume | — | ASSUMED |

Practical consequence for later sessions: **flash on AC power, through a direct
port.** The power settings on this host were not changed; that is the operator's
call and remains available if the fault recurs.

## 2. SD card and artifact provenance

The card was flashed from the existing golden artifact. Nothing was rebuilt.

| Item | Observation | Class |
| --- | --- | --- |
| Card | 58.59 GB (62,914,560,000 bytes), previously holding `Raspberry Pi reference 2026-04-21` | VERIFIED |
| Image written | `out/k230/sysimage-sdcard.img`, 763,363,328 bytes | VERIFIED |
| Source SHA-256 | `bafed837dca4d9279d05047e21c0c8513bee22af8bde184daaa2ebcb97123ad6`, matching `out/k230/SHA256SUMS.txt` | VERIFIED |
| Partition layout after write | p1 at 30 MB / 80 MB; p2 at 128 MB / 600 MB | VERIFIED |
| Readback comparison | 727 of 728 MB byte-identical to the image | VERIFIED |
| Sole differing region | bytes 0 – 1,048,576, outside any partition | VERIFIED |
| Content of that region in the image | zero throughout except the MBR partition table at `0x1b0`–`0x1ff` in sector 0 | VERIFIED |
| Difference is the Windows MBR disk-signature stamp at `0x1b8` | — | ASSUMED |
| The written image is bootable despite that difference | — | ASSUMED (see §4: it booted) |

### 2.1 Build identity of the flashed image

Read from `out/k230/BUILD_INFO.txt`:

```
PocketOS 0.0.1 image for LILYGO T-Display K230
Build UTC : 2026-09-06T07:14:28Z
Defconfig : k230_pocketos_defconfig
Vendor BSP: bb831ab358b66f5bd9a87ecd7c580fee4537492e
SDK       : 22d02c6b6783a57a3aca7eb3160e313e772cb710
PocketOS  : d092a2b
```

`d092a2b` is a documentation-only commit sitting directly on top of code commit
`7b181fa`; the code content of the image is therefore `7b181fa`. VERIFIED from
`git log`.

Note for the record: `BRINGUP_CHECKLIST.md` §1 states the image SHA-256 as
`098e88b1…`. The artifact on disk hashes to `bafed837…` and matches its own
`SHA256SUMS.txt`. The checklist table predates a rebuild of the same source and
is stale. **Not corrected here** — flagged only.

## 3. USB connection and serial console

| Item | Observation | Class |
| --- | --- | --- |
| Connection used | **USB2**, the port marked *Power*, to the PC by USB-C. Carries both power and the CH342K UART | VERIFIED |
| Board USB identity | `USB\VID_1A86&PID_55D2`, composite device, serial `5B92023532` | VERIFIED |
| **COM9 = console** | `USB-Enhanced-SERIAL-A CH342`, interface `MI_00`. Linux login prompt and shell obtained on it at 115200 8N1 | VERIFIED |
| **COM10 = UART3** | `USB-Enhanced-SERIAL-B CH342`, interface `MI_02`. Enumerated, `CM_PROB_NONE`. **The port was never opened this session** | DOCUMENTED |
| Both interfaces healthy | `Status: OK`, `Problem: CM_PROB_NONE` | VERIFIED |
| Console settings | 115200 8N1 | VERIFIED |

`COM10 = UART3` is carried over from `BRINGUP_CHECKLIST.md` and is consistent
with COM9 being the console, but it was not tested and is deliberately **not**
promoted.

### 3.1 Serial driver — correction to the §0 expectation

`BRINGUP_CHECKLIST.md` §0 anticipated either a Windows inbox driver or a manual
WCH CH343SER installation. Neither is quite what happened.

| Item | Observation | Class |
| --- | --- | --- |
| Manual driver installation performed | None | VERIFIED |
| `CH343S64.SYS` in `System32\drivers` **before** the board was connected | Absent | VERIFIED |
| `CH343S64.SYS` present **after** the board was connected | Present, 93,544 bytes | VERIFIED |
| Driver bound to COM9 | Provider `wch.cn`, version `2.1.2025.7`, driver date 2025-07-19 | VERIFIED |
| Driver is a Microsoft inbox CDC driver | **No** — it is WCH's own | VERIFIED |
| Mechanism (staged in the driver store vs fetched on demand) | Not established | UNRESOLVED |

So: **no additional WCH driver had to be installed by hand on this host**, but
the driver in use is WCH's, supplied automatically by Windows. The §0 wording
should eventually reflect that distinction.

## 4. PocketOS 0.0.1 running on physical K230

Verbatim from `pos system info` over the COM9 console:

```
model           Canaan CanMV-K230 with RM69A10 OLED
kernel          Linux 6.6.36 (riscv64)
hostname        canaan
cpus            1
uptime          133 s
load            0.10 0.10 0.04
memory          total 990544 kB, available 927896 kB, free 886720 kB
swap            total 0 kB, free 0 kB
pocketos        0.0.1
vendor-sdk      sdk:v1.2-20260906-091334-dolby-ThinxPad-22d02c6-nncase2.11.0
vendor-sdk      CONF:k230_pocketos
```

| Item | Observation | Class |
| --- | --- | --- |
| PocketOS 0.0.1 runs on physical K230 hardware | `pos` responds from the device | VERIFIED |
| Boot to a usable Linux userspace | Buildroot banner, `canaan login:`, root shell | VERIFIED |
| Kernel | Linux 6.6.36, riscv64 | VERIFIED |
| **Build identity at runtime** | `22d02c6` in the vendor-sdk string is the pinned SDK commit `22d02c6b…` from BUILD_INFO; stamp `20260906-091334` on host `dolby-ThinxPad` corresponds to the same build as the flashed image | VERIFIED |
| **Defconfig identity at runtime** | `CONF:k230_pocketos` — the PocketOS defconfig, not the vendor build | VERIFIED |
| The running system is the artifact that was hash-verified in §2 | — | ASSUMED (strongly supported by the two identity strings above) |
| **Memory total = 990544 kB** | As reported by the running kernel | VERIFIED |
| Interpretation: ~1 GB physical RAM variant | 990544 kB ≈ 967 MiB visible to Linux, consistent with a 1 GB part less reservations; **not** consistent with the 512 MB the DTS declares | ASSUMED |
| **Linux-visible CPU count = 1** | `cpus 1` | VERIFIED |
| Whether a second C908 core exists and runs RT-Smart | Not examined | UNRESOLVED |
| **Runtime model string** | `Canaan CanMV-K230 with RM69A10 OLED` | VERIFIED |
| The panel is physically an RM69A10 | The model string derives from the same DTS that already carried this claim, so it is not independent confirmation | DOCUMENTED |
| A panel exists and displays a working UI | Vendor launcher visible on the panel, reported by the operator | VERIFIED |
| Swap | None configured | VERIFIED |

### 4.1 Proposed repo changes — NOT applied

Recorded so the decision is deliberate rather than incidental:

- [T-DISPLAY-K230.md:28](T-DISPLAY-K230.md:28) currently reads
  *"CONFLICTING, treat as UNVERIFIED"* for RAM, on the conflict between the
  wiki's 1 GB and the DTS's 512 MB. The runtime figure resolves it in favour of
  ~1 GB. **Proposed**, not applied.
- [T-DISPLAY-K230.md:30](T-DISPLAY-K230.md:30), console via CH342K at 115200n8,
  is DOCUMENTED and could become VERIFIED for UART0/COM9 only.
- [T-DISPLAY-K230.md:27](T-DISPLAY-K230.md:27) claims a dual C908. Linux seeing
  one core is not a contradiction, but the observation belongs in the row.
- [T-DISPLAY-K230.md:104](T-DISPLAY-K230.md:104) asks for the CH342K COM port
  numbers on this host: COM9 and COM10.

## 5. Board variant

| Item | Observation | Class |
| --- | --- | --- |
| An nRF52840 base board is fitted | Inferred from the vendor port diagram supplied by the operator: nRF52840 debug USB, Ethernet, HDMI, two MMCX, QWIIC, headphone jack, nRF SWD pins on the expansion header | ASSUMED |
| Board revision from silkscreen | Not inspected | UNRESOLVED |
| LoRa module marking (13A / 16A / T89 / 16E) | Not inspected | UNRESOLVED |
| Second unit | Not present, or not reported | UNRESOLVED |

`FIRST_BOOT.md` §1 asks for photographs and silkscreen readings of both PCBs.
That step has not been done.

## 6. Current device state

| Item | State | Class |
| --- | --- | --- |
| Power and connection | Board powered and connected to the PC over USB2 | VERIFIED |
| Console | Open on COM9, logged in as `root` | VERIFIED |
| Panel ownership | Vendor LILYGO launcher on the panel; the PocketOS shell is disabled by default (`ENABLE=0` in `S90pocketos-shell`) and was **not** enabled | VERIFIED |
| Runtime configuration | **Unchanged.** No file under `/etc/default` or `/etc/pocketos` was written this session | VERIFIED |
| Root password | Advised but not confirmed set | UNRESOLVED |
| Network | Not brought up; no SSH access established | UNRESOLVED |
| Readiness | Connected to the PC and ready for continued bring-up | VERIFIED |

The three `rcS` lines that `BRINGUP_CHECKLIST.md` §3 expects
(`Starting radiod (mock, EU868): OK`, `Starting pocketos-shell: disabled`,
`Starting k230_phone_ui: OK`) were **not** observed: the console was attached
after the board had already booted.

## 7. Open questions still needing hardware validation

Nothing below has been exercised on hardware.

| Area | Checklist ref | Status |
| --- | --- | --- |
| Display ownership / PocketOS shell taking the panel | §6 | UNRESOLVED |
| Touch — ranges, corner mapping, calibration overrides | §3, §6 | UNRESOLVED |
| `/var/lib` persistence across reboot | §3 | UNRESOLVED |
| PocketFleet — deploy, battle, resume across reboot | §8 | UNRESOLVED |
| PocketRadar — contacts, quadrant selection, record persistence | §9 | UNRESOLVED |
| H1 rendering measurement — scope repaint cost, staging on/off | §11 | UNRESOLVED |
| SX1262 — init, RSSI, CAD, TX, RX re-entry, pair link | §5, §12 | UNRESOLVED |
| Crash and recovery — crash reports, supervisor, crash loop | §13 | UNRESOLVED |
| Power-cut test | §15 | UNRESOLVED |

### 7.1 Further items open from this session

| Area | Checklist ref | Status |
| --- | --- | --- |
| Boot chain — U-Boot banner, `bootdelay=1`, kernel within ~2 s | §3 | UNRESOLVED (console attached after boot) |
| `rcS` service start lines and init order | §3 | UNRESOLVED |
| Network and SSH, SSH key on the unit | §3 | UNRESOLVED |
| `pos-hwcheck` and `pos-hwcheck --lora` inventory | §3 | UNRESOLVED |
| DRM connector status and mode list | §3 | UNRESOLVED |
| Wi-Fi chip identification | §3 | UNRESOLVED |
| Clock sanity (`date`) | §3 | UNRESOLVED |
| Theme and settings persistence | §7 | UNRESOLVED |
| Reboot / shutdown / power key behaviour | §14 | UNRESOLVED |
| Second unit for the radio link test | §5 | UNRESOLVED |

## 8. Tooling note

`platforms/k230/scripts/flash_card.ps1` and `compare_card.ps1` were written
during this session as host-side helpers. They are untracked and are not part of
the image or the build.

`flash_card.ps1` reports `VERIFY FAILED` on a correctly written card, because its
readback hash covers the MBR sector that Windows stamps with a disk signature the
moment the partitions appear. The card in §2 is good; the tool's verdict was
wrong. **Not fixed here** — deliberately left, since fixing it is a code change
and this checkpoint is documentation only.

## 9. Summary

PocketOS 0.0.1 booted and ran on physical K230 hardware for the first time, from
an image whose bytes on the card were confirmed against the golden artifact, with
build and defconfig identity confirmed again from the running system. The console
path is established. Nothing beyond that has been tested, and no evidence class
in the hardware baseline has been changed.

## 10. Continuation in the main engineering session (2026-09-07, 20:10 to 20:20)

The console on COM9 was driven from the engineering session (PowerShell
`System.IO.Ports.SerialPort`, 115200 8N1, raw logs kept by the session). No
configuration was changed, nothing was rebuilt, the radio was not touched.

### 10.1 State found on the device before the reboot

The board had been used by hand after the §6 record was written: uptime was
3949 s at 20:10, and the runtime configuration differed from §6.

| Item | Observation | Class |
| --- | --- | --- |
| `/etc/default/k230_phone_ui` | `ENABLE=0` (written 00:03:03 boot-relative) | VERIFIED |
| `/etc/default/pocketos-shell` | `ENABLE=1` (written 00:03:14 boot-relative) | VERIFIED |
| `/etc/default/radiod` | absent; radiod on the mock backend, state `rx`, EU868 defaults | VERIFIED |
| `/etc/pocketos/settings.conf` | `theme=ice`, `display_mode=normal` (file from the image) | VERIFIED |
| Processes | `pos-supervise radiod`, `radiod --backend mock`, `pos-supervise pocketos-shell`, `pocketos-shell`; no `k230_phone_ui` | VERIFIED |
| Shell state | `pos shell info`: `current: home`, theme ice, mode normal, display 568 x 1232, backend drm; `pos app list`: radio, system, fleet, radar | VERIFIED |
| Panel ownership | PocketOS shell owns the panel; the persistent switch has survived at least seven boots (seven `shell.* listening` lines in `shell.log`) | VERIFIED |
| App use across reboots | `shell.log`: Fleet resumable match read back at Officer turn 1, then Recruit turn 1, 13 and 27 on successive boots; Radar `best score 252 over 2 run(s)` read back; radio and system opened | VERIFIED |
| `/var/lib/pocketos` persistence | `fleet/save.v1` (898 B) and `radar/record.v1` (30 B) present and read back after reboots | VERIFIED |
| Log persistence | `/var/lib/pocketos/log` holds `shell.log` (3337 B, several boots), `radiod.log`, `supervise-radiod.log`; `pos logs --crashes`: none | VERIFIED |
| DRM connector | `/sys/class/drm/card0-DSI-1`: `connected`, modes `568x1232` | VERIFIED |
| Touch device | `event1 = goodix_ts`; driver log `[DT]x:1060, y:2400`; `event0 = K230 PMU Power Key` | VERIFIED |
| Device nodes | spidev0.0, gpiochip0/1, i2c-0/1, ttyS0..3, video0..4, dri/card0, mmcblk1p1/p2; no rtc, no usb-serial | VERIFIED |
| Network devices | eth0 (USB `10/100 LAN`, RTL8152 class), wlan0 and wlan1 down, SDIO device `0xf179` | VERIFIED |
| Backlight and thermal | `rm69a10` backlight at 254; `canaan_thermal_zone` 48.6 C at idle | VERIFIED |
| Power supply class | empty | VERIFIED |
| Clock | `date` = 1970-01-01; no RTC. Log timestamps cannot order events across boots | VERIFIED |
| Root filesystem | 574 MB, 400.6 MB used, 131.6 MB free (75 %); the 175 MB free estimated from the image was optimistic | VERIFIED |
| Mounts | `/` ext4 rw, `/boot` ext4 (mmcblk1p1), `/tmp` and `/run` tmpfs | VERIFIED |
| `shell.stdio.log` | 0 bytes on every boot: no LVGL or DRM message from the shell; note that the shell's own `printf` lines are block-buffered under nohup and never flushed, so their absence is not evidence | VERIFIED |
| Operator observation (this session) | PocketOS launcher correctly oriented in portrait, status bar at the top, tiles displayed correctly, nothing mirrored; touches landed where expected in the launcher, PocketFleet and PocketRadar, no noticeable offset | VERIFIED (operator) |
| Touch calibration override needed | None: LVGL's evdev auto-calibration from the driver's 1060 x 2400 range is sufficient | VERIFIED (operator use) |

### 10.2 Reboot captured with the console held (20:16)

`reboot` issued over COM9 at t = 0.8 s; every byte was captured.

| Item | Observation | Class |
| --- | --- | --- |
| rcK order | `Stopping k230_phone_ui: not running`, `Stopping pocketos-shell: OK`, `Stopping radiod: OK`, then the vendor services; rootfs re-mounted read-only; `reboot: Restarting system` at t = 4.3 s | VERIFIED |
| Supervisor stop | `supervise-pocketos-shell.log` and `supervise-radiod.log` each gained a `stopped` line at 01:11:02 boot-relative | VERIFIED |
| U-Boot | `U-Boot SPL 2022.10 (Sep 04 2026 - 18:36:22 +0200)`, LPDDR4 training messages, `U-Boot 2022.10`, `Model: kendryte k230 canmv v3`, **`DRAM:  1 GiB`**, MMC 0 and 1, environment loaded from MMC | VERIFIED |
| Boot logo | `ext4load mmc 1:1 0x1f000000 /logo.xrgb`, `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4` | VERIFIED |
| Countdown | `Hit any key to stop autoboot:  1` at t = 6.5 s, then `0` and `ext4load ... lcd_dtb`, Image (19,084,288 bytes), DTB (60,191 bytes), kernel image created 2026-09-06 07:13:52 UTC | VERIFIED |
| Kernel command line | `root=/dev/mmcblk1p2 loglevel=8 rw rootdelay=4 rootfstype=ext4 console=ttyS0,115200 earlycon=sbi` | VERIFIED |
| Kernel | `Starting kernel ...` at t = 8.1 s; OpenSBI v1.4; `Linux version 6.6.36 (dolby@ThinxPad)`, Xuantie-900 toolchain; `Machine model: Canaan CanMV-K230 with RM69A10 OLED` | VERIFIED |
| Display driver at boot | canaan-vo and canaan-mipi-dsi bound at 1.2 s, `preserving boot splash`, `skip DRM fbdev setup to preserve boot splash`, DRM registered on minor 0 | VERIFIED |
| Touch driver at boot | Goodix core v1.3.3, `goodix,nottingham`, reset-gpio 536, irq-gpio 535, `goodix_ts_core probe success` at 2.8 s; `goodix_cfg_group.bin` firmware load fails (-2) three times and the driver continues | VERIFIED |
| rcS order | mdev, seedrng, syslogd, klogd, sysctl, dbus, canaan_isp (vvcam/isp probes; `vo_init: not found`), bluetoothd (disabled), `S40k230_pocketos_defconfig` (aic8800 and btusb modules load), network (udhcpc: no lease), adb_mtp (disabled), sntp (FAIL: no name resolution), ntpd, crond, sshd, telnetd, **`Starting radiod (mock, EU868): OK`**, **`Starting pocketos-shell: OK`**, showversion banner, **`Starting k230_phone_ui: disabled (/etc/default/k230_phone_ui)`** | VERIFIED |
| Shell takes the panel | `canaan login:` at t = 25.3 s (kernel 16.9 s); at kernel 16.86 s the DSI driver logs `DSI mode: clock=49500 hdisplay=568 hsync_start=668 hsync_end=708 htotal=748 vdisplay=1232 vsync_start=1236 vsync_end=1252 vtotal=1268`, `lanes=2 bpp=24`, auto-PHY 594000 kHz; that is the shell's DRM mode set | VERIFIED |
| Post-boot state | uptime 19 s at login; `pos shell info` current home, theme ice, backend drm; the four supervised processes back (pids 221, 227, 238, 240); `save.v1` and `record.v1` unchanged | VERIFIED |
| Boot time | reboot command to login prompt about 24.5 s; power-on equivalent (U-Boot SPL to login) about 20 s | VERIFIED |
| Kernel messages worth knowing | `dw_spi_mmio 91584000.spi: error -ENXIO: IRQ index 9 not found` (SPI0, the LoRa bus, runs without an IRQ); `aht10: probe of 0-0038 failed with error -121` (no AHT20 answering on I2C0); `cfg80211: failed to load regulatory.db`; during shutdown `mount: mounting /dev/mmcblk1p1 on /boot failed: Device or resource busy` and `/dev/mmcblk1p1: Can't open blockdev` from a vendor stop script | VERIFIED |
| Consequence of the SPI0 IRQ message for spidev | Not established; the vendor launcher uses the same node | UNRESOLVED |
| nRF52840 base board fitted (§5) | The AHT20 probe failure argues against a base board with its I2C sensor being connected, or the sensor sits on another bus | UNRESOLVED |
| Second CPU core | U-Boot and Linux report nothing about a second core; not examined | UNRESOLVED |

### 10.3 Proposed repo changes from §10 — NOT applied

- T-DISPLAY-K230.md RAM row: U-Boot prints `DRAM: 1 GiB` and Linux sees 990544 kB; the DTS's 512 MB node is fixed up at boot as the row already suggests. Resolve to 1 GB, VERIFIED.
- T-DISPLAY-K230.md display and touch rows: DRM mode 568x1232 on DSI-1 and the Goodix range 1060 x 2400 are now VERIFIED at runtime; the RM69A10 identity itself stays DOCUMENTED (the model string comes from the DTS).
- BRINGUP_CHECKLIST.md §1: image SHA-256 must read `bafed837…`; the free-space figure is 131 MB, not 175 MB.
- KNOWN_ISSUES.md: the shell's `printf` diagnostics never reach `shell.stdio.log` because stdout is block-buffered under the supervisor; consider `setvbuf` or pocketlog for those lines (code change, later).
- BRINGUP_CHECKLIST.md §3: `Starting pocketos-shell: OK` precedes the launcher line at boot, as recorded above.

### 10.4 Position after §10

Done: checklist §1, §2, §3 (except network/SSH, `pos-hwcheck`, and the evtest
corner readings), §4, §6 (by operator use, oriented and touch-correct), §7
partly (stored theme reloads across reboot; live switching not exercised), §8
and §9 persistence rows (by log evidence; the play-through observations rest
on the operator's report). Not started: §5 and §12 (SX1262), §10, §11, §13,
§14 beyond the reboot, §15.

## 11. `pos-hwcheck` inventory (20:21, read-only, no `--lora`)

Run from the held console; the full report is kept as
`hwcheck-unitA/report-2026-09-07.txt` beside this file (460 lines, ANSI
stripped). `dmesg.txt` (430 lines) stays on the device under
`/root/hwcheck/hwcheck-19700101_000523/`. Nothing was changed on the device.

### 11.1 Findings

| Item | Observation | Class |
| --- | --- | --- |
| CPU | one hart, `uarch thead,c908`, `rv64imafdcv_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt`, mmu sv39, mvendorid 0x5b7 | VERIFIED |
| Kernel build | `6.6.36 #2 SMP Fri Sep 4 16:42:03 CEST 2026`; os-release `v1.2-11-g22d02c6-dirty`, Buildroot 2025.02.1 | VERIFIED |
| Card and partitions | mmcblk1 61,440,000 blocks (58.6 GB); p1 81,920 KB (`/boot`, 69.5 MB, 31 % used); p2 614,400 KB (`/`, 574 MB, 75 % used). Not resized to the card | VERIFIED |
| tmpfs sizing | `/tmp`, `/run`, `/dev/shm` 483.7 MB each (half of RAM) | VERIFIED |
| Device nodes | as §10.1, plus `/dev/watchdog` and `/dev/watchdog0` present; no `/dev/rtc*`, no framebuffer (`fb0` absent, DRM only) | VERIFIED |
| GPIO controllers | gpiochip0 `9140b000.gpio` and gpiochip1 `9140c000.gpio`, 32 lines each (matches the GPIO N/32 mapping the radiod HAL assumes) | VERIFIED |
| GPIO consumers (chip0) | line 21 output `reset`; line 22 output `dsi_reset` (panel reset GPIO22); line 23 input `ts_irq_gpio` (touch IRQ GPIO23); line 24 output `ts_reset_gpio` (touch RST GPIO24); line 25 output `backlight_gpio` (panel enable GPIO25). Everything else unclaimed input | VERIFIED |
| GPIO consumers (chip1) | line 20 (GPIO52) is an output with no consumer; all other lines unclaimed inputs, including line 12 (GPIO44 LoRa power) and line 13 (GPIO45 Wi-Fi enable) | VERIFIED |
| LoRa control lines | GPIO 5, 19, 20 (chip0) and 44 (chip1) are free inputs: nothing holds them while radiod runs the mock backend | VERIFIED |
| SPI | `spi0.0` = `spi:dh2228fv` (spidev) on `91584000.spi`; kernel thread `spi0` present | VERIFIED |
| I2C controllers | `i2c-0` = `91409000.i2c`, `i2c-1` = `91408000.i2c`, both DesignWare | VERIFIED |
| I2C-0 devices | `0-0037` GC2093 camera answers at 0x37; `0-0038` AHT20 node exists but nothing answers at 0x38 (`i2cdetect` shows only 0x37; driver probe failed -121 at boot) | VERIFIED |
| I2C-1 devices | `1-005d` Goodix touch, driver-bound (`UU` at 0x5d); **an unlisted device answers at 0x3b** | VERIFIED |
| Identity of the 0x3b device | The LT9611 HDMI bridge the schematic places on the touch I2C lines uses 0x3b when its address pin is high; consistent with the HDMI port on this unit, and with the separate `k230-canmv-rm69a10-hdmi.dtb` in the image | ASSUMED |
| Camera | GC2093 present on I2C-0 and enumerated by vvcam (`sensor=gc2093`), video1..3 = vvcam, video0 = VPU (`mvx`), video4 = non-AI 2D | VERIFIED |
| Wi-Fi | SDIO `mmc0:0001:1` vendor 0x024c device 0xf179, driver `rtl8189fs`, interfaces wlan0 (88:3b:dc:b7:9e:c7) and wlan1 (locally administered twin, same driver), both down. This is the Realtek RTL8189FTV | VERIFIED (chip family), DOCUMENTED (exact part marking) |
| Ethernet | `eth0` = USB 0bda:8152 Realtek 10/100 LAN on driver `r8152`, MAC 00:e0:4c:3a:5e:d0, `NO-CARRIER` (no cable), udhcpc waiting | VERIFIED |
| Bluetooth | no HCI device; `aic_btusb`, `aic8800_fdrv`, `aic_load_fw` and `8723ds` modules are loaded without hardware (vendor boot script), harmless | VERIFIED |
| USB | two DWC OTG controllers (usb1, usb2); the LAN adapter on bus 2; nothing else on the device side | VERIFIED |
| Audio | ALSA card 0 `K230_I2S_INNO`, `9140e000.inno_codec` playback and capture | VERIFIED |
| Thermal | `canaan_thermal_zone` 49.8 C during the inventory (48.6 C idle earlier) | VERIFIED |
| Power supply | no `power_supply` device: no battery gauge or charger visible on this unit | VERIFIED |
| Processes | `isp_media_server` (vendor), udhcpc on eth0, ntpd, sshd, telnetd, the two supervisors, radiod (mock), pocketos-shell; no vendor launcher | VERIFIED |
| Memory at idle | MemTotal 990544 kB, MemAvailable 931408 kB with the shell at home | VERIFIED |

### 11.2 Deviations from the existing hardware documentation

- **T-DISPLAY-K230.md "HDMI: optional LT9611 bridge"**: a device answers at
  0x3b on the touch bus, so the bridge appears to be populated on this unit
  (identity ASSUMED). The row should say "present on unit A (0x3b), unused
  by PocketOS" once confirmed, for example by probing its chip-ID register
  or by booting the HDMI DTB, neither of which is part of this bring-up.
- **T-DISPLAY-K230.md "Sensors on main board: none; AHT20 ... on the base
  boards on I2C4"**: consistent with the DTS (the AHT20 node sits on i2c4
  next to the camera, Linux `i2c-0`), and the sensor is absent on this unit.
  Together with the empty `power_supply` class this says **no base board
  with an AHT20, BQ25896 or BQ27220 is connected**; the §5 assumption of an
  nRF52840 base board rests only on the port diagram and should be
  downgraded to UNRESOLVED until the silkscreen is read.
- **Wi-Fi enable GPIO45**: DOCUMENTED as the enable line, but no kernel
  consumer claims gpiochip1 line 13 and the driver is up regardless. Either
  the line defaults high or the SDIO power sequence handles it elsewhere.
  UNRESOLVED; harmless.
- **GPIO21 (`reset` consumer) and GPIO52 (bare output)** are not in the
  document. GPIO21 is most likely the camera reset (vvcam logged a reset
  pulse at boot); GPIO52 is the keyboard-backlight PWM pin of the optional
  base board, left as a GPIO output by the vendor DTS. Both ASSUMED.
- **RAM**: U-Boot `DRAM: 1 GiB`, Linux 990544 kB. The document's
  "CONFLICTING" row resolves to 1 GB (VERIFIED), as proposed in §10.3.
- **Watchdog**: `/dev/watchdog0` exists and nothing feeds it; the review's
  M9 note stands (kernel `watchdogd` thread only).
- Everything else the document states for the display, touch, LoRa SPI,
  camera, Ethernet, SDIO Wi-Fi, audio and GPIO numbering matches what the
  device reports.

### 11.3 Position after §11

Checklist §3 now lacks only network/SSH (needs a cable or Wi-Fi credentials)
and the evtest corner readings; `pos-hwcheck --lora` stays for the SX1262
step (§5). The reboot (§10.2) has already been done with the console held.

## 12. Network and SSH (20:27 to 20:36)

Ethernet through the board's USB LAN adapter to the bench switch; the PC is
on the same subnet over Wi-Fi (192.168.10.186). Nothing was persisted on the
device: the lease came from the vendor's own background `udhcpc` started by
S40network, no file was written.

| Item | Observation | Class |
| --- | --- | --- |
| Link | `eth0: carrier on` at kernel 649 s once the cable was in; `carrier off` at 805 s; `carrier on` again at 966 s after the operator reseated the cable | VERIFIED |
| Cause of the 805 s carrier loss | The operator reports the switch performed a software update and rebooted during the test. The board logged a clean link loss and regain with no USB or r8152 error, the adapter kept its address and route, and the lease survived: consistent with the switch interruption, not with a fault on the K230 or its adapter | ASSUMED (consistent), the switch reboot itself is reported by the operator |
| DHCP | 192.168.10.157/24, gateway 192.168.10.1, obtained by the boot-time `udhcpc -b` (pid 186); no manual DHCP needed after the link returned | VERIFIED |
| MAC address | r8152 logs `Invalid ether addr 00:00:00:00:00:00` then `Random ether addr 00:e0:4c:3a:5e:d0`: the adapter has no burned-in MAC, so **the address and therefore the DHCP lease can change on every boot**. Note the IP after each reboot | VERIFIED |
| SSH | `ssh root@192.168.10.157` runs commands with no key and no password (`PermitRootLogin yes`, `PasswordAuthentication yes`, `PermitEmptyPasswords yes`, root has an empty password) | VERIFIED |
| SSH policy | Anyone on the LAN can log in as root. Unchanged this session by instruction; `passwd` on the console is the operator's call, then a key for `deploy.sh` and `lora_pair_test.sh` | VERIFIED (state), decision pending |
| scp | `sftp-server` present, `scp` present; OpenSSH's default SFTP mode stalled once during the link loss, `scp -O` (legacy protocol) works | VERIFIED |
| Evidence pulled | `hwcheck-unitA/device-hwcheck-19700101_000523/` (report.txt 18,712 B and dmesg.txt 30,089 B, SHA-256 of dmesg `0fd661cc…adf92`) | VERIFIED |
| Stale session | One `sshd-session: root@notty` from the interrupted transfer remained ESTABLISHED after the link came back; harmless, it times out | VERIFIED |

### 12.1 Position after §12

Checklist §3 is complete apart from the evtest corner readings and the SSH
key (the key needs a root password decision first). Everything read-only
that the checklist puts before the shell takeover has now been done, and the
takeover itself already happened by hand (§10.1).

### 12.2 One more fact from the pulled dmesg

`Memory: 465812K/1048576K available (... 524288K cma-reserved)`: the vendor
DTS reserves 512 MB of the 1 GB as CMA for the video, ISP and VPU blocks.
MemTotal still reports 990544 kB because CMA pages count as movable memory,
but a large CMA allocation by a vendor media path would take them back. With
the PocketOS shell at home, MemAvailable was 931 MB (§11.1). VERIFIED.
Also: `mmc1: sdhci: Timeout: 0x00000000` at 2.1 s is the SD host register
dump printed at probe, not a timeout event. VERIFIED (dmesg), harmless.

## 13. Settings and theme persistence, checklist §7 (20:41, over SSH)

Driven through the shell's own `shell.theme` API with `pos shell theme`; the
settings file was left exactly as found (`theme=ice`, `display_mode=normal`).
No reboot in this step: the board's USB LAN adapter takes a random MAC per
boot (§12), so a reboot can move the IP; the shell was restarted through its
init script instead, which exercises the same read-at-start path the boot
log already showed (`appearance: ... (settings loaded)` on every boot in §10).

| Step | Result | Class |
| --- | --- | --- |
| `pos shell theme brass outdoor` | `theme brass mode outdoor`, rc 0; `/etc/pocketos/settings.conf` rewritten to `theme=brass`, `display_mode=outdoor` (mtime 2026-09-07 18:41:42 UTC) | VERIFIED |
| `pos shell theme brass night` | rc 0, `display_mode=night` stored | VERIFIED |
| `/etc/init.d/S90pocketos-shell restart` | `Stopping pocketos-shell: OK`, `Starting pocketos-shell: OK`; new supervisor (pid 748) and shell (pid 754); `pos shell info` reports `brass` / `outdoor` / `current: home`; log `appearance: theme brass mode outdoor (settings loaded)` | VERIFIED |
| Shell re-acquires DRM after a restart | `shell.* listening` and `pos shell info` answer within 5 s of the restart; the panel content after the restart is for the operator to confirm | VERIFIED (service), UNRESOLVED (panel) |
| `pos shell theme neon` | `theme ice mode normal (fallback)`, rc 3, `pos: unknown theme 'neon', using ice`; shell log WARN; stored file still `brass` / `outdoor` | VERIFIED |
| `pos shell theme brass dusk` | `theme ice mode normal (fallback)`, rc 3: an unknown mode falls back to ice **and** normal, as DS §8 specifies (both reset), stored file untouched | VERIFIED |
| `pos shell theme ice normal` | rc 0, file back to `theme=ice`, `display_mode=normal` | VERIFIED |
| Crash reports after the sequence | none; the only WARN lines are the two intended fallbacks | VERIFIED |
| Device clock | The settings file's mtime is 2026-09-07 18:41:42 UTC: with Ethernet up, `ntpd` synchronised the clock, so timestamps from here on are real. Before the cable, everything was 1970 | VERIFIED |

Not done: a full reboot for §7 (see the IP note above; the boot log in §10
already proves the file is read at boot), and the visual side: whether the
panel showed brass/outdoor, night and the fallback to ice while the commands
ran, and whether it came back correctly after the service restart. Operator
input needed for those rows.

### 13.1 Position after §13

§7 done on the data side. Remaining: §10 (reduced motion, theme matrix
photographs), §11 (H1 measurement), §13 (crash handling), §14 (poweroff,
power key), §15 (power cut, last), and the radio sections §5 and §12.

## 14. PocketRadar H1: scope repaint cost, checklist §11 (18:45 to 18:57 UTC)

Three passes of the same workload: PocketRadar opened through `pos app start
radar`, the operator tapped BEGIN SCAN and kept a scan running on the panel,
and the session sampled 60 s over SSH every 5 s: whole-system CPU from
`/proc/stat` deltas (one core), the shell's own CPU from `/proc/<pid>/stat`
(utime + stime), load average, `/sys/class/thermal/thermal_zone0/temp`, and
the shell's RSS. Raw sample files: `hwcheck-unitA/h1-2026-09-07/`.
Configuration per pass was read back from the live shell's environment and
from the files. Everything was restored afterwards from byte-exact backups
(md5 identical, original mtimes kept) and the shell restarted; no crash
report was produced at any point.

| Pass | Configuration (read back) | System CPU busy | Shell CPU | Temp | Load (1 min) at end | Shell RSS | Operator |
| --- | --- | --- | --- | --- | --- | --- | --- |
| idle reference | launcher at home, then Radar open but not scanning | 1 % | ~0 % | 51 to 52 C | 0.00 | 12.0 MB | |
| 1 default | `ENABLE=1`, `K230_LVGL_DRM_STAGING=1`, no `reduced_motion` | 17 % (60 s), 16 % (30 s repeat) | not captured in this pass (sampler bug, fixed before pass 2); the system figure is the shell, nothing else ran | 51.3 to 52.5 C, flat | 0.62 | 12.0 MB | "scanning, looks good" |
| 2 reduced motion | `reduced_motion=1` appended to `/etc/pocketos/settings.conf` with the shell stopped; staging on | 6 % | 6 % | 52.2 to 52.5 C, flat | 0.09 | 12.5 MB | static sweep line, as designed |
| 3 staging off | `printf 'ENABLE=1\nK230_LVGL_DRM_STAGING=0\n' > /etc/default/pocketos-shell`, shell restarted, `K230_LVGL_DRM_STAGING=0` in its environment; `reduced_motion` removed | 75 % | 75 % | 52.5 to 54.0 C, slight rise | 0.80 | 9.7 MB | "looks good, no flickering" |

Per-sample detail (pass 1): `top` showed 18 to 20 % usr with occasional 9 to
10 % sys, two samples at 10 to 11 %; pass 3 ranged 66 to 90 % usr. No I/O
wait in any pass.

### 14.1 Classification

| Statement | Class |
| --- | --- |
| The Radar scan screen costs about 16 to 17 % of the single C908 core in the default configuration (staging on, motion on), of which about 10 points are the sweep animation (pass 1 minus pass 2) and about 5 points the moving contacts and the 20 Hz tick | VERIFIED (60 s windows) |
| Reduced motion cuts the scan-screen cost to about 6 % | VERIFIED |
| With the vendor staging buffer disabled the same workload costs about 75 % of the core, four and a half times the default, while looking the same to the operator | VERIFIED (cost), operator (appearance) |
| Why staging off is so much slower: without staging LVGL renders straight into the DRM dumb buffer, which the kernel maps uncached or write-combined; the software renderer's read-modify-write for arcs, blending and the dirty-area copy between the two buffers is then paid on uncached memory, whereas staging renders into cached RAM and copies whole frames once | ASSUMED (consistent with the numbers and the vendor patch text; not profiled) |
| The staging default (`K230_LVGL_DRM_STAGING=1` in S90) is the right one for this panel; the review's M5 concern about extra copies with staging on is not the dominant cost | VERIFIED by comparison |
| Temperature effect of a 60 s scan: none measurable in passes 1 and 2, about 1 C in pass 3 | VERIFIED (60 s only; the checklist's 10-minute soak was not run) |
| The 20 Hz tick holds in the default configuration | ASSUMED: at 17 % load there is headroom; not instrumented (no frame timing in the shell) |
| No flicker or tearing with staging on or off during a scan | VERIFIED (operator) |
| Memory: the shell's RSS is 12 MB with staging and 9.7 MB without; the two staging buffers account for the difference | VERIFIED (RSS), ASSUMED (attribution) |

### 14.2 Comparison and consequence

Pass 1 versus 2 says the sweep is the expensive part of the scope, pass 1
versus 3 says the render target matters more than anything the app does.
Nothing here calls for a design change: the KNOWN_ISSUES H1 entry's "measure
before changing the design" is answered with 17 %, and the order of knobs
it lists (tick rate, sweep bands, scope size) stays hypothetical. What the
numbers do settle is that `K230_LVGL_DRM_STAGING=1` must stay the default
and that the bench override to 0 is a diagnostic only.

Not measured: frame time and the tick rate directly (the shell has no
timing instrumentation, review M5), the 10-minute thermal soak, and
PocketFleet Battle with its two grid timers.

### 14.3 State after §14

`/etc/default/pocketos-shell` = `ENABLE=1` (md5 53d71117…, mtime unchanged),
`/etc/pocketos/settings.conf` = header + `theme=ice` + `display_mode=normal`
(md5 bf7f97ef…, mtime unchanged), shell restarted with staging 1, at home,
radar record updated by the operator's runs during the passes (21 runs, best
252). Golden image untouched, nothing rebuilt, radio untouched.

## 15. Crash handling, stage 1: one deliberate SIGSEGV to radiod (19:00:33 UTC)

Over SSH: `kill -SEGV` to radiod (pid 238), then `pos radio info` polled
every 100 ms. Raw transcript: `hwcheck-unitA/h1-2026-09-07/crash_stage1*.txt`
(pulled with the H1 files). The switch restarted once more right after the
test (`carrier off` at 2676 s, `on` at 2737 s), which delayed the follow-up
check but did not touch the board.

| Item | Observation | Class |
| --- | --- | --- |
| Crash report | `crash-radiod-1788807633.txt` (400 B): process radiod, pid 238, signal 11, backtrace of seven frames through `__poll`, the server loop and `main` (addresses only, no symbols: the binary is stripped) | VERIFIED |
| Supervisor | `supervise-radiod.log`: `exited rc=139 after 1788807617s (restart 1, backoff 1s)`; the absurd duration is the clock jumping from 1970 to 2026 (NTP) between start and exit, harmless | VERIFIED |
| radiod recovery | `radio.info` answered again about 1.1 s after the signal (1 s backoff plus start); new pid 1452, state `rx`, `listening on /run/pocketos/radiod.sock` logged at 19:00:34.303 | VERIFIED |
| Crash-loop marker | none (one restart) | VERIFIED |
| **The shell died with radiod** | `supervise-pocketos-shell.log`: `exited rc=141 after 214s (restart 1, backoff 1s)` at the same second. 141 = 128 + 13 = **SIGPIPE**: the shell was writing its once-per-second `radio.status` poll into radiod's socket when the peer vanished. No shell crash report, because SIGPIPE is not one of the signals the pocketlog handler catches, and the shell (unlike radiod) never ignores SIGPIPE | VERIFIED (exit code and timing), root cause ASSUMED from the code: `ui/shell/shell.c` installs no SIGPIPE handling and `core/pocketipc/pocketipc.c` writes without `MSG_NOSIGNAL`, while `services/radiod/main.c` does `signal(SIGPIPE, SIG_IGN)` |
| Shell recovery | Restarted by its supervisor after 1 s: new supervisor pid 1375, shell pid 1473, `appearance: theme ice mode normal (settings loaded)` and `shell.* listening` at 19:00:34.99; `pos shell info` at home, ice/normal | VERIFIED |
| UI / status chip | The panel therefore blanked and came back within about two seconds (shell restart), rather than showing the `--` chip for a second as designed. Operator confirmation of what the panel did is pending | VERIFIED (restart), UNRESOLVED (what was visible) |
| Data | Fleet save and Radar record untouched; settings unchanged | VERIFIED |

### 15.1 Finding for the platform (not fixed here)

A radiod crash takes the shell down with it. The designed path (transport
failure, chip `--`, reconnect on the next poll) never runs because the write
to the dead socket raises SIGPIPE in the shell first. The fix is small and
belongs to the next code pass: ignore SIGPIPE in the shell (as radiod already
does) or send with `MSG_NOSIGNAL` in pocketipc, plus a host test that kills
radiod under a running shell. Until then any radiod crash or `S60radiod
restart` while the shell runs costs one shell restart (about 2 s, state
preserved through the settings file and the app stores). Severity: HIGH for
robustness, LOW for data (nothing is lost). Related review item: H3.

### 15.2 Position after §15

Stage 1 done. Stage 2 (deliberate SIGSEGV to the shell for its own crash
report and restart) and stage 3 (crash loop on radiod) not started.

## 16. Crash handling, stage 2: one deliberate SIGSEGV to the shell (19:05:39 UTC)

Over SSH: `kill -SEGV` to the shell (pid 1473) with radiod (pid 1452)
running; `pos shell info` polled every 100 ms. Transcript:
`hwcheck-unitA/h1-2026-09-07/crash_stage2.txt`, report copied beside it.

| Item | Observation | Class |
| --- | --- | --- |
| Crash report | `crash-shell-1788807939.txt` (494 B): process shell, pid 1473, signal 11, eight frames: the handler, `__vdso_rt_sigreturn`, `clock_nanosleep`, `nanosleep`, `usleep`, `main+0x39c`, `__libc_start_main`, `_start`. The signal landed in the main loop's `pocketos_platform_sleep_ms()`, which is where the shell spends most of its idle time | VERIFIED |
| Supervisor | `supervise-pocketos-shell.log`: `exited rc=139 after 305s (restart 1, backoff 1s)` | VERIFIED |
| Restart timing | `shell.info` answered again about 1.2 s after the signal (1 s backoff plus start); new shell pid 1562; `appearance: theme ice mode normal (settings loaded)` and `shell.* listening` at 19:05:40.3 | VERIFIED |
| Home screen and stored settings | `pos shell info`: `current: home`, theme ice, mode normal, reloaded from `/etc/pocketos/settings.conf` (md5 unchanged) | VERIFIED |
| radiod unaffected | still pid 1452, state `rx`, no restart logged | VERIFIED |
| Persistence intact | `fleet/save.v1` and `radar/record.v1` md5 identical before and after; after the restart Fleet reports `resumable match ... Recruit turn 31` and Radar `best score 252 over 25 run(s)` when opened through the API | VERIFIED |
| Crash-loop marker | none (single restart) | VERIFIED |
| DRM master after a shell crash | the restarted shell serves `shell.info` and the app opens; whether the panel showed home again is for the operator to confirm (the logs predict a blank of about 1.5 s) | VERIFIED (service), UNRESOLVED (panel) |

Stage 1's SIGPIPE finding (§15.1) stays a confirmed follow-up defect: the
shell's supervisor path works, but a radiod crash should not need it.

### 16.1 H1 pass 2 rows closed by the operator

Contacts continued moving while the sweep line was static: YES. Contact
selection still worked with the static line: YES. Both VERIFIED (operator);
reduced motion behaves as DS §12 and the PocketRadar docs describe.

### 16.2 Position after §16

Stages 1 and 2 done. Stage 3 (crash loop on radiod, six exits in 60 s,
marker, recovery with `S60radiod restart`) not started. Each radiod exit in
stage 3 will also restart the shell once (§15.1), so the shell supervisor
will count restarts too; with six radiod kills spread over the backoff
schedule (about 35 s) the shell's own restarts stay under its limit of five
per minute only if the kills are paced, which the checklist loop does not
guarantee. That is a consequence to watch, not a reason to skip the stage.

## 17. Decisions after stage 2 (19:10 UTC)

### 17.1 Stage 3 held

The radiod crash loop (six SIGSEGV in 60 s) is not run tonight: with the
§15.1 defect every radiod exit also restarts the shell, so the loop would
push the shell supervisor towards its own crash-loop state and contaminate
the measurement. Stage 3 waits for the SIGPIPE fix.

### 17.2 SIGPIPE defect: root cause and minimal fix proposal (no code changed)

Root cause, from the code and the §15 evidence: every PocketOS socket
write goes through `write_all()` in `core/pocketipc/pocketipc.c` (line 45),
which calls `write(2)`. When the peer has closed, the kernel returns EPIPE
**and** raises SIGPIPE in the writer. radiod protects itself with
`signal(SIGPIPE, SIG_IGN)` (`services/radiod/main.c:817`); the shell and
`pos` install no SIGPIPE disposition (pocketlog's crash handler covers
SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT only), so the default action,
terminate, killed the shell (rc 141) the moment its one-second
`radio.status` poll (`ui/shell/shell.c:96` via `shell_ipc.c:64` and
`pocketipc_call()` at `pocketipc.c:313`) wrote into radiod's dead socket.
The error path that was designed for this (`pocketipc_call` returning NULL
with code 0, `shell_ipc_call` closing and reconnecting, the chip showing
`--`) never ran because the signal arrived first.

Writers affected (all through `pocketipc_send` → `pocketipc_write_frame` →
`write_all`):

| Caller | Process | Exposure |
| --- | --- | --- |
| `pocketipc_call()` | shell (status poll, radio app), `pos` | peer service dies mid-request |
| `pocketipc_server_reply()` (`server.c:200`) | shell, radiod | a `pos` client or the shell disconnects before the reply |
| `pocketipc_server_broadcast()` (`server.c:213`) | shell (`shell.app`, `shell.theme` events), radiod (`radio.rx`, `radio.state`, `radio.tx_done`) | a subscriber vanishes |

Two minimal fixes compared:

1. `signal(SIGPIPE, SIG_IGN)` in the shell's `main()`. One line, mirrors
   radiod, covers every write in the shell process. Leaves `pos` and any
   future service or out-of-process app to remember the same line, and it
   is process policy rather than a library guarantee.
2. `send(fd, p, len, MSG_NOSIGNAL)` instead of `write()` in `write_all()`.
   One line in shared code; every pocketipc user (shell, radiod, `pos`,
   future services) becomes SIGPIPE-safe by construction, with no change
   to signal dispositions anywhere. pocketipc fds are always sockets
   (`socket()`/`accept4()` in the same file), so `send` is a drop-in;
   MSG_NOSIGNAL is Linux-specific, which matches every PocketOS host
   (K230 and the WSL simulator). EPIPE is then reported through the
   existing `-1` path and the callers' transport-failure handling.

Recommendation: fix 2, in `core/pocketipc/pocketipc.c` only. It is the
smaller change in effect (no per-process policy), the more correct one (the
library promises that a vanished peer is an error, not a signal), and it
covers all three writers in all three processes at once. radiod's
`SIG_IGN` can stay as belt and braces. Fix 1 is not needed once fix 2 is in;
adding both would hide a future regression of fix 2.

Regression tests to add with the fix:

- `tests/pocketipc_test.c`: a `socketpair`, close the reading end, call
  `pocketipc_write_frame()` on the other end with SIGPIPE at its default
  disposition; expect `-1` with `errno == EPIPE` and, implicitly, the test
  process still alive (without the fix the process dies with 141 and
  `make test` fails). Also `pocketipc_call()` on such a socket must return
  NULL with code 0 and "send failed".
- `tests/shell_ipc_test.sh`: with the shell and the mock radiod running,
  `kill -KILL` radiod, wait two status polls (about 2.5 s), assert the
  shell's pid is unchanged and `pos shell info` still answers; restart
  radiod and assert `pos radio info` works again through the shell's
  reconnect (the chip path). This is the scenario that bit on the bench.

The fix is IPC-wide; nothing shell-specific is required. Not applied
tonight: it needs the normal host suite with `-Werror`, a cross-compile,
and a new image, and the golden image stays as it is until the operator
decides.

### 17.3 Hardware safety note for the SX1262 (operator, 2026-09-07)

**The SX1262 antenna is currently NOT installed.** SX1262 transmit is
forbidden until the operator explicitly confirms that the correct antenna
is installed. That covers `pos radio send`, `tests/hw/lora_pair_test.sh`
(its sending side) and anything else that keys the transmitter. CAD is
receive-side Channel Activity Detection, not a transmit operation. SPI and
register inspection (`pos-hwcheck --lora`), receive-side bring-up
(`radiod --backend sx1262` entering RX, `pos radio rssi`, `pos radio
listen`) and CAD may technically run without the antenna, although
meaningful RF measurements should preferably wait for it. This supersedes
the TX rows of BRINGUP_CHECKLIST.md §5 until the confirmation is given.

## 18. SX1262 bring-up, stage A: no transmit (19:26 to 19:33 UTC)

Operator update: antennas are now fitted on both MMCX connectors; TX is
permitted once the RF path to the SX1262 is confirmed, EU868 is confirmed,
power is conservative and the antenna on that path suits 868 MHz. Transcripts:
`hwcheck-unitA/h1-2026-09-07/sx_*.txt` (pulled later with the session files).

### 18.1 RF path evidence (documents)

The V1.0 schematic text (`T-Display K230_V1.0_NEW.pdf`, sheet with U26
`LoRa89_SX1262`, variants 13A/16A/T89/16E) routes the module's ANT pin
through L8 (0R) and R102 (0R), with C235 not fitted, to connector **RF3, part
BWIPX-3-001E**, a Hirose ultra-miniature coaxial receptacle of the IPEX/U.FL
class (SIG pin 1, GND pin 3). Wi-Fi uses a chip antenna (ANT1, ANT1CA-C03
with R55/L5/D3). **The schematic contains no MMCX connector at all**, so the
mapping of the two MMCX connectors on this unit to the SX1262 path cannot be
read from documents: either the PCB is a later revision than the published
schematic, or the MMCX connectors sit on pigtails or on the base board.
DOCUMENTED (schematic), mapping UNRESOLVED. It has to be established on the
bench, receive-side, before TX (§18.4).

### 18.2 Register probe and reset (SPI, no transmit)

`pos-hwcheck --lora` **hangs** on this image: its `gpioset --chip gpiochip0
--hold-period 5ms 5=0` (libgpiod 2 `gpioset` without `-z`) blocks waiting for
input when run without a terminal; the probe had to be killed (tool defect,
LOW, for a later fix). The same probe done by hand with daemonised
`gpioset -z` and `spi-pipe`:

| Item | Observation | Class |
| --- | --- | --- |
| Power line GPIO44 (chip1 line 12) driven high, reset pulse on GPIO5 | accepted by the kernel; lines free again after `killall gpioset` | VERIFIED |
| BUSY (GPIO19) and DIO1 (GPIO20) after reset | both inactive (low) | VERIFIED |
| `ReadRegister 0x0740..0x0741` | `aa aa aa aa 14 24`: the LoRa sync-word reset defaults, exactly as the SX1261/2 datasheet gives them; the chip answers on spidev0.0 at 1 MHz, mode 0 | VERIFIED |
| `GetStatus` | `aa 2a`: chip mode STDBY_RC (bits 6:4 = 010) after a bare reset, command status 5 (previous raw command, harmless) | VERIFIED |

### 18.3 Backend init, line ownership, RSSI, CAD

`echo RADIOD_BACKEND=sx1262 > /etc/default/radiod` and `S60radiod restart`
(the documented switch; the PocketOS shell was stopped for these steps as
checklist §5 says, its config file untouched).

| Item | Observation | Class |
| --- | --- | --- |
| Init | `radiod INFO listening ... backend=sx1262 region=EU868` at 19:31:46; no `begin failed`; `radio.info` reports chip and backend `sx1262`, region EU868 | VERIFIED: SX1262 `begin()` with TCXO 3.3 V on DIO3 and the DC-DC regulator works on this module |
| State and profile | `rx`; 869.525 MHz, BW 125, SF9, CR 4/5, sync 0x12, preamble 8, **tx_power 14 dBm** (the EU868 default, to be lowered before the first TX), CRC on | VERIFIED |
| Line ownership | GPIO5 output, GPIO19 input, GPIO20 input with rising-edge detection, GPIO44 output, all `consumer=radiod` | VERIFIED: the pin map 5/19/20/44 is right for this unit |
| RSSI (`pos radio rssi`, instantaneous) | 5 quick reads: -73, -94, -94, -74, -93 dBm; 20-sample baseline with both antennas fitted: floor -93 to -95 dBm, with -74 dBm readings in 5 of 20 samples, mean -89 | VERIFIED (numbers); the -74 dBm readings are ASSUMED to be real 868 MHz band activity or a nearby interferer |
| CAD (`pos radio cad`, receive-side) | three runs, `activity: false` each, state back to `rx` afterwards | VERIFIED: CAD and RX re-entry after CAD |
| Stats | tx 0, rx 0, crc errors 0 | VERIFIED |

### 18.4 Before the first transmit

Still to establish, in this order: which MMCX feeds the SX1262 (receive-side
A/B: RSSI with each antenna detached in turn; the SX1262 port is the one whose
noise floor and -74 dBm readings change), that the antenna on that port is an
868 MHz antenna (operator, from its marking), then `pos radio configure
tx_power_dbm=2` (SX1262 range -9 to 22, region cap 14) before a single short
`pos radio send`.

## 19. SX1262 RF connector mapping with a known-good MeshCore node (19:42 to 19:53 UTC)

Operator update: antennas fitted on both MMCX connectors; a known-good
Norwegian MeshCore node available as a deliberate transmitter. K230
receive-only throughout; TX power pre-set to 2 dBm.

### 19.1 Receiver profile (verified against the MeshCore source before use)

MeshNO (the Norwegian configurator, meshno.github.io) fixes 869.618 MHz,
62.5 kHz and SF8 ("EU/UK Narrow"), coding rate normally 4/5. From
`meshcore-dev/MeshCore` main, `src/helpers/radiolib/`: sync word
`RADIOLIB_SX126X_SYNC_WORD_PRIVATE` (0x12, the same value radiod hands to
RadioLib as `sync_word=18`); preamble `preambleLengthForSF(sf) = sf <= 8 ? 32
: 16` (PR 1954, merged 2026-04-30; older node firmware sends 16, which the
receiver detects just the same); CRC on (`setCRC(1)`); explicit header
(RadioLib default), so the coding rate is carried in each packet header and
a CR difference cannot hide packets. DOCUMENTED (source).

`pos radio configure frequency_mhz=869.618 bandwidth_khz=62.5
spreading_factor=8 coding_rate=5 sync_word=18 preamble_length=32
tx_power_dbm=2`: accepted (inside the EU868 guard), state `rx`. A 25 s
validation listen decoded one ambient MeshCore packet (RSSI -74 dBm, SNR
1.75 dB, CRC good, 77 bytes, frequency error -362 Hz), so the earlier -74
dBm RSSI readings in §18 were MeshCore traffic. VERIFIED: the K230 receives
the Norwegian MeshCore network.

### 19.2 Three rounds, 60 s each, operator sending 5 to 10 adverts or messages per round

| Round | Antennas | Packets | Operator's node (strong cluster) | Ambient nodes |
| --- | --- | --- | --- | --- |
| 1 | both connected | 14 | 9 packets, -15 to -29 dBm, SNR 11.8 to 14 | 5 packets, -64 to -86 dBm |
| 2 | **MMCX1 disconnected**, MMCX2 connected | 12 | 7 packets, **-54 to -71 dBm**, SNR 11 to 13.8 | 5 packets, **-106 to -109 dBm** |
| 3 | MMCX1 reconnected, **MMCX2 disconnected** | 10 | 7 packets, -15 to -44 dBm, SNR 10 to 13.5 | 3 packets, -76 to -79 dBm |

Removing MMCX1 cost about 30 to 40 dB on the operator's packets and about
25 dB on the ambient ones (still decodable through the open connector, as
expected at a few metres); removing MMCX2 changed nothing. Zero CRC errors
in all rounds (52 packets received in total by then).

| Statement | Class |
| --- | --- |
| **MMCX1 is the SX1262 antenna port on this unit**; MMCX2 is not in the SX1262 path | VERIFIED (repeatable, large, one-sided degradation with deliberate packets) |
| What MMCX2 feeds (Wi-Fi or a base-board radio) | UNRESOLVED, not needed for PocketOS |
| The schematic's RF3 (BWIPX-3-001E) is what MMCX1 is wired to on this unit | ASSUMED (the V1.0 schematic shows no MMCX) |

Raw captures: `hwcheck-unitA/h1-2026-09-07/sx_round1.txt`, `sx_round2.txt`,
`sx_round3.txt`, `sx_configure_meshno.txt`.

### 19.3 Next: first transmit

Preconditions from the operator's list: RF path VERIFIED (MMCX1); region
EU868 with the guard active; TX power set to 2 dBm; the antenna on MMCX1
being an 868 MHz antenna is for the operator to confirm. The first TX will
use the PocketOS default profile (869.525 MHz, BW 125, SF9, CR 4/5, sync
0x12, preamble 8, CRC) at 2 dBm rather than the MeshCore profile, so the
test packet is not injected into the Norwegian mesh on its exact modem
settings; both frequencies sit in the 869.4 to 869.65 MHz sub-band.

## 20. SX1262 first transmit (19:55:20 UTC)

Operator: both antennas reconnected, "go tx". Profile switched back to the
PocketOS default at 2 dBm (`pos radio configure frequency_mhz=869.525
bandwidth_khz=125 spreading_factor=9 coding_rate=5 sync_word=18
preamble_length=8 tx_power_dbm=2`, accepted, state `rx`), then exactly one
`pos radio send 506f636b65744f53` (8 bytes, "PocketOS"). Transcripts:
`hwcheck-unitA/h1-2026-09-07/sx_first_tx.txt`, `sx_first_tx_post.txt`.

| Item | Observation | Class |
| --- | --- | --- |
| Transmit | `radio.send` returned `bytes 8, airtime_ms 123.904`, rc 0; RadioLib `transmit()` completed (DIO1 TX-done seen by the HAL, otherwise the call would have timed out and failed) | VERIFIED: the SX1262 transmits on this board through the PocketOS backend |
| Airtime | 123.9 ms computed by `lora_airtime_ms()` for 8 bytes at SF9/BW125/CR4/5, preamble 8, explicit header, CRC; the checklist's rough "about 185 ms" was an over-estimate, the formula value is the one to use | VERIFIED (computed), the on-air duration itself was not measured |
| RX re-entry | state `rx` immediately after the send and again 1 s later; the backend's `enter_rx()` after `transmit()` succeeded (`is_receiving` true) | VERIFIED |
| Stats | tx_packets 1, tx_airtime 123.904 ms, duty cycle last hour 0.0034 %, rx_packets 58 unchanged, 0 CRC errors | VERIFIED |
| Lines and process | GPIO 5/19/20/44 still held by radiod; no crash report, no restart, radiod log quiet (no warnings) | VERIFIED |
| Reception of the packet by another node | not checked (the MeshCore node cannot decode the PocketOS profile) | UNRESOLVED |
| Radiated power, spectrum, harmonics | not measured | UNRESOLVED |
| Antenna on MMCX1 rated for 868 MHz | taken as confirmed by the operator's "go tx"; no marking recorded | operator |

The bench script lost its wall-clock measurement (BusyBox `date` has no
`%N`), so the blocking time of `radio.send` as seen by the client was not
captured; the checklist's expectation is airtime plus a few ms.

Stopped here by instruction: no power increase, no repeated or extended TX.
The device is left on the sx1262 backend (`/etc/default/radiod`), profile
PocketOS default at 2 dBm, state `rx`, PocketOS shell still stopped (its
config file untouched; it returns on `S90pocketos-shell start` or reboot).

### 20.1 Position after §20

Checklist §5 rows done: init, RSSI, CAD, one TX with RX re-entry, line
ownership. Not done: the 10-send repetition, `gpioinfo` during TX, the pair
test with a PocketOS second unit (none available; the MeshCore node cannot
serve as the receiving side of the PocketOS profile), the error-path
provocation. §12 (radio with the shell running) waits for the SIGPIPE fix
image, since restarting radiod under the shell would restart the shell.

