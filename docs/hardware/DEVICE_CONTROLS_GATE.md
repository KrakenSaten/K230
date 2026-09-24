# Device controls and diagnostics: unit A gate

Branch `feat/device-controls-diagnostics`. Status: **NOT RUN.** Everything on
this branch is host-built and host-tested only. The session that wrote it ran in
a cloud container: unit A (192.168.10.157) was not reachable from there, and no
Buildroot tree, LVGL, RadioLib or ggwave checkout was available. Every
hardware claim below is therefore **DEVICE UNVERIFIED** until this procedure has
been run on unit A.

Evidence classes as in AGENTS.md: VERIFIED (observed on unit A), DOCUMENTED
(repository or vendor source), ASSUMED.

## What the branch changes on the device

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

| Step | Result | Notes |
| --- | --- | --- |
| 1 build and host suites | NOT RUN | |
| 2-3 survey, deploy | NOT RUN | |
| 4 default off, no RF | NOT RUN | |
| 5 restart while off | NOT RUN | |
| 6 antenna question, enable | NOT RUN | |
| 7 RF tx/rx | NOT RUN | |
| 8 disable, reboot persistence | NOT RUN | |
| 9 volume, mute, persistence | NOT RUN | |
| 10 Bluetooth, battery | NOT RUN | |
| 11 diagnostics | NOT RUN | |
| 12 soak | NOT RUN | |
