# Vision GENERAL 80, COLOR, LINE TRACE - unit handoff (PARTLY DONE)

Branch `feat/vision-general-colour-line` (docs/apps/VISION.md). The branch was
first built and tested on the host only (the fake camera, the fake detector
and synthetic pictures). A bounded real-camera session on unit A followed on
2026-10-26; its record is "Unit A session" at the end, with what it settled
and what is still open. The table below is the original list of checks.

What a host test cannot say, and so what this sheet is for: how the camera's
colours, exposure and noise behave in front of COLOR and LINE TRACE; whether
the protocol-3 helper and shell run together on the K230; and the GENERAL 80
detector's behaviour on live scenes with the new screen.

## Before

- Build identity first: the shell and `pos-vision` from this branch (protocol
  3 - an older helper with this shell, or the reverse, is refused at `hello`,
  so both must be replaced together), cross-built against the pinned SDK
  sysroot. Record both sha256 and the rollback path.
- For GENERAL 80: `tools/vision/install-model.sh --general <file> <unit ip>`
  with the gated file (sha256 `8c304651…e354`); it refuses any other. Bench
  unit only: weight licensing and redistribution permission remain
  unresolved (MODEL_LICENSES.md), and this install by hand does not resolve
  them.

## Checks

| # | Check | Pass when |
| --- | --- | --- |
| 1 | Open Vision, DETECT, R0 only | MODEL `R0 · Beta` takes no tap; CLASSES shows R0's six and "no 80-class detector file"; the status's second line names what is in view or "Nothing it knows in view (6 classes)" |
| 2 | GENERAL 80 installed, MODEL | `LOADING GENERAL`, then `GENERAL 80`; log `detector GENERAL 80 in force`; `detector=upstream` stored; CLASSES pages through 12 groups; an everyday object (a cup, a chair, a laptop) boxed with its name and "In view" counting it. Record KPU ms and fps from the status (the A/B gate's 57-65 ms is the reference, not a requirement) |
| 3 | MODEL back and forth 5 times | each switch: the old helper `bye`, no WARN/ERROR, RSS and CmaFree back to their levels |
| 4 | COLOR on plain coloured objects (red, green, blue, yellow, white paper, a black object) in room light | the square and swatch on the picture in both orientations; the name in the first counter matches the object for the saturated colours; the swatch does not flicker while the unit is still; `(settling)` only for a moment after a move. Record any name that is wrong and the light - a wrong name under coloured light is expected, not a defect |
| 5 | COLOR: a two-colour object across the square; a dim corner | `mixed: A nn% and B`; the dim one `dark: black, or too little light to tell` |
| 6 | COLOR: tap to move the square; MATCH; CLEAR | the square follows the tap; MATCH paints the colour's pixels and says the share; CLEAR stops it; a turn of the unit puts the square back in the centre |
| 7 | LINE TRACE: black tape on a light floor, unit held over it, portrait and landscape | yellow along the tape, cyan on its edges, the accent dots on the fitted line; `LEFT n%` / `CENTRE` / `RIGHT n%` follow the tape as the unit moves; no jumping between the tape and a shadow or a floor seam |
| 8 | LINE TRACE: move the tape out of view, then back | at once `LOST` with a growing time and the warning, no dots drawn; back in view the line is followed again within a picture or two |
| 9 | LINE TRACE: white tape on a dark floor with LINE: LIGHT; a grey tape on a grey floor in dim light | found; record if not, with a saved frame (`pos-vision bench --save`) |
| 10 | HDMI (1920 x 1080) with the keyboard | DETECT's CLASSES sheet, COLOR's MATCH/CLEAR and the LINE button reached by the arrows and Enter; Esc and Back close the CLASSES sheet |
| 11 | Each text size (small, medium, large) | DETECT's two status lines, COLOR's two and TRACE's two readable within their place; the CLASSES page's names inside the sheet |
| 12 | Leave from each new mode, 10 opens and closes | no `pos-vision` left, Camera works afterwards; mind the known camera lock-up (KNOWN_ISSUES.md) - a bounded count with the console logged |
| 13 | Performance in COLOR and TRACE | the status's fps and post ms (the pixel pass) against EDGE's on the same build; helper CPU share and RSS from the log |

## Not to be concluded from this sheet

- That COLOR measures colour: it names categories as this camera sees them,
  under the light there is.
- That LINE TRACE is fit to steer anything: it reports a line on the
  picture; no output drives hardware.
- That GENERAL 80 recognises arbitrary objects: it boxes the 80 COCO classes.
- Anything about body pose or hands: docs/vision/BODY_HAND_TRACKING_PLAN.md
  lists its own measurements, none of which this branch can make.

## Unit A session (2026-10-26)

A bounded real-camera smoke test, not a measurement of accuracy: a few
objects, one sheet of paper with a hand-drawn S-shaped line, a TV in front of
the unit. Evidence classes as in AGENTS.md; each result says where it comes
from.

### Unit and staging

| | |
| --- | --- |
| Unit | unit A, identified read-only over its console: wlan0 `88:3b:dc:b7:9e:c7`, Doors 0.3.8 `BUILD_ID=bc889b0d`, up 9 days |
| Installed (unchanged before and after) | `/usr/bin/doors-shell` `d224e5ba…`, `/usr/bin/pos-vision` `06189d72…`, `/usr/lib/liblvgl.so.9.5.0` `52ff8acc…`, `settings.conf` `34b51595…`; Vision `settings.v1` `002981c5…` and DeskBuddy `prefs.v1` `11c31694…` present; `vision/owner.v1` and `deskbuddy/guard.v1` absent |
| GENERAL 80 file | already on the unit: `/usr/share/doors/vision/det-upstream-yolox-tiny-416.kmodel`, sha256 `8c304651…e354` = `tools/vision/general-80-model.sha256` (VERIFIED); R0 `94a20ac0…` beside it; nothing was staged or changed in the model directory |
| Kit | `docs/hardware/vision-gcl-gate/` in `/root/vision-gcl-kit`, sent over the console (gzip + base64; no network service and no key were added to the unit): the test shell ran with `POCKETOS_CONFIG_DIR`/`POCKETOS_STATE_DIR` on kit copies and `POCKETOS_VISION_HELPER` on the kit's `pos-vision`, so no installed setting, Vision setting or DeskBuddy preference was written; the installed supervised shell was stopped only while the test shell had the panel |
| Binaries (cross-built from `git archive` of the commit, pinned SDK toolchain and sysroot, 0 warnings) | shell `1a554b76` `a75e3804…`, then `7fa84de4` `40019d60…`; `pos-vision` `1a554b76` `7aa22929…`, then `a7b3ddcb` `5b59d129…`, `dd56746d` `1261e71e…`, `df3353b5` `b51ebf49…` |
| Screens | the DRM-plane capture (`shell.action screenshot`); the device's LVGL has no snapshot support. Navigation by `shell.key` (arrows, Enter) and kit settings; touch by the owner |
| Restore | `vn-restore.sh`: `RESTORE: OK` - the installed doors-shell supervised and the only panel owner, every installed binary and model hash and the preserved files identical (existence and content), meshcored and radiod the same processes, no camera helper; the state diff only in logs and meshcored's own `state.v1`; then the kit directory removed (root free space back to 50.6 MB) |

### Results

**Logs and captures (VERIFIED on the camera and KPU):**

- *GENERAL 80 loads and runs:* the helper started with `--model
  …/det-upstream-yolox-tiny-416.kmodel`, `detector GENERAL 80 in force … 80
  classes`; KPU 55-68 ms, 10.3-11.7 fps, RSS 7.5 MB, 0 bad tensors over five
  minutes. MODEL by keys switched to R0: the old helper `bye` in 480 ms, R0
  in force (KPU 56 ms, 11.8 fps), `detector=r0` stored (kit copy).
- *Labels and boxes agree on a desk:* mouse 90 %, cell phone 88 %, book 47 %,
  a second "mouse" 46 % (a hand in a white sleeve), keyboard 38 %, two
  screens as tv 40 %/39 %, a drink can as bottle 38 % and cup 35 %
  (overlapping boxes); the boxes lie on the objects in portrait. On TV
  pictures: person 84 % and tie 79 % (a class R0 does not have); R0 on
  another TV picture person 66 % and 50 %, and a false car 49 % on the TV's
  frame; a sun logo as scissors 43 %. A smoke test: no accuracy is claimed
  from these.
- *COLOR:* the square and swatch at the picture's centre in portrait and in
  landscape; readings `dark: black, or too little light to tell` (a black
  mouse), `white, no clear hue` (a laptop lid), `mixed: red 47% and white`
  with a red swatch (a red card half over the square), `grey, no clear hue`
  (lined paper in dim light), `BLUE 66%` (a blue-grey pipe on TV). MATCH by
  key took the white read and painted the matching pixels beside the square
  (4.0 %); CLEAR stopped it and the counter went back to `-`.
- *LINE TRACE:* the S-line followed with the yellow centre on the line;
  after the fixes below, the offset at the line's own end (`LEFT 50%` with the
  end at about -50 %); with the line out of view (a dark scene) `LOST 0.2 s`
  and the warning at once, nothing drawn; with it back, followed again.
- *Alignment:* boxes, square, swatch and trace marks on the picture in
  portrait; in landscape (the kit's `display_rotation=landscape` with the unit
  held upright, so the picture turned) the GENERAL 80 box lay on the object
  and the square at the centre.
- *Leaving:* closing Vision ended `pos-vision` at once (0 s); Camera then
  opened with the installed `pos-camera` (NV16) and showed the picture;
  after Home neither helper ran.

**Owner observations (OBSERVED):** the owner did the grouped physical action
(objects, a red card, the lined paper) and reseated the console cable when
it came loose. The tap that moves the square was not seen in any capture, so
moving the square by touch is **not** shown on the unit (it is in the host
tests and the simulator); no blue or green object reached the square.

### Defects found and fixed (each with a host regression check)

1. *DETECT's "In view" line was off the panel* (`7fa84de4`): the full numbers
   wrapped onto the second line. DETECT now shows detector, fps, KPU and CPU;
   pre, post and RSS stay in the log. Seen fixed on the unit (`In view: tv 2,
   mouse 2, bottle 1, cup 1, +3 more`).
2. *COLOR's MATCH line lost its end* (`7fa84de4`): the frame rate in front
   made it wrap. Seen fixed on the unit (`MATCH #FCE5DE: 4.0% of the picture`).
3. *A line in plain view was said lost* (`a7b3ddcb`, then `df3353b5`): specks
   in the dark lower corners held every start row (3, then the 32 of the
   first fix; the vignette made about 50 such rows). Every unvisited run of
   the lower half is now a start; a run already on a chain starts none.
4. *The offset came from the straight fit carried down* (`a7b3ddcb`): an
   S-line read `LEFT 99%` with its end near -74 %. The bottom is now where
   the line itself is on its lowest rows. Seen right on the unit (`LEFT 50%`).
5. *A speck bridged below the line became its bottom* (`dd56746d`): a short
   fragment cut off by a gap at the chain's bottom is dropped.

The last helper (`df3353b5`) ran on the unit, but by then the camera saw a
dark scene ("No dark line found yet", correct for it); fixes 3 and 5 are
therefore checked on the host (each regression check fails with the fix
removed), not again on the camera.

### Still open

- LINE TRACE with `df3353b5` on a line in view; a straight tape on a floor,
  as the mode is meant for.
- Moving COLOR's square by touch on the unit; a clean colour change to blue,
  green or yellow in front of the camera.
- Busy texture (rock, wallpaper, a TV picture) can be followed as a "line"
  through scattered dark fragments - a limitation, recorded, not fixed.
- HDMI and the three text sizes on hardware (the simulator matrix stands).
- The fitted dots are a straight line: on a curve they show the overall
  lean, not the shape.
