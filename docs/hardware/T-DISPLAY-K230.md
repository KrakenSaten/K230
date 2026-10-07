# LILYGO T-Display K230 hardware baseline

Recorded 2026-09-04. Sources, in priority order: official schematic
`T-Display K230_V1.0_NEW.pdf` (sheet dated 6/26/2026, "P1 V1.0"), LILYGO BSP
device tree patches and `k230_bsp/docs/HARDWARE_PINMAP.md`, vendor launcher
source, LILYGO wiki. Rows marked VERIFIED were confirmed on unit A on
2026-09-07; the evidence is in BRINGUP_SESSION_2026-09-07.md and
POST_BRINGUP_REVIEW_2026-09-07.md (hardware truth table).

Evidence classes: VERIFIED (our hardware or direct source/runtime proof),
DOCUMENTED (vendor schematic/source/docs), ASSUMED (inference).

## Board revision

- Schematic in the vendor repo is "V1.0" (file suffix NEW, date 6/26/2026).
  The RT-Smart firmware has module revisions V1.0..V1.3 (V1.1 added SX1262,
  V1.2 BLE, V1.3 LR2021/battery/keyboard/GPS/LTE-M). Those are firmware
  revisions, not PCB revisions. DOCUMENTED.
- Our two units: PCB revision and LoRa module variant not yet read from the
  boards. ASSUMED to be V1.0 PCB with SX1262. Verify by reading the silkscreen
  and the LoRa module marking (13A / 16A / T89 / 16E footprint options on the
  schematic).

## Core

| Item | Value | Evidence |
| --- | --- | --- |
| SoC | Canaan Kendryte K230 (schematic uses the K230D/"K230_LP4" symbol with PMU) | DOCUMENTED |
| CPU | Dual RISC-V C908 (1.6 GHz + 0.8 GHz), RVV 0.7-compatible, NPU. Linux sees one hart (`thead,c908`, rv64imafdcv, sv39); the second core is not examined | DOCUMENTED (dual core), VERIFIED (one Linux-visible hart), UNRESOLVED (second core) |
| RAM | LPDDR4 on-package, 1 GiB: U-Boot prints `DRAM: 1 GiB`, Linux MemTotal 990544 kB; the DTS 0x20000000 node is a placeholder that U-Boot fixes up. 512 MB of it is CMA reserved by the vendor DTS for the media blocks (still counted in MemTotal). | VERIFIED |
| Boot/storage | microSD on SDIO (mmc_sd1 in DTS, GPIO54..59). No eMMC. Runtime: mmcblk1, p1 80 MB `/boot`, p2 600 MB `/` (not grown to the card: the LILYGO BSP removes the SDK's first-boot resize); no RTC; `/dev/watchdog0` present and unfed | VERIFIED |
| Console | UART0 via CH342K USB-UART, 115200n8; on the bench PC it is COM9 (`USB-Enhanced-SERIAL-A CH342`), the second port COM10 is UART3 (untested). Windows supplied WCH's CH343 driver on first connection | VERIFIED (UART0/COM9), DOCUMENTED (COM10 = UART3) |
| Power key | SW2, silkscreen "0": PMU INT0 (ball C10, not a GPIO), switch to `VDD_1V8_RTC`, active high; driver `k230-pmu-pwrkey`, `/dev/input/event0`, `KEY_POWER`; kernel powers off after a 5 s hold. Details: K230_BUTTONS.md | DOCUMENTED; VERIFIED (unit B, 2026-10-06) |
| BOOT0 button | SW3, silkscreen "boot": pad IO0 / GPIO0 (gpiochip0 line 0), R8 10k pull-up, switch to GND, active low; boot strap (held at reset selects eMMC); pad is `0xAC4` (BOOT0 function, input disabled) as booted, so Linux cannot see it. Details: K230_BUTTONS.md | DOCUMENTED; VERIFIED (unit B, 2026-10-06) |
| Thermal | K230 on-chip tsensor, `CONFIG_CANAAN_THERMAL`; `thermal_zone0` = `canaan_thermal_zone`, 48 to 54 C on the bench | VERIFIED |

## Display and touch

| Item | Value | Evidence |
| --- | --- | --- |
| Panel | 4.1" AMOLED, RM69A10, 568x1232, MIPI DSI 2-lane, 65x145 mm active. DRM connector `card0-DSI-1` reports the single mode 568x1232 and the shell drives it (2 lanes, 24 bpp) | VERIFIED (mode and DRM path), DOCUMENTED (RM69A10 part identity) |
| Panel reset | GPIO22 | DOCUMENTED |
| Panel enable / "backlight" | GPIO25 (AMOLED has no backlight; DSI brightness command via panel driver patch 0049) | DOCUMENTED |
| DRM node | /dev/dri/card0, LVGL uses RGB565/XRGB8888 via DRM dumb buffers, GDMA rotation patch. The PocketOS shell runs on the vendor-patched LVGL with `K230_LVGL_DRM_STAGING=1`; with staging off the same workload costs 4.5x the CPU | VERIFIED (shell on DRM, staging cost) |
| Touch | Goodix GT9895 (Berlin), I2C addr 0x5D, SDA GPIO37, SCL GPIO36, IRQ GPIO23, RST GPIO24. Runtime: `goodix,nottingham` at i2c-1 0x5d, `event1 = goodix_ts`, range 1060 x 2400, GPIO23/24 held as `ts_irq_gpio`/`ts_reset_gpio`; LVGL's auto-calibration maps it correctly, no override needed | VERIFIED |
| HDMI | LT9611 DSI-to-HDMI bridge (U18) at 0x3b on I2C3 (GPIO36/37), reset GPIO24 and IRQ GPIO23 - all four shared with the touch controller; its 4-lane DSI input shares CLK/D0/D1 with the AMOLED, so HDMI and the panel are exclusive and chosen per boot (`/boot/force_dtb`, `pos-display-boot`). HPD and DDC end at the bridge. On unit A the device at 0x3b is an LT9611 (chip id `17 02 e2`, 2026-09-27). Map, driver and limits: HDMI_OUTPUT.md; gate: HDMI_GATE.md. The HDMI boot, HPD and EDID work on unit A; there is no usable picture yet (vendor driver set up for 1080p only; 1080p60 shows stripes) | DOCUMENTED (schematic V1.0 p. 2/4/7), VERIFIED (LT9611 at 0x3b, chip id, HDMI boot, EDID), FAILED (picture on a 2560x1440 monitor, 2026-09-27) |

## Radio and network

| Item | Value | Evidence |
| --- | --- | --- |
| Wi-Fi | Realtek RTL8189FTV (schematic footprint "RTL8821/RTL8189"), SDIO on mmc_sd0, module `8189fs`, enable GPIO45 (net IO45_WIFI_EN). Runtime: SDIO 0x024c:0xf179 bound to `rtl8189fs`, wlan0/wlan1 present; GPIO45 has no kernel consumer; Wi-Fi not brought up yet | VERIFIED (chip family, driver), DOCUMENTED (exact part), UNRESOLVED (function, GPIO45 role) |
| Bluetooth | Not on the RTL8189FTV. BSP supports USB BT dongles (btusb) and RTL8723DS BT UART (not wired). BLE in the vendor image comes from the optional nRF52840 base board over UART1. | DOCUMENTED |
| ESP32-S3 | The LILYGO wiki lists an "ESP32-S3-R8 co-processor" for Wi-Fi/BT. The schematic, BSP and launcher contain no ESP32. Schematic wins: treat the wiki statement as wrong for this PCB. | DOCUMENTED (conflict resolved by schematic) |
| Ethernet | RTL8152B-VB-CG USB 2.0 to 100 Mbps, appears as eth0 on the `r8152` driver (USB 0bda:8152). The adapter has no burned-in MAC: the kernel assigns a random one on every boot, so the DHCP lease can change per boot | VERIFIED |
| LoRa | Module footprint "LoRa89_SX1262" with 13A/16A/T89/16E variants; vendor firmware supports SX1262 or LR2021 on the same pins. Datasheet supplied: HPDTEK HPD16A (SX1262). | DOCUMENTED |
| LoRa SPI | spi0, /dev/spidev0.0, 4 MHz, mode 0. SCLK GPIO15, MOSI GPIO16, MISO GPIO17, CS GPIO14 (iomux alt1, DTS patch 0054). The SX1262 answers on it (sync-word registers read 0x14 0x24) and RadioLib runs it at 4 MHz | VERIFIED |
| LoRa control | RST GPIO5, BUSY GPIO19, DIO1/IRQ GPIO20, module power enable GPIO44 (net IO44_LoRa_EN, RT9080 3.3 V LDO). radiod holds exactly these four lines, DIO1 edge events deliver packets, init with TCXO 3.3 V on DIO3 and the DC-DC regulator works, RSSI, CAD and a 2 dBm transmit with RX re-entry work | VERIFIED |
| LoRa alternates | Schematic also has nets IO4_IRQ and IO3_TCXO_EN with 0R/NC options. BSP reassigns GPIO3/4 to UART1 for the nRF52840 base, so those options are ASSUMED unpopulated. Verify with a meter or by checking DIO1 events on GPIO20. | ASSUMED |
| LoRa RF | Sub-GHz only for SX1262 (EU868 target). No SDR, no wideband spectrum capability. LR2021 variant adds 2.4 GHz. | DOCUMENTED |
| LoRa antenna connector | Schematic V1.0: module ANT pin via L8/R102 (0R) to RF3, a Hirose BWIPX-3-001E (IPEX class); no MMCX on the schematic. Unit A carries two MMCX connectors; a controlled A/B test with a MeshCore node showed **MMCX1 is the SX1262 antenna port**, MMCX2 is not in the path (what it feeds is unknown) | VERIFIED (MMCX1 mapping), CONFLICTING (schematic vs fitted connectors) |
| USB | Two USB-C: one power/UART (CH342K), one K230 USB OTG (usb0/usb1 enabled, configfs gadget: ACM, RNDIS, mass storage, MTP via adb_mtp init) | DOCUMENTED |

## Camera, audio, sensors

| Item | Value | Evidence |
| --- | --- | --- |
| Camera | GC2093 on MIPI CSI2 (connector CAMERA1), I2C4 on IO7/IO8 (Linux `i2c-0`), addr 0x37; reset GPIO21, MCLK1 on GPIO13; vvcam driver and the vendor `isp_media_server`, V4L2 `/dev/video1..3`. Corrected 2026-09-24: this row said SDA GPIO49 / SCL GPIO48, copied from the vendor pin map; those are I2C0, on the FPC1 connector (CAMERA_PLATFORM_RESEARCH.md §1). No frame has been captured on unit A yet | DOCUMENTED (device tree, schematic); VERIFIED (0x37 answers on `i2c-0`, video nodes present on unit A) |
| Audio | K230 internal INNO codec (MMIO, ALSA card `K230I2SINNO`, `hw:0,0`), I2S, 3.5 mm headphone jack with mic bias, analog onboard mic on the right ADC channel and headset mic on the left; no speaker or amplifier on the main board itself (the product can still have a built-in speaker: next row); MAX98357A external I2S amp on the nRF52840 base board (data GPIO35, BCLK GPIO32, LRCK GPIO33, SD GPIO34). The booted default routes I2S to the header pads, and GPIO35 is also `IO35_DISEN` (display power) on the schematic: see AUDIO_FEASIBILITY_2026-09-12.md before any playback | DOCUMENTED, VERIFIED (card and PCM names on unit A) |
| Built-in speaker | Present in a second unit, unit B (owner's photographs, 2026-09-13): a TR-WS-2014B (owner's reading; 7.2 Ω, 1 W) on a lead with a white 2-pin plug. The separate base board inside (`K230_nRF52840_Board` VER 0.3) carries two 2-pin receptacles and a 16-terminal IC marked `AKK`: a MAX98357A. Which receptacle takes the speaker is not shown. Path: I2S on GPIO32/33/35 through the header to the MAX98357A (enable GPIO34), bridge-tied output to the speaker; not the codec, and neither speaker wire is ground. Unit A is the same hardware configuration (owner). Both paths validated on unit A (2026-09-13): the first controlled SEND was heard, decoded by a phone and left the panel steady: see AUDIO_HARDWARE_MAP_2026-09-13.md | PHYSICALLY CONFIRMED (unit B, identical to unit A per owner); amplifier CONFIRMED WITH HIGH CONFIDENCE (physical + vendor) |
| Sensors on main board | None documented. AHT20, BQ25896, BQ27220, TCA8418, XL9555 live on the optional base boards on I2C4 (SDA GPIO47, SCL GPIO46; the same controller as the camera's IO7/IO8 pins, Linux `i2c-0`: whether camera traffic reaches IO46/47 is open, CAMERA_PLATFORM_RESEARCH.md U9). On unit A nothing answers at 0x38 on `i2c-0` and the power_supply class is empty. That scan does not settle whether a base board is fitted: the vendor bit-bangs GPIO46/47 rather than using that controller (KEYBOARD_BRINGUP_2026-09-10.md) | DOCUMENTED; VERIFIED (no answer on `i2c-0`) |
| GPIO 40-pin header | See vendor HARDWARE_PINMAP.md; GPIO numbering 0..63, gpiochip0 = GPIO0..31, gpiochip1 = GPIO32..63 (vendor HAL `pin_chip`/`pin_offset`) | DOCUMENTED |

## Linux userspace facts relevant to PocketOS

- Init is BusyBox init + `/etc/init.d/S??` scripts; no systemd. The vendor
  launcher is started by `S99zz_k230_phone_ui` after waiting for
  /dev/dri/card0. DOCUMENTED.
- Root filesystem is ext4 (600 MB image), `/boot` ext4 with Image and DTBs,
  `/root/app` holds applications, `/app` symlinks to it. VERIFIED on unit A
  (574 MB filesystem, 131 MB free with PocketOS 0.0.1 installed); `/var/log`
  is a tmpfs, PocketOS keeps logs under `/var/lib/pocketos/log`.
- After a software power-off (`/sbin/poweroff`, BusyBox signalling init) the
  board does **not** restart when USB power is re-plugged. USB power has to be
  disconnected for about 30 seconds first; after that it boots normally.
  VERIFIED on unit A 2026-09-10 (V0.0.7_BLOCK2C_SMOKE.md). This is the K230
  power path, not software: the rails need the bulk capacitance to drain before
  a fresh insertion reads as a power-on event, and the same board reboots
  cleanly under `system.reboot`, which never removes power. Consequence for
  PocketOS: **power-off is not remotely recoverable on this hardware**, and any
  UI offering it must say so rather than present it as the peer of reboot.
- `/run` is a tmpfs (`rw,nosuid,nodev,relatime,mode=755`), so the PocketOS
  runtime directory `/run/pocketos` starts empty on every boot. VERIFIED on
  unit A at bring-up and again behaviourally in v0.0.7 block 2a: a planted
  `ghost.pid` was gone after a reboot (V0.0.7_BLOCK2A_SMOKE.md). PocketOS
  depends on this: `system.status.services[].running` is `kill(pid, 0)`, which
  is only safe against pid reuse because a stale pid file cannot survive a
  reboot.
- sshd accepts root with an empty password (`PermitRootLogin yes`,
  `PermitEmptyPasswords yes`, vendor sshd_config): anyone on the LAN has
  root until a password is set. VERIFIED.
- Wi-Fi driver is loaded by a generated `S40<conf>` script running
  `modprobe 8189fs` (and aic8800 modules, which will fail harmlessly without
  that hardware). DOCUMENTED.
- Vendor LoRa access is pure userspace: spidev + libgpiod v2 + RadioLib, with a
  polling/edge-event thread for DIO1. There is no kernel LoRa driver. This is
  the natural seam for a PocketOS radio service. DOCUMENTED.
- Input devices: touch and power key as evdev nodes; keyboard base via TCA8418
  evdev. DOCUMENTED.

## Companion hardware from LILYGO (optional base boards)

- nRF52840 base: BLE central bridge over UART1 (GPIO3 TX, GPIO4 RX,
  /dev/ttyS1, AT protocol), AHT20, MAX98357A. Firmware repo
  T-Display-K230-nRF52840.
- nRF9151 keyboard base: LTE-M/GNSS over UART3 (GPIO28/29, /dev/ttyS3),
  enable GPIO2, TCA8418 keyboard (IRQ GPIO42, RST GPIO43), BQ25896 charger,
  BQ27220 gauge, XL9555 expander, keyboard backlight PWM4 on GPIO52. Firmware
  repo T-Display-K230-nRF9151.

Which base boards, if any, we own is not recorded. A second unit, opened on
2026-09-13 (unit B), carries the nRF52840 base: silkscreen
`K230_nRF52840_Board`, `VER:0.3`, `20260407`, with the MAX98357A and the
built-in speaker (AUDIO_HARDWARE_MAP_2026-09-13.md §0-§4). Unit A has a
keyboard base attached as well, and is the same hardware configuration as unit
B (owner, 2026-09-13).

## Open verification items (do on physical hardware first)

1. Read PCB silkscreen revision and LoRa module marking on both units. (open)
2. Boot vendor image, capture `dmesg`, `cat /proc/meminfo`, `ls /dev/gpiochip* /dev/spidev* /dev/ttyS* /dev/i2c-* /dev/input/*`, `cat /proc/device-tree/model`. (done 2026-09-07 on the PocketOS image, hwcheck-unitA/)
3. Confirm DIO1 on GPIO20 by running the vendor LoRa app and watching edge events. (done through PocketOS radiod: 58 packets received on DIO1 edges)
4. Confirm Wi-Fi chip via `lsmod` and `/sys/bus/sdio/devices/*/device`. (done: 0x024c:0xf179, rtl8189fs)
5. Record CH342K COM port numbers on this host. (done: COM9 console, COM10 UART3)
6. Second unit: everything above, plus the pair test. (open)
7. Second C908 core, what MMCX2 feeds. (open) The HDMI bridge identity at 0x3b is settled: LT9611, chip id `17 02 e2`, read over I2C on unit A 2026-09-27 (HDMI_GATE.md step 1).
