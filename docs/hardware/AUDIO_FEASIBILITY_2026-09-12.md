# Audio and microphone on the T-Display K230: feasibility and foundation

> **Update 2026-09-13:** a second unit has a built-in speaker, physically
> confirmed, on a separate base board carrying a MAX98357A (confirmed with
> high confidence from its top marking and the vendor sources). Its path - I2S
> on IO32/33/35 to that amplifier, enabled by IO34, which puts GPIO35 in the
> speaker path - the R54 analysis and the proposed first audio tests are in
> AUDIO_HARDWARE_MAP_2026-09-13.md. Section 1's "no speaker" describes the
> main-board sheets only, **not the product**. Later the same day both paths
> were validated on unit A - RECEIVE, and SEND on the external I2S route with
> the panel steady - so section 0's playback caution is closed (hardware map
> §8.8, §15, §16).

Recorded 2026-09-12. A static study only: the pinned kernel build tree
(`k230_pocketos_defconfig`, the tree unit A's kernel was built from), the
V1.0 schematic, the LILYGO BSP and launcher sources, the Buildroot
configuration, and the unit A bench records already in the repository.

**No sound was played, no tone was generated, no playback or capture stream
was opened, no microphone was activated, and no mixer control was read or
changed on any board while this was prepared.** Nothing was built for audio.

Classes: **PROVEN** = DOCUMENTED (schematic, device tree, driver source,
config) or VERIFIED (unit A runtime record); **STRONGLY INFERRED**; **UNKNOWN**.

Path shorthand: `K` = kernel build tree
`output/k230_pocketos_defconfig/build/linux-7d4e1f444f461dbe3833bd99a4640e7b6c2cd529`
(its audio sources, DTS and sound config are byte-identical to the vendor
rm69a10 tree); `SCH` = `vendor/T-Display-K230_canmv_rt/schematic/T-Display
K230_V1.0_NEW.pdf`; `HW` = `docs/hardware/hwcheck-unitA/device-hwcheck-19700101_000523/`;
`L` = `vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/`.

## 0. Read this before any audio test: GPIO35

The default audio route and the display power switch share a pin.

- The sound node sets `canaan,external-i2s-output-default` (LILYGO patch
  0059), and unit A's dmesg says `External I2S output enabled by default`
  (`HW/dmesg.txt:260`). So as booted, playback goes to the I2S pads on the
  header, **not** to the codec, and the device tree muxes **IO35 as the I2S
  data output** (`k230-canmv-rm69a10.dts:130-158,228-231`). PROVEN.
- The schematic names GPIO35 **`IO35_DISEN`**: through R114, Q3 (S8050) and
  Q2 (AO3401A) it switches VDD_3V3 to TP_VDDIO, LCD_VDD and VBAT (`SCH` p2,
  p4). A resistor marked `NC/0R`, R54, can bypass Q2. PROVEN (schematic).
- The display works on unit A today, so either R54 is fitted or IO35 idles at
  a level that keeps the rail on. **Update 2026-09-13 (section 6):** IO35 was
  read LOW on unit A with the display on and no stream open. An NPN (Q3)
  driving a P-channel high-side switch (Q2) conducts when its input is high,
  as the net name "DISEN" says; with IO35 low, Q2 is off, so the rail can only
  be on through R54. R54 is therefore STRONGLY INFERRED fitted. A visual check
  of the board is still wanted before the first playback.

Consequence: a playback stream on the default external route would put audio
data on a line that may switch the display supply. **No playback may be
started on this board until R54 has been checked and the route has been
switched to the internal codec.** The same pin question applies to any future
use of the base board's MAX98357A.

## 1. Proven

**Codec.** The K230's built-in INNO codec, memory-mapped at `0x9140e000`,
compatible `canaan,k230-inno-codec`, status okay (`K/arch/riscv/boot/dts/canaan/k230.dtsi:678-684`).
Driver `sound/soc/codecs/inno_k230.c` + `inno_k230_reg.c`, built in
(`CONFIG_SND_SOC_K230_INNO=y`, objects present). Register access is direct
MMIO (`readl`/`writel`), not I2C. On unit A `/proc/asound/pcm` lists `Audio
9140e000.inno_codec-0 : playback 1 : capture 1` (`HW/report.txt:333`, VERIFIED).

**Microphones** (`SCH` p6). Both analog, no PDM/MEMS digital mic on the main
board, one shared `MIC_BIAS` (C8 100 nF, p2), each fed through 2 kΩ:
- Onboard mic MIC1, part B4012AP422-003 (2 pins, OUT/GND), via C171 4.7 µF to
  `MIC_PR` = **right** ADC channel; `MIC_NR` AC-grounded by C173; bias R52
  2 kΩ, R53 2 kΩ to GND, C172 10 nF.
- Headset mic on jack pin 1, via C165 4.7 µF to `MIC_PL` = **left** ADC
  channel; `MIC_NL` AC-grounded by C162; bias R50 2 kΩ.

**Playback.** 3.5 mm jack AUDIO1 `Audio_PJ-3220`, 4 pins (MIC, GND, L, R).
`HP_OUTL`/`HP_OUTR` through C167/C166 47 µF. No jack-detect line, no detect
GPIO in the DT, and the driver's detect thread is commented out
(`inno_k230_reg.c:268-328`). **No speaker, amplifier or line-out on any of the
eight main-board sheets.** The MAX98357A exists only on the optional nRF52840
base board (`k230_bsp/docs/HARDWARE_PINMAP.md:95-98`).

**I2S.** One controller, `canaan,snps,designware-i2s` @`0x9140f000`, DMA tx/rx
on PDMA channels 0x14/0x15, no interrupt; the audio block `canaan,k230-audio`
@`0x9140f400` (`k230.dtsi:660-676`). The same I2S feeds either the codec or the
header pads, chosen by bit 5 `audio_codec_bypass`
(`sound/soc/canaan/canaan_k230_audio.h:197-203`). Pins: IO32 BCLK, IO33 WS,
IO35 data out, IO34 a plain GPIO output with no level set. Route default:
external (section 0).

**Kernel.** Built in, no modules (`K/.config`): `SND`, `SND_SOC`,
`SND_SOC_GENERIC_DMAENGINE_PCM`, `CANAAN_SND_DESIGNWARE_I2S`/`_PCM`,
`SND_SOC_CANAAN_K230_INNO`/`_AUDIO`, `SND_SOC_K230_INNO`, `K230_PERIDMA`,
`SND_JACK`; `PREEMPT_NONE`, `HZ=250`. Device-tree nodes: the `sound` card node,
`i2s@9140f000`, `audio@9140f400`, `inno_codec@9140e000`, all status okay.

**Userspace (Buildroot).** alsa-lib (all PCM/CTL plugins, UCM, topology) and
alsa-utils `alsactl`, `alsamixer`, `amixer`, `aplay` (`arecord` is aplay).
The vendor also installs `audio_demo`, `audio_rec_play` and `ffmpeg`. The
target has **no** `asound.conf`, no UCM2 profile, an empty `/var/lib/alsa`,
and no init script that touches ALSA. No tinyalsa, no speaker-test.

**ALSA names** (VERIFIED from unit A records): card 0, id `K230I2SINNO`,
name `K230_I2S_INNO`; PCM `00-00`, i.e. **`hw:0,0`**, one playback and one
capture substream. Driver `canaan-k230-snd-inno`, link `k230-inno-codec`.

**Mixer controls** (names from source, DOCUMENTED):
- card: `External I2S Output Switch` (`canaan_k230_inno.c:74-78`);
- codec (`inno_k230.c:206-255`): `PCM Playback Volume` 0-45 in steps of 3 =
  −39 … +6 dB headphone gain; `PCM Playback Switch` (digital mute); `Mic
  Capture Volume` 0-30 in steps of 10; `Mic Capture Switch`; `PCM` and `PCM
  Switch` as aliases of the playback pair. No DAPM widgets or routes.
- Hardware defaults written on the first DAC/ADC setup
  (`inno_k230_reg.c:49-65`): headphone gain register 0x10 (−15 dB), DAC digital
  volume 0xf1, mic gain 3, ALC 0x12, ADC volume 0xe1/0xf0.

**Rates and formats.** Codec: 8, 16, 32, 44.1, 48 kHz; S16_LE and S32_LE;
1-2 channels; playback and capture; the same rate is enforced in both
directions (`inno_k230.c:382-414`). I2S controller: 8-192 kHz, formats read
from hardware at runtime, `hw_params` accepts only 2/4/6/8 channels
(`canaan-dwc-i2s.c:322-331`). The SoC is clock master, MCLK = rate × 256.

**Buffers.** No IRQ on the I2S, so the generic DMA PCM layer is used with its
defaults: 512 KiB preallocated per stream, at least 2 periods, 32-byte minimum
period (`soc-generic-dmaengine-pcm.c:18,123-130`). One Linux-visible hart and
512 MiB CMA (VERIFIED). The vendor launcher uses 48 kHz, 10 ms periods, a
50 ms buffer (`L/ui_audio.c:45-47`).

**Hardware behaviour that matters for safety** (DOCUMENTED in the driver):
- Probe resets the codec and ramps its reference voltage; the headphone driver
  is not enabled then (`inno_k230_reg.c:143-248`).
- Playback `hw_params` enables and unmutes the headphone driver **whatever the
  route**, and nothing ever powers it down again (empty shutdown, no-op bias
  level) (`inno_k230_reg.c:567-766`).
- The first DAC setup **unmutes before it writes the gain** (743-755 then
  757-760), and overwrites any volume or mute set earlier in hardware while the
  ALSA control keeps reporting the old value (107-130). A mixer value set
  before the first playback is therefore not a guarantee.
- Mic bias is switched on at capture, cycled off for 100 ms, and never switched
  off again (250-266, 563).
- The vendor launcher defaults to volume 45 = +6 dB (`L/ui_audio.c:49-50,353-365`).
  It is disabled whenever the PocketOS shell owns the panel.

## 2. Strongly inferred

- On the default external route the headphones are silent and capture reads
  no codec data. The vendor launcher turns `External I2S Output Switch` off
  before every capture (`L/ui_hardware.c:1771-1789`).
- `hw:0,0` is effectively stereo only: mono needs the `plug` layer. On
  capture, **left = headset mic, right = onboard mic**.
- Full duplex works only at one shared rate and format (one control register
  for both directions, `canaan-dwc-i2s.c:315-335`). The only useful duplex pair
  is headphones plus mics on the internal route.
- Possible driver bug: after an S16 stream, a later S32 playback may program
  the wrong DMA width (`canaan-dwc-i2s.c:290-307`). Use S16_LE only until tested.
- `Mic Capture Volume` changes only the left gain (`inno_k230.c:195-197`): the
  onboard mic's gain is not adjustable through ALSA.
- The headphone driver runs from 1.8 V, so output swing is under 1.8 Vpp, but
  +6 dB can still be loud on sensitive in-ear phones. A click on the first
  playback is likely (47 µF coupling capacitors charging).
- On a single hart with `PREEMPT_NONE`, periods of 10-20 ms or more are
  advisable; a 64 KiB maximum period is the kernel default.
- Nothing produces sound at boot on unit A: the launcher is off and nothing
  opens a stream.

## 3. Unknown

- **Whether R54 is fitted** (section 0) by sight. Electrically STRONGLY
  INFERRED fitted since 2026-09-13; IO35's idle level is now VERIFIED low
  (section 6).
- GPIO34's level at boot (it would enable the base-board amplifier). After
  boot it is VERIFIED low: GPIO function, pull-down, not driven (section 6).
- The I2S controller's real format list, channel limit and FIFO depth.
- Headphone gain and mute register values before the first playback; the mic
  bias voltage (its field is never set).
- Whether the external route really silences the headphones; 44.1 kHz clock
  accuracy; loudness; pop size; mic sensitivity; jack pole order (CTIA or
  OMTP); the mic capsule's exact type.

## 4. Proposed PocketOS audio architecture (not built)

Following ADR-002, one service owns the sound card and everything else asks
it. Nothing of this exists yet; it is a sketch to review, not a decision.

- **`audiod`**, supervised like radiod and sysd, the only process that opens
  `hw:0,0`, linked against alsa-lib (already in the image). Started muted.
- **HAL below it**: route (internal codec / external I2S), headphone gain,
  mute, mic gain, rate. The K230 quirks (unmute-before-gain on the first DAC
  setup, the route bit, the GPIO35 question) live here and nowhere else.
- **`audio.*` API** (versioned like the others):
  `audio.info` (card, routes, rates, capture channels),
  `audio.status` (route, volume, mute, active streams and their owners),
  `audio.set_volume` (clamped to a hard ceiling below +6 dB, default low),
  `audio.set_mute`, `audio.play` (a WAV from an allowed directory, bounded
  length; later a tone for alarms), `audio.stop`,
  `audio.record` (bounded duration, to the caller's state directory, only
  while the requesting app is in the foreground), events `audio.state`.
- **Ownership**: one playback and one capture stream at a time. System alerts
  (the Clock alarm, DS §18) pre-empt app playback; a second app is refused
  with code 5 rather than mixed. Mixing, if ever needed, goes into audiod, not
  into apps.
- **Privacy**: a capture is always visible - a status-bar indicator for as
  long as the mic is open, driven by `audio.state` - and ends when its app
  leaves. No background recording.
- **Volume and safety**: the stored volume is applied only after the route is
  the internal codec; the first playback after boot starts at the lowest gain
  and ramps; no playback while the route is external until R54 is resolved.
- **Stream shape**: 48 kHz, S16_LE, stereo, 20 ms periods, 4-period buffer
  (80 ms). Well inside the single hart's budget; no resampling on the device.
- **Settings**: a Sound section (volume, mute, output route) only after the
  physical validation below has passed.

## 5. Staged physical test plan for unit A

Each stage needs the previous one to have passed. Stages 1-3 open no stream
and make no sound. Stages 4 and 5 require the product owner present and
explicitly agreeing to each step.

**Stage 1 - enumerate** (silent, read-only):

```
cat /proc/asound/cards /proc/asound/devices /proc/asound/pcm
ls -l /dev/snd
cat /proc/asound/card0/pcm0p/info /proc/asound/card0/pcm0c/info
cat /proc/asound/card0/pcm0p/sub0/hw_params /proc/asound/card0/pcm0c/sub0/hw_params
ls /proc/device-tree/sound/
dmesg | grep -iE 'snd|asoc|i2s|inno|dma|audio'
```

Expect card `K230I2SINNO`, device `00-00` playback 1 capture 1, `hw_params`
reading `closed`.

**Stage 2 - mixer state** (silent, read-only; these only read cached values):

```
amixer -c 0 controls
amixer -c 0 contents
amixer -c 0 scontrols
```

Record `External I2S Output Switch` (expect on), the playback volume and
switch, the capture volume and switch.

**Stage 3 - capture path, silently** (no stream opened):
- Power off, inspect the board: is R54 fitted? Photograph it. Note the jack.
- With the owner's approval only, if debugfs is already mounted:
  `cat /sys/kernel/debug/asoc/components /sys/kernel/debug/asoc/dais`,
  `grep -E 'pin 3[2-5] ' /sys/kernel/debug/pinctrl/*/pinmux-pins`.
  Do not mount debugfs without asking.
- `gpioinfo -c gpiochip1 2 3` (read-only, libgpiod 2.x): record the
  consumers of lines 2 and 3 (GPIO34, GPIO35).
- The pad levels without touching the lines: `devmem 0x91105088 32` (IO34)
  and `devmem 0x9110508c 32` (IO35), read form only, never with a value. Bit
  31 is the pad's input level when bit 8 (input enable) is set; bits 13:11
  the function (`drivers/pinctrl/canaan/pinctrl-k230-iomux.c`). Never
  `gpioget`: it would make the line an input.

**Stage 4 - controlled microphone capture** (owner present, no headphones
plugged, the owner knows the mic is on):
1. `amixer -c 0 cset name='External I2S Output Switch' off` (route to the codec).
2. `arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 3 /tmp/mic.wav` while the
   owner speaks near the board.
3. Copy the file off and analyse per channel on the host (right channel =
   onboard mic should carry the speech; left = headset input, near silence).
4. `rm /tmp/mic.wav`. Leave the route off for stage 5 or restore it with `on`.

**Stage 5 - controlled low-volume playback** (owner present; only if R54 is
understood and the route is the codec):
1. Headphones plugged but **on the desk, not worn**.
2. `amixer -c 0 cset name='PCM Playback Volume' 0` (−39 dB). Remember the
   first DAC setup may override this with −15 dB (section 1).
3. Play a prepared 1 s, 1 kHz, −30 dBFS, S16_LE, 48 kHz stereo WAV with
   `aplay -D hw:0,0 tone.wav`. Watch the panel for flicker the whole time.
4. Owner listens from a distance; then, only if quiet, once with the earpiece
   held away from the ear.
5. `amixer -c 0 contents` again; record what the first playback changed.

Never run unattended: `aplay`/`arecord` in any form (`--dump-hw-params` also
opens the device), `amixer cset`/`sset`, `alsamixer`, `alsactl restore`/`init`,
`gpioset`/`gpioget` on lines 34-35, `audio_demo`, `audio_rec_play`, `ffmpeg`
with ALSA output, enabling `k230_phone_ui`, writes to `prealloc`.

## 6. Unit A, silent stages 1-3, 2026-09-13

Build `3d4a6e7` on unit A's v0.0.9 card, over SSH. No stream was opened, no
mixer value written, no GPIO line requested, debugfs not mounted.

| Item | Result | Evidence |
| --- | --- | --- |
| Card | `0 [K230I2SINNO]: K230_I2S_INNO`; devices control, `0-0` playback, `0-0` capture, timer; `/dev/snd` 0660 root | VERIFIED |
| PCM | `Audio 9140e000.inno_codec-0`, one playback and one capture substream, both available, `hw_params` and status `closed`; no process holds `/dev/snd` | VERIFIED |
| Device tree | `sound` compatible `canaan,k230-audio-inno`, okay, `canaan,external-i2s-output-default` present; dmesg `External I2S output enabled by default` | VERIFIED |
| Mixer (read with `amixer contents`) | `External I2S Output Switch` **on**; `PCM Playback Volume` 24 of 45 (= -15 dB); `PCM Playback Switch` on; `Mic Capture Volume` 30 of 30; `Mic Capture Switch` on; `PCM`/`PCM Switch` mirror the playback pair | VERIFIED |
| IO35 pad (`0x9110508c`) | `0x00001191`: function 2 = I2S data out, output and input enabled, no pull, **level 0** | VERIFIED |
| IO34 pad (`0x91105088`) | `0x000001b0`: function 0 = GPIO34, pull-down, level 0; gpiochip1 line 2 is an unused input | VERIFIED |
| IO32/IO33 | function 2 (I2S BCLK/WS), output enabled | VERIFIED |
| Control for the level bit | IO21 and IO22 (the board DT's two reset GPIOs, driven high while the panel runs) read level 1 | VERIFIED |
| GPIO consumers | gpiochip1 lines 2 and 3: unnamed, no consumer | VERIFIED |
| Display during all of this | on, raw brightness 153, shell running | VERIFIED |

**R54.** It is the 0 Ω bypass across Q2, the P-channel switch that
`IO35_DISEN` controls for TP_VDDIO, LCD_VDD and VBAT. With IO35 held low by
the idle I2S output and the panel powered, R54 is STRONGLY INFERRED fitted
(section 0). Nothing earlier in boot drives it high either: U-Boot's device
tree (`k230_canmv_v3`) sets IO35 as a GPIO and no U-Boot code writes that
GPIO (DOCUMENTED).

**GPIO35 is shared, on the board only.** In software it has one owner, the
I2S controller's pinctrl state; nothing else in Linux claims it. On the board
the same net is the display power enable. With R54 fitted, data toggling on
IO35 switches Q3/Q2 but not the rail, so a stream on the external route should
not cut the display (STRONGLY INFERRED).

**Is playback safe later?** Not proven. The display-power question is now
electrically settled, pending a visual check of R54. Stages 4 and 5 stay as
written: owner present, route switched to the internal codec first, lowest
gain, headphones on the desk, watching the panel for flicker. Nothing in
PocketOS opens a stream until then.

## Statement for the record

NO AUDIO WAS PLAYED. NO MICROPHONE CAPTURE WAS PERFORMED. For the static
study (sections 0-5) no mixer level was read or changed on unit A, and unit A
was not contacted. For section 6 (2026-09-13) the mixer was read, not
changed, no PCM device was opened, and no GPIO line was requested or written.
