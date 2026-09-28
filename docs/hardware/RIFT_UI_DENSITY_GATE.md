# RIFT UI density — hardware gate

**Unit B carries `feat/rift-ui-density` build `7c9e26b`** (the shell binary
alone), deployed 2026-09-28 16:01 UTC and left on it: rotation mode
**Automatic** (keyboard base present, so landscape), home, radio `rx`,
meshcored online as `K230-B` with its own 157 stored nodes. Rollback:
`/root/rollback-rift-density/RESTORE.sh` puts back the shell unit B carried
before, `feat/vision-app` `7591725` (md5 `6330c834…`);
`/root/rollback-rift-density/state-before.tar` holds `/var/lib/pocketos`
(logs excluded) as it was.

**Unit A was not available**: it answered neither at `.171` nor at `.157`,
its Wi-Fi MAC (`88:3b:dc:b7:9e:c7`) was absent from the network, and the one
serial console attached (COM12) was unit B's. The gate ran on unit B, the
same hardware revision, instead.

**Result: PASS.** No defect was found that needed a code change.
**ACCEPTED 2026-09-28 by the owner** as the hardware gate for this branch,
on these terms:

- **Unit B was used instead of unit A**, which was unreachable.
- **The 1000-node / 256-conversation load came from a non-transmitting
  stand-in** on meshcored's socket: it refuses `mesh.send` and was never
  asked for one. The only transmission in the gate was one DM through the
  real meshcored.
- **The real meshcored was restored afterwards** and came back online with
  its own 157 nodes.
- **Conversation names truncate in the narrow landscape list** (about 8
  characters; finding 1).
- **The 256-conversation bound can omit older conversations and channels**
  when the list is full (finding 2).

## What was deployed

Only `/usr/bin/doors-shell`: the riscv64 DRM build of a clean `git archive`
of `7c9e26b` with `BUILD_ID` `7c9e26b`, stripped, 1,469,120 bytes, md5
`42decb26…` (sha256 `abbe9936…`), copy verified on the unit, restarted
through `/etc/init.d/S90doors-shell`, logged
`start version=0.1.0 build=7c9e26b`. meshcored (`7591725`), radiod, netd,
sysd, CLI, init scripts, art, settings, the MeshCore identity and the radio
configuration were not touched; the SD card was not flashed.

For the scale checks the real meshcored was stopped for about 25 minutes and
a **gate stand-in** served its socket: the repo's scripted service
(`tests/fake_meshcored.c`) with a generated mesh - 1000 nodes (chat, repeater
and room interleaved, one in 17 never heard), 256 conversations (then 255
plus a 200-message thread), two channels, every heard time re-ordered each
30 s. The stand-in refuses `mesh.send`; it was never asked for one. The real
meshcored was restarted afterwards and came back online with its 157 nodes.
Stand-in source and all scripts are in `out/rift-density-gate/` (not
committed).

## How

Over SSH from WSL with the bench key. Taps were injected as the GT9895
reports a finger (`tests/hw/touch_slot0_tap.py`), swipes with the RIFT gates'
`rift_tap.py`; captures are kmsgrab frames of the panel. CPU is the shell's
share of one core from `/proc/<pid>/stat` ticks over each window; RSS from
`/proc/<pid>/status`. No finger was on the glass.

## Results

| Item | Result | Evidence |
| --- | --- | --- |
| Launch RIFT from the landscape launcher | **PASS** | a tap on the RIFT tile; `chrome: none`, `shell.info` answered 6 of 6 times with RIFT open, header `present: false`, `h` 0, body at y 0 (the 2026-09-28 host hang path) |
| Portrait ↔ landscape, repeatedly | **PASS** | 4 cycles on the live mesh and 3 with 1000 nodes / 256 conversations loaded: RIFT reopened from the tile 14 times, NODES and COMMS by their tabs in each orientation, `shell.info` answered every time (portrait header 72 px, landscape none), same pid, no crash report, no fault |
| COMMS default layout | **PASS** | narrow list, thread with the width, one-line header with route and `DETAILS ›`, details pane closed, 36 px command line; nothing clipped |
| DETAILS open / close | **PASS** | a tap on the thread header opens the route, signal, tally (`OF 1 SENT · 0 DELIVERED · 1 NO ACK`) and history note; a second tap closes it and the thread has the width back |
| Conversation selection and scrolling | **PASS** | a tap opens a conversation; the open row stays highlighted at the foot of a 256-row list |
| 256-conversation virtualization | **PASS** | 60–70 swipes to the end of the list, the last row opened the 200-message thread; RSS unchanged |
| 1000-node list | **PASS** | 150 back-to-back swipes reached the last never-heard node (GATE-0985); identity marks on chat and room rows, repeaters neutral |
| Selection attached to identity during re-order | **PASS** | live mesh: two newly heard nodes moved in above, the selected row went from 3rd to 5th and kept highlight and detail; 1000 nodes: GATE-0226 stayed selected with its detail through two full re-orders (heard 4 h → 5 h) |
| Live MeshCore traffic, ACTIVITY | **PASS** | the 20-minute graph drew live adverts and group text in their classes, peak 3/min, agreeing with the feed under it |
| Live MeshCore traffic, COMMS | **PASS** | one real DM unit B → T-Deck-RIFT appeared in the open thread within 3 s and turned `NO ACK` live when the service said so (the T-Deck did not acknowledge a flood-routed DM) |
| Tab usability on touch (landscape, 36 px) | **PASS (injected)** | 8 of 8 taps selected the intended tab, 4 of them 13–14 px from the strip's top or bottom edge; a finger on the glass is the owner's |
| Visible lag or clipping | **PASS** | no clipping anywhere; taps took effect within the 0.8–2 s capture interval |

**Thread area on the device**: the thread's scrolling area is about 932 ×
388 px of 1232 × 568, **≈52 %**, where master's layout gives 520 × 318,
23.6 % (the RIFT UI next gate's unit A frame); **17 whole one-line messages**
above the composer in the 200-message thread, the same as the host count.
The device keeps about 20 px more at the top than the host test's 30 px
corner (the landscape safe area), which is the 2 points between this and the
host's 54.3 %.

### CPU and RSS (doors-shell, one core)

| Situation | CPU | RSS |
| --- | --- | --- |
| RIFT open, live mesh (157 nodes), idle | 2.1 % | 16.5 MB |
| one real DM sent and its state change | 2.1 % | 16.5 MB |
| 1000 nodes, idle through list re-orders | 2.3 % | 17.9 MB |
| 1000 nodes, 40 swipes (ssh-paced) | 8.7 % | 17.9 MB |
| 1000 nodes, 150 swipes back to back | 9.6 % | 18.0 MB |
| 256 conversations, 60 swipes back to back | 13.6 % | 18.2 MB |
| 200-message thread + 256 conversations open, idle | 4.1 % (≈40 ms per 1 s repaint) | 18.4 MB |
| after 7 rotation cycles | - | 16.3–18.0 MB, no growth |

Unit memory: 60 MB used, 912 MB available at the end.

## Findings

1. **Names are cut to about 8 characters in the landscape conversation
   list** (260 px: identity mark, glyph, name, pill, age). `GATE-0…` rows
   were indistinguishable once the unread pill was there; the live T-Deck
   row read `T-Deck-R…`. The thread header names the open one in full. A
   consequence of the narrow list the brief asked for; the width is the
   owner's to choose.
2. **A full conversation list leaves out the oldest conversation and the
   joined channels.** With 256 one-message peers the older 200-message
   thread and both channels were not listed (channels are added only while
   the list has room). Existing behaviour at the new bound, not a hardware
   issue.
3. **COMMS repaint costs about 4× the host** (≈40 ms per repaint with a
   long thread and 256 conversations, 4.1 % idle): acceptable, and the
   number to watch if the thread window grows.
4. Not from this branch: 13 `radio.status poll failed ... timed out after
   200 ms` warnings from the status chip, around transmits; `icon-zabbix`
   art and the Zabbix helper are absent from unit B's image (a shell-only
   deploy onto a vision-app card); glyphs such as `™` in remote names draw
   as boxes.

No hardware-only defect was found. The one hardware-only difference is the
landscape top inset (item above), which the host test models with 30 px.

## Not exercised

- An **incoming** live direct message: unit A was unavailable and the
  T-Deck cannot be driven from here.
- A finger on the glass (tabs, flings); keyboard-base keys (not injectable).
- Unit A itself; §37.7 of the design system names it.
