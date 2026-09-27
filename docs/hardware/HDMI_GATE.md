# HDMI output - unit A gate

**Status 2026-09-27, round 1: HDMI boot and bridge PROVEN; HDMI PICTURE
FAILED.** Steps 0-5 and 13 PASS. Step 6 (720x480) and step 8 (720p and 1080p)
FAIL: 720x480 and 720p give no signal, and 1080p60 gives black and white
flickering stripes. Step 12 FAIL: the shell takes the monitor's 2560x1440,
which the bridge cannot take. The cause is in the vendor kernel (see
"Result"). **Round 2, the same day, on the patched kernel of
`HDMI_KERNEL_FIX.md` (ADR-011, Proposed): every kernel-visible check PASSES
at 720p and 1080p and 2560x1440 is no longer offered. Round 3, the owner at
the bench: the DSI's own colour bars reach the monitor at 720p and 1080p, so
the patches are proven on the glass; the K230 VO's output was still wrong.
Round 4, with VO patch 0072: 1920x1080@60 is clean and stable from
`pos-drmtest` and from the Doors shell; 1280x720@60 still gives no signal
(VO side, cause open, "Round 4").** Unit A is back on the panel with the
v0.1.0 kernel.

**Unit A carries:** image v0.1.0, build 1368695; shell 608f972 (the Recorder
gate's build, the same device code as master 6077b8d); `pos-drmtest` built from
`feat/k230-hdmi-out` 2451279 (sha256 `b1c18867…`, byte-identical to the build of this commit's tree); `pos-display-boot` sha256
`b9d562f5…`. It is in the **LCD tree**, with no `/boot/force_dtb`, rotation mode
Automatic (landscape on the bench). Rollback: `/root/rollback-hdmi/RESTORE.sh`.
Before this gate the unit had neither tool and no `force_dtb`.

Each step names a pass criterion. Record the result beside it, as the other
gates in this directory do.

What is under test: the vendor HDMI device tree that every Doors image already
carries (`/boot/k230-canmv-rm69a10-hdmi.dtb`, CanMV-K230 v3 tree), selected
with `pos-display-boot`, and driven by `pos-drmtest`. No kernel change.

You need: unit A with its serial console (COM9, 115200n8) **and** Ethernet
(the recovery paths below use them), a known-good HDMI monitor or TV that
publishes an EDID (1080p class), an HDMI cable fitting HDMI1, and ideally an
EDID-less sink (an HDMI-to-VGA adapter often is) for step 11.

## Recovery (read first)

The choice is persistent until changed. If an HDMI boot is unusable:

1. SSH (Ethernet keeps working in the HDMI tree) or the serial console:
   `pos-display-boot lcd && reboot`
2. Serial console only, Linux not reachable: at `Hit any key to stop autoboot`
   press a key, then boot the panel tree once (the same command as `blinux`
   in the SDK's `default.env`, with the device tree named directly):

   ```
   ext4load mmc ${mmc_boot_dev_num}:1 0x3000000 /fw_jump_add_uboot_head.bin && ext4load mmc ${mmc_boot_dev_num}:1 0x200000 /Image && ext4load mmc ${mmc_boot_dev_num}:1 0x2200000 /k230-canmv-rm69a10.dtb && bootm 0x3000000 - 0x2200000
   ```

   then `pos-display-boot lcd` from Linux. (Derived from the vendor
   environment, not yet tried on unit A.)
3. The card in a Linux PC: delete `force_dtb` from partition 1 (ext4).

## 0. Install

Either a full image of this branch (`BUILD_ENVIRONMENT.md`, "Build Doors";
both tools are in the `pocketos` package), or only the two tools onto the
running image, which already has the HDMI tree:

```sh
# in WSL, from a clean checkout of feat/k230-hdmi-out
TC=/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc
mkdir -p out/hdmi
$TC -O2 -mcpu=c908v -mtune=c908 -std=gnu11 -Wall -Wextra -Werror -Icore \
    tools/drmtest/pos_drmtest.c tools/drmtest/drmtest_logic.c -o out/hdmi/pos-drmtest
scp out/hdmi/pos-drmtest root@<unit>:/usr/bin/pos-drmtest
scp tools/display/pos-display-boot.sh root@<unit>:/usr/bin/pos-display-boot
ssh root@<unit> 'chmod 0755 /usr/bin/pos-drmtest /usr/bin/pos-display-boot; sha256sum /usr/bin/pos-drmtest /usr/bin/pos-display-boot'
```

Pass: both run (`pos-drmtest --help` prints the usage and exits 2).

## 1. Baseline on the panel (LCD tree)

```sh
cat /proc/device-tree/model; echo
pos-display-boot status
i2cdetect -y -r 1                 # 0x3b must answer; 0x5d shows UU (touch)
pos-drmtest list                  # shell running: cached state
grep -E 'MemTotal|MemAvailable|CmaTotal|CmaFree' /proc/meminfo
top -b -n 3 -d 2 | grep -E 'doors-shell|Mem:|CPU:'
```

Pass: model `Canaan CanMV-K230 with RM69A10 OLED`; `next boot: lcd`; 0x3b
present (**if 0x3b is absent, stop: no bridge, an HDMI boot would have no DRM
device**); `DSI-1 connected`, one mode 568x1232, `49500 kHz -> DSI 49500 kHz
+0.00 %`.

Prove the tool on the known-good path before trusting it on the new one:

```sh
/etc/init.d/S90doors-shell stop
pos-drmtest pattern --seconds 10  # on the AMOLED
/etc/init.d/S90doors-shell start
```

Pass: the AMOLED shows black, a white border, red/green/blue blocks and
`DOORS K230 HDMI TEST` / `568x1232@… DSI-1`; the shell comes back.

## Before step 2 (added 2026-09-27)

Do not make the HDMI boot unattended unless all of the following hold. On
2026-09-27 the first two did not hold while the unit was unattended, so the
switch waited until the owner was at the bench with a monitor connected.

1. **A monitor is connected and powered on.** The owner confirms it. As a
   cross-check from Linux in the LCD tree, read the bridge's HPD register the way
   the vendor driver does (SDK patch 0025: 0x825e, connected when bit 0 or bit
   2 is set):
   `i2cset -y 1 0x3b 0xff 0x80; i2cset -y 1 0x3b 0xee 0x01; i2cset -y 1 0x3b 0xff 0x82; i2cget -y 1 0x3b 0x5e; i2cset -y 1 0x3b 0xff 0x80`.
   On 2026-09-27 it read **0x78** (bits 0 and 2 clear) with no monitor, and
   **0x7d** (bits 0 and 2 set) once the owner had connected one. The kernel
   driver later read the same 0x7d. VERIFIED: this read tells a sink from no
   sink even in the LCD tree.
2. **A remote path that survives the HDMI tree.** Wi-Fi is on `sdhci0`, which
   the HDMI tree reconfigures (`HDMI_OUTPUT.md` §5). **It still works**: the HDMI
   boot on 2026-09-27 joined Wi-Fi and SSH on .171 answered at 29 s of uptime.
   VERIFIED. `eth0` still passes no traffic on its own on this bench.
3. **The serial console answers**: `ser_cmd.ps1 -Port COM9` (out/rc1-0.0.12-gate)
   returned unit A's wlan0 MAC and a root prompt on 2026-09-27. VERIFIED. It
   recovers any HDMI boot that reaches a login prompt. A kernel that hangs
   before the prompt needs U-Boot's autoboot stopped on the console (Recovery
   2, never tried) or a power cycle, which is physical.
4. No other session is using unit A. Checked on 2026-09-27: none was.

## 2. Switch to HDMI, monitor attached

Connect the monitor (powered on) before the reboot.

```sh
pos-display-boot hdmi && reboot
```

Watch the console. Pass: U-Boot prints `ext4load mmc 1:1 … force_dtb`, then
loads `/k230-canmv-rm69a10-hdmi.dtb`; Linux boots to a login prompt. Record
what the AMOLED shows (U-Boot logo frozen, garbage, dark).

## 3. Kernel log

```sh
cat /proc/device-tree/model; echo
dmesg | grep -iE 'lt9611|hdmi|canaan-drm|mipi-dsi|dsi|drm' | head -60
ls /sys/class/drm/
cat /sys/class/drm/card0-HDMI-A-1/status /sys/class/drm/card0-HDMI-A-1/modes
```

Pass: model `Canaan CanMV-K230`; `LT9611 revision: 0x…`, `Attached device
lt9611`, `Initialized canaan-drm`; `card0-HDMI-A-1` present, `connected`, a
mode list. Record the revision and the first lines of `modes`.

## 4. Connectors and modes

```sh
/etc/init.d/S90doors-shell stop   # if it started on HDMI
pos-drmtest list | tee /tmp/hdmi-list.txt
```

Pass: `this process is DRM master, connectors were probed`; `HDMI-A-1:
connected`, mm size of the monitor, modes with their DSI clock; `EDID: …
header ok, checksum ok`. Keep `/tmp/hdmi-list.txt`.

## 5. EDID

```sh
pos-drmtest edid HDMI-A-1 | tee /tmp/hdmi-edid.txt
od -A x -t x1 /sys/class/drm/card0-HDMI-A-1/edid | head -4
```

Pass: vendor/name match the monitor; the hex matches the sysfs copy.

## 6. Safest mode

```sh
pos-drmtest pattern --seconds 20
```

Pass: the line `safest WxH@R … exact` (typically 720x480@60 or 720x576@50 at
27 MHz), `mode set … in N ms`, and on the monitor: black, white border all
round (no overscan cut), red/green/blue blocks in that order, the three text
lines. `previous CRTC state restored` or `CRTC switched off` at the end.
Record N and anything wrong with colours or geometry.

## 7. 640x480

```sh
pos-drmtest pattern --mode 640x480 --seconds 20
```

Expected `INEXACT` (24.75 MHz, −1.7 %). Record whether the monitor shows it.
Also `--mode 800x600` if offered.

## 8. 720p (and 1080p only after 720p)

```sh
pos-drmtest pattern --mode 1280x720@60 --seconds 20
pos-drmtest pattern --mode 1920x1080@60 --seconds 20
```

Pass: both exact (74.25 / 148.5 MHz) and shown. Record the monitor's own
mode readout.

## 9. Boot with HDMI disconnected

Unplug the cable, `reboot`.

```sh
dmesg | grep -iE 'lt9611|hdmi' | head
cat /sys/class/drm/card0-HDMI-A-1/status
pos-drmtest list; echo "rc=$?"
cat /run/pocketos/doors-shell.state
tail -n 20 /var/lib/pocketos/log/shell.log
```

Pass: boot completes, no oops; `disconnected`; `pos-drmtest list` exit 2. The
shell is expected to fail (`display init failed`) and the supervisor to stop
after its crash-loop limit: `crashloop=1`, `running=0`, then quiet - confirm
with `top -b -n 2 -d 5` that nothing restarts or spins.

## 10. Hotplug, unplug, reconnect

```sh
# plug the cable in, then:
dmesg | tail -5                                   # 'hdmi cable connected'
cat /sys/class/drm/card0-HDMI-A-1/status          # may still say disconnected: no uevent
pos-drmtest list | grep HDMI-A-1                  # master probe: connected
pos-drmtest pattern --seconds 60 &
# after ~10 s unplug; after ~10 s plug back in
wait
dmesg | tail -10
```

Pass: the probe finds the monitor without a reboot; unplug and replug during
the pattern produce the two log lines, no oops, and the picture comes back on
replug (record whether it does). Then with the shell stopped by the crash
loop: `/etc/init.d/S90doors-shell restart` and record what the shell shows.

## 11. Monitor off, no EDID

- Monitor powered off with the cable in: `pos-drmtest list` (record connected
  or not: many sinks keep HPD up), then on again and `pattern`.
- EDID-less sink, if available: `pos-drmtest edid` exit 2 with `EDID: none`,
  `list` shows the kernel's fallback modes, `pattern` picks 640x480 and says
  INEXACT. Without such a sink: NOT TESTED.

## 12. Doors on HDMI

```sh
/etc/init.d/S90doors-shell restart
sleep 10
grep -E 'display|DRM' /var/lib/pocketos/log/shell.log | tail -5
cat /run/pocketos/doors-shell.state
top -b -n 5 -d 2 | grep -E 'doors-shell|CPU:'
grep -E 'MemAvailable|CmaFree' /proc/meminfo
pos radio status; echo "rc=$?"
```

Pass (for this milestone): the shell comes up on the monitor at the monitor's
first mode (log line `display came up WxH, expected 568x1232`), no restarts
over 5 minutes. Expected and recorded, not failures: no touch, radio
unavailable (no spidev), layouts not designed for the size. Record CPU at
idle and while opening an app, and MemAvailable/CmaFree against step 1.

## 13. Back to the panel

```sh
pos-display-boot lcd && reboot
# after boot:
cat /proc/device-tree/model; echo
pos-display-boot status
pos-drmtest list | head -8
pos radio status
```

Pass: model with `RM69A10 OLED`, AMOLED and touch work, radio as before,
`DSI-1` 568x1232. Unit left in LCD mode.

## Result

Run 2026-09-27 on unit A by the local integration session. Steps 0-1 ran with
no one at the bench. Steps 2-13 ran with the owner at the bench and a **DUS
D27QP** (27", 2560x1440, EDID 1.3 plus one CEA extension, range 48-70 Hz,
31-122 kHz, up to 250 MHz) on HDMI1. The owner confirmed monitor and cable
are known-good with another source on the same input. Evidence:
`out/hdmi-gate/` on the bench PC (`g*.out`, `serial-boot*.log`, the decompiled
DTBs, `caps/amoled-pattern.png`, copies of the vendor `lontium-lt9611.c` and
`canaan_dsi.c` as built).

| Step | Result | Notes |
| --- | --- | --- |
| 0 install | PASS | Built with the pinned Xuantie 14.1.1 via `make all` (links libc only), copied by scp. `pos-drmtest --help` exits 2, and so does `pos-display-boot` with no argument. Rollback script written first |
| 1 baseline, tool on the AMOLED | PASS, after two tool fixes | Model `Canaan CanMV-K230 with RM69A10 OLED`; `next boot: lcd`; `i2cdetect -r 1`: **0x3b answers**, 0x5d `UU`; the LT9611 chip id reads `17 02 e2`. `gpioinfo`: GPIO23 `ts_irq_gpio`, GPIO24 `ts_reset_gpio`. `list` with the shell running: cached, `DSI-1 connected, 65x145 mm`, `568x1232@52 49500 kHz -> DSI 49500 kHz +0.00 %`, `EDID: none`. With the shell stopped: DRM master, probed, same. `pattern` first refused the panel (640x480 floor, fixed in aa5aa53), then failed `SETCRTC … No space left on device` (the shell's rotate-270 left on the primary plane, fixed in a61ec37). After the fixes: `safest 568x1232@52 … exact`, `rotation 0x8 … rotate-0`, `mode set … in 10 ms`, `rotation 0x8 restored`, `CRTC switched off`. The scanout read back with kmsgrab shows the border, the R/G/B blocks and the three lines. **The glass itself was not looked at** (no one at the bench). The shell came back each time (3 restarts, build 608f972, no crash reports). `pos-display-boot status`/`lcd`/usage correct; `hdmi` and `lcd` exercised against a scratch copy of `/boot`, and a damaged HDMI tree refused. Real `/boot` md5-identical before and after |
| 2 switch, boot | PASS | `pos-display-boot hdmi` rc 0, `force_dtb` = `k230-canmv-rm69a10-hdmi.dtb`. Serial: U-Boot `ext4load … force_dtb`, and Linux reached login. Wi-Fi joined, and SSH answered at 29 s of uptime. The AMOLED stayed **dark** (owner) |
| 3 kernel log | PASS | Model `Canaan CanMV-K230`; `LT9611 revision: 0x2`, `Attached device lt9611`, `Initialized canaan-drm`, `fb0: canaan-drmdrmfb`; `lt9611_connect_detect 1 reg_val=0x7d`; `card0-HDMI-A-1` connected, 26 modes, first 2560x1440 |
| 4 connectors | PASS | DRM master, probed: `HDMI-A-1: connected, 600x330 mm, 26 mode(s)`. 2560x1440@60 wants 241.5 MHz, the DSI makes 297 MHz (+22.98 %). 1080p60/50, 720p60/50, 720x576@50, 720x480@60 and 800x600@75 are exact. 1600x900, 1280x1024@60 and 1280x960 are −8.33 % |
| 5 EDID | PASS | `DUS 0x2700 "D27QP"`, week 51 of 2020, 256 bytes, header and checksum ok; the hex matches `/sys/class/drm/card0-HDMI-A-1/edid` |
| 6 safest mode | **FAIL** | `safest 720x480@60 … 27000 kHz; exact`, `mode set … in 826 ms`, and the bridge's own check reads `hactive_a=720, vactive=480, v_total=525`: the input is right. Monitor: **no signal, goes to sleep** |
| 7 640x480 | NOT RUN | Inexact, and nothing below 1080p showed a picture |
| 8 720p / 1080p | **FAIL** | 1280x720@60: exact 74.25 MHz, set in 838 ms, bridge input `1280/720/750`; monitor **no signal**. 1920x1080@60: exact 148.5 MHz (891 Mbit/s per lane), set in 841 ms, bridge input `1920/1080/1125`; monitor locks but shows **black and white stripes, flickering** |
| 9 unplugged boot | NOT RUN | Not useful before a picture exists |
| 10 hotplug | NOT RUN | Same |
| 11 monitor off / no EDID | NOT RUN | Same |
| 12 Doors on HDMI | **FAIL** | The shell starts (`display came up 1440x2560`, rotation 270), running, 0 restarts, 27.6 MB RSS (15 MB on the panel), 1-2 % CPU at idle. It takes the preferred 2560x1440, which the DSI runs at 297 MHz (1.78 Gbit/s per lane); the bridge check reads garbage and the monitor shows no signal. radiod crash-loops as expected (no spidev: `crashloop=1`, 6 restarts). MemAvailable 853 MB, CmaFree 409 MB |
| 13 back to the panel | PASS | `pos-display-boot lcd` rc 0, reboot; model `… with RM69A10 OLED`, `next boot: lcd`, `/boot` md5-identical, `DSI-1` 568x1232, touch bound, `/dev/spidev0.0`, radio `rx`, shell 608f972 with 0 restarts; the owner confirmed the AMOLED and touch work. Wi-Fi came back on the owner's phone hotspot (the other saved network, in range), not the bench network; it returned to TP-TanK-BE3600 once the hotspot was switched off. Nothing to do with HDMI |

### Why no picture (read from the kernel that was built)

- **The LT9611 driver is set up for 1080p only.** In `lontium-lt9611.c` (SDK
  patch 0001) `lt9611_modes[]` holds only 1080p60 and 1080p30; 720p, 480p and
  640x480 are commented out. Yet `lt9611_find_mode()` returns the 1080p60
  entry for **any** mode up to 300 MHz, so `mode_valid` offers the whole EDID
  list. The PCR registers (`lt9611_pcr_setup`: 0x8321/0x8324/0x8325/0x834a…)
  are fixed constants. Only the TX PLL post-divider and `pcr_m` follow the
  clock. That fits what the monitor showed: nothing at 27 and 74.25 MHz, a
  locked but broken picture at 148.5 MHz. DOCUMENTED (source), cause ASSUMED.
- **1080p is received but not shown cleanly.** The bridge's video check
  counts the right active and total sizes, so the DSI timing reaches it. The
  stripes point at the pixel data or at the TMDS side. Candidates:
  - the DSI D-PHY gets the same `hsfreq=0x96` at every lane rate
    (`canaan_dsi_clk_cfg`, BSP patches 0041/0042);
  - the link runs 891 Mbit/s per lane on CLK/D0/D1 nets that also run to the
    AMOLED connector (a stub);
  - the fixed PCR set-up.
  None of these is proven. ASSUMED.
- **The shell cannot choose a mode the bridge can take.** LVGL takes the
  connector's first mode, and the kernel offers 2560x1440 because the driver
  accepts anything up to 300 MHz. Even with a working 1080p, the shell would
  need a mode choice (or a driver `mode_valid` limited to the modes it can
  set) before Doors on HDMI can work.

All three are in the kernel (vendor driver, BSP DSI patches) or need a Doors
display-stack change. Neither is in this branch's scope (no kernel change
without an ADR-001 proposal, no UI change). `HDMI_OUTPUT.md` §9 lists the next
steps.

*Revised the same day by `HDMI_KERNEL_FIX.md`:* the register recipe is not
1080p-only (it is the RT-Smart recipe, mode-derived); the defects are the
mode table's use, one timing register, and the 4-lane PHY set-up.

## Round 2: the patched kernel (2026-09-27, unattended)

**Unit A carried:** the v0.1.0 image and tools as above, with `/boot/Image`
replaced by the round-2 kernel (`7ab9b4bf…`: the same kernel tree plus Doors
patches 0070 and 0071, `HDMI_KERNEL_FIX.md` §7) and the v0.1.0 kernel kept as
`/boot/Image.orig`. Rollback scripts `/root/rollback-hdmi/RESTORE.sh` (whole
gate), `KERNEL_ORIG.sh`, `KERNEL_R2.sh`. Nobody was at the bench; the
monitor (DUS D27QP) stayed connected and powered from round 1 (HPD `0x7d`).
Harness: `out/hdmi-gate/r2_*.sh`, outputs `r2_*.out`, serial captures
`r2-serial-boot-{lcd,hdmi,lcd-final}.log`.

| Step | Pass criterion | Result |
| --- | --- | --- |
| R2.1 LCD tree first | The panel path is unaffected by the kernel change | PASS. `uname` shows the new build stamp; DSI log line value-for-value identical to round 1's (`lanes=2 … auto_m=97 auto_n=3 auto_voc=0x17 hsfreq=0x96`); `DSI-1` 568x1232 connected and enabled; shell running, 0 restarts; touch, spidev, radio `rx`; no oops |
| R2.2 HDMI boot | 2560x1440 gone; first mode 1080p60; bridge counts a full frame | PASS. Modes: 1920x1080 x3, 1280x720 x4, nothing else. fbdev's modeset: DSI `lanes=4 div=4 auto_m=295 auto_n=15 auto_voc=0x9 hsfreq=0xa9`; bridge `1920/1080/1125`, `h_total_sysclk=400`. `fb0` registered. Shell up on 1080x1920 (rotation 270), 0 restarts. SSH at 30 s |
| R2.3 720p60 pattern | DSI at 445.5 Mbit/s with RT-Smart's PHY values; bridge sees sync+back porch 260 and a full 720p frame | PASS (kernel-visible). `auto_voc=0x19 hsfreq=0x96`, `hsync_porch: 260`, video check `1280/720/750`, `h_total_sysclk=600`; set in 824 ms; held 40 s at 0 % CPU; `rc=0`; restore to 1080p gave `1920/1080/1125/400` again |
| R2.4 1080p60 pattern | as R2.2 | PASS (kernel-visible). Already the current mode (2 ms, flip only); held 40 s; `rc=0`; no oops in any run |
| R2.5 Picture on the monitor | The owner sees the pattern | **NOT OBSERVED** (unattended). Round 1's 720p failure was at the HDMI transmitter, which these counters do not see |
| R2.6 Back to the panel, original kernel | Unit as before the gate | PASS. `pos-display-boot lcd`, `KERNEL_ORIG.sh`, reboot: `6.6.36 #2 … Sep 4`, panel, shell, touch, spidev, radio `rx`, no `force_dtb`. The patched kernel stays at `/root/rollback-hdmi/Image.hdmi-r2` |

## Round 3: the owner looks (2026-09-27, patched kernel, owner at the bench)

Same unit state as round 2 (`KERNEL_R2.sh`, `pos-display-boot hdmi`, reboot;
HPD `0x7d` before the switch). Each row pairs what the owner saw with what the
kernel and the bridge reported at the same moment. Harness `r3_*.sh`, outputs
`r3_*.out`, `r3_regs_*.txt` in `out/hdmi-gate/`.

| Step | Owner saw | Machine-visible | Verdict |
| --- | --- | --- | --- |
| R3.0 Boot (fbdev 1080p60 XRGB8888, then the shell at 1080x1920 rotation 270) | nothing on the monitor | as R2.2: DSI `auto_voc=0x9 hsfreq=0xa9`, bridge `1920/1080/1125/400`, shell running | see R3.5/R3.7 |
| R3.1 `pos-drmtest pattern --mode 1280x720@60` (XRGB8888) | **no signal** | DSI `auto_voc=0x19 hsfreq=0x96`, bridge `hsync_porch: 260`, `1280/720/750`, `h_total_sysclk=600`, rc 0 | FAIL, as round 1 |
| R3.2 `pos-drmtest pattern --mode 1920x1080@60` (XRGB8888) | **locked, black** (monitor synced, picture black; round 1 had stripes here) | modeset 13 ms (mode already current), rc 0, no oops | FAIL, changed symptom |
| R3.3 DSI host video pattern generator at 1080p (`VID_MODE_CFG` bit 16 set by `devmem` while R3.2-style hold ran; bypasses VO and framebuffer) | **vertical colour bars** | DSI as R3.2 | **PASS: DSI PHY → LT9611 → HDMI proven at 891 Mbit/s** |
| R3.4 Same generator at 720p | **vertical colour bars** | DSI as R3.1, bridge `1280/720/750/600` | **PASS: proven at 445.5 Mbit/s** |
| R3.5 `modetest -s 54@52:1920x1080-60@RG16` (SMPTE bars, RGB565, the shell's format), first run right after the pattern's restore | locked, black | OSD4: INFO 0x02, DMA_CTRL 0x4F, stride 0x1E0, ADDR_SEL 0x1100, IRQ status 0x10000000 | FAIL |
| R3.6 Same, second and third run (each after killing the previous modetest) | **SMPTE colour bars** | ADDR_SEL read 0x100 during the first bars; 0x1100 and status bit 28 in the third run, so those readbacks do not discriminate | **PASS: VO → DSI → bridge → monitor at 1080p RGB565** |
| R3.6a raw `devmem` write of ADDR_SEL 0x100 while bars showed | bars went **black** | a live raw write to a plane register stalls the layer; not a driver path, recorded only as a caution | n/a |
| R3.7 `modetest … 1280x720-60@RG16`, twice (second from the already-set 720p state) | **no signal** both times | DSI/bridge as R3.1; ADDR_SEL 0x100 (consumed) on the re-set | FAIL |
| R3.8 Back to the panel, original kernel | panel and touch as before | `6.6.36 #2 … Sep 4`, `536d4770…`, `DSI-1` 568x1232, shell running, spidev, radio `rx`, no `force_dtb` | PASS |

What round 3 settles:

- **Patches 0070 and 0071 do what they claim.** With the DSI host's own
  generator the monitor shows bars at both 720p and 1080p (R3.3, R3.4): PHY
  values, lane configuration, the bridge's PLL/PCR/timing and its HDMI
  transmitter are all right, and 2560x1440 is gone. The round-1 failures on
  the bridge side are fixed.
- **The remaining defect is in front of the DSI, in the K230 VO output**, and
  it has two parts:
  1. XRGB8888 planes never fetch. `pos-drmtest` and fbdev use XRGB8888; the
     BSP's XRGB8888 entry writes OSD DMA_CTRL 0x40, and bits [3:0] of that
     register are the DMA request enable (the vendor U-Boot logo code writes
     `0xf` there in `kd_vo_osd_set_dma_request()`, DOCUMENTED). RGB565 with
     0x4F shows bars (R3.6). This also explains "nothing at boot".
  2. The VO's 1280x720 output is refused by the bridge's transmitter (no
     signal) although the bridge counts it correctly, and although the same
     DSI configuration carries the generator's 720p (R3.4 vs R3.7). And a
     1080p RGB565 modeset shows bars only on a re-set, not on the first set
     after a different plane configuration (R3.5 vs R3.6). Both point at the
     VO/plane programming sequence for a non-panel mode; neither is
     understood yet (ASSUMED: the BSP's deferred config load, patch 0043, or
     the display reset at CRTC enable).
- The shell on HDMI (R3.0) runs RGB565 through the BSP's GDMA rotation path
  at 1080x1920, which no gate has verified; its black is not attributed.

Next, smallest first: (a) VO patch: XRGB8888 (and ARGB8888, which the BSP
also set to request-off) get the DMA request bits back, one constant each,
DOCUMENTED by the U-Boot code; (b) the 720p and first-set failures need the
VO register state captured per step (the `voregs_unit.sh` harness exists)
and a comparison against a pristine-driver (K0) VO sequence, before any code
is changed. Neither is in patches 0070/0071. (a) became patch 0072, round 4.

## Round 4: patch 0072 (2026-09-27, owner at the bench)

**Unit A carried:** the v0.1.0 image and tools, `/boot/Image` = round-4
kernel `86072a52…` (0070 + 0071 + 0072 on the v0.1.0 kernel tree), v0.1.0
kernel kept as `/boot/Image.orig`, `KERNEL_R4.sh` / `KERNEL_ORIG.sh` /
`RESTORE.sh` in `/root/rollback-hdmi/`. Monitor connected (HPD `0x7d`).
Harness `r4_*.sh`, outputs in `out/hdmi-gate/`.

| Step | Owner saw | Machine-visible | Verdict |
| --- | --- | --- | --- |
| R4.1 LCD tree first | (panel, as always) | new build stamp; DSI line identical to v0.1.0's; 568x1232; shell, touch, spidev, radio `rx`; no oops | PASS |
| R4.2 HDMI boot | not asked | modes 1920x1080 / 1280x720 only; fbdev 1080p60; bridge `1920/1080/1125/400`; shell running | PASS |
| R4.3 `pos-drmtest pattern --mode 1280x720@60`, 30 s and again 120 s | **no signal** (the 30 s run was read as "black, locked" after the hold had ended and the 1080p console was back; the 120 s run, looked at during the hold: no signal) | OSD4 `INFO 0x03 DMA_CTRL 0x4F`; DSI `auto_voc=0x19 hsfreq=0x96`; bridge `1280/720/750/600`; rc 0 | **FAIL (VO 720p, unchanged)** |
| R4.4 `pos-drmtest pattern --mode 1920x1080@60`, 30 s | "it was nice, like 1 [clean pattern], but now it is black" - clean while it showed; the hold had ended when looked at again | modeset 3 ms, rc 0 | PASS (clean) |
| R4.5 Same, 150 s hold, watched for 20 s | **clean and stable**: black frame, white border, red/green/blue blocks in that order, text lines | OSD4 `0x03 / 0x4F`, ADDR_SEL 0x1100, IRQ status bit 28 (again: not discriminating) | **PASS: first picture from a framebuffer through the whole path; block order right, so bit 6 of DMA_CTRL is the right byte order for XRGB8888** |
| R4.6 Doors shell on HDMI (`S90doors-shell start`; RGB565, rotation 270, 1080x1920 through the GDMA rotation path) | **Doors UI visible**, stable | `display came up 1080x1920 … rotation 270`, running, 0 restarts; OSD4 `INFO 0x02 DMA_CTRL 0x4F STRIDE 0x1E0` | **PASS** |
| R4.7 Back to the panel, original kernel | panel as before | `6.6.36 #2 … Sep 4`, `536d4770…`, 568x1232, shell, spidev, radio `rx`, no `force_dtb` | PASS |

What round 4 settles: with 0070 + 0071 + 0072 the T-Display K230 shows
**1920x1080@60 on HDMI**, from a DRM framebuffer (`pos-drmtest`, XRGB8888)
and from the Doors shell (RGB565, rotated), clean and stable, the whole path
VO → DSI → LT9611 → monitor. **1280x720@60 still gives no signal** from the
VO's stream (the DSI's own 720p bars reach the monitor, round 3), and that
is the one open display defect. The round-3 "black on the first 1080p set"
was not seen in round 4 (three 1080p sets, all showed; the earlier black
sets were XRGB8888 with the request bits off, or `modetest` runs whose
sequence is not reproduced here) - watch for it, not proven gone.
