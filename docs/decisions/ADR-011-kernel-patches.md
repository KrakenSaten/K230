# ADR-011: Doors-owned kernel patches on top of the LILYGO BSP

Status: Proposed (author, 2026-09-27); awaits the product owner's decision
after the unit A HDMI gate on the patched kernel (docs/hardware/HDMI_GATE.md,
round 2).
Date: 2026-09-27
Deciders: product owner (final), AI engineering partner (author)

## Context

ADR-001 decision 5: "Kernel and U-Boot: consumed unchanged from the LILYGO
BSP. PocketOS does not add kernel patches in v0.1." The reason was to keep
Core small and to stay on the exact kernel the vendor validated LoRa, Wi-Fi,
the camera and the NPU runtime on.

The HDMI gate of 2026-09-27 (HDMI_GATE.md) proved the hardware path - LT9611
probe, hot-plug detect, EDID, 26 modes from a 2560x1440 monitor - and then
found that no mode gives a usable picture, and that the shell picks 2560x1440,
a mode neither the K230 D-PHY nor the bridge can carry. Every cause found is
inside the kernel (docs/hardware/HDMI_KERNEL_FIX.md):

- the vendor LT9611 driver's `mode_valid` accepts any mode up to 300 MHz, so
  userspace is offered modes the bridge is not set up for (VERIFIED);
- the same driver computes the bridge's horizontal timing register 0x831a
  with an operator-precedence bug that breaks exactly 1280x720 (VERIFIED by
  reading; RT-Smart and upstream compute it correctly);
- BSP patch 0025 brings the bridge's 4-lane DSI link up with a PHY sequence
  written for the 2-lane AMOLED, at a fixed `hsfreqrange` and with a VCO
  table that disagrees with the D-PHY databook table and with the K230
  RT-Smart SDK's own values for this bridge (DOCUMENTED).

No userspace change can work around these: the DRM mode list is the kernel's,
and the PHY and bridge registers are the kernel's. The choice is between
carrying a small, documented kernel delta and not having HDMI output.

## Options

### A. Keep ADR-001 decision 5 as written: no kernel patches

- Pro: nothing to maintain across BSP updates; the vendor kernel stays the
  vendor's.
- Con: HDMI output stays at "proven path, no picture". Userspace cannot fix a
  mode list or a bridge register.

### B. Fork the kernel tree (a Doors kernel repository)

- Pro: full control; git history for every change.
- Con: a second pinned tree next to the SDK's; every BSP update becomes a
  rebase of a whole kernel; far more than the change at hand needs.

### C. Doors-owned patches in the Buildroot linux package directory (chosen)

The BSP itself is a patch stack (`buildroot-overlay/linux/00nn-*.patch`,
numbered up to 0064, applied by Buildroot in order on a fresh extract). Doors
adds its patches to the same directory from `platforms/k230/patches/linux/`,
numbered 0070-0099, installed by `apply_to_sdk.sh` from the same committed
snapshot as every other build input and recorded in the apply manifest
(`doors_kernel_patches=`).

- Pro: the mechanism already exists and is what the vendor uses; a patch is
  the unit of review; the build stays a function of a commit; a BSP update
  either applies the patches or fails loudly at extract time.
- Con: patches are written against the tree *after* the BSP stack, so a BSP
  change to the same files needs a refresh; Buildroot re-patches only on a
  fresh extract (`make linux-dirclean`), which the build notes must say.

## Decision

Option C, under these rules:

1. A Doors kernel patch fixes a defect or a hardware-facing limitation that
   userspace cannot work around, and says so in its header. No features, no
   refactors, no upgrades (ADR-001 pins stay).
2. Every patch names the ADR and a hardware sheet that holds its evidence,
   with the register values it programs derived from a source that is cited
   (vendor SDK, vendor RT-Smart sources, upstream Linux, or a datasheet), and
   never invented. `tests/kernel_patches_test.sh` checks the mechanics.
3. The 2-lane AMOLED path is not changed by an HDMI patch. A patch that
   touches shared code keeps the panel path bit-identical unless its own
   gate covers the panel.
4. A patch is merged only after a unit A gate on the built kernel, with the
   LCD tree booted first (the panel must be unaffected) and a written
   rollback.
5. The first patches are the two HDMI patches of 2026-09-27: 0070 (LT9611
   mode table and 0x831a) and 0071 (4-lane DSI PHY from the D-PHY tables).

ADR-001 decision 5 is amended to: "Kernel and U-Boot come from the LILYGO
BSP; Doors adds kernel patches only under ADR-011." U-Boot stays unchanged.

## Consequences

Needed now: `platforms/k230/patches/linux/`, the apply step, the test, the
build note (`linux-dirclean` before a kernel rebuild), and the HDMI gate on
the patched kernel.

Useful soon: `BUILD_INFO.txt` listing the kernel patches (the manifest already
does); a `pos-hwcheck`/`doors` line that prints the running kernel's build
stamp so a unit's kernel can be told apart from the image's.

Risks: a BSP update that rewrites `lontium-lt9611.c` or `canaan_dsi.c` makes
the patches fail to apply - which is the intended failure mode (loud, at
extract). A kernel that does not boot is recovered from the serial console
(U-Boot loads `/Image.orig`, see HDMI_GATE.md round 2) or by editing the
card's boot partition on a PC.

## Evidence

- ADR-001 decision 5 and its reason: DOCUMENTED (the ADR).
- The three kernel-side causes: docs/hardware/HDMI_KERNEL_FIX.md, each with
  its evidence class; the unit A observations behind them: HDMI_GATE.md
  (VERIFIED 2026-09-27).
- The BSP patch mechanism (Buildroot applies `linux/*.patch` in order on
  extract; the SDK's `tools/sync.mk` copies the overlay and drops patches
  that left it): DOCUMENTED (SDK `tools/sync.mk`, Buildroot
  `.applied_patches_list` of the pinned build).
- The patched kernel builds with the pinned Xuantie toolchain and the two
  patches apply on a fresh extract: see HDMI_KERNEL_FIX.md, "Build".
