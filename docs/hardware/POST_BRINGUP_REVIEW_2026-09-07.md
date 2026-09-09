# Post-bring-up review, 2026-09-07 (offline, repository only)

Consolidation of the first physical K230 session and readiness review for
the next image. Written from the repository evidence only: the session record
(`BRINGUP_SESSION_2026-09-07.md`, sections 1 to 20, cited below as S§n), the
hwcheck report and dmesg (`hwcheck-unitA/`), the raw captures under
`hwcheck-unitA/h1-2026-09-07/`, `out/k230/BUILD_INFO.txt` and
`SHA256SUMS.txt`, the hardware baseline (`T-DISPLAY-K230.md`, cited as T),
the checklist (`BRINGUP_CHECKLIST.md`, cited as C§n), the vendor BSP patch
stack, the code on master `3d6120a` and the fix branch
`fix/pocketipc-sigpipe` at `feed274`. The physical unit was not touched, no
serial port was opened, nothing was transmitted, nothing was deployed or
rebuilt, and master was not modified while writing this.

Evidence classes: VERIFIED (observed on this unit or in a captured log),
DOCUMENTED (vendor material, DTS, schematic or repository docs), ASSUMED
(inferred), UNRESOLVED (not established), CONFLICTING (sources disagree).

## 1. Executive verdict

**READY WITH FIXES.** PocketOS 0.0.1 boots, owns the panel, runs both apps
with persistence, survives single-process crashes through its supervisor,
and drives the SX1262 through its own backend on real hardware, including one
transmit at 2 dBm with clean RX re-entry. Two defects were confirmed on the
bench: a radiod crash kills the shell through SIGPIPE (fixed on a branch,
reviewed below, merge-ready with a minor follow-up), and `pos-hwcheck --lora`
hangs. The next image needs the SIGPIPE fix, the hwcheck fix and a root
password before it is handed to anyone else; everything else is checklist
and documentation hygiene.

## 2. What works on real hardware (VERIFIED)

- Boot chain: U-Boot SPL and U-Boot 2022.10, `DRAM: 1 GiB`, kernel 6.6.36,
  login prompt about 25 s after `reboot`; rcS order with `Starting radiod
  (mock, EU868): OK`, `Starting pocketos-shell: OK`, `Starting
  k230_phone_ui: disabled` (S§10.2).
- Persistent panel ownership: the `/etc/default/k230_phone_ui` and
  `/etc/default/pocketos-shell` switches survived at least eight boots; the
  shell takes DRM (DSI mode set logged at 16.9 s) and shows the launcher
  correctly oriented (operator) (S§10.1, S§10.2).
- Touch: Goodix at 1060 x 2400 auto-calibrated by LVGL; no override needed;
  Fleet and Radar playable (operator) (S§10.1).
- Persistence: settings, Fleet save and Radar record across reboots and
  across shell restarts; logs and crash reports under `/var/lib/pocketos/log`
  survive reboots (S§10, S§13, S§16).
- Theme engine: live switch, mode switch, fallback for unknown theme and
  mode, reload at start (S§13).
- H1 measurement: Radar scan 17 % of the core by default, 6 % with reduced
  motion, 75 % with the vendor staging buffer off (S§14).
- Supervisor: radiod and shell each restarted about 1.1 to 1.2 s after a
  SIGSEGV, with crash reports and backtraces written; no crash-loop marker
  for single crashes (S§15, S§16).
- Network: Ethernet through the USB adapter, DHCP, root SSH; scp with the
  legacy protocol; NTP time sync once the cable was in (S§12, S§13).
- SX1262: register probe `14 24`, backend init with TCXO 3.3 V and DC-DC,
  RSSI, CAD, reception of Norwegian MeshCore traffic (58 packets, 0 CRC
  errors), MMCX1 identified as the antenna port by a controlled A/B test,
  one 8-byte transmit at 2 dBm with airtime 123.9 ms and immediate RX
  re-entry (S§18 to S§20).

## 3. What failed or misbehaved

- **The shell died when radiod was killed** (rc 141, SIGPIPE), and was
  brought back by its supervisor after 1 s (S§15). Root cause and fix in §7.
- **`pos-hwcheck --lora` hangs** on the image: `gpioset --hold-period` without
  `-z` waits for a terminal (S§18.2).
- The checklist's artifact table quoted a superseded image hash and an
  optimistic free-space figure (S§2.1, S§11.1).
- The bench scripts' assumptions broke twice on BusyBox (`gpioinfo` syntax,
  `date +%N`), costing evidence for the pass 1 per-process CPU number and the
  client-side blocking time of the first send (S§14, S§20). Not product
  defects, but the checklist should carry BusyBox-safe commands.
- The switch at the bench rebooted twice, taking Ethernet down each time;
  no board fault (S§12, S§15).

## 4. Confirmed bugs

| ID | Bug | Severity | Where | Status |
| --- | --- | --- | --- | --- |
| B1 | A radiod crash or restart kills the running shell with SIGPIPE; the designed transport-failure path never runs | HIGH (robustness), LOW (data) | `core/pocketipc/pocketipc.c` `write_all()` used `write(2)`; shell installs no SIGPIPE disposition | Fixed on `fix/pocketipc-sigpipe` (`feed274`), reviewed in §7, not merged |
| B2 | `pos-hwcheck --lora` hangs waiting for input; a killed probe leaves `gpioset -z` daemons holding GPIO44 and GPIO5 | LOW (bench tool), MEDIUM if it is the documented first LoRa probe | `tools/hwcheck/hwcheck.sh:174` | Open |
| B3 | `pos-hwcheck --lora` and radiod on the sx1262 backend can drive the same lines and the same spidev at once; the probe has no guard | MEDIUM (bench safety) | `tools/hwcheck/hwcheck.sh` | Open |
| B4 | The shell's `printf` diagnostics never reach `shell.stdio.log` (block-buffered stdout under the supervisor), so an empty stdio log is not evidence of a quiet start | LOW | `ui/shell/platform_drm.c`, `shell.c` printf lines | Open |
| B5 | `radio.send` blocks the shell UI for the airtime and a hung radiod hangs the shell (review H3) | HIGH at long airtimes, harmless at the bench profile | `ui/shell/shell_ipc.c`, `core/pocketipc/pocketipc.c` (blocking read, no timeout) | Known, open |

## 5. Hardware truth table (this unit, unit A)

| Item | Finding | Class | Evidence |
| --- | --- | --- | --- |
| SoC | Canaan K230, `Machine model: Canaan CanMV-K230 with RM69A10 OLED`; U-Boot `Model: kendryte k230 canmv v3` | VERIFIED | S§10.2, hwcheck report |
| Linux-visible CPUs | 1 hart, `uarch thead,c908`, `rv64imafdcv_zicbom_zicboz_zicntr_zicsr_zifencei_zihpm_zba_zbb_zbs_svpbmt`, sv39, mvendorid 0x5b7 | VERIFIED | hwcheck `cpuinfo`, `pos system info` |
| Second C908 core | T says dual C908 (1.6 GHz + 0.8 GHz); Linux sees one; nothing in U-Boot or dmesg mentions the other; whether it exists and runs RT-Smart on this image is not examined | UNRESOLVED (T's DOCUMENTED claim stands, not contradicted) | S§10.2, S§11.1 |
| RAM | U-Boot `DRAM: 1 GiB`; Linux MemTotal 990544 kB; the DTS memory node says 0x20000000 (512 MB) and U-Boot fixes it up at boot | VERIFIED (1 GB); the DTS/wiki conflict T records is resolved, the DTS value is a placeholder | S§10.2, S§12.2, dmesg `Memory: 465812K/1048576K` |
| CMA reservation | 524288K cma-reserved by the vendor DTS for the media blocks; counted in MemTotal, MemAvailable 931 MB at idle | VERIFIED | dmesg, S§12.2 |
| Display controller path | canaan-vo + canaan-mipi-dsi + canaan-panel-dsi, DRM `canaan-drm 1.0.0`, boot splash preserved, no fbdev | VERIFIED | dmesg, S§10.2 |
| Physical panel | `card0-DSI-1` connected, single mode `568x1232`; DSI 2 lanes, 24 bpp, `clock=49500` kHz; panel identity RM69A10 comes from the DTS/model string only | VERIFIED (mode and connector), DOCUMENTED (RM69A10 part) | S§10.1, S§10.2 |
| Touch controller | Goodix `goodix,nottingham` at I2C-1 0x5d, driver core v1.3.3, `event1 = goodix_ts`; DT range x 1060, y 2400; `goodix_cfg_group.bin` missing, driver continues | VERIFIED | dmesg, hwcheck, S§10.1; T says GT9895 (DOCUMENTED, consistent with `nottingham`) |
| Touch calibration/orientation | LVGL evdev auto-calibration from the driver's ABS ranges; portrait, status bar at top, nothing mirrored, taps land where expected in launcher, Fleet and Radar; no override used | VERIFIED (operator) | S§10.1, S§16.1 |
| DRM path in the shell | vendor-patched liblvgl (rotation patch 0002 unused at rotation 0, staging patch 0004) via `lv_linux_drm_create/set_file`; mode set logged by the DSI driver when the shell starts | VERIFIED | S§10.2, `platforms/k230/package`, BSP `package/lvgl/*/0002`, `0004` |
| DRM staging behaviour | `K230_LVGL_DRM_STAGING=1` (S90 default): Radar scan 17 % CPU, no flicker; `=0`: 75 % CPU, no flicker either | VERIFIED (numbers, operator for flicker); the cause of the 4.5x cost (rendering into uncached scanout memory) ASSUMED | S§14 |
| GPIO chips | gpiochip0 `9140b000.gpio` and gpiochip1 `9140c000.gpio`, 32 lines each; GPIO N = chip N/32 line N%32 as the HAL assumes | VERIFIED | hwcheck `gpiodetect` |
| GPIO consumers | chip0: 21 `reset` (output, undocumented, probably camera), 22 `dsi_reset`, 23 `ts_irq_gpio`, 24 `ts_reset_gpio`, 25 `backlight_gpio`; chip1: line 20 (GPIO52) bare output; GPIO45 (Wi-Fi enable per T) unclaimed | VERIFIED (state); identities of 21 and 52 ASSUMED; GPIO45 role UNRESOLVED | hwcheck `gpioinfo`, S§11.2 |
| SX1262 GPIO map | RST GPIO5 (output), BUSY GPIO19 (input), DIO1 GPIO20 (input, rising edge), power GPIO44 (chip1 line 12, output), all `consumer=radiod` while the backend runs | VERIFIED | S§18.3, S§20 |
| SX1262 SPI path | `/dev/spidev0.0` = `spi:dh2228fv` on `91584000.spi`, mode 0; manual read at 1 MHz and RadioLib at 4 MHz both work; the controller probes with `IRQ index 9 not found` (polling), no effect seen | VERIFIED (works); IRQ message consequence UNRESOLVED | S§18.2, S§18.3, dmesg |
| SX1262 init | `begin()` with TCXO 3.3 V on DIO3 and the DC-DC regulator, no DIO2 RF switch; state `rx` | VERIFIED on this module | S§18.3; `backend_sx1262.cpp:78-81, 115` |
| Module variant | 13A/16A/T89/16E footprint options; silkscreen not read; it answers as an SX126x with the SX1262 register defaults and transmits, consistent with SX1262 | ASSUMED (SX1262) | S§18.2, T "Board revision" |
| RSSI | instantaneous floor -93 to -95 dBm; MeshCore packets at -74 (ambient) and -15 to -29 dBm (node at a few metres) | VERIFIED | S§18.3, S§19.2 |
| CAD | receive-side CAD returns, `activity: false` on an idle channel, state back to `rx` | VERIFIED | S§18.3 |
| TX | one 8-byte packet, PocketOS default profile, 2 dBm, airtime 123.904 ms, rc 0 | VERIFIED (transmit completed); on-air reception by another node UNRESOLVED | S§20 |
| RX re-entry | `rx` immediately and 1 s after TX; after CAD; after every received packet (58 packets, 0 CRC errors) | VERIFIED | S§18.3, S§19.2, S§20 |
| DIO1 interrupt | edge events on GPIO20 through libgpiod delivered 58 packets to radiod's poll loop; TX-done seen by RadioLib's polling of DIO1 | VERIFIED | S§19, S§20 |
| Antenna / RF connector mapping | **MMCX1 = SX1262 antenna port**; MMCX2 not in the SX1262 path. Schematic V1.0 shows the module's ANT pin to RF3 (Hirose BWIPX-3-001E, IPEX class) and no MMCX at all | VERIFIED (mapping, controlled A/B); CONFLICTING between the V1.0 schematic (IPEX RF3) and the fitted MMCX connectors, resolved for practical purposes by the measurement; what MMCX2 feeds UNRESOLVED | S§18.1, S§19 |
| Wi-Fi | SDIO `mmc0:0001:1` vendor 0x024c device 0xf179, driver `rtl8189fs`, wlan0 and wlan1 (down); RTL8189FTV family; not brought up | VERIFIED (chip family, driver bound); DOCUMENTED (exact part); Wi-Fi function UNRESOLVED | hwcheck, S§11.1 |
| Ethernet adapter | Realtek 0bda:8152 `USB 10/100 LAN` on `r8152`, eth0, works with DHCP | VERIFIED | S§11.1, S§12 |
| Ethernet MAC behaviour | adapter EEPROM holds 00:00:00:00:00:00; kernel assigns a random locally administered MAC each boot; the DHCP lease can change every boot | VERIFIED | dmesg `Random ether addr`, S§12 |
| Camera | GC2093 at I2C-0 0x37 answers; vvcam enumerates it (video1..3); not exercised | VERIFIED (present), function UNRESOLVED | hwcheck, dmesg |
| HDMI bridge | a device answers at 0x3b on the touch bus; LT9611 uses 0x3b when its address pin is high; the image ships a separate HDMI DTB | ASSUMED (LT9611 fitted), UNRESOLVED (identity) | S§11.2 |
| Audio | ALSA card `K230_I2S_INNO`, playback and capture; not exercised | VERIFIED (present), function UNRESOLVED | hwcheck |
| Thermal | `canaan_thermal_zone` 48.6 C idle, 52 to 54 C during Radar passes | VERIFIED | S§11.1, S§14 |
| Watchdog | `/dev/watchdog0` (DW watchdog) present, nobody feeds it, kernel `watchdogd` thread only; PANIC_TIMEOUT 0 | VERIFIED (present, unfed) | hwcheck, review M9 |
| RTC | none (`/dev/rtc*` absent); clock starts at 1970 and jumps when NTP syncs over Ethernet | VERIFIED | hwcheck, S§13 |
| Battery / charger / base board | `power_supply` empty; AHT20 node on I2C-0 does not answer; no HCI; the nRF52840 base-board assumption of S§5 rests on a port diagram only | VERIFIED (nothing visible), base board UNRESOLVED | S§11.2 |
| Storage geometry | mmcblk1 61,440,000 blocks (58.6 GB card); p1 80 MB `/boot` ext4; p2 600 MB `/` ext4 at 128 MB; not grown to the card: the LILYGO BSP deletes the SDK's `S00resizemmc` and `/first_boot_flag` | VERIFIED | hwcheck, S§10.1; `k230_bsp/scripts/apply.sh` |
| Rootfs free space | 574 MB filesystem, 400.6 MB used, 131.6 MB free (75 %) | VERIFIED | hwcheck `df` |
| Persistence | `/` and `/boot` rw ext4; `/tmp`, `/run`, `/dev/shm` tmpfs (483.7 MB each); `/var/log` is a symlink into tmpfs; PocketOS logs and state under `/var/lib/pocketos` persist | VERIFIED | hwcheck `mounts`, S§10.1 |
| Serial ports | UART0 = console on COM9 (`USB-Enhanced-SERIAL-A CH342`), login and shell obtained; COM10 (`SERIAL-B`) is UART3 per docs, never opened; ttyS0..3 exist on the device | VERIFIED (COM9, ttyS nodes), DOCUMENTED (COM10 = UART3) | S§3, hwcheck |
| USB serial driver on Windows | no manual install; Windows fetched or staged WCH's own `CH343S64.SYS` 2.1.2025.7 on first connection; mechanism not established | VERIFIED (driver identity), UNRESOLVED (mechanism) | S§3.1 |
| Image on the card | `sysimage-sdcard.img` 763,363,328 bytes, SHA-256 `bafed837…3ad6`, built from `d092a2b` (= `3d6120a` after the email rewrite, identical tree); readback identical except the MBR disk signature | VERIFIED | S§2, `out/k230/BUILD_INFO.txt`, `SHA256SUMS.txt` |

Contradictions worth stating explicitly:
- DTS memory node 512 MB versus U-Boot and Linux 1 GiB: the DTS value is a
  placeholder that U-Boot fixes up; T should say 1 GB VERIFIED.
- Schematic V1.0 RF3 (IPEX class) versus two MMCX connectors on the unit:
  the unit is a later revision or uses pigtails; the mapping is measured,
  the wiring behind MMCX1 is assumed.
- T places the AHT20 on "I2C4 (SDA GPIO47, SCL GPIO46)" while the camera is
  on "I2C4 SDA GPIO49, SCL GPIO48": both are pinctrl groups of the same
  controller (BSP patch 0058), which Linux shows as `i2c-0`; not a conflict,
  but the wording invites one.
- The checklist's expectation of a first-boot `resize2fs` contradicts the
  BSP, which removes that script; the rootfs stays 600 MB.

## 6. Checklist corrections (proposed diff plan, not applied on master)

Applied on the docs branch (see §12 of the final report) where the evidence
is unambiguous; the rest is listed for a later edit.

Factual corrections:
- C§1 artifact table: hash `bafed837…3ad6`, gz `d0f33efb…1590`, commit
  `d092a2b` (pushed as `3d6120a`); drop the "(dirty)" paragraph; free space
  131 MB, not 175 MB; the vendor BSP removes `S00resizemmc` (already right).
- C§2 serial: Windows supplied WCH's CH343 driver automatically on this
  host; PuTTY at 115200 8N1 on COM9 (lower port) confirmed.
- C§3 rcS lines: the shell line comes before the launcher line; add the
  `S40k230_pocketos_defconfig` module loads and the sntp FAIL without network
  as expected noise.
- C§3 network: the USB LAN adapter has a random MAC per boot, so the IP must
  be re-read after every reboot (`ip -4 addr show eth0` on the console or
  the router's lease table); write the IP into the session notes.
- C§3 SSH: root login is accepted with an empty password on this image; the
  "set one" note becomes a numbered step before the key, and the key step
  becomes optional for the bench once a password exists.
- C§3 hwcheck: `pos-hwcheck --lora` hangs (B2); until fixed, use the manual
  probe sequence from S§18.2, and never run either while radiod is on the
  sx1262 backend (B3).
- C§4 logs: state that `shell.stdio.log` stays empty by design until B4 is
  fixed.

Structural changes:
- Move the `/var/lib` persistence probe, DRM mode read and evtest ranges
  before the panel hand-over (they were done that way in practice) and mark
  them read-only.
- Tag every step as READ-ONLY, TRANSIENT (service restart, `pos radio
  configure`, `/etc/init.d/... stop`) or PERSISTENT (`/etc/default/*`,
  `settings.conf`, `/var/lib/pocketos`), and say what restores it.
- C§5 radio: split into (a) register probe, (b) backend init, RSSI, CAD with
  the shell stopped, (c) RX of known traffic, (d) antenna mapping by
  controlled A/B (S§19 procedure), (e) TX only after the operator's antenna
  confirmation, first send at 2 dBm; state that CAD is receive-side; replace
  the "10 sends" row with a staged 1, then 10 at 2 dBm.
- C§11 H1: keep the three-pass structure; use `/proc/stat` and
  `/proc/<pid>/stat` deltas rather than `top -n 1` (which shows 0 % per
  process on BusyBox); BusyBox-safe commands only (`gpioinfo --chip`, no
  `date +%N`).
- C§13 crash tests: Stage 1 and 2 as done; Stage 3 (crash loop) marked
  "only on an image with the SIGPIPE fix" because each radiod exit currently
  restarts the shell too.
- C§12 radio with the shell running: same gate as Stage 3 (restarting radiod
  under the shell restarts the shell today).
- Operator visual confirmation rows: launcher orientation, tiles, mirroring,
  touch accuracy, theme change visible, panel back after a shell restart,
  reduced motion appearance, flicker with staging on and off, contact
  selection with the static sweep. Each needs a written YES/NO from the
  operator, as the session record now has.
- Redundant or over-complex: the evtest corner readings add little once the
  operator has played both apps (keep as OPTIONAL); the "SSH key on both
  units" step can wait for a second unit; the checklist's `kill -SEGV` via
  `/run/pocketos/<name>.pid` is right, the crash-loop loop should use
  `pkill -x radiod` as written but note that `pgrep` exists on the image
  (BusyBox) while `pkill -SEGV -x` also works.
- Safety wording: TX rows must carry the antenna precondition verbatim; the
  power increase after the first send needs a separate operator go.

## 7. SIGPIPE PR review: `feed274` on `fix/pocketipc-sigpipe`

Diff: `core/pocketipc/pocketipc.c` `write_all()` uses `send(fd, p, len,
MSG_NOSIGNAL)` instead of `write(fd, p, len)`, with a comment; a header
comment; `tests/pocketipc_test.c` gains `test_dead_peer`; `tests/shell_ipc_test.sh`
gains the kill-radiod scenario.

- Portability: `MSG_NOSIGNAL` is POSIX.1-2008 optional and present on Linux
  (glibc, musl) and the Xuantie glibc; absent on macOS. PocketOS builds only
  on Linux hosts (K230, WSL). Minor follow-up: a
  `#ifndef MSG_NOSIGNAL / #define MSG_NOSIGNAL 0` guard plus a note that a
  non-Linux host then needs `SIG_IGN`, so the file stays compilable elsewhere.
- Semantics: on a `SOCK_STREAM` socket `send(fd, buf, len, 0)` is `write()`;
  `MSG_NOSIGNAL` only suppresses the signal and leaves the `EPIPE` return.
  Partial writes, `EINTR` and `EAGAIN`/`EWOULDBLOCK` come back exactly as
  before, so the `continue`, the POLLOUT wait and the 200 ms deadline are
  untouched. `errno` is preserved: the function returns `-1` straight after a
  failing `send` without touching `errno` except for the deliberate
  `ETIMEDOUT`. Verified by reading the function; the stalled-peer and
  slow-peer tests still pass.
- Are all pocketipc writes sockets? Every fd pocketipc creates comes from
  `socket()` or `accept4()`; `pocketipc_write_frame()` is public and a caller
  could pass a pipe, which would now fail with `ENOTSOCK` instead of working.
  The header already says "connected socket"; no caller in the tree passes
  anything else (shell, radiod, `pos`, tests all use socketpairs or
  pocketipc connections). Acceptable.
- Other raw socket writes in the tree: none. The only `write()` calls
  outside pocketipc are pocketlog's stderr and log-file writes and the crash
  handler's `safe_write`, plus the stores' `fwrite` to files; none is a
  socket. `pos` writing to a closed stdout pipe would still die of SIGPIPE,
  which is normal CLI behaviour.
- radiod's `signal(SIGPIPE, SIG_IGN)` should stay: it costs nothing and also
  covers any future non-pocketipc socket in that process.
- Future apps and services: protected as long as they use pocketipc; an
  out-of-process app with its own socket code would need the same care. Worth
  one sentence in `docs/api/pocketipc.md` ("the library never raises SIGPIPE;
  do not rely on process-wide SIG_IGN").
- Shell reconnect after EPIPE: `shell_ipc_call` treats code 0 as transport
  failure, closes the fd and reconnects on the next call; the status poll
  runs every second, so the chip shows `--` within a second of radiod dying
  and `RX` within a second of it listening again. Adequate for the status
  bar. Not adequate, and unchanged by this PR, is the blocking read in
  `pocketipc_call` (B5): a radiod that is alive but wedged still freezes the
  shell.
- Tests: the unit test is deterministic on Linux (AF_UNIX stream, peer closed
  before the first write gives EPIPE at once). The integration scenario
  depends on two fixed sleeps (2.5 s each) and on radiod's `--verbose` DEBUG
  line naming `radio.status`; generous on a host, but a loaded CI box could
  need the second sleep raised. Minor.
- Verified on the branch: host suite 0 failures, the new unit test dies with
  rc 141 against master's `pocketipc.c` and passes with the fix, shell,
  Fleet and Radar shell tests green, riscv64 cross-compile clean (S§17.2 and
  the branch commit message).

**Verdict: MERGE READY WITH MINOR FOLLOW-UP** (portability guard, one
sentence in the IPC doc, optionally a longer second sleep in the scenario).
Not merged.

## 8. Additional code findings (cold review against the hardware evidence)

| ID | Finding | Severity | Evidence / location |
| --- | --- | --- | --- |
| F1 | Root SSH login with an empty password is accepted on the LAN (`PermitRootLogin yes`, `PermitEmptyPasswords yes`, empty root password; vendor sshd_config, no `BR2_TARGET_GENERIC_ROOT_PASSWD` in the PocketOS defconfig) | HIGH (security), bench-tolerable | S§12; `platforms/k230/configs/k230_pocketos_defconfig` |
| F2 | The vendor U-Boot default environment carries a Wi-Fi SSID and password (`wlanssid`, `wlanpass`) that `interfaces` feeds to wpa_supplicant for wlan0; wlan0 is not `auto`, so it is dormant, but the credentials ship in every image | MEDIUM (security hygiene) | `board/canaan/k230-soc/default.env`, `rootfs_overlay/etc/network/interfaces` |
| F3 | Random Ethernet MAC per boot: the r8152 EEPROM is blank, so every boot gets a new MAC and possibly a new lease; `deploy.sh` and `lora_pair_test.sh` take an IP argument and the docs assume it is stable | MEDIUM (bench friction), LOW (product) | dmesg `Random ether addr`, S§12 |
| F4 | `pos-hwcheck --lora` hangs and leaves `gpioset -z` daemons; no guard against running it while radiod holds the lines or against sharing spidev | MEDIUM | `tools/hwcheck/hwcheck.sh:171-184`, S§18.2 |
| F5 | The radio profile is not persisted: every radiod restart returns to 869.525 MHz, SF9, **14 dBm**; a bench session that lowered TX power to 2 dBm silently goes back to 14 dBm after a restart or reboot | MEDIUM (bench safety) | `services/radiod/main.c:797-804`, S§20 |
| F6 | `radio.send` is synchronous and `pocketipc_call` reads without a timeout: the shell freezes for the airtime and forever on a wedged radiod (review H3) | HIGH at long airtimes | `services/radiod/main.c:531`, `core/pocketipc/pocketipc.c:83-103` |
| F7 | Supervisor duration accounting uses wall-clock seconds: the NTP jump produced `after 1788807617s`, which also resets backoff and the restart window (a crash right after time sync always counts as a long run). Cosmetic today, wrong if the window logic ever matters during a time jump; `CLOCK_MONOTONIC` is not available to POSIX sh, `cut -d. -f1 /proc/uptime` is | LOW | S§15, `tools/supervise/pos-supervise:41-56` |
| F8 | pocketlog timestamps come from `CLOCK_REALTIME`: before NTP every boot logs 1970 and `pos logs` cannot order events across boots; crash report names collide only within a second | LOW (docs) | S§10.1, `core/pocketlog/pocketlog.c:112, 207` |
| F9 | Shell and radiod stdio logs: the shell's `printf` output is block-buffered under the supervisor and never flushed; `shell.stdio.log` is always empty and LVGL's `LV_LOG_PRINTF` warnings would be too until exit | LOW | `ui/shell/platform_drm.c:33, 158`, S§10.1 |
| F10 | Shutdown noise from vendor scripts: `S31canaan_isp` stop runs `vo_init: not found` and something tries to mount `/boot` during rcK (`Device or resource busy`); harmless, but it will be mistaken for a PocketOS fault on the console | DOCS ONLY | S§10.2 |
| F11 | Rootfs stays 600 MB on a 58.6 GB card with 131 MB free; PocketOS logs are capped (2 x 512 KB per process, stdio per boot) and app state is tiny, so no near-term risk, but crash reports accumulate without limit and the vendor launcher's media directories are on the same filesystem | LOW now, MEDIUM later | S§11.1; `core/pocketlog/pocketlog.c` (no crash-report pruning) |
| F12 | PocketFleet seeds from `time(NULL) ^ lv_tick_get()`: with no RTC and no network, boots start at the same epoch and the seed repeats within the same second window; Radar and Timber mix in a run counter, Fleet does not | LOW | `apps/fleet/fleet_app.c:144` |
| F13 | Settings and app-state persistence are correct on this filesystem; the stores still do not fsync the parent directory after rename (review M4), so a power cut right after a save can lose the newest file while keeping the previous one; the deliberate power-cut test has not been run | LOW, needs the destructive test | `apps/fleet/engine/fleet_store.c`, `apps/radar/radar_store.c` |
| F14 | GPIO ownership is correct (all four LoRa lines held by radiod, released on shutdown via `term()`), but nothing prevents a second process from opening `/dev/spidev0.0` concurrently; a `flock` on the node in the HAL would make B3 impossible | LOW (design) | `services/radiod/hal_linux.cpp:329` |
| F15 | Watchdog present and unfed; `CONFIG_PANIC_TIMEOUT=0`; a hang needs a power cycle (review M9). The vendor kernel opens nothing, so PocketOS is free to decide later | FUTURE | hwcheck, review |
| F16 | Nothing in PocketOS assumes two CPUs, 512 MB, a battery, a base board or audio output: the only readers are `pos system info` (`sysconf`), `pos hardware list` and hwcheck, all descriptive | none (checked) | grep over `ui services tools apps core` |
| F17 | Startup ordering is right (S60 radiod before S90 shell, launcher last and disabled); shutdown ordering is right (rcK stops S90 then S60). One gap: S90 waits up to 10 s for `/dev/dri/card0` but not for radiod's socket; the first status poll therefore shows `--` for a second on a slow boot. Cosmetic | LOW | S§10.2, `platforms/k230/rootfs_overlay/etc/init.d/S90pocketos-shell` |
| F18 | Crash-loop logic is sound for a single service (sixth exit in 60 s with 1-2-4-8-16 s backoff, marker written, supervision stops); with B1 unfixed a radiod loop also drives the shell's counter, which is why Stage 3 was held | none beyond B1 | `tools/supervise/pos-supervise`, S§17.1 |
| F19 | Vendor `S40network` runs `ifup eth0` with `udhcpc -b` and a 15 s wait-delay, so boot pauses briefly without a cable and the lease arrives later when a cable appears (observed). sshd starts before any address exists, harmless | DOCS ONLY | vendor `S40network`, S§12 |

Counts: BLOCKER 0, HIGH 2 (F1, F6), MEDIUM 4 (F2, F3, F4, F5), LOW 8 (F7,
F8, F9, F11, F12, F13, F14, F17), DOCS ONLY 2 (F10, F19), FUTURE 1 (F15),
checked-clean 2 (F16, F18).

## 9. Next-image fix plan

MUST FIX BEFORE NEXT IMAGE
- B1: merge `fix/pocketipc-sigpipe` (with the portability guard follow-up),
  so a radiod crash or restart no longer takes the shell down and Stage 3
  and C§12 become clean tests.
- B2/B3/F4: `pos-hwcheck --lora`: use `gpioset -z` (or `-t0`/`--hold-period`
  with `-z`) for the reset pulse so it never waits for a terminal, kill its
  daemons on every exit path, and refuse to run while radiod holds the lines
  (`gpioinfo` consumer check) or while `/etc/default/radiod` selects sx1262.
- F1: set a root password in the PocketOS defconfig
  (`BR2_TARGET_GENERIC_ROOT_PASSWD`) or at least turn off
  `PermitEmptyPasswords` in a PocketOS-owned sshd_config; document the bench
  password handling. Without this the next image is not safe on any shared
  LAN.
- Documentation corrections of §6 (hash, free space, MAC, SSH, hwcheck,
  antenna wording) so the checklist matches the hardware.

SHOULD FIX
- F5: persist the last accepted radio profile (or at least TX power) in
  `/etc/pocketos/radio.conf` through the documented settings path, or lower
  the default TX power to a bench-safe value until the pair test exists.
- F9: `setvbuf(stdout, NULL, _IOLBF, 0)` at shell start or route the
  platform diagnostics through pocketlog, so `shell.stdio.log` carries the
  DRM/evdev start messages.
- F7: supervisor timing from `/proc/uptime` instead of `date +%s`.
- F3: derive a stable MAC for the r8152 from the SoC (or set one in the
  vendor overlay's network script) so the lease stays put; until then the
  checklist carries the "re-read the IP" step.
- F2: drop the vendor's default `wlanssid`/`wlanpass` from the U-Boot
  environment the PocketOS image ships.
- F11: cap the number of crash reports pocketlog keeps.

DEFER
- Storage layout: growing the rootfs or adding a data partition (the
  600 MB root with 131 MB free is enough for the next sessions).
- Watchdog feeding and panic reboot policy (F15).
- Time handling beyond documentation: an RTC is absent; NTP over Ethernet
  works; a monotonic log clock is a design change.
- F6 asynchronous TX and IPC timeouts (design item, review H3).
- F12, F13, F14, F17 (low, design-level).
- sx1262 as the default backend: keep **opt-in** through
  `/etc/default/radiod` for the next image. Reasons: the antenna/TX safety
  rule needs an operator decision per unit, the profile is not persisted
  (F5), and the vendor launcher, which may still be switched back on for
  recovery, uses the same radio; making it default belongs with F5 and a
  documented TX-power policy.
- Crash-loop Stage 3: ready to run on the first image that contains B1; not
  before.

## 10. Remaining physical tests (validation debt)

USER HANDS REQUIRED
- Panel confirmation rows still open: panel content after the shell restarts
  in S§13, S§15, S§16 (blank of about 1.5 s expected).
- Antenna on MMCX1: rated band from its marking (taken as confirmed by "go
  tx", never written down).
- PCB silkscreen revision and LoRa module marking (T open item 1).
- evtest corner readings (OPTIONAL now, see below).
- Reduced-motion and theme-matrix photographs (C§10) for the design record.

SECOND RADIO NODE REQUIRED (PocketOS profile)
- Pair TX/RX with the PocketOS default profile (`tests/hw/lora_pair_test.sh`),
  which the MeshCore node cannot receive; needs a second PocketOS unit or a
  RadioLib node on the same profile.
- Reception of the K230's own transmit by another node (on-air proof).

LONG-RUN TEST
- Ten-send repetition at 2 dBm with per-send RX re-entry and stats, then a
  power increase only with a separate operator go.
- `gpioinfo` during TX (line states while transmitting).
- 10-minute thermal soak on the Radar scan screen with staging on.
- Shell RSS over 20 open/close cycles of each app (leak check, C§13 bullet).
- Wi-Fi bring-up (credentials typed by the operator, never stored by PocketOS).
- Radio with the shell running (C§12) and Stage 3 crash loop, both after the
  next image with B1.

DESTRUCTIVE / RECOVERY TEST
- Power cut during a Fleet match, then fsck state and save state (C§15).
- `poweroff` and power-key behaviour (C§14).
- Recovery path: hand the panel back to the vendor launcher and boot (C§17).
- Crash-loop Stage 3 (after B1), including `S60radiod restart` recovery.

OPTIONAL
- evtest corner readings (the operator's play-through already covers the
  mapping).
- HDMI bridge identity (chip-ID read at 0x3b, or boot the HDMI DTB).
- Camera capture through vvcam; audio playback through the INNO codec.
- Second C908 core: whether RT-Smart or anything runs on it under this image.
- What MMCX2 feeds.

## 11. Recommended order for the next session

1. Before touching the board: merge B1 and fix B2/B3 and F1, build the next
   image, verify it as the golden one was (BUILD_INFO commit, checksums,
   contents), set a root password.
2. Flash card 2 with the new image (keep the current golden card as the
   known-good fallback), boot with the console held, confirm rcS and the
   panel; read the new IP.
3. Stage 3 crash loop and C§12 (radio with the shell running) on the new
   image; confirm the status chip shows `--` and recovers without a shell
   restart.
4. Ten sends at 2 dBm with RX re-entry and `gpioinfo` during TX; then the
   power step only on an explicit go.
5. Thermal soak and the open/close leak check while the board is otherwise
   idle.
6. The destructive block last: poweroff, power key, power cut, recovery to
   the vendor launcher and back.
7. Pair test whenever a second PocketOS-profile node exists.

## 12. Golden image statement

The golden hardware bring-up image (`out/k230/sysimage-sdcard.img`,
763,363,328 bytes, SHA-256 `bafed837dca4d9279d05047e21c0c8513bee22af8bde184daaa2ebcb97123ad6`,
built from `d092a2b`, pushed as `3d6120a`) is unchanged by this review.
Master is unchanged. The physical unit was not touched, COM9 was not opened,
no transmission was made, nothing was deployed. The device was left as the
session ended: sx1262 backend selected in `/etc/default/radiod`, TX power
2 dBm in the running profile (which resets to 14 dBm on the next radiod
restart, F5), PocketOS shell stopped with its enable file intact.
