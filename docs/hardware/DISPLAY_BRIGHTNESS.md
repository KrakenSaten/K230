# Display brightness on the T-Display K230

Recorded 2026-09-12 from a read-only study of the pinned kernel build tree
(`k230_pocketos_defconfig`, the tree unit A's kernel was built from), the
LILYGO BSP patches, the vendor launcher and the unit A bench records. No
brightness value was written to any board while this was prepared.

Evidence classes as in `T-DISPLAY-K230.md`: VERIFIED (unit A runtime record),
DOCUMENTED (source, device tree, config), ASSUMED (inference).

Paths below: `K` = the kernel build tree
`output/k230_pocketos_defconfig/build/linux-7d4e1f444f461dbe3833bd99a4640e7b6c2cd529`,
`DTS` = its preprocessed `arch/riscv/boot/dts/canaan/.k230-canmv-rm69a10.dtb.dts.tmp`.
The DTB shipped in the v0.0.6, v0.0.8 and v0.0.9 images has the same SHA-256
as the build-tree DTB, and unit A's `uname` matches that tree's Image build
time (VERIFIED).

## The mechanism

| Fact | Value | Evidence |
| --- | --- | --- |
| Panel driver | `K/drivers/gpu/drm/panel/panel-canaan-universal.c`, compatible `canaan,universal` (LILYGO patch 0049), built in (`CONFIG_DRM_PANEL_CANAAN_UNIVERSAL=y`, `CONFIG_BACKLIGHT_CLASS_DEVICE=y`) | DOCUMENTED |
| Backlight device | `/sys/class/backlight/rm69a10`, registered by that driver (type RAW, `update_status` only) because the DT node carries `canaan,dsi-command-backlight` | VERIFIED present on unit A (`hwcheck-unitA/*/report.txt`, Display section) |
| How a level reaches the panel | one MIPI DCS **0x51** (`SET_DISPLAY_BRIGHTNESS`) with an **8-bit** parameter, sent by the DSI host as a long write (0x39, length 2). No PWM, no GPIO | DOCUMENTED |
| Range | `max-brightness` 255, `default-brightness` 254 (DTS); the init sequence itself also sends `51 FE`; the driver clamps to 255 | DOCUMENTED |
| Boot level | 254 on every boot; nothing persists in the kernel | DOCUMENTED; 254 read on unit A 2026-09-07 (BRINGUP_SESSION_2026-09-07.md) VERIFIED |
| Level 0 | sends `0x51 00` and nothing else: GPIO25 stays high, no display-off (0x28) or sleep (0x10) command. Whether the AMOLED is then fully black is unknown | DOCUMENTED; visual result UNKNOWN |
| `bl_power` | any non-zero value also sends `0x51 00` but keeps the stored level | DOCUMENTED |
| GPIO25 | panel power enable (`backlight_gpio-gpios` in the DT, `power_on` in the driver): high in prepare, low in unprepare. It is **not** a brightness control | DOCUMENTED; line 25 held as `backlight_gpio` on unit A VERIFIED |
| fbdev blanking | not applicable: `canaan,skip-fbdev-setup`, no `/dev/fb0` | VERIFIED |
| Runtime safety | after enable the DSI host is in video mode and sends commands in high-speed mode; that the panel accepts a 0x51 during video is ASSUMED. A vendor FIXME about DCS after the HS switch reads as text inherited from another driver, not K230 evidence | DOCUMENTED (host config), ASSUMED (panel behaviour) |
| Acknowledgement | none: the write only waits for FIFO space, so a successful `write()` does not prove the panel changed | DOCUMENTED |
| Locking | sysfs writes are serialised by the backlight core; a write racing a modeset could land inside the panel init sequence. Risk ASSUMED low: PocketOS never modesets after start | DOCUMENTED, ASSUMED |
| DRM master | not needed; the sysfs path never touches DRM state | DOCUMENTED |
| Permissions | `brightness` is 0644 root; PocketOS processes run as root | DOCUMENTED; runtime mode UNKNOWN |
| HDMI variant | `k230-canmv-rm69a10-hdmi.dts` has no `canaan,universal` panel and the LT9611 driver has no backlight: no device, brightness unsupported | DOCUMENTED |
| High brightness mode, 0x53 | no init sequence sends 0x53 (WRCTRLD); whether 0x51 needs it on this panel and whether HBM exists are unknown (no datasheet in the repo) | UNKNOWN |

The vendor launcher uses the same attribute: first `/sys/class/backlight/*`,
decimal writes, a Settings slider from **20** to max, persistence in its own
settings file, and a screen-off fade to 0 (DOCUMENTED, launcher source,
mechanism only). Its floor of 20 is a UI choice, not a measurement.

## What PocketOS implements

- `ui/shell/brightness.[ch]`: the HAL. Finds the first backlight-class device
  with at least 10 levels, maps percent to raw as `raw = (pct * max + 50) /
  100`, and writes only percentages in **10..100**. Pure C with the sysfs root
  passed in; `tests/brightness_test.c` covers it against a fake tree.
- **Safe minimum 10 %** = raw 26 of 255, above the vendor floor of 20. Raw 0
  can never be requested: the HAL clamps, and `shell.brightness` rejects
  anything outside 10..100 before it reaches the HAL. Checked on unit A on
  2026-09-13: readable and comfortable in Normal, readable but marginal in
  Night, so the floor stays at 10 % (below).
- The **shell owns it**, because the shell owns the panel (ADR-002): one probe
  at start, `pocketos_shell_brightness_get/_set` in `ui/shell/app.h` for apps,
  and `shell.brightness` over pocketipc (`docs/api/shell.md`, `pos shell
  brightness [10..100]`). Settings calls the shell; it has no sysfs code.
- **Persistence**: `display_brightness=<percent>` in
  `/etc/pocketos/settings.conf`, written only after the panel accepted the
  level. **Startup**: applied once, before the first frame. Nothing stored
  leaves the boot level (254) untouched. An invalid stored value (not a plain
  integer 10..100) is logged, not applied and not rewritten, like a rejected
  theme (DS §8).
- **Unsupported display** (simulator, HDMI build): `supported: false`, a set is
  refused with code 6, the stored preference is kept for a panel that has the
  control, and nothing is logged as an error.

Recovery if the panel is ever too dark to use: over SSH, `pos shell
brightness 100`; or stop the shell, remove the `display_brightness` line from
`/etc/pocketos/settings.conf` and reboot (the panel boots at 254).

## Unit A validation, 2026-09-13

Build `3d4a6e7` deployed as userspace onto unit A's v0.0.9 card; the owner at
the panel, everything else over SSH. Owner = seen on the panel by the product
owner.

| Step | Result | Evidence |
| --- | --- | --- |
| Boot level | `brightness 100% (rm69a10)`, raw 254; log `nothing stored, left as booted` | VERIFIED |
| 50 % | raw 128, `actual_brightness` 128; panel dimmer, clean, no flicker, touch works; no DSI or panel error in dmesg | VERIFIED, owner |
| 10 % (floor), Normal | raw 26; readable and comfortable | VERIFIED, owner |
| 10 %, Night | raw 26; readable but marginal | VERIFIED, owner |
| Below the floor | `pos shell brightness 9` and `0` refused with code 2, raw unchanged | VERIFIED |
| Settings | showed 10 % with `-` disabled; five `+` taps each brightened the panel immediately; log 20 → 60 %, raw 153, `display_brightness=60` stored | VERIFIED, owner |
| Shell restart | raw 153, `restored to 60%` | VERIFIED |
| Reboot | panel boots at 254, the shell restores 60 % (raw 153) before its first frame | VERIFIED |
| Invalid stored value | `display_brightness=seven`: warning `stored display_brightness=seven is not 10..100, left as booted`, raw left as it was, the line not rewritten, shell running with 0 restarts | VERIFIED |
| 100 % | raw 255 | VERIFIED |

**Decision: the 10 % floor stays**; no code change. DCS `0x51` during video
mode was exercised about a dozen times without a DSI error or visible artefact,
which retires the "ASSUMED safe" note for this panel. Not tested: raw 0 and
`bl_power` (unreachable from PocketOS), 10 % in Outdoor, and the
no-backlight path (host-tested only). Unit A was left at 60 %.

Bench note: a shell restarted from an SSH session inherits its umask (0077),
so a `settings.conf` it rewrites comes out 0600 instead of 0644. Every reader
is root, so nothing breaks. It was set back to 0644 by hand, and the
boot-started shell's later writes kept 0644.

## Physical validation procedure

Operator at the panel, device on the bench network, PocketOS image with this
branch deployed. Stop at the first unexpected result and restore with step 9.

1. Read-only inventory, record the output:
   `ls -l /sys/class/backlight/rm69a10/; for f in type max_brightness brightness actual_brightness bl_power; do printf '%s=' $f; cat /sys/class/backlight/rm69a10/$f; done`
   Expect `max_brightness=255`, `brightness=254`, `bl_power=0`.
2. `pos shell brightness` → expect `brightness 100% (rm69a10)`.
3. `pos shell brightness 50`; operator: panel visibly dimmer, no flicker, no
   tearing, touch still works. `cat /sys/class/backlight/rm69a10/brightness`
   → `128`. `dmesg | tail -5` → no DSI or panel error.
4. `pos shell brightness 100` → panel back to full; raw `255`.
5. Lowest level: `pos shell brightness 10` (raw 26); operator: launcher text
   still readable indoors? Then in Outdoor mode (`pos shell theme ice
   outdoor`)? Record the answer; it decides whether the 10 % floor stays.
6. Settings app: open Settings, Display: the value matches step 5, `-` is
   disabled at 10 %, `+` steps by 10 up to 100 and is disabled there; each
   step changes the panel immediately.
7. Persistence: set 60 in Settings, `grep display_brightness
   /etc/pocketos/settings.conf` → `display_brightness=60`; `reboot`; after
   boot the panel comes up at the boot level and dims to 60 % when the shell
   starts; `cat .../brightness` → `153`; `grep 'brightness:'
   /var/lib/pocketos/log/shell.log` shows `restored to 60%`.
8. Shell restart: `/etc/init.d/S90pocketos-shell restart`; brightness stays 60 %.
9. Restore: `pos shell brightness 100`.

Deliberately not part of the test: level 0 and `bl_power` (they are not
reachable from PocketOS), and anything with the HDMI DTB.
