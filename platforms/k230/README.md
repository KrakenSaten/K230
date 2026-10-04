# platforms/k230

Board support for the LILYGO T-Display K230, layered on the pinned LILYGO BSP
and Kendryte K230 Linux SDK (ADR-001).

Doors was previously known as PocketOS through v0.0.9. The package, defconfig,
init scripts, services, state paths and variables on this page keep their
PocketOS-era names (docs/decisions/ADR-005-product-name-doors.md). Since
Phase 2 the command-line tool is `doors`, with `pos` kept as a permanent
alias that prints what it always printed, and the release file is
`/etc/doors-release`, with `/etc/pocketos-release` a symlink to it; commands
written with `pos` keep working.

```text
configs/k230_pocketos_defconfig   Vendor board defconfig + BR2_PACKAGE_POCKETOS
package/pocketos/                 Buildroot package building the repository root Makefile
scripts/apply_to_sdk.sh           BSP overlay + Doors package into the SDK (removes the vendor launcher)
scripts/build_image.sh            Build and export sysimage-sdcard.img and doors-*.img.gz (+ .sha256) to out/k230/
scripts/deploy.sh                 Push built binaries to a running board over SSH
scripts/verify_image.sh           Refuse an image whose boot partition cannot boot
scripts/verify_splash.sh          Check an image's boot partition carries the committed splash
vendor_bsp_commit.txt             Pinned Xinyuan-LilyGO/T-Display-K230 commit
vendor_sdk_commit.txt             Pinned kendryte/k230_linux_sdk commit
```

The Doors shell owns the display by default, from the first boot of a
freshly flashed card: it is installed as `/usr/bin/doors-shell` with
`S90doors-shell` enabled. The vendor LVGL launcher (`k230_phone_ui`) is not
in the image: `apply_to_sdk.sh` no longer installs it, removes what an
earlier apply left in the SDK, and `build_image.sh` and `verify_image.sh`
refuse an image that still carries it (up to v0.3.0 it was installed and on
by default, which is why a fresh card used to boot the LILYGO launcher once;
after that it was installed but off). radiod runs with the mock backend, and
meshcored
is installed as `/usr/sbin/meshcored` with `S65meshcored` disabled
(`MESHCORED_ENABLE=1` in `/etc/default/meshcored` switches it on; see
docs/services/MESHCORED.md before doing that on a real radio).

## Panel ownership (persistent across reboots)

With no settings file (a fresh card), Doors has the panel; the radio is a
separate switch:

```sh
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
reboot
```

After the reboot: `doors app list`, `doors radio info`.
`echo ENABLE=0 > /etc/default/doors-shell` keeps the shell down (bench work
on the panel); remove the file or set `ENABLE=1` to hand the panel back.

A unit still on an image from before the launcher was removed may carry
`/etc/default/k230_phone_ui`. S90 honours `ENABLE=1` in it only while
`/etc/init.d/S99zz_k230_phone_ui` is installed, and always refuses to start
beside a running launcher, so the two never fight for DRM master. Once a
Doors image without the launcher is on the card, a leftover `ENABLE=1` is
ignored (S90 says so) rather than leaving the panel dark.

There is no on-device way back to the vendor launcher any more. The vendor's
own SD card is the reference image (docs/hardware/FIRST_BOOT.md,
"Recovery").

`/etc/default/doors-shell` is a whole file: keep `ENABLE=1` in it when
adding the bench overrides (`K230_LVGL_DRM_STAGING`, `POCKETOS_DRM_ROTATION`,
`POCKETOS_TOUCH_CALIB`, `POCKETOS_TOUCH_SWAP`, `POCKETOS_TOUCH_DEVICE`,
`POCKETOS_DRM_DEVICE`; exact usage in docs/hardware/BRINGUP_CHECKLIST.md
section 6).

If the device does not boot far enough for SSH, use the serial console
(115200) or swap back to the vendor SD card; see docs/hardware/FIRST_BOOT.md
"Recovery".

## First login and remote access (from v0.0.2)

The root account ships without a password, as in the vendor image, so the
serial console (UART0, 115200) always gives a root shell for recovery. From
v0.0.2 the network side is closed by default: sshd runs with
`PermitEmptyPasswords no` (apply_to_sdk.sh patches the vendor sshd_config)
and telnetd listens on 127.0.0.1 only (`/etc/default/telnet`). Nothing
secret is embedded in the image. To enable SSH, on the serial console:

```sh
passwd                      # sets the root password; SSH password login works from now on
# or, key only:
mkdir -p /root/.ssh && chmod 700 /root/.ssh
cat >> /root/.ssh/authorized_keys      # paste the public key, Ctrl-D
chmod 600 /root/.ssh/authorized_keys
```

Neither step survives a re-flash, which is intended. The USB LAN adapter has
no burned-in MAC, so its address and therefore the DHCP lease can change on
every boot: read the IP after each boot (`ip -4 addr show eth0`).

## Boot splash

U-Boot shows `/logo.xrgb` from the boot partition before Linux starts. The
Doors splash is `rootfs_overlay/logo.xrgb`, generated from the owner's
artwork by `tools/design/png2xrgb.py`; `apply_to_sdk.sh` puts it over the
vendor's copy and the vendor `post-image.sh` copies it to the boot partition.
It must stay exactly 568 × 1232 × 4 = 2,799,104 bytes, B, G, R, X per pixel,
or U-Boot skips it. Only a flash changes it: `deploy.sh` never writes the boot
partition. `scripts/verify_splash.sh <image>` checks a built image carries it.
Details and hashes: docs/design/brand/README.md; the hardware gate:
docs/hardware/DOORS_GRAPHICS_GATE.md. Doors includes the boot splash, but the
vendor U-Boot's display bring-up is intermittent: on some boots, cold or warm,
the panel stays black until Doors starts. The boot itself is unaffected
(known vendor limitation, docs/KNOWN_ISSUES.md).

## Logs on the device

`/var/log` is a tmpfs on this image, so Doors keeps its logs, crash
reports and supervisor logs in `/var/lib/pocketos/log` (persistent):
`radiod.log`, `shell.log` (rotated once at 512 KB), `supervise-<name>.log`,
`crash-<name>-<time>.txt`, and `<name>.stdio.log` (LVGL and raw stdio,
restarted on every boot, previous copy in `.1`). `doors logs`, `doors logs
<name>`, `doors logs --crashes`.

Image contents from this package: `doors` (and its `pos` alias),
`pos-hwcheck`, `pos-supervise`, `radiod` (mock + sx1262), `doors-shell`
(DRM/evdev, untested), init scripts `S60radiod` and `S90doors-shell`,
`/etc/doors-release` (and its `/etc/pocketos-release` alias), and the
third-party notices in `/usr/share/doors` (linked from `/usr/share/pocketos`).
Screenshots (`doors shell screenshot`) are not available on the device: the
vendor LVGL build has LV_USE_SNAPSHOT off (docs/KNOWN_ISSUES.md).

## Bench deploy and the Doors names

`scripts/deploy.sh` sends the aliases as symlinks, so a deploy onto a
PocketOS-era unit leaves one file under each pair of names, like a flashed
card. Rolling such a unit back with an older checkout's `deploy.sh` restores
the old files but deletes nothing: remove the Doors-only files afterwards with
`rm -rf /usr/bin/doors /etc/doors-release /usr/share/doors` (ADR-005,
"Upgrade and rollback").

Build inside WSL2 Ubuntu 22.04 (see docs/BUILD_ENVIRONMENT.md):

```sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/apply_to_sdk.sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/build_image.sh
```
