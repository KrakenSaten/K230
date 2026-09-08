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
| `pos-hwcheck` first-boot inventory | done, untested on hardware |
| pocketipc v0 | done, tested |
| radiod with mock backend, region guard, airtime stats | done, tested |
| radiod sx1262 backend (own HAL + RadioLib 7.7.1) | written, compiles for riscv64, untested |
| Shell: status bar, launcher, app host, SDL simulator, `shell.*` IPC | done on PC |
| Shell DRM/evdev backend | written, untested |
| Shell in the Buildroot image, replacing the vendor launcher | installed (S90 disabled by default); replacement not started |
| Logging library, crash reports, `pos logs` | done, tested on PC |
| Service respawn with backoff, crash-loop detection | done (`pos-supervise`), tested on PC |
| netd: Ethernet, Wi-Fi (wpa_supplicant), BLE status | not started |
| Settings app (network, display, system) | not started |
| Reboot/shutdown, hardware info app | not started |
| Basic updater (image on SD, no rollback) | not started |

Exit criteria for v0.1: boots from SD on both units, shell usable by touch,
`pos hwcheck` report attached to docs/hardware, radiod sends and receives a
packet between the two units, Wi-Fi joins a network from the Settings app.

## Status 2026-09-08

One development stream from here on; the earlier idea of a second
parallel track is withdrawn. Work proceeds in this order:

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
   temp+fsync+rename, replacement, error reporting). Evaluate extracting
   it into a shared PocketOS facility, together with the data-partition
   move in `docs/STORAGE_PLAN_v0.0.3.md`, so the path changes in one
   place. Bench fact: `/var/lib/pocketos` is writable and persists across
   reboots on unit A (VERIFIED 2026-09-07); a full power cycle is checked
   in step 3.

## Phase 2: PocketUI design system

Design System v0.1 is approved (`docs/design/POCKETOS-DS-v0.1.md`).
Implementation steps 1 to 4 are done on the simulator (theme engine with
five themes and three modes, converted fonts, shared role styles with live
switching, persistence and fallback). Steps 5 to 10 (components, screens,
motion, contrast gate) await approval; hardware items H1 to H5 await boards.
Design review happens on simulator screenshots in `docs/design/shots/`.

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
