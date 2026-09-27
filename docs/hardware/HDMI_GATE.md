# HDMI output - unit A gate

**Status 2026-09-27: steps 0-1 PASS on unit A. Steps 2-13 NOT RUN: HARDWARE GATE
BLOCKED, PHYSICAL HDMI CONNECTION REQUIRED** (see "Before step 2" and
"Result").

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
2026-09-27 the first two did not, and the switch was not made.

1. **A monitor is connected and powered on.** The owner confirms it. As a
   cross-check from Linux in the LCD tree, read the bridge's HPD register the way
   the vendor driver does (SDK patch 0025: 0x825e, connected when bit 0 or bit
   2 is set):
   `i2cset -y 1 0x3b 0xff 0x80; i2cset -y 1 0x3b 0xee 0x01; i2cset -y 1 0x3b 0xff 0x82; i2cget -y 1 0x3b 0x5e; i2cset -y 1 0x3b 0xff 0x80`.
   On 2026-09-27 it read **0x78** (bits 0 and 2 clear) three times: no sink
   seen. Caveat: in the LCD tree no driver has powered up the bridge's HPD
   logic, so a low reading is not proof of absence. A high reading, together
   with the owner's word, is enough.
2. **A remote path that survives the HDMI tree.** Wi-Fi is on `sdhci0`, which
   the HDMI tree reconfigures (`HDMI_OUTPUT.md` §5). On this bench `eth0` does
   not pass traffic on its own. Before an unattended HDMI boot, either prove
   `eth0` alone (take `wlan0` down, ping and SSH to .157 over `eth0` only) or
   have the owner at the bench.
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

Run 2026-09-27 on unit A, by the local integration session (no owner at the
bench). Evidence: `out/hdmi-gate/` on the bench PC (`g*.out`, the decompiled
DTBs, `caps/amoled-pattern.png`).

| Step | Result | Notes |
| --- | --- | --- |
| 0 install | PASS | Built with the pinned Xuantie 14.1.1 via `make all` (links libc only), copied by scp. `pos-drmtest --help` exits 2, and so does `pos-display-boot` with no argument. Rollback script written first |
| 1 baseline, tool on the AMOLED | PASS, after two tool fixes | Model `Canaan CanMV-K230 with RM69A10 OLED`; `next boot: lcd`; `i2cdetect -r 1`: **0x3b answers**, 0x5d `UU`; the LT9611 chip id reads `17 02 e2`. `gpioinfo`: GPIO23 `ts_irq_gpio`, GPIO24 `ts_reset_gpio`. `list` with the shell running: cached, `DSI-1 connected, 65x145 mm`, `568x1232@52 49500 kHz -> DSI 49500 kHz +0.00 %`, `EDID: none`. With the shell stopped: DRM master, probed, same. `pattern` first refused the panel (640x480 floor, fixed in aa5aa53), then failed `SETCRTC … No space left on device` (the shell's rotate-270 left on the primary plane, fixed in a61ec37). After the fixes: `safest 568x1232@52 … exact`, `rotation 0x8 … rotate-0`, `mode set … in 10 ms`, `rotation 0x8 restored`, `CRTC switched off`. The scanout read back with kmsgrab shows the border, the R/G/B blocks and the three lines. **The glass itself was not looked at** (no one at the bench). The shell came back each time (3 restarts, build 608f972, no crash reports). `pos-display-boot status`/`lcd`/usage correct; `hdmi` and `lcd` exercised against a scratch copy of `/boot`, and a damaged HDMI tree refused. Real `/boot` md5-identical before and after |
| 2 switch, boot | NOT RUN | Blocked: "Before step 2" items 1 and 2 |
| 3 kernel log | NOT RUN | |
| 4 connectors | NOT RUN | |
| 5 EDID | NOT RUN | |
| 6 safest mode | NOT RUN | |
| 7 640x480 | NOT RUN | |
| 8 720p / 1080p | NOT RUN | |
| 9 unplugged boot | NOT RUN | |
| 10 hotplug | NOT RUN | |
| 11 monitor off / no EDID | NOT RUN | |
| 12 Doors on HDMI | NOT RUN | |
| 13 back to the panel | n/a | The unit never left the panel tree |

**Still required, physically:** connect a powered HDMI monitor to HDMI1 on unit
A, and either be at the bench or prove an `eth0`-only SSH path. Then start at
step 2. `pos-display-boot lcd && reboot` over SSH or the COM9 console is the way
back.
