# Known issues and open questions

Updated 2026-09-17. Move items to git history when resolved.

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

## Hardware and BSP

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
- The rounded corners' extent is PROVISIONAL (30 px squares, the vendor
  launcher's status-bar side inset). No datasheet gives it; unit A decides
  (POCKETOS_SAFE_CORNERS tries other values). The status bar uses the safe
  area, and so does every app that has been given a landscape layout -
  Calculator, Notes, Settings, System, Clock and Calendar - through the one
  rule pos_display_rect_insets() (DS §22.2, §23.4). The touch keyboard's
  bottom row (DEV-1 fixed 52 px keys, 6 px sheet padding) and the full-screen
  alert's card corners still reach
  into the 30 px corner squares; widening them changes approved geometry and
  is left for a design decision if unit A shows them cut.
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
  (docs/ROADMAP.md, "Landscape app adaptation").
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
- `aic8800` modules are modprobed by the vendor boot script although the board
  has RTL8189FTV; harmless warnings expected in dmesg.
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
  vendor bring-up is post-v0.0.10 work (ROADMAP).
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
- PocketOS first-party licence undecided.

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
  refused. An asynchronous TX path is a later design item.
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
  /proc/uptime. Persisted app state does not use the clock; PocketFleet
  seeds from `time(NULL)`, so a boot without network can repeat a layout.
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
- The vendor launcher is still in the PocketOS image and owns the display
  and the radio by default; radiod runs with the mock backend until the
  launcher is switched off. The switch is persistent
  (`/etc/default/k230_phone_ui`, see platforms/k230/README.md) and
  S90doors-shell refuses to start while the launcher is enabled or
  running. The launcher has no kernel driver for the LoRa module, so with
  it running the sx1262 backend must not be used.
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
  state facility (ROADMAP, step 5).
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
  Doors' own licence (PocketOS through v0.0.9) is undecided and external
  redistribution is not authorised; the toolchain's C/C++ runtime licences
  and some vendor packages are not in legal-info (LICENSING.md open items 1,
  5, 8).
- Messages over 64 bytes from other ggwave programs are heard but not shown
  (reported as "could not decode").
- The launcher grid now has six rows and is full: a twelfth app needs a
  different launcher layout.
