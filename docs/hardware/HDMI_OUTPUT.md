# HDMI output on the T-Display K230

Recorded 2026-09-26 on branch `feat/k230-hdmi-out` (baseline `origin/master`
`dd3809b`). This is a desk study plus build-host validation. **Nothing here has
been run on a K230 yet**; the hardware gate is `HDMI_GATE.md`.

Evidence classes as in `T-DISPLAY-K230.md`: VERIFIED (unit A runtime record, or
reproduced here from the pinned sources), DOCUMENTED (vendor schematic, source,
device tree, config), ASSUMED (inference).

## Sources

| Short | What | Pin |
| --- | --- | --- |
| SCH | `T-Display K230_V1.0_NEW.pdf` from `Xinyuan-LilyGo/T-Display-K230_canmv_rt` `schematic/`, sheets "HDMI+ETH" (p. 7), "Video" (p. 4), K230 IO (p. 2) | `abb0709` |
| BSP | `Xinyuan-LilyGO/T-Display-K230` `k230_bsp/overlay/buildroot-overlay` | `bb831ab` |
| SDK | `kendryte/k230_linux_sdk` `buildroot-overlay` | `22d02c6` |
| K | `ruyisdk/linux-xuantie-kernel` with every SDK and BSP `linux/*.patch` applied in Buildroot order (all 61 apply) | `7d4e1f4` |
| LV | LVGL `src/drivers/display/drm/lv_linux_drm.c` plus BSP patches 0002/0004 | `59dc7e4` |
| UA | unit A bring-up records, `hwcheck-unitA/` | 2026-09-07 |

The DTBs were rebuilt here from K with `cpp` + `dtc`: `k230-canmv-rm69a10.dtb`
(60,191 bytes, SHA-256 `3ac313c4…`) and `k230-canmv-rm69a10-hdmi.dtb` (56,410
bytes, `2a6b49ad…`) are **byte-identical** to the ones every Doors image ships
(`V0.0.2_BUILD_REPORT.md`). So the trees analysed below are the trees on the
card. VERIFIED.

## Conclusion

**PARTIALLY SUPPORTED: HDMI works as an alternative boot-time output, never
beside the built-in AMOLED.** The bridge, its kernel driver and an HDMI device
tree all exist and are already in every Doors image; what was missing was a
safe way to select it and a way to prove it. Mirroring and dual display are
ruled out by the hardware (one DSI transmitter, lanes shared with the panel)
and by the driver (one CRTC, one encoder, panel *or* bridge).

## 1. Hardware path

```
K230 VO (one display pipeline, one CRTC)
  └─ MIPI DSI TX (balls Y9/W9 CLK, W10/Y10 D0, Y11/W11 D1, W8/Y8 D2, Y7/W7 D3)
       ├─ nets DSI_CLK, DSI_D0, DSI_D1 ──────────► J1 display FPC (RM69A10 AMOLED, 2 lanes)
       └─ nets DSI_CLK, DSI_D0..D3 (same nets) ──► U18 LT9611, MIPI port B (4 lanes)
                                                     └─ TMDS D0..D2, CLK ─► U16/U17 TPD4E05U06 ESD ─► HDMI1
```

| Element | Fact | Evidence |
| --- | --- | --- |
| Display controller | `vo@90840000`, one output port, to `dsi@90850000` only. No second display interface in the device tree | DOCUMENTED (K `k230.dtsi`) |
| SoC interface | MIPI DSI TX0/TX1 pins, net names DSI_CLK_P/N, DSI_D0..D3_P/N | DOCUMENTED (SCH p. 4, U22C) |
| Shared lanes | The **same** DSI_CLK/D0/D1 nets reach J1 pins 18-25 and U18 pins 49-54; D2/D3 reach only U18. No switch, mux or series part between them | DOCUMENTED (SCH p. 4 J1, p. 7 U18) |
| Bridge | Lontium **LT9611** (U18), DSI input on port B ("LT9611 swap" note on the sheet), port A unused | DOCUMENTED (SCH p. 7); an LT9611 is visible on unit A's main board (AUDIO_HARDWARE_MAP_2026-09-13.md) |
| Bridge clock | 27 MHz crystal on XTALI/XTALO | DOCUMENTED (SCH p. 7) |
| Bridge power | VCC33 = VDD_3V3, VCC18_RX/TX = VDD_1V8_RX/TX (ferrites from VDD_1V8), VDD = VDD_1V8. No enable GPIO, no switched rail: always powered | DOCUMENTED (SCH p. 7) |
| Connector | HDMI1, 19 signal pins numbered as Type A (1 D2+, 2 D2 shield, … 18 +5 V, 19 HPD), symbol "HDMI-MH"; the vendor device tree says `type = "a"`. Mechanical size not stated on the sheet | DOCUMENTED |
| ESD | TPD4E05U06 on all four TMDS pairs | DOCUMENTED (SCH p. 7) |
| +5 V to the sink | Connector pin 18 from VDD_5V through Schottky D13 | DOCUMENTED |
| HPD | Connector pin 19 → LT9611 pin 13 (HPD_GPIO2), 100 kΩ pull-down R70. **Not wired to the K230**; the SoC learns of it from the LT9611 over I2C and its IRQ line | DOCUMENTED |
| DDC | Connector SDA/SCL → LT9611 DSDA/DSCL (pins 14/15), 10 kΩ pull-ups to VDD_5V. **Not wired to the K230**; EDID is read by the LT9611's DDC master and fetched over I2C | DOCUMENTED |
| CEC | Connector pin 13 → LT9611 CEC, pulled to 3.3 V through R73/D12. The driver does not use it | DOCUMENTED |
| HDMI audio | LT9611 I2S/SPDIF inputs are not connected to the K230 (only SCLK to test point T1): no HDMI audio possible | DOCUMENTED |

### Control pins

| Signal | K230 | Shared with | Board detail | Evidence |
| --- | --- | --- | --- | --- |
| I2C SCL / SDA | GPIO36 / GPIO37, I2C3 (Linux `i2c-1`, `91408000.i2c`) | GT9895 touch (TP_SCL/TP_SDA on J1) | 10 kΩ pull-ups R76/R77 | DOCUMENTED (SCH p. 2, 4, 7); bus VERIFIED on unit A |
| I2C address | 0x3b | - | ADDR_GPIO0 pulled high through R78 | DOCUMENTED; **a device answers at 0x3b on `i2c-1` on unit A** VERIFIED (hwcheck 2026-09-07) |
| Reset (active low) | GPIO24 (HDMI_RSTN) | GT9895 reset (TP_RST) | R74 10 kΩ pull-up + C182 100 nF | DOCUMENTED; GPIO24 held as `ts_reset_gpio` on unit A VERIFIED |
| Interrupt | GPIO23 (HDMI_INT) | GT9895 interrupt (TP_INT) | R75 10 kΩ pull-up | DOCUMENTED; GPIO23 held as `ts_irq_gpio` on unit A VERIFIED |
| Enable / regulator | none | - | - | DOCUMENTED |

**Pin conflicts.** The bridge shares all four of its control lines with the
touch controller, and its video lanes with the panel. It touches nothing of
SX1262 (GPIO5, 14-17, 19, 20, 44), the keyboard base (GPIO42/43/46/47), the
camera (IO7/8, 13, 21), audio (GPIO32-35), SD (GPIO54-59) or Wi-Fi/Ethernet.
Consequence: the GT9895 and the LT9611 cannot both have a kernel driver in the
same device tree - both drivers would claim GPIO24 and GPIO23, and every touch
reset also resets the bridge. The vendor's HDMI tree simply leaves the touch out.

## 2. Software path

### Kernel (K, both device trees)

| Layer | LCD tree `k230-canmv-rm69a10.dtb` (default) | HDMI tree `k230-canmv-rm69a10-hdmi.dtb` |
| --- | --- | --- |
| DRM device | `/dev/dri/card0`, `canaan-drm` (VERIFIED unit A) | same driver, same node |
| CRTC | one (`canaan_crtc`), VO | same |
| Planes | OSD4 primary (XRGB8888, ARGB8888, ARGB4444, ARGB1555, RGB888, BGR888, RGB565), OSD5 cursor, OSD6/7 overlays, video_1..3 overlays (NV12/NV21/NV16/NV61); rotation property (GDMA, BSP patch 0035) | same |
| Encoder | one DSI encoder, `possible_crtcs = 1`; `drm_of_find_panel_or_bridge()` on DSI port 1: a panel **or** a bridge | same encoder |
| Next stage | `canaan,universal` panel (RM69A10), 2 lanes, burst video | `lontium,lt9611` bridge on i2c3 @0x3b, reset GPIO24, IRQ GPIO23 falling; asks the DSI host for 4 lanes, RGB888, sync-pulse video |
| Connector | `card0-DSI-1`, one mode 568x1232, 49.5 MHz (VERIFIED unit A) | `card0-HDMI-A-1` created by the bridge driver itself, modes from EDID |
| fbdev | skipped (`canaan,skip-fbdev-setup`, boot splash kept) | generic fbdev emulation, 32 bpp |
| Kernel config | `CONFIG_DRM_LONTIUM_LT9611=y`, `CONFIG_DRM_DISPLAY_CONNECTOR=y` in `k230_defconfig`: the bridge driver is in every Doors kernel already | DOCUMENTED |

The LT9611 driver is the upstream `lontium-lt9611.c` rewritten by SDK patch
0001 for the K230. What it does, read from K:

- **Probe**: toggles reset, reads the chip revision at 0x8000/0x8001. If the
  chip does not answer, probe fails; the DSI host then waits for a bridge that
  never arrives (deferred probe), `canaan-drm` never binds and there is **no
  `/dev/dri/card0`** at all. The boot itself continues. DOCUMENTED (code path),
  not observed.
- **Detect**: reads register 0x825e on every probe of the connector.
- **HPD**: the IRQ handler logs `hdmi cable connected` / `disconnected` and
  clears the flags; the call that would tell userspace
  (`drm_kms_helper_hotplug_event`) is commented out and `connector->polled`
  is 0. So **no hotplug uevent and no kernel polling**: a new monitor shows up
  when userspace asks for the connector again (DRM master's GETCONNECTOR, or
  `echo detect > /sys/class/drm/card0-HDMI-A-1/status`). No CPU is spent
  waiting.
- **EDID**: `drm_do_get_edid()` over the LT9611's DDC master, base block plus
  at most one extension. No ACK → `read edid failed: no ack`, no EDID property.
- **Modes**: `mode_valid` accepts anything up to 300 MHz with htotal and
  vtotal ≤ 4095. Invalid or missing EDID on a connected sink: the DRM core's
  no-EDID fallback applies (upstream 6.6 behaviour, not re-read in K: ASSUMED).
- **Enable**: fixed HDMI (not DVI) mode, no infoframes beyond a fixed AVI
  setup, a 500 ms sleep during every enable. No audio.

**The pixel clock is quantised.** `canaan_dsi_encoder_mode_fixup()` replaces
every requested clock by `594000 kHz / round(594000 / clock)`. Exact: 148.5
(1080p60), 74.25 (720p60, 1080p30), 27 (480p, 576p), 49.5 MHz (the panel).
Not exact: 640x480@60 25.175 → 24.75 MHz (−1.7 %), 800x600@60 → −1 %,
1024x768@60 → +1.5 %, 1280x1024@60 108 → 99 MHz (−8.3 %). A monitor may refuse
a mode whose refresh is off by several percent, which is why `pos-drmtest`
shows the real clock of every mode and picks exact ones first. DOCUMENTED (K),
and the arithmetic VERIFIED by `tests/drmtest_test.c`.

### The Doors display stack today (LCD tree)

- The shell owns the display (ADR-002). `ui/shell/platform_drm.c` calls
  `lv_linux_drm_create()` and `lv_linux_drm_set_file("/dev/dri/card0", -1)`
  (`POCKETOS_DRM_DEVICE` overrides the node).
- LVGL takes the **first connected connector with modes** and its
  `modes[0]` (LV `drm_find_connector`), creates two dumb buffers, and with
  `K230_LVGL_DRM_STAGING=1` renders into heap copies that are copied into
  the scanout buffer once per frame (VERIFIED: 4.5x CPU without it,
  T-DISPLAY-K230.md).
- Rotation is DRM plane rotation through the vendor LVGL patch 0002 and the
  GDMA kernel patch, decided once at shell start (`shell_display.c`).
- If the mode is not 568x1232 the shell logs `display came up WxH, expected …`
  and lays out in what it got (`platform_drm.c`). If no connector qualifies,
  `display init failed`, exit 1; `pos-supervise` backs off 1, 2, 4, 8, 16 s and
  gives up after more than five restarts in 60 s (crash-loop state file).

## 3. Can HDMI and the AMOLED run together?

No, and not as a mirror either:

- One DSI transmitter drives one link. The panel wants 2 lanes at 568x1232
  (52 Hz, burst mode, DCS init in LP mode); the LT9611 wants 4 lanes with a
  monitor timing. The lanes are physically common, so both receivers always
  see the same signal; they can never be given different ones.
- Feeding both the panel's own timing is not a mirror: 568x1232 is not a mode
  an HDMI monitor accepts, and the LT9611 driver would have to run in a 2-lane
  configuration nobody has tested. Not pursued. ASSUMED impractical.
- The driver model has one CRTC and one encoder whose port 1 is either the
  panel or the bridge. A device tree with both is not supported by
  `canaan_dsi.c` (it takes whichever it finds first).
- Software mirroring needs a second scanout, which does not exist.

So the outcome is **external-only, chosen at boot**. The AMOLED's state during
an HDMI boot is not known (U-Boot has already initialised it for the boot logo,
Linux never touches it, and its lanes then carry the HDMI stream): observe it in
the gate.

## 4. How the output is selected

U-Boot runs `k230_set_dtb` on every boot (SDK `default.env`: `blinux=k230_set_dtb
&& ext4load … /${dtb} …`). In the BSP's `k230_board_common.c`:

1. `/boot/force_dtb` exists and is non-empty → load the device tree it names.
2. Otherwise probe I2C bus 3 for 0x38 (an LCD touch) then 0x3b (LT9611) →
   `lcd_dtb` or `hdmi_dtb`.
3. Otherwise `lcd_dtb`.

The LILYGO BSP removes `&i2c3` from U-Boot's device tree, so step 2 never finds
a bus and **every boot takes `lcd_dtb`**. That is why unit A boots the panel
although its bridge answers at 0x3b (VERIFIED: `ext4load … lcd_dtb` in the boot
log, BRINGUP_SESSION_2026-09-07.md §10.2). `force_dtb` is the supported
override and changes no device tree. DOCUMENTED.

Not used, on purpose: the vendor launcher's HDMI page (`ui_hdmi_test.c`)
copies the HDMI tree over `/boot/k.dtb`. In this image `k.dtb` is a symlink to
`k230-canmv-rm69a10.dtb` (`post-image.sh`), so the copy writes the HDMI tree
into the panel's device-tree file, and its "restore" copies that file onto
itself. DOCUMENTED (source reading), not reproduced.

## 5. The HDMI tree is a CanMV board tree

BSP patch 0062 turned `k230-canmv-rm69a10-hdmi.dts` into `#include
"k230-canmv-v3.dts"`, with the note that this tree "was verified on T-Display
K230: LT9611 probes, EDID is read, and HDMI-A-1 exposes 800x480@60" (vendor
claim, DOCUMENTED). Rebuilt here, the two DTBs are identical. Compared with the
LCD tree it:

| Lost in HDMI mode | Why it matters to Doors |
| --- | --- |
| `spi0` disabled, no spidev | no `/dev/spidev0.0`: radiod's SX1262 backend cannot start, no LoRa, no MeshCore |
| `uart1` disabled | no nRF52840 base-board link |
| no GT9895 node | no touch (the panel is dark anyway) |
| no GC2093 node, no MCLK1/I2C4 pin mux | camera not expected to work |
| no AHT20, amplifier, keyboard IRQ/backlight pin mux | base-board features unmanaged |
| no `reserved-memory` for the boot splash, no `skip-fbdev-setup` | fbdev console on HDMI instead |
| model `Canaan CanMV-K230` | what `/proc/device-tree/model` reports |

Kept: UART0 console, UART3, USB (Ethernet), SD, SDIO Wi-Fi, audio codec,
thermal. DOCUMENTED (decompiled DTBs), effects ASSUMED until the gate.

## 6. What this branch adds

No kernel, device-tree, U-Boot, defconfig or SDK change. ADR-001 decision 5
("Kernel and U-Boot: consumed unchanged from the LILYGO BSP") would need a
proposal for any of those, and none is needed to prove the output:

- **`pos-drmtest`** (`tools/drmtest/`): LVGL-free and libdrm-free (kernel DRM
  ioctls only). `list` (connectors, state, every mode with the DSI's real clock
  and error, EDID summary), `edid [CONNECTOR]` (decoded plus hex), `pattern
  [--connector] [--mode WxH[@R]] [--seconds N]` (black frame, white border,
  red/green/blue blocks, "DOORS K230 HDMI TEST", mode and real clock; restores
  the previous CRTC state afterwards; refuses with exit 3 while another process
  is DRM master). Without `--mode` it picks the smallest progressive mode of at
  least 640x480 in either orientation (so the AMOLED's portrait 568x1232
  qualifies), at or below 148.5 MHz, clock-exact first. `modetest` is in the
  image (`BR2_PACKAGE_LIBDRM_INSTALL_TESTS=y`) but knows nothing of the 594 MHz
  quantisation and does not decode EDID; it remains a second opinion.
- **`pos-display-boot status|lcd|hdmi`** (`tools/display/`): writes or removes
  `/boot/force_dtb`. Refuses, without writing, when `hdmi_dtb` is missing,
  names the panel tree, names something that is not a plain file in `/boot`,
  or when either tree is missing or not a device-tree blob. Writes through a
  temporary file and `sync`. Persistent until changed; recovery in
  `HDMI_GATE.md`.
- Tests: `tests/drmtest_test.c` (45 checks: clock quantisation, EDID decoding
  and corruption, mode choice with and without EDID, pattern geometry),
  `tests/display_boot_test.sh` (27 checks against a fake boot partition, also
  under BusyBox sh).
- The ioctl half of `pos-drmtest` was run on the build host against a real
  KMS driver: `vkms` in an Ubuntu 6.8.0-142 kernel under QEMU (no K230 DRM
  code involved). `list` probed one connector with 34 modes, `pattern` set
  1280x720 and 1280x720@60 and put the fbdev framebuffer back, a missing mode
  exited 2, a second instance exited 3 while the first held DRM master, and
  SIGTERM during the hold still restored the CRTC. That proves the tool, not
  the K230 display path.

No Doors UI, launcher, application, radio or `VERSION` change. The shell is
not modified: on an HDMI boot it already opens whatever connector is there.

## 7. Resource impact (estimates, not measurements)

| Item | 568x1232 panel | 720p60 | 1080p60 |
| --- | --- | --- | --- |
| One XRGB8888 frame | 2.80 MB | 3.69 MB | 8.29 MB |
| LVGL: two dumb buffers (CMA) | 5.6 MB | 7.4 MB | 16.6 MB |
| LVGL staging: one heap copy per dumb buffer (BSP LVGL patch 0004) | 5.6 MB | 7.4 MB | 16.6 MB |
| fbdev emulation (HDMI tree only) | none | +3.7 MB | +8.3 MB |
| Scanout read bandwidth | ~146 MB/s (52.2 Hz) | 221 MB/s | 498 MB/s |
| DSI lane rate | 594 Mbit/s x 2 | 446 Mbit/s x 4 | 891 Mbit/s x 4 |
| Staging copy per full frame | 2.8 MB | 3.7 MB | 8.3 MB |

CMA reserves 512 MB (VERIFIED), so buffer memory is not a constraint.
`pos-drmtest pattern` draws one frame and then sleeps: static output costs no
CPU beyond scanout DMA. The shell's CPU cost scales with the pixels it
redraws; a full redraw at 1080p touches about 3x the panel's pixels, so expect
up to ~3x the shell's panel-mode CPU for full-screen animation (ASSUMED,
measure in the gate). No software mirroring exists or is proposed.

## 8. Known limitations

- HDMI and the AMOLED are exclusive; switching needs a reboot.
- The HDMI tree drops LoRa, touch, uart1 and the camera sensor (section 5).
- No hotplug event reaches userspace; a monitor connected after boot needs a
  re-probe, and the shell does not re-probe.
- A shell started with no monitor attached exits and ends in the supervisor's
  crash-loop state; it has to be restarted by hand once a monitor is there.
- Modes whose clock does not divide 594 MHz run slightly off-rate.
- No HDMI audio, no CEC.
- If the bridge is absent or dead, an HDMI boot has no DRM device at all.
- The shell's layouts are designed for 568x1232 and 1232x568; at monitor sizes
  they are untested.

## 9. Next platform step (proposal, not applied)

A Doors-owned HDMI device tree that keeps the T-Display peripherals: the LCD
tree minus the `canaan,universal` panel and minus the GT9895 node (shared
GPIO23/24), plus the LT9611 node on `i2c3` (reset GPIO24, IRQ GPIO23 falling,
the `hdmi_i2c3_pins` mux on IO36/37) and the `hdmi-connector`. That is a kernel
tree change and needs a proposal under ADR-001. A second, optional kernel patch
would restore the LT9611 hotplug event and set `connector->polled =
DRM_CONNECTOR_POLL_HPD`. Neither is worth doing before the gate has shown the
vendor tree driving a monitor on unit A.
