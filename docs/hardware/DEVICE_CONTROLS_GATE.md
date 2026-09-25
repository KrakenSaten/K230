# Device controls and diagnostics: unit A gate

Branch `feat/device-controls-diagnostics`. Status: **RUN 2026-09-25 on unit A,
PASS**, including the owner's listening check of the volume steps; DS §31.5
**ACCEPTED** by the owner as it is (results at the foot). Unit A carries build **`09be665`** (Doors 0.0.13,
this branch rebased on master `d2b2be6`, deployed with `deploy.sh` over the
flashed v0.0.12 card), rotation **Automatic**, radio **on** (stored), volume
100 %. Rollback of the userspace and state it replaced (v0.0.12 `a8b1a9f` plus
the Camera shell `e3d3f71`): `/root/rollback-devctl/RESTORE.sh`.

The branch was written in a cloud container with no unit, no Buildroot tree
and no LVGL; the section below is that session's plan. The steps were run as
written except where the results say otherwise; unit A answers on both
192.168.10.157 (eth0) and 192.168.10.171 (Wi-Fi) since the v0.0.12 flash.

Evidence classes as in AGENTS.md: VERIFIED (observed on unit A), DOCUMENTED
(repository or vendor source), ASSUMED.

## What the branch changes on the device

The evidence column is as written before the gate; every DEVICE UNVERIFIED
in it is now VERIFIED by the results at the foot.

| Area | Change | Hardware path | Evidence |
| --- | --- | --- | --- |
| LoRa radio (SX1262) | `radio.set_enabled`; the choice kept in `/var/lib/pocketos/radiod/radio.conf`; **off when nothing is stored** on the sx1262 backend; off = the existing shutdown (sleep, GPIO44 low, SPI and GPIO released); a start while off parks the chip (init, configure, shutdown) | RST GPIO5, BUSY GPIO19, DIO1 GPIO20, power enable GPIO44 (T-DISPLAY-K230.md) | paths VERIFIED at every radiod start/stop since v0.0.5; the off/on/park sequence DEVICE UNVERIFIED |
| meshcored | radiod `off` is `degraded` "the radio is switched off", runtime told the radio is not there; link, lease and profile kept | none (IPC) | DEVICE UNVERIFIED; meshcored not linked on the host that wrote this (no vendor MeshCore trees) |
| Controls | LoRa radio tile switches the radio, off-to-on asks about the antenna; Bluetooth and Battery read-only tiles; Volume slider and mute | none (IPC, settings) | layout host-tested, never drawn |
| Volume | `audio_volume` / `audio_muted` in settings.conf; a digital gain in pocketaudio on every played sample (0 dB at 100 %, -27 dB at 10 %), under the unchanged -12 dBFS ceiling; Wave sends nothing while muted | MAX98357A via I2S, IO34 enable; no mixer volume on this route (AUDIO_HARDWARE_MAP §7) | route VERIFIED 2026-09-13; the gain DEVICE UNVERIFIED |
| Bluetooth | presence only: `system.status.bluetooth.controllers` from `/sys/class/bluetooth` | no controller on unit A | "no HCI device" VERIFIED 2026-09-07; kernel RFKILL unset DOCUMENTED |
| Battery | `system.status.power.battery` from the power_supply class (driver's own capacity, status, voltage) | BQ27220/BQ25896 on the keyboard base, no kernel driver bound | empty power_supply class VERIFIED 2026-09-07 |
| Diagnostics | System > Diagnostics; sysd `system.logs`, `system.crashes` | none (log directory reads) | DEVICE UNVERIFIED |

## What happens to unit A's existing state (the migration)

Unit A runs the sx1262 backend (`RADIOD_BACKEND=sx1262` in
`/etc/default/radiod`) and has meshcored enabled. v0.0.12 had no radio on/off
choice, so there is no `radio.conf` on the card. **After this build is
deployed, radiod finds no stored choice and starts with the radio off**,
parked. meshcored stays connected and holding the lease, reports `degraded`
("the radio is switched off"), and keeps its identity, channels and contacts
untouched. RIFT shows RADIO OFF. Nothing transmits until the owner switches the
radio on in Controls (antenna question) or with `doors radio on`; from then on
the stored choice decides. This is the deliberate, predictable default for a
card that never recorded a choice; to keep the old behaviour, switch the radio
on once. Wi-Fi credentials (`/var/lib/pocketos/netd/wifi.conf`), the MeshCore
store (`/var/lib/pocketos/meshcored/`) and app data are not touched by
anything on this branch.

Rolling back to v0.0.12 binaries needs nothing undone: v0.0.12's radiod
ignores `radio.conf`, and its shell ignores the two `audio_*` keys.

## Before starting

- Confirm no other session (the Camera work) is using unit A. If one is, run
  steps 1-2 only and come back.
- Do not reflash. Do not touch the golden SD card. Everything here is a
  userspace deploy that `deploy.sh` of the previous build undoes.
- An antenna must be on MMCX1 before step 6 (it is, per the owner, for this
  gate).

```bash
A=root@192.168.10.157
u() { ssh -o BatchMode=yes -o ConnectTimeout=8 $A sh -s -- "$@"; }
```

## 1. Build (WSL build host)

```bash
cd /home/dolby/work/<clone of feat/device-controls-diagnostics>
make all && make test            # host: expect the full suite green there (ggwave present)
cmake -S ui/shell -B ~/work/pocketos-build/shell -DLVGL_DIR=$HOME/work/lvgl -DPOCKETOS_DISPLAY=sdl
cmake --build ~/work/pocketos-build/shell -j8   # first real LVGL compile of controls.c, system_app.c, wave_app.c
# the SDL shell suites, including the ones that touch the changed screens:
for t in system_shell_test wave_shell_test rift_shell_test doors_shell_test settings_shell_test; do
    SHELL_BIN=~/work/pocketos-build/shell/pocketos-shell bash tests/$t.sh; done
SDL_VIDEODRIVER=dummy ~/work/pocketos-build/shell/pocketos-shell --open system --screenshot out/sim/system.png --exit-after-ms 2500
platforms/k230/scripts/apply_to_sdk.sh && platforms/k230/scripts/build_image.sh "" pocketos-rebuild
```

Pass: everything builds (this is the first time `controls.c`, `system_app.c`
and `wave_app.c` meet the real LVGL headers; they were only type-checked
against a declaration stub), every suite green, the simulator screenshots of
Controls (portrait and `--rotation landscape`) and System > Diagnostics show
nothing overlapping or cut.

## 2. Survey before deploying (read-only)

```bash
u <<'EOF'
cat /etc/doors-release; cat /etc/default/radiod /etc/default/meshcored 2>/dev/null
ls -la /var/lib/pocketos/radiod 2>&1; ls /sys/class/bluetooth 2>&1; ls /sys/class/power_supply
doors radio status; doors call meshcored mesh.status | head -20
ls /var/lib/pocketos/log; dmesg | tail -20
EOF
```

Record: backend, meshcored state, no `radiod/radio.conf`, no HCI, empty
power_supply.

## 3. Deploy

```bash
platforms/k230/scripts/deploy.sh 192.168.10.157
```

## 4. Default off after deploy (the migration)

```bash
u <<'EOF'
doors radio status            # expect "state": "off", "enabled": false
ls /var/lib/pocketos/radiod   # expect nothing stored yet
grep -h "radio off" /var/lib/pocketos/log/radiod.log | tail -3
doors call meshcored mesh.status | grep -E '"state"|"reason"|radio_state|online'
doors radio send 0102 ; echo rc=$?   # expect code 3, "switched off"
ls /var/lib/pocketos/meshcored       # identity.id, state.v1, channels.v1 unchanged
EOF
```

Pass: off, parked in the log, meshcored `degraded` / "the radio is switched
off" / `online` false, no reconnect lines accumulating in
`meshcored.log` over two minutes, send refused, MeshCore files unchanged
(compare sizes and mtimes with step 2).

**No RF while off:** GPIO44 low and SPI free:

```bash
u 'gpioinfo gpiochip1 | grep -E "line +12:"; fuser /dev/spidev0.0; echo fuser=$?'
```

Expect line 12 of gpiochip1 (GPIO44) unused/low, `fuser` finding nobody. With
a second node (unit B or a phone companion) listening on the MeshCore channel
for five minutes: nothing heard from unit A.

## 5. radiod restart while off

```bash
u '/etc/init.d/S60radiod restart; sleep 2; doors radio status | grep -E "state|enabled"; cat /run/pocketos/radiod.state'
```

Pass: still off, `restarts` does not climb, meshcored reconnects once and is
`degraded` again, no crash loop (`doors system status`).

## 6. Enable through the antenna question (antenna fitted)

On the panel: swipe to Controls, tap **LoRa radio**. Expect the question
"Connect an antenna before enabling the radio. / Transmitting without an
antenna may damage the RF output stage." with Cancel and Enable radio.

1. Tap **Cancel**: radio stays Off (`doors radio status` off, nothing stored).
2. Tap the tile again, then leave Controls (back): the question goes, radio Off.
3. Tap the tile, **Enable radio**: tile shows Receiving within two seconds.

```bash
u 'doors radio status | grep -E "state|enabled"; cat /var/lib/pocketos/radiod/radio.conf; doors call meshcored mesh.status | grep -E "\"state\"|online"'
```

Pass: `rx`, `enabled=1` stored, meshcored `online` within a few seconds with
the MeshCore profile (869.618 MHz, SF8) - the one it configured while the
radio was off.

## 7. RF works

From RIFT send an advert (ADVERT) and a channel message; confirm the peer
hears both. From the peer send a message; confirm RIFT shows it. `doors radio
stats` shows tx and rx packets counted.

## 8. Disable, reboot, persistence

Controls: tap **LoRa radio** (on) - it goes Off without a question.
`radio.conf` says `enabled=0`. `reboot`. After boot: `doors radio status` off,
parked in the log, meshcored degraded, Controls tile Off. Then `doors radio on`,
`reboot`, and confirm it comes back `rx` on its own (stored on wins).
Finish with the radio in the state the owner wants it left in.

## 9. Volume

Open Wave, send `DOORS` at the default 100 % (the validated level), then set
Volume to 50 % and 10 % in Controls and send again; the phone's Waver (or the
ear) should hear each clearly quieter, 50 % still decoding. Tap the speaker
glyph: Volume says Muted; a Wave send answers "Sound is muted. Turn it on in
Controls to send." and nothing is played (IO34 stays low:
`gpioinfo gpiochip1 | grep -E "line +2:"`). Unmute, `reboot`, and confirm Controls and
`doors shell volume` show the level set before the reboot. Also
`doors shell volume 55` must answer code 2 with nothing changed.

## 10. Bluetooth and battery

Controls: Bluetooth "Not available", Battery "External power". `doors call sysd
system.status | grep -A8 '"power"'`: `source` external, `battery` null,
`bluetooth.controllers` []. If the keyboard base with a charged cell and its
switch on is fitted, record whether a power_supply appears (it is not expected
to: no BQ27220 driver is bound) and what Controls then says.

## 11. Diagnostics

System > Diagnostics. Expect the rows filled within five seconds (one call a
second), Version with this build's id, Services "N running", LoRa radio and Mesh
matching steps 6-8, Bluetooth "No controller", Crashes "None" or the real
reports. Then make real warnings and errors:

```bash
u 'kill -9 $(cat /run/pocketos/radiod.pid)'        # supervisor: "radiod exited" WARN
u 'doors radio off; doors radio on'                 # radiod INFO lines
u 'kill -SEGV $(cat /run/pocketos/netd.pid)'        # a real crash report for netd; it restarts
```

Refresh: the log shows the radiod exit (WARN) and the netd crash, Warnings and
Errors filter correctly, Crashes lists `netd · SIGSEGV`. The panel keeps
drawing (status bar clock ticking) during every refresh. Scroll the log to the
end: 40 entries at most. With the logs deliberately grown
(`for i in $(seq 1 200); do doors radio off; doors radio on; done`) the page
still refreshes in the same time.

## 12. Soak and hygiene

Thirty minutes with Controls and Diagnostics opened and closed now and then:
`doors system status` shows no restarts, `dmesg` no new kernel errors,
`grep VmRSS /proc/$(cat /run/pocketos/doors-shell.pid)/status` and radiod's
not growing between the start and the end.

## Result

Run 2026-09-25 on unit A (antenna on MMCX1, owner-confirmed), build `09be665`.
Panel observations are DRM-plane captures (kmsgrab) with touch injected into
the Goodix evdev node; everything else is read over SSH. VERIFIED throughout
unless a row says otherwise.

| Step | Result | Notes |
| --- | --- | --- |
| 1 build and host suites | PASS | Clean clone of `09be665` under WSL: `make all` and `make test` -Werror (141 suites, 0 failures), the whole `make test` again under ASan/UBSan (0 reports), `meshcored-test`, `meshcore-*-test`, `camera-san-test`, SDL shell against the real LVGL tree (0 first-party warnings), every LVGL app test and all 23 shell suites; riscv64 `make all` (0 first-party warnings), radiod with `ENABLE_SX1262=1`, DRM/sysroot shell (0 warnings). The first real-LVGL run found two defects, fixed on the branch (below) |
| 2-3 survey, deploy | PASS | Before: v0.0.12 `a8b1a9f` + Camera shell `e3d3f71`, sx1262 `rx`, no `radiod/radio.conf`, no HCI, empty power_supply. Image built (IMAGE GATE PASS, not flashed); `deploy.sh` from its target tree: 8 binaries hash-match the tree, every service `09be665`. The first attempt ran over Wi-Fi and cut itself off (findings); repeated over eth0 |
| 4 default off, no RF | PASS | `off`, `enabled:false`, nothing stored; log "radio off (no stored choice: the sx1262 default)" and "transceiver parked". radiod holds no spidev or gpiochip descriptor, `fuser /dev/spidev0.0` finds nobody. With meshcored stopped, `send`, `send_async`, `cad`, `rssi` all code 3 "the radio is switched off"; `info` answers. tx_packets 0. meshcored `degraded` "the radio is switched off", `online:false`, lease held, 0 tx submitted; 120 s: 0 new log lines in radiod.log and meshcored.log, no pid change, 0 restarts. RIFT: RADIO OFF, adverts disabled. `gpioinfo` is not on the image, so GPIO44 was not read directly |
| 5 restart while off | PASS | `S60radiod restart`: still `off`, nothing stored; meshcored reconnects once (lease, configuring, degraded), 0 restarts, no crash loop |
| 6 antenna question, enable | PASS | Tap on the tile: the question with its exact text, Cancel and Enable radio. Cancel: off, nothing stored, logged. Leaving Controls with the question open (`shell.controls show=false`, the swipe's path) dismisses it; reopened, no question, still off. The scrim swallows every other tap, including back, as designed. Enable radio: `rx`, `enabled=1` stored, radiod "switched on by the owner", meshcored `degraded -> online` in the same millisecond with 869.618 MHz SF8, RIFT ONLINE / TRANSMIT READY, tile Receiving, RX chip. `doors radio on`/`off` and a boot with `enabled=1` show no question |
| 7 RF tx/rx | PASS | ADVERT NEAR tapped on the RIFT panel: tx 0 -> 1, 775 ms airtime. Direct message to T-Deck-RIFT `e34a0352`: **acked** (our frame reached the peer, its ACK reached unit A). `#doorsbench` flood sent. rx 2 -> 8 in 3 min, 0 CRC errors; meshcored 3/3 tx accepted |
| 8 disable, reboot persistence | PASS | Tile while on: off at once, no question, `enabled=0`, spidev and GPIO released, meshcored `online -> degraded`, no tx after. Reboot with nothing stored: off and parked again. Reboot with `enabled=1`: "radio on (stored choice)", meshcored online at boot. Both reboots: Wi-Fi credentials, MeshCore identity and channels, settings and every other state file byte-identical (sha256) |
| 9 volume, mute, persistence | PASS (owner heard it, see Owner checks) | Controls slider dragged to 50 %: `audio_volume=50`; the speaker glyph mutes (`effective` 0, "Muted") and unmutes; `doors shell volume 55`, `0`, `110`, `x` refused, nothing changed. `pos-wave send` with the Wave app's argv (`audible_fast`, `--volume 10`, `--volume-percent 70/40/10`, none at 100) played to completion at every level, 2.0 s each. 60 % survived a shell restart; 30 % muted survived a reboot. The gain itself is host-tested (pocketaudio_test, 151 checks); this unit cannot measure its own speaker (the mic route and the speaker route are exclusive, AUDIO_FEASIBILITY), and the Wave app could not be driven end to end because typing needs the keyboard base's keys. Audibility: the owner's listening check below |
| 10 Bluetooth, battery | PASS | `/sys/class/bluetooth` empty: Controls "Not available" (no toggle), Diagnostics "No controller". power_supply empty: Controls "External power", Diagnostics "No battery, external power". No percentage anywhere. Camera changed neither path |
| 11 diagnostics | PASS | Real values in every row: 0.0.13 · 09be665, uptime, memory, storage, "No battery, external power", "5 running · restarted: radiod 1x, sysd 1x" (after `kill -9` radiod and `kill -SEGV` sysd; sysd instead of netd, see findings), "Receiving · 869.618 MHz SF8", "Online", "No controller", "1 report" listed as `sysd · SIGSEGV · 09-25 13:12 UTC` with its first frame. Warnings shows the exits and poll failures; Errors showed "No errors logged" (true: no ERROR line on the card) and then the one real ERROR provoked with `radiod --radio-default bogus`. 35 refreshes: shell RSS 14960 kB before and after; `shell.info` answered in 19 ms on average, 40 ms at most, throughout. sysd read 82 KB of 10 sources and says older ones were not scanned (bounded) |
| 12 soak | PASS | 50 radio off/on cycles (slowest switch-on 150 ms), 5 more through the tile and the question, 30 Controls open/close, 30 volume steps, 10 Diagnostics open/close, 10 lock/unlock, portrait/landscape/portrait/automatic with Controls and Camera (live preview) in each: no restarts, no crash loop, same pids; RSS shell 14960 -> 14488 kB, radiod 3072 -> 2944, meshcored 3584 -> 3456, sysd 1408 -> 1536, netd 1536 -> 1536; 93 new kernel lines, all the camera stack's open/close messages |

### Owner checks (2026-09-25, owner at the unit)

- **Volume heard: PASS.** The same Wave transmission (`pos-wave send
  --protocol audible_fast --volume 10 --text DOORS`, the Wave app's argv, with
  the system volume passed as the app passes it) played twice each at 100 %,
  50 % and 10 %, then muted, where the app's rule starts no stream (effective
  0, no `pos-wave` process). The owner confirmed 50 % clearly quieter than
  100 %, 10 % clearly quieter than 50 %, mute silent, and no audible clipping
  or distortion at 100 %. Unit left at 100 %, unmuted.
- **DS §31.5 Controls: ACCEPTED as it is**, from panel captures of Controls in
  portrait and landscape and of the antenna question in both. That includes
  the glass antenna question through which the tiles show (more in portrait,
  where the Brightness and Volume tracks cross it).

### Fixed on the branch during the gate

- `8cfbcc8` `pos shell` usage appended volume to the brightness line, which
  identity_test pins as v0.0.9 printed it (scripts quote it); volume is now a
  line of its own.
- `09be665` Diagnostics values used the dotted 60 % kv row, so the portrait
  Mesh row read "Waiting: the radio is swit..."; they wrap now. The
  system_app_test pins still held master's Restart and Power off (the
  Diagnostics panel moves them down 120 px); they now pin all three, and the
  page itself is under test in the real LVGL tree (open, one bounded call per
  service, filters through sysd, 60-line log bounded to 40, same object count
  over ten refreshes, width, Back).

### Findings not caused by this branch

- `deploy.sh` to the Wi-Fi address stops netd before it unpacks, which drops
  its own SSH session; the remote tar waited forever with every service
  stopped. Recovered by running it again over eth0. Deploy over eth0.
- netd started at runtime (not at boot) timed out on wpa_supplicant's control
  socket 0.3 s after starting it and left Wi-Fi disconnected; `wpa_cli ping`
  answered moments later. At boot it connects. netd is untouched here.
- One kernel WARNING in the vendor RTL8189FS driver (`rtw_lps_state_chk`,
  its xmit thread leaving power save), 521 s after a boot; no crash, not seen
  again.
- System asks `system.info` once when it opens (as on master): opened a few
  seconds after sysd restarted it kept dashes for Doors, Model and Kernel
  until reopened.
- The antenna question is a glass panel over glass: the tiles behind show
  through its text. Readable on the panel; accepted as it is (DS §31.5).
- "1 newest errors" (singular wording).
