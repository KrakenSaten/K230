# Vision GENERAL 80, COLOR, LINE TRACE - unit handoff (OPEN)

Branch `feat/vision-general-colour-line` (docs/apps/VISION.md). Everything on
the branch was built and tested on the host only: the fake camera, the fake
detector and synthetic pictures. This sheet lists the checks that need a unit,
its camera or its KPU, and nothing that the host suites already settle. No
unit was touched for this branch.

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
  unit only (MODEL_LICENSES.md: UNKNOWN - DO NOT REDISTRIBUTE).

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
