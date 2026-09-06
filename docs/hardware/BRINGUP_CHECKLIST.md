# First physical K230 session: operational checklist

One page for the bench. Tick each line and record the result (photo, log
excerpt or number) in a `hwcheck-unit<X>/` folder; the evidence class to
promote is given per item (V = VERIFIED on the device once ticked).
Background and recovery detail: FIRST_BOOT.md, platforms/k230/README.md.
No screenshots on the device (decision H2, option B): photograph the panel.

## 1. Artifact

| | |
| --- | --- |
| Image | `out/k230/sysimage-sdcard.img` (763,363,328 bytes; `.gz` is the same image compressed) |
| PocketOS commit | `7ede1cf` (master), VERSION 0.0.1 |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` |
| SHA-256 of the image | `c227ce03b93e2b79388cc35effc16965c35fb6bc6fa2ac2cefa268ceda92dc19` |
| SHA-256 of the .gz | `4c30d7d55034b0f0086c2f531e5fb3459c1cc46fd14275746bcd3611b4cda84d` |
| Contents (verified from the image) | pocketos-shell with PocketFleet and PocketRadar, radiod (mock + sx1262), pos, pos-supervise, pos-hwcheck, vendor launcher at /root/app/k230_phone_ui |

Check before flashing: `sha256sum out/k230/sysimage-sdcard.img` matches
`out/k230/SHA256SUMS.txt`; `out/k230/BUILD_INFO.txt` says `PocketOS : 7ede1cf`.

## 2. Flash

- [ ] Card 1: vendor image `out/vendor/sysimage-sdcard.img` (reference and recovery).
- [ ] Card 2: `out/k230/sysimage-sdcard.img`, written raw with Rufus (DD mode)
      or balenaEtcher to a microSD of 8 GB or more. WSL cannot write USB readers.
- [ ] Serial: USB-C "UART" port, PuTTY 115200 8N1 on the lower of the two
      CH342K COM ports (the other is UART3). Install the WCH CH343SER driver
      if no COM ports appear.

## 3. First power-on (card 2) — expectations

- [ ] Serial within ~2 s: U-Boot banner, `bootdelay=1` countdown, kernel. (V: boot chain)
- [ ] rcS lines: `Starting radiod (mock, EU868): OK`,
      `Starting pocketos-shell: disabled (/etc/default/pocketos-shell)`,
      `Starting k230_phone_ui: OK`. Vendor launcher appears on the panel. (V: init order)
- [ ] `S00resizemmc` grows the root partition on the first boot only (expect one
      `resize2fs` message); the second boot must not repeat it.
- [ ] Login on serial as root, then `pos system info`, `pos hardware list`,
      `pos radio info` (backend `mock`), `pos logs`. (V: PocketOS services)
- [ ] Network for SSH: Ethernet (RTL8152B, `eth0`, DHCP) or Wi-Fi via the
      vendor launcher; `pos network interfaces`. `ssh root@<ip>` (no password on
      the vendor rootfs: set one).
- [ ] `pos-hwcheck` then `pos-hwcheck --lora` (stops the launcher; register read
      must show `14 24`). Copy `/root/hwcheck/` off the device. (V: RAM size,
      device nodes, input names, DRM modes, thermal zone, LoRa SPI)
- [ ] `date`: note whether the clock is sane (affects PocketFleet seeding only).

## 4. Logs

- Directory `/var/lib/pocketos/log` (persistent ext4; `/var/log` is tmpfs).
- `pos logs` (list), `pos logs radiod -n 100`, `pos logs shell`, `pos logs --crashes`,
  `pos logs --crash <file>`; supervisor: `supervise-radiod.log`, `supervise-pocketos-shell.log`;
  raw stdio: `shell.stdio.log` (LVGL messages; previous boot in `.1`).
- Crash-loop marker: `/run/pocketos/<name>.crashloop` (after 5 restarts in 60 s;
  nothing displays it, check by hand).
- [ ] After the first reboot, `pos logs radiod` still shows the previous boot. (V: log persistence)

## 5. Vendor launcher off, PocketOS shell on (persistent)

```sh
echo ENABLE=0 > /etc/default/k230_phone_ui
echo ENABLE=1 > /etc/default/pocketos-shell
reboot
```

- [ ] rcS now prints `Starting k230_phone_ui: disabled (/etc/default/k230_phone_ui)`
      and `Starting pocketos-shell: OK`. Panel shows the PocketOS launcher
      (status bar, four tiles). (V: shell owns DRM)
- [ ] `pos app list` shows radio, system, fleet, radar; `pos shell info`.
- [ ] Without reboot the same is `/etc/init.d/S99zz_k230_phone_ui stop` then
      `/etc/init.d/S90pocketos-shell start`; S90 refuses while the launcher is
      enabled or running (expected message, not a fault).

Re-enable the vendor launcher (reverse, nothing is deleted):

```sh
echo ENABLE=1 > /etc/default/k230_phone_ui
echo ENABLE=0 > /etc/default/pocketos-shell
rm -f /etc/default/radiod
reboot
```

## 6. Display and touch

- [ ] Photograph the launcher: status bar 56 px high, tiles, no rotation or
      mirroring, no tearing while idle. `shell.stdio.log` has no `DRM` or
      `drm` error lines. (V: DRM mode 568x1232, staging on)
- [ ] `evtest /dev/input/eventN` (Goodix device from hwcheck): ABS_MT_POSITION_X
      0..1060, ABS_MT_POSITION_Y 0..2400, ABS_X/ABS_Y present. Touch the four
      corners and note the raw values. (V: touch ranges)
- [ ] Tap each tile: app opens; the back slab (top-left) returns home. Tap in
      all four corners of the panel and in the centre: the intended element
      reacts, nothing mirrored or swapped. (V: touch mapping)
- [ ] Radio app: scroll the body with a finger; no stuck press.
- [ ] Note any flicker while scrolling. If present, retry once with
      `K230_LVGL_DRM_STAGING=0` in `/etc/default/pocketos-shell` and record both.

## 7. Settings and persistence

- [ ] `pos shell theme brass outdoor` changes the panel live; `cat
      /etc/pocketos/settings.conf` shows `theme=brass`, `display_mode=outdoor`.
- [ ] `reboot`: the shell comes back in brass/outdoor. (V: settings persistence)
- [ ] `pos shell theme neon`: the panel falls back to ice with a fallback
      message, and the stored value stays brass (DS section 8).
- [ ] `touch /var/lib/pocketos/probe && sync && reboot`; the file is still there.
      (V: /var/lib writable and persistent)

## 8. PocketFleet

- [ ] `pos app start fleet` or tap the tile: Command screen, status hint COMMAND.
- [ ] Pick a difficulty segment (56 px targets: note whether they are hit
      reliably), DEPLOY FLEET, place ships by tapping 48 px cells, TURN / AUTO /
      CLEAR, CONFIRM DEPLOYMENT.
- [ ] Battle: tap a target cell then FIRE, at least ten exchanges; the sweep
      animation runs; note any lag when the reply lands (per-turn fsync).
- [ ] Back to home mid-match, reopen: RESUME is offered and restores the board.
- [ ] `reboot`, reopen: the match still resumes. (V: /var/lib/pocketos/fleet/save.v1)
- [ ] Finish or abandon; `pos logs shell` has no ERROR lines from fleet.

## 9. PocketRadar

- [ ] `pos app start radar` or tap the tile: BEGIN SCAN. Contacts appear and move;
      the sweep rotates.
- [ ] Tap a contact: brackets appear on that contact (not a mirrored position);
      ENGAGE after acquisition. Try contacts in all four quadrants. (V: C1 fix
      on real touch)
- [ ] Try a tap with a slight finger slide: note whether it selects or scrolls
      the body (review item M6).
- [ ] Let a run finish: RUN COMPLETE, result screen, NEW RUN. `reboot`, reopen:
      the best score is shown. (V: /var/lib/pocketos/radar/record.v1)

## 10. Reduced motion and themes

- [ ] Stop the shell (`/etc/init.d/S90pocketos-shell stop`), append
      `reduced_motion=1` to `/etc/pocketos/settings.conf`, start it. Radar: sweep
      is a static line, no bursts, contacts still move. Fleet: no sweep on the
      grids, replies land without pacing. Remove the line afterwards.
- [ ] Theme matrix, photographed with the radio app open and with radar scan
      open: ice, brass, olive, slate, carbon x normal, outdoor, night
      (`pos shell theme <id> <mode>`). Check hairlines in outdoor (1 px), text
      legibility, chip colours; custom-drawn grids and scope follow the switch
      without reopening the app. (V: DS H1/H2 items, AMOLED legibility)

## 11. PocketRadar H1: scope repaint cost (measure, do not tune)

With a run active on the scan screen, over 60 s:

- [ ] `top -d 5` (BusyBox): CPU % of `pocketos-shell`; record three samples.
- [ ] `cat /proc/loadavg` before and after.
- [ ] `/sys/class/thermal/thermal_zone0/temp` before, after 10 minutes of scan.
- [ ] Subjective: does the sweep stutter, does the tick rate hold (a run's
      timing feels uniform; slower means the 50 ms tick is being missed).
- [ ] Repeat once with `reduced_motion=1`.
Record the numbers in KNOWN_ISSUES.md under H1. No design change from this
session.

## 12. SX1262 (launcher must be off; unit A first, then unit B)

```sh
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
/etc/init.d/S60radiod restart
pos radio info        # backend sx1262, chip sx1262
pos radio status      # state rx
pos logs radiod -n 20
```

- [ ] Init: `radio.info` shows sx1262; log has no `begin failed`. If it fails,
      record the RadioLib code and the TCXO/regulator assumption in
      KNOWN_ISSUES.md; try `export POCKETOS_SX1262_TCXO_MV=0` in
      `/etc/default/radiod` only as a diagnostic. (V: SPI, RST/BUSY/DIO1/power pins, TCXO)
- [ ] RSSI: `pos radio rssi` returns a plausible noise floor (about -100 dBm or
      lower). CAD: `pos radio cad` returns without error.
- [ ] TX: `pos radio send 506f636b65744f53`; result has `airtime_ms`; afterwards
      `pos radio status` is `rx` again within 1 s (re-entry). Repeat 10 times;
      `pos radio stats` counts 10. (V: TX, RX re-entry)
- [ ] While a send runs, expect the shell UI to pause for the airtime and the
      status chip to show RX again afterwards (known, review H3). Note the
      duration.
- [ ] RX: unit B `pos radio send ...`, unit A `pos radio listen 10` prints a
      `radio.rx` event with payload, RSSI and SNR. Then
      `tests/hw/lora_pair_test.sh <ip-A> <ip-B>` from the PC. (V: link)
- [ ] Error path: `pos radio status` must never stay `error`; if it does, `pos
      logs radiod` shows once-per-second recovery attempts. Do not force it by
      pulling power lines.
- [ ] DIO1: `gpioinfo | grep radiod` shows GPIO 20 held by radiod with edge
      detection while the backend is up.

## 13. Reboot and shutdown

- [ ] `reboot` from SSH: rcK stops S90 then S60 (serial shows both), the board
      comes back with the shell and the same theme; no fsck messages.
- [ ] `poweroff`: the board halts; power-cycle boots cleanly.
- [ ] Power key: record what a short and a long press do (PMU behaviour is
      undocumented for PocketOS; nothing handles the key yet).
- [ ] One deliberate power cut during a Fleet match, then boot: the filesystem
      recovers, the save is either the last turn or the one before, never corrupt.

## 14. Crash handling

- [ ] `kill -SEGV $(cat /run/pocketos/radiod.pid)`: `pos logs --crashes` lists a
      report with a backtrace; `pos radio info` works again within 2 s; status
      chip recovers. (V: crash reports, supervisor)
- [ ] Same for the shell (`/run/pocketos/pocketos-shell.pid`): the panel comes
      back within a few seconds with the stored theme.
- [ ] Five kills within 60 s: `/run/pocketos/radiod.crashloop` appears, supervision
      stops; `/etc/init.d/S60radiod restart` recovers.

## 15. If the PocketOS shell fails

1. Serial console (always available on UART0) or SSH: `pos logs shell`,
   `cat /var/lib/pocketos/log/shell.stdio.log`, `pos logs --crashes`.
2. Hand the panel back: `echo ENABLE=1 > /etc/default/k230_phone_ui`,
   `echo ENABLE=0 > /etc/default/pocketos-shell`, `reboot` (vendor launcher
   returns; radiod stays on the mock backend unless `/etc/default/radiod` says
   otherwise, so remove that file too when the launcher needs the radio).
3. If the board does not reach a login: press a key on the serial console
   within a second of power-on to stop in U-Boot (`bootdelay=1`); `run blinux`
   resumes. Otherwise swap in card 1 (vendor image) and re-flash card 2 from
   `out/k230/sysimage-sdcard.img`. Nothing is stored on the board itself.
4. File every failure in KNOWN_ISSUES.md with the log excerpt and the photo,
   and promote or demote the evidence class in T-DISPLAY-K230.md.
