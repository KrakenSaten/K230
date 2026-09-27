# HDMI: the kernel-side causes and the fix

Recorded 2026-09-27 on branch `feat/k230-hdmi-out`, after the unit A gate of
the same day (`HDMI_GATE.md`: bridge, HPD and EDID proven, no picture at 480p
and 720p, stripes at 1080p, shell on 2560x1440). This sheet is the desk work
that the gate's "Why no picture" asked for, and the evidence behind kernel
patches 0070 and 0071 in `platforms/k230/patches/linux/` (ADR-011, Proposed).

Evidence classes as in `T-DISPLAY-K230.md`: VERIFIED (unit A record, or
reproduced here from the pinned sources by reading the code that ran),
DOCUMENTED (vendor source, upstream source, device tree), ASSUMED (inference).

## Sources

| Short | What | Where |
| --- | --- | --- |
| K | The kernel that is on unit A's card: `linux-xuantie-kernel` `7d4e1f4` with the SDK and BSP `linux/*.patch` stack applied by Buildroot. The build tree's `arch/riscv/boot/Image` is byte-identical to `/boot/Image` on the v0.1.0 card (sha256 `536d4770…`), VERIFIED | WSL `output/k230_pocketos_defconfig/build/linux-7d4e1f4…/`; copies of the files read in `out/hdmi-kernel/drm/` |
| K0 | The same kernel before any SDK/BSP patch (the pinned tarball) | `out/hdmi-kernel/upstream/` |
| P | The patch stack as applied, in order | `.applied_patches_list` of K; SDK `buildroot-overlay/linux/0001…0064` |
| RT | The K230 RT-Smart SDK LT9611 connector and its per-mode table | `vendor/T-Display-K230_canmv_rt` `abb0709`: `canmv_k230/src/rtsmart/mpp/kernel/connector/src/lt9611.c`, `.../userapps/src/connector/mpi_connector.c` |
| RTH | RT-Smart sample header with named PHY constants | same tree, `userapps/sample/sample_vo/vo_test_case.h` |
| KMB | The DesignWare D-PHY tables as they ship in the pinned kernel | K0 `drivers/gpu/drm/kmb/kmb_dsi.c` (`vco_table[]`, `mipi_hs_freq_range[]`) |
| DW | The DesignWare DSI host driver, for register bit names | K0 `drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi.c` |
| UA | Unit A, 2026-09-27: serial console `out/hdmi-gate/serial-boot.log`, gate outputs `g*.out`, the gate sheet | `out/hdmi-gate/`, `HDMI_GATE.md` |

## 1. What the gate measured, read against the code

| Mode | DSI (from the kernel log) | Bridge video check | Monitor | Evidence |
| --- | --- | --- | --- | --- |
| 2560x1440@60 (fbdev and shell) | 297 MHz, 4 lanes, `phy_target=891000 kHz`, `auto_phy_freq=1782000 kHz` (1.78 Gbit/s per lane), `auto_voc=0x3`, `hsfreq=0x96` | all zero, later garbage (1920/1050/1125) | no signal | UA serial 1.8 s, 643 s |
| 720x480@60 | 27 MHz, `phy_target=81000`, `auto_phy_freq=162000`, `auto_m=25 auto_n=0 auto_voc=0x37 hsfreq=0x96` | `hactive_a=720 vactive=480 v_total=525 h_total_sysclk=859` (exact: 858 px at 27 MHz) | no signal | UA serial 72.9 s, `g14_diag.out` |
| 1280x720@60 | 74.25 MHz, 445.5 Mbit/s per lane | `1280/720/750` | no signal | UA gate sheet step 8 |
| 1920x1080@60 | 148.5 MHz, 891 Mbit/s per lane | `1920/1080/1125` | locks, black/white flickering stripes | UA gate sheet step 8 |

Two facts follow. The DSI link delivers frames the bridge can count at every
rate, so the K230 VO timing and the bridge's MIPI receiver are not where the
picture is lost. The bridge's HDMI transmitter never produced a signal the
monitor accepted below 1080p, and at 1080p produced a locked but corrupt one.

## 2. Cause A: `mode_valid` accepts every mode (VERIFIED)

K `lontium-lt9611.c`, SDK patch 0001 ("modify lt9611 for k230"):

```c
static struct lt9611_mode lt9611_modes[] = {
	{ 1920, 1080, 60, 4, 1 },
	{ 1920, 1080, 30, 4, 1 },
	// { 1280, 720, 60, 4, 1 }, ...
};
static struct lt9611_mode *lt9611_find_mode(const struct drm_display_mode *mode)
{
	if (mode->clock > 300000 || mode->vtotal > 4095 || mode->htotal > 4095)
		return NULL;
	return &lt9611_modes[0];
}
```

Both `mode_valid` hooks call this, so every EDID mode up to 300 MHz is
offered. The DRM probe helper sorts the list with the preferred mode first and
then by size, fbdev and LVGL take the first entry, and unit A's monitor
prefers 2560x1440@60 (241.5 MHz, quantised to 297 MHz by the DSI encoder). At
297 MHz the DSI computes a 1.78 Gbit/s lane rate (`auto_phy_freq=1782000`),
above the D-PHY's 1.5 Gbit/s and far above the bridge's input rating; the
bridge counted nothing (UA). The table entry itself is otherwise unused: the
driver never reads `lanes` or `intfs` for anything but the `intfs > 1` check.

The only way userspace can avoid this today is to know the bridge's limits,
which is what ADR-002 and the gate say it must not need to.

## 3. Cause B: register 0x831a loses a nibble (VERIFIED by reading)

The bridge takes its horizontal timing in `lt9611_mipi_video_setup()`. K:

```c
regmap_write(lt9611->regmap, 0x831a, (u8)((hfront_porch / 256)<<4 + hsync_porch / 256));
```

`+` binds tighter than `<<`, so this is `(hfront_porch / 256) << (4 +
hsync_porch / 256)`; with `hfront_porch < 256` it is always 0. Register 0x831a
is `hfront_porch[11:8] << 4 | (hsync_len + hback_porch)[11:8]`; upstream (K0)
writes `(u8)(hsync_porch / 256) | ((hfront_porch / 256) << 4)` and RT-Smart
writes `((hfront_porch / 256) << 4) + (hsync_len + hback_porch) / 256`
(DOCUMENTED, both). Effect per mode, with `hsync_porch = hsync_len + hback_porch`:

| Mode | hsync_len | hback_porch | hsync_porch | 0x831a should be | K wrote | Bridge's idea of sync+back porch |
| --- | --- | --- | --- | --- | --- | --- |
| 1920x1080@60 | 44 | 148 | 192 | 0x00 | 0x00 | 192 (right) |
| 1280x720@60 | 40 | 220 | **260** | **0x01** | **0x00** | **4** |
| 1280x720@50 | 40 | 220 | 260 | 0x01 | 0x00 | 4 |
| 720x480@60 | 62 | 60 | 122 | 0x00 | 0x00 | 122 (right) |
| 2560x1440@60 | 32 | 80 | 112 | 0x00 | 0x00 | 112 (right) |

So the bug breaks exactly 720p, which is one of the two modes that gave no
signal. It does not explain 480p (see §6).

## 4. Cause C: the bridge's 4-lane link is brought up as the panel's (DOCUMENTED)

### 4.1 What the BSP changed

BSP patch 0025 ("Fix DSI for RM69A10 2-lane panel") edited the shared DSI
host, not the panel driver:

- `canaan_dsi_clk_cfg()`: `k230_dsi_config_4lan_phy(…)` became
  `k230_dsi_config_2lan_phy(…)` for every device (diff K0 → K). The new 2-lane
  function differs from the original 4-lane one in the `DSI_PHY_RSTZ` writes:
  `0x5` then `0x7` instead of `0xd` then `0xf`. Per DW, bit 3 is
  `phy_forcepll`, bit 2 `phy_enableclk`, bit 1 `phy_unrstz`, bit 0
  `phy_unshutdownz`; the upstream host driver writes all four
  (`PHY_ENFORCEPLL | PHY_ENABLECLK | PHY_UNRSTZ | PHY_UNSHUTDOWNZ`). The
  patch's comment "reset only lane 0 and 1" describes something the register
  does not do; lane count is set separately in `PHY_IF_CFG` (`0x2803` = 4
  lanes, unchanged).
- `DPI_COLOR_CODING` 0x105 → 0x005 (RGB888 either way; bit 8 is the 18-bit
  loosely-packed flag, ignored for 24-bit) and `VID_MODE_CFG` 0xbf02 → 0x3f02
  (burst mode either way; bit 15 `lp_cmd_en` dropped). Neither concerns the
  bridge; RT-Smart drives the LT9611 in `K_BURST_MODE` too (RT), so burst
  stays.

The LT9611 asks the host for 4 lanes (`dsi->lanes = 4`, K and K0) and the
RT-Smart connector table lists every LT9611 mode as `K_DSI_4LAN` (RT). Since
patch 0025 the 4-lane device has been running on the 2-lane sequence.

### 4.2 `hsfreqrange` is fixed at the 450 Mbit/s code

`canaan_dsi_clk_cfg()` passes `0x96` at every rate (K0 and K). The K230 PHY
is programmed through DesignWare test codes: `k230_dsi_write_phy_reg(dsi,
0x44, hsfreq)` with the comment "set 445.5 hsfreqrange = 0010110" (K
`canaan_phy.c`). Test code 0x44 holds `hsfreqrange_ovr[6:0]` and
`hsfreqrange_ovr_en` in bit 7 (KMB `set_lane_data_rate()`: `(code & 0x7f) |
(1 << 7)`), so 0x96 is code 0x16, which KMB's table gives for 450 Mbit/s.
The K230's own sample header agrees with that table where it names a rate:
`TXPHY_445_5_HS_FREQ 0x96` (450 → 0x16), `TXPHY_216_HS_FREQ 0x93` (220 →
0x13), `TXPHY_297_HS_FREQ 0xa5` (325 → 0x25) (RTH). The table's entry for
the 1080p lane rate is different:

| Lane rate | KMB code (first range covering the rate) | What K programmed | RT-Smart programmed |
| --- | --- | --- | --- |
| 445.5 Mbit/s (720p, 4 lanes) | 450 → 0x16 | 0x16 | 0x16 (`0x96`) |
| 594 Mbit/s (panel, 2 lanes) | 600 → 0x07 | 0x16 | 0x16 (panel runs on it: VERIFIED) |
| 891 Mbit/s (1080p, 4 lanes) | 900 → 0x29 | 0x16 | 0x16 (`0x96`, RT table) |

RT-Smart uses 0x96 for every LT9611 mode (RT), so a fixed code is not by
itself fatal; the panel proves that at 594 Mbit/s. `hsfreqrange` sets the
PHY's internal HS timing counters for the rate range, so a code two ranges
low is a deviation whose cost grows with the rate. At 891 Mbit/s the bridge
counted 1050 of 1080 active lines while being fed the 2560x1440 stream (UA
643 s) - that stream was out of range anyway, so it is not proof, and the
1080p pattern's own count was complete (`1920/1080/1125`). The stripes at
1080p remain unexplained by any single reading; this is the first suspect
(ASSUMED).

### 4.3 The VCO table differs from the databook and from RT-Smart

`canaan_dsi_clk_cfg()` picks `voc` (the PHY's `vco_cntrl`, bits [5:4] =
output divider 2^P, bits [3:0] = VCO band) from the PHY DDR clock
`phy_clk_freq = pclk * 24 / lanes / 2`, then searches M and N so that
`24 MHz * M / N` hits `phy_clk_freq * 2^P`, and programs `M - 2`, `N - 1`,
`voc`. The lane bit rate it reports is `48 MHz * M / N / 2^P`.

| DDR clock (kHz) | K table | KMB `vco_table[]` (freq below, code) | RT-Smart evidence |
| --- | --- | --- | --- |
| < 55000 | 0x3f | < 52 MHz: 0x3f | |
| < 82500 | 0x37 | < 80 MHz: 0x39 | 640x480: `{15, 394, 0x39}` (74.25 MHz DDR) |
| < 110000 | 0x2f | < 105 MHz: 0x2f | |
| < 165000 | 0x27 | < 160 MHz: 0x29 | |
| < 220000 | 0x1f | < 210 MHz: 0x1f | |
| < 330000 | 0x17 | < 320 MHz: 0x19 | 720p: `{15, 295, 0x19}` (222.75 MHz); rm69a10: `{4, 97, 0x19}` (237.6 MHz) |
| < 440000 | 0x0f | < 420 MHz: 0x0f | |
| < 660000 | 0x07 | < 630 MHz: 0x09 | 1080p: `{15, 295, 0x09}` (445.5 MHz) |
| < 1149000 | 0x03 | < 1100 MHz: 0x03 | |
| else | 0x01 | 0x01 | |

The odd rows are the difference: K uses x7 codes where the databook table
(KMB) and the RT-Smart constants both use x9. The panel runs on 0x17 at
297 MHz DDR (VERIFIED), so 0x17 is not a dead code; whether 0x07 at 445.5 MHz
locks cleanly is not known (ASSUMED risk). RT-Smart's `{n, m, voc}` for
1080p60 and 720p60 are exactly what the K search produces for M and N
(M = 297, N = 16, i.e. registers 295 and 15) with the databook `voc`, so the
databook table makes the Linux driver program the RT-Smart values without
copying constants.

### 4.4 What patch 0071 programs, derived

For a 4-lane device, `canaan_dsi_clk_cfg()` takes `voc` from the KMB
`vco_table[]` (first entry whose bound the DDR clock is below) and
`hsfreqrange` from the KMB `mipi_hs_freq_range[]` (first entry whose rate is
at or above the lane rate, as KMB's `set_lane_data_rate()` selects), then
calls the original `k230_dsi_config_4lan_phy()`. The 2-lane path is the same
code as before, moved into `canaan_dsi_panel_vco_cntrl()`, with the 2-lane
sequence and `0x96`.

| Quantity | 1920x1080@60 | 1280x720@60 | Panel 568x1232 (unchanged) |
| --- | --- | --- | --- |
| `div = round(594000 / clock)`, adjusted clock | 4, 148500 kHz | 8, 74250 kHz | 12, 49500 kHz |
| Lanes | 4 | 4 | 2 |
| `phy_clk_freq` (DDR) = clock x 24 / lanes / 2 | 445500 | 222750 | 297000 |
| `voc` | 0x09 (KMB: < 630 MHz) | 0x19 (KMB: < 320 MHz) | 0x17 (K table) |
| VCO target = DDR x 2^P | 445500 x 1 | 222750 x 2 = 445500 | 297000 x 2 = 594000 |
| M / N search (`24 MHz x M / N` = VCO) | 297 / 16 (exact) | 297 / 16 (exact) | 99 / 4 (exact) |
| Registers `M - 2`, `N - 1` | 295, 15 | 295, 15 | 97, 3 |
| Lane rate reported = 48 x M / N / 2^P | 891000 kbit/s | 445500 | 594000 |
| `hsfreqrange` | 0x29 (900) → writes 0xa9 | 0x16 (450) → 0x96 | 0x96 (as before) |
| PHY sequence | `k230_dsi_config_4lan_phy` (K0's) | same | `k230_dsi_config_2lan_phy` (as before) |
| RT-Smart for this mode | `{15, 295, 0x09, 0x96}` | `{15, 295, 0x19, 0x96}` | `{4, 97, 0x19, 0x96}` at 39.6 MHz (different timing) |
| DSI lane-byte-clock timings (`lbcc`) HSA / HBP / HLINE | 33 / 111 / 1650 | 30 / 165 / 1238 | as before |

The one value that departs from RT-Smart is 1080p's `hsfreqrange` (0x29 from
the table against RT-Smart's fixed 0x16). The table is what the register
means (KMB, RTH agree on three named rates); RT-Smart's fixed code is a
vendor shortcut that the bench can fall back to if 1080p behaves worse with
the table value.

## 5. What patch 0070 programs, derived (the bridge)

The bridge's PLL, pixel-clock-recovery (PCR) and timing registers are already
derived from the mode in K, and the same way as in RT-Smart (RT
`lt9611_setup_pll/pcr/timing` against K `lt9611_pll_setup/pcr_setup/
mipi_video_setup`: the same register lists, the same `pcr_m` formula, the
same post-divider thresholds). The gate's earlier reading "fixed for 1080p"
was wrong in that sense; what is fixed is the mode table's *use*. Patch 0070
changes two things and leaves the register recipe alone:

1. `lt9611_find_mode()` matches `hdisplay`, `vdisplay` and
   `drm_mode_vrefresh()` against the table and refuses interlaced modes; the
   table holds the 4-lane one-port modes that K0 and RT list in common:
   1920x1080@60, 1920x1080@30, 1280x720@60, 1280x720@50. 720x480@60 (K0
   only, 4 lanes), 640x480@60 (RT: 4 lanes; K0: 2 lanes) and 720x576@50 (K0:
   2 lanes) are left out until one of them is shown to work here; 480p gave
   no signal in the gate and nothing in this analysis explains that yet.
2. 0x831a as in §3.

Values the unchanged code programs for the two target modes (VERIFIED by
reading K; the formulas are RT's too):

| Register / value | 1920x1080@60 | 1280x720@60 |
| --- | --- | --- |
| 0x812d, `postdiv` (`> 150000` → 1, `> 80000` → 2, else 4) | 0x99, 2 (148500 is not > 150000) | 0xaa, 4 |
| `pcr_m = clock x 5 x postdiv / 27000 - 1` | 1485000 / 27000 - 1 = 54 = 0x36 | 74250 x 20 / 27000 - 1 = 54 = 0x36 |
| 0x8326 in `pll_setup` / in `pcr_setup` | 0xb6 / 0x36 | 0xb6 / 0x36 |
| 0x82e3 / e4 / e5 = clock/2 split 16 / 8 / 0 | 74250 → 0x01 / 0x22 / 0x0a | 37125 → 0x00 / 0x91 / 0x05 |
| 0x830d/0e v_total | 1125 → 0x04 0x65 | 750 → 0x02 0xee |
| 0x830f/10 vactive | 1080 → 0x04 0x38 | 720 → 0x02 0xd0 |
| 0x8311/12 h_total | 2200 → 0x08 0x98 | 1650 → 0x06 0x72 |
| 0x8313/14 hactive | 1920 → 0x07 0x80 | 1280 → 0x05 0x00 |
| 0x8315 vsync_len, 0x8316 hsync_len | 0x05, 0x2c | 0x05, 0x28 |
| 0x8317 vfront_porch, 0x8318 vsync+vback | 0x04, 0x29 (5+36) | 0x05, 0x19 (5+20) |
| 0x8319 hfront_porch | 0x58 (88) | 0x6e (110) |
| 0x831a, 0x831b (patched) | 0x00, 0xc0 (192) | **0x01**, 0x04 (260) |

`pcr_m` is 54 in K and RT and 55 in K0 (K0 has no `- 1`); RT's value is the
one Canaan ran on this SoC, so it stays.

## 6. Not changed, and what is still open

- **Burst mode.** The LT9611 requests `MIPI_DSI_MODE_VIDEO_SYNC_PULSE`, the
  host ignores mode flags and always programs burst (`VID_MODE_CFG` 0x3f02),
  and RT-Smart drives the LT9611 in `K_BURST_MODE`. Kept.
- **VO colour path.** BSP patch 0033 turned off the VO's OSD RGB→YUV and the
  display YUV→RGB stages together (K0 had both on). The panel's colours are
  right with that (VERIFIED, every gate since), so RGB reaches the DSI. Kept.
- **Stale stream during bridge set-up.** `canaan_dsi_encoder_disable()` does
  nothing for a bridge, so every mode change reprograms the LT9611 (PLL, PCR
  reset) while the *previous* DSI stream is still running, then re-inits the
  PHY live. Upstream's host drivers stop the link on disable and RT-Smart
  resets the display block before init, so both vendor references set the
  bridge up with no video present. This is the remaining structural
  difference and the leading suspect for 480p's "no signal" (ASSUMED). Not
  in these patches: it would change the DSI host's disable path, and the
  round-2 gate can show whether it is needed (a 720p or 1080p mode switch
  from a running 1080p console is exactly that case).
- **480p.** Not in the table. Its 162 Mbit/s lane rate got `voc=0x37`
  (KMB: 0x2f, output divider 4) and `hsfreq=0x96` (KMB: 0x12), and RT-Smart
  has no 720x480 entry to compare with.
- **Hotplug to userspace** (`drm_kms_helper_hotplug_event` commented out):
  unchanged, `HDMI_OUTPUT.md` §8.

## 7. Build (VERIFIED 2026-09-27)

Pinned flow, WSL, Xuantie-900 gcc 14.1.1 (`riscv64-unknown-linux-gnu-gcc
(Xuantie-900 linux-6.6.0 glibc gcc Toolchain V3.0.2 B-20250410)`), SDK
`22d02c6`, BSP `bb831ab`, kernel `7d4e1f4`:

1. The two patches copied into `k230_linux_sdk/buildroot-overlay/linux/` (the
   step `apply_to_sdk.sh` now does); `patch -p1 --dry-run` against the build
   tree that produced the card's kernel: both apply with no offset, no fuzz.
2. `make CONF=k230_pocketos_defconfig linux-dirclean`, then `make … linux`:
   Buildroot re-extracted the tarball, applied 0001-0064, then
   `0070-drm-bridge-lt9611-k230-mode-table-and-0x831a.patch` and
   `0071-drm-canaan-dsi-4-lane-bridge-dphy-tables.patch` ("patching file …",
   no hunk message), then the vendor's unnumbered suspend patch. 17 minutes,
   `rc=0`.
3. Warnings: 44 lines in the whole kernel build, all vendor code.
   `canaan_dsi.c`: none. `lontium-lt9611.c`: the ten pre-existing
   unused-function/variable/label warnings at lines 591-1199 (the ops the
   vendor commented out), none in the patched hunks. No `-Werror` is in
   effect for the kernel.
4. Result: `Image` 19,084,288 bytes, sha256 `7ab9b4bf…`, against the card's
   `536d4770…` (same size, same config). Both DTBs byte-identical to the
   card's (`3ac313c4…`, `2a6b49ad…`): no device-tree change. Log and Image in
   `out/hdmi-kernel/build/` on the bench PC.

Host tests on the branch: `tests/kernel_patches_test.sh` (22 checks),
`provenance_state_test.sh`, `build_provenance_test.sh`,
`display_boot_test.sh` all 0 failures under WSL.

## 8. Hardware, round 2 (unit A, 2026-09-27, no one at the bench)

Kernel-only deployment: `Image` `7ab9b4bf…` copied to `/boot/Image` with the
v0.1.0 kernel kept as `/boot/Image.orig`; DTBs, rootfs and tools untouched.
The same DUS D27QP monitor was attached and powered (HPD register 0x825e read
`0x7d` before the switch). Every observation below is from the kernel log,
the DRM sysfs and the bridge's own counters; **nobody looked at the monitor**,
so "picture" is not claimed. Record: `HDMI_GATE.md` "Round 2"; raw outputs
`out/hdmi-gate/r2_*.out`, serial logs `r2-serial-boot-*.log`.

| Step | Result | Evidence |
| --- | --- | --- |
| LCD tree on the patched kernel | PASS: `6.6.36 #2 … Sep 27 15:48:51`, panel 568x1232 connected and enabled, DSI line identical to the v0.1.0 kernel's (`lanes=2 … auto_m=97 auto_n=3 auto_voc=0x17 hsfreq=0x96`), shell running, 0 restarts, touch `event0/1`, `/dev/spidev0.0`, radio `rx`, no oops | `r2_lcd.out` |
| HDMI tree boot (fbdev's own modeset, no stream running) | PASS: connector offers 7 modes, all 1920x1080 or 1280x720, **no 2560x1440**; fbdev takes 1920x1080@60; DSI `lanes=4 … auto_m=295 auto_n=15 auto_voc=0x9 hsfreq=0xa9` (exactly §4.4); bridge `hactive_a=1920 vactive=1080 v_total=1125 h_total_sysclk=400` (complete, exact); `fb0` registered; shell comes up on 1080x1920 (rotation 270), 0 restarts; Wi-Fi/SSH up at 30 s; no oops | `r2_hdmi.out` |
| 1280x720@60 pattern (switch under the running 1080p stream) | PASS at the bridge: DSI `auto_voc=0x19 hsfreq=0x96` (§4.4, = RT-Smart), bridge told `hsync_porch: 260`, video check `1280/720/750`, `h_total_sysclk=600` (1650 px at 74.25 MHz in 27 MHz sysclk: exact); mode set in 824 ms; 40 s hold, CPU idle, `rc=0`, previous state restored | `r2_pat_p720.out`, `p720.dmesg` |
| 1920x1080@60 restore (switch back from 720p under the running 720p stream) | PASS at the bridge: DSI 1080p values as above, video check `1920/1080/1125/400` | `p720.dmesg` at 106.9 s |
| 1920x1080@60 pattern | Mode already current (set in 2 ms, page flip only); 40 s hold, `rc=0`, no oops | `r2_pat_p1080.out` |
| Back to the panel, v0.1.0 kernel restored | PASS: `pos-display-boot lcd`, `/boot/Image` = `536d4770…`, reboot; `6.6.36 #2 … Sep 4`, panel, shell, touch, spidev, radio `rx`, no `force_dtb` | `r2_lcdfinal.out` |

What round 2 proves (VERIFIED): the patched kernel boots both trees; the
panel path is unchanged in every logged value; the kernel no longer offers
modes outside the table; the 4-lane PHY is programmed with the databook
values, which are RT-Smart's; the bridge receives complete, exactly timed
frames at 720p and 1080p, including across mode switches, and the 720p
timing register carries the right value.

## 9. Hardware, round 3 (the owner looks, 2026-09-27)

Full table in `HDMI_GATE.md` "Round 3". The decisive runs used the DSI
host's video pattern generator (DesignWare `VID_MODE_CFG` bit 16, `vpg_en`,
as named in K0 `drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi.c`; switched on
with `devmem 0x90850038` while a mode was held) and `modetest` in RGB565:

| What fed the DSI | 1280x720@60 | 1920x1080@60 |
| --- | --- | --- |
| The DSI's own colour bars (no VO, no framebuffer) | **bars on the monitor** | **bars on the monitor** |
| VO, XRGB8888 plane (`pos-drmtest`, fbdev) | no signal | locked, black |
| VO, RGB565 plane (`modetest`) | no signal | black on the first set, **bars** on a re-set |

So patches 0070 and 0071 are proven on the glass: everything from the DSI
PHY through the bridge to the monitor is right at both rates. What remains
is in the K230 VO (what feeds the DSI):

- **XRGB8888 planes never fetch (DOCUMENTED cause).** BSP patches 0026/0030
  added XRGB8888 with OSD `DMA_CTRL = 0x40` and changed ARGB8888 to `0x0`.
  The vendor U-Boot logo code (`board/canaan/common/logo/display_logo.c`,
  `kd_vo_osd_set_dma_request()`) writes bits [3:0] of that register as the
  DMA request enable (`0xf`) and bits [5:4] as the DMA byte map. K0 used
  `0x4F` for every format; RGB565 still does, and it is the only format that
  showed a picture. Fix candidate: `0x4F` for XRGB8888 and ARGB8888 (the
  BSP's "rb swap" was bit 6, which `0x4F` keeps). One constant each.
- **The VO's 720p stream is refused by the bridge's transmitter, and a
  1080p RGB565 set works only as a re-set.** Not understood. The VO timing
  registers read exactly as derived (`TOTAL_SIZE`, `XZONE`, `YZONE`, `DRAW`,
  `HSYNC` 2..5, `VSYNC` 0..0, the same as RT-Smart's `sync_attr`), the
  bridge counts the right frame, and the DSI is identical between the
  failing VO run and the passing generator run. Candidates (ASSUMED): the
  BSP's deferred config load (patch 0043: `ADDR_SEL_MODE 0x1100`, address
  and `REG_LOAD_CTL` written from the vblank IRQ) interacting with a mode
  change; `k230_display_rst()` at every CRTC enable; the VO's per-layer
  line-buffer setting (`0x701`, meaning undocumented). The register
  readbacks taken (`out/hdmi-gate/r3_regs_*.txt`) do not discriminate:
  `ADDR_SEL 0x1100` and `IRQ_STATUS` bit 28 appeared in passing runs too.

Unit A was returned to the v0.1.0 kernel and the LCD tree after round 3
(`HDMI_GATE.md` R3.8).
