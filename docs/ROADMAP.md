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
