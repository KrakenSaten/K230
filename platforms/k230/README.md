# platforms/k230

Board support for the LILYGO T-Display K230, layered on the pinned LILYGO BSP
and Kendryte K230 Linux SDK (ADR-001).

```text
configs/k230_pocketos_defconfig   Vendor board defconfig + BR2_PACKAGE_POCKETOS
package/pocketos/                 Buildroot package building the repository root Makefile
scripts/apply_to_sdk.sh           BSP overlay + vendor launcher + PocketOS package into the SDK
scripts/build_image.sh            Build and export sysimage-sdcard.img to out/k230/
scripts/deploy.sh                 Push built binaries to a running board over SSH
vendor_bsp_commit.txt             Pinned Xinyuan-LilyGO/T-Display-K230 commit
vendor_sdk_commit.txt             Pinned kendryte/k230_linux_sdk commit
```

The vendor LVGL launcher is still installed by `apply_to_sdk.sh` and owns
the display and the radio by default. `apply_to_sdk.sh` adds one switch to
its init script (`ENABLE` in `/etc/default/k230_phone_ui`, default 1); the
PocketOS shell is installed as `/usr/bin/pocketos-shell` with
`S90pocketos-shell` disabled, and radiod runs with the mock backend.

## Panel ownership (persistent across reboots)

Hand the panel and the radio to PocketOS:

```sh
echo ENABLE=0 > /etc/default/k230_phone_ui      # vendor launcher stays down
echo ENABLE=1 > /etc/default/pocketos-shell     # PocketOS shell takes the panel
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
reboot
```

After the reboot: `pos app list`, `pos radio info`. Without a reboot, the
same state is reached with `/etc/init.d/S99zz_k230_phone_ui stop`, then
`/etc/init.d/S60radiod restart` and `/etc/init.d/S90pocketos-shell start`.
S90 refuses to start while the launcher is enabled or running, so the two
never fight for DRM master.

Back to the vendor launcher (recovery and reference path; nothing is removed
from the image):

```sh
echo ENABLE=1 > /etc/default/k230_phone_ui
echo ENABLE=0 > /etc/default/pocketos-shell
rm -f /etc/default/radiod
reboot
```

If the device does not boot far enough for SSH, use the serial console
(115200) or swap back to the vendor SD card; see docs/hardware/FIRST_BOOT.md
"Recovery".

## Logs on the device

`/var/log` is a tmpfs on this image, so PocketOS keeps its logs, crash
reports and supervisor logs in `/var/lib/pocketos/log` (persistent):
`radiod.log`, `shell.log` (rotated once at 512 KB), `supervise-<name>.log`,
`crash-<name>-<time>.txt`, and `<name>.stdio.log` (LVGL and raw stdio,
restarted on every boot, previous copy in `.1`). `pos logs`, `pos logs
<name>`, `pos logs --crashes`.

Image contents from this package: `pos`, `pos-hwcheck`, `pos-supervise`,
`radiod` (mock + sx1262), `pocketos-shell` (DRM/evdev, untested), init
scripts `S60radiod` and `S90pocketos-shell`. Screenshots (`pos shell
screenshot`) are not available on the device: the vendor LVGL build has
LV_USE_SNAPSHOT off (docs/KNOWN_ISSUES.md).

Build inside WSL2 Ubuntu 22.04 (see docs/BUILD_ENVIRONMENT.md):

```sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/apply_to_sdk.sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/build_image.sh
```
