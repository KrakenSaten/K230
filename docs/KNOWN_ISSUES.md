# Known issues and open questions

Updated 2026-10-09 for the v0.3.6 release candidate (docs/releases/v0.3.6.md;
not published) on top of v0.3.5 (tag `v0.3.5`; docs/releases/v0.3.5.md; the
v0.3.0 summary in docs/releases/v0.3.0.md still applies where not fixed, the
per-app details are in docs/apps/ and the gate sheets). Move items to git
history when resolved.

- **Vision's R0 detector is a beta and misses things** (v0.3.6,
  docs/apps/VISION.md "The model"). It can miss vehicles, including cars in
  ordinary street scenes; on its held-out test set it found none of the very
  small cars and about one in six small ones, and confused cars and trucks
  (docs/vision/R0_TRAFFIC6_RESULTS.md on `research/yolox-traffic-training`).
  It detects only car, truck, bus, motorcycle, bicycle and person. Its
  distribution terms are still the owner's to decide (MODEL_LICENSES.md).
- **DeskBuddy does not use R0** (v0.3.6): its provider opens the helper's
  default model, `yolov8n.kmodel`, which no image ships, so without face
  models it reports NO VISION YET as on 0.3.5 (docs/apps/DESKBUDDY.md).

- **RIFT's first use from Controls** (v0.3.5 notes, "RIFT on a fresh
  card") passed on unit B from a fresh card (2026-10-07), the success path
  only: no failure path (radiod or meshcored not coming back) was exercised
  on hardware. A unit on the SX1262 with meshcored deliberately disabled is offered the
  setup on a tap; `doors radio on|off` switches its radio alone.
- **Poker loses the hand on a rotation** (docs/apps/POKER.md). Its session
  is kept in memory only, and a rotation re-executes the shell: attaching
  or detaching the keyboard, or opening Poker from landscape, deals a fresh
  table (observed on unit B, 2026-10-08). Back and reopen within the same
  shell resumes the hand. The wanted behaviour, keeping the table across
  rotation and keyboard attach/detach, is a separate follow-up
  (docs/ROADMAP.md, games).

Closed by 0.0.3, listed here only because the bench sheets still cite them:
B4 (the shell's `printf` diagnostics never reached a log; they go through
pocketlog now), the shell leaving a stale `shell.sock` after `stop`, init
`stop` returning before anything had gone, `/dev/spidev0.0` being shareable
(F14/B3), and logs and crash reports that could not name the build they came
from. Closed by 0.0.4, found on unit A with 0.0.3: the Radio app's tick
blocking the panel while radiod was stopped (M5), the supervisor leaving
before its child so that `stop` reported forced (M6), and the hwcheck probe
releasing RST instead of driving it high (M7). Closed by 0.0.5, found on
unit A with 0.0.4: the shell's reconnect blocking in `connect()` once
radiod's listen backlog was full of its own abandoned connections (M5,
second cause).

Closed as understood by 0.0.6 (unit A, 2026-09-09): the `24 b4` register
read of the v0.0.4 and v0.0.5 retests is not a framing, decoding or
SPI-transport defect. The probe's ReadRegister frame is byte-for-byte what
RadioLib sends, spi-pipe 1.0.2 already issues one `SPI_IOC_MESSAGE(1)` per
block, and on the bench pos-spixfer at 4 MHz (radiod's transaction),
spi-pipe at 1 MHz and spi-pipe at 4 MHz all read `aa aa aa aa 14 24` on the
same chip state, with `pos-hwcheck --lora` VERIFIED, exit 0
(docs/hardware/V0.0.6_M7_BENCH.md). The failing runs differed in the
chip's status byte (`a2`, command status 1, versus `aa`, command status 5,
on every run that read `14 24`), so the value depends on the SX1262's
state after reset. Not a radiod defect, not a runtime blocker, and not
investigated further unless it recurs: the probe now names it
chip-state dependent and dumps both transports' windows when it does.

## Vendor launcher removed (chore/remove-vendor-launcher)

The LILYGO launcher (`k230_phone_ui`, its init script
`S99zz_k230_phone_ui`, its helpers under `/root/app/k230_phone_ui` and the
sample media it installed in `/root/music`, `/root/videos` and
`/root/notification`, about 100 MB) is no longer built into the image.
Doors had run with it disabled since the panel handover, so no Doors
function changes. What the image loses, and what is left open:

- **Charger set-up, gauge capacity, low-battery shutdown.** The launcher's
  own start-up turned off the BQ25896 I2C watchdog, set a 704 mA charge
  current and a 4288 mV charge voltage, wrote a 6000 mAh design capacity
  into the BQ27220 once, and powered the unit off at 3600 mV on battery
  (DOCUMENTED from its source, ui_hardware.c). None of that ran on Doors
  before this change either: the charger runs on its own power-on defaults
  and nothing reads the gauge (see "No battery reading"). Doors owning
  charger configuration, battery percentage and low-battery shutdown is
  open work that needs the bus-ownership decision first; whether the
  vendor's 704 mA / 4288 mV suit the fitted cell is unknown (ASSUMED
  neither way).
- **Base-board external speaker route** (I2S route plus amplifier GPIO):
  only the launcher drove it; Doors plays through the board codec.
- **On-device recovery to the vendor UI.** `ENABLE=1` in
  `/etc/default/k230_phone_ui` no longer brings anything back. Recovery is
  the serial console, SSH and the vendor's own SD card
  (docs/hardware/FIRST_BOOT.md, "Recovery"). The launcher's bench pages
  (I2C scan, charger page, LED test) go with it.
- **Units on older images** keep their `/etc/default/k230_phone_ui`. S90
  honours `ENABLE=1` only while the launcher's init script is installed and
  ignores it otherwise, saying so, so a leftover switch cannot leave the
  panel dark. The guards in S90, `ui/shell/kbd_bus_k230.c` and `pos-hwcheck`
  against a running launcher stay for those units.
- **Shared SDK tree.** `apply_to_sdk.sh` removes the launcher package and
  its menu line from the SDK, so a vendor reference build
  (`k230_canmv_t_display_rm69a10_defconfig`) from the same tree needs the
  vendor's `install_to_sdk.sh` run again first.
- Buildroot: `BR2_PACKAGE_POCKETOS` now selects libdrm, libevdev and jpeg
  itself. libevdev had no other selector; with it the image's `.config`
  differs from the v0.3.x one only by `BR2_PACKAGE_K230_PHONE_UI`
  (kconfig comparison, 2026-10-03).

## Settings and System cleanup (feat/settings-system-cleanup, DS §52)

- **No system sleep.** The kernel lists `freeze mem` in `/sys/power/state`
  and `s2idle [deep]` in `mem_sleep` (unit B, 2026-10-02), with four wakeup
  sources that are not identified. Which of touch, the keyboard base, the
  buttons or the radio can wake the board from either state is unverified;
  there is no RTC (`/dev/rtc*` absent), so no wake on a timer; and a board
  that does not wake needs USB power out for 30 seconds. Power & Sleep
  therefore offers screen off and lock only and says sleep is not available.
  Establishing wake sources is hardware work of its own.
- **Screen off is a black cover, not a panel power state.** The backlight
  device's level 0 (DCS 0x51 00) and `bl_power` are DOCUMENTED to blank by
  brightness only; what they show on this AMOLED is UNKNOWN
  (docs/hardware/DISPLAY_BRIGHTNESS.md), so neither is used. Black pixels
  are off pixels on the AMOLED; the panel itself stays powered and the shell
  keeps drawing under the cover (LVGL skips what the cover hides).
- **The time zone is the shell's.** It is applied in the shell process
  (TZ + `tzset()`) and inherited by what the shell starts afterwards. Services
  started by init, SSH sessions and `date` keep UTC; their logs already say
  UTC. A helper the shell started before a change keeps the old zone until it
  is started again. The image has no zone database, so the zones are a fixed
  list of POSIX rules as in force in 2026 (`ui/shell/tz_zones.c`).
- **No 12/24-hour setting.** Every clock in Doors is 24-hour and there is no
  setting for it to move; none was added.
- **The debug overlay is not in `shell.screenshot`.** It is on LVGL's top
  layer, which `lv_snapshot_take(lv_screen_active())` does not include; the
  panel shows it (an F7/kmsgrab capture does too).
- **Keep-awake is a list in the shell.** Video, Camera, Vision and DeskBuddy
  hold the screen and the lock off by id (`awake_apps` in `ui/shell/shell.c`);
  a new app that plays or watches has to be added there.

## Open on feat/device-controls-diagnostics (not merged)

Radio on/off with SX1262 off by default, the antenna question, system volume
and mute, Bluetooth and battery status, and System > Diagnostics. **Unit A
gate PASS on build `09be665`** (docs/hardware/DEVICE_CONTROLS_GATE.md),
including the owner's listening check of the volume steps; DS §31.5 accepted.

- **Unit A's radio goes off on deploy.** v0.0.12 stored no on/off choice, so
  the first start of this radiod finds none and, on the sx1262 backend,
  starts off (docs/api/radio.md, "Radio on and off"). meshcored stays
  connected, `degraded` "the radio is switched off", with its identity and
  channels untouched. Switch it on once in Controls or with `doors radio on`.
- **Bluetooth cannot be switched: there is nothing to switch.** Unit A has no
  HCI device (VERIFIED 2026-09-07), the kernel has RFKILL unset, no Doors
  service owns Bluetooth, and the nRF52840 base-board BLE bridge (UART1, AT
  protocol) is unverified. Controls and Diagnostics say "Not available" /
  "No controller" from sysd's `bluetooth.controllers`. A real toggle needs a
  controller and an owner (ADR-002 names netd): a product decision, not
  in this branch.
- **No battery reading on unit A.** The BQ27220 gauge and BQ25896 charger on
  the keyboard base have no kernel driver bound and the power_supply class is
  empty (VERIFIED); the keyboard driver bit-bangs that bus, and adding charger
  or gauge support needs the bus ownership decided first
  (KEYBOARD_DRIVER_DESIGN_2026-09-12.md). Battery shows "External power";
  the percentage path (the driver's own `capacity`, never computed from a
  voltage) is host-tested only.
- **Volume is a digital gain.** The speaker route has no mixer volume
  (AUDIO_HARDWARE_MAP §7), so 100 % is the validated -12 dBFS level and the
  control only attenuates (to -27 dB at 10 %). It applies to what Wave plays;
  the 3.5 mm jack's `PCM Playback Volume` is not driven (nothing plays to the
  jack). A bench `pos-wave send` without `--volume-percent` still plays at
  100 %.
- **The antenna question is glass over glass:** the tiles behind show through
  its text (more in portrait, where two slider tracks cross it). Readable on
  the panel and accepted as it is (DS §31.5).
- **Controls glyphs for Bluetooth and Battery are borrowed** (network and
  power) until the DS draws their own; the landscape Controls moves Lock and
  Power into the header row to fit three tile rows (DS §31.5, accepted).

## Vision (merged to master 2026-09-28, in v0.2.0)

The Vision prototype (docs/apps/VISION.md): DETECT, TRACK and COUNT on the
KPU with the vendor's YOLOv8n kmodel. DS §38 is still PROPOSED. **Hardware gate run on unit B
(2026-09-28, docs/hardware/VISION_GATE.md): everything passes but repeated
open/close, which reaches the camera lock-up; the owner accepted that as the
known vendor fault and sent the prototype to review with this caveat.**
Unit A was not available.

- **Vision can freeze the whole unit (ACCEPTED by the owner as the known
  camera lock-up, 2026-09-28).** Silent on the console, a normal
  `vvcam_isp_release` as the last line, a power cycle to recover - the same
  signature as Camera's entry under "Hardware and BSP", but reached far
  sooner: on unit B within about 5-20 stops of a Vision stream, where
  Camera ran 40 open/close cycles clean on the same unit and boot. In
  isolation the KPU alone, the BG3P stream alone, and both together run to
  completion never froze (30 runs each); streams stopped part-way froze it
  2 times in 24. Blocking signals in the helper (`543dc0e`) did not cure it.
  Finding the cause needs kernel-side debugging or the vendor.

- **No detector model from 0.3.5**: the SDK's `yolov8n.kmodel` is
  compiled from Ultralytics weights (AGPL-3.0) and was in the images 0.2.1
  to 0.3.0 for internal use only (docs/LICENSING.md item 10). 0.3.5 does not
  ship it; DETECT, TRACK and TRAFFIC are off (the app says so) and DeskBuddy
  cannot see unless a face model is installed by hand. A DOORS-trained
  replacement is in preparation.
- **Weak detection in warm, dim light, and counts above the real
  crossings** (v0.2.1 fresh-flash smoke on unit B, docs/releases/v0.2.1.md):
  a person standing in full view was boxed in some frames only, at 35-59 %,
  track ids climbed to #73 in two minutes, and COUNT reached LEFT 16 /
  RIGHT 16 for a walk asked as 3-4 crossings each way. Same code and model
  as the gate that saw 76-83 %; the scene was warmer and dimmer.
- **The ISP's `BG3P` at 640 x 360 on `/dev/video2` works** (VERIFIED on unit
  B), and its planes are R, G, B despite the name (fixed in `99739f5`).
- **False detections on dark clutter**: a coat on a chair drew
  `baseball-glove`, `backpack` and a wide `person` box (35-83 %) in a lit
  room, and a black frame a full-frame `person` (35-50 %). The threshold is
  the vendor's 0.35; a false `person` merging with a real one can add a
  count.
- **The horizontal (ACROSS) line counts a sideways walk** as DOWN or UP
  when the body enters or leaves at the frame edge; the vertical (DOWN) line
  is the one for people walking past.
- **One frame copy per inference (691 KB)** into the runtime's shared pool,
  because a V4L2 MMAP buffer has no physical address the AI2D engine can
  use. A zero-copy path (dma-buf or an mmz-backed V4L2 buffer) is the
  obvious next step once the cost is measured.
- **Inference is synchronous in the helper**: the frame rate through the
  detector is 1 / (copy + AI2D + KPU + decode + track), and the preview
  shares that loop. A second thread for the preview is the change to make
  if the measured rate is too low.

## Open for v0.0.12

What changed since v0.0.11 is Files (DS §33) and the fullscreen apps
(DS §30.8); both have unit A gates. **v0.0.12 RC1 (`a8b1a9f`) was flashed on
unit A and passed its release gate** on 2026-09-24
(docs/hardware/V0.0.12_RELEASE_SMOKE.md, which lists what it did not cover:
RF on the flashed card and RF from the peer). Nothing here is DEVICE VERIFIED unless it
says so. Everything under "Open for v0.0.11" still applies.

- **Keyboard-base hot-plug was done once with the board running** (v0.0.12
  RC1 gate, at the owner's decision): detach and re-attach were each detected,
  the display turned, nothing failed. That is software evidence only. Hot-plug
  is still not an electrically verified operation (below, "Attaching or
  removing the keyboard base"), and the advice stays: mate it with USB power
  removed.
- **Timber has no landscape layout**: in landscape it is its portrait page in
  a wide body and scrolls. Seen on unit A, 2026-09-24. (Wave had the same
  problem - TRANSMIT below the fold - and has had a landscape layout since
  `feat/wave-next`, unit A gate PASS on `f2c22f1`:
  docs/hardware/WAVE_NEXT_GATE.md.)
- **Radar in portrait** keeps the §29 scope, which the panel's width limits;
  under the fullscreen body the page ends about 200 px above the foot.
- **RIFT's landscape ROUTE pane reaches the right edge of the screen** (the
  COMMS details column). Seen in the simulator before the fullscreen change
  and on unit A after it; cosmetic, nothing in it is cut.
- **A landscape app under the 32 px COMPACT bar has its back slab's top
  10 px inside the 50 px top corner band.** The rounded edge there is about
  1 px, and nothing was seen cut on unit A (v0.0.11 RC1 gate, fullscreen
  gate), but the header is not inset for it (DS §30.1).
- **The shipped shell still reads the games' development variables**
  (`POCKETFLEET_SCREEN`, `POCKETRADAR_SCREEN`, `POCKETTIMBER_SCREEN`,
  `POCKETTIMBER_TRACE`, `POCKETTIMBER_PLACEHOLDER`; docs/apps/POCKET*.md), as
  every release before it has. They do nothing unless set in the shell's
  environment, which no init script does. The simulator's test hooks
  (`POCKETOS_TEST_*`) are compiled out of the panel's build (checked in the
  `a30678e` target tree, 2026-09-24).

## Open for v0.0.11

What a v0.0.11 reader most needs to know, gathered here; the detail is in the
sections below. Evidence classes as in AGENTS.md: nothing in this section is
DEVICE VERIFIED unless it says so.

- **v0.0.11 RC1 (`9a4afeb`) was flashed on unit A and passed its short
  release gate** on 2026-09-23 (docs/hardware/V0.0.11_RELEASE_SMOKE.md). Not covered there:
  keyboard-base attach and removal while running, service kill and
  recovery, power-off from System, a cold power cycle.
- **Landscape safe corners: decided, 50 px at the top, device confirmation
  owed.** In landscape the two top corner squares are 50 px (the bottom ones
  and every portrait corner stay 30 px), the owner's decision of 2026-09-23
  (DS §21.1). The value comes from unit A's calibration with
  `POCKETOS_SAFE_CORNERS` on 2026-09-21 (45 px failed, 50 px the smallest
  that passed); the built-in default replacing that bench override is
  confirmed on the device by the v0.0.11 release gate.
- **DS §30 (landscape COMPACT status bar) and DS §32 (Doors app theme) are
  ACCEPTED** (owner, 2026-09-23) on the v0.0.11 RC1 unit A gate
  (docs/hardware/V0.0.11_RELEASE_SMOKE.md). That gate covered their §30.7 and §32.8 checklists
  in part; what it did not exercise on the device is listed in the sheet
  (among it: the keyboard up in landscape apps, every app under COMPACT,
  Fleet's full bar, Outdoor, and Ice and back).
- **A MeshCore message's state does not say whether it was transmitted**
  (meshcored section below; docs/api/mesh.md, "Accepted is not transmitted").
- **Notes shows 1970 dates for notes saved before the clock is set** (below,
  under "No RTC").
- **Adverts sent before the clock is set are ignored by peers** (meshcored
  section below).
- **RIFT's message sounds are not yet heard on hardware**: they play through
  pos-record (ADR-010 Amendment 1, accepted); audibility on the speaker is
  the owner's to judge (RIFT section below).


## Hardware and BSP

- **HDMI output gives no usable picture on the vendor kernel (VERIFIED on
  unit A, 2026-09-27, `docs/hardware/HDMI_GATE.md`).** Bridge, hot-plug and
  EDID work; 480p and 720p give no signal, 1080p60 stripes, and the shell is
  offered 2560x1440, which the DSI cannot carry. Three kernel-side causes are
  documented in `docs/hardware/HDMI_KERNEL_FIX.md` (mode filtering, an LT9611
  timing-register bug that hits 720p, the 4-lane D-PHY brought up as the
  2-lane panel's); kernel patches 0070/0071 on `feat/k230-hdmi-out` under
  ADR-011 (Accepted 2026-09-27). Round 3 (owner at the bench) proved those patches on
  the monitor with the DSI's own colour bars at 720p and 1080p. What is left
  is the K230 VO: XRGB8888 planes had their OSD DMA request bits off (a BSP
  constant, documented by the vendor U-Boot code; patch 0072 restores them,
  and with it 1920x1080@60 is clean and stable from `pos-drmtest` and the
  Doors shell, round 4). The VO's 720p "no signal" was the DSI host's line
  time, which must be a whole number of lane-byte clocks (round 5); patch
  0073 pads the timing and round 6 showed 720p and 1080p clean and stable.
  Still open on the branch: 1080p30 and 720p50 untested on a monitor, 480p
  not offered, no hot-plug event to userspace, the HDMI tree drops
  LoRa/touch/uart1. `docs/hardware/HDMI_KERNEL_FIX.md` §9-11.
- **Whole-unit lock-up after repeated camera open/close (VERIFIED on unit A,
  master's binaries, 2026-09-27).** Opening Camera, streaming for about 3 s
  and leaving it, over and over, eventually freezes unit A completely: the
  serial console stops answering and the unit drops off the network. Only a
  power cycle recovers it. It took 21, 27 and 40 opens in three runs.
  - The console, at loglevel 8, prints nothing at the moment of the freeze:
    no oops, panic, RCU stall or watchdog line. Its last output is the normal
    `vvcam_isp_release` of the last close.
  - The freeze came a few seconds after that release, while the unit was idle
    or starting the next action, never during streaming.
  - It happens with master's own `doors-shell` (`f2c22f1`) and v0.1.0's
    `pos-camera`, with no gallery involved.
  - Suspected cause (not proven): the vendor vvcam/ISP stack
    (`isp_media_server` plus the out-of-tree vvcam modules). This is the same
    class as the earlier STREAMOFF+STREAMON lock-up in
    `docs/hardware/CAMERA_GATE.md`.
  - Anything that opens and closes the camera often, such as the gallery on
    `feat/camera-gallery`, reaches it sooner.
  - Evidence and reproduction: `docs/hardware/CAMERA_GALLERY_GATE.md`, �6.
  - Candidate mitigations, owner's call:
    - keep the camera open across a gallery visit (amends ADR-006's per-visit
      open);
    - a minimum interval between a release and the next open;
    - a vendor fix.
- RAM size unknown: wiki says 1 GB, Linux DTS declares 512 MB, U-Boot fixes
  the memory node at boot. Verify with `pos-hwcheck` on first boot.
- LoRa module variant on our units (SX1262 vs LR2021) unverified.
- SX1262 module parameters used by the backend (TCXO 3.3 V on DIO3, DC-DC
  regulator, no DIO2 RF switch) are taken from the vendor launcher's
  configuration and are unverified for our module variant.
- SX1262 receive re-entry after transmit, CAD and packet read now reports
  failure through `is_receiving`/`resume_rx` (radiod state `error` with
  once-per-second recovery). The logic is tested on the mock backend only;
  whether `startReceive()` ever fails on real hardware, and whether the
  recovery path clears it, is ASSUMED until K230 testing.
- Schematic has alternate LoRa nets (IO4_IRQ, IO3_TCXO_EN) with 0R/NC
  options. BSP uses GPIO20 as DIO1. Assumed populated that way.
- The PocketOS shell links the vendor-built liblvgl from the Buildroot
  package, which carries LILYGO's DRM patches (0002 plane rotation, 0004
  staging scanout buffer), not a stock LVGL. The shell runs with
  `K230_LVGL_DRM_STAGING=1` set by S90doors-shell, as the vendor launcher
  does. It calls the rotation API only when the orientation is landscape
  (rotation 270, feature/doors-display-geometry); portrait stays rotation 0.
  Landscape on the panel, and touch following it, are VERIFIED on unit A
  (2026-09-16, docs/hardware/DOORS_DISPLAY_GEOMETRY_GATE.md: PASS).
- The display cannot be turned while it is open: the vendor DRM path sizes its
  framebuffers for the rotation when the display is opened, and the vendor
  launcher restarts its own process to switch between portrait and landscape.
  So Doors opens the display again - it re-executes itself in place, keeping
  its pid, and comes back on the launcher; Settings says so before it happens.
  The panel is dark for the length of a shell start (about a second on unit A),
  and an app that was open is closed the ordinary way.
- Keyboard presence is the TCA8418 answering its probe on the bit-banged bus
  (`ui/shell/shell_kbd.c`), which is what the vendor launcher calls the base
  being detected. Nothing on this board reports the base mechanically, so a
  keyboard whose controller cannot be reached reads as absent, and a bus that
  cannot be claimed at all reads as unknown. The orientation Doors opens with
  is VERIFIED on unit A for every case of the policy (four power-off boots,
  DOORS_DISPLAY_GEOMETRY_GATE.md).
- **Attaching or removing the keyboard base while the board is powered is not
  a verified hardware operation.** The software path is verified (simulator,
  and on unit A with the controller held in reset), but the connector is a
  plain 2x20 header whose 3V3 pin shares the net that feeds the K230's VDDIO
  banks, with no load switch, series element or ESD part on any of its signals,
  no board-detect pin, no mating specification, no base-board schematic and no
  vendor statement about hot-plug either way (KEYBOARD_BRINGUP §8, which also
  names the two measurements that would settle it). Mate and unmate with the
  board powered down and USB power removed.
- The rounded corners' extent has no datasheet; the corner squares are what
  unit A measured (`platform.h`, DS §21.1, owner decision 2026-09-23): 30 px
  at every corner in portrait, the vendor launcher's status-bar side inset;
  in landscape 50 px at the two top corners (the native left ones) and 30 px
  at the bottom. Landscape's top needed 50 there (50 PASS, 45 FAIL,
  2026-09-21, with `POCKETOS_SAFE_CORNERS`). `POCKETOS_SAFE_CORNERS` in
  `/etc/default/doors-shell` still overrides all four in both orientations;
  the old bench line `50,30,30,50` also puts 50 px at the portrait left
  corners, which the default does not, so remove it from a unit to run the
  default. The status bar uses the safe area, and so does every app that has
  been given a landscape layout - Calculator, Notes, Settings, System, Clock,
  Calendar, Fleet and Radar - through the one rule pos_display_rect_insets()
  (DS §22.2, §23.4). The touch keyboard's bottom row (DEV-1 fixed 52 px keys,
  6 px sheet padding) and the full-screen alert's card corners are not inset
  for the corners; widening them changes approved geometry and is left for a
  design decision if unit A shows them cut.
- Landscape is laid out for the status bar, the launcher and, each under its
  own accepted amendment, Calculator (DS §22), Notes (§23), Settings (§24),
  System (§25), Clock (§26), Calendar (§27), Fleet (§28) and Radar (§29) - which
  is all of them that are having one. The app bodies without one keep their
  portrait-derived fixed widths - Timber 528 - inside a 1232 px wide, 440 px
  high body and scroll vertically; Radio and Wave have no fixed width, so they
  stretch across the landscape body without being laid out for it. The touch
  keyboard stays 568 px wide at the bottom centre. Timber stays portrait on
  purpose (the vertical tower is the game);
  Radio is not planned, because the app is expected to be replaced
  (docs/ROADMAP_HISTORY.md, "Landscape app adaptation").
- The target lv_conf.h is the vendor package's, not ui/shell/lv_conf.defaults:
  LV_USE_FLOAT 1, LV_USE_SNAPSHOT 0, ThorVG/FreeType/FFmpeg compiled in,
  LVGL asserts abort the process (which pocketlog turns into a crash report).
  Consequence: `pos shell screenshot` and `--screenshot` do not work on the
  device (they need LV_USE_SNAPSHOT). Decision H2 (product owner,
  2026-09-06): option B, photographs for the first hardware validation; the
  vendor LVGL configuration stays unchanged before first boot. Device-side
  screenshot support is a post-bring-up improvement.
- UART3 is wired both to the CH342K USB-UART (channel 1) and, per BSP, to the
  optional nRF9151 base board. Potential conflict if both are used.
- **The boot splash is intermittently black, on cold boots and warm reboots:
  a known vendor U-Boot / display-init limitation** (product owner,
  2026-09-16; v0.0.10 ships with it, docs/hardware/V0.0.10_RELEASE_SMOKE.md).
  Observed on unit A: the splash sometimes appears on a cold boot and sometimes
  stays black on a true cold boot (about 60 s without USB power, base
  detached); a warm reboot can do the same; both `k230_logo` invocations can
  execute and load 2,799,104 bytes while the panel stays black, with console
  output identical to a boot that shows the splash; keyboard-base presence
  does not explain it; the boot into Doors is otherwise successful. The image
  is not the cause: its boot path is byte-identical to the one whose splash
  was verified on 2026-09-15. Unknown: the root cause inside the vendor panel
  power, reset and DSI bring-up, and a deterministic retry point that fixes it.
  Not attributed to power-off time, discharge or keyboard-base back-feed. A
  single `PHY_STATUS` read at the U-Boot prompt does not decide lit or black.
  **Tried and rejected:** running `k230_logo` a second time from `bootcmd`
  (`236a142`, branch `experiment/v0.0.10-splash-bootcmd-retry`) passed every
  software and image gate, showed the splash on three warm reboots, but left
  two cold boots black and added about 1 s to every boot. Investigating the
  vendor bring-up is post-v0.0.10 work (ROADMAP_HISTORY.md).
  The original finding, 2026-09-15: no boot splash after a warm `reboot`
  (VERIFIED on unit A, docs/hardware/DOORS_GRAPHICS_GATE.md), while from
  power-on the splash showed and handed over cleanly to the shell. After `reboot` U-Boot still loads
  `/logo.xrgb` and prints `RM69A10 direct XRGB8888 logo.xrgb full-screen OSD4`,
  but the panel stays dark until the shell's first mode set re-initialises it
  (about 17 s), so the unit boots normally with no splash rather than a broken
  one. Held at the U-Boot prompt: panel power and reset GPIOs are high, and the
  DSI PHY has lane 0 turned to receive with the data lanes in stop state
  (`PHY_STATUS 0x15bb`); running the vendor `k230_logo` command once more
  lights the splash (`0x1529`). U-Boot's first DSI bring-up after a warm reset
  does not recover the link, while its write that would reset the DSI host is
  commented out (`display_logo.c`). Independent of the splash file: U-Boot, the
  kernel and the device tree are vendor code the Doors graphics did not touch
  (whether the stock LILYGO splash does the same was not run). A fix belongs in
  the vendor U-Boot overlay, for example resetting the DSI host before the
  command phase or repeating the bring-up after a warm reset, and needs the
  product owner's go. (The 2026-09-16 observations above supersede "from
  power-on the splash shows": it does not always.)

- PocketRadar H1: the scan screen repaints a 520 x 520 custom-drawn scope at
  20 Hz (RADAR_TICK_MS), which is the app's whole frame cost and is unmeasured
  on the K230. Measure frame time and CPU load on hardware before changing
  the design; the sweep is three filled arcs and a line, and every contact is
  two to six draws, so the knobs if it is too slow are the tick rate, the
  sweep band count and the scope size, in that order. Do not pre-optimise
  against a number nobody has taken (product owner, 2026-09-06).

## Licensing

- Xinyuan-LilyGO/T-Display-K230 (BSP scripts and launcher) has no licence.
  Treated as documentation only. Ask LILYGO.
- ~~PocketOS first-party licence undecided.~~ Doors is Apache-2.0 since
  2026-10-01 (ADR-013). Publishing the source or an image is still blocked
  by the keyboard tables copied from LILYGO's launcher (deferred until the
  owner is at a unit), and for the image by the model, vendor packages,
  launcher and firmware (docs/licensing/APACHE_2_READINESS.md §14). The
  artwork is resolved: Apache-2.0, with the Doors brand reserved
  (docs/licensing/BRAND.md).

## Build environment

- WSL VM shuts down seconds after the last `wsl.exe` client exits, killing
  background builds. Keep a client alive or set `vmIdleTimeout`.
- Buildroot rejects the WSL default PATH (Windows entries with spaces).
  Scripts export a clean PATH.
- Primary toolchain mirror `ai.b-bug.org` does not resolve; fallback
  `download.kendryte.com` works at roughly 1 MB/s.
- The vendor image build takes several hours on this laptop and dies on
  sleep; Buildroot resumes from stamps.

- `vendor/RadioLib` is an ignored working-tree checkout, so `git clean -xdf`
  removes it and `apply_to_sdk.sh` then fails at step 5/5 with a bare rsync
  "change_dir failed" that does not name what is missing. Restore it at
  `034126e` (7.7.1) before a release build. Found 2026-09-10 during the
  v0.0.7 pre-release build.
- The `/mnt/c` checkout of the vendor BSP is a Windows checkout with CRLF line
  endings; its scripts fail with `env: 'bash\r': No such file or directory`.
  The build vendor tree is the WSL-native `~/work/t-display-k230`, which is
  also the one whose pins are enforced. Point `POCKETOS_VENDOR_DIR` at it.
- Do not export `GIT_DIR`/`GIT_WORK_TREE` around a build. The v0.0.7 worktree
  needs them (its `.git` names a Windows path WSL git cannot follow), but
  exported globally they answer for the worktree on every `git -C` the build
  makes, including the pin check - which then compares the vendor BSP commit
  against the PocketOS commit and refuses a correct tree. Scope them to the
  worktree, e.g. with a `git` wrapper that sets them only for that path.
- Re-running `apply_to_sdk.sh` on an existing output tree invalidates the SDK
  overlay sync stamp; Buildroot then re-syncs the overlay and can hit
  "duplicate filename ... already applied" on a host package's patch step.
  Removing that package's build dir and re-running make recovers (the
  scratch `build_pocketos_retry.sh` does this automatically). WSL clock
  jitter also made perl's MakeMaker abort once with "Makefile out-of-date";
  a plain re-run resumes.
- The build id is compiled in through `-D`, so an incremental `make` after a
  VERSION or commit change leaves the old id in an object that is not stale by
  mtime. `radiod_mock_test` then fails on a build string that is otherwise
  correct. Run the suite from `make clean` before believing such a failure.
- Neither `apply_to_sdk.sh` nor Buildroot removes files an earlier package
  install left in the target tree. Since ADR-005 Phase 2 that matters across
  branches: a PocketOS-era source built in an SDK where the Doors names have
  been installed carries a stale `/usr/bin/doors`, `/etc/doors-release` and
  `/usr/share/doors` into its image. Remove them, and the
  `/etc/pocketos-release` link, from `output/k230_pocketos_defconfig/target`
  before such a build (ADR-005, "Upgrade and rollback"; the same applies to a
  unit rolled back with an older `deploy.sh`).

## Software

- A reflash costs SSH access twice over: the fresh rootfs has no
  `/root/.ssh/authorized_keys` and a newly generated host key, so the bench key
  must be restored over the serial console and `known_hosts` rewritten only
  after the new fingerprint is read off that console. Expect it on every
  reflash and attach the serial console before starting (v0.0.7 release run).
- `pos-hwcheck --lora` prints `REQUIRES RADIOD STOPPED` in its report but does
  not enforce it. On a bench where radiod holds the mock backend it never opens
  the SPI device, so the probe appears to work with radiod up and the violation
  is silent. Stop radiod first; consider making the probe refuse.
- The gated card writer's own final line is `RESULT: FLASH FAIL` on every
  successful flash, because it compares the whole-image hash and Windows stamps
  the MBR disk identifier into bytes 440..443. Only the byte-compare wrapper's
  `VERDICT:` line is meaningful. Reading the writer log alone will mislead.
- The `Powering off...` screen's instruction cannot be read in practice: the
  board goes down about 200 ms after it appears. The instruction that matters
  is the one in the confirmation dialog, which is read before committing. Keep
  the terminal text for the case where the command fails and the screen stays
  up, but do not rely on it. (v0.0.7, unit A.)
- The System Status `Model` row dot-truncates to `Canaan CanMV-K230 wit...`.
  Correct behaviour rather than overflow, and the identifying half is visible;
  the device font is wider than the simulator estimate suggested.
- `POS_STYLE_TEXT_MUTED` (`#5e6670`) against `POS_STYLE_CAPTION` (`#8d99a6`) is
  at the edge of what the panel resolves at caption size - an operator can tell
  them apart side by side and not from memory. Muting is fine as a secondary
  cue; anything that has to say "live" needs a chip or a colour with more
  distance. Measured on unit A and in the simulator, 2026-09-10.
- radiod v0 has no client arbitration: any client can reconfigure the radio.
- radiod airtime accounting is process-local and lost on restart.
- `radio.send` is synchronous and blocks radiod for the airtime (about 1.3 s
  for the EU868 default with 255 bytes, 9 s at SF12/BW125, up to 225 s in
  the SF12/BW7.8/CR4/8 corner). Documented v0 behaviour; `timeout_ms` is
  refused. `radio.send_async` (radiod async IPC, 2026-09-19) is the
  asynchronous path, and it is what meshcored uses; `radio.send` stays
  synchronous for its existing callers (the Radio app, `doors radio send`).
- pocketipc disconnects a client that does not drain its socket within
  200 ms of a blocked write (documented backpressure policy). Event
  subscribers must read continuously.
- The settings store must not hold secrets; there is no credential storage
  yet (security note in `ui/shell/settings.h`).
- The shell blocks the UI thread on pocketipc calls. From 0.0.3 the
  once-a-second status poll carries a 200 ms deadline; from 0.0.4 so does
  every app tick on the LVGL thread (the Radio app's refresh and its mock
  inject button), after unit A showed the poll alone was not enough: with
  the Radio app open, its tick blocked the panel, touch and the shell's own
  socket while radiod was stopped, until radiod answered again. The v0.0.4
  retest then showed the reconnect after a timeout blocking in `connect()`
  once radiod's listen backlog had filled with the shell's own abandoned
  connections; the connect is bounded by the same deadline now. Only
  `radio.send` still waits, which is correct while a timeout there would
  report failure for a packet that was transmitted. A service that can
  accept a request and answer later, and the asynchronous transmit it would
  allow, are still needed before netd.
- Shell app launch by touch is untested in the simulator (only `--open`).
- App lifecycle is create/tick/destroy only: no pause, resume or suspend
  (ADR-002 names them as later work). Apps needing continuity persist state
  on each change themselves (PocketFleet finding 7). v0.1 limitation.
- PocketUI has no shared panel caption, segmented control or segmented
  meter/list rows yet; PocketFleet carries app-local versions. They are DS
  §9 components scheduled for implementation step 5 and will be promoted to
  the shared library then (PocketFleet finding 5).
- Custom-draw widgets must call `pos_theme_watch()` (or subscribe to
  `pos_event_theme_changed()`) to repaint on theme changes; shared styles
  repaint on their own (PocketFleet finding 4, documented in pos_styles.h).
- Design System steps 1 to 4 only: the status bar is not yet the four-cell
  layout of §9 (the radio chip sits vertically high in the 56 px bar), the
  launcher tiles and panels use role styles but not every §7 dimension, and
  LV_SYMBOL glyphs still come from the Montserrat symbol fonts until the
  stroke icon set exists (step 5+).
- LVGL's `generate_lv_conf.py` writes `LV_FONT_CUSTOM_DECLARE` into the
  template's comment example instead of the define; the body font is
  therefore set at runtime on the screen, and `LV_FONT_DEFAULT` is unused.
- Radio profile is not persisted (by design in v0): every radiod start
  returns to the EU868 defaults with the power from `--tx-power-dbm`
  (`RADIOD_TX_POWER_DBM` in /etc/default/radiod, 2 dBm). Raising it is a
  per-session operator action. Persisting a chosen profile is future work
  and must not persist a raised power silently.
- The USB Ethernet adapter (RTL8152B) has no burned-in MAC; the kernel
  assigns a random locally administered address on every boot, so the DHCP
  lease and IP can change per boot. PocketOS has no code that depends on a
  stable MAC, lease or IP (deploy.sh and the pair test take the address as
  an argument); the bench documents re-reading the IP after each boot. A
  stable address derived from the SoC is future work, not a fake constant.
- No RTC: the clock starts at 1970 on every boot until NTP syncs over the
  network. Log timestamps and crash-report names before that are
  boot-relative and cannot be ordered across boots; crash names carry the
  pid so they do not collide; the supervisor measures run time from
  /proc/uptime. PocketFleet seeds from `time(NULL)`, so a boot without
  network can repeat a layout.
- **Notes dates and order come from the file's modification time, which is
  the wall clock** (`notes_store.c`, `st_mtime`). A note saved before NTP has
  answered - or on a unit that never reaches it - shows a real-looking date
  such as `1970-01-01 00:02`, and sorts below notes saved after a sync in an
  earlier boot; on a unit that never syncs, "newest first" across boots is
  not reliable. No text is lost. Clock's rule for an unset wall clock
  (`CLOCK_WALL_VALID_FROM`: say nothing rather than a time the board does not
  know) is not applied here. Found by the 2026-09-23 cold review (R6), host
  evidence from reading the code; follow-up.
- Shutdown prints `mount: mounting /dev/mmcblk1p1 on /boot failed: Device or
  resource busy`, three `Can't open blockdev` lines and `vo_init: not found`:
  the vendor `S31canaan_isp` ignores its argument and re-runs its start
  actions (mount /boot, modprobes, isp_media_server, vo_init) when rcK calls
  it with `stop`. Harmless vendor noise, not a PocketOS action and not a
  corruption risk (`/boot` is already mounted and is unmounted normally by
  `umount -a -r` afterwards). The same shutdown also prints `Stopping crond:
  no /usr/sbin/crond found; none killed` followed by `FAIL`, from the vendor
  crond init script (seen on unit A with v0.0.8 and v0.0.10); vendor noise
  too.
- The vendor launcher is not in the image (see "Vendor launcher removed"
  at the top). Up to v0.3.0 it was on by default, so a freshly flashed card
  booted the LILYGO launcher once, until the panel switch was written by
  hand; there was no first-boot flag behind that. radiod still starts with
  the mock backend.
- On the K230 image /var/log is a tmpfs. PocketOS logs, crash reports and
  the supervisor logs therefore go to /var/lib/pocketos/log (persistent
  ext4); the stdio capture of each service is restarted on every boot with
  the previous one kept as `.1`. Ordinary log lines are written once, to
  the pocketlog file (POCKETOS_LOG_STDERR=0 in the init scripts).

## v0.0.8: PocketNotes, PocketClock and the system alert

From the v0.0.8 cold review and release preflight (host evidence, each
reproduced) and the release acceptance on unit A
(docs/hardware/V0.0.8_RELEASE_SMOKE.md). None of these blocks the release.
Any fix is a code change and moves the build id, so each waits for a later
build.

Documented for v0.0.8:

- **Clock shows UTC.** The image configures no time zone (no `/etc/TZ`, no
  `/etc/localtime`), so PocketClock, the status bar and every alarm run on
  UTC. VERIFIED on unit A, 2026-09-11: the face read 15:29 against a local
  clock two hours ahead. The time itself comes from `S48sntp` and `S49ntp`
  at boot when there is a network (within 115 s of a cold boot on the bench
  LAN).
- **The minute stepper wraps within the hour.** In the new-alarm form,
  Minute +5 from :58 gives :03 of the same hour, a time already past, and an
  alarm added for a minute that has begun arms for the next day. Adjust the
  hour as well. Found on unit A.
- **The Clock label's keyboard Done does nothing.** Its `on_done` is NULL,
  where DS §17.3 has Done dismiss the keyboard. Tap Add.
- **Opening a long note is slow.** The editor sets the note's text under its
  2000-character cap, and LVGL then inserts it one character at a time:
  quadratic in the length. On the host (i7-8665U, release LVGL) 300
  characters take 16 ms, 1,000 take 159 ms, 1,990 of prose 0.6 s and 1,990
  unbroken `ø` 4.7 s; lifting the cap around the call, as the read-only path
  already does, takes 0.7 ms. On unit A a 1,980-character prose note opens
  in under 4 s (operator count); the two-byte worst case is unmeasured on
  the K230.
- **Notes saves only on the way out**, on Done or when the app closes. A
  shell crash or a power cut during editing loses that session's edits;
  there is no autosave timer, by design. The save on the way out is real on
  hardware, including for edits nobody meant: a stray Backspace in an open
  note was saved like any other edit during the release acceptance.
- **Store I/O errors.** The Timber and Clock stores treat an I/O error while
  loading as a damaged file, and the next save replaces the data. A Notes
  save that fails (an unwritable store) closes the editor and drops the
  edit, silently when leaving by Back. The proper fix belongs to the common
  state facility (ROADMAP_HISTORY.md, step 5).
- ~~An orientation change ends a running stopwatch, countdown and snooze.~~
  Fixed on `fix/clock-rotation-state`: the outgoing shell hands its clock
  runtime to the incoming one through a boot-scoped file on the `/run` tmpfs
  ("Storage" in docs/apps/POCKETCLOCK.md). **PASS on unit A, 2026-09-18, build
  `646dcbb`** (docs/hardware/CLOCK_ROTATION_STATE_GATE.md): a stopwatch and a
  countdown both ran through a turn of the display and came back continuing.
  What is left, and is by design: a shell that is killed or crashes writes no
  handoff, so a running stopwatch, countdown or snooze ends there; and a power
  cycle clears the handoff, which is the whole point, because every instant in
  it is measured on a clock that starts again at the boot. The snooze leg was
  not run on the panel; it is covered on the host across a real `execv`.
- **The shell does not log that an alert was shown.** `open app` and
  `close app` lines prove that an alert navigated nowhere, but whether an
  alarm sheet appeared at all can only be read off the panel.

Deferred to the physical keyboard milestone. The premise of this block has
changed: the device now has a physical keyboard, so these are reachable where
they once were not.

- ~~DS §18.8 key isolation~~ — **CLOSED 2026-09-12**, commit `2afe7fe`,
  accepted on unit A the same day
  (`docs/hardware/KEYBOARD_BRINGUP_2026-09-10.md` §5.4).
  Keys no longer reach the app behind
  an alert, and an app asking for the touch keyboard while an alert is up is
  refused rather than obeyed. The mechanism differs from the one DS §18.8
  sketched — a private alert focus group with the stream redirected into it,
  rather than the actions joining the one group — and DS §18.8 records that.
  One path is **not** tested and, to be exact about it, **not covered
  either**: `lv_group_create()` failing at shell start-up, which would leave
  the alert without its group. An earlier version of this entry claimed the
  failure was "covered at the `pos_input` boundary" by `shell_alarm_test`'s
  nine assertions that the keyboard is never suppressed while the stream is
  un-redirected. That is the wrong direction. Those assertions catch
  suppression standing without isolation — the harmless half. Allocator
  failure produces the other half: no group, so the push is refused, so
  suppression is never raised and the stream is never redirected, and the
  alert is shown anyway with every key still reaching the app underneath.
  The coupling check passes in exactly that state, because nothing is
  suppressed.

  So what is actually covered is the `pos_input` contract (a refused push
  changes nothing), not the consequence at the alert. The trigger is LVGL
  heap exhaustion during shell start-up, which has never been observed and
  would be accompanied by larger problems; the entry is left open rather than
  closed, and deliberately not worked on in the v0.0.9 keyboard branch. The
  smallest honest fix when it is taken up is to make the failure loud — log
  it at `shell_alarm_create()` and refuse to raise the alert silently without
  isolation — rather than to keep asserting coverage that does not exist.
- `pos_input_add_source` loses `LV_KEY_NEXT` and `LV_KEY_PREV`: LVGL's keypad
  processing consumes them in the source's private group, so the
  simulator's Tab key does nothing. A physical keyboard must push through
  `pos_input_push_key`, not be adopted.
- The Notes delete dialog leaves focus on the hidden text field (DS §17.5).
- Only text fields and dialog buttons join the focus group (DS §17.2).
- **Tab does not advance focus in an ordinary text field.** Measured on the
  host, 2026-09-12, while testing the alert isolation: with a text area
  focused, `lv_group_get_editing()` reads **0** — LVGL does *not* put the
  group into editing mode — and yet `LV_KEY_NEXT` leaves focus on the field,
  where DS §17.2 says a logical Next moves it. This is **pre-existing and
  separate** from the §18.8 work: an alert swaps the whole delivery group, so
  what NEXT does inside the app group cannot affect it, and Tab does move
  focus between the alert's own actions (verified on unit A, Test A). It is
  recorded here because the mechanism was assumed to be editing mode before
  it was measured, and it is not.

Future cleanup:

- No store fsyncs its directory after the rename. Notes came back
  byte-identical from a real power cut on unit A (2026-09-10), so this is
  hardening, not a known loss.
- **A duplicate alarm line was observed in `clock.conf`.** During the §18.8
  acceptance on unit A (2026-09-12) the store held `alarm 0 15 36 0 test æøå`
  **twice**. Both copies were disabled, so neither could ring, and nothing in
  the keyboard or alert work touches the store — it is noted because it was
  seen, not because its cause is known. Whether it came from a save path, an
  earlier bench session or an edit through the app is unestablished.
- ~~The launcher has eight tile slots and uses seven. Resolve before a ninth
  app.~~ Resolved on `feature/post-v0.0.9-foundations`: five rows, ten tiles
  (Calculator and Settings), 870 px of the 1176 below the status bar. A
  sixth row still fits once; an eleventh and twelfth app need the launcher
  to scroll or to change.
- `notes_text_is_utf8` accepts overlong sequences (`C0 80`, `E0 80 xx`) and
  the lead bytes `F5` to `F7`. A note written outside the app that contains
  them loses those bytes on Done.
- An existing whitespace-only note is kept when it is left unchanged,
  although a note edited down to blank is deleted.
- An alarm that comes due while another alert is ringing is lost for that
  day if the ringing alert is acknowledged after midnight. Waiting snoozes
  survive midnight.
- The header comment of `apps/clock/clock_app.c` still describes the app
  before the shell took over the alarms: a ringing screen of its own, and
  alarms that ring only while the app is open. Correct it with the next code
  change to that file.
- ~~`apply_to_sdk.sh` enforces the BSP and SDK pins but not RadioLib's.~~
  Fixed: RadioLib is pinned in `platforms/k230/vendor_radiolib_commit.txt`
  and enforced by the same `pin_check` as the BSP and SDK, a dirty RadioLib
  checkout is refused because it is copied rather than archived, and both the
  commit and that state travel in the applied manifest and BUILD_INFO.txt.

## Post-v0.0.9 foundations: Wi-Fi, Settings, brightness, Calculator

Branch `feature/post-v0.0.9-foundations` (2026-09-12). Host-tested, and on
2026-09-13 validated on unit A: Wi-Fi, brightness, Settings and Calculator
PASS; audio silent stages only. The bench records are the unit A sections of
docs/hardware/WIFI_2026-09-12.md, docs/hardware/DISPLAY_BRIGHTNESS.md and
docs/hardware/AUDIO_FEASIBILITY_2026-09-12.md, and the end of
docs/apps/SETTINGS.md and docs/apps/POCKETCALCULATOR.md.

Decisions:

- **ADR-003 (Wi-Fi credentials) is accepted for this milestone** (owner,
  2026-09-13): passphrases persist in `/var/lib/pocketos/netd/wifi.conf`,
  protected by Unix file permissions only (0700/0600, root). The hex in the
  file is an encoding, not encryption; anyone with root or the card can read
  them. Revisit when a device-bound key store exists.

Wi-Fi (netd):

- **WPA3-only networks cannot be joined**: the rtl8189fs build has 802.11w
  off and no SAE path (DOCUMENTED from the driver source). Transition
  networks are joined as WPA2. A driver rebuild would be a BSP change.
- **Passphrases are printable ASCII only** (8..63, IEEE 802.11i). A network
  whose passphrase contains æ, ø or å is refused with a message saying so.
- **The vendor `ifup wlan0` stanza is still in the image.** `S40network`
  runs it when no Ethernet adapter is present at boot; it reads `wlanssid`
  and `wlanpass` from the U-Boot environment (review item F2). On unit A it
  does not conflict with netd (VERIFIED 2026-09-13): without
  `/etc/fw_env.config` it fails before starting anything, and its
  `ifdown` `killall wpa_supplicant` never runs because wlan0 is never
  recorded as configured. It is disarmed by that missing file, not by design.
  netd would report `interface_busy` rather than fight a supplicant it did
  not start. Removing the stanza at apply time is still recommended; it
  changes the image build script and was left for the owner.
- **Default routing with Ethernet and Wi-Fi both up is not managed**:
  BusyBox's udhcpc script adds a default route per interface without a
  metric. On the bench, with both on one LAN, both routes were installed and
  the kernel chose eth0 (VERIFIED 2026-09-13). Products without the adapter
  have only wlan0.
- **A control-socket reply can arrive late.** netd waits 300 ms for
  wpa_supplicant; once on unit A, right after an auth failure, a STATUS poll
  timed out and the next one answered (one WARN, no effect). Stale replies are
  drained before each request, but one arriving after the next request is
  sent would be read as that request's answer. Not seen to cause harm.
- **wpa_supplicant can be busy for seconds right after it starts.** On unit A
  (build 09be665, 2026-09-25) `/etc/init.d/S55netd restart` at runtime
  reproducibly left Wi-Fi disconnected: the supplicant answered PING, ATTACH
  and GET_CAPABILITY, then nothing for seconds, and netd gave up the saved
  network it was adding and never offered it again. The same netd connects at
  boot. netd now retries the hand-over and restarts a supplicant only after
  10 s of silence (tests/netd_test.sh, "busy"). Why it is busy is not
  established: a blocking scan in the rtl8189fs driver started by the first
  ENABLE_NETWORK fits (ASSUMED). On unit A (2026-09-25, netd sha256
  ef8b260c… over 09be665) the trigger is netd stopped for 60 s, then
  started: the shipped netd stayed disconnected 2 of 2 times; the fixed one
  retried once, removed the half-made entry and connected in about 4 s 3 of
  3 times (VERIFIED). Restarts with no pause, with or without a scan just
  before, connected in 3 s with both (8 of 8 old, 5 of 5 new).
- **One unreproduced netd_test event (test flake, non-blocking).** On
  2026-09-13 one full `make test` on `8d7af16` had 13 netd_test failures: every
  `wifi.connect` in the malformed-input block that reaches netd's readiness
  gate returned an unexpected code, while the checks netd refuses earlier
  passed. That pattern fits a short `starting` window (code 5, documented, and
  waited out by `pos wifi` and Settings), for example a supplicant restart,
  but the run's logs were deleted, so the cause is not established. It did not
  recur in 51 later executions: 21 isolated runs (3 of them under heavy CPU
  load), a bounded campaign on `9e24ec5` of 20 isolated runs (10 normal, 10
  under moderate load), 2 focused runs on `815f76e`, and 8 inside full suites. netd_test now keeps a failed run's evidence
  (netd log, fake supplicant record, store, every request and response with
  times, status at each failure, exit status) in `out/test-failures/` or
  `$TEST_EVIDENCE_DIR`, so a recurrence can be diagnosed. No product change.
- **No regulatory domain** is set; the driver uses its built-in channel plan,
  and the kernel logs that `regulatory.db` is missing.
- **Wi-Fi power depends on a pad pull-up.** GPIO45 enables the Wi-Fi
  regulator, nothing in Linux claims it, and the net has a 100 kΩ pull-down:
  any GPIO consumer of gpiochip1 line 13, or a load on header pin 12, can cut
  Wi-Fi power (ASSUMED from the schematic).
- **netd has no events**; clients poll `wifi.status`. Settings polls once a
  second.
- Hidden networks can be joined with `pos wifi connect --hidden`, not from
  Settings.
- Copies of a passphrase inside cJSON (the IPC request) and inside
  wpa_supplicant are not wiped; netd's own buffers are.

Brightness:

- **At the 10 % floor Night mode is marginal**: readable and comfortable in
  Normal, readable but marginal in Night on unit A (2026-09-13). The floor
  stays at 10 %. Brightness 0 (DCS `0x51 00`) is unreachable from PocketOS and
  its visual effect is unknown.
- A shell restarted from an SSH session inherits that session's umask, so
  `settings.conf` rewritten by it becomes 0600 rather than 0644. Only root
  reads it; bench effect only.

Audio (not implemented, findings only):

- **The booted default routes I2S to the header pads with IO35 as data, and
  GPIO35 is `IO35_DISEN`** (display power) on the schematic. On unit A IO35
  reads low with the panel powered, so the bypass resistor R54 is STRONGLY
  INFERRED fitted (2026-09-13); it has not been seen. No playback test until
  it is checked by sight and the route is switched to the codec.
  **Refined by the audio milestone below**: the built-in speaker found in a
  second unit is on that I2S route (a MAX98357A on a separate base board), not
  on the codec. **Superseded 2026-09-13**: R54 closed by evidence and speaker
  playback validated on unit A with the panel steady (audio milestone below).

Calculator:

- Holding the keypad backspace does not repeat.
- The error state leaves on any key; an operator pressed there is not applied
  (docs/apps/POCKETCALCULATOR.md).

## Audio milestone: pocketaudio, pos-wave and Wave

Branch `feature/audio-ggwave` (2026-09-13, from master `a748101`). Host-tested
and cross-built; build `e778daf` is deployed on unit A. **RECEIVE and SEND are
both validated on unit A** (2026-09-13): the owner's phone sent `test` and
Wave decoded it (hardware map §15); the controlled first SEND of `DOORS` at
-26 dBFS was heard, decoded by Waver, left the panel steady and IO34 low
(§16). Both paths are enabled in code and need no override. The hardware record is
docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md; the app is docs/apps/WAVE.md.

Decisions:

- **ADR-004 accepted for this milestone** (owner, 2026-09-13): a
  per-operation helper process owns the sound card for Wave, a narrow
  exception to ADR-002, not a replacement for it.
- **RECEIVE-first** (owner, 2026-09-13): the microphone test (hardware map
  §13) is approved; SEND stays blocked until the amplifier IC, the speaker
  connector and R54 are identified physically. 2026-09-13: the amplifier is
  identified on the second unit (unit B); R54 is not seen on any unit, so SEND
  stays blocked on both. Later the same day the owner confirmed units A and B
  are the same hardware; R54 was closed by evidence (hardware map §8.8) and
  the first controlled SEND on unit A was approved, run once and passed
  (§16); `playback_verified` became 1 in `e778daf`, no limit raised.

Audio hardware:

- **The built-in speaker is driven over I2S by a MAX98357A, not by the
  codec** (2026-09-13, owner's photographs of the second unit): the speaker
  plugs into a white 2-pin connector on a separate base board, beside an IC
  marked `AKK`; with the vendor pinmap, BSP patches, device tree and launcher
  that is a MAX98357A, CONFIRMED WITH HIGH CONFIDENCE. I2S on IO32/IO33/IO35
  through the header, enable IO34, bridge-tied output: neither speaker wire is
  ground. GPIO35 is in the speaker path. The photographs themselves show the
  board as `K230_nRF52840_Board` VER 0.3 and the IC as a 16-terminal QFN. No
  base-board schematic or layout exists in any vendor source. Two identical
  2-pin receptacles sit beside the IC and the speaker plug is not seated in
  any image, so which one is the speaker's is unknown; the designator and
  `2618` are unknown too.
- ~~R54 is still inferred, not seen.~~ **Closed by evidence, not by sight**
  (hardware map §8.8): IO35 high can only turn the display switch further
  on, unit A's rail is up with IO35 low, and the V1.0 polarity holds for this
  hardware (V1.0 marking on the identical unit B). The first SEND watches the
  panel; a photograph (§8.7) remains optional.
- ~~Unit A's base board is unconfirmed.~~ Units A and B are the same hardware
  configuration (owner, 2026-09-13); unit B's inspection stands for unit A,
  which stays unopened.
- **Unit B's speaker socket is not identified.** Two identical 2-pin
  receptacles sit beside the MAX98357A and its plug was out in the photos;
  the other one may be a fan or power output. Before unit B is powered with
  its speaker connected: continuity from the receptacle to the IC's OUTP
  (pin 9) and OUTN (pin 10), and the base board reseated (hardware map §9.1).
  Not a blocker for unit A.
- **The amplifier's gain strap and supply are unknown**, and no longer a risk
  to the 1 W speaker: by the datasheet's gain law the -12 dBFS ceiling is at
  most 0.44 W at the highest strap, and the first SEND (-26 dBFS) at most
  17 mW (hardware map §11).
- ~~The microphone slot is inferred.~~ Channel 1 (right) is the on-board
  microphone on unit A (hardware map §15).
- ~~Every capture opens with a start-up transient.~~ **Fixed in `c688309`**:
  the K230 codec/capture startup transient (140-210 ms at negative full
  scale) is read and discarded for 500 ms inside pocketaudio's normal
  per-call wait, so no decoder, level meter or recording sees it; STOP still
  works during it (hardware map §15.1). A residual DC offset around -29 dBFS
  decays over the next ~600 ms, below ggwave's band.
- **The on-board mic's gain is fixed at 30 dB** and not adjustable through
  ALSA (`Mic Capture Volume` writes only the left channel); the codec's ALC
  behaviour is unknown. A loud room may clip.
- ~~After a SIGKILL of pos-wave the route and the amplifier line keep their
  last values.~~ Fixed before merge: write-ahead recovery record, reconciled
  by the next audio open and by `pos-wave recover`, which the Wave session
  starts when its helper dies by a signal. Verified on unit A for RECEIVE,
  both by `pos-wave recover` and by the next open (hardware map §15).
  Remaining gap: if the shell and its helper are SIGKILLed together, nothing
  restores the state until the next audio operation or a reboot.
- **Mic bias stays on after the first capture** (driver), and a speaker-route
  playback still enables the headphone driver (driver): keep the jack empty
  during the tests.
- ~~Unit A's read-only checks listed in the hardware map §10 were not run.~~
  Run before the deploy, identical to the morning record (§15).

Wave and ggwave:

- ~~The K230 speaker path is gated in code.~~ Both K230 paths are validated
  (`capture_verified` 1 since `c688309`, `playback_verified` 1 since
  `e778daf`). The limits did not move: -12 dBFS ceiling, modem volume cap 25,
  Wave default volume 10, all held by tests/wave_lint.sh.
- ~~Wave's own TRANSMIT button has not been pressed on hardware.~~ Done
  2026-09-13: the owner sent `HELLO` from the Wave UI on unit A at its
  default volume 10; it was heard, Waver decoded it, the panel stayed steady,
  Wave returned to idle and nothing sounded afterwards (hardware map §16.3).
- **Over-the-air range and the speaker's response across 1.9-6.3 kHz are
  unmeasured**; the first SEND was decoded by a phone at arm's length.
- **ggwave's decode spike on the C908 is unmeasured.** Steady listening costs
  0.6 % of unit A's single core (hardware map §15), and the owner's receive
  decoded without loss, but nothing sampled the CPU at the end of a message.
  The capture buffer is 500 ms to absorb it.
- **ggwave defects worked around, not fixed upstream**: frame-sync loss on
  unaligned input, init reporting success on bad parameters, payload logging,
  38.5 s deafness after a missed end marker (wave_modem.h). Its TX instance
  allocates an unused 4 MB buffer for S16 output.
- ~~Ooura FFT licence terms are not stated in ggwave's `fft.h`.~~ Resolved
  from the author's page (docs/LICENSING.md, "Audio milestone"). The notices
  for ggwave, Reed-Solomon, the Ooura FFT, RadioLib, the IBM Plex fonts and
  LVGL now ship in the image as /usr/share/doors/THIRD_PARTY_NOTICES.txt,
  linked from the old /usr/share/pocketos path (docs/LICENSING.md,
  "Third-party notices"). **Still blocking distribution, not merging:**
  the toolchain's C/C++ runtime licences and some vendor packages are not in
  legal-info (LICENSING.md open items 5, 8). Doors' own licence (item 1) is
  decided: Apache-2.0 (ADR-013).
- Messages over 64 bytes from other ggwave programs are heard but not shown
  (reported as "could not decode").
- The launcher grid now has six rows and is full: a twelfth app needs a
  different launcher layout.

## radiod, from the asynchronous transmit work (feat/radiod-async-ipc)

- **No completion deadline for an asynchronous backend.** `radio.send_async`
  drives a backend through `tx_begin`/`tx_poll`. If a backend ever loses a
  hardware completion - a missed DIO1 edge, a chip that stops answering -
  `tx_poll` returns "still on air" for ever and radiod stays in `tx`: later
  transmits refused BUSY, `radio.configure` and `radio.cad` refused,
  `radio.channel` reporting nothing, and no event to say why. Only a restart
  clears it. **Deliberately deferred**, because no backend reaches that path
  on hardware: the mock completes on a deterministic deadline of its own and
  the SX1262 uses the blocking `send()` fallback, whose bound is RadioLib's.
  Required before the first backend that implements the pair for real; a
  deadline from the expected airtime, after which the transmit completes as
  failed and the radio is taken back (services/radiod/tx.h, docs/api/radio.md
  "Deferred: a completion deadline for asynchronous backends").
- **The SX1262 has no asynchronous transmit.** It keeps RadioLib's blocking
  `transmit()`, so radiod is still unresponsive for the airtime on hardware;
  only the submitting client stops waiting. Changing it means
  `startTransmit()` with a DIO1-driven completion, which alters the exact
  sequence this board's two successful on-air runs used, and needs hardware
  validation.
- **`radio.channel` cannot say whether the channel is busy on real
  hardware.** CAD detects a LoRa preamble at the configured modulation and
  takes the radio off receive, so it is not a busy signal; deriving one from
  RSSI needs a threshold nobody has measured on this board. Reported as
  `activity_known: false` rather than a fabricated `busy: false`.
- **None of the asynchronous transmit, lease, monotonic-timestamp or channel
  work has run on unit A.** Host and mock verified, compile verified for the
  SX1262 backend, hardware UNRESOLVED.
- `tests/netd_test.sh`'s "a foreign supplicant on wlan0" check is flaky under
  load: its `wait_state unavailable` window is 4 s, and it has been seen to
  miss once on a machine busy with a parallel build, then pass repeatedly on
  the same commit and on master. Pre-existing, unrelated to radiod.

## meshcored, the MeshCore protocol service (feat/meshcored)

- **What has run on hardware, and how.** meshcored on unit A with the SX1262,
  on air against a T-Deck RIFT peer: the first hardware gate (2026-09-19,
  docs/hardware/MESHCORED_HARDWARE_GATE.md), RIFT channels parts A-D
  (2026-09-21, RIFT_CHANNELS_GATE.md), RIFT improvements (2026-09-22,
  RIFT_IMPROVEMENTS_GATE.md) and 256 retained nodes (2026-09-22,
  MESH_NODE_CAPACITY_256_GATE.md) - RF VERIFIED on those builds. Every one
  of them ran bench-deployed binaries on a unit configured by hand
  (`/etc/default/meshcored`, the sx1262 backend in `/etc/default/radiod`);
  none ran from a flashed image. Nothing under `services/`, `core/` or
  `protocols/` has changed since the last of them (`b41be37`) except the
  netd late-reply fix (2026-09-23), which is host-tested only.
- **Messages are not persisted.** The identity and the node table survive a
  restart; the message list does not, and `mesh.messages` reports
  `persistent: false` rather than leaving that to be discovered. Writing
  decrypted message text to the device is a privacy decision the owner has
  not made. The obvious next step, and the one a RIFT conversation view will
  want.
- **A failed transmit stalls receive for up to 1.5 times the packet's
  airtime.** `mesh::Dispatcher` returns early from its loop while an outbound
  packet is unfinished, so a transmit meshcored cannot report as complete -
  one radiod refused, or whose completion said the bytes did not go out - is
  cleared by the dispatcher's own deadline rather than at once. For this
  profile that is about 1.1 s for an advert and about 2.3 s for a full-length
  packet. Bounded, upstream's mechanism, and not worth changing vendored
  scheduling for.
- **The PATH underflow is reachable over the air and is guarded, not fixed.**
  `vendor/RIFT/src/Mesh.cpp:172` computes a trailing length as `len - k` with
  nothing checking `k <= len` (debt 2 in `protocols/meshcore/README.md`).
  Running the MeshCore receive path in a daemon is what makes it reachable:
  it needs a valid MAC, and MeshCore adds contacts from adverts by itself, so
  any node that adverts can get there. meshcored refuses such a payload in its
  own handler before anything reads through the pointer, counts it as
  `path_payloads_refused`, and does not touch the vendored tree. The upstream
  defect is unchanged.
- **Contacts are added automatically, as upstream does.** Any node that
  adverts within range becomes a contact, up to 1000 on this port (upstream's
  default is 32); the table then refuses new ones rather than evicting -
  `mesh.node_remove` makes room. There is no allow-list and no "known nodes
  only" mode.
- **An advert sent before the clock is set is ignored by peers that know this
  node.** The board has no RTC and nothing checks the clock before signing
  (`SystemRTCClock::isSet()` exists in the port and is not called): an
  advert stamped 1970 is transmitted, and a MeshCore peer holding a newer one
  from this node drops it as a replay (upstream `BaseChatMesh.cpp:131`), so
  a name or route refresh silently does not arrive. Direct messages sent then
  carry 1970 timestamps too. Advert after NTP has set the clock. Follow-up:
  refuse or warn when the clock is not set. (Cold review 2026-09-23, R13.)
- **A direct message a peer resends is recorded once per attempt.** A resend
  carries the next attempt number, which changes the ciphertext and so the
  packet hash MeshCore de-duplicates on; meshcored does not de-duplicate on
  sender, timestamp and text, and RIFT shows each copy. Seen only if our ACK
  is lost and the peer retries (the T-Deck RIFT firmware sends attempt 0
  only). (Cold review 2026-09-23, R13; code path confirmed, rate unmeasured.)
- **The duty-cycle budget is upstream's default**, which is far above any
  regional limit, and nothing has exercised it against one. meshcored
  transmits only when a client asks or when the protocol owes a reply, so the
  budget has never been the thing that limited it; on a busy network with a
  UI above it, that changes. Regional policy belongs above the raw radio
  backend and is still the operator's.
- **There is no periodic advert**, by decision. A node that never adverts is
  not discovered by nodes that have not heard it; a client has to ask. What
  the right interval is - and whether it should exist at all on a handheld -
  is a decision for the phase that has a UI.
- **meshcored is in every image and runs on none by default.** The image
  package builds and installs it (`ENABLE_MESHCORED=1`, gated on the
  third-party notices, which cover MeshCore, ed25519 and Crypto since
  2026-09-22), and `S65meshcored` ships with `MESHCORED_ENABLE=0`. Enabling
  it on a unit (`/etc/default/meshcored`) means the node acquires the radio,
  needs radiod on the sx1262 backend, and will answer messages addressed to
  it (docs/services/MESHCORED.md).
- **One outstanding transmit at a time.** radiod has no queue and neither does
  this service: a second submission while one is outstanding is refused, and
  MeshCore is told the send did not start. That is the honest shape, and it
  means a busy node drops its own outbound packets rather than delaying them.
- **A message's state does not say whether it was transmitted** (known
  limitation, follow-up; cold review R2, 2026-09-23). `mesh.send` records the
  outgoing message as `sent_flood` / `sent_direct` when it is *accepted*,
  before anything reaches radiod, and the radio's outcome reaches only the
  `mesh.activity` `tx` event, keyed by radiod's `submit_id`, which the
  message does not carry. `failed` is never assigned. So a frame that never
  left - radiod restarted before dispatch, a `tx_failed` or `refused`
  outcome, the dispatcher giving up on it - leaves a channel message at
  `sent_flood` for good, looking exactly like one that went out (RIFT draws
  it `SENT · FLOOD · NO ACK ON CHANNELS`), and a direct message at `sent_*`
  until its ACK deadline, then `no_ack` rather than `failed`. A channel
  message is an unacknowledged flood in any case; only a direct message's
  `acked` confirms anything. docs/api/mesh.md ("Accepted is not
  transmitted") states the contract as it is. The fix is to carry the
  transmit outcome into the message - the submit belongs to one message id -
  and assign `failed` when the frame provably did not go out; it changes the
  meshcored TX state machine and is out of scope for the pre-release fixes.

### meshcored, deferred from the review-fix pass (2026-09-19)

Two low-severity findings from the independent review, left alone on purpose
so that pass stayed the size it was scoped to be.

- **`ensureDir()` accepts a state directory that already exists, whatever its
  mode or owner.** It creates the leaf 0700, and the files inside it are
  written 0600, so a fresh install is right. What it does not do is *correct*
  a directory somebody else created 0777, or one owned by another user - it
  takes what is there. On this image the service runs as root on a
  root-owned tree, so there is nobody to take advantage of it; on a system
  with other users there would be. The fix is to check the mode and ownership
  of an existing directory and refuse, or repair, rather than assume.
- **An outbound packet can be lost between two deadlines.** After a
  `radio.tx_done` goes missing, MeshCore's dispatcher clears its outbound slot
  at 1.5 times the packet's airtime while meshcored's transmit map holds its
  own slot for five seconds. In that window the protocol core will hand over
  another packet and meshcored refuses it, so the packet is dropped rather
  than delayed - correct, and reported to MeshCore as a send that did not
  start, but a packet nobody sends again. Narrowing it means either a shorter
  transmit-map deadline (which risks calling a slow completion lost) or a
  queue on this side, which is a design decision rather than a fix.

## RIFT, the mesh client

- **Channel management, rename and the path hash size have not been tried
  on air** (`feat/rift-management`; gate run on unit B 2026-10-02 without
  transmitting). Joins, leaves, the key checks and the path hash setting
  passed on the unit, and channel hashes matched an independent computation
  and, for Public, unit A; a live Public message was received and shown. No
  channel joined from RIFT has been written to by RIFT and read by another
  client, no rename has reached a peer, and no 2- or 3-byte flood *from
  meshcored* has been sent. The bench mesh's repeaters do carry other nodes'
  2- and 3-byte path floods (up to 11 relays, radio log of the gate); older
  repeater firmware elsewhere may not, which RIFT's confirmation says. Gate
  sheet: `docs/hardware/RIFT_MANAGEMENT_GATE.md` (steps 7 and 8 open).
- **No flood scopes, so a channel's scope cannot be set.** meshcored writes
  no transport codes (upstream's per-channel scope is a TODO; the T-Deck
  RIFT's is its own extension). Every channel floods unscoped, and the
  CHANNELS panel says so. Needs an owner decision and an on-air gate.
- **NODES' ZERO-HOP is passive; SCAN 0-HOP is the active one.** ZERO-HOP
  lists repeaters whose own adverts reached this node with no relay
  (`advert_hops: 0`, this run) or with a direct learned route, so a quiet
  repeater appears there only at its next advert. ACTIVITY's SCAN 0-HOP asks
  (`CTL_TYPE_NODE_DISCOVER_REQ`, docs/api/mesh.md "Repeater control") and
  lists every repeater that answers directly; host-tested, not yet on air
  (docs/hardware/RIFT_REPEATER_CONTROL_GATE.md).
- **Repeater control: a wrong password is indistinguishable from not being
  heard.** Upstream's repeater answers a wrong password with nothing, so
  RIFT shows "No answer to the login" for both. Logout is local only (MeshCore
  has none on the air). The password is wiped in RIFT and meshcored, but
  upstream's `sendLogin` copies it to a stack buffer it does not clear
  (vendored, not changed). pocketipc clears the frames it prints and reads;
  the kernel's socket buffers and cJSON's own parse are outside reach. A 48-byte RepeaterStats reply (one firmware generation) is padded to
  the AES block, so its last two fields may read as 0. Not yet on air.
- **RIFT's colour emoji reach message bodies, previews and claimed senders
  only.** They are compiled-in Noto Color Emoji images (docs/apps/RIFT.md,
  "Colour emoji"); skin tones are stripped by decision, a sequence with no
  artwork shows its parts, the images keep their colours in Night and Outdoor
  mode, and at Large text an 18 px emoji is small beside 22 px letters. Node
  names, the NET view and details still draw an emoji as Plex's box, as does
  every other app. Seen on unit B (docs/hardware/RIFT_COLOUR_EMOJI_GATE.md). Stored and sent text is
  unchanged.
- **A thousand nodes are proven with synthetic ones, not a real mesh.**
  RIFT (`RIFT_MAX_NODES`, DS §37.5) and meshcored (`MAX_CONTACTS`,
  `MCD_MAX_NODES`, PR #43) both hold 1000; at 1000 meshcored refuses new
  nodes rather than evicting, and RIFT's ACTIVITY says the node table is
  full. On unit B a table of 1000 - 259
  real nodes and 741 synthetic ones written into `state.v1` - loaded, its
  368 KB `mesh.nodes` reply reached readers draining every 50 to 150 ms,
  and RIFT showed `1000 KNOWN` with the session connected
  (MESH_NODE_CAPACITY_1000_GATE.md); the real table grew past 256 and
  survived a reboot. Not covered on hardware: 1000 real nodes, a shell
  stalled for more than 200 ms during a reply above about 215 KB (pocketipc
  would drop and reconnect it), and flash wear from rewriting a `state.v1`
  of up to 148 KB, which had no soak. Scrolling is VERIFIED with a real mesh
  of 241 nodes on unit A, end to end in both orientations at up to ~31 %
  shell CPU (RIFT_UI_NEXT_GATE.md); a full list of 1000 was not scrolled on
  a board.
- **The message sounds play through pos-record** (ADR-010 Amendment 1,
  ACCEPTED): only while RIFT's screen is open, and not at all while another
  app holds the audio lock. Whether the 150 / 222 ms tones are audible and
  pleasant on the speaker is not measured; host tests prove the files play
  to the end through the real helper on a fake card.
- **A resent direct message is shown once per attempt** (meshcored section
  above); RIFT's DM sound recognises the retry by sender, timestamp and text
  and does not sound twice, but the thread shows what the service recorded.
- **A message's state is meshcored's, and that state does not report
  transmission** (see "A message's state does not say whether it was
  transmitted" under meshcored). An outgoing channel message reads
  `SENT · FLOOD · NO ACK ON CHANNELS` whether or not it went out.
- **Channel identity after a slot is reused.** A channel conversation is keyed
  by slot, one-byte channel hash and local name (2026-09-23, cold review R3),
  so a different channel added into a slot that was emptied no longer
  inherits the old channel's history, and a reply from the old thread is
  refused instead of reaching the new channel. What stays indistinguishable
  is a different channel re-added into the same slot under the same local
  name with the same one-byte hash; telling that apart needs a per-channel
  identity meshcored does not report. Host-tested; not yet seen on air.
- **RIFT cannot tell a hung meshcored from a quiet one.** A pending request
  has no reply deadline; once the 16 request slots are full, requests are
  refused without dropping the connection, and the last state ("online")
  stays on screen. A dead meshcored is noticed (the socket closes); a
  stopped one (SIGSTOP, a stuck write) is not. (Cold review R14, deferred.)
- **Conversation names truncate in the landscape COMMS list** at about eight
  characters (the list is 260 px wide, DS §37.2). Accepted with DS §37
  (docs/hardware/RIFT_UI_DENSITY_GATE.md, finding 1).
- **A full conversation list drops the oldest conversations and the
  channels.** At the 256-conversation bound the oldest conversation is not
  listed, and channels are added only while the list has room. Accepted with
  DS §37 (RIFT_UI_DENSITY_GATE.md, finding 2).
- **Portrait ACTIVITY: the traffic graph's caption runs into its legend**
  while it reads `HEARD ON AIR · 20 MIN · NOTHING YET` (568 px wide); with a
  peak (`PEAK 4/MIN`) it fits. Cosmetic. Seen on unit B in the v0.2.0 smoke
  (build `c687cac`, right after meshcored restarted); landscape is not
  affected.

## Deferred from the 2026-09-23 cold review

Low-severity findings, confirmed by reading the code, left for after v0.0.11
on purpose. None has been reproduced on hardware.

- **Keyboard presence probe on the UI thread.** With no keyboard attached the
  shell's 1 s presence watch runs a full controller configure, including a
  3 + 12 ms reset pulse, on the LVGL thread - about one dropped frame per
  second, and GPIO43 toggled every second. The cost on unit A is unmeasured
  (R5).
- **A refused store is overwritten by the next save.** A `clock.conf` or
  `settings.conf` that fails to load (damaged, EIO, or a newer format after a
  rollback) is replaced, not preserved, the next time an alarm or a setting
  changes (R7).
- **Clock:** a backward wall-clock step across midnight rings a daily alarm
  at once and again the next morning; a damaged `/run` handoff naming a timer
  ring with no expired timer wedges the alert until reboot (R8).
- **No directory fsync after rename or unlink** in the Clock, Notes and
  settings stores: a power cut just after a save can revert it or bring a
  deleted note back. A torn file is not possible (R9).
- **System:** if `/sbin/reboot` or `/sbin/poweroff` fails after sysd accepted
  the request, the screen stays on "Restarting…" / "Powering off…" while the
  system runs on (R10).
- **Settings Wi-Fi:** one missed 200 ms netd poll shows "Wi-Fi service is not
  running" and rebuilds the screen until the next poll (R11).
- **Fleet:** after one failed save, a stale saved match can be offered as
  Resume at the next launch (R12).
- **pocketipc / init:** a second instance of a service started by hand takes
  over, and on exit removes, the running service's socket (meshcored guards
  against this, the others do not); supervisor pid files stay stale after a
  crash loop and are trusted with `kill -0` only (R15).
- **netd:** forgetting a network while a join is pending breaks the restore of
  the saved entry it displaced; netd can flag its own dying supplicant as
  foreign for up to 5 s (R16).
- **Shell:** `--rotation` is ignored when working out the next rotation, so a
  bench shell started with it restarts to the same orientation; Controls
  keeps polling netd behind the lock (R17).
- **Build:** `make clean` misses several objects (two missing spaces in its
  list, and the meshcored objects); `ENABLE_SX1262` is not part of the build
  stamp. Host builds only - the image package builds from a fresh tree.
- **Provenance quirk:** `apply_to_sdk.sh` asks git whether `vendor/RIFT` and
  `vendor/Crypto` are checkouts; a plain copy with no `.git` sits inside the
  Doors repository and git answers for that instead. The pin check still
  refuses such a tree unless the drift override is set.
