# PocketOS roadmap

Product owner sets priorities. Dates are absolute; "done" means built and
tested on the stated platform.

## Phase 1: Core v0.1 (current)

Goal: a PocketOS image that boots on the T-Display K230, shows the shell,
and exposes radio, network and system information through services and the
`pos` CLI.

| Item | Status 2026-09-04 |
| --- | --- |
| Build environment (WSL2, toolchain, pinned SDK/BSP) | done, docs/BUILD_ENVIRONMENT.md |
| Hardware baseline with evidence classes | documented, unverified on hardware |
| Repository skeleton, Buildroot package, defconfig | done, PocketOS 0.0.1 image built 2026-09-04 |
| `pos` CLI (system, hardware, network, radio) | done on PC, riscv64 compiles |
| `pos-hwcheck` first-boot inventory | done, VERIFIED on unit A (inventory 2026-09-07, `--lora` probe 2026-09-09) |
| pocketipc v0 | done, tested |
| radiod with mock backend, region guard, airtime stats | done, tested |
| radiod sx1262 backend (own HAL + RadioLib 7.7.1) | written, compiles for riscv64, untested |
| Shell: status bar, launcher, app host, SDL simulator, `shell.*` IPC | done on PC |
| Shell DRM/evdev backend | written, untested |
| Shell in the Buildroot image, replacing the vendor launcher | installed (S90 disabled by default); replacement not started |
| Logging library, crash reports, `pos logs` | done, tested on PC |
| Service respawn with backoff, crash-loop detection | done (`pos-supervise`), tested on PC |
| sysd: `system.info`, `system.status` (identity, resources, storage, network summary, service health) | done (v0.0.7 blocks 1 and 2a), validated on unit A from `3a56804` (docs/hardware/V0.0.7_BLOCK2A_SMOKE.md); service health from the supervisor state file, validated on unit A from `792f754` (block 2b, docs/hardware/V0.0.7_BLOCK2B_SMOKE.md) |
| netd: Ethernet, Wi-Fi (wpa_supplicant), BLE status | not started |
| Settings app (network, display, system) | not started |
| Shell System Status screen (vitals, storage, network, services, radio, identity, restart, power off) | done (v0.0.7), validated on unit A from `b9203c8`, with the dialog hierarchy and the radio chip corrected and re-verified from `dbba4a0` (docs/hardware/V0.0.7_SYSTEM_STATUS_SMOKE.md) |
| Reboot/shutdown, hardware info app | `system.reboot` and `system.poweroff` done (v0.0.7 block 2c); both validated on unit A, reboot from `db529fb` and poweroff operator-attended from `fdc795f` (docs/hardware/V0.0.7_BLOCK2C_SMOKE.md). Hardware info app not started |
| Basic updater (image on SD, no rollback) | not started |

v0.0.7 is code-complete, validated on unit A across four bench sheets, and
validated as a release image: built at `4ab5a55`, flashed and cold-booted, PASS
on all nine steps of the gate (docs/hardware/V0.0.7_RELEASE_SMOKE.md, gate in
V0.0.7_PRE_RELEASE_CHECKPOINT.md). Not tagged and not merged.

Exit criteria for v0.1: boots from SD on both units, shell usable by touch,
`pos hwcheck` report attached to docs/hardware, radiod sends and receives a
packet between the two units, Wi-Fi joins a network from the Settings app.

## Status 2026-09-08

One development stream from here on; the earlier idea of a second
parallel track is withdrawn.

**v0.0.3** exists on `integration/v0.0.3-platform`, on top of the v0.0.2
candidate. It is platform-only and changes no application source: build
provenance and identity, `core/pocketpaths`, a deadline on the shell's status
poll, a clean shell stop, a confirming init-script stop, and an exclusive lock
on the SPI device. Host suite and both cross-builds are green; no image has
been built. Because it contains everything v0.0.2 has, the open question for
the owner is whether the first flashed card is the v0.0.2 candidate as
planned in step 1 below, or the v0.0.3 image instead; the bench sheet for
either is the v0.0.2 operator checklist plus the M1-M9 lines from the v0.0.3
implementation report.

Work proceeds in this order:

1. **v0.0.2 hardware validation.** Candidate image built from `349a9b0`
   (`docs/hardware/V0.0.2_BUILD_REPORT.md`), operator sheet
   `docs/hardware/V0.0.2_OPERATOR_CHECKLIST.md`. Not yet flashed. The
   golden v0.0.1 image stays the fallback.
2. **Merge the integration line to master** once the sheet passes.
3. **PocketTimber D3.** PocketTimber (block-tower game,
   `docs/apps/POCKETTIMBER.md`) is software-complete at `11878ca` on
   `pockettimber-engine`, frozen until hardware validation. That branch
   predates every v0.0.2 fix, so it is rebased onto the integration line
   after step 2 and only then built and validated on the K230: launch,
   play, completed run, collapse, summit, BEST updates, record file
   created and surviving app restart, reboot and a full power cycle,
   corrupt record not stopping the game, storage path and permissions,
   no clipping or font problems on the physical 528 x 700 viewport,
   repeated runs stable.
4. **Freeze PocketTimber v1 and merge it.**
5. **Common state facility.** Fleet, Radar and Timber each carry their own
   copy of the same storage pattern (`/var/lib/pocketos/<app>/`,
   `$POCKETOS_STATE_DIR` override, directory creation, atomic
   temp+fsync+rename, replacement, error reporting). v0.0.3 landed the
   prerequisite for the path half of that: `core/pocketpaths` owns the four
   roots, adopted by the platform only, with the app stores left untouched
   on purpose. What remains is the atomic-write and byte-cursor half, the
   store conversions, and the data-partition move in
   `docs/STORAGE_PLAN_v0.0.3.md`, which belong in one release so the app
   stores are touched once. Bench fact: `/var/lib/pocketos` is writable and
   persists across reboots on unit A (VERIFIED 2026-09-07); a full power
   cycle is checked in step 3.

## Phase 2: PocketUI design system

Design System v0.1 is approved (`docs/design/POCKETOS-DS-v0.1.md`).
Implementation steps 1 to 4 are done on the simulator (theme engine with
five themes and three modes, converted fonts, shared role styles with live
switching, persistence and fallback). Steps 5 to 10 (components, screens,
motion, contrast gate) await approval; hardware items H1 to H5 await boards.
Design review happens on simulator screenshots in `docs/design/shots/`.

**Amendment A (DS §17) approved 2026-09-10** — text field, focus model,
touch keyboard and dialog — closing caveat C8, which had left all four
undesigned. It is implementation step 12, and it is the design gate for
v0.0.8 M3 (the logical key layer and the text field) and M4 (the keyboard).
It carries one approved DS-level deviation, DEV-1: 52 px wide keyboard keys
against the 64 px minimum, on stated conditions and for keyboard keys only.
New open caveat C9: where Norwegian and other Latin-1 letters live on the
keyboard. That one needs the product owner.

## Phase 3: first strong application

RIFT was the planned first application; the owner deprioritised it on
2026-09-04. The slot is open. Candidates: RadioLab (link measurements,
airtime, CAD scans) because radiod already provides the data.

## Phase 4: PocketLink

Companion-device protocol over UART/USB/BLE with capability discovery. The
LILYGO nRF52840 and nRF9151 base boards are the first candidates; their AT
protocols are documented in the vendor repositories.

## Later

PocketAI (K230 NPU via an `ai.*` service), PocketNet, image transport over
LoRa, updates with rollback, out-of-process apps, security-sensitive Labs
projects.
