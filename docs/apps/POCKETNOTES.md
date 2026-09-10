# PocketNotes

A list of notes and a place to write one. It exists to be the first thing on
PocketOS that takes text, and it is deliberately the smallest app that can
honestly be called useful.

Status: **MVP, v0.0.8 M5.** Built on the text field of DS §17.1, the focus
model of §17.2, the touch keyboard of §17.3 and the one logical key stream of
§17.4. Nothing about it is hardware-specific; it has not yet run on a K230.

## What it is

- **List.** Every stored note, newest first. A row is 64 px with the whole
  row as its hit area (DS §9): the title on the left, the time it was last
  changed on the right. The title is the note's first non-blank line — there
  is no separate title field to fill in.
- **Empty state.** The DS §9 empty state when there are no notes.
- **New note.** A full-width button under the list.
- **Editor.** One multi-line text field, the keyboard under it, and two
  actions: Done and Delete.
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
loses nothing.

## Tests

| Test | Covers |
| --- | --- |
| `tests/notes_view_test.c` | titles: first non-blank line, trimming, truncation on a UTF-8 boundary, untitled; blankness; what counts as UTF-8 |
| `tests/notes_store_test.c` | generated names, an absent store, write/read/list, newest first, the atomic write and its stale temporary, empty notes, files that are not text, the size cap, foreign filenames, delete and id reuse |
| `tests/notes_app_test.c` | the app under a real pointer device and a real keyboard against a real store: create, type, Enter, æ ø å through the symbol layer, Done, the stored bytes, reopen and edit, a blank note, the app closing mid-edit, delete cancelled and delete confirmed, an unreadable note left alone, a long note scrolling |
| `tests/notes_shell_test.sh` | the app test, plus the shell owning exactly one keyboard, no app creating one, and Notes opening from the launcher |
| `tests/notes_lint.sh` | the store is the only file that touches the filesystem, the text rules are LVGL-free, no keyboard is named, no title reaches a path, the write stays atomic, and none of the excluded features exist |

## Not validated on hardware

Everything above is host evidence. The bench items that only a panel can
answer are the fit of a 296 px keyboard under a real editor, whether 52 px
keys are usable with a thumb (DEV-1's open question), and whether
`/var/lib/pocketos/notes` behaves on the card the way the other app stores
do.
