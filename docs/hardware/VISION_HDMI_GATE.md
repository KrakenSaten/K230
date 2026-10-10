# Vision on HDMI: unit B gate

Branch `feat/vision-hdmi`. What it puts on a unit: a Doors HDMI device tree
(kernel patch 0075, `HDMI_OUTPUT.md` §10) and a shell that treats the
monitor as an external display (rotation 0, no corner insets, the camera
turned by how the unit is held) with keyboard navigation in Vision (DS
§38.10). HDMI stays an explicit, boot-time choice: `pos-display-boot hdmi
&& reboot`, and back with `pos-display-boot lcd && reboot`. Nothing here
switches outputs on its own, reboots on unplug, or writes to the bridge.

Evidence classes as in `T-DISPLAY-K230.md`.

**Status: NOT RUN.** Nothing on unit B has been changed for this gate. The
read-only checks of 2026-10-10 are under "Unit B before the gate".

## Unit B before the gate (read-only, 2026-10-10)

| Item | Value |
| --- | --- |
| Image | v0.3.6, `BUILD_ID=9ad64a1`, model `Canaan CanMV-K230 with RM69A10 OLED` |
| Kernel | `/boot/Image` md5 `f73df255…`; carries patch 0073's `DSI mode padded` string |
| Panel tree | `/boot/k230-canmv-rm69a10.dtb` sha256 `7420e8e7…` (60,557 B, with 0074's BOOT key); `k.dtb` links to it |
| HDMI tree | `/boot/k230-canmv-rm69a10-hdmi.dtb` sha256 `2a6b49ad…` (56,410 B), the vendor tree; `/boot/k230-canmv-v3.dtb` is the same file |
| Pointers | `lcd_dtb` and `hdmi_dtb` name the two trees above; **no** `force_dtb` |
| Boot partition | 47.7 MB free |
| Shell | `doors-shell` md5 `21202a56…` (build e9469ab); `pos-vision` `5ac7992c…`, `pos-camera` `a83641cd…`, `pos-display-boot` `8821097b…`, `pos-drmtest` `215d4d37…` |
| Rollback dirs already there | `/root/rollback-deskbuddy-opus`, `/root/rollback-rift-basemap` |
| Devices | `gc2093` at `0-0037`, `/dev/video0-4`, `/dev/spidev0.0`, `goodix_ts`, `K230 PMU Power Key`, `K230 BOOT Key` |
| Keyboard base | attached by the owner 17:53Z: TCA8418 ready, rotation 270 (1232x568); F2 delivered twice as `settings` (owner key presses, 17:57Z) |
| Settings | `display_rotation=automatic`, `text_size=large` |
| Memory | MemAvailable 904 MB, CmaFree 411 MB |
| Consoles | COM12 on the bench PC is unit B's serial console (not opened for this sheet); SSH over Wi-Fi at .187 |

## Artifacts

| File on the unit | Source | Identity |
| --- | --- | --- |
| `/boot/k230-canmv-rm69a10-hdmi.dtb` (replaced) | the SDK kernel tree's own `cpp` + `dtc` on `k230-canmv-rm69a10-hdmi.dts` after 0075. The same recipe rebuilds unit B's panel tree byte for byte (sha256 `7420e8e7…`), so it is the DTB a kernel build makes | sha256 `bb346f9e…`, 59,754 B |
| `/usr/bin/doors-shell` (replaced) | `9d22211` from a clean clone, cross-built with the pinned Xuantie toolchain against the SDK sysroot (`Doors 0.3.6 build 9d22211`, 0 first-party warnings) | md5 `9fb87816…`, sha256 `a70e7eaa…`, 4,464,568 B |

The DTB's md5 is `58925544…`. Bench copies and the session scripts (`h0_state.sh`,
`h2_backup_shell.sh`, `h3_dtb.sh`, `stage/RESTORE.sh`) are in
`out/vision-hdmi-gate/` on the bench PC; none has been run.

Not changed: `/boot/Image`, the panel tree, `k.dtb`, the pointer files,
U-Boot, `pos-vision`, `pos-camera`, `pos-display-boot`, every setting.

The decompiled Doors HDMI tree differs from the panel tree in exactly: the
model string, `gt9895@5d` replaced by `hdmi-bridge@3b`, `panel@0` gone and
the DSI port's endpoint pointing at the bridge, `canaan,skip-fbdev-setup`
gone, and the `connector` node - plus the phandle numbers. Camera
(`gc2093@37`, I2C4 pins, `mipi.0` with MCLK1), `spi0`/spidev, `uart1`,
`uart3`, the keyboard pins, `gpio-keys` (BOOT) and `mmc_sd0` (4-bit SDIO
Wi-Fi) are the panel tree's.

## Backups (H2, before any file changes)

`/root/rollback-vision-hdmi/` gets:

- `doors-shell` (md5 `21202a56…`), the shell now installed;
- `k230-canmv-rm69a10-hdmi.dtb` (sha256 `2a6b49ad…`), the vendor HDMI tree
  (a second copy stays on `/boot` as `k230-canmv-v3.dtb`);
- `boot.md5`: `md5sum /boot/*` before the change;
- `RESTORE.sh`: puts both files back, verifies their sums, removes
  `/boot/force_dtb`, syncs, and restarts the shell. It does not reboot.

## Recovery, if HDMI gives no picture or no way in

In order. Steps 1-2 are rehearsed in H1 before anything is changed.

1. **SSH over Wi-Fi** (the Doors tree keeps the panel tree's SDIO Wi-Fi):
   `pos-display-boot lcd && reboot`. Expected to work as on a panel boot;
   VERIFIED for the vendor tree on unit A (Wi-Fi up 29 s after an HDMI
   boot), to be VERIFIED for the Doors tree in H4.
2. **COM12, Linux running:** the same command from the console's root
   shell.
3. **COM12, Linux not coming up:** at `Hit any key to stop autoboot`
   press a key and boot the panel tree once with
   `ext4load mmc ${mmc_boot_dev_num}:1 0x3000000 /fw_jump_add_uboot_head.bin && ext4load mmc ${mmc_boot_dev_num}:1 0x200000 /Image && ext4load mmc ${mmc_boot_dev_num}:1 0x2200000 /k230-canmv-rm69a10.dtb && bootm 0x3000000 - 0x2200000`,
   then `pos-display-boot lcd` (or `RESTORE.sh`). Never tried on any unit
   until H1.
4. **The card in a Linux PC:** delete `force_dtb` from partition 1 (ext4).

`pos-display-boot lcd` alone always returns the next boot to the panel: it
removes `force_dtb`, and with none U-Boot loads `lcd_dtb`. It touches no
device tree, so it works whichever HDMI tree is installed.

## Hardware session

Owner at the bench, monitor on unit B's HDMI, keyboard base attached.

### H0. State (read-only)

The table above again; stop if anything differs.

### H1. Recovery rehearsal (vendor tree, nothing installed)

1. COM12: a root shell answers (`uname -a`).
2. `pos-display-boot hdmi && reboot`. Record: the monitor (console text,
   then the shell), the AMOLED (dark expected), the SSH time after power
   on. Expected from unit A's gate: picture at 1080p, no camera, no touch,
   radio unavailable.
3. Over SSH: `pos-display-boot lcd && reboot`. Panel, touch, radio back.
4. COM12: stop autoboot, run the step-3 command above, check that the panel
   tree boots (`cat /proc/device-tree/model`), `reboot`.

Pass: recovery steps 1-3 each proven once.

### H2. Backups and the shell, on the panel

Backups as above, then the new shell, `S90doors-shell restart`. Panel
regression, the keyboard base attached: rotation 270, the launcher, Camera
and Vision upright as before (`hold_rotation` equals the display's rotation
on the panel), `doors shell info` reports `"external":false`. Vision's keys
on the panel: the ring appears with the first arrow, Enter opens MODE,
Esc closes it.

### H3. The Doors HDMI tree

Copy the DTB to `/boot/k230-canmv-rm69a10-hdmi.dtb`, check its sha256, `sync`,
`pos-display-boot hdmi && reboot`.

### H4. HDMI checks

| Check | How | Pass |
| --- | --- | --- |
| Boot and picture | the monitor | console text then the launcher, landscape, upright, at 1920x1080 |
| Model and tree | `cat /proc/device-tree/model` | `Canaan CanMV-K230 with LT9611 HDMI` |
| Display | `doors shell info` | `external` true, rotation 0, 1920x1080, `hold_rotation` 270 with the base |
| Wi-Fi / SSH | time to SSH | answers (recovery step 1 for this tree) |
| Camera | `dmesg \| grep -E 'gc2093\|vvcam-mipi'`, `ls /dev/video2` | the sensor probes as on the panel |
| Radio | `ls /dev/spidev0.0`, `pos radio status` | spidev present, radio `rx` |
| Keys | keyboard base, BOOT key | the base is present; F1 Home, F2 Settings; BOOT short press Back |
| Vision | from the launcher with the arrows and Enter (it sits in the Apps folder), then keys only | picture upright for the way the unit stands; boxes on their objects; arrows/Enter reach MODE, the picker and every mode's buttons; Esc and BOOT close a sheet |
| Load | `top -b -n 5 -d 2` while Vision runs | record helper and shell CPU, frame rate from the status line |
| Camera app | open, look, leave | upright as Vision's |

Not tested here, by the owner's scope: unplugging the cable (no automatic
return), a monitor that is off, 720p.

### H5. Back to the panel

`pos-display-boot lcd && reboot`: panel, touch, radio, keyboard. The
owner decides whether unit B keeps the Doors HDMI tree and the new shell or
runs `RESTORE.sh`.

## Results

Not run.
