# Back corner (DS §48) - hardware gate

**Unit A carries doors-shell `1df9438` (this branch: the drawn corner),
hot-swapped over master `3a1303b`; every other binary is unchanged
(userspace `9eda04e`). Rotation mode Automatic (landscape with the keyboard
base), text size Small (key absent, as found).**
Rollback: `/root/rollback-back-hit/RESTORE.sh` (doors-shell `3a1303b`, md5
`c2099da4`, and `settings.conf` as found).

- Date: 2026-10-02, 07:20-07:55Z
- Branch `feat/back-button-hit-area`, code commit `8e2fac5` (base origin/master `3a1303b`)
- Binary: riscv64 DRM `doors-shell` from a clean clone of `8e2fac5` (sysroot LVGL,
  0 first-party warnings, no test hooks), md5 `579527a6fe7333de4f480e789d5d957c`
- Deploy: `doors-shell` only, over SSH (no flash, no deploy.sh); `settings.conf`
  byte-identical before and after the gate
- Touches: injected through the panel's own input device (`/dev/input/event1`,
  GT9895 multitouch slot 0, `tests/hw/touch_slot0_tap.py` and the app-groups
  gate's `gate_touch.py`), mapped through the shell's logged calibration. The same
  kernel path as a finger; **no physical finger was used**.
- Tools: `C:\K230\out\back-hit-gate\` (g1 build, g2 deploy, g3 corner checks,
  g_pass captures + g3, g4 soak, g5 restore mode; logs/ and caps/)

Two passes on this sheet: **the drawn corner** (`1df9438`, the section
straight below) and, before it, **the touch target alone** around the old
slab (`8e2fac5`, from "Points tapped" on).

## The drawn corner - `1df9438`, 2026-10-02 08:54-09:05Z

- Binary: riscv64 DRM `doors-shell` from a clean clone of `1df9438` (sysroot
  LVGL, 0 first-party warnings, no test hooks), md5 `48159d74794633c00b35323ba4cbeb9d`;
  hot-swapped over `8e2fac5` (rollback unchanged: back to master `3a1303b`).
- Corner drawn from the edges to x 101 / row 71 in portrait (slab at x 30)
  and x 121 / row 71 in landscape (slab at x 50); Utilities folder page to
  x 107, Controls to x 121 (their margins 36 and 50). Title unmoved.

| Step | Landscape | Portrait |
| --- | --- | --- |
| Settings at Small, Medium, Large: layout audit clipped / overlap (header row) | 0 / 0 (0) each | 0 / 0 (0) each |
| Settings, Calculator, Clock, Notes, Files, Terminal: 7 corner taps go home | 7/7 each | 7/7 each |
| Same apps: 4 px past the corner, the title, below the header stay | 3/3 each | 3/3 each |
| System -> corner -> Settings -> corner -> home | PASS | PASS |
| Folders Utilities, Games; Controls | PASS | PASS |
| Twenty corner rounds | 20/20 | 20/20 |
| Total (g3) | **18 PASS, 0 FAIL** | **18 PASS, 0 FAIL** |

Captures (`C:\K230\out\back-hit-gate\caps\`): `corner-land-settings-{small,medium,large}`,
`corner-land-settings-held`, `corner-land-folder`, `corner-land-controls`, and the
same `corner-port-*`. Held at (3, 3): the whole corner lit (`surface_raised`)
with the focus outline along its right and bottom edges. The frame buffer
shows the corner square; on the glass the panel's rounded corner rounds it.

Since this deploy: 839 shell.log lines, 0 ERROR/assert, 0 WARN, no crash
report, one process throughout (the rotation-mode changes restart in place).
The text-size steps wrote `text_size=small` into settings.conf, where the key
had been absent (the same Small); the file was put back byte-identical.

Not done: the owner's eyes and finger on the glass (the corner's look under
the curve, the 16 px gap to the title).

## Points tapped

For each app with the shell's header, with the slab at `(pl, 8)`, 72 x 56:

| Inside the corner (should go back) | Outside (should not) |
| --- | --- |
| (2, 2) - the screen's corner pixel | (pl + 82, 36) - 3 px past the reach |
| (pl / 2, 36) - the strip left of the slab | (pl + 120, 36) - the title |
| (pl + 36, 3) - the row above the slab | (3, body_y + 3) - just below the header |
| (pl + 36, 69) - the header's foot under it | |
| (3, 68) - bottom-left of the header row | |
| (pl + 78, 36) - the last columns of the reach | |
| (pl + 36, 36) - the slab's middle | |

## Results

| Step | Base `3a1303b`, landscape | Branch `8e2fac5`, landscape | Branch, portrait |
| --- | --- | --- | --- |
| Slab position | x 50, row 8 | x 50, row 8 | x 30, row 8 |
| Settings, Calculator, Clock, Notes, Files, Terminal: corner taps go home | 1/7 each (only the middle) | **7/7 each** | **7/7 each** |
| Same apps: taps outside stay in the app | 3/3 each | 3/3 each | 3/3 each |
| System (page of Settings): corner -> Settings -> corner -> home | FAIL (stays in System) | PASS | PASS |
| Folders Utilities, Games: title keeps the folder, corner -> launcher page | FAIL (corner does nothing) | PASS | PASS |
| Controls: corner (3, 66) closes it | FAIL | PASS | PASS |
| Twenty rounds Calculator -> corner (x 1..28, y 1..70) | 0/20 | **20/20** | **20/20** |
| Same shell process throughout | yes | yes | yes |
| Total | 7 PASS, 11 FAIL | **18 PASS, 0 FAIL** | **18 PASS, 0 FAIL** |

The base column is the "before": on master a tap anywhere in the corner but on
the 72 x 56 slab itself does nothing.

**Visuals unchanged.** Settings in landscape captured on base and branch
(`caps/base-land-settings.png`, `caps/branch-land-settings.png`): the header row
and the page below it, left of the status cluster (whose clock ticks), are
**pixel-identical**. With a finger held at (3, 3) for 2.5 s, base's capture is
identical to the unheld one; branch's differs only inside the slab's own box
(48,6)-(123,65): the slab is drawn pressed (`caps/branch-land-settings-held.png`).

**Soak.** 100 rounds open an app -> tap the corner (x 1..25, y 2..61) on
branch, landscape: 100/100 home; VmRSS 15596 kB at rounds 0, 25, 50, 75 and
100 (the ~0.9 MB rise across a first g3 pass is the first opening of each app;
base rose 0.5 MB over the same pass). Since the deploy: 1078 shell.log lines,
0 ERROR/assert, 0 WARN, no crash report, no restart other than the
rotation-mode change to portrait (in place, same pid).

## Not done

- Taps with a real finger on the glass: left for the owner (the feel of the
  corner, and whether 8 px past the slab toward the title is right).
- Unit B, RIFT and Video landscape (their own top row; unchanged by design),
  Large text size (does not move the slab).
