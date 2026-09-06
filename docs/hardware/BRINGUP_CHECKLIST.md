# First physical K230 session: operational checklist

One page for the bench. Tick each line and record the result (photo, log
excerpt or number) in a `hwcheck-unit<X>/` folder; the evidence class to
promote is given per item (V = VERIFIED on the device once ticked).
Background and recovery detail: FIRST_BOOT.md, platforms/k230/README.md.
No screenshots on the device (decision H2, option B): photograph the panel.
The order below is the bench order: everything non-destructive first, the
radio with the shell stopped, then the shell, crash tests, and the power cut
last.

## 1. Artifact

| | |
| --- | --- |
| Image | `out/k230/sysimage-sdcard.img` (763,363,328 bytes; `.gz` is the same image compressed) |
| Built from | PocketOS commit `7b181fa` (the last code commit), VERSION 0.0.1 |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` |
| SHA-256 of the image | `098e88b1dab4367ec6ca49b0db9e67407d1d251c6abeec5527f1075fc640d74f` |
| SHA-256 of the .gz | `7388ed7e72ccd519b2156c8558b065a36a0ea296bb29ce635ada2244efb1ac87` |
| Contents (verified from the image) | pocketos-shell with PocketFleet and PocketRadar, radiod (mock + sx1262), pos, pos-supervise, pos-hwcheck, vendor launcher at /root/app/k230_phone_ui |

Provenance: `out/k230/BUILD_INFO.txt` names the commit the image was built
from. Master may be ahead of it by documentation-only commits; `docs/` is not
part of the image (apply_to_sdk.sh excludes it), so
`git diff 7b181fa..master --stat` must list only files under `docs/`
for the image to be current. Any other file means rebuild.

Check before flashing: `sha256sum out/k230/sysimage-sdcard.img` matches
`out/k230/SHA256SUMS.txt`. BUILD_INFO.txt for this image reads
`PocketOS : 7b181fa (dirty)`: the source was synced from a clean `7b181fa`
checkout; the only files edited while the build ran were this checklist
and platforms/k230/README.md, neither of which is part of the image.

The root filesystem is a 600 MB ext4 partition (rootfs at 128 MB into the
card, boot partition at 30 MB) and stays 600 MB whatever the card size: the
LILYGO BSP removes the Kendryte SDK's first-boot resize (`S00resizemmc` and
`/first_boot_flag`, `k230_bsp/scripts/apply.sh`), and neither is in the
image (VERIFIED). About 175 MB are free; enough for logs and app state.

## 2. Flash

- [ ] Card 1: vendor image `out/vendor/sysimage-sdcard.img` (reference and recovery).
- [ ] Card 2: `out/k230/sysimage-sdcard.img`, written raw with Rufus (DD mode)
      or balenaEtcher to a microSD of 8 GB or more. WSL cannot write USB readers.
- [ ] Serial: USB-C "UART" port, PuTTY 115200 8N1 on the lower of the two
      CH342K COM ports (the other is UART3). Install the WCH CH343SER driver
      if no COM ports appear.

## 3. First power-on (card 2, vendor launcher still on)

- [ ] Serial within ~2 s: U-Boot banner, `bootdelay=1` countdown, kernel. (V: boot chain)
- [ ] rcS lines: `Starting radiod (mock, EU868): OK`,
      `Starting pocketos-shell: disabled (/etc/default/pocketos-shell)`,
      `Starting k230_phone_ui: OK`. Vendor launcher appears on the panel. (V: init order)
- [ ] Login on serial as root, then `pos system info`, `pos hardware list`,
      `pos radio info` (backend `mock`), `pos logs`. (V: PocketOS services)
- [ ] Network for SSH: Ethernet (RTL8152B, `eth0`, DHCP) or Wi-Fi via the
      vendor launcher; `pos network interfaces`. `ssh root@<ip>` (no password on
      the vendor rootfs: set one).
- [ ] SSH key on the unit (needed later by deploy.sh and lora_pair_test.sh,
      both use `BatchMode=yes`). From the PC (PowerShell):
      `type $HOME\.ssh\id_ed25519.pub | ssh root@<ip> "mkdir -p /root/.ssh && cat >> /root/.ssh/authorized_keys && chmod 700 /root/.ssh && chmod 600 /root/.ssh/authorized_keys"`,
      then `ssh -o BatchMode=yes root@<ip> true` must succeed silently. Do it
      on both units.
- [ ] Storage: `cat /proc/mounts` shows `/` ext4 rw and `/tmp` tmpfs; `df -h /`
      about 600 MB; `touch /var/lib/pocketos/probe && sync && reboot`; after the
      reboot the file is still there and `pos logs radiod` shows the previous
      boot. (V: /var/lib writable and persistent, log persistence)
- [ ] `pos-hwcheck` then `pos-hwcheck --lora` (stops the launcher; register read
      must show `14 24`; it does not restart the launcher: `/etc/init.d/S99zz_k230_phone_ui start`).
      Copy `/root/hwcheck/` off the device. (V: RAM size, device nodes, input
      names, thermal zone, LoRa SPI)
- [ ] DRM, independent of PocketOS: `cat /sys/class/drm/card0-*/status` is
      `connected` and `cat /sys/class/drm/card0-*/modes` lists `568x1232`.
      Record the exact mode line. (V: panel mode)
- [ ] Touch ranges, independent of PocketOS: `evtest /dev/input/eventN` (the
      Goodix device named by hwcheck): ABS_MT_POSITION_X 0..1060,
      ABS_MT_POSITION_Y 0..2400, ABS_X/ABS_Y present with the same ranges.
      Touch the four corners and note which raw values each corner gives
      (this decides whether section 6 needs a calibration override). (V: touch ranges)
- [ ] `date`: note whether the clock is sane (affects PocketFleet seeding only).

## 4. Logs

- Directory `/var/lib/pocketos/log` (persistent ext4; `/var/log` is tmpfs).
- `pos logs` (list), `pos logs radiod -n 100`, `pos logs shell`, `pos logs --crashes`,
  `pos logs --crash <file>`; supervisor: `supervise-radiod.log`, `supervise-pocketos-shell.log`;
  raw stdio: `shell.stdio.log` (LVGL messages; previous boot in `.1`).
- Crash-loop marker: `/run/pocketos/<name>.crashloop`, written when the sixth
  exit happens within 60 s (pos-supervise allows five restarts); nothing
  displays it, check by hand.

## 5. SX1262 with the PocketOS shell stopped (unit A first, then B)

Take the panel away from the vendor launcher but leave the shell disabled,
so only radiod touches the radio:

```sh
echo ENABLE=0 > /etc/default/k230_phone_ui
echo RADIOD_BACKEND=sx1262 > /etc/default/radiod
reboot
```

- [ ] rcS: `Starting k230_phone_ui: disabled (/etc/default/k230_phone_ui)`,
      `Starting radiod (sx1262, EU868): OK`, shell still disabled. The panel
      shows the boot logo only.
- [ ] Init: `pos radio info` shows backend `sx1262`; `pos radio status` is
      `rx`; `pos logs radiod -n 20` has no `begin failed`. If init fails, record
      the RadioLib code in KNOWN_ISSUES.md. As a diagnostic only, try the
      crystal setting with the full file
      `printf 'RADIOD_BACKEND=sx1262\nexport POCKETOS_SX1262_TCXO_MV=0\n' > /etc/default/radiod`
      and `/etc/init.d/S60radiod restart`; restore the one-line file afterwards.
      (V: SPI, RST/BUSY/DIO1/power pins, TCXO)
- [ ] `pos radio rssi` returns a plausible noise floor (about -100 dBm or lower);
      `pos radio cad` returns without error.
- [ ] TX: `pos radio send 506f636b65744f53`; result has `airtime_ms`; then
      `pos radio status` is `rx` again within 1 s (re-entry). Repeat 10 times;
      `pos radio stats` counts 10 and the duty cycle is under 1 %. (V: TX, RX re-entry)
- [ ] `gpioinfo | grep radiod` shows GPIO 20 held by radiod (DIO1 edge input),
      GPIO 5, 19 and 44 held as well.
- [ ] Error path: `pos radio status` must never stay `error`; if it does,
      `pos logs radiod` shows once-per-second recovery attempts. Do not force
      it by pulling power lines.
- [ ] Both units done: from the PC `tests/hw/lora_pair_test.sh <ip-A> <ip-B>`
      (needs the SSH key from section 3 on both). A prints the payload with RSSI
      and SNR; record both numbers. (V: link)

## 6. PocketOS shell on (persistent)

```sh
printf 'ENABLE=1\n' > /etc/default/pocketos-shell
reboot
```

- [ ] rcS: `Starting pocketos-shell: OK`. Panel shows the PocketOS launcher
      (status bar, four tiles). (V: shell owns DRM)
- [ ] `pos app list` shows radio, system, fleet, radar. `pos shell info` reports
      the compile-time PocketOS geometry (568 x 1232) and backend `drm`; it is
      not DRM evidence, section 3 is.
- [ ] Photograph the launcher: status bar 56 px high, tiles, no rotation or
      mirroring, no tearing while idle. `shell.stdio.log` has no `DRM` or `drm`
      error lines. (V: DRM output through the vendor-patched LVGL, staging on)
- [ ] Tap each tile: app opens; the back slab (top-left) returns home. Tap in
      all four corners of the panel and in the centre: the intended element
      reacts, nothing mirrored or swapped. (V: touch mapping)
- [ ] Radio app: scroll the body with a finger; no stuck press.
- [ ] Note any flicker while scrolling (compared again in section 11).

Escape hatches if the picture or the touch mapping is wrong. All are read
by the shell from `/etc/default/pocketos-shell` at start (S90 exports them);
always write the whole file with `ENABLE=1` in it, then
`/etc/init.d/S90pocketos-shell restart`. An invalid value is logged as a
WARN in `pos logs shell` and the default stays.

```sh
# picture upside down: DRM plane rotation (0, 90, 180, 270; vendor LVGL patch).
# 90 and 270 swap the framebuffer to 1232x568, which the layout does not follow: diagnostic only.
printf 'ENABLE=1\nPOCKETOS_DRM_ROTATION=180\n' > /etc/default/pocketos-shell
# touch lands in the wrong place: raw range to map onto the panel, and/or swapped axes.
# The numbers come from the evtest corners in section 3 (inverted ranges are allowed, e.g. 1060,0,0,2400).
printf 'ENABLE=1\nPOCKETOS_TOUCH_CALIB=0,0,1060,2400\n' > /etc/default/pocketos-shell
printf 'ENABLE=1\nPOCKETOS_TOUCH_SWAP=1\n' > /etc/default/pocketos-shell
# a specific touch node instead of discovery, or another DRM node
printf 'ENABLE=1\nPOCKETOS_TOUCH_DEVICE=/dev/input/event1\n' > /etc/default/pocketos-shell
printf 'ENABLE=1\nPOCKETOS_DRM_DEVICE=/dev/dri/card0\n' > /etc/default/pocketos-shell
```

Combine lines as needed in one file. `pos logs shell` confirms what was
applied (`touch calibration ...`, `touch axes swapped`, `DRM plane rotation`).
Record any override that was needed in KNOWN_ISSUES.md: it is a finding
about the board, not a setting to keep silently.

## 7. Settings

- [ ] `pos shell theme brass outdoor` changes the panel live; `cat
      /etc/pocketos/settings.conf` shows `theme=brass`, `display_mode=outdoor`.
- [ ] `reboot`: the shell comes back in brass/outdoor. (V: settings persistence)
- [ ] `pos shell theme neon`: the panel falls back to ice with a fallback
      message, and the stored value stays brass (DS section 8).

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
      the body (review item M6, not to be fixed this session).
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

With a run active on the scan screen, over 60 s each, four runs:
staging on (default) and off, each with reduced motion off and on.

- [ ] Staging off is `printf 'ENABLE=1\nK230_LVGL_DRM_STAGING=0\n' > /etc/default/pocketos-shell`
      and `/etc/init.d/S90pocketos-shell restart`; restore with
      `printf 'ENABLE=1\n' > /etc/default/pocketos-shell` and another restart.
- [ ] `top -d 5` (BusyBox): CPU % of `pocketos-shell`; record three samples per run.
- [ ] `cat /proc/loadavg` before and after each run.
- [ ] `/sys/class/thermal/thermal_zone0/temp` before, after 10 minutes of scan (staging on only).
- [ ] Subjective per run: stutter, flicker while the scope repaints, whether
      the tick rate holds (a run's timing feels uniform; slower means the 50 ms
      tick is being missed).
Record the numbers in KNOWN_ISSUES.md under H1. No design change and no
staging default change from this session.

## 12. Radio with the shell running

- [ ] `pos radio send 506f636b65744f53` while watching the panel: expect the UI
      to pause for the airtime and the status chip to read RX afterwards
      (known, review H3). Note the duration.
- [ ] Unit B sends, unit A `pos radio listen 10` prints the `radio.rx` event;
      the radio app on A shows the packet count and last RSSI.

## 13. Crash handling (before the power cut)

- [ ] Single crash: `kill -SEGV $(cat /run/pocketos/radiod.pid)`. Within 2 s
      `pos radio info` works again, `pos logs --crashes` lists a report with a
      backtrace, `supervise-radiod.log` says `exited rc=139 ... restart 1`. (V:
      crash reports, supervisor)
- [ ] Same for the shell (`/run/pocketos/pocketos-shell.pid`): the panel comes
      back within a few seconds with the stored theme.
- [ ] Crash loop: pos-supervise restarts after 1, 2, 4, 8 and 16 s and gives up
      on the sixth exit inside 60 s. Executable form, about 35 s in total:

```sh
for i in 1 2 3 4 5 6; do
    until pgrep -x radiod >/dev/null; do sleep 0.2; done
    pkill -SEGV -x radiod; sleep 1
done
sleep 2; cat /run/pocketos/radiod.crashloop; tail -3 /var/lib/pocketos/log/supervise-radiod.log
```

      Expect the marker with `restarts=6` and a `crash loop` log line; `pos
      radio info` fails. Recover with `/etc/init.d/S60radiod restart` (the
      marker is removed on the next supervisor start).

## 14. Reboot and shutdown

- [ ] `reboot` from SSH: rcK stops S90 then S60 (serial shows both), the board
      comes back with the shell and the same theme; no fsck messages.
- [ ] `poweroff`: the board halts; power-cycle boots cleanly.
- [ ] Power key: record what a short and a long press do (PMU behaviour is
      undocumented for PocketOS; nothing handles the key yet).

## 15. Power cut (last, destructive)

- [ ] One deliberate power cut during a Fleet match, then boot: the filesystem
      recovers (note any journal messages on serial), the save is either the
      last turn or the one before, never corrupt; the theme is intact.

## 16. Fast bench fix path (no reflash)

For a code fix during the session, from the WSL checkout (the vendor build
tree must exist from the image build):

```sh
platforms/k230/scripts/apply_to_sdk.sh                       # re-sync the source into the package
platforms/k230/scripts/build_image.sh "$POCKETOS_VENDOR_DIR" pocketos-rebuild   # package only, no image
platforms/k230/scripts/deploy.sh <ip>                        # push binaries + init scripts over SSH, restart
```

deploy.sh needs the SSH key from section 3, leaves `/etc/default/*` and
`/etc/pocketos` alone, and restarts S60 and S90 (S90 still respects the panel
switch). apply_to_sdk.sh removes the vendor launcher from the build tree's
target directory; a later full `build_image.sh` rebuilds it, a
`pocketos-rebuild` does not need it. Record the commit deployed in the
session notes: the flashed card and the running binaries then differ.

## 17. If the PocketOS shell fails

1. Serial console (always available on UART0) or SSH: `pos logs shell`,
   `cat /var/lib/pocketos/log/shell.stdio.log`, `pos logs --crashes`.
2. Try the escape hatches in section 6 first (rotation, calibration, device).
3. Hand the panel back: `echo ENABLE=1 > /etc/default/k230_phone_ui`,
   `printf 'ENABLE=0\n' > /etc/default/pocketos-shell`, `rm -f /etc/default/radiod`
   (the launcher needs the radio and the mock backend must return), `reboot`.
4. If the board does not reach a login: press a key on the serial console
   within a second of power-on to stop in U-Boot (`bootdelay=1`); `run blinux`
   resumes. Otherwise swap in card 1 (vendor image) and re-flash card 2 from
   `out/k230/sysimage-sdcard.img`. Nothing is stored on the board itself.
5. File every failure in KNOWN_ISSUES.md with the log excerpt and the photo,
   and promote or demote the evidence class in T-DISPLAY-K230.md.
