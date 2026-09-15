# Doors graphics: hardware gate

Branch `rebrand/doors-graphics`.

**Result: PASS on unit A, 2026-09-15** (product owner's final visual
confirmation). The splash byte order, orientation and 1:1 placement are
VERIFIED on the panel, the System mark in every theme and display mode, and DS
Amendment C (§19) is accepted. One boot-path limitation was found and is
**not** a graphics failure: on a warm `reboot` the vendor U-Boot's first panel
bring-up leaves the panel dark, so the splash shows from power-on but not after
a warm reboot (below, and docs/KNOWN_ISSUES.md).

Not merged. No change to VERSION, internal names, Phase 2 or Phase 3.

## Before the bench

1. Build an image from the branch tip (`apply_to_sdk.sh`, `build_image.sh`);
   the build runs `verify_image.sh`.
2. `platforms/k230/scripts/verify_splash.sh <sysimage-sdcard.img>` must say
   `SPLASH: PASS` with SHA-256 `434f4a6c…8f94`.
3. Put the vendor splash back in the SDK overlay afterwards
   (docs/design/brand/README.md, "How it reaches the board").
4. Flash with the bench tool. Keep a serial console open for the U-Boot log.

## Checklist

What "right" looks like, so each item is a yes or no. Record every result as
VERIFIED (operator) or FAIL, with a photo where it helps.

| # | Check | Pass | Tells you, if it fails |
| --- | --- | --- | --- |
| 1 | Splash visible | U-Boot log has `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4` and no `size mismatch`; the panel shows the Doors splash, not the LILYGO one and not black | size or copy problem (`verify_splash.sh` should have caught it) |
| 2 | Portrait orientation | the mark is above the word "Doors", both upright | rows are stored bottom-up, or the layer is rotated |
| 3 | RGB channel order | the mark is light blue (`#8ccfff`), the word near-white, the background black | light orange (`#ffcf8c`) means R and B are swapped: fix the converter's byte order, not the art |
| 4 | No mirroring, clipping or stretching | "Doors" reads left to right; the mark's gap is at its lower **left**; the group is centred with even black margins, nothing cut off | pixels are stored right-to-left, or the layer size/offset is wrong |
| 5 | Splash-to-shell handoff | the splash holds until the Doors shell draws; no white, garbage or torn frame in between; `dmesg` shows `preserving boot splash` | panel/DRM handoff issue, unrelated to the file itself |
| 6 | System mark appearance | System app, scroll to the identity panel: the mark sits before "Doors" in the first row, crisp, same height band as the text, not blurred or blocky | A8 drawing or layout on the device |
| 7 | Mark follows the theme | Settings → Theme: the mark changes with each of the five themes, matching the back chevron's colour | the tint role is not repainting |
| 8 | Outdoor and Night | Outdoor: mark and name clearly legible. Night: dim like the rest of the screen, still recognisable | DS §13 levels on the AMOLED |
| 9 | Short app regression | launcher; open and close Radio, Fleet, Notes, Settings, System; one reboot; no crash report (`pos logs --crashes`), no ERROR in `shell.log` | the change touched something else |

## Result on unit A (2026-09-15)

**Card.** The development card, flashed with
`C:\K230\tools\flash-devcard.ps1 -Image C:\K230\out\doors-gfx-e999ab1\sysimage-sdcard.img -Sha256 25f2f273…`:
source gate PASS; target PhysicalDrive1, removable USB, 62,914,560,000 bytes,
reader serial 121220160204, disk 0 (NVMe system disk) excluded; 763,363,328
bytes written in 68.9 s; unbuffered read-back differs only at 440..443;
`RESULT: FLASH PASS`. On the device `/etc/pocketos-release` reads
`0.0.9 / BUILD_ID=e999ab1`, and `/boot/logo.xrgb` is 2,799,104 bytes with
SHA-256 `434f4a6c…8f94`.

**Provenance.** The candidate was built from `e999ab1`; every later commit on
the branch changes documentation only, none of which reaches an image. It was
built before any other branch used the shared SDK tree (it carries no
`/etc/doors-release`, `/usr/bin/doors` or `/usr/share/doors`).

| # | Result | Evidence |
| --- | --- | --- |
| 1 | **PASS** | operator, cold power-on after the flash; serial on every boot: `2799104 bytes read`, `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4`, no `size mismatch` |
| 2 | **PASS** | operator: portrait and upright |
| 3 | **PASS** | operator: colours correct |
| 4 | **PASS** | operator: no clipping, stretching or mirroring |
| 5 | **PASS** | operator, from a lit splash (boot from the U-Boot prompt): the splash held until the Doors shell took over, no white flash, garbage frame or tearing, the launcher appeared normally with **DOORS** top-left. Serial: `preserving boot splash` (reset and backlight left alone), `skip DRM fbdev setup to preserve boot splash`; `dmesg`: the panel is re-initialised at the shell's first mode set (`prepare: sending init commands`, 17.57 s) |
| 6 | **PASS** | operator: the mark before "Doors", crisp, sized right, the light-blue accent of the back arrow |
| 7 | **PASS** | operator: the mark changed with every switch and always matched the back arrow, no flicker; ten remote switches through `shell.theme` (the path Settings uses), all five themes twice, System on screen throughout, shell pid unchanged, 0 ERROR |
| 8 | **PASS** | operator: Outdoor clearly legible, nothing clipped or overlapping; Night dimmed consistently with the rest of the screen, matching the back arrow, still recognisable. Remote: theme and mode reported by `shell.info` and stored in `settings.conf` |
| 9 | **PASS** | remote, 21 checks, 0 failures: System, Notes, Timber, Fleet and Radar each launched and returned to the launcher; shell pid 292 throughout; no new crash report; no new ERROR or WARN in `shell.log`; empty `shell.stdio.log`; no new kernel errors; `sysd`, `radiod`, `netd` and the shell running with 0 restarts, both services answering; shell VmRSS 12,672 → 13,056 kB. Operator: nothing looked broken during the run. The reboot is covered by items 1 and 5 |

The unit was left at the launcher in Ice & Ember, Normal, with the Doors shell
owning the panel (`/etc/default/k230_phone_ui` `ENABLE=0`,
`/etc/default/pocketos-shell` `ENABLE=1`) and the bench SSH key installed.

### Found: no splash after a warm reboot (vendor U-Boot)

After a warm `reboot` the operator saw no splash; the unit booted straight to
the Doors shell. The U-Boot log was identical to a cold boot (splash loaded,
`OSD4` set up). Held at the U-Boot prompt, so nothing could replace the splash:

| | Panel | GPIO22 / GPIO25 | DSI `PHY_STATUS` |
| --- | --- | --- | --- |
| T1: warm reboot, U-Boot's first pass | **black** (operator) | 1 / 1 | `0x15bb`: lane 0 turned to receive, data lanes in stop state, so no video |
| T1b: the vendor `k230_logo` command run once more at the prompt | **Doors splash** (operator) | 1 / 1 | `0x1529`: lanes forward and streaming |

Panel power and reset were high throughout (the GPIO direction register held
only U-Boot's two outputs, so the warm reboot had reset it), the DSI host was
powered and video mode set in both. Booting Linux from the lit
state (T1b) gave the clean handoff recorded as item 5. So the splash file, its
format and the kernel handoff are right; the vendor U-Boot's first DSI bring-up
does not survive a warm reset. U-Boot, the kernel and the device tree are
untouched by this branch, so this is existing vendor behaviour (DOCUMENTED by
source; whether the stock LILYGO splash shows it too was not run). Recorded in
docs/KNOWN_ISSUES.md. A fix belongs in the vendor U-Boot overlay and needs the
owner's go.

### Harness incident, no product impact

During the first handoff attempt one remote command lost its quoting and
started a second `sysd` and `radiod` in the foreground, which took over their
socket names. They were removed (SIGKILL, so their cleanup could not unlink the
sockets) and both services restarted through their init scripts; the shell was
never touched. The single WARN left in `shell.log` (`radio.status poll failed:
send failed: Broken pipe`, 15:01:46) is from that restart. Every result above
comes from later clean boots; remote commands afterwards went through a script
file on stdin.

Evidence (not in the repository): `out/doors-gfx-e999ab1/` holds the flash log
and `hwgate-unitA/` the serial captures of each boot (`boot3-serial.log` warm
retry, `t1-serial.log` T1/T1b with the register reads, `n1-serial.log`
handoff), `theme-cycle.log` and `regression-unitA.log`.

## Validation done without hardware (2026-09-15)

Source `e999ab1`, from fresh WSL clones. The commit that adds this section
changes nothing but this sheet.

| Check | Result |
| --- | --- |
| `make all`, `make test` (host, -Werror) | rc 0; 3,491 ok, 0 FAIL, 0 warnings (master 8070379: 3,401) |
| New gates inside it | `boot_splash_test` (conversion, determinism, sizes, decode round trip), `brand_mark_test`, `splash_image_test`: 0 failures, none NOT RUN |
| Shell (SDL) build and shell tests | 0 warnings; 350 ok, 0 FAIL (master: 317); `system_brand_shell_test` 33 ok across 5 themes × 3 modes, live switch, reduced motion |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0, 4 warnings, all vendor ggwave, as on master |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings, `pos_brand_mark` linked |
| Image build | rc 0; `verify_image.sh` IMAGE GATE: PASS |
| `verify_splash.sh` | SPLASH: PASS, `/logo.xrgb` 2,799,104 bytes, SHA-256 `434f4a6cf697544764ccbd111d4b3a60eaf5ced5f86e41be731f1f7ce4f58f94` |
| Boot-partition splash decoded as B, G, R rows from the top | equals the source PNG's pixels; content x 184–383, y 517–715 |
| Shared SDK overlay afterwards | vendor splash restored, SHA-256 `9fd79fee…` |

Candidate image, not in the repository: `~/work/doors-gfx-image-e999ab1/` in
the WSL build host, copied to `C:\K230\out\doors-gfx-e999ab1\` for flashing,
`sysimage-sdcard.img` 763,363,328 bytes, SHA-256
`25f2f273a8f74ea2c2a1c59f4a6dbb0df00b5f1c3887103940a1b3996836359c`
(`.gz` 204,497,237 bytes, `f1ed26169244cc9b7a95c2cd5a27cd9e47cc5beef17926a2f9d3a8063a3f017a`).
BUILD_INFO says PocketOS 0.0.9 / `e999ab1`: VERSION is unchanged on purpose.
