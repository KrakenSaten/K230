# Vision on HDMI: unit B gate

Branch `feat/vision-hdmi`. What it puts on a unit: a Doors HDMI device tree
(kernel patch 0075, `HDMI_OUTPUT.md` §10) and a shell that treats the
monitor as an external display (rotation 0, no corner insets, the camera
turned by how the unit is held) with keyboard navigation in Vision (DS
§38.10). HDMI stays an explicit, boot-time choice: `pos-display-boot hdmi
&& reboot`, and back with `pos-display-boot lcd && reboot`. Nothing here
switches outputs on its own, reboots on unplug, or writes to the bridge.

Evidence classes as in `T-DISPLAY-K230.md`.

**Status: RUN on unit B 2026-10-13 - PASS** for explicit HDMI selection,
the Doors HDMI tree (camera, LoRa, Wi-Fi), an upright shell and camera
picture, Vision with aligned boxes, the return to the panel and touch
(owner confirmed visually). Physical keyboard navigation is **UNTESTED**
(no keyboard base). Details and limitations under "Results". The read-only
checks of 2026-10-10 are under "Unit B before the gate"; those of
2026-10-13, which supersede them where they differ, directly below.

## Unit B on 2026-10-13 (read-only, over COM12) and the kit that matches it

Unit B was given the NinjaOne build after the first kit was made, so that kit's
shell (9d22211) would have removed NinjaOne. Read over the serial console (root
shell, no password; Wi-Fi had no carrier, none of its saved networks in range):

| Item | Value |
| --- | --- |
| Image, kernel, all `/boot` files | as on 2026-10-10 below: v0.3.6 `9ad64a1`, `Image` `f73df255…`, panel tree `7420e8e7…`, HDMI tree the vendor `2a6b49ad…`, no `force_dtb` |
| Shell | `doors-shell` md5 `f71aafaf…`, build `66f9bf0` (`feat/ninjaone-app`); the earlier shell is in `/root/rollback-ninjaone` |
| NinjaOne | `/usr/bin/pos-ninjaone` `6e702ff1…`; `/etc/pocketos/ninjaone.conf` (29 B, nonsecret); no `/run/pocketos/ninjaone` - no login held. Logins live in `/run` only (NINJAONE.md): **every reboot of this gate ends one**, by design |
| Other binaries | `pos-vision` `5ac7992c…`, `pos-camera` `a83641cd…`, `pos-display-boot` `8821097b…`, `pos-browser` `9dcea31e…` |
| Keyboard base | not answering at this boot (`the controller did not answer`) |
| Rollback dirs | `rollback-deskbuddy-opus`, `rollback-rift-basemap`, `rollback-ninjaone` |

Kit (`out/vision-hdmi-gate/stage2/`):

| File on the unit | Source | Identity |
| --- | --- | --- |
| `/usr/bin/doors-shell` | `integration/vision-hdmi-ninjaone` `eabc97f` = `feat/ninjaone-app` 66f9bf0 + `feat/vision-hdmi` (no conflicts), same cross recipe. The recipe rebuilds the installed 66f9bf0 shell byte for byte (md5 `f71aafaf…`) | md5 `158fe80c…`, sha256 `abd68a2e…`, 4,478,216 B, 0 first-party warnings |
| `/boot/k230-canmv-rm69a10-hdmi.dtb` | as below | sha256 `bb346f9e…` |

Focused host checks on `eabc97f`: `make all` and the SDL shell at
`-Werror`, `vision_keys_test`, `kernel_patches_test`, `vision_lint`,
`external_display_shell_test`, `vision_shell_test`, `ninjaone_shell_test.py
--focused`, `launcher_folder_shell_test` - all pass. The backups below take
`f71aafaf…`, not the 2026-10-10 shell.

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

- `doors-shell`, the shell installed at the time (md5 `f71aafaf…` on 2026-10-13);
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

## Results (unit B, 2026-10-13, owner at the bench)

Unit B: v0.3.6 `9ad64a1`, kernel `f73df255…`, shell from
`integration/vision-hdmi-ninjaone` (local, not pushed), monitor on HDMI, no
keyboard base. Commands over SSH (Wi-Fi 172.16.5.198) and COM12; screen
captures from the DRM plane (`shell.action action=screenshot`), bench copies
in `out/vision-hdmi-gate/caps/`, logs in `out/vision-hdmi-gate/logs/`.

| Step | What | Result |
| --- | --- | --- |
| Console | COM12 | Buildroot login, `root` without a password: root shell. VERIFIED |
| R2 | vendor HDMI tree selected with `pos-display-boot hdmi`, reboot, then `lcd`, reboot | PASS: SSH at ~30 s; launcher on the monitor in landscape with "some stripes" (owner), AMOLED dark, no spidev/touch/BOOT key; back on the panel at ~22 s, AMOLED and touch OK (owner) |
| R3 | U-Boot manual panel boot from COM12 | PASS: autoboot waits 1 s, so keys are sent from the banner on; `blinux` read back (`k=Image`, `mmc_boot_dev_num=1`); the command in "Recovery" step 3 booted the panel tree. Nothing saved to the U-Boot environment |
| H2 | md5 check against the record, backups, integration shell `eabc97f` on the panel | PASS: all nine files matched; `ORIG.md5` OK (`f71aafaf…`, `cedfe712…`); panel, touch, NinjaOne OK (owner); Vision live, `shell.key` Up/Enter/Esc opened and closed the picker (3 keys delivered) |
| H3 | Doors HDMI tree installed (sha256 checked on the unit), `pos-display-boot hdmi`, reboot | PASS: model `Canaan CanMV-K230 with LT9611 HDMI`, `HDMI-A-1` 1920x1080, `external:true`, rotation 0, gc2093 probed, `spidev0.0` + radio `rx`, BOOT key, Wi-Fi, no oops; SSH at ~25 s |
| Defect found | the lock screen covered only 1232x568 of the monitor; the launcher showed through outside it | the DRM capture showed the same frame, so a shell layout defect: panel-sized art drawn unscaled at 0,0 and a lock root with no ground. Fixed in `5a9a511` (art centred, the lock keeps the screen's ground when its art is smaller than the display); panel captures unchanged; on the unit the lock covers the monitor (shell `ac24832`, md5 `cc3b9c1a…`) |
| Vision, portrait hold | `automatic`, no keyboard: hold 0 | picture 542x963 (9:16, the largest that fits); boxes on the cars of a car video, OK (owner) |
| Orientation | same unit position (landscape), same video; only the hold changed | hold 0: the detector's input sideways, 0 real cars (one false full-frame box); `shell.rotation mode=landscape` (hold 270, display still 0): upright, 4-6 cars per capture. The input to the detector and the preview are turned by the same rotation, so an upright preview is an upright input (code). Model and thresholds unchanged |
| Preview size | landscape hold | 1024x576 (the slot cap), ~28 % of the monitor |
| Display-side scaling (tried, reverted) | preview drawn scaled to 1464x823, overlays scaled | worked and stayed aligned, but shell CPU 13 % -> 63 % (bilinear) / 55 % (nearest), idle 43 % -> 0-5 %, detector ~11 -> 6-9 fps. Not suitable; owner chose to keep 1024x576. Commits `422a180`, `9e90178` reverted by `55a3263`, `ac82284` |
| Camera app | opened once on the monitor | live, upright, 1024x576; helper closed on leaving |
| Existing functions in HDMI mode | Wi-Fi, LoRa, NinjaOne, launcher | Wi-Fi up, radio `rx`, NinjaOne opens (no login held), 0 ERROR lines, no oops |
| Back to the panel | `shell.rotation mode=automatic`, `pos-display-boot lcd`, reboot | PASS: `settings.conf` byte-identical to before (`1dff6098…`), no `force_dtb`, `DSI-1` 568x1232, touch, BOOT key, `spidev0.0` + radio `rx`, gc2093, shell `ac24832`; AMOLED and touch OK (owner) |

**Unit B is left** on the panel tree, shell `ac24832` (md5 `cc3b9c1a…`,
the NinjaOne build + this branch), every other file as before the gate;
the Doors HDMI tree (`bb346f9e…`) is installed as
`k230-canmv-rm69a10-hdmi.dtb`, the vendor tree as `k230-canmv-v3.dtb`.
`/root/rollback-vision-hdmi/` holds the original NinjaOne shell and the
vendor HDMI tree (verified after the last boot), `RESTORE.sh`,
`settings.conf.before-landscape` and the gate's shells (`eabc97f`,
`ac24832`, `7d76f92`, `abc4e37`).

### Limitations and open items

- Physical keyboard navigation in Vision on HDMI: **UNTESTED** (no keyboard
  base on the bench). The key path was exercised with `shell.key` on the
  panel and in the simulator only. Without touch or a keyboard, an HDMI boot
  is driven over SSH.
- The hold comes from the rotation mode and the keyboard. Without the base,
  `automatic` means portrait: a unit placed in landscape on a monitor needs
  the mode set to Landscape, which also turns the panel on the next panel
  boot.
- The preview stays at most 1024 px a side (about 28 % of a 1920x1080 monitor
  in landscape). A larger picture needs bigger shared-memory slots in the
  shell, `pos-vision` and `pos-camera` (estimated 16.6 MB instead of 8.4 MB
  while open, roughly twice the conversion and copy work); not built.
- Boxes trail fast-moving objects slightly: they come from the frame the
  detector ran on, the preview refreshes at up to 10 Hz.
- "Some stripes" on the monitor were seen once, with the vendor tree;
  not seen in the captures (the DRM frame is clean). Not investigated.
- The focus ring is hard to see on a primary-filled button (panel and
  simulator).
- The unit's `pos-display-boot` still describes the vendor tree when
  selecting HDMI; the branch's version has the corrected text.
- Not tested, by scope: unplugging the cable, a boot without a monitor, 720p,
  automatic switching (none exists), bridge register access.
- Every reboot ends a NinjaOne login (by NinjaOne's design); none was held.
- Nothing is merged or pushed: `feat/vision-hdmi` and
  `integration/vision-hdmi-ninjaone` are local branches.
