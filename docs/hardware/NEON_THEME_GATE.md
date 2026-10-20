# Neon theme (PR #98) on unit B: the gate

**Status: PREPARED, hardware session not started (2026-10-20).** The K230
cross-build passed and a reversible test kit is ready. Nothing has been staged
on, or changed on, unit B. Owner visual approval: **not given.** HDMI:
**untested** (not part of this session; needs separate approval).

Evidence classes as in `docs/hardware/T-DISPLAY-K230.md`. Software tests,
device observations, owner observations and performance measurements are kept
in separate sections and never merged.

## Build under test

| | |
| --- | --- |
| PR | KrakenSaten/K230#98, draft, branch `design/rift-neon-activity-inspiration` |
| Commit | `56aefe5159ffa544908272107bb491ee434ac56c` (PR head confirmed 2026-10-20 against origin) |
| Base | origin/master `0f7c587` (v0.3.7 download links); merge base equals master, no rebase needed |
| Integration build | **none needed**: unit B's installed shell is `d7ace73`, which master and therefore this PR contain (see below) |

## Unit B as found (read-only inspection, 2026-10-20 08:08 unit time)

SSH `root@172.16.5.198`, host key from the PC's `known_hosts` (ed25519
`AAAAC3NzaC1lZDI1NTE5AAAAIEtJhNAn…`), `StrictHostKeyChecking=yes`. All VERIFIED.

| Item | Found |
| --- | --- |
| Release | `/etc/doors-release` 0.3.6, `BUILD_ID=9ad64a1`; kernel 6.6.36, up 21 h |
| `/usr/bin/doors-shell` | 4,469,104 bytes, Oct 14, sha256 `2d96a53a2cc951ef6983f589b4341aa71cade15c0a3b7e8887f0fa7067cac001`, embeds `BUILD_ID=d7ace738` (master commit `d7ace73`, "decoded basemap tiles live by the disk cache's expiry"). Not the 0.3.6 image's shell. No `neon`, no `lv_qrcode_create` |
| Supervision | `pos-supervise doors-shell /usr/bin/doors-shell` from `/etc/init.d/S90doors-shell`; environment `K230_LVGL_DRM_STAGING=1`, `POCKETOS_LOG_DIR=/var/lib/pocketos/log`, `POCKETOS_LOG_STDERR=0`; no `/etc/default/doors-shell` |
| `/usr/lib/liblvgl.so.9.5.0` | 2,902,192 bytes, sha256 `43f3f26571195297e9bec3e2c4ac2bca0358cbd07f030351d6fc700e66c7a761`; **no QR symbols** (0.3.6 configuration) |
| Other libraries the shell needs | `libcjson.so.1` 1.7.18, `libgpiod.so.3` 3.1.1, `libdrm.so.2` 2.124.0, `libevdev.so.2` 2.3.0, `/lib/libatomic.so.1` present |
| `/usr/sbin/meshcored` | PR #100 candidate (Oct 19), rollback in `/root/rollback-explicit-path`; not touched by this gate |
| Settings `/etc/pocketos/settings.conf` | sha256 `34b44ebcd747d4d618a42b5b7f594eb6740b6b875731376bdc436b6002a02b85`: `theme=doors`, `display_mode=normal`, `display_rotation=automatic`, `text_size=large` |
| Display | `card0-DSI-1` connected, 568x1232, "Canaan CanMV-K230 with RM69A10 OLED"; no `force_dtb` HDMI mode |
| Input | power key (event0), BOOT key (event1), `goodix_ts` touch (event2). **No keyboard attached** |
| Memory, storage | 990 MB RAM, 885 MB free; rootfs 159.8 MB free |
| Existing rollback directories | `/root/rollback-{deskbuddy-opus,explicit-path,ninjaone,rift-basemap,rift-correctness,settings-power-fit,sol-indices,vision-hdmi,vision-hdmi-2}`, `/root/sol-gate`: left alone |

## Compatibility findings

1. **The installed LVGL cannot run this shell.** QR support (offline Help &
   Feedback, `95d759a`) entered with v0.3.7, after unit B's 0.3.6 image. The
   PR shell imports 290 `lv_*` symbols; 6 are absent from the installed
   library: `lv_qrcode_create`, `_set_dark_color`, `_set_light_color`,
   `_set_quiet_zone`, `_set_size`, `_update` (VERIFIED, `nm -D`).
2. **The kit therefore carries its own LVGL**, built from the pinned LVGL
   `59dc7e4` with the vendor package's patches 0001–0004 (incl. the K230 DRM
   staging scanout buffer) and the package's `lv_conf.defaults` plus
   `LV_USE_QRCODE 1` — the one line `apply_to_sdk.sh` adds since v0.3.7.
3. **Equivalence check** (VERIFIED): a control build of the same source with the
   unchanged defaults exports exactly the same 3,404 dynamic symbols as unit B's
   installed `liblvgl.so.9.5.0`. The QR build adds only the 23
   `lv_qrcode_*`/`qrcodegen_*` symbols. The generated `lv_conf.h` of the two
   differ in `LV_USE_QRCODE` only. `liblvgl_demos`/`_thorvg` export the same
   symbols as unit B's; `_examples` adds one (the QR example). Not
   byte-identical to unit B's files (the Doors image build is not
   reproducible on this host: its `k230_pocketos_defconfig` output stopped at
   e2fsprogs on 2026-09-29); the rebuilt libraries additionally list
   `libatomic.so.1`, which unit B has.
4. QR does not change any shared structure: in the pinned tree
   `LV_USE_QRCODE` appears outside `src/libs/qrcode/` only in
   `lv_conf_internal.h` (VERIFIED, staged headers).
5. The shell links: `libcjson.so.1 libm.so.6 liblvgl.so.9 libpthread.so.0
   libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1 libc.so.6`, highest
   symbol version `GLIBC_2.33` — the same set as earlier gate shells.
6. Apps started by the test shell inherit `LD_LIBRARY_PATH` and so load the
   kit's LVGL too. Given 3 and 4 that is ABI-compatible; it ends with the test
   shell.

## Focused review of target risks

| Risk | Finding |
| --- | --- |
| LVGL shadow/outline support | `LV_DRAW_SW_COMPLEX 1` in the target configuration, so outlines and shadows draw (VERIFIED, staged `lv_conf.h`). `LV_DRAW_SW_SHADOW_CACHE_SIZE 0`, one SW draw unit, `LV_OS_NONE`: every Neon tube shadow (width 1, spread 2–3) is recomputed on each redraw. Bounded and static, but real per-frame work: **measure** |
| Launcher object ownership | The legacy art moves under one non-clickable container; the Neon well, symbol and focus ring are non-clickable children of the cell. All are owned by `c->obj` and deleted with it; `struct cell` keeps no pointer to them; symbol sources are static masks. Touch hit-testing still lands on the cell. No issue found |
| Hidden presentation cost | The inactive presentation is `opa` transparent, and LVGL skips a transparent subtree whole (`lv_refr.c:502`, `opa_layered <= LV_OPA_MIN`). Cost in existing themes: three to four extra objects per cell, no drawing |
| Theme switching and restoration | Every role Neon modifies is reset in the normal fill path before Neon refills it (`reset()` or `env_text()`); the variant roles are reset or `lv_style_copy`'d, and the pinned `lv_style_copy` resets its destination first (`lv_style.c:210`). Switch-back was validated in the simulator; still to be **observed** on the panel |
| Shared-library compatibility | See above: needs the kit's QR LVGL; nothing else differs |
| Added drawing work | Besides the shadows: in Neon the launcher photograph gets `image_recolor_opa` 210 (Night already used 150), a per-pixel mix over a full-screen image on every launcher redraw; symbols are 32 px A8 masks drawn at scale 512 (transformed). Likeliest target cost; the session compares CPU in Doors vs Neon on the same binary |

No build or correctness blocker was found, so the PR code is unchanged by
this preparation.

## Cross-build (VERIFIED, 2026-10-20, WSL build host)

Script: [`neon-gate/build_neon_k230.sh`](neon-gate/build_neon_k230.sh), from a
`git archive` of `56aefe5` (tar sha256
`4706bad90e64c935f9e941b854e98904c7b0215735a428433aab5d0c9b8f2ee3`) with
`BUILD_ID` `56aefe5` beside `VERSION`.

- Toolchain and sysroot: the pinned SDK (`22d02c6`, BSP `bb831ab`) Buildroot
  output `k230_canmv_t_display_rm69a10_defconfig` (Xuantie 900 gcc 14.1.1,
  `-mcpu=c908v -mtune=c908`), its `toolchainfile.cmake`. No toolchain, SDK or
  dependency upgraded; no SDK copied.
- Sysroot for the shell: a hard-link copy of that sysroot (no data copied, the
  vendor files untouched — a symlink farm fails because ld will not apply the
  sysroot to libc's linker scripts outside it) with only `usr/include/lvgl` and
  `usr/lib/liblvgl*` replaced by the QR build.
- Shell: `pocketos.mk`'s flags, `-DCMAKE_BUILD_TYPE=Release
  -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot`. PocketTimber art
  converted (not the placeholder).

| Result | |
| --- | --- |
| LVGL control / QR | rc 0, 3 warnings each, all upstream `lv_matrix.c:131-132` `-Wuninitialized` |
| Shell | rc 0, **0 warnings** |
| Shell identity | strings `0.3.7`, `56aefe5`, `Neon`/`neon`; `POCKETOS_TEST_CHROME` absent |
| Format | ELF 64-bit LSB pie, UCB RISC-V, RVC, double-float ABI, interpreter `/lib/ld-linux-riscv64-lp64d.so.1` |

Stripped with `--remove-section=.comment --remove-section=.note`.

| Artefact | Bytes | sha256 |
| --- | --- | --- |
| `doors-shell` | 4,259,608 | `82dee097275c7a631cc6fa7310bfde40b7a2dc6830781f385ec326912d2ffed2` |
| `doors-shell.unstripped` (build host only) | | `80a82d17a172988dcd314e661f2a5437d7667e9b158231291988e8d30963f506` |
| `lib/liblvgl.so.9.5.0` (QR) | 2,910,384 | `2c4fd81b848331af7904ed28e590bbe9e99e8170a51b4822b8e18184f6d29a5a` |
| `lib/liblvgl_thorvg.so.9.5.0` | 563,104 | `b48705ee92878e86a64c9145b279905388a856c0887026367b3bbef727b3f903` |
| `lib/liblvgl_demos.so.9.5.0` | 1,536,416 | `2ecc36874c8ea88470a0d4f4c084e98bf5b06f124a8b8942d1a09acc68329b7c` |
| `lib/liblvgl_examples.so.9.5.0` | 626,712 | `90a65539531a1479ab7b5738bbacfce4e5f8d9df8f01dd384778b62e341f6518` |
| control `liblvgl.so.9.5.0` (QR off, comparison only) | | `4601827cec8e7a892cc51cc35219c41cea818a1565d5521372c44cd326c48a26` |

Build tree: `~/work/neon-p98/56aefe5/` in WSL (user `kraken`).

## The test kit

`neon-p98-56aefe5.tar` (sha256
`732e2e01a7f0396847b4660e9d1247b5e728c35c91b89a7d065d732564bcd151`), built by
[`neon-gate/assemble_kit.sh`](neon-gate/assemble_kit.sh); on the PC in
`C:\K230-wt\neon-hwkit\`. It unpacks to **`/root/neon-p98-56aefe5/`** on unit B
(about 10 MB, rootfs has 159.8 MB free):

| File | Role |
| --- | --- |
| `doors-shell` | the PR shell above |
| `lib/liblvgl*.so.9.5.0` + `.so.9` links | the QR LVGL set, loaded only through the test shell's `LD_LIBRARY_PATH` |
| `kit.env` | the commit and unit B's original hashes (shell, LVGL, settings) |
| [`neon-start.sh`](neon-gate/neon-start.sh) | checks the kit and unit B's original hashes; backs up `settings.conf` with a hash check; stops `S90doors-shell` through its init script; refuses to go on while any shell, the shell's supervisor or any `/dev/dri/card0` holder remains; starts the test shell with the installed service's environment plus `LD_LIBRARY_PATH=<kit>/lib` and logs in `<kit>/log`; checks the test shell is the only panel owner |
| [`neon-restore.sh`](neon-gate/neon-restore.sh) | stops the test shell (TERM, then KILL after 10 s); stops the installed service if it runs; restores `settings.conf` from the hash-checked backup; checks shell, LVGL and settings hashes; starts `S90doors-shell` only when no shell and no panel owner remain; requires exactly one `doors-shell`, `/usr/bin/doors-shell`, without `LD_LIBRARY_PATH`. Idempotent; also correct after a failed start or a reboot |
| [`neon-status.sh`](neon-gate/neon-status.sh) | read-only: panel owners, shells and their loaded `liblvgl`, supervisor, settings |
| [`neon-measure.sh`](neon-gate/neon-measure.sh) | read-only: 1 s samples of the shell's CPU (utime+stime, `USER_HZ` 100 ASSUMED) and RSS for N seconds; a summary line to `<kit>/log/measure.txt` |

**Replaced on unit B: nothing.** `/usr/bin/doors-shell`, `/usr/lib/liblvgl*`,
`/etc/init.d/*`, `/boot` and `meshcored` are not written. Written by the
session: `/root/neon-p98-56aefe5/` (kit, backup, logs) and
`/etc/pocketos/settings.conf` (by the test shell when theme or rotation
change; restored from the backup).

### Recovery

1. Normal: `/root/neon-p98-56aefe5/neon-restore.sh`, expect `RESTORE: OK`.
2. If the panel is dark or the shell hangs: same script over SSH, or on the
   serial console (COM3, 115200 8N1).
3. If the unit is rebooted mid-session: it boots the installed service
   unchanged. A `theme=neon` left in settings is unknown to `d7ace73`, which
   falls back to `doors` (VERIFIED in source, `pos_theme.c`
   `pos_theme_fallback_id`). Run `neon-restore.sh` to put the original
   settings back.
4. Manual equivalent: `kill` the test shell; `/etc/init.d/S90doors-shell stop`;
   `cp -p /root/neon-p98-56aefe5/backup/settings.conf /etc/pocketos/settings.conf`;
   `sha256sum` it against `34b44ebc…`; `/etc/init.d/S90doors-shell start`.

The kit directory is kept after the session as evidence; removing it is a
separate decision.

## Hardware session plan (after approval)

0. Stage: `scp` the tar to `/root/`, unpack, `sha256sum -c SHA256SUMS`.
   Re-run the read-only inspection; stop if unit B differs from the table above.
1. `neon-start.sh`; `doors shell info` must report `56aefe5`. Owner selects
   Settings → Appearance → Neon.
2. Owner inspects launcher, Settings and RIFT: colours, readable text,
   unclipped focus rings and tube edges. Captures with the shell's screenshot
   facility where possible.
3. Touch (keyboard only if one is attached; none was on 2026-10-20).
4. Portrait and landscape (Settings → Display → Rotation), back to `automatic`.
5. Performance: the same short sequence (launcher page swipes, open/close
   Settings, open RIFT) under `neon-measure.sh` once in Doors and once in
   Neon, same binary. Host timings are not target evidence.
6. Switch back to Doors and check the original appearance returns.
7. `neon-restore.sh`, expect `RESTORE: OK`.

## Software tests

Reused from the PR's cloud validation (not repeated here): host simulator
builds; theme, Settings, launcher, shell lifecycle and RIFT checks; portrait,
landscape, three text sizes, representative HDMI captures; fifteen
existing-theme frames unchanged; live switch back restores the original.
Added here: the K230 cross-build and the library checks above.

`tests/license_audit_test.sh` at `56aefe5` failed twice on files this PR adds:
`tests/neon_shell_test.py` had no SPDX line, and the Neon captures under
`docs/design/neon/` had no asset class. Both fixed in the commit that adds
this sheet (SPDX line; `docs/design/neon/*` as `CAPTURE`, which is what they
are: simulator captures and sheets composed from them). **One failure
remains:** `docs/design/rift/concepts/neon-activity-landscape.svg` needs a
class, and its provenance (drawn in this repository, `ORIGINAL`, or made by
the owner with an AI tool, `OWNER-DESIGN`) is not recorded. That is the
owner's call; it does not affect the hardware session.

## Device observations

Not yet recorded.

## Owner observations

Not yet recorded. Visual approval: not given.

## Performance measurements

Not yet recorded.

## Not tested

HDMI output on hardware (no HDMI boot in this session without separate
approval). Keyboard input (no keyboard attached at inspection).
