# Audio hardware map and first audio test gate

Recorded 2026-09-13 on branch `feature/audio-ggwave` (from master `a748101`).
It extends docs/hardware/AUDIO_FEASIBILITY_2026-09-12.md.

**Revision 2 (same day):** the owner opened the second unit further and
photographed the speaker, the board it plugs into and the IC beside its
connector. This revision separates the evidence by source (§1-§4), identifies
the amplifier (§4), states the speaker path (§5), answers the GPIO35/R54
questions (§8) and records the SEND decision (§9). Revision 1 read the
main-board schematic as "no speaker path"; that reading described the main
board only and is withdrawn as a statement about the product.

**Revision 3 (same day):** the owner's photographs themselves were examined
(three images: a macro of the amplifier and its receptacles; the unit seen
from the port end with the front cover raised; the unit from the side with the
cover tilted open). They upgrade the base board's identity and the IC's
package to PHYSICALLY CONFIRMED, show that the speaker plug is not seated in
any image, and show nothing of R54 on either unit. The evidence is now
separated by unit (§0). The amplifier and R54 conclusions do not change.

**No sound was played, no microphone was opened, and no mixer control, GPIO
line or pad was written, on any board, for this record.** Unit A was not
contacted for any revision (§10).

## Evidence labels

These are kept apart on purpose; a statement carries the weakest label its
argument needs.

| Label | Meaning |
| --- | --- |
| **PHYSICALLY CONFIRMED** | Visible in the owner's photographs of unit B, examined for revision 3. A reading the owner made on the hardware that is not legible in the images is marked "owner's reading" |
| **LIVE-READ** | Read, read-only, from the running unit A (procfs, mixer, pad registers, dmesg) |
| **VENDOR-DOCUMENTED** | LILYGO's documents: the V1.0 main-board schematic, BSP docs, product docs, repository READMEs. For the parts: their datasheet content, which reached this session only through secondary copies (ADI's own PDF timed out) |
| **SOURCE-CONFIRMED** | Code that runs: device tree, kernel driver, BSP patches, the vendor launcher, pocketaudio |
| **STRONGLY INFERRED** | Follows from the labels above by the argument given next to it; not observed |
| **UNKNOWN** | Not established |

Two units are involved and must not be mixed up: **unit A** is the unit every
live read comes from; **unit B** is the second, opened unit every photograph
comes from. That they are the same product revision is **not established**.

Path shorthand: `SCH` = `vendor/T-Display-K230_canmv_rt/schematic/T-Display
K230_V1.0_NEW.pdf` (V1.0, dated 2026-06-26), `K` = the pinned kernel build
tree, `L` = `vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/`,
`BSP` = `vendor/T-Display-K230/k230_bsp/` (clone `bb831ab`).

## 0. Unit by unit

Nothing is carried from one unit to the other. The V1.0 schematic, the vendor
sources and the PocketOS code describe the design; they are not evidence of
which parts either unit carries.

### Unit A - the deployed PocketOS test unit (live reads, no photographs)

| Item | State | Label |
| --- | --- | --- |
| Live register evidence | 2026-09-13 morning, read-only: IO35 pad `0x00001191` (I2S data out, level 0) with the panel on; IO34 pad `0x000001b0` (GPIO, pull-down, level 0); gpiochip1 lines 2 and 3 without consumer; IO32/IO33 in I2S function; IO21/IO22 read 1 as the level-bit control (feasibility §6) | LIVE-READ |
| Deployed software | Last recorded deploy: userspace build `3d4a6e7` on the v0.0.9 card (2026-09-13). No audio-branch build is deployed and `pos-wave` is not on the unit; package `da3c5e5` is built, not deployed | bench record |
| Audio enumeration | Card 0 `K230I2SINNO`, `hw:0,0` with one playback and one capture substream, both closed; `canaan,external-i2s-output-default` in the DT, confirmed by dmesg; `External I2S Output Switch` on, `PCM Playback Volume` 24/45, `Mic Capture Volume` 30/30. The §10 re-read is outstanding | LIVE-READ (morning) |
| R54 | Not seen. Fitted is STRONGLY INFERRED by the §8.3 chain, whose residual premise is that unit A's main board matches V1.0 | STRONGLY INFERRED |
| Base board / speaker | A keyboard base is attached and answers (TCA8418 at 0x34, XL9555 at 0x20, on the bit-banged GPIO46/47 bus; KEYBOARD_BRINGUP_2026-09-10.md §3); the vendor documents that board as the nRF9151 cellular/GNSS/keyboard base, which carries no amplifier. An nRF52840 base board was only ASSUMED on 2026-09-07 from a port diagram; no probe of its AHT20 on the bit-banged bus is recorded; the unit has not been opened. No amplifier or speaker has been observed | keyboard base LIVE-READ; nRF52840 board, amplifier and speaker UNKNOWN |

### Unit B - the opened, photographed unit (photographs, no live reads)

| Item | State | Label |
| --- | --- | --- |
| Photographed base board | A black board over the main board, silkscreen legible as **`K230_nRF52840_Board`, `VER:0.3`, `20260407`**, carrying an nRF52840 (top line legible as `N52840`) and a 2x20 header row | PHYSICALLY CONFIRMED |
| Main board beneath | Seen only through the base board's cutouts: text legible as `…230 V1.0`, and a Lontium LT9611 (the V1.0 schematic has one, U18). A revision marking, not a parts list: R54 is a population option on that revision | PHYSICALLY CONFIRMED (partial) |
| Photographed speaker | A rectangular body in the front cover with a red/black lead. Its marking is not legible in the images | body and lead PHYSICALLY CONFIRMED; `TR-WS-2014B` owner's reading |
| Photographed connector | Two white 2-pin receptacles on the base board, one above and one below the amplifier in the macro; `2618` in silkscreen beside one of them (side view). The speaker's white 2-pin plug lies beside the board and is **not seated in any receptacle in any image**, so which receptacle is the speaker's is not shown | receptacles and plug PHYSICALLY CONFIRMED; speaker receptacle UNKNOWN |
| Photographed amplifier | A QFN with **16 terminals, four per side**, between the two receptacles, with a column of passives beside it | PHYSICALLY CONFIRMED |
| IC marking | **`AKK` / `NKI` / `+`**, legible in the macro | PHYSICALLY CONFIRMED |
| Identification | MAX98357A (§4) | CONFIRMED WITH HIGH CONFIDENCE |
| That the macro shows the labelled board | The same arrangement - two white 2-pin receptacles with a small IC between them - sits at the port end of the labelled board in both overview images; the macro's frame does not include the label | STRONGLY INFERRED |
| R54 | The main board's display-power area is covered by the base board in every image | UNKNOWN |
| Live evidence, software | None recorded: no PocketOS build deployed or read on this unit | UNKNOWN |

### The five questions

1. **Confirmed on both units:** no hardware fact. The owner describes them as
   the same product, which is not verified part by part. What applies to both
   is documentary (the V1.0 schematic, the vendor BSP and device tree, the
   PocketOS gates), and it describes the design, not either unit's parts.
2. **Confirmed only on unit A:** the running audio stack (card, PCM, mixer,
   default external route); the pad states - IO35 low with the panel
   powered, IO34 low with pull-down - and with them the R54 inference; the
   attached keyboard base.
3. **Confirmed only on unit B:** the `K230_nRF52840_Board` VER 0.3 base board
   with its nRF52840; the 16-terminal `AKK` amplifier IC and the two 2-pin
   receptacles beside it; `2618`; the speaker body, its red/black lead and its
   plug; a main board marked V1.0 with an LT9611.
4. **Still to check before SEND on unit A:** open it and see whether it
   carries an nRF52840 base board with the amplifier and a speaker at all;
   photograph R54 on its main board (§8.7), which on unit B's layout means
   lifting any base board off the header; see the speaker plug seated in the
   amplifier's output receptacle; deploy a build with pos-wave and repeat the
   §10 reads after reassembly; then owner approval of §12.
5. **Unit B as the first SEND unit:** a better candidate than unit A,
   provided the same gate is closed **on unit B itself**. It already has the
   base board and the amplifier physically confirmed, and it is already open,
   so R54 can be seen directly instead of inferred. Before SEND on unit B:
   - photograph R54 on unit B's main board (lift the base board; §8.7);
   - identify the amplifier's output receptacle, by a photograph of the plug
     seated where it came from or, powered off, continuity from the receptacle
     pins to the IC's output pads, and seat the plug there. The two
     receptacles look identical, and the other one's purpose is unknown: a
     speaker plugged into a receptacle carrying DC would burn its coil;
   - seat the base board fully on the header, and remove the loose fibre
     lying across the board beside the IC in the macro;
   - deploy a build containing pos-wave and run the §10 read-only enumeration
     **on unit B**, including IO35 low with the panel on, which gives unit B
     its own §8.3 chain to back the photograph;
   - the owner present, the §12 preconditions, approval.

   It is not safe today: none of those is done, and no software has run on
   it. RECEIVE stays on unit A.

## 1. Photographed hardware (unit B)

What the three images show, beyond §0:

- **Macro.** The two receptacles' contacts suggest a 1.25 mm pitch; at that
  scale the 16-terminal QFN is about 3 mm on a side, consistent with the
  TQFN-16 3 x 3 mm package (an estimate from the image, not a measurement).
  Two identical two-terminal parts sit between the IC's upper side and the
  upper receptacle, which would fit an output filter to that receptacle; their
  function and nets are not visible. Several footprints beside the IC appear
  unpopulated (bare pad pairs); if one is the GAIN_SLOT strap, an open strap
  would mean 9 dB, but nothing in the image shows which net any pad is on. The
  upper receptacle is visibly empty (contacts showing). A thin loose fibre lies
  on the board beside the IC's right-hand and lower terminals.
- **Port-end view.** The nRF52840 near the far end of the base board; through
  the cutouts, the main board's heat spreader, the `…230 V1.0` text and the
  LT9611; at the port end, the receptacle-IC-receptacle group. The speaker
  lead runs in the raised front cover.
- **Side view.** The board label; `2618` beside the left receptacle of the
  port-end pair; text legible as `AUDIO1` and `MIC` along the port-end edge
  (AUDIO1 is the V1.0 main board's designator for the 3.5 mm jack; which
  board carries the text is not resolvable, so it is not used); the speaker
  body in the tilted cover.

Not visible in any image: the speaker's marking, the plug seated, which wire
lands on which pin, the receptacles' designators, the IC's pin 1 orientation
relative to its nets, the supply parts, the header mating, and the main
board's display-power switch (Q2, Q3, R54).

What a TR-WS-2014B is, from a reseller sheet for the part number
(techiesms.com): 20 x 14 x 4.5 mm cavity speaker, **7.2 Ω ± 10 %**, **1.0 W
rated, 1.5 W maximum**, 94.5 ± 2 dB at 3.1 V / 10 cm / 2 kHz, resonance
900 Hz ± 10 %. VENDOR-DOCUMENTED for the part number; that the fitted part
meets that sheet is STRONGLY INFERRED from the owner's reading of the marking,
which the images do not resolve.

## 2. Main-board schematic evidence (V1.0)

All VENDOR-DOCUMENTED (`SCH`), unless marked.

- **The main board carries no speaker amplifier and no speaker connector.**
  Its only analog audio outputs are `HP_OUTL`/`HP_OUTR` (K230 balls G1/G2)
  through C167/C166 47 µF to the 4-pin jack AUDIO1 (PJ-3220). This is a
  statement about the main board. **It says nothing about base boards, and
  "the product has no speaker" must not be read from it** - unit B has one
  (§1).
- **The signals that could reach an external amplifier are digital**: the
  40-pin header JP1 carries IO32, IO33, IO34 and IO35 (IO35 on pin 9) and no
  analog audio net.
- **The display power switch** (sheet "Video", with the display FPC connector
  J1, the camera FPCs and the K230's MIPI block). Netlist groups:

  | Node | Members |
  | --- | --- |
  | VDD_3V3 | Q2 pin 2 (source), R54 pin 1, R99 pin 2, R93 pin 1 |
  | Q2 gate | Q2 pin 1, Q3 pin 3 (collector), R99 pin 1 |
  | Switched node | Q2 pin 3 (drain), R54 pin 2, R92 pin 1, R94 pin 1, R98 pin 1 |
  | Q3 base | R114 4.7 kΩ from `DIS_EN` = `IO35_DISEN` = K230 GPIO35 (ball W12, also JP1 pin 9); Q3 emitter to GND |
  | LCD_VDD | R94 0 Ω, J1 pins 13 and 14, C223 4.7 µF, C224 10 µF |
  | TP_VDDIO / VDDIO | R92 0 Ω, J1 pins 6 and 12, C221 4.7 µF, C222 10 µF |
  | VBAT (local net on this sheet) | R98 0 Ω, J1 pins 15A and 16A, C225 4.7 µF, C226 10 µF |
  | TP_VDD | R93 0 Ω **straight from VDD_3V3** (not switched), J1 pin 7, TP2 |

  Parts: Q2 AO3401A (P-channel MOSFET, high side), Q3 S8050 (NPN), R99 10 kΩ
  gate pull-up, R114 4.7 kΩ base resistor, **R54 `NC/0R`** across Q2 source
  and drain. J1 is a BM28B0.6-30DS/2-0.35V, 30 positions, 0.35 mm pitch.
- **No layout exists in any source**: no PCB file, Gerber, BOM, placement
  file or assembly drawing in any vendor repository listed in §3.

## 3. Base/daughter-board evidence

### 3.1 Vendor and BSP

| Source | What it says | Label |
| --- | --- | --- |
| `BSP/docs/HARDWARE_PINMAP.md:11,82-98` | An optional nRF52840 "BLE/audio/sensor base board" provides the nRF52840, an AHT20 and "the MAX98357A external I2S amplifier"; DIN = GPIO35, BCLK = GPIO32, LRCK = GPIO33, shutdown = GPIO34, high enables the amplifier | VENDOR-DOCUMENTED |
| `BSP/README.MD:43` | "MAX98357A external I2S amplifier pins and output switch support" | VENDOR-DOCUMENTED |
| BSP patch `0058-riscv-dts-rm69a10-add-audio-fan-sensor-pins.patch:9-20` | Pins as above; the MAX98357A is not a second ALSA card: it listens to the same I2S stream and is gated by GPIO34 from userspace | SOURCE-CONFIRMED |
| BSP patch `0059-asoc-canaan-add-external-i2s-output-switch.patch:6-19` | The RM69A10 board revision "can use an external MAX98357A amplifier on the K230 I2S pads"; adds the mixer switch that sets `audio_codec_bypass`; RM69A10 defaults to the external route "so the phone UI can drive the MAX98357A after boot" | SOURCE-CONFIRMED |
| `K/.../k230-canmv-rm69a10.dts:33-40,130-158,228-231` | Pin group `amp_i2s_pins` (IO32 `IIS_CLK`, IO33 `IIS_WS`, IO35 `IIS_D_OUT0`), IO34 a GPIO output named `amp_shutdown`, `canaan,external-i2s-output-default` | SOURCE-CONFIRMED |
| `K/sound/soc/canaan/canaan_k230_inno.c:40-45,90-99,116` | The switch sets the bypass bit and re-applies it on every stream start; the link's DAI format is `SND_SOC_DAIFMT_I2S` | SOURCE-CONFIRMED |
| `L/ui_hardware.c:1655-1769,6299-6303`, `L/ui_i2s_test.c:21,246-249`, `k230_launcher/docs/APPS.md:67` | "External speaker" = the switch on plus gpiochip1 line 2 (GPIO34) high; "Headphones" = switch off, GPIO34 low; the I2S test page is titled MAX98357A and plays S16_LE 48 kHz stereo; the external speaker is the default only when the base board's AHT20 answers | SOURCE-CONFIRMED |
| LILYGO documentation repo, `en/products/t-display-series/t-display-k230/cased-version.md` | The cased version's audio row lists microphone, speaker and 3.5 mm headphone jack; its expansion row lists an nRF52840 GPIO expansion header | VENDOR-DOCUMENTED |
| `Xinyuan-LilyGO/T-Display-K230-nRF52840` README (tree `4646a72`) | Calls it a "secondary board"; nRF52840 P0.04 enables a 5 V boost from the 21700 battery compartment that supplies the host. Nothing about the amplifier, speaker, I2S, rails or revision | VENDOR-DOCUMENTED |
| linuxgizmos.com article on the K230 kit | Speaker driven through a MAX98357A | secondary press, not vendor; supporting only |

Unit A: dmesg `External I2S output enabled by default`, mixer switch on
(LIVE-READ, feasibility §6).

### 3.2 Searched and not found

**No schematic, PCB, BOM, revision list, connector designator or amplifier
circuit for any base board was found.** Searched:

- `vendor/T-Display-K230` (BSP, patches, launcher, SDK docs): pin names and
  software only. No other amplifier part is named anywhere in the BSP or the
  launcher (searched for MAX98357B, NS4168, NS4150, NS8002, PAM8xxx, TPA2xxx,
  AW8xxx, SC8002, LTK8002, CS4344, PCM510x, ES8311, ES8388, TAS27xx).
- `vendor/T-Display-K230_canmv_rt` (`abb0709`): `schematic/` holds only the
  V1.0 main board; `datasheet/` holds K230, nRF52840, nRF9151, BQ25896,
  BQ27220, TCA8418, RTL8189FTV and HPD16A sheets, no MAX98357A.
- `Xinyuan-LilyGO/T-Display-K230-nRF52840` (tree `4646a72`): bootloader,
  firmware, AT docs, one BSP patch, images `top.png`, `bottom.png`,
  `left.png`, `pins_annotated.png`. No schematic. The images were **not
  examined** (not downloaded); they may show the vendor's layout of the
  amplifier and connector and would be a cheap cross-check against §1.
- `Xinyuan-LilyGO/T-Display-K230-nRF9151` (tree `ed4ecbe`): no schematic.
- LILYGO documentation repository: product text only.
- Vendor product images (`T-Display-K230/images/top.png`: the cased front;
  `T-Display-K230_canmv_rt/image/k230_display_product.png`: the main board's
  back at low resolution): no designator is legible.

### 3.3 Physical (unit B)

§0, §1: a separate board over the main board carries two white 2-pin
receptacles and the `AKK` IC between them. PHYSICALLY CONFIRMED.

**Which board it is** (upgraded in revision 3): its silkscreen reads
`K230_nRF52840_Board`, `VER:0.3`, `20260407`, and it carries an nRF52840.
PHYSICALLY CONFIRMED. That is the board the vendor documents with the
MAX98357A (§3.1), so the vendor's documentation now attaches to a named,
photographed board rather than to a guess. No document for revision 0.3
exists (§3.2). The interconnect is STRONGLY INFERRED to be JP1, the only
documented connector carrying IO32-IO35 off the main board; the header
mating itself is not visible.

## 4. Amplifier: MAX98357A, CONFIRMED WITH HIGH CONFIDENCE (PHYSICAL + VENDOR EVIDENCE)

Unit B only. Upgraded from "INFERRED" in revision 2; revision 3 checked the
photographs and found them consistent, adding lines 1a and 1b. The lines of
evidence, and what each is worth alone:

| # | Evidence | Label | Worth alone |
| --- | --- | --- | --- |
| 1 | An IC between the two white 2-pin receptacles on the separate board | PHYSICALLY CONFIRMED | Shows a driver sits beside the receptacles; names nothing |
| 1a | The board is the `K230_nRF52840_Board` (VER 0.3) | PHYSICALLY CONFIRMED | Ties line 3 to this very board |
| 1b | The IC is a QFN with 16 terminals, four per side, about 3 mm on a side by image scale | PHYSICALLY CONFIRMED (terminal count); size estimated | Matches the TQFN-16 package the `AKK` listings describe; rules out the 9-bump WLP |
| 2 | Top marking `AKK` / `NKI` / `+`, legible in the macro | PHYSICALLY CONFIRMED (marking); decoding STRONGLY INFERRED | Marketplace listings (Amazon, Alibaba) sell MAX98357AETE+T, the TQFN-16 3 x 3 mm part, with `AKK` given as its marking. ADI's own marking table was not retrieved, so this is secondary. `NKI` fits the lot/date line and `+` Maxim's lead-free mark |
| 3 | Pinmap names MAX98357A on the base board, with the four pins | VENDOR-DOCUMENTED | Names the part, but on paper |
| 4 | BSP patches 0058/0059 name MAX98357A and add the bypass route for it | SOURCE-CONFIRMED | Vendor engineering intent in shipped code |
| 5 | Device tree muxes exactly those pins as `amp_i2s_pins` + `amp_shutdown` | SOURCE-CONFIRMED | Consistent topology |
| 6 | Launcher drives exactly that route for "External speaker", titles its I2S test MAX98357A, and picks it when the base board is detected | SOURCE-CONFIRMED | The vendor's product uses it |
| 7 | The link is standard I2S (`SND_SOC_DAIFMT_I2S`) | SOURCE-CONFIRMED | Matches the **A** variant; the B variant expects left-justified data |
| 8 | The cased version is documented with a speaker; press coverage names MAX98357A | VENDOR-DOCUMENTED; press secondary | Supporting |

**Why the upgrade is justified.** Lines 3-8 all come from LILYGO and could
share one documentation error; lines 1, 1b and 2 are independent of LILYGO,
come from the hardware itself, and point at the same part, and line 1a puts
LILYGO's naming of the part on the photographed board. For the conclusion to
be wrong, a different 16-terminal QFN carrying the same `AKK` code would have
to sit beside those receptacles on the very board the vendor says carries a
MAX98357A. Nothing
supports that, and nothing contradicts the identification: no other
amplifier appears in any source (§3.2), the main board's codec outputs go
only to the jack (§2), and the vendor's software drives precisely a
MAX98357A-style interface (I2S in, one shutdown line, no control bus).

**What stays unconfirmed about the part:** its exact size (estimated from the
image, not measured), the gain strap, the supply rail, the SD_MODE wiring and
which receptacle its outputs reach (§6). **Nothing about unit A's amplifier is
confirmed**: unit A has not been opened (§0).

## 5. The built-in speaker path

The path proposed in the brief is **confirmed**, with one precision: the
amplifier is on the separate base board, reached through the main board's
header, not on the K230 main board.

```
K230 DesignWare I2S (0x9140f000)
  INNO codec bypassed: "External I2S Output Switch" = on   (boot default; LIVE-READ on unit A only)
    IO32 BCLK     IO33 LRCK     IO35 SDATA               IO34 GPIO, gpiochip1 line 2, high = on
       |             |             |                           |
  =====+=============+=============+===========================+=====  40-pin header JP1 (main board)
       |             |             |                           |       JP1 -> base board: STRONGLY INFERRED
  -----v-------------v-------------v---------------------------v-----  K230_nRF52840_Board VER:0.3 (PHYSICALLY CONFIRMED on unit B only)
     BCLK          LRCLK          DIN      MAX98357A        SD_MODE
                             (unit B: 16-terminal QFN "AKK"; CONFIRMED WITH HIGH CONFIDENCE)
                              OUTP  --+          +--  OUTN      BTL: both terminals switch,
                                      |          |              neither is ground
                white 2-pin receptacle: one of the two beside the IC (which one UNKNOWN;
                designator UNKNOWN; pin order UNKNOWN; plug not seen seated)
                                      |          |
                              red / black leads (a colour says nothing about polarity here)
                                      |          |
                                 TR-WS-2014B (owner's reading), 7.2 Ω, 1 W rated
```

The diagram is the design path. Its physical parts are confirmed on unit B
only, its live reads on unit A only (§0).

- **The INNO codec is not in the speaker path.** With the route on, the SoC
  bypasses it and sends I2S to the pads. The codec remains the path for the
  on-board microphone, the headset microphone and the 3.5 mm headphone jack
  (§7).
- **Neither speaker conductor is ground.** A MAX98357A drives a bridge-tied
  load from two switching outputs (VENDOR-DOCUMENTED for the part). That the
  speaker's receptacle carries those two outputs is STRONGLY INFERRED (the IC
  sits between the two receptacles; no trace was followed; which receptacle is
  the speaker's is UNKNOWN). Nobody should tie the black lead, or
  either pin, to ground or measure it against ground as if it were one.
- GPIO35 is in this path (serial data). GPIO34 enables it.

## 6. Connector and amplifier details

| Question | Answer | Label |
| --- | --- | --- |
| Which receptacle | Two identical white 2-pin receptacles sit beside the IC (unit B). The plug is not seated in any image. Two identical parts between the IC and the upper receptacle would fit an output filter to it; that is appearance, not a traced net | receptacles PHYSICALLY CONFIRMED; which one UNKNOWN |
| The other receptacle | Purpose not documented and not visible | UNKNOWN |
| Connector designator | Not legible in the images; no document names it | UNKNOWN |
| Connector pinout | Two pins, OUTP and OUTN in some order | order UNKNOWN; OUTP/OUTN STRONGLY INFERRED |
| Wire colours | Red and black | PHYSICALLY CONFIRMED; no polarity meaning |
| Output type | Filterless class D, bridge-tied (BTL) | VENDOR-DOCUMENTED (part) |
| Amplifier supply rail | The part runs from 2.5 to 5.5 V. The nRF52840 board has a 5 V boost from its 21700 battery that supplies the host; whether the amplifier sits on that 5 V, on a battery rail or on 3.3 V is not documented | UNKNOWN (part range VENDOR-DOCUMENTED) |
| Gain | Set by the GAIN_SLOT pin's strap: 15, 12, 9 (open), 6 or 3 dB. The macro shows some bare footprints beside the IC, but not which net any of them is on | UNKNOWN |
| SD_MODE wiring | Vendor: GPIO34 is the shutdown control, high = on. Direct, through a resistor, or with a pull resistor on the board: not documented | function VENDOR-DOCUMENTED; wiring UNKNOWN |
| Is IO34 treated as a direct enable | Yes, by all software: DTS GPIO `amp_shutdown`, launcher line 2 high/low, pocketaudio line 2 high only while a playback stream is open | SOURCE-CONFIRMED |
| Channel mode | SD_MODE above 1.4 V selects left; a resistor to SD_MODE can select right or (L+R)/2. Irrelevant here: pocketaudio writes the same mono sample to both slots | part VENDOR-DOCUMENTED; L = R SOURCE-CONFIRMED |
| IO34 at rest on unit A | GPIO function, pad pull-down, level 0, no consumer | LIVE-READ |
| Is the amplifier powered continuously | Its supply: UNKNOWN. Its enable: low at rest (above), so with the vendor's wiring it is in shutdown whenever no playback holds line 2 | supply UNKNOWN; shutdown at rest STRONGLY INFERRED |
| Output power into 7.2 Ω | Ideal BTL sine limit VDD²/2R: 1.74 W at 5.0 V, 0.95 W at 3.7 V, 0.76 W at 3.3 V (losses ignored). Part rating about 1.8 W into 8 Ω at 5 V (secondary copy) | arithmetic; rail UNKNOWN |
| Against the speaker | On a 5 V rail the amplifier can exceed the speaker's 1 W rating. The level must stay digitally limited (§11) | STRONGLY INFERRED |
| Board filtering (ferrite beads, capacitors at the connector) | Passives are visible beside the IC (unit B); their values and nets are not | UNKNOWN |
| Protection | The part documents thermal and output short-circuit protection and click/pop suppression; nothing is known about board-level protection | part VENDOR-DOCUMENTED; board UNKNOWN |
| What `2618` is | - | UNKNOWN |

## 7. Playback and microphone paths

| | Built-in speaker | 3.5 mm jack |
| --- | --- | --- |
| Route switch | `External I2S Output Switch` = on (boot default) | = off |
| Signal | I2S on IO32/33/35 -> JP1 -> MAX98357A on the base board | codec DAC -> `HP_OUTL/R` -> 47 µF -> AUDIO1 |
| Level control | **Digital only**; `PCM Playback Volume` does not apply | `PCM Playback Volume` 0-45 (-39..+6 dB) |
| Enable | IO34 high | none (the driver enables the headphone driver on every playback, whatever the route) |
| GPIO35 | **in the path** (serial data) | not in the path |
| Used by Wave | SEND | no |

**Microphones** (unchanged from revision 1, VENDOR-DOCUMENTED unless marked):

- On-board mic MIC1 `B4012AP422-003`, biased from `MIC_BIAS` through R52
  2 kΩ (R53 2 kΩ, C172 10 nF), coupled by C171 4.7 µF to `MIC_PR` (ball F1),
  the codec's **right** ADC input; `MIC_NR` AC-grounded by C173.
- Headset mic on jack pin 1, C165 4.7 µF to `MIC_PL` (E1), the **left** ADC;
  bias R50 2 kΩ.
- ALSA: card 0 `K230I2SINNO`, PCM `hw:0,0`, one capture substream
  (LIVE-READ). pocketaudio opens `hw:CARD=K230I2SINNO,DEV=0`.
- Slot mapping: right = on-board, left = headset. STRONGLY INFERRED from the
  nets and I2S slot order; the first RECEIVE test checks it (§13 step 2).
- Capture needs the route **off**; the vendor turns it off before every
  capture (`L/ui_hardware.c:1771-1789`), and so does pocketaudio.
- `Mic Capture Volume` writes only the left gain; the on-board mic stays at
  the driver's 30 dB. The codec's ALC is enabled with a fixed gain word and
  its adaptive behaviour is UNKNOWN. Mic bias is switched on at the first
  capture and never off (`inno_k230_reg.c:49-65,250-266,396-406,563`).

## 8. GPIO35 and R54

### 8.1 What GPIO35 does

On the pad, one signal; on the board, two loads. The device tree gives IO35 to
the I2S controller as serial data out (SOURCE-CONFIRMED); nothing else in
Linux claims it. On the main board the same net, `IO35_DISEN`, feeds R114 into
Q3's base, which drives the display power switch Q2 (§2). On the base board it
is the amplifier's DIN (§5). During a playback it toggles at bit rate; with
no stream, or during a capture, the I2S output idles low (LIVE-READ on unit
A: pad `0x9110508c` = `0x00001191`, function 2, level 0).

The polarity as drawn: **IO35 high** -> base current through R114 -> Q3 on
-> Q2's gate pulled to ground -> **Q2 on**. **IO35 low** -> Q3 off -> R99
holds the gate at the source -> **Q2 off**. High enables the rail, as the
name `DISEN` says.

### 8.2 Question 1: R54 not fitted and GPIO35 toggling at I2S rate

- **At rest** (IO35 low, which is the whole time no playback runs): Q2 off,
  and nothing else feeds the switched node. LCD_VDD, VDDIO/TP_VDDIO and the
  connector's VBAT pins are unpowered; only TP_VDD, the touch controller's
  core supply, stays up. The panel cannot run. No U-Boot code drives IO35
  (feasibility §6), so such a board would show no picture once Linux holds
  IO35 as idle I2S data.
- **During playback**: each high bit saturates Q3 and pulls Q2's gate down
  hard; each low bit leaves only R99 (10 kΩ, at most 0.33 mA) to charge the
  gate back. A small P-MOSFET's gate charge is of the order of nanocoulombs
  (the AO3401A sheet is not in the repository), so turn-off takes tens of
  microseconds or more, against I2S bit periods well under one microsecond
  and a 20.8 µs sample period. Q2's gate would therefore follow a smoothed
  version of the data's density of ones: off in digital silence, more or
  less on while a signal plays, and **in its linear region in between**. The
  panel supply would rise, sag and ripple with the audio content: brown-outs,
  flicker, resets, a touch controller whose I/O rail collapses under a live
  core supply, panel I/O pins driven while the panel is unpowered, and heat in
  Q2. Not necessarily destructive, but uncontrolled.
- On unit A this case is contradicted outright by §8.3.

### 8.3 Question 2: does the live evidence logically prove R54 fitted?

Premises:

- **P1** Unit A's main board matches `SCH` around Q2, Q3, R54, R99, R114 and
  R92/R94/R98: same parts, same connections, same polarity.
- **P2** IO35 is really low: the pad register's level bit reads 0 (LIVE-READ),
  and the same bit reads 1 on IO21 and IO22, which the panel driver holds
  high (LIVE-READ control, feasibility §6).
- **P3** The panel runs from the switched node: it was on, at raw brightness
  153, with the shell running (LIVE-READ), and LCD_VDD's only source on `SCH`
  is R94 from that node.
- **P4** Q2 and Q3 are healthy (neither shorted nor leaking).

Chain: P2 -> IO35 at 0 V -> no base current -> Q3 off (P1, P4) -> R99 holds
Q2's gate at its source -> Q2 off (P4) -> the switched node's only remaining
source on `SCH` is R54 (P1) -> the panel is powered (P3) -> **R54 conducts**.

**Answer: not unconditionally.** It proves that *something* carries VDD_3V3
to the switched node while Q2 is off. Under P1 and P4 that something can only
be R54, so R54 is fitted. Without P1 and P4, the same reading is produced by
a solder bridge across Q2, a Q2 failed short, a Q3 failed short, or a board
revision without the switch. R54 fitted stays **STRONGLY INFERRED**.

**A stronger result that does not need R54 itself.** By the polarity in
§8.1, IO35 going high can only *add* conduction (Q3 on, gate to ground, Q2
on); it cannot remove whatever keeps the rail up at IO35 low. So under P1
(polarity) and P2 alone, **no pattern on IO35 can cut the display rail on
unit A**. Every explanation of the reading above is safe for SEND in this
sense: R54, a bridge, a shorted Q2 or Q3, no switch at all. The explanations
that would not be safe are an undocumented revision with the *opposite*
polarity (IO35 low = rail on, with no bypass) and IO35 not really being low.
P2 is covered by its control. P1 is the residual premise, and only the board
can settle it. Circumstantial support for P1 on vendor-built units: the vendor
drives its own MAX98357A playback through IO35 by default (§3.1), which would
blank the display on every such unit if the polarity were reversed.

### 8.4 Question 3: is visual confirmation still required?

**Yes.** The owner's gate requires R54 to be identified physically. P1 cannot
be checked from any file, and unit B's photographs say nothing about unit A's
main board. Also, whichever unit runs the first SEND must be the unit
inspected: unit A has the live evidence but no photographs; unit B has the
photographs of its base board but no live reads and no photograph of its R54.
Revision 3 checked every image: the base board covers the main board's
display-power area in all of them, so R54 is UNKNOWN on unit B, and the V1.0
marking seen on unit B's main board does not help, because R54 (`NC/0R`) is a
population option within that revision.

### 8.5 Question 4: can R54's location be found from the files?

**No.** There is no PCB, Gerber, BOM, placement file or assembly drawing in
any source (§3.2), and the vendor product images do not resolve designators.
The schematic gives only electrical neighbours. What follows from the
circuit, STRONGLY INFERRED: R54 joins Q2's source and drain, so its pads sit
beside Q2 and share copper with it; R99 and Q3 are close to Q2's gate. That
this cluster sits near the display connector J1 is **layout convention, not
evidence**: its outputs go only to J1's supply pins through R92/R94/R98 and
the six decoupling capacitors, but a designer could place it anywhere.

### 8.6 Question 5: could anything else keep the display powered?

- **Per `SCH`, no.** LCD_VDD comes only through R94, VDDIO/TP_VDDIO only
  through R92, the connector's VBAT pins only through R98, all from the
  switched node, whose only sources are Q2 and R54. The one unswitched supply
  pin is TP_VDD (J1 pin 7, through R93), the touch controller's core, which
  cannot run the panel.
- **Back-powering through signal pins:** the two reset lines the device tree
  holds high (IO21, IO22: level 1, LIVE-READ), the touch I2C lines and
  interrupt, and the DSI
  lanes can leak into an unpowered panel's I/O rail through its input
  protection diodes. That can hold a fraction of VDDIO; it cannot supply
  LCD_VDD for an AMOLED panel drawing tens of milliamps and showing a stable
  picture. STRONGLY INFERRED.
- **Outside `SCH`:** a different board revision, a rework or bridge, a
  failed-short transistor. None can be excluded from files; all are exactly
  what the photograph in §8.7 would show.

### 8.7 What to photograph to close it

**Unit:** the unit that will run the first SEND. Unit B is already open and
has its base board and amplifier confirmed (§0, question 5); unit A has the
live reads but has not been opened. Whichever is chosen also needs the §10
reads run on it.

**Board:** the **K230 main board**, not the base board. It is the board with
the K230 SoC, the camera FPC connectors and the display's 30-pin FPC
connector **J1** (0.35 mm pitch; the panel's flex cable lands there). On unit
B the base board covers it in every image, so the base board has to be lifted
off the header, powered off, noting its orientation so it goes back on the
same pins.

**What to look for, both sides of the board:**

- **Q2** and **Q3**: two 3-lead SOT-23 transistors near each other. (Marking
  codes vary by maker; do not identify them by marking alone.)
- **R54**: a chip resistor whose two pads connect to Q2's drain (pin 3, the
  lead alone on one side of the package) and Q2's source (pin 2, on the
  two-lead side, the same net as R99's far end). A 0 Ω part is usually a
  black body marked `0` or `000`, or unmarked at 0402. **Fitted** = a part
  soldered there; **not fitted** = two bare pads.
- Around them: **R99** (10 kΩ, `103` if marked) from Q2's gate to its source;
  **R114** (4.7 kΩ, `472` if marked) at Q3's base; three 0 Ω links **R92,
  R94, R98** and **R93** leading towards J1's supply pins; three capacitor
  pairs (4.7 µF + 10 µF: **C221/C222, C223/C224, C225/C226**) beside J1's
  supply pins.
- Silkscreen designators Q2, Q3, R54, if printed (dense boards often omit
  them).

**How:** powered off and battery disconnected; one photograph wide enough to
place the cluster relative to J1, and macro photographs in which individual
resistor pads are resolvable, lit at a low angle so solder fillets show; a
ruler in frame. Both sides of the board, because the cluster may be on the
back.

**Supporting electrical check (optional, powered off, battery out):**
resistance between Q2 pin 2 and pin 3, or between a VDD_3V3 capacitor and
C224's non-ground terminal. With R54 fitted as 0 Ω: under 1 Ω. Without it:
clearly more (loads and Q2's body diode). A short proves a bypass path but not
which part it is; the photograph does.

## 9. SEND gate decision

The owner's rule (revision 3): `playback_verified` stays 0 until **the
selected SEND unit itself** has its speaker and base board confirmed, its
amplifier confirmed, and R54/display-power safety closed.

| Condition, per unit | Unit A | Unit B |
| --- | --- | --- |
| Base board and speaker confirmed | **No**: not opened; only a keyboard base is known to be attached | **Yes** for the base board and the speaker body; **no** for the plug seated in the amplifier's output receptacle |
| Amplifier confirmed | **No** | **Yes**: MAX98357A, CONFIRMED WITH HIGH CONFIDENCE (§4) |
| R54 / display-power safety closed | **No**: STRONGLY INFERRED fitted (§8.3), not seen | **No**: UNKNOWN, not photographed, no live reads |
| Software able to run the test | **No**: no pos-wave deployed | **No**: nothing recorded as deployed |

Also open on both: the gain strap and supply rail (§6), so the first level
stays digitally limited (§11).

**Decision: SEND stays BLOCKED on both units.** `playback_verified = 0`
unchanged; nothing was played; `--allow-unverified` and
`POCKETOS_AUDIO_ALLOW_UNVERIFIED=playback` are not to be used for a real
send. **Recommended first SEND unit: unit B**, once §0 question 5's list is
done on it. That list is one photograph session plus one deploy away, where
unit A needs its first inspection as well. Unit A remains the RECEIVE unit.

**RECEIVE is unaffected** by all of this: a capture puts no data on IO35 and
leaves IO34 low. It proceeds as approved (§13), waiting only for unit A's
address.

## 10. Read-only live enumeration

Not run in this session: unit A's network address was not available, and
discovering it on the LAN was not permitted. What stands is the silent
enumeration of 2026-09-13 morning (feasibility §6, same unit and kernel):
card and PCM present and closed, `External I2S Output Switch` on, `PCM
Playback Volume` 24/45, `Mic Capture Volume` 30/30, both switches on, IO35 =
I2S data out at level 0, IO34 = GPIO with pull-down at level 0, gpiochip1
lines 2 and 3 unused. LIVE-READ then.

Wanted before the first test, all reads (no stream, no write, no line
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

IO4 is UART1 RX from the nRF52840 board (`uart1_nrf52840_pins`): its level
bit high with no pull configured would be weak evidence that the base board
is fitted to unit A. Looking inside is the decisive check.

If unit B is chosen for SEND, the same reads are run on unit B once software is
deployed there; unit A's results say nothing about unit B.

## 11. Format and level

- **48 000 Hz, S16_LE, 2 channels on the wire**: mono duplicated into both
  slots for playback, the right slot taken for capture. 20 ms periods, 80 ms
  playback buffer, 500 ms capture buffer. The codec enforces one rate for
  both directions; the vendor's speaker test uses exactly this format;
  ggwave's 1024-sample frames give 46.875 Hz per bin at 48 kHz, so Wave talks
  to other ggwave programs; nothing needs resampling.
- **Hard ceiling in pocketaudio: -12 dBFS** (peak 8192), 1/16 of the power
  full scale would command. Every played sample is clamped; only a code
  change raises it. Whether that ceiling alone keeps a 1 W speaker safe
  depends on the gain strap and rail, both UNKNOWN (§6), so it is not relied
  on alone.
- **ggwave level**: peak is about volume/100 of full scale (host measurement,
  tests/wave_modem_test.c): volume 10 -> 3192 (-20.2 dBFS), 25 -> 7980
  (-12.3 dBFS). pos-wave accepts 1 to 25; Wave uses 10.
- **First playback level: `--volume 5`**, peak about 1600 (-26 dBFS), 1/400
  of full-scale power. Even if full scale commanded 10 W - several times what
  the part can deliver from 5 V - that is 25 mW, against a 1 W speaker.
  STRONGLY INFERRED safe for the speaker whatever the strap.

## 12. Proposed first real SEND test (blocked: needs §9 closed and owner approval)

**Preconditions, board powered off, all on the unit that will run it:** §8.7
shows R54 fitted; the base board, amplifier and speaker are seen; the speaker
plug is seen seated in the receptacle wired to the amplifier's outputs; the
base board is fully seated on the header, the same way round; no loose fibres
or debris on the boards; nothing in the 3.5 mm jack; the owner present, the
board on the desk, nobody's ear near the speaker. Then, powered: a build
containing pos-wave, the §10 reads on this unit, and `pos-wave info` printing
`board k230-t-display` with both paths `not validated (gated)`.

`--allow-unverified` appears in step 2 only for this one approved test, once
§9 is closed; until then the owner's rule stands and it is not used for a
real send.

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
ls /run/pocketos/audio.recovery 2>/dev/null              # expected: absent
```

pocketaudio in step 2: takes the audio lock; reconciles any recovery record;
reads the route (on at boot, not written); opens and prepares the PCM; writes
the recovery record; requests gpiochip1 line 2 as an output **already high**;
plays; drains; drives line 2 low and releases it; closes the PCM; removes the
record; releases the lock. Ctrl-C stops within one 200 ms wait and runs the
same cleanup; a SIGKILL is undone by the next audio open or `pos-wave
recover`.

**Watch for:** the panel flickering or blanking (would contradict §8.3), a
click at enable, anything louder than expected. **Abort:** Ctrl-C.
**Pass:** tones heard, panel steady, exit 0, mixer unchanged, IO34 low after,
no recovery record left.

## 13. Proposed first real RECEIVE test (approved, waiting for unit A's address)

**Preconditions:** the owner present and aware the microphone will be on;
nothing in the jack; a transmitter a hand's length to an arm's length away at
a modest level - a phone ggwave app sending `DOORS`, or a laptop playing a WAV
made by `pos-wave encode --text DOORS` on the host.

```sh
# 1. state before (reads)
amixer -c 0 contents > /tmp/audio-before.txt
# 2. which slot is the on-board mic: two 5 s recordings while DOORS is sent repeatedly
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
on afterwards; the diff in step 4 should be empty. `--allow-unverified` on a
capture command opens the capture direction only; it cannot open the speaker.
Expected side effect that is the driver's: mic bias stays on after the first
capture.

**Pass:** step 2 shows the message on the right slot only (otherwise the
board entry's `capture_channel` changes before Wave is enabled), step 3
decodes at least one transmission, the mixer is back as it was.

## 14. What stays open

Carried into docs/KNOWN_ISSUES.md. The first three gate SEND:

1. R54 not seen on any unit (§8). STRONGLY INFERRED fitted on unit A.
2. On unit A: whether it carries the nRF52840 base board, amplifier and
   speaker at all (§0). On unit B: which receptacle is the speaker's, with the
   plug seen seated there (§0, §6).
3. The amplifier's gain strap and supply rail (§6); the first level rests on
   digital attenuation (§11).
4. Unit A's §10 reads are outstanding (no address available here).
5. The capture slot mapping is inferred (§7).
6. The connector's designator and pin order, the other receptacle's purpose,
   and `2618` (§1, §6). The base board's name and revision are known on unit
   B (`K230_nRF52840_Board` VER 0.3), with no document for that revision. The
   vendor's nRF52840 board images (§3.2) are an unexamined cross-check.
7. ~~After a crash or SIGKILL of pos-wave the route and IO34 keep their last
   values.~~ Closed before merge: a write-ahead recovery record, reconciled by
   the next owner of the audio lock and by a `pos-wave recover` the Wave
   session starts when its helper dies by a signal (pocketaudio.h,
   "Recovery"; tests/audio_recovery_test.sh).
8. A playback on the speaker route still enables the codec's headphone driver
   (driver behaviour): a click on the jack is possible; keep it empty.
9. ggwave's CPU cost on the C908 is unmeasured. Its worst single decode call
   on the host was 6.2 ms with one protocol and 17.9 ms with all twelve (Wave
   enables three); a spike twenty times that on the board would still fit the
   500 ms capture buffer.
10. The codec's ALC behaviour and the fixed 30 dB on-board mic gain may clip
    in a loud room; neither is adjustable through ALSA.
11. The small speaker's response across 1.9 - 6.3 kHz is unmeasured.
