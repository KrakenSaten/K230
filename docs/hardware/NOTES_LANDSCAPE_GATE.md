# Notes in landscape: unit A gate

Branch `feature/notes-landscape`. First gated from master `0d46e34`
(Calculator landscape accepted): code `45cecad`, docs `c025779`, build
`c025779` - the sections below up to "Rebased onto 65224de" record that
gate, under those pre-rebase hashes. The branch was then rebased onto master
`65224de` (the shared PocketUI error-caption fix) and gated again remotely on
build `ebd5a01`: see the last section.

**Result: PASS on unit A, 2026-09-16** - remote validation, then the product
owner's physical check, which also approved the 20 px taller portrait editor
field; after the rebase, remote validation PASS on `ebd5a01`, with every
non-error screen pixel-identical to the build the owner checked. VERSION stays
0.0.10. DS Amendment G (§23) is PROPOSED. Not merged.

Scope: Notes only. No other app, no rotation policy, no keyboard presence
logic, no shell keyboard change, no boot splash, no first-boot or
vendor-launcher behaviour, no Phase 4.

## What changes

Notes lays its three screens out in one frame that is exactly the body's
content box and shapes each from that box's size (DS §23.1): tall in
portrait, as before; wide in landscape, with the actions in a 288 px rail on
the right - New note beside the rows, Done and Delete beside the field - and
the delete confirmation at its portrait width, centred. On master the
landscape editor above the touch keyboard left the field 4 px under Done and
Delete; now the field has the whole 1192 x 100 px body. In portrait the
field is 20 px taller (an LVGL flex gap taken for a hidden screen), and a long
list's New note and a read-only note's field stop 10 px short of the foot,
clear of the rounded corners (§22.2). Details: `docs/apps/POCKETNOTES.md`
(Layout).

## Host validation

From a fresh clone of `c025779`.

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,731 ok, 0 FAIL, 0 warnings (Calculator record 3,723; the eight new `notes_lint` checks) |
| SDL simulator build | rc 0, 0 warnings |
| 20 shell and UI test scripts | 494 ok, 0 FAIL, every script rc 0 (Calculator record 483; `notes_shell_test.sh` 8 -> 19) |
| `notes_app_test` | 880 checks, 0 failures (was 99), no LVGL warning in its output |
| `notes_view_test` / `notes_store_test` / `notes_lint.sh` | 38 / 51 / 0 failures |
| `calc_app_test`, `display_geometry_shell_test.sh` | 456 checks / 69 ok, 0 failures: Calculator and all eleven apps in landscape unaffected |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave` (as in the release); 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build c025779` |
| Portrait against master, simulator | list, empty state and confirmation identical below the status bar with 30 px and square corners; differences only where intended (field +20 px; long list and read-only field clear of the corners) |
| Mutations of `notes_app.c` | 14 of 14 caught, after an unmutated run passed |

The mutations: the wide shape never chosen; its width guard removed; the
corner clearance removed; the resize handler removed; the rail on the left;
the field's floor kept in the wide shape; a row gap in the frame; the dialog
not centred; the dialog full width; the wide list unbounded in height; the
actions full width; the field not full height; New note full width; no layout
at creation. Three of them first stopped the test in an LVGL assert or a
layout livelock after printing their failures; the test's helpers were made
NULL-safe so the two asserts now run to the end, and the livelock (a
content-sized wrapper around a 100 %-tall field) is the mutant's own.

Found on the way and left alone, because they are on master too and outside
this scope:

- When a note cannot be saved, the red caption under the editor field is laid
  out below the field's wrapper and clipped: only the red border and the
  status-bar hint show (`pocketui_text_field`, multi-line). Proposed as its
  own task.
- For about 120 ms after the confirmation's Cancel, a scrolled note is drawn
  shifted, then settles on the right picture (identical to before, both
  orientations, on master as here).
- The empty state's file glyph draws as a box (the symbol font has no
  `LV_SYMBOL_FILE`), in the simulator and on the panel.

## Unit A: remote validation

Everything below ran with nobody at the unit, over the USB serial console
(COM9, 115200). The unit had Ethernet, but no SSH key is authorized on its
current card (its host key changed with the reflash and matches the console's
`ssh-keygen -lf`), so the console was used, as for Calculator.

| Step | Evidence | Result |
| --- | --- | --- |
| Identity before | `/etc/doors-release` 0.0.10 / `9f9c802`; `doors-shell` md5 `a5e7e33e…` (940,720 B), `build 667ca37`; Landscape (stored), theme Slate, keyboard absent; restarts 0; 0 crash reports; `shell.log` 0 ERROR, 0 WARN; no Notes store | recorded |
| Transfer | stripped `doors-shell` from the riscv64 DRM build, gzip + base64 over the console: 458,260 B in 95 s, md5 `f32ee88b…` equal on both ends | PASS |
| Install | rollback copy `/root/doors-shell.667ca37`; only `S90doors-shell` stopped and started; installed md5 `793ff503…` (944,816 B), 0755 root; the shell answers `build c025779`, supervised, running, crashloop 0, restarts 0 | PASS |
| Touch path | taps written as input events into `/dev/input/event1`, raw coordinates by inverting the calibration the shell logged for the rotation in force (the Calculator method); keyboard keys at the DS §17.3 geometry | used for every tap below |
| Landscape on the panel | captures against the simulator (theme Slate): the editor with a typed note above the keyboard and the confirmation within 13 per channel everywhere below the status bar; the empty state within it but for 16 px of the disc's antialiased edge | PASS |
| Landscape behaviour | New note, `Hello world⏎Second line` typed on the sheet, Delete, Cancel (keyboard and focus back), ` more`, Done: the file is `Hello world\nSecond line more` byte for byte; reopened, `⏎æøå` from the symbol layer, Done: `C3 A6 C3 B8 C3 A5` appended, 35 B, 0644, store 0755, no `.tmp` | PASS |
| Long note, landscape | a 40-line note opened and typed into at its end: the field scrolled and the typed line is in view above the keyboard | PASS |
| **Turned with a note open** | the long note's edit unsaved, `shell.rotation mode=portrait`: `close app notes`, restart in place (pid 747 -> 747, restarts 0), portrait, on the launcher; the typed line is in the file | PASS |
| Portrait on the panel | the editor with the same typed note and the confirmation against the simulator: within 13 per channel below the status bar but for 84 px of the blinking caret | PASS |
| Portrait behaviour | a new note typed and Delete confirmed: nothing stored; an existing note deleted through the confirmation: exactly that file gone | PASS |
| **Turned back with a note open** | typed ` Y`, `mode=landscape`: saved on the way out, pid 747, restarts 0, landscape | PASS |
| Long list and read-only note, landscape | 14 notes: six rows beside the rail, New note at the top of it; a 2,537 B note over the editor's 2,000 characters opens read-only with the keyboard down, its field ending 10 px above the body's foot; nothing but background in the 30 px foot corner squares on either screen; Done leaves the file byte-identical | PASS |
| Back to Automatic | `mode=automatic`: portrait (keyboard absent), pid 747, restarts 0; the 14-note list with New note clear of the corners (0 px in the corner squares) | PASS |
| Health after | 57 new `shell.log` lines, 0 ERROR, 0 WARN, no LVGL message; 0 crash reports; no crashloop marker; no segfault, oops or panic in `dmesg`; sysd, netd and radiod running, restarts 0; `doors-shell` VmRSS 12,672 kB with Notes open (the Calculator gate's figure) | PASS |

Unit A was left running build `c025779`, Automatic (portrait, keyboard base
absent), theme Slate, with Notes open on a store of 14 gate notes (the store
did not exist before the gate; `rm -r /var/lib/pocketos/notes` with Notes
closed empties it). Captures, simulator references and the bench scripts:
`out/notes-landscape-c025779/hwgate-unitA/` (outside the repository).

**Rollback** (if wanted): stop `S90doors-shell`, copy
`/root/doors-shell.667ca37` to `/usr/bin/doors-shell`, start it again
(`/tmp/ngate/n_rollback.sh` until the next reboot).

## What only the panel and a finger can show

A capture is the framebuffer, not the glass, and injected events do not show
where a finger lands or how typing feels. The physical check:

1. **Portrait** (as the unit is): Notes looks right, text readable; tap a
   note, type a word on the sheet, Done.
2. **Landscape** (Settings > Display > Rotation > Landscape, then Notes):
   looks designed for landscape - rows beside New note, nothing clipped or
   overlapping, nothing cut by the rounded corners; the list scrolls under a
   finger; open a note: the field above the keyboard with Done and Delete
   beside it, the balance sensible; type a line with a thumb; Delete then
   Cancel; Done.
3. **Back to Automatic**: Notes still looks right in portrait.
4. Optional, physical keyboard: power off, attach the base, power on
   (Automatic opens landscape), type a line into a note on the keys.

**Result: PASS** - the product owner at the panel, 2026-09-16, build
`c025779`:

| Check | Owner's finding |
| --- | --- |
| Portrait | looks correct; text readable |
| Touch | lands correctly |
| Landscape | looks intentional; nothing clipped or overlapping |
| List | scrolling feels correct |
| Editor | the balance of field, Done and Delete looks good; typing works |
| Delete, Cancel, Done | behave correctly |
| Back to Automatic | restores the portrait layout correctly |
| Portrait editor field 20 px taller (DS §23.3) | acceptable |

The optional physical-keyboard check (4) was not reported and is not claimed.

## Rebased onto 65224de

On 2026-09-16 the owner merged the shared PocketUI fix for the clipped error
caption to master (`0ec7b36`, docs `65224de`) - the first pre-existing issue
recorded above - and asked for the branch to be rebased onto it, the Notes
geometry adapted where the caption needs it, and the whole remotely gated
again. The pre-rebase branch is kept locally as
`backup/notes-landscape-pre-rebase` (`763e88f`).

**The rebase.** `45cecad`, `c025779`, `90da3c0` and `763e88f` became `ba0c34e`,
`be3c415`, `c34bce3` and `b6ad9f2` on `65224de`. Two conflicts, both resolved
keeping both sides:

- `tests/notes_app_test.c`: master's new helpers and checks (`find_label`,
  `find_labelled` on top of it, `check_caption`, `fills_wrapper`, and the
  caption checks in the Notes sections) and the branch's layout helpers and
  sections 17-20 were added in the same place. All of both was kept; the
  branch's own `find_label`, identical in meaning to master's, was dropped.
  Against master the file deletes exactly the eight lines the original branch
  commit deleted, and adds every line it added but that duplicate.
- `docs/apps/POCKETNOTES.md`: the tests table - the branch's rows, with
  master's list of error captions merged into the `notes_app_test` row.

**What the shared fix meant for Notes, and what was changed.** The field now
grows into its wrapper and a caption takes its room from it. Every Notes
screen without an error is pixel-identical to the build the owner checked
(`c025779`), in both orientations, Normal and Outdoor, with 30 px and square
corners (simulator, below the status bar; the only other differences are the
blinking caret). What changes is the caption itself, now seen. Three things in
Notes had to follow, each found by a check that failed first:

| Commit | Found | Change |
| --- | --- | --- |
| `08b201e` | In landscape above the keyboard, the wide shape's field floor (94 px in a 100 px body) pushed a failed save's caption out of its wrapper and under the keyboard (y 254..274 under a wrapper ending at 251). And on `65224de` itself, a note that fits the portrait field opened scrolled to its last lines over empty field: its text went into a hidden editor whose field was still at its three-line floor. | The wide shape's field has no floor of its own: the whole wrapper, or all of it but the caption. The editor is shown, keyboard and caption in place, before the note goes in. |
| `74cd459` | On unit A, running `f49b1de`: in a freshly started Notes, a failed save on a two-line note in landscape left the field blank above its caption, 0 text pixels for 4 s and more, the text intact. The first caption of an app instance takes the field's room for one layout pass, the floorless field scrolled its caret into that moment and LVGL finished the scroll afterwards. Reproduced in the simulator (label at y 108..149 over a 152..222 field). | A failed save or delete lays the editor out, drops that scroll, readjusts the field and places the caret again. |
| `cb37b46` | A mutation removing an explicit layout call in `open_editor` survived: setting the text lays the screen out anyway. | The note goes in through one helper whose position a mutation can move; moved before the editor is shown it fails. |

Geometry with the caption (30 px corners): landscape failed save, field
152..222 (Outdoor 152..217), caption 231..251 (226..251), keyboard from 272;
read-only note, caption 517..537; portrait failed save, field 228..886,
caption 895..915; read-only, caption 1181..1201. Docs: `f49b1de`, `ebd5a01`.

**Host validation of `ebd5a01`, from a fresh clone.**

| Check | Result |
| --- | --- |
| `make all` (-Werror) | rc 0, 0 warnings |
| `make test` | 3,731 ok, 0 FAIL, 0 warnings |
| SDL simulator build | rc 0, 0 warnings |
| 20 shell and UI test scripts | 494 ok, 0 FAIL, every script rc 0 |
| `notes_app_test` | 1,138 checks, 0 failures (880 before the rebase; master's 8 caption checks; 250 for the captions and opening in both shapes and modes) |
| `pos_input_test` / `pos_keyboard_test` | 80 / 63 checks, 0 failures (the shared fix's section 8b included) |
| `notes_view_test` / `notes_store_test` / `notes_lint.sh` | 38 / 51 / 0 failures |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0; 4 warnings, all `vendor/ggwave`; 0 first-party |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings; `Doors 0.0.10 build ebd5a01` |
| Mutations of `notes_app.c` | 17 of 17 caught, each run beside a passing unmutated run: on `74cd459`, the 14 above (the floor mutant rewritten for the new rule), the stale scroll kept and the caret not placed again; on `cb37b46`, the note put in before the editor is shown |

**Unit A, remote, build `ebd5a01`** (serial console, userspace only; the
unit's LVGL is the vendor SDK's copy, the same 9.5 sources as the host's).

| Step | Evidence | Result |
| --- | --- | --- |
| Install | `f49b1de` then `ebd5a01`, each by gzip + base64 over the console with md5 equal on both ends (`ebd5a01`: 458,410 B, `77fd87a8…`), only `S90doors-shell` restarted; the shell answers `build ebd5a01`, supervised, restarts 0; rollback copies `/root/doors-shell.c025779`, `.f49b1de`, `.74cd459` | PASS |
| Owner's notes | the store as the owner left it after the physical check (15 files) copied to `/root/notes-before-gate2` before three test notes were seeded, and put back at the end, byte for byte | PASS |
| The defect, again | fresh Notes, the long note opened and closed, the two-line note opened, typed into, its file made immutable (`chattr +i`, so the save's rename fails for root too), Done: 278..370 text pixels in the field for 4 s in landscape and in portrait (0 on `f49b1de`); the file unchanged, no `.tmp` | PASS |
| Failed save, landscape | the note, its caret, the caption under it and the keyboard below, captured; cleared, Done: the edit on disk byte for byte | PASS |
| Failed delete, landscape | fresh Notes: the note in view, the caption at the foot, "Note not deleted", the file still there and unchanged; Done leaves it as it was | PASS |
| Read-only note, landscape | the "too long to edit" caption at the foot, 0 px in the corner squares | PASS |
| First open, landscape | the 40-line note opens at its end, nothing past it | PASS |
| Turned with a note open | ` y` typed, Portrait: saved on the way out, pid 2087 kept, restarts 0; later ` z`, Landscape: the same | PASS |
| Portrait | the defect sequence (text kept), a failed save captured (note, caption, keyboard), cleared and saved | PASS |
| Back to Automatic | portrait, pid 2087, restarts 0; the restored list with nothing in the corner squares | PASS |
| Health after | 61 new `shell.log` lines, 0 ERROR, 0 WARN, no LVGL message; 0 crash reports; no crashloop; no segfault, oops or panic; sysd, netd, radiod running, restarts 0; VmRSS 12,544 kB with Notes open | PASS |

**The physical check is not repeated.** Without an error every screen is the
one the owner checked, to the pixel; the caption states are the shared fix's
and are proven here on the panel.

Unit A was left running build `ebd5a01`, Automatic (portrait, keyboard base
absent), theme Slate, Notes open on the owner's restored store. Evidence:
`out/notes-landscape-ebd5a01/hwgate-unitA/` (outside the repository).
