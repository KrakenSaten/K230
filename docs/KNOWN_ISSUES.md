# Known issues and open questions

Updated 2026-09-11. Move items to git history when resolved.

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
  staging scanout buffer), not a stock LVGL. The shell never calls the
  rotation API (the panel mode is 568x1232 portrait, rotation 0) and runs
  with `K230_LVGL_DRM_STAGING=1` set by S90pocketos-shell, as the vendor
  launcher does. Whether that call sequence drives the RM69A10 correctly is
  ASSUMED until tested (DS H1, H2).
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
  `umount -a -r` afterwards).
- The vendor launcher is still in the PocketOS image and owns the display
  and the radio by default; radiod runs with the mock backend until the
  launcher is switched off. The switch is persistent
  (`/etc/default/k230_phone_ui`, see platforms/k230/README.md) and
  S90pocketos-shell refuses to start while the launcher is enabled or
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
  One path is **not** deterministically tested: `lv_group_create()` failing
  at shell start-up, which would leave the alert without its group. Forcing
  it needs production-only hooks, so the failure is covered at the
  `pos_input` boundary instead: a refused push leaves suppression untouched,
  and `shell_alarm_test` asserts at nine points that the keyboard is never
  suppressed while the stream is un-redirected.
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
- The launcher has eight tile slots and uses seven. Resolve before a ninth
  app.
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
- `apply_to_sdk.sh` enforces the BSP and SDK pins but not RadioLib's. The
  v0.0.8 release build checked `034126e` and zero build products itself;
  until apply does, that check is a manual release step.
