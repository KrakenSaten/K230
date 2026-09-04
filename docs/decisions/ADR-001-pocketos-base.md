# ADR-001: PocketOS base platform

Status: Accepted (product owner, 2026-09-04)
Date: 2026-09-04
Deciders: product owner (final), AI engineering partner (author)

## Context

PocketOS needs an operating-system base for the LILYGO T-Display K230 that
can grow into a modular platform (radio, networking, local AI, field tools,
companion devices) without coupling Core to one board. The board ships with
two vendor software stacks:

- Linux: Kendryte K230 Linux SDK (Buildroot 2025.02.1, kernel 6.6, U-Boot
  2022.10) plus the LILYGO BSP overlay and an LVGL launcher. DOCUMENTED and
  currently being built by us.
- RT-Smart/CanMV: RT-Thread based RTOS with MicroPython and LVGL sample apps
  (T-Display-K230_canmv_rt). DOCUMENTED, not built by us.

Constraints from the project charter: Core must stay small and boring;
hardware belongs to services, not apps; public APIs must be generic
(display.*, radio.*, ...); reproducible builds; updates and rollback must
remain possible later; commercial distribution is possible, so licensing
must stay visible.

Hardware facts that matter here (see docs/hardware/T-DISPLAY-K230.md):
microSD is the only boot medium, RAM is 512 MB or 1 GB (unverified), the
LoRa radio is a plain SX1262 on spidev, Wi-Fi is an SDIO Realtek module with
an out-of-tree driver, Ethernet is USB.

## Options

### A. Linux, PocketOS as a layer on top of the pinned LILYGO BSP (recommended)

PocketOS is a set of Buildroot packages, a rootfs overlay and its own
defconfig applied to the unmodified, pinned K230 Linux SDK plus the LILYGO
BSP overlay. The LILYGO launcher is replaced by the PocketOS shell; the
LILYGO BSP (kernel patches, DTS, drivers, U-Boot) is consumed as-is.

- Pro: the vendor has already proven RM69A10 DSI, GT9895 touch, GC2093
  camera, SDIO Wi-Fi, USB Ethernet, BlueZ, ALSA, DRM, V4L2, spidev/gpiod
  LoRa and the nncase NPU runtime on this exact kernel.
- Pro: process isolation, mature networking stacks (wpa_supplicant, BlueZ,
  usbnet, OpenSSH), standard tooling (SSH deploy, gdb, strace, evtest),
  Python available for tools and Labs.
- Pro: BSP updates from LILYGO can be rebased by bumping a pinned commit.
- Con: boot time and idle power are worse than an RTOS. Not measured yet.
- Con: we depend on LILYGO keeping the BSP maintained; mitigated by pinning
  and by keeping our overlay separable.
- Con: kernel patches in the BSP are GPL-2.0; the rootfs mixes many
  licences. Must be tracked in an SBOM before distribution.

### B. RT-Smart/CanMV

Single-image RTOS with LVGL and MicroPython.

- Pro: fast boot, lower RAM and power, simple deployment.
- Con: no process isolation; one bad app takes the device down.
- Con: networking (Wi-Fi supplicant, BLE stack, Ethernet, SSH) is far less
  complete than Linux; PocketNet-class tooling would be written from
  scratch.
- Con: developer tooling (debugging, logging, deploy over SSH) is weaker.
- Con: vendor examples are demo-grade and tied to a monolithic UI binary.

### C. Fork the whole SDK and BSP into the PocketOS repository

- Pro: one repo, full control.
- Con: hundreds of MB of vendor code with unclear boundaries; every LILYGO
  fix becomes a manual merge; licensing attribution becomes harder. Rejected
  in favour of pinned submodules and an overlay.

### D. Hybrid: Linux now, RTOS on a co-processor later

Not applicable to this board: there is no free MCU on the main PCB. The
optional nRF52840/nRF9151 base boards are companion devices and belong to
PocketLink, not to the base platform.

## Decision

Option A. Concretely:

1. Repository shape: `pocketos/` holds first-party code; `vendor/` (or
   submodules) holds `T-Display-K230` at a pinned commit, which in turn pins
   `k230_linux_sdk`. Nothing under vendor is edited in place; changes to the
   BSP go upstream or into a documented patch directory.
2. Build: `platforms/k230/` contains `k230_pocketos_defconfig` (derived from
   `k230_canmv_t_display_rm69a10_defconfig`), Buildroot package definitions
   for PocketOS components, and a rootfs overlay. A single script applies BSP
   plus PocketOS to the SDK and builds the SD image.
3. Init: keep BusyBox init and `/etc/init.d` scripts for v0.1. systemd is
   not in the vendor rootfs and would add size and boot time for no v0.1
   benefit. Revisit when service supervision and crash-loop detection are
   designed (ADR later).
4. Language: Core and services in C (C11) with C++20 allowed where a
   dependency requires it (RadioLib). Python allowed for tools and Labs, not
   for Core.
5. Kernel and U-Boot: consumed unchanged from the LILYGO BSP. PocketOS does
   not add kernel patches in v0.1.
6. Storage layout: keep the vendor MBR layout (boot ext4 + rootfs ext4) for
   v0.1, but reserve the decision on A/B rootfs partitions for the update
   ADR; do not hard-code partition numbers in PocketOS code.

## Consequences

Needed now: defconfig, package skeleton, apply/build script, replacement of
`S99zz_k230_phone_ui` with the PocketOS shell service.

Useful soon: SBOM generation from Buildroot (`make legal-info`), boot-time
and idle-power measurements, a developer-mode switch that keeps SSH and the
serial console enabled.

Future: A/B partitions and rollback, signed images, possible second board
port (the HAL boundary must not leak K230 device paths into services).

Risks: RAM headroom is unknown until measured on hardware; the LILYGO BSP
moves fast (v0.2.4 was released the day before this ADR) and our pin will
drift; the SDIO Wi-Fi driver is out-of-tree vendor code.

Migration cost if reversed later: the UI layer (LVGL) and RadioLib usage
would carry over to RT-Smart; services, IPC and everything relying on Linux
process isolation would not.

## Evidence

- Vendor Linux stack works on the board: DOCUMENTED (LILYGO v0.2.4 release
  notes and launcher source); VERIFIED only once our image boots.
- Hardware inventory: DOCUMENTED from schematic V1.0 and BSP DTS.
- Boot time, RAM use, power: ASSUMED worse than RTOS, not measured.
