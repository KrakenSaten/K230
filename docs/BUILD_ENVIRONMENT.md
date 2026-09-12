# Build environment

Recorded 2026-09-04 from the local host and the pinned vendor sources.
Evidence classes: VERIFIED (observed here), DOCUMENTED (vendor source or docs),
ASSUMED (inference, not yet checked).

## Host

| Item | Value | Evidence |
| --- | --- | --- |
| Host OS | Windows 11 Pro 10.0.26200 | VERIFIED |
| Git | 2.55.0.windows.5 | VERIFIED |
| Python | 3.12.10 (native Windows) | VERIFIED |
| WSL2 | Ubuntu-22.04 installed 2026-09-04, user `dolby`, systemd enabled; `.wslconfig` sets memory=11GB, processors=8, swap=8GB | VERIFIED |
| Docker | Not installed | VERIFIED |
| cmake / make / gcc / ninja | Not on Windows; inside WSL: gcc 11.4, cmake 3.22.1, make 4.3, python 3.10 | VERIFIED |
| Free disk on C: | ~421 GB | VERIFIED |

The K230 Linux SDK cannot be built on native Windows. LILYGO explicitly
recommends WSL2 with Ubuntu 22.04 and building inside the Linux filesystem
(not under `/mnt/c`). The checkout used for builds is
`/home/dolby/work/t-display-k230` inside WSL (BSP bb831ab, SDK 22d02c6).

WSL gotchas found 2026-09-04 (all VERIFIED):

- WSL appends Windows PATH entries containing spaces. Buildroot aborts with
  "Your PATH contains spaces". Export a clean PATH before building:
  `export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`.
- The WSL VM shuts down a few seconds after the last `wsl.exe` client exits,
  killing background jobs and even transient systemd units. Long builds need
  a live client (an open Ubuntu terminal, or a hidden `wsl.exe` started with
  `Start-Process`). `vmIdleTimeout` in `.wslconfig` is the alternative.
- The primary toolchain mirror `ai.b-bug.org` does not resolve; the SDK
  script falls back to `download.kendryte.com`, which also serves Buildroot
  package downloads via `BR2_PRIMARY_SITE` at roughly 1 MB/s.
- Ubuntu 22.04 needs `python3 -m pip install pcpp` for the LVGL preprocessor
  (the SDK script does this; on 24.04 it is the `python3-pcpp` package).

## Target toolchain and sources

| Item | Value | Evidence |
| --- | --- | --- |
| Toolchain | Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2-20250410 | DOCUMENTED (SDK tools/install_toolchain_and_depend.sh) |
| Toolchain path | /opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/ (installed in WSL 2026-09-04, MD5 verified, gcc 14.1.1) | VERIFIED |
| Target triplet | riscv64-unknown-linux-gnu | DOCUMENTED |
| Target CFLAGS | -mcpu=c908v -mtune=c908 -mrvv-v0p10-compatible -mrvv-auto-vectorize | DOCUMENTED |
| Buildroot | 2025.02.1 | DOCUMENTED (SDK Makefile) |
| Linux kernel | ruyisdk/linux-xuantie-kernel @ 7d4e1f444f461dbe3833bd99a4640e7b6c2cd529 (6.6 series), defconfig `k230` + BSP fragments | DOCUMENTED |
| U-Boot | 2022.10, board `k230_canmv_t_display` | DOCUMENTED |
| OpenSBI | 1.4, generic platform, Linux payload | DOCUMENTED |
| LVGL | 9.x, pinned commit 59dc7e436ae97a25e32656739ea6a943f9f11b6a | DOCUMENTED |
| libgpiod | v2 API | DOCUMENTED |
| K230 Linux SDK | https://github.com/kendryte/k230_linux_sdk @ 22d02c6b6783a57a3aca7eb3160e313e772cb710 (branch dev) | VERIFIED locally as submodule |
| LILYGO BSP + launcher | https://github.com/Xinyuan-LilyGO/T-Display-K230 @ bb831ab358b66f5bd9a87ecd7c580fee4537492e (v0.2.4, 2026-09-03) | VERIFIED locally |
| LILYGO RT-Smart repo | https://github.com/Xinyuan-LilyGo/T-Display-K230_canmv_rt @ abb07090ad8a666ed7a5e097b3c714b918731645 | VERIFIED locally |
| Board defconfig | k230_canmv_t_display_rm69a10_defconfig | DOCUMENTED |
| Device trees built | k230-canmv-rm69a10, k230-canmv-v3, k230-canmv-rm69a10-hdmi | DOCUMENTED |

Do not upgrade any of these without a proposal. The BSP overlay is a patch
stack on top of the pinned SDK commit and will break on a different SDK.

## Build PocketOS (inside WSL2 Ubuntu)

The PocketOS scripts wrap the vendor flow below and add the PocketOS package
and defconfig. The vendor checkout lives in the Linux filesystem, the PocketOS
repository on /mnt/c is only read and rsynced from.

```sh
export POCKETOS_VENDOR_DIR=$HOME/work/t-display-k230
/mnt/c/K230/platforms/k230/scripts/apply_to_sdk.sh
/mnt/c/K230/platforms/k230/scripts/build_image.sh            # full image
/mnt/c/K230/platforms/k230/scripts/build_image.sh "" pocketos-rebuild   # package only
```

Output: `out/k230/sysimage-sdcard.img` plus BUILD_INFO.txt and SHA256SUMS.txt.
VERIFIED 2026-09-04: PocketOS 0.0.1 image built (rc=0, 763 MB, about two
hours after the vendor baseline, including two recoverable stops noted in
KNOWN_ISSUES). radiod links RadioLib and libgpiod2, the shell links the
vendor LVGL from staging. Nothing has booted on hardware yet.

### The build is a commit, and a dirty tree is refused

`apply_to_sdk.sh` assembles the package with `git archive HEAD`. Only tracked
files at HEAD travel, with the modes git records — which is what makes an
image a function of a commit rather than of whatever the build host happened
to have lying around. **Uncommitted work is never packaged.**

So a dirty working tree is refused before anything is packaged:

```
ERROR: the working tree is dirty, and the package is assembled with
       `git archive HEAD`. Uncommitted changes would NOT be included ...
```

This is not pedantry about hygiene. The loop it protects is *edit, build,
deploy, test on hardware*: without the guard that loop silently tests HEAD
while the result reads as evidence about the edit. It has happened —
`docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md` §5.2 records a keyboard
acceptance whose conclusion had to be withdrawn for a related reason, and the
v0.0.8 review's P1-6 is the same failure from the other direction.

Commit or stash, or override deliberately:

```sh
POCKETOS_ALLOW_DIRTY_BUILD=1 platforms/k230/scripts/apply_to_sdk.sh
```

**The override does not include your changes.** It does not widen what is
packaged and it cannot: there is no path in the script that puts the working
tree into the package. It only says "package HEAD, I know the tree is dirty",
prints a prominent WARNING naming the exact commit, and stamps BUILD_ID
`<commit>-dirty` so the artifact records that the tree it was built beside was
not clean. Same shape as `POCKETOS_ALLOW_PIN_DRIFT=1`, and like that one it
belongs in the build report when used.

Every successful run ends with a provenance summary — packaged commit,
worktree state, whether the override was used — so the answer survives a
`| tail`:

```
Provenance
  Packaged source : HEAD 5e2a05d via git archive
  Source worktree : clean
  BUILD_ID        : 5e2a05d
  The working tree is never packaged, with or without the override.
```

A release build must show `Source worktree : clean` and a BUILD_ID with no
`-dirty` suffix; `V0.0.8_RELEASE_SMOKE.md` already checks the latter on the
built binaries. `tests/build_outputs_test.sh` keeps the default path usable by
making sure no build output can dirty a checkout by accident.

Quick host checks without Buildroot: `make CC=gcc all` builds `pos`
natively; the Xuantie gcc with `-mcpu=c908v -mtune=c908` cross-builds it.

## Build vendor baseline (inside WSL2 Ubuntu, Linux filesystem)

```sh
sudo apt update && sudo apt install -y git ca-certificates build-essential make rsync python3
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
./scripts/setup_ubuntu.sh          # installs toolchain to /opt/toolchain (sudo)
./scripts/apply_to_sdk.sh          # copies BSP overlay + launcher into the SDK
./scripts/build_sdcard_image.sh    # full image
```

Output: `k230_bsp/images/sysimage-sdcard.img` (also
`k230_linux_sdk/output/k230_canmv_t_display_rm69a10_defconfig/images/`).
VERIFIED 2026-09-04: the vendor baseline built cleanly on this host (rc=0,
763 MB image, roughly four hours including one interrupted run); a copy is
kept in `out/vendor/` with BUILD_INFO.txt and SHA256SUMS.txt.

Incremental targets used by the vendor:

```sh
make -C k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig k230_phone_ui-rebuild
make -C k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig linux-rebuild
make -C k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig uboot-rebuild
```

## Flash

SD-card image, MBR partition table. Layout from genimage.cfg: SPL at 1M and
1.5M, U-Boot env at 0x1e0000, U-Boot at 2M, boot ext4 at 30M (80M),
rootfs ext4 at 128M.

```sh
sudo dd if=k230_bsp/images/sysimage-sdcard.img of=/dev/sdX bs=8M status=progress conv=fsync
```

On Windows a tool such as Rufus or balenaEtcher writes the same image.

## Deploy without reflashing

`scripts/deploy_launcher.sh <ip>` copies the launcher (and optionally
/boot/Image + DTB) over SSH as root and restarts the launcher via
`/etc/init.d/S99zz_k230_phone_ui`. PocketOS can reuse the same mechanism.

## Serial console

CH342K dual USB-UART on the board (DOCUMENTED, schematic): channel 0 is K230
UART0 (Linux console, 115200n8, `stdout-path = serial0`), channel 1 is K230
UART3. Note that the BSP also assigns UART3 to the optional nRF9151 base board.
No K230 was attached to this host on 2026-09-04, so COM port names are not yet
recorded.

## Before a release build

Three things that are easy to get wrong and fail late (see
docs/KNOWN_ISSUES.md, "Build environment"):

- `vendor/RadioLib` must be present at `034126e` (7.7.1) with no build
  products. It is gitignored, so `git clean -xdf` removes it, and
  `apply_to_sdk.sh` then fails at step 5/5 with a bare rsync error.
- Point `POCKETOS_VENDOR_DIR` at the WSL-native vendor tree
  (`~/work/t-display-k230`). The `/mnt/c` copy is a Windows checkout with CRLF
  line endings and its scripts fail with `env: 'bash\r'`.
- Do not export `GIT_DIR`/`GIT_WORK_TREE` around the build. A git worktree
  whose `.git` names a Windows path needs them, but exported globally they
  answer for that worktree on every `git -C` the build makes - including the
  pin check, which then refuses a correct vendor tree. Scope them with a `git`
  wrapper that sets them only for the worktree path.

## Shell simulator (PC, inside WSL with WSLg)

Needs `libsdl2-dev`, `pkg-config`, `cmake` and the pinned LVGL tree
(`vendor/lvgl` at 59dc7e4, mirrored to `~/work/lvgl` for speed).

```sh
cmake -S /mnt/c/K230/ui/shell -B ~/work/pocketos-build/shell -DLVGL_DIR=$HOME/work/lvgl -DPOCKETOS_DISPLAY=sdl
cmake --build ~/work/pocketos-build/shell -j8
POCKETOS_RUNTIME_DIR=/tmp/pos /mnt/c/K230/services/radiod/radiod --backend mock &
POCKETOS_RUNTIME_DIR=/tmp/pos ~/work/pocketos-build/shell/pocketos-shell        # window via WSLg
```

Headless screenshots for design review (no window needed):

```sh
SDL_VIDEODRIVER=dummy ~/work/pocketos-build/shell/pocketos-shell --open radio --screenshot out/sim/radio.png --exit-after-ms 2500
```

`POCKETOS_SDL_ZOOM` scales the 568x1232 window (default 0.5). The DRM
backend (`-DPOCKETOS_DISPLAY=drm`) compiles against libdrm/libevdev from
Buildroot and is untested until hardware is available.

## Fonts (host-only, regeneration only)

The Design System fonts are committed as generated C files. To regenerate:
Node 20 LTS lives as a plain tarball in `~/tools/node` inside WSL with
`lv_font_conv` 1.5.3 installed under `~/tools/npm` (no system packages), and
the IBM Plex TTFs plus their OFL text are in `~/work/fonts`. Then:

```sh
PATH=$HOME/tools/node/bin:$HOME/tools/npm/bin:$PATH tools/design/gen_fonts.sh $HOME/work/fonts
```

## Test

Native, inside WSL (needs `libcjson-dev`, installed 2026-09-04):

```sh
cd /mnt/c/K230 && make CC=gcc CFLAGS="-O2 -Wall -Wextra -Werror" test
```

Runs `tests/airtime_test` (LoRa time-on-air reference values),
`tests/pocketlog_test` (log format, rotation, crash report),
`tests/radiod_mock_test.sh` (radiod mock backend and `pos radio` end to end
over pocketipc, including region guard and protocol robustness) and
`tests/supervise_test.sh` (crash-loop detection, clean stop). The shell has
its own headless test, run after the CMake build:

```sh
SHELL_BIN=~/work/pocketos-build/shell/pocketos-shell bash tests/shell_ipc_test.sh
``` There is no
on-device test runner yet; `pos-hwcheck` is the manual first-boot check. Vendor provides
`scripts/dev/*.sh` hardware scripts (LoRa pair test, Meshtastic matrix) that
run against two boards over SSH and are useful as templates.
