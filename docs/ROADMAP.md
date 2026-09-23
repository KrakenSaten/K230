# Doors roadmap

Product owner sets priorities. Dates are absolute; "done" means built and
tested on the stated platform.

Doors was previously known as PocketOS through v0.0.9. Entries written before
the rename keep the old name.

## Phase 1: Core v0.1 (current)

Goal: a Doors image that boots on the T-Display K230, shows the shell,
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
| netd: Ethernet, Wi-Fi (wpa_supplicant), BLE status | Wi-Fi done on `feature/post-v0.0.9-foundations` (wifi.*, `pos wifi`, credential store under ADR-003, accepted 2026-09-13), host-tested against a fake wpa_supplicant and validated on unit A 2026-09-13. Ethernet stays with the vendor ifupdown; BLE not started |
| Settings app (network, display, system) | Wi-Fi, brightness and appearance done on the same branch, host-tested and validated on unit A 2026-09-13 |
| Shell System Status screen (vitals, storage, network, services, radio, identity, restart, power off) | done (v0.0.7), validated on unit A from `b9203c8`, with the dialog hierarchy and the radio chip corrected and re-verified from `dbba4a0` (docs/hardware/V0.0.7_SYSTEM_STATUS_SMOKE.md) |
| Reboot/shutdown, hardware info app | `system.reboot` and `system.poweroff` done (v0.0.7 block 2c); both validated on unit A, reboot from `db529fb` and poweroff operator-attended from `fdc795f` (docs/hardware/V0.0.7_BLOCK2C_SMOKE.md). Hardware info app not started |
| Basic updater (image on SD, no rollback) | not started |

v0.0.7 is code-complete, validated on unit A across four bench sheets, and
validated as a release image: built at `4ab5a55`, flashed and cold-booted, PASS
on all nine steps of the gate (docs/hardware/V0.0.7_RELEASE_SMOKE.md, gate in
V0.0.7_PRE_RELEASE_CHECKPOINT.md). Merged to master and tagged `v0.0.7`
(`6561b50`).

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
Caveat C9 — where Norwegian and other Latin-1 letters live on the keyboard —
was opened with the amendment and **closed by the product owner on
2026-09-10**: æ, ø and å go on the symbol layer, the alpha layout and the
DEV-1 geometry are left alone, and long-press accent popups stay out of
scope.

**M3 to M5 are implemented and validated on hardware.** M3
(`ui/pocketui/pos_input.*`, `pocketui_text_field`) is the one logical key
stream, one focus group and the text field; M4 (`ui/pocketui/pos_keyboard.*`)
is the touch keyboard, a source of that stream and nothing else; M5 is
PocketNotes (`apps/notes/`) plus the shell taking ownership of the single
keyboard instance. Unit A passed nineteen operator checks on 2026-09-10 at
build `0b16f0e` (`docs/hardware/POCKETNOTES_SMOKE_2026-09-10.md`), including
æ ø å from the symbol layer, autosave, and a note surviving a real power cut.
DEV-1 holds on the panel with a qualification: 52 px keys mis-key at roughly
one character in ten, correctably.

**PocketClock** followed (`apps/clock/`, `7e3bfd9` and `a7f1d92`): the time,
alarms, a stopwatch and a timer, with the alarms run by the shell and rung
through the one full-panel system alert that **DS Amendment B (§18)** made
normative on 2026-09-11 (`1750076`). A cold review of that tree found six P1
defects across Notes, Clock and the release build; all six were fixed in
`03851f5`, which was frozen as the release candidate.

**v0.0.8 M6 is done.** The release image was built from `03851f5` in one
attempt, flashed, and passed its acceptance on unit A on 2026-09-11
(`docs/hardware/V0.0.8_RELEASE_SMOKE.md`): PocketClock on hardware for the
first time, the system alert over Clock, the launcher, Timber and the Notes
editor, 21 notes with the oldest reachable, a near-limit note opening in
under 4 s, and the Clock and Notes stores byte-identical across a restart
and a power cycle. The `v0.0.8` tag is on the commit that adds that sheet.
Physical keyboard support is not in this release; it continues on its own.

**v0.0.9 is open.** PocketCalendar is its first feature: a month view, Monday
first, with a Today button and a selected day, and no scheduling of any kind
(`docs/apps/POCKETCALENDAR.md`). It takes the date from the shell rather than
reading a clock, and it is explicit when the board does not know the date -
which on hardware with no RTC is the state it boots into. Host-tested and
cross-built; not yet run on a board.

**Post-v0.0.9 foundations** (`feature/post-v0.0.9-foundations`, 2026-09-12,
host-tested, validated on unit A 2026-09-13 with audio at the silent stages
only, not merged): netd with Wi-Fi
(`docs/api/network.md`, `docs/decisions/ADR-003-wifi-credentials.md`,
`docs/hardware/WIFI_2026-09-12.md`), display brightness through the shell
(`docs/hardware/DISPLAY_BRIGHTNESS.md`), a Settings app (`docs/apps/SETTINGS.md`),
PocketCalculator (`docs/apps/POCKETCALCULATOR.md`), and an audio and
microphone feasibility map with a staged bench plan
(`docs/hardware/AUDIO_FEASIBILITY_2026-09-12.md`). VERSION is unchanged.

**Audio milestone: Wave** (`feature/audio-ggwave`, 2026-09-13, not merged):
the pocketaudio layer, the `pos-wave` helper and the Wave app, sending and
receiving short text as ggwave sound (`docs/apps/WAVE.md`, ADR-004 accepted
for this milestone as a narrow exception to ADR-002). Both paths validated on
unit A: a phone's message decoded by Wave, and SEND heard and decoded by a
phone from the Wave UI (`docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md`).
VERSION is unchanged.

**Third-party notices** (`feature/third-party-notices`, 2026-09-13): the image
carries `/usr/share/pocketos/THIRD_PARTY_NOTICES.txt` for everything
third-party compiled into PocketOS binaries and for LVGL, verified against the
pinned upstream sources at packaging. PocketOS's own licence stays undecided
and external redistribution is not authorised until the owner chooses one
(`docs/LICENSING.md`).

**Doors** (`rebrand/doors-1-visible`, 2026-09-15, not merged): the product is
renamed from PocketOS to Doors in stages, under
`docs/decisions/ADR-005-product-name-doors.md`. Phase 1 changes only what a
user reads - the status bar wordmark, the System identity row and its
dialogs, crash reports and the current documentation - and no path, command,
service, environment variable or C identifier. v0.0.10 is the first Doors
release. VERSION is unchanged.

**v0.0.10, the first Doors release** (2026-09-16, `release/v0.0.10`): ADR-005
Phases 1 to 3, the Doors splash and mark, the app icons, the rounded-corner
safe area and system-owned rotation, together with everything above merged
since v0.0.9 (Wi-Fi, brightness, Settings, Calculator, Wave, the third-party
notices). Release notes: `docs/releases/v0.0.10.md`; the release candidate
`9f9c802`, its image and the unit A gate (PASS, with a known boot-splash
limitation): `docs/hardware/V0.0.10_RELEASE_SMOKE.md`. Phase 4 of ADR-005 is
not started and not approved.

**v0.0.11** (released 2026-09-23, tag `v0.0.11`, image built at `9a4afeb`): everything merged since the `v0.0.10` tag - landscape layouts for
every app that is having one (below), the MeshCore stack (the portable
protocol core, radiod's asynchronous transmit and lease, meshcored as a
supervised per-unit opt-in service, and RIFT, the mesh client, with channels
and 256 retained nodes), the DOORS environment (DS §31: lock screen, grouped
launcher, Controls), the landscape status chrome (DS §30, accepted), the
Doors app theme (DS §32, accepted), and the pre-release fixes from the
2026-09-23 cold review. Release notes: `docs/releases/v0.0.11.md`; the RC1
image and its unit A gate (PASS): `docs/hardware/V0.0.11_RELEASE_SMOKE.md`.

### Landscape app adaptation (after v0.0.10)

System rotation works (DS §21). When this was written most app screens were
still portrait layouts shown in the landscape body - fixed widths scrolled
vertically (docs/KNOWN_ISSUES.md; confirmed by the product owner on unit A
during the v0.0.10 gate). Each app gets a responsive landscape layout **on its
own**, as its own change with its own DS amendment where §21.3 asks for one,
its own tests in both orientations, and a unit A check - not one redesign of
every app at once. The portrait layouts stay as they are. Order: the product
owner's. §22.3 records the pattern Calculator set for the apps after it, and
each amendment since adds what its app learned.

Accepted, in the order they were done:

| App | DS | Accepted | Merged |
| --- | --- | --- | --- |
| Calculator | §22, Amendment F | 2026-09-16 | `0d46e34` |
| Notes | §23, Amendment G | 2026-09-17 | `228bf22` |
| Settings | §24, Amendment H | 2026-09-17 | `f5d81ec` |
| System | §25, Amendment I | 2026-09-17 | `dcf906d` |
| Clock | §26, Amendment J | 2026-09-17 | `2bdf279` |
| Calendar | §27, Amendment K | 2026-09-17 | `1608c3f` |
| Fleet | §28, Amendment L | 2026-09-18 | `f577fff` |
| Radar | §29, Amendment M | 2026-09-18 | `b9c4a47` |

Fleet went in first, as §28: the two branches were developed independently from
`7d0d1ea` and numbered so they would not collide, and Radar was rebased onto
the master Fleet made. **Every app that is having a landscape layout now has
one**; what is below is what is deliberately not getting one.

Still to do:

- **Timber stays portrait**, on purpose: the tower is built upwards and the
  vertical playing field is intrinsic to the game, so there is no landscape
  layout to give it. Not a gap.
- **Radio landscape is not planned.** The Radio app is expected to be
  replaced, and laying out a screen that is going to be thrown away would be
  work spent twice. To be reconsidered if that changes.
- **Wave** has no fixed-width content, so it stretches across the landscape
  body rather than being clipped or scrolled. Whether it is worth an amendment
  of its own is the product owner's call, and no work is scheduled.

### Vendor U-Boot display bring-up investigation (after v0.0.10)

The boot splash is intermittently black on cold boots and warm reboots, while
the image, the splash file and the boot into Doors are fine
(docs/KNOWN_ISSUES.md; docs/hardware/V0.0.10_RELEASE_SMOKE.md, "Boot splash:
known vendor limitation"). Not started before v0.0.10 (product owner). Scope
when it is taken up: the vendor U-Boot panel power, reset and DSI bring-up in
the LILYGO overlay (`k230_logo.c`, `st7701.c`, `display_logo.c`), to find the
root cause and a fix that is deterministic rather than a retry. Known going in:
the console output of a black boot is identical to a lit one, a single
`PHY_STATUS` read does not tell them apart, and a second `k230_logo` from
`bootcmd` did not make cold boots reliable (`236a142`, branch
`experiment/v0.0.10-splash-bootcmd-retry`, rejected). Any change is a vendor
bootloader change, validated on many cold and warm boots with the panel
watched.

### Settings fundamentals before v0.1.0

Settings holds only what is backed by working functionality. Reviewed
2026-09-12:

| Setting | Status | Recommendation |
| --- | --- | --- |
| Wi-Fi | in Settings | validated on unit A 2026-09-13 (WPA2, reboot rejoin, forget, auth failure); it is the v0.1 exit criterion |
| Display brightness | in Settings | validated on unit A 2026-09-13; the 10 % floor stays (readable in Normal, marginal in Night) |
| Theme and display mode | in Settings (Appearance) | already worked over `shell.theme`; now reachable on the device |
| Time zone | **missing, user-visible**: Clock and the status bar show UTC | before v0.1.0: needs a decision on the zone data in the image (none today) and a `TZ`/`/etc/localtime` owner; then a Settings row |
| Date and time (manual) | not added | not before v0.1.0: NTP sets the clock when there is a network, there is no RTC, and a manual clock that NTP later overrides needs a design |
| Reduced motion | not added | small: the key exists, but carets read it only at shell start; a row needs either a restart note or live re-application |
| Radio (region, power, profile) | not added | not until radiod persists its profile (a restart returns to the start-up defaults); the Radio app remains the place |
| System information, reboot, power off | in System | stay there; Settings links nothing it would duplicate |
| Network information (Ethernet, addresses) | in System; Wi-Fi address in Settings | enough for v0.1 |
| Keyboard (backlight, layout) | not added | nothing to back it yet (no backlight control, one layout) |
| Sound | not added | the audio paths are validated (Wave milestone), but nothing needs a user setting yet: Wave's level is fixed under a -12 dBFS ceiling |

## Phase 3: first strong application

RIFT was the planned first application; the owner deprioritised it on
2026-09-04 and it came back as the MeshCore client on 2026-09-19. It is on
master with ACTIVITY, NODES and COMMS (direct messages and channels), over
meshcored (docs/apps/RIFT.md, docs/services/MESHCORED.md); NET is not in
this build. Open follow-ups are in docs/KNOWN_ISSUES.md (meshcored and RIFT).
RadioLab (link measurements, airtime, CAD scans) remains a candidate.

## Phase 4: PocketLink

Companion-device protocol over UART/USB/BLE with capability discovery. The
LILYGO nRF52840 and nRF9151 base boards are the first candidates; their AT
protocols are documented in the vendor repositories.

## Later

PocketAI (K230 NPU via an `ai.*` service), PocketNet, image transport over
LoRa, updates with rollback, out-of-process apps, security-sensitive Labs
projects.
