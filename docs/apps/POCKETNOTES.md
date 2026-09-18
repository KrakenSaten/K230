# PocketNotes

A list of notes and a place to write one. It exists to be the first thing on
PocketOS that takes text, and it is deliberately the smallest app that can
honestly be called useful.

Status: **MVP, v0.0.8 M5. Validated on unit A, 2026-09-10, and in the v0.0.8
release image, 2026-09-11.** Built on the
text field of DS §17.1, the focus model of §17.2, the touch keyboard of
§17.3 and the one logical key stream of §17.4.

## What it is

- **List.** Every stored note, newest first. A row is 64 px with the whole
  row as its hit area (DS §9): the title on the left, the time it was last
  changed on the right. The title is the note's first non-blank line — there
  is no separate title field to fill in. Once there are more rows than the
  screen holds, they scroll inside the list; the screen and the body do not.
- **Empty state.** The DS §9 empty state when there are no notes.
- **New note.** A full-width button under the list, which stays on screen
  however long the list grows; in landscape, at the top of a rail beside it
  (see Layout).
- **Editor.** One multi-line text field, the keyboard under it, and two
  actions: Done and Delete, above the field in portrait and beside it in
  landscape.
- **Delete.** A confirmation under DS §17.5, with Cancel accented because
  deleting a note cannot be undone.

## What it is not

No search, no folders, no tags, no formatting, no sync, no undo, no text
selection beyond LVGL's own, no sort options, and no physical keyboard. None
of these are deferred designs; they are simply not part of the MVP, and
`tests/notes_lint.sh` fails the build if any of them appears.

## Typing

The app never creates a keyboard, never holds a pointer to one, and never
learns where a character came from. It asks the shell for the one keyboard
through `app.h` and then forgets about it; what it types into is a focused
`pocketui_text_field`, and characters arrive there whether they were tapped
on the sheet, typed on the simulator's host keyboard or, later, on a physical
keyboard. That is DS §17.4, and `tests/notes_lint.sh` enforces it by name.

Showing the keyboard shrinks the shell's content area by the sheet's height,
so the editor keeps a usable 808 px above it without the app knowing the
keyboard's geometry. Enter inserts a line break: the editor is multi-line, so
its return key is ENTER and not DONE (DS §17.3).

æ, ø and å are on the keyboard's symbol layer, by the ruling that closed
caveat C9. They are ordinary characters here — the store is UTF-8 and the
field takes them like any other.

## Layout

DS §23 (Amendment G, accepted), on the pattern of §22.3. The app puts one
frame in the body the shell gives it - exactly the body's content box - and
its three screens inside that. Each screen is shaped from the frame's size,
never from the orientation, and shaped again whenever the frame changes size,
which in practice is the keyboard coming up or going down. The objects are
built once; a change of shape moves nothing but flow and sizes, so the open
note, its caret, the focus and the keyboard are untouched by it. Only the
list's rows are rebuilt, from the store, as they always were.

| Shape | When | Arrangement |
| --- | --- | --- |
| **tall** | the frame is at least as tall as it is wide, or too narrow for the wide shape | content above or below its actions, as in v0.0.10 |
| **wide** | wider than tall, and wide enough that the content beside a 288 px rail keeps the portrait body's 528 px (836 px in all) | actions in a 288 px rail on the right, content taking the rest of the width and the full height |

**Why a rail.** Landscape is short of height, and most of all above the
keyboard: the shell takes the sheet's 296 px off the content area across its
whole width, which leaves the app a 1192 × 100 px body. Under a row of Done
and Delete that left the field 4 px (v0.0.10, where landscape was portrait
stretched). Beside them the field has all 100 px, and every action keeps its
own height, so no body can bring a control under the touch minimum.

**The foot clears the rounded corners** (DS §22.2): the frame pads its foot
by however far a corner square reaches into the body, computed by PocketUI's
`pos_display_rect_insets()` from `pocketui_display_geometry()` and handed to
the app by the shared layout guard (`pocketui_layout_begin()`, DS §22.4) - the
same rule and the same guard as every other responsive app. That is 10 px when something reaches the foot
of a full-height body - a long list, or the field and caption of a note that
opens read-only with the keyboard down - and 0 above the keyboard or on a
panel with square corners.

**Error captions** (DS §17.1). The field grows into its wrapper and a caption
takes its room from the field (`pocketui_text_field`), so a refusal, a failed
save or a failed delete is always read under the field, inside the wrapper,
clear of the keyboard. The editor is shown - keyboard, caption and all -
before the note goes into the field, so the field scrolls to the caret for
the size it is seen at. The first caption of an app instance is created when
it is first shown, and for one layout pass it takes the field's room; the
field's scroll begun for that moment is dropped and the caret placed again for
the field's real size, or a short note in the landscape editor was left
scrolled out of sight above its caption (found on unit A).

**The frame has no row gap.** LVGL 9.5 takes a row gap from a growing flex
item for every sibling before it, hidden ones included, so with the three
screens straight in the body's 20 px flow the editor (the second screen) was
always 20 px shorter than the body: it stopped 40 px above the keyboard in
portrait instead of the 20 of DS §7. Inside a gapless frame it has the full
height.

### Portrait

The body is 528 × 1060 px, and 528 × 764 with the keyboard up (1232 − 56
status bar − 296 sheet − 72 header − 24 − 20).

| Screen | Rectangles (30 px corners) |
| --- | --- |
| empty list | empty state 20..547 × 152..307, New note 20..547 × 328..391 - as v0.0.10 |
| long list | rows 20..547 × 152..1117, New note 20..547 × 1138..1201: 10 px higher than v0.0.10, clear of the corners |
| editor, keyboard up | Done 20..279 and Delete 288..547 × 152..207; field 20..547 × 228..915: **20 px taller than v0.0.10**, ending 20 px above the keyboard |
| editor, keyboard up, a save failed | field 20..547 × 228..886, caption 895..915 |
| editor, keyboard down (a note shown read-only) | field 20..547 × 228..1172, caption 1181..1201 |
| confirmation | 20..547 × 152..331 - as v0.0.10 |

With `POCKETOS_SAFE_CORNERS=0,0,0,0` the long list is v0.0.10's to the pixel
and a read-only note's caption reaches 1211.

### Landscape

The body is 1192 × 396 px, and 1192 × 100 with the keyboard up (568 − 56 −
296 − 72 − 24 − 20).

| Screen | Rectangles (30 px corners) |
| --- | --- |
| list | rows 20..903 × 152..537 (six rows in view; 152..307 for the empty state), New note 924..1211 × 152..215 |
| editor, keyboard up | field 20..903 × 152..251; Done 924..1063 and Delete 1072..1211 × 152..207 |
| editor, keyboard up, a save failed | field 20..903 × 152..222 (Outdoor 152..217), caption 231..251 (Outdoor 226..251) |
| editor, keyboard down (read-only) | field 20..903 × 152..508, caption 517..537 |
| confirmation | 352..879 × 152..331: its portrait width, centred |

A row is 884 px wide, so a title has 699 px before the time. The field shows
four lines of Normal body type above the keyboard, and three of Outdoor's.
The field's own floor of three body lines is counted by `pocketui` as the
font's line height times 1.5 (94 px Normal, 117 px Outdoor), which the 100 px
above the keyboard cannot hold in Outdoor, nor in Normal together with a
caption; a floor the wrapper cannot hold keeps the field taller than its box,
which pushes the caption out of sight and scrolls the caret into the part that
is cut off. So in the wide shape the field has no floor of its own: it is the
whole wrapper, or all of it but the caption's room. A failed save therefore
shows the note in 71 px (three lines) in Normal and 66 px (two and a half) in
Outdoor, with the caption under it and the keyboard below that.

### Turning the display

The shell turns the display by closing the open app and opening the display
again (DS §21.2). A note open at that moment is saved on the way out like on
any other close, and Notes comes back on the launcher in the new
orientation; the note is in the list, as written. Nothing else of the
editor's state - which note was open, the caret - survives, by the shell's
design rather than Notes'. `tests/notes_app_test.c` also turns the display
under the open app, which the shell never does, to prove the harder case:
the open note, its text, the caret position, the focus and the keyboard all
stay as they were, and typing carries on at the caret.

## Storage

`$POCKETOS_STATE_DIR/notes/`, default `/var/lib/pocketos/notes/`. One plain
UTF-8 file per note, no container format and no index:

```
/var/lib/pocketos/notes/note-00000001.txt
```

- **Names are generated, never derived from a title.** A title is user input
  and has no business in a path.
- **Writes are atomic**: temp file, `fsync`, `rename`. A reader sees either
  the previous note or the new one, never a partial file, and an interrupted
  write leaves only a `.tmp` that the next write overwrites and that the
  listing ignores.
- **A note is text.** A file with an embedded NUL, bytes that are not UTF-8,
  or more than 4096 bytes is *refused*, not repaired. It stays in the list,
  marked unreadable, and opening it gives a disabled field and an
  explanation — so leaving it cannot replace a damaged note with an empty
  one.
- **A blank note is not stored.** Writing nothing and leaving deletes the
  note rather than keeping an "Untitled" file.
- At most 64 notes and 4096 bytes each. The editor caps input at 2000
  characters; the keyboard cannot produce a character wider than two bytes,
  so that cannot exceed the byte cap.
- **A note longer than the editor holds is shown, not edited.** Only a file
  written somewhere other than this app can be over 2000 characters and
  still within 4096 bytes. It opens read-only and whole, with an
  explanation, and leaving it writes nothing: an editable field would have
  kept its first 2000 characters and the way out would have saved them over
  the rest.

Kept small and local on purpose. The common state facility on the roadmap
will absorb this together with the Fleet, Radar and Timber stores; this
milestone does not start that migration.

## Saving

There is no save button, and there is no autosave timer. A note is written
on the way out, and there are only two ways out:

- **Done**, which returns to the list;
- **the app closing**, which is the last moment an app gets under the v0.1
  lifecycle — there is no pause or resume (ADR-002), so `destroy()` saves.

Both paths run the same code, so leaving through the shell's back button
loses nothing. A note nobody changed is not written at all: opening a note
and leaving it is reading it, and writing the same bytes again would only
move its time and its place in the list.

## Tests

| Test | Covers |
| --- | --- |
| `tests/notes_view_test.c` | titles: first non-blank line, trimming, truncation on a UTF-8 boundary, untitled; blankness; what counts as UTF-8; counting characters rather than bytes |
| `tests/notes_store_test.c` | generated names, an absent store, write/read/list, newest first, the atomic write and its stale temporary, empty notes, files that are not text, the size cap, foreign filenames, delete and id reuse |
| `tests/notes_app_test.c` | the app under a real pointer device and a real keyboard against a real store, hosted in the shell's own frame: create, type, Enter, æ ø å through the symbol layer, Done, the stored bytes, reopen and edit, a blank note, the app closing mid-edit, delete cancelled and delete confirmed, an unreadable note left alone, a long note scrolling, a 3000-character note opened read-only and left byte for byte as it was, a note nobody changed keeping its time, twenty notes scrolled to the oldest by a finger in Normal and Outdoor with New note on screen throughout, a short list keeping its height, and every error caption (an unreadable note, a note too long to edit, a failed save in Normal and Outdoor, a failed delete) below the field and on screen, the field filling its wrapper again once the error clears. On the reference panel with its 30 px corners, and in both orientations: every screen's rectangles to the pixel with 30 px and square corners; every screen - long list scrolled by finger, empty list, editor with a long note typed into it, confirmation, a read-only note with the keyboard down - checked in Normal and Outdoor for controls inside the body and the safe area, no overlaps, the touch minimum, nothing under the keyboard, rows with title and time apart, the field whole in its container and three lines tall, the caret in view, and a body that never scrolls; the display turned under an open note (text, caret, focus and keyboard kept, typing on at the caret by tap and from the key stream, nothing saved until Done), under the confirmation and under a scrolled list; the shell's own way of turning (close, reopen) keeping every edit; in each shape and mode the errors: a failed save above the keyboard with its caption placed and the note still in view, a failed delete, a read-only note's caption at the foot clear of the corners; a note opened first after the app starts and a reopened long note never scrolled past their end; an 800 × 480 display keeping the tall shape; and six open-close rounds in both orientations leaving nothing behind |
| `tests/notes_shell_test.sh` | the app test and a clean LVGL log, plus the shell owning exactly one keyboard, no app creating one, Notes opening from the launcher, and Notes drawn by the real shell in both orientations: New note where the app test lays it out, nothing in the rounded corners, no warning |
| `tests/notes_lint.sh` | the store is the only file that touches the filesystem, the text rules are LVGL-free, no keyboard is named, no title reaches a path, the write stays atomic, none of the excluded features exist, and the layout: chosen from the body's size and never the orientation, redone on a size change, its handler gone before the app is freed, a gapless frame, corners from the display geometry through PocketUI's shared layout guard `pocketui_layout_begin()` with no copy of the rule or of the guard of its own, the wide shape's width guard, and rows rebuilt without rebuilding the objects around them |

## Hardware

**Validated on unit A, 2026-09-10**, runtime `0.0.8` build `0b16f0e`:
nineteen operator checks, PASS, no defect found
(`docs/hardware/POCKETNOTES_SMOKE_2026-09-10.md`).

What that settled: the editor is comfortable above the keyboard and nothing
scrolls; æ ø å type and render correctly from the symbol layer; autosave
through the shell's Back button keeps an uncommitted edit; the store comes
out 0755/0644 root-owned like Fleet's and Radar's; and the note survives both
a warm reboot and a real power cut, the latter with the kernel replaying its
journal and the file coming back byte-identical with no temporary left
behind.

The one qualified result is DEV-1. The 52 px keys work with a thumb and
every mis-key was correctable, but they mis-keyed at roughly one character in
ten. That is fine for a note and would not be fine behind an irreversible
action; see the smoke sheet before designing a screen that types into one.

**In the release image, 2026-09-11**, build `03851f5`, which carries the two
Notes fixes of the cold review (`docs/hardware/V0.0.8_RELEASE_SMOKE.md`,
section 3): 21 notes, with the list scrolled by finger to the oldest and New
note on screen throughout, where the list before `03851f5` clipped from
sixteen; a 1,980-character note opening in under 4 s and staying responsive
(an operator count; the slow path is in KNOWN_ISSUES); Back saving an edit,
and Done on an unchanged note writing nothing; delete with Cancel; the system
alert over the editor taking the keyboard away and not giving it back; and
every note byte-identical across a restart and a power cycle. The one note
that changed across the restart was changed by the operator: a Backspace in
the open note, saved on the way out like any other edit.

**In landscape, 2026-09-16/17** (`docs/hardware/NOTES_LANDSCAPE_GATE.md`,
PASS, DS §23 accepted): the owner's physical check of both orientations,
touch and typing on build `c025779`; then, rebased onto the shared
error-caption fix, remote validation of build `ebd5a01` - failed save, failed
delete and read-only captions in portrait and landscape, a note over 2000
characters opening read-only, rotation with a note open saving its edit, and
nothing drawn in the rounded corners.

Still untested on hardware: Notes at its limits (64 notes, the 4096-byte
cap), an unreadable note on a real card, and the keyboard under Outdoor mode's
20 px body.
