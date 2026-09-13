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

**Revision 4 (same day):** the owner confirms that unit A and unit B are the
same hardware configuration, so unit B's inspection stands for unit A (§0).
RECEIVE is closed: the K230 capture startup transient is discarded and the
microphone path enabled in code (build `c688309`, §15). The SEND gate is
re-evaluated: R54 closed by evidence (§8.8), the first controlled SEND on unit
A ready for the owner's approval (§9, §12).

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

**Owner premise (revision 4): unit A and unit B are the same hardware
configuration** - the `K230_nRF52840_Board` VER 0.3, the MAX98357A, the
TR-WS-2014B speaker and its connector, and the main-board revision. It is a
project fact stated by the owner, not re-proved here, and unit A is not to be
opened to confirm it. Unit A is the deployed software unit with the live
evidence; unit B is the inspection reference. The tables below keep saying
where each piece of evidence was gathered.

Before revision 4 nothing was carried from one unit to the other; the V1.0
schematic, the vendor sources and the PocketOS code describe the design, not
which parts either unit carries.

### Unit A - the deployed PocketOS test unit (live reads, no photographs)

| Item | State | Label |
| --- | --- | --- |
| Live register evidence | 2026-09-13 morning, read-only: IO35 pad `0x00001191` (I2S data out, level 0) with the panel on; IO34 pad `0x000001b0` (GPIO, pull-down, level 0); gpiochip1 lines 2 and 3 without consumer; IO32/IO33 in I2S function; IO21/IO22 read 1 as the level-bit control (feasibility §6) | LIVE-READ |
| Deployed software | Userspace `c688309` (audio branch, microphone enabled, startup discard) over the v0.0.9 card, with `pos-wave` and Wave; rollback tars of `3d4a6e7` and `da3c5e5` in `/root/rollback-*/` (§15) | LIVE-READ |
| Audio enumeration | Card 0 `K230I2SINNO`, `hw:0,0` with one playback and one capture substream, both closed; `canaan,external-i2s-output-default` in the DT, confirmed by dmesg; `External I2S Output Switch` on, `PCM Playback Volume` 24/45, `Mic Capture Volume` 30/30. Re-read before the deploy, identical to the morning (§15) | LIVE-READ |
| RECEIVE | **Validated and closed** on 2026-09-13: the owner's phone sent `test`, Wave decoded it; slot, lifecycle, CPU, mixer restore and SIGKILL recovery checked over SSH; with `c688309` the microphone opens without any override and the startup transient is gone (§15) | LIVE-READ, owner |
| R54 | Not seen. Fitted is STRONGLY INFERRED by the §8.3 chain, whose residual premise is that unit A's main board matches V1.0 | STRONGLY INFERRED |
| Base board / speaker | **Same as unit B (owner, revision 4)**: `K230_nRF52840_Board` VER 0.3, MAX98357A, TR-WS-2014B. Never opened, so the speaker is as the factory plugged it. A keyboard base is also attached and answers (TCA8418 at 0x34, XL9555 at 0x20; KEYBOARD_BRINGUP_2026-09-10.md §3) | owner premise + unit B PHYSICALLY CONFIRMED; keyboard base LIVE-READ |

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

### The five questions (revision 3; items 1, 4 and 5 superseded by the owner premise)

Revision 4: with A and B the same hardware, unit A needs no opening; SEND runs
on unit A, and what remains for unit B is only its speaker socket and seating
before it is next powered (§9).

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
| The other receptacle | Purpose not documented and not visible. The vendor drives a fan (GPIO42 PWM) and the case back carries one, so it may be a fan or power output (§9.1) | UNKNOWN |
| TQFN pins | OUTP 9, OUTN 10, VDD 7/8, GND 3/11/15, SD_MODE 4, DIN 1, GAIN_SLOT 2, LRCLK 14, BCLK 16, N.C. 5/6/12/13, exposed pad not internally connected; pin 1 at the `+` | VENDOR-DOCUMENTED (datasheet copy) |
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
| Output power into 7.2 Ω | Ideal BTL sine limit VDD²/2R: 1.74 W at 5.0 V, 0.95 W at 3.7 V, 0.76 W at 3.3 V (losses ignored). Datasheet: 1.8 W into 8 Ω at 5 V and 0.93 W at 3.7 V, 10 % THD+N | arithmetic; datasheet VENDOR-DOCUMENTED; rail UNKNOWN |
| Against the speaker | The amplifier could exceed the speaker's 1 W rating, but not from PocketOS: the -12 dBFS ceiling is at most 0.44 W at the highest strap (§11) | VENDOR-DOCUMENTED (gain law) + arithmetic |
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
- Slot mapping: right = on-board, left = headset. **Channel 1 (right) carries
  the on-board microphone: LIVE-READ on unit A** (§15): Wave decoded the
  owner's transmission on it, and it shows room sound 14 dB above channel 0.
  That channel 0 is the empty headset input is STRONGLY INFERRED.
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

### 8.8 Revision 4: the R54 decision

Inputs added since §8.3: **the owner confirms unit A and unit B are the same
hardware configuration** (§0), so unit B's main-board marking `…230 V1.0`
(with the V1.0 LT9611 visible) describes unit A's main board; and unit A's
IO35 read was repeated at 11:36, 11:38, 11:48, 11:52, 12:27 and 12:29 UTC,
across two deploys and three shell restarts (last on build `c688309`), always
low with the panel running.

1. **Is the evidence sufficient to conclude that SEND cannot cut display
   power? Yes, with high confidence.** The conclusion does not need R54 in
   particular (§8.3): on the V1.0 topology IO35 high can only turn Q2 on,
   and unit A shows the rail up with IO35 low, so whatever holds the rail up
   stays in place whatever the I2S data does. The two premises are now
   covered: the polarity by the V1.0 schematic plus the V1.0 marking on this
   hardware configuration; the IO35 level by the pad reads and their controls
   (IO21/IO22, IO46/IO47, IO4 read 1). The vendor's own design points the
   same way: this configuration boots with IO35 as I2S data for the
   MAX98357A (BSP 0059, launcher default "External speaker"), which only
   works if the display does not hang on IO35 - exactly what the `NC/0R`
   bypass is for.
2. **If R54 were absent:** with the V1.0 polarity and nothing else bypassing
   Q2, the display supply would be off whenever IO35 is low - the panel could
   never have run under this firmware, which it has for hours - and during a
   playback the supply would chop with the data (brown-out, flicker, resets,
   Q2 half-on). If something else bypasses Q2 (a bridge, a shorted part),
   playback is exactly as safe as with R54. So "R54 absent and SEND cuts the
   display" requires unit A's observation to be false.
3. **Is a visual confirmation of R54 materially necessary for a low-volume
   controlled first SEND? No.** The residual risk - a main board that
   departs from its own V1.0 revision in the polarity of this switch - has no
   evidence for it, and its worst outcome in a 1.4 s playback watched by the
   owner is a display blink or reset, with the panel's supply restored as
   soon as IO35 idles.
4. (Not applicable: no exact unresolved risk remains that a photograph would
   remove at proportionate cost.)
5. **The R54 blocker is CLOSED**, by schematic, revision, live electrical and
   vendor-design evidence, not by sight. The first SEND keeps "the panel stays
   steady" as its confirming observation and abort criterion (§12). §8.7
   stays as the procedure should anyone want the visual check later.

## 9. SEND gate decision

**Revision 4.** Unit A and unit B are the same hardware (owner). Unit A is
the SEND unit: it carries the software, its live evidence, and its factory
assembly - no record has it opened, so its speaker should still be plugged
where the factory put it (the owner confirms this before the SEND, §12).
Unit B is the inspection reference.

| Blocker | State | Decision |
| --- | --- | --- |
| Hardware identity: base board, amplifier, speaker, connector on unit A | Same configuration as unit B, photographed (§0-§4) | **Closed** (owner premise + unit B evidence) |
| A. R54 / display-power safety | §8.8 | **Closed** by evidence; the panel is watched during the first SEND |
| B. Which socket carries the BTL output | Not identifiable from the evidence (§9.1) | **Not a blocker for unit A** (factory-seated, unopened). **Blocks powering unit B** with its speaker plugged in, until identified |
| C. Base board seating | Unit A untouched since the factory | **Not a blocker for unit A**. Unit B: reseat before it is next powered |
| D. Playback level | pocketaudio clamps at -12 dBFS; the first SEND uses -26 dBFS (§11) | **Kept conservative**; `playback_verified` stays 0 until the first controlled SEND passes |

**Decision: the first controlled SEND on unit A is READY, waiting only for
the owner's explicit approval** and the §12 preconditions (the owner present
and confirming that unit A has never been opened or had its speaker
unplugged). Nothing was played. `playback_verified = 0`.

### 9.1 The speaker socket (unit B)

Two identical white 2-pin receptacles sit beside the MAX98357A. **Which one
carries OUTP/OUTN is not identified, and is not guessed.** By the owner's
order of preference:

- **PCB routing or source:** none exists for the base board (§3.2), and the
  black solder mask hides the traces in the macro. The two identical
  two-terminal parts between the IC and the upper receptacle look like an
  output filter, which is appearance, not routing.
- **Physical trace evidence:** none resolvable in the images.
- **Known original seating:** the plug is not seated in any image.
- **Powered-off continuity (battery out, not powered):** the datasheet (a copy
  of Maxim's, retrieved 2026-09-13) gives the TQFN pins: **OUTP pin 9, OUTN
  pin 10**, GND 3/11/15, VDD 7/8, SD_MODE 4, DIN 1, GAIN_SLOT 2, LRCLK 14,
  BCLK 16, N.C. 5/6/12/13, and an exposed pad that is not internally
  connected; its top view marks pin 1 with the `+`.
  The speaker receptacle is the one whose two contacts have continuity to
  pins 9 and 10 (through the series parts, near 0 Ω). A quick exclusion
  first: a receptacle with a contact at 0 Ω to ground is not the amplifier
  output (OUTP and OUTN are never grounded).

Context, not evidence: the vendor launcher drives a fan (GPIO42, PWM,
`ui_hardware.c:69,275`) and the case back carries one (vendor image
`bottom.png`), so the other receptacle may be a fan or power output. A
speaker plugged into a DC or PWM output would be damaged; that is why this
check comes before unit B is powered with its speaker connected.

**RECEIVE is unaffected** by all of this: a capture puts no data on IO35 and
leaves IO34 low.

## 10. Read-only live enumeration

**Run on unit A on 2026-09-13 at 11:36 UTC before the deploy (§15): identical
to the morning record below.** The earlier note stays for the record:
not run at first, because unit A's network address was not available, and
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
- **Hard ceiling in pocketaudio: -12 dBFS** (peak 8192). Every played sample
  is clamped; only a code change raises it.
- **What a level means at the speaker** (datasheet copy, revision 4): output
  in dBV = input in dBFS + 2.1 dB + the GAIN_SLOT gain (3 to 15 dB), unless
  the supply limits the swing. At the **highest** strap, 15 dB, into 7.2 Ω,
  treating the peak as a full sine (the multi-tone ggwave signal carries
  less): the -12 dBFS ceiling gives 1.79 V rms, **0.44 W**, under the
  speaker's 1 W rating; at 12 dB 0.22 W, at 9 dB (strap open) 0.11 W. So the
  ceiling alone keeps the TR-WS-2014B inside its rating whatever the strap
  and rail. VENDOR-DOCUMENTED (part) + arithmetic.
- **ggwave level**: peak is about volume/100 of full scale (host measurement,
  tests/wave_modem_test.c): volume 10 -> 3192 (-20.2 dBFS), 25 -> 7980
  (-12.3 dBFS). pos-wave accepts 1 to 25; Wave uses 10.
- **First playback level: `--volume 5`**, peak about 1596 (-26.2 dBFS): at
  most 0.35 V rms, **17 mW**, at the 15 dB strap; 4 mW at 9 dB.

## 12. Proposed first controlled SEND (unit A; ready, needs the owner's explicit approval)

**Unit:** A, build `c688309`, unopened. **Preconditions:** the owner present
at the desk, confirming unit A has never been opened and its speaker never
unplugged; nothing in the 3.5 mm jack (a speaker-route playback still enables
the codec's headphone driver); nobody's ear near the unit; the owner watching
the panel. Powered and verified just before: `pos-wave info` prints `board
k230-t-display`, `playback not validated (gated)`, `capture validated`; no
`pos-wave` running; no recovery record.

`--allow-unverified` appears in step 2 only, for this one approved command.
`playback_verified` stays 0 in the code until this test has passed.

| Item | Value |
| --- | --- |
| Protocol / profile | ggwave `audible_fast` (Wave's default), variable length, payload `DOORS` (5 bytes) |
| Sample rate / format | 48 000 Hz, S16_LE, mono duplicated into both I2S slots |
| Duration | 56 frames of tones = 1.194 s, plus 100 ms of silence before and 150 ms after: **1.444 s** |
| Frequency range | **1875 - 6328 Hz** (ggwave audible protocols) |
| Digital level | `--volume 5`: peak about 1596 (-26.2 dBFS), clamped anyway at -12 dBFS; at most 17 mW into the speaker at the highest gain strap (§11) |
| Amplifier enable sequence | audio lock; reconcile any record; route read (`External I2S Output Switch` already on: not written); PCM opened and prepared; recovery record written; gpiochip1 line 2 requested as an output **already high** (IO34 high, amplifier on); 100 ms silence; tones; 150 ms silence; drain (bounded 1 s) |
| Cleanup sequence | line 2 driven low (amplifier shutdown), released; PCM closed; route left as found; record removed; lock released. Ctrl-C stops within one 200 ms wait and runs the same; a SIGKILL is undone by the next open or `pos-wave recover` |
| Mixer before / after | identical; `External I2S Output Switch` = on throughout; the codec volumes do not apply to this route |
| IO34 before / after | pad `0x000001B0` (GPIO, pull-down, level 0), line 2 without consumer; during: consumer `pocketaudio-amp`, output high; after: level 0, no consumer |
| IO35 | I2S data during the 1.4 s; idle low before and after, panel powered throughout |

```sh
# 0. state before (reads)
amixer -c 0 contents > /tmp/audio-before.txt
gpioinfo -c gpiochip1 2 3; devmem 0x91105088 32; devmem 0x9110508c 32
pos-wave info | grep -E '^(playback|capture) '
# 1. the waveform, checked with no audio device involved
pos-wave encode --volume 5 --text DOORS /tmp/doors.wav   # expect duration_ms 1194, peak ~1596
pos-wave decode /tmp/doors.wav                           # expect "text DOORS"; then rm /tmp/doors.wav
# 2. the one playback (owner watching the panel and listening)
pos-wave send --allow-unverified --volume 5 --protocol audible_fast --events --text DOORS
#    expect: ready k230-t-display / sending 1444 / sent ; exit 0
# 3. state after (reads)
amixer -c 0 contents > /tmp/audio-after.txt; diff /tmp/audio-before.txt /tmp/audio-after.txt
gpioinfo -c gpiochip1 2; devmem 0x91105088 32; devmem 0x9110508c 32
ls /run/pocketos/audio.recovery 2>/dev/null              # expected: absent
```

**What the owner watches and listens for:** a short burst of chirping tones,
about 1.2 s, quiet - clearly audible at arm's length, not loud; possibly a
faint click as the amplifier switches on or off; **the panel steady**
throughout (no blink, dimming, flicker or reset); no buzz, hiss or tone after
the burst ends (the amplifier must be back in shutdown). Optionally, Waver on
the phone within arm's length, listening, to decode `DOORS`.

**Abort** (tell the operator, who stops it with Ctrl-C): any panel
disturbance, anything louder than expected, a sound that continues. **Pass:**
tones heard, panel steady, exit 0, mixer identical, IO34 low with no consumer
after, no recovery record. Only after a pass does `playback_verified` become 1,
in a separate change.

## 13. Proposed first real RECEIVE test (superseded by the owner's receive, §15)

The owner ran the physical receive through Wave itself before this script
was reached; the owner accepted it as the RECEIVE test, and the checks below
that it does not cover were done over SSH instead (§15).

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

Carried into docs/KNOWN_ISSUES.md. Revision 4: nothing here blocks the first
controlled SEND on unit A except the owner's approval (§9).

1. ~~R54 not seen on any unit.~~ Closed by evidence, not by sight (§8.8); the
   first SEND watches the panel.
2. ~~On unit A: whether it carries the base board, amplifier and speaker.~~
   Same hardware as unit B (owner). **Still open for unit B:** which
   receptacle is the speaker's (§9.1), and reseating its base board, before
   unit B is powered with its speaker plugged in.
3. The amplifier's gain strap and supply rail (§6). No longer a risk to the
   speaker: the -12 dBFS ceiling stays under 0.44 W at the highest strap
   (§11).
4. ~~Unit A's §10 reads are outstanding.~~ Done 2026-09-13 (§15).
5. ~~The capture slot mapping is inferred.~~ Channel 1 confirmed on unit A
   (§15).
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
9. ggwave's steady listening cost on unit A is measured: 0.6 % of the one
   core (§15). The cost of the decode at the end of a message is not: the
   owner's receive worked, but nothing sampled the CPU during it. On the host
   the worst single decode call was 17.9 ms with all twelve protocols.
10. The codec's ALC behaviour and the fixed 30 dB on-board mic gain may clip
    in a loud room; neither is adjustable through ALSA.
11. The small speaker's response across 1.9 - 6.3 kHz is unmeasured.
12. ~~Every capture opens with a start-up transient.~~ **Fixed in
    `c688309`** as the K230 codec/capture startup transient: both channels
    sat at negative full scale for about 140 ms (channel 1) and 210 ms
    (channel 0). pocketaudio now reads and discards the first 500 ms of every
    capture inside its normal per-call wait, so nothing of it reaches the
    decoder, the level meter or a recording (§15). What remains after 500 ms
    is a DC offset of about -1200 (-29 dBFS) decaying over the next ~600 ms:
    no sample near full scale, and nothing in ggwave's band (1875 Hz and up).

## 15. Unit A RECEIVE validation, 2026-09-13

Build `da3c5e5` deployed to unit A over Ethernet (192.168.10.157). SSH from
the development host. Owner = seen by the product owner at the panel. No
SEND, nothing played, `playback_verified` and `capture_verified` 0
throughout.

**Preparation (11:36-11:40 UTC):**

| Step | Result | Evidence |
| --- | --- | --- |
| Identity | `0.0.9`, `BUILD_ID=3d4a6e7`, no `pos-wave`; unit A's v0.0.9 rollback tar, netd store, brightness 60 and the keyboard IRQ line held by the shell present | LIVE-READ |
| §10 reads before the deploy | identical to the morning: card and PCM closed, no `/dev/snd` holder, mixer as recorded, IO35 `0x00001191` (level 0), IO34 `0x000001B0` (level 0), gpiochip1 lines 2-3 unused, four services with 0 restarts. IO4 (UART1 RX from the nRF52840 board) reads high with no pull configured: weak, inconclusive hint that something drives it | LIVE-READ |
| Rollback tar | `/root/rollback-3d4a6e7/pocketos-userspace-3d4a6e7.tar`, the 13 files the deploy replaces, 1,242,624 B, sha256 `0dee7a9b…41bb4f5`; extracted and hash-checked (13/13 OK); `ROLLBACK.txt` beside it (includes removing `/usr/bin/pos-wave`); host copy `~/work/rollback-unitA/` | LIVE-READ |
| Deploy | `deploy.sh`, rc 0; all 14 deployed files hash-identical to the build host's target tree | LIVE-READ |
| After the deploy | `BUILD_ID=da3c5e5`; `pos-wave info`: `k230-t-display`, mic channel 1, both paths `not validated (gated)`; Wave in `pos app list`; four services running, 0 restarts after 25 s, 0 crash reports; no process carrying `POCKETOS_AUDIO_ALLOW_UNVERIFIED`, none set in `/etc/default`, init scripts or profiles; mixer identical, no device opened | LIVE-READ |
| Shell for the UI test | restarted with `POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture` and umask 022; only the shell and its supervisor carried it | LIVE-READ |

**Physical RECEIVE (owner):** Wave opened on unit A, RECEIVE, listening; a
phone sent `test` with ggwave; Wave decoded it and showed it as expected.
**PASS**, accepted by the owner as the RECEIVE acceptance test. The audio
lock file's timestamp (11:41 UTC; the file did not exist in the checks right
after the deploy) places the helper's open of the device in that session.

**Checks over SSH afterwards (11:48-11:52 UTC):**

| Check | Result | Evidence |
| --- | --- | --- |
| Which channel decoded `test` | Channel 1: Wave starts `pos-wave listen --events --seconds 120` with no `--channel` (`wave_session.c:439`), so the board's `capture_channel` 1 applies (`pos-wave info`); the deployed binaries are hash-identical to that source's build | SOURCE-CONFIRMED + LIVE-READ |
| Channels, ambient (3 s each, no transmitter, deleted at once) | channel 1: steady rms 355 and 312 (-39.3 / -40.4 dBFS), peaks to -26 dBFS: room sound. Channel 0: rms 71 (-53.3 dBFS), peaks -36 dBFS, slower DC settling: a different input. On-board mic = channel 1; channel 0 = empty headset input STRONGLY INFERRED | LIVE-READ |
| State left by the owner's session | no `pos-wave`, no zombie, no child of the shell, no recovery record, route on, no `/dev/snd` holder, both PCMs closed, mixer identical to before the deploy, services 0 restarts | LIVE-READ |
| Helper lifecycle (same binary, same arguments as the app, from SSH) | `ready` then `listening` 0.44 s after start; route off and recovery record written while open, capture PCM RUNNING at 48 kHz S16_LE 2 ch, period 960, buffer 24000; SIGTERM (the app's stop) -> `stopped`, exit 0 in 0.03 s; route restored, record removed, device closed | LIVE-READ |
| CPU while listening | `pos-wave` 0.6 % of the single core over 15 s; system 4.3 % busy with the shell running; load 0.16; RSS 3.7 MB | LIVE-READ |
| Capture-only override against playback | `pos-wave send` with `POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture`: `error audio_disabled k230-t-display speaker playback is not validated on hardware`, exit 3 in 0.05 s. Fenced twice more, so a broken gate could not have sounded: the audio lock was held by a listen, and the PCM name pointed at a card that does not exist. gpiochip1 line 2 unused and IO34 low before and after, playback PCM closed | LIVE-READ |
| SIGKILL during RECEIVE, `pos-wave recover` | killed (rc 137) while listening: route left off, record present, device closed by the kernel. `pos-wave recover`: `recovered`, "restored after an owner that did not close: route 1", exit 0; route on, record gone | LIVE-READ |
| SIGKILL during RECEIVE, next open | killed while listening; the next `listen` printed `recovered` before `ready`, ran 2 s and exited 0; route on, record gone | LIVE-READ |
| Start-up transient | found here, item 12 of §14 | LIVE-READ |
| Mixer at the end | identical to before the deploy (21 lines); IO34/IO35 and lines 2-3 unchanged | LIVE-READ |
| Cleanup | no WAV under `/tmp`, `/root`, `/run`, `/var/lib/pocketos`; event files and scripts removed | LIVE-READ |
| Shell restored | restarted without the override (umask 022): no process carries it, four services 0 restarts, both paths still gated | LIVE-READ |

Not covered: the Wave session's own detached `pos-wave recover` after a
helper killed under the running shell (host-tested with the real helper,
`wave_session_test`), CPU during a decode, and a SIGKILL of the shell and its
helper together (the known gap).

### 15.1 RECEIVE closeout: build `c688309` (revision 4)

**Change:** the K230 codec/capture startup transient is discarded in
pocketaudio (500 ms, `capture_settle_frames`), and the K230 microphone path is
marked validated (`capture_verified = 1`); `playback_verified = 0` unchanged.

**Host validation of `c688309`:** `make test` rc 0 (73 suites, 3359 ok, 0
failures; pocketaudio_test 135, wave_view_test 93, wave_session_test 81,
wave_tool_test, audio_recovery_test, capture_settle_test and wave_lint 0
failures); SDL shell build 0 warnings; all 16 shell tests 0 failures (Wave's
LVGL app test among them); riscv64 `make all` (ENABLE_SX1262=1) and the DRM
shell 0 warnings. With the discard disabled in a throwaway copy, 11
pocketaudio_test and 4 capture_settle_test checks fail.

**Unit A (12:25-12:29 UTC):**

| Check | Result | Evidence |
| --- | --- | --- |
| Rollback tar before the deploy | `/root/rollback-da3c5e5/pocketos-userspace-da3c5e5.tar`, 14 files, sha256 `fb6b814b…3652c358`, hash-verified; host copy `~/work/rollback-unitA-da3c5e5/` | LIVE-READ |
| Deploy | `deploy.sh` rc 0; `BUILD_ID=c688309`; 14/14 files hash-identical to the build host | LIVE-READ |
| Gates | `pos-wave info`: `playback not validated (gated)`, `capture validated`, `capture_settle_ms 500`, `mic_channel 1` | LIVE-READ |
| Shell | restarted with umask 022 and **no override**; no process carries `POCKETOS_AUDIO_ALLOW_UNVERIFIED`, nothing sets it; Wave listed; four services running, 0 restarts, 0 crash reports (again 20 s later) | LIVE-READ |
| RECEIVE without the override | `pos-wave listen --events --seconds 3` (the app's command with the shell's environment): `ready`, `listening`, exit 0 in 3.94 s (3 s + 0.5 s discard + open); first level 7, not 100; route back, record gone, device closed, no helper left | LIVE-READ |
| Transient gone | 3 s recording on channel 1 (statistics on the device, deleted): no sample at or above 32000, peak 2387; residual DC offset -1216 in the first 100 ms after the window, decaying to -120 by 600 ms | LIVE-READ |
| STOP during the discard | SIGTERM right after `listening`: `stopped`, exit 0, gone in 0.18 s; route back, record gone, device closed | LIVE-READ |
| SIGKILL during the discard | killed (137): route left off with its record; the next listen printed `recovered` before `ready`, exit 0; route back, record gone | LIVE-READ |
| SEND still refused | `pos-wave send` without and with `POCKETOS_AUDIO_ALLOW_UNVERIFIED=capture`: `error audio_disabled ... speaker playback is not validated on hardware`, exit 3, both times; fenced by a held lock and a nonexistent PCM; gpiochip1 line 2 unused, IO34 `0x000001B0`, playback PCM closed | LIVE-READ |
| Mixer | identical before and after all of it; `External I2S Output Switch` on | LIVE-READ |
| Display power | IO35 `0x00001191` (level 0) with the panel on, before and after | LIVE-READ |
| Cleanup | no WAV files, no event files, no helper | LIVE-READ |

**RECEIVE: READY.** The Wave UI was not exercised again: it starts the same
helper with the same arguments and the shell's environment, verified above.
