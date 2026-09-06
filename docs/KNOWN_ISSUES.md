# Known issues and open questions

Updated 2026-09-04. Move items to git history when resolved.

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
  device (they need LV_USE_SNAPSHOT). Decision pending (review item H2):
  a PocketOS-owned override of the vendor LVGL config, or photographs for
  the first hardware validation.
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

- Re-running `apply_to_sdk.sh` on an existing output tree invalidates the SDK
  overlay sync stamp; Buildroot then re-syncs the overlay and can hit
  "duplicate filename ... already applied" on a host package's patch step.
  Removing that package's build dir and re-running make recovers (the
  scratch `build_pocketos_retry.sh` does this automatically). WSL clock
  jitter also made perl's MakeMaker abort once with "Makefile out-of-date";
  a plain re-run resumes.

## Software

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
- The shell blocks the UI thread on pocketipc calls; acceptable with local
  services, wrong for slow ones. Needs an async path before netd.
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
