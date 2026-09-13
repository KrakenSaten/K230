# Audio hardware map and first audio test gate

Recorded 2026-09-13 on branch `feature/audio-ggwave` (from master `a748101`).
It extends docs/hardware/AUDIO_FEASIBILITY_2026-09-12.md with the built-in
speaker found in a second unit, and ends with the first-test proposal the
owner has to approve (§9, §10).

**No sound was played, no microphone was opened, and no mixer control, GPIO
line or pad was written, on any board, for this record.** Unit A was not
contacted in this session (§7).

Classes: **VERIFIED** (measured on unit A), **DOCUMENTED** (schematic, device
tree, driver or vendor source, datasheet), **INFERRED** (follows from
documented facts, not observed), **ASSUMED**, **UNKNOWN**.

Path shorthand as in the feasibility record: `SCH` = the V1.0 schematic,
`K` = the pinned kernel build tree, `L` = the vendor launcher source.

## 1. The new physical evidence (owner, second unit)

| Item | Observation | Class |
| --- | --- | --- |
| Speaker | A rectangular module marked `TR-WS-2014B` | owner, visual |
| Wiring | One red and one black wire, both to a white 2-pin connector on "the main PCB" | owner, visual |
| Nearby marking | Possibly `2618`. Its meaning is **not assumed** here | owner, visual |
| Photograph | Exists, but was not available to this session: nothing in this record rests on it | - |

What a TR-WS-2014B is, from a reseller datasheet (techiesms.com product page):
20 x 14 x 4.5 mm cavity speaker, **7.2 Ω ± 10 %**, **1.0 W rated, 1.5 W
maximum**, 94.5 ± 2 dB SPL at 3.1 V / 10 cm / 2 kHz, resonance 900 Hz ± 10 %,
range F0 to 20 kHz. DOCUMENTED for the part number; that the fitted part
matches this sheet is ASSUMED.

## 2. What the documents say

- **The V1.0 main board has no speaker path.** All eight sheets (dated
  2026-06-26): the only audio outputs are `HP_OUTL`/`HP_OUTR` from the K230's
  codec balls G1/G2, through C167/C166 (47 µF) to the 4-pin jack AUDIO1
  (PJ-3220). There is no amplifier IC and no 2-pin speaker connector; the
  peripheral sheet's parts are the CH342K, MT9700 switches, RTL8189FTV,
  XC6206, RT9080, BSS138, the mic B4012AP422-003, the jack, buttons and ESD
  diodes. DOCUMENTED.
- **The only audio signals that leave the main board are digital.** The
  40-pin header JP1 carries IO32, IO33, IO34 and IO35 among the GPIOs, and no
  analog audio net. DOCUMENTED (`SCH` p2 JP1, `k230_bsp/docs/HARDWARE_PINMAP.md`).
- **The vendor names that digital path as the speaker path.**
  - Pinmap: a **MAX98357A** on the optional nRF52840 BLE/audio/sensor base
    board, DIN = GPIO35, BCLK = GPIO32, LRCK = GPIO33, shutdown = GPIO34,
    "high enables the amplifier" (HARDWARE_PINMAP.md:95-98). DOCUMENTED.
  - Device tree: the I2S pin group is called `amp_i2s_pins`, with IO32
    `IIS_CLK`, IO33 `IIS_WS`, IO35 `IIS_D_OUT0` and IO34 as a GPIO output named
    `amp_shutdown`; the sound node sets `canaan,external-i2s-output-default`
    (`K/.../k230-canmv-rm69a10.dts:33-40,130-158,228-231`). DOCUMENTED.
  - Launcher: its "External speaker" output is `amixer cset name='External
    I2S Output Switch' 1` plus GPIO34 driven high on gpiochip1 line 2
    (consumer `k230-phone-amp`); "Headphones" is the switch at 0 and GPIO34 low
    (`L/ui_hardware.c:1655-1769`). Its I2S test page is titled "MAX98357A" and
    plays S16_LE, 48 kHz, 2 channels (`L/ui_i2s_test.c:21,246-249`). It picks
    the external speaker as the default only when an AHT20 - the nRF52840
    base board's sensor - answers (`L/ui_hardware.c:6299-6303`). DOCUMENTED.
- **Driver.** "External I2S Output Switch" sets the audio block's
  `audio_codec_bypass` bit ("use I2S directly to IO", `canaan_k230_audio.h:197`)
  and re-applies it on every stream start (`canaan_k230_inno.c:40-45,90-99`).
  DOCUMENTED.
- **Product pages.** Neither the LILYGO product page, the kit page nor the
  wiki names a speaker, amplifier or its connector.

## 3. The built-in speaker path

```
K230 DesignWare I2S (0x9140f000), codec bypassed by "External I2S Output Switch" = on
  IO32 BCLK, IO33 LRCK, IO35 SDATA          IO34 = amplifier enable (high = on)
        |                                          |
   40-pin header JP1  -------------------------------
        |
   [board not covered by any schematic available: vendor pinmap and launcher say
    MAX98357A, an I2S-input class-D amplifier]
        |
   white 2-pin connector  ->  red / black  ->  TR-WS-2014B
```

**INFERRED, strongly, and not verified:** the speaker is driven digitally over
I2S by an amplifier on a board fitted to the header, not by the codec's analog
output. It rests on three independent documents agreeing (the main-board
schematic has no speaker path; the only signals that could reach one are the
I2S pins; the vendor's software and pinmap name exactly this path) and on none
contradicting. **The INNO codec is not in this path.** The owner's "main PCB"
is, on this reading, the board that carries the amplifier.

| Question (brief §A) | Answer | Class |
| --- | --- | --- |
| Connector designator | Not in any schematic we have | UNKNOWN |
| Connector pinout | If the amplifier is a MAX98357A: OUTP / OUTN | UNKNOWN (which wire is which) |
| Differential / BTL | MAX98357A outputs are filterless bridge-tied (BTL) | DOCUMENTED for the part; the part is INFERRED |
| Is either wire ground | **No, if it is a MAX98357A** - both terminals switch; the black wire must not be treated as ground | DOCUMENTED for the part |
| Amplifier chip | MAX98357A per vendor documents; not seen | INFERRED |
| Amplifier type | Class D, I2S/TDM input, no I2C, gain by strap, channel select by SD_MODE voltage | DOCUMENTED for the part |
| Power rail | MAX98357A runs from 2.5 - 5.5 V; the board's rail is not documented | UNKNOWN |
| Enable / mute / shutdown | IO34 (gpiochip1 line 2), high = on. SD_MODE below 0.16 V = shutdown, above 1.4 V = left channel | DOCUMENTED (vendor, secondary datasheet) |
| Gain | GAIN_SLOT strap: 15 / 12 / 9 (floating) / 6 / 3 dB | UNKNOWN which is fitted |
| Speaker impedance and power | 7.2 Ω nominal, 1.0 W rated, 1.5 W max | DOCUMENTED (reseller sheet) |
| What the amplifier can deliver | About 1.8 W into 8 Ω at 5 V (secondary source), more than the speaker's rating | DOCUMENTED for the part, so **the digital level must be limited** (§8) |
| What `2618` is | - | UNKNOWN |

**What would settle it, powered off, with no tools beyond a camera:** the
board carrying the white connector, its silkscreen (name, revision), which
connector it plugs into, and the IC nearest the connector with its top marking.
A MAX98357A is either a 9-bump WLP (1.35 x 1.44 mm) or a 16-pin TQFN (3 x 3 mm).
If instead the connector sits on the K230 board itself, the V1.0 schematic does
not describe this unit and §3 must be redone before any playback.

## 4. Playback paths

| | Built-in speaker | 3.5 mm jack |
| --- | --- | --- |
| Route switch | `External I2S Output Switch` = on (boot default) | = off |
| Signal | I2S on IO32/33/35 to the header | codec DAC -> `HP_OUTL/R` -> 47 µF -> AUDIO1 |
| Level control | **Digital only** (sample values); the codec's `PCM Playback Volume` does not apply | `PCM Playback Volume` 0-45 (-39..+6 dB), with the first-DAC-setup quirk (feasibility §1) |
| Enable | IO34 high | none (the driver enables the headphone driver on every playback, whatever the route) |
| GPIO35 | **in the path** (serial data) | not in the path |
| Used by Wave | yes (SEND) | no |

## 5. Microphone path

- **On-board mic** MIC1 `B4012AP422-003`, two pins (OUT, GND), biased from
  `MIC_BIAS` through R52 2 kΩ with R53 2 kΩ and C172 10 nF, coupled by C171
  4.7 µF to `MIC_PR` (K230 ball F1), the codec's **right** ADC input; `MIC_NR`
  (F2) AC-grounded by C173. DOCUMENTED (`SCH` p6). Capsule type (electret)
  ASSUMED from the two-pin biased topology.
- **Headset mic** on jack pin 1, C165 4.7 µF to `MIC_PL` (E1), the **left**
  ADC; `MIC_NL` AC-grounded by C162; bias R50 2 kΩ. DOCUMENTED.
- **ALSA capture device:** card 0 `K230I2SINNO`, PCM `hw:0,0`, one capture
  substream (VERIFIED, feasibility §6). pocketaudio opens it by id,
  `hw:CARD=K230I2SINNO,DEV=0`, so a second card cannot take index 0.
- **Channel mapping:** the I2S link is stereo only (`canaan-dwc-i2s.c` accepts
  2/4/6/8 channels). Right slot = on-board mic, left slot = headset mic.
  INFERRED from the schematic nets and the standard I2S slot order; **checked
  by the first RECEIVE test** (§10 step 2).
- **Route for capture:** `External I2S Output Switch` must be **off**; with it
  on, the codec is bypassed and capture reads no codec data. The vendor turns
  it off before every capture (`L/ui_hardware.c:1771-1789`). DOCUMENTED.
- **Gain controls:** `Mic Capture Volume` 0-30 in steps of 10 writes only the
  **left** mic gain (0/6/20/30 dB); the on-board mic's gain stays at the
  driver's power-on value 3 = 30 dB and is not adjustable through ALSA.
  `Mic Capture Switch` mutes both. VERIFIED values on unit A: volume 30,
  switch on. DOCUMENTED behaviour (`inno_k230.c:174-203`,
  `inno_k230_reg.c:49-65,789-840`).
- **Automatic gain / noise processing:** the SoC audio block's AGC is
  bypassed (`canaan_k230_audio.c:23`). The codec's two "ALC" stages are
  enabled with a fixed gain word 0x12 and their setters are commented out
  (`inno_k230_reg.c:56-57,396-406,492-502,1007-1122`); whether that stage
  adapts by itself is not stated anywhere - UNKNOWN. There is no noise
  suppression in the driver path. Mic bias is switched on at the first capture
  and never off again (`inno_k230_reg.c:250-266,563`). DOCUMENTED.

## 6. GPIO35 and R54

- **GPIO35 is in the built-in speaker path**: it carries the I2S serial data
  to the amplifier (INFERRED, §3). It is **not** in the microphone path and
  not in the headphone path. The brief's hoped-for case - speaker on the
  codec's analog output with GPIO35 out of the path - does not match the
  documents.
- IO35 is also `IO35_DISEN`: R114 4.7 kΩ into Q3 (S8050), which switches Q2
  (AO3401A) feeding TP_VDDIO, LCD_VDD and VBAT; R54 (`NC/0R`) bypasses Q2.
  DOCUMENTED (`SCH` p4).
- On unit A IO35 reads **low with the panel powered** (VERIFIED,
  feasibility §6). With IO35 low Q3 and Q2 are off, so the rail can only be on
  through R54: **R54 fitted is INFERRED, strongly. Not yet seen.**
- **Relevance:** decisive for SEND - speaker playback toggles IO35 at audio
  bit rates, and with R54 fitted that switches Q3/Q2 without affecting the
  rail. Irrelevant for RECEIVE: a capture opens no transmit data, so IO35
  stays idle.
- During a capture the SoC still clocks IO32/IO33 onto the header, so the
  amplifier sees BCLK and LRCK; it stays in shutdown because IO34 is low
  (VERIFIED: pad pull-down, level 0, no consumer on gpiochip1 line 2).
- IO35 drives both the amplifier's DIN and R114 into Q3's base: about 0.23 mA
  at a 1.8 V pad or 0.55 mA at 3.3 V (the pad's supply is not established
  here), well within the pad's 8 mA drive setting (DOCUMENTED in the DTS).

## 7. Read-only live enumeration

This session **could not reach unit A**: its network address was not
available, and discovering it on the LAN was not permitted in this session.
Nothing new was read. What stands is the silent enumeration of this morning
(feasibility §6, same unit, same kernel): card and PCM present and closed,
`External I2S Output Switch` on, `PCM Playback Volume` 24/45, `Mic Capture
Volume` 30/30, both switches on, IO35 = I2S data out at level 0, IO34 = GPIO
with pull-down at level 0, gpiochip1 lines 2 and 3 unused. VERIFIED then.

Still wanted before the first test, all reads (no stream, no write, no line
requested):

```sh
cat /proc/asound/cards /proc/asound/card0/id
cat /proc/asound/card0/pcm0p/info /proc/asound/card0/pcm0c/info
cat /proc/asound/card0/pcm0p/sub0/hw_params /proc/asound/card0/pcm0c/sub0/hw_params
amixer -c 0 contents
gpioinfo -c gpiochip1 2 3
for p in 3 4 32 33 34 35 46 47; do printf 'IO%s ' $p; devmem $((0x91105000 + p * 4)) 32; done
ls -l /proc/[0-9]*/fd 2>/dev/null | grep /dev/snd
```

The pad reads add one thing: IO4 is UART1 RX from the nRF52840 base board
(`uart1_nrf52840_pins`). Bit 31 of its pad reading high, with no pull
configured, would say something is driving that line - weak evidence that the
audio base board is present on unit A. The 2026-09-07 finding that "no base
board is connected" rested on an I2C scan of the wrong bus (the vendor
bit-bangs IO46/47; KEYBOARD_BRINGUP_2026-09-10.md) and does not settle it.

## 8. Format and level

- **48 000 Hz, S16_LE, 2 channels on the wire**: mono duplicated into both
  slots for playback, the right slot taken for capture. 20 ms periods,
  80 ms playback buffer, 500 ms capture buffer (room for ggwave's analysis
  step on a slow core; the kernel preallocates 2.7 s per stream anyway). Why: the codec supports it and
  enforces one rate for both directions; the vendor's own speaker test uses
  exactly 48 kHz S16_LE stereo; ggwave's standard 1024-sample frames give
  46.875 Hz per bin at 48 kHz, so Wave can talk to other ggwave programs; and
  nothing needs resampling (ggwave's resampling path loses data and costs
  about 170 times the CPU).
- **Hard ceiling in pocketaudio: -12 dBFS** (peak 8192). Every played sample
  is clamped to it; only a code change raises it.
- **ggwave level**: peak is about volume/100 of full scale (measured on the
  host, software only; tests/wave_modem_test.c prints it): volume 10 -> 3192
  (-20.2 dBFS), 25 -> 7980 (-12.3 dBFS). pos-wave
  accepts 1 to 25; Wave uses 10.
- **Proposed first playback level: `--volume 5`**, a peak of about 1600
  (-26 dBFS). Whatever the amplifier's gain strap, a -26 dBFS peak is at most
  1/400 of its full-scale power: under 5 mW even if full scale were 2 W,
  against a speaker rated 1 W. Audible at arm's length if the path works,
  harmless if the gain is higher than expected.

## 9. Proposed first real SEND test (needs owner approval)

**Preconditions, board powered off:** the amplifier board and IC near the
white connector identified (§3); R54 seen fitted; nothing in the 3.5 mm jack;
the owner present, the board on the desk, nobody's ear near the speaker; unit
running a build that contains pos-wave (deploy.sh or image). `pos-wave info`
must print `board k230-t-display` with both paths `not validated (gated)`.

**Signal:** ggwave AUDIBLE_FAST, payload `DOORS` (5 bytes), 56 frames of
tones between **1875 and 6328 Hz**, 1.194 s, plus 100 ms of silence before
and 150 ms after: **1.444 s** in all.

```sh
# 0. state before (reads)
amixer -c 0 contents > /tmp/audio-before.txt
gpioinfo -c gpiochip1 2 3
# 1. the waveform, checked with no audio device involved
pos-wave encode --volume 5 --text DOORS /tmp/doors.wav   # expect duration_ms 1194, peak ~1596
pos-wave decode /tmp/doors.wav                           # expect "text DOORS"
# 2. the one playback (owner watching the panel)
pos-wave send --allow-unverified --volume 5 --protocol audible_fast --events --text DOORS
#    expect: ready k230-t-display / sending 1444 / sent ; exit 0
# 3. state after (reads)
amixer -c 0 contents > /tmp/audio-after.txt; diff /tmp/audio-before.txt /tmp/audio-after.txt
gpioinfo -c gpiochip1 2
devmem 0x91105088 32                                     # IO34 pad: level bit 31 expected 0
```

What pocketaudio does in step 2, in order: takes the audio lock; reads the
route (on at boot, so it is not written); opens and prepares the PCM; requests
gpiochip1 line 2 as an output **already high** (amplifier on); plays; drains;
drives line 2 low and releases it; closes the PCM; releases the lock. Ctrl-C
at any point stops within one 200 ms wait and runs the same cleanup.

**Watch for:** the panel flickering or blanking (would contradict R54), a
click at amplifier enable, anything louder than expected. **Abort:** Ctrl-C.
**Pass:** tones heard, panel steady, exit 0, mixer unchanged, IO34 low after.
A second, later step (separately approved): decode the same transmission on a
phone running a ggwave receiver, or on unit B.

## 10. Proposed first real RECEIVE test (needs owner approval)

**Preconditions:** the owner present and aware the microphone will be on;
nothing in the jack; a transmitter a hand's length to an arm's length away at
a modest level - a phone ggwave app sending `DOORS`, or a laptop playing a WAV
made by `pos-wave encode --text DOORS` on the host.

```sh
# 1. state before (reads)
amixer -c 0 contents > /tmp/audio-before.txt
# 2. which slot is the on-board mic: two 5 s bench recordings while DOORS is sent repeatedly
pos-wave record --allow-unverified --seconds 5 --channel 1 /tmp/mic-right.wav   # prints samples, peak
pos-wave record --allow-unverified --seconds 5 --channel 0 /tmp/mic-left.wav
pos-wave decode /tmp/mic-right.wav     # expected: text DOORS
pos-wave decode /tmp/mic-left.wav      # expected: nothing (exit 1)
# 3. live, 30 s, while DOORS is sent a few times
pos-wave listen --allow-unverified --seconds 30 --events
#    expect: ready k230-t-display / listening / level N ... / received 444f4f5253
# 4. state after, and the recordings removed
amixer -c 0 contents > /tmp/audio-after.txt; diff /tmp/audio-before.txt /tmp/audio-after.txt
rm /tmp/mic-right.wav /tmp/mic-left.wav
```

pocketaudio switches `External I2S Output Switch` off for the capture and back
on afterwards; the diff in step 4 should be empty. Expected side effect that is
the driver's, not ours: mic bias stays on after the first capture.

**Pass:** step 2 shows the message on the right slot only (otherwise the
board entry's `capture_channel` is wrong and must change before Wave is
enabled), step 3 decodes at least one transmission, the mixer is back as it
was. Receive over the air at 30 cm is not a pass criterion of this first
test; it is its measurement.

## 11. What stays open

Carried into docs/KNOWN_ISSUES.md. The first four gate SEND:

1. The amplifier and its connector are inferred from documents, not seen
   (§3). If the connector is on an undocumented main-board revision with an
   analog amplifier, the playback plan changes.
2. R54 is inferred, not seen (§6).
3. The amplifier's gain strap and supply are unknown; the first level rests on
   digital attenuation alone (§8).
4. Unit A's address was not available here; the §7 reads are outstanding.
5. The capture slot mapping is inferred (§5).
6. After a crash or SIGKILL of pos-wave the route and IO34 keep their last
   values: the kernel closes the PCM, so nothing plays, but the amplifier may
   stay enabled until the next pos-wave run. That gpiolib leaves a released
   line at its value is ASSUMED.
7. A playback on the speaker route still enables the codec's headphone driver
   (driver behaviour): a click on the jack is possible; keep it empty.
8. ggwave's CPU cost on the C908 is unmeasured. Its worst single decode call
   on the host was 6.2 ms with one protocol and 17.9 ms with all twelve (Wave
   enables three); a spike twenty times that on the board would still fit
   the 500 ms capture buffer, much more would not and would cost a message.
   riscv64 test binaries from the ggwave study exist to measure it (not run).
9. The codec's ALC behaviour and the fixed 30 dB on-board mic gain may clip
   in a loud room; neither is adjustable through ALSA.
10. The small speaker's response across 1.9 - 6.3 kHz is unmeasured.
