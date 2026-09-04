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
the display and the radio at boot. The PocketOS shell is installed as
`/usr/bin/pocketos-shell` with `S90pocketos-shell` disabled; radiod runs with
the mock backend. To try PocketOS on the panel:

```sh
/etc/init.d/S99zz_k230_phone_ui stop
echo ENABLE=1 > /etc/default/pocketos-shell
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
/etc/init.d/S60radiod restart
/etc/init.d/S90pocketos-shell start
pos app list
```

Image contents from this package: `pos`, `pos-hwcheck`, `pos-supervise`,
`radiod` (mock + sx1262), `pocketos-shell` (DRM/evdev, untested), init
scripts `S60radiod` and `S90pocketos-shell`.

Build inside WSL2 Ubuntu 22.04 (see docs/BUILD_ENVIRONMENT.md):

```sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/apply_to_sdk.sh
POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230 platforms/k230/scripts/build_image.sh
```
