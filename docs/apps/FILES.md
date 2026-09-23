# Files

A file explorer: browse folders, read text files, and create, rename, copy,
move and delete - with the places Doors and the system depend on kept
read-only.

Status: **v1, merged to master 2026-09-23 (after v0.0.11). Host-tested (unit tests
against a real temporary tree, and the app under a real LVGL pointer and the
real touch keyboard in portrait and landscape). Unit A gate run 2026-09-23
on build `2bccc5b` (`docs/hardware/FILES_GATE.md`): everything run passes,
after three fixes the gate found; the keyboard-base step was not run. The
layout is DS §33 (Amendment Q), ACCEPTED 2026-09-23, with the landscape
launcher wrap it brings.**

## What it does

- **Browse.** The launcher opens Files in the owner's home (`$HOME`, which the
  init script sets to `/root`; else `/root`; else `/`). Up goes to the parent
  folder and selects the folder it came from. Every path can be browsed,
  system paths included; hidden files are shown.
- **Select, then act.** A tap selects an entry; a tap on the selected entry,
  or **Open**, opens it: a folder is entered, a text file is shown.
- **Details.** Every row shows the name and a caption: size and time for a
  file, type and time for a folder. In landscape the details pane shows the
  selected entry's full name (two lines), type, size, modification time and
  whether it can be changed.
- **Read text.** A regular file of up to 32 KB is shown read-only; a longer one
  is shown to 32 KB and says so. A file with NUL bytes is refused as not text.
  Bytes that are not UTF-8, and control characters, are shown as `?`.
- **New folder, Rename.** A name is typed on the shell's keyboard. A name is
  refused, in the field and without touching the disk, when it is empty, `.`
  or `..`, contains `/` or a control character, starts or ends with a space, is
  not UTF-8 or is longer than 255 bytes.
- **Copy, Move.** Copy or Move carries the selection; the owner goes to the
  target folder and presses **Paste here** (or **Cancel**). A copy into a
  folder that already has the name becomes `name (2).ext`, `name (3).ext`...;
  a move never replaces anything and refuses a taken name.
- **Delete.** Always confirmed first (Cancel is the accented choice, DS §17.5);
  a folder is deleted with everything in it.
- **Sort.** Name (A to Z, case-insensitive), type (extension), size (largest
  first) or date (newest first); folders always first.

A folder of more than 200 entries shows the first 200 and says how many there
are (a folder is read up to 2048 entries).

## What it is not (v1)

No network shares, archives (ZIP), thumbnails or image preview; no text
editing; no multi-select; no search; no trash or undo; no free-space display.
`tests/files_lint.sh` holds the scope.

## Safety

Browsing and reading are allowed everywhere. Changing something is allowed
only **strictly inside** a writable root - `/root`, `/home`, `/tmp`, `/mnt`,
`/media` - and never **at, inside or around** a protected path:

- Doors' own directories, from `core/pocketpaths.h` including any environment
  override: runtime (`/run/pocketos`), config (`/etc/pocketos`), state
  (`/var/lib/pocketos`) and logs;
- `/root/.ssh`.

So `/etc`, `/usr`, `/boot`, `/proc`, `/sys`, `/dev`, `/var` and `/` itself are
read-only; a writable root itself (`/root`, `/tmp`...) cannot be renamed, moved
or deleted; and a folder that contains a protected path cannot be moved or
deleted either. Paths are resolved through symbolic links before they are
judged, so a link inside `/root` that points at `/etc` does not open a way in
(the link itself can still be removed; it is never followed).

The app shows what the policy refuses as disabled actions and a "Read-only"
line. The operations enforce the policy themselves (`apps/files/files_fs.c`),
whatever the caller checked.

Existing files are preserved on errors:

- nothing is ever replaced: renames use `renameat2(RENAME_NOREPLACE)` (with a
  checked fallback where the kernel or filesystem lacks it), and copied files
  are created with `O_EXCL`;
- a copy is built under a hidden temporary name (`.files-partial-*`) and
  renamed into place only when complete; a copy that fails, is refused
  half-way (a pipe or device inside a folder) or is stopped is removed again,
  and its source is only ever read;
- a move within one filesystem is one rename; across filesystems the copy is
  completed and synced before the original is removed, and if the original
  cannot then be removed completely, both remain and the app says so;
- a delete walks the whole tree first and refuses a tree that holds another
  mounted filesystem (or is nested deeper than 32) before anything is removed;
  it never follows a link.

## Architecture

| File | Role |
| --- | --- |
| `apps/files/files_fs.[ch]` | Listing, sorting, paths, names, the write policy and every operation. No LVGL. The only file that touches the filesystem. |
| `apps/files/files_job.[ch]` | One worker thread for copy, move and delete. No LVGL. |
| `apps/files/files_view.[ch]` | Sizes, types, times, drawable names, the shortened path. No LVGL. |
| `apps/files/files_app.c` | The LVGL app (`app_files`, id `files`). |

Listing a folder and reading a text file are bounded and run on the LVGL
thread from a tap. Copy, move and delete can take as long as the storage
takes, so they run on the worker; a 30 ms LVGL timer collects the result.
No file operation runs in a layout or draw pass. Opening from a row is
deferred to that timer, because it rebuilds the row it was tapped on. Closing
the app while a job runs stops it (a copy at its next 64 KB chunk, removing
what it made) and waits for the thread; a copy flushes every 4 MB, so that
wait is short (245 ms for a 40 MB copy on unit A).

Registered like any other app: `ui/shell/shell.c` registry, launcher group
DEVICE in the files colour (`ui/shell/home_layout.c`), the B package's `files`
glyph in its portal (`icon-files.bin`), and the icon extension's `files` mask
for the fallback (`pos_app_icons.c`).

## Layout

DS §33. Portrait: the path bar (Up and the path, shortened from the front),
Sort and New folder, the list, a status line, and the five actions across the
foot. Landscape: the list column with Sort and New folder in the path bar, and
a 420 px details pane with the actions under it; the name entry puts the
field and its two buttons in one row above the keyboard. The layout is chosen
from the body the app is given, never from the orientation, through
`pocketui_layout_begin`.

## Tests

- `tests/files_fs_test.c` (`make test`): paths and names, listing and the
  four sorts, the policy (roots, Doors' data, the folder around it, links,
  `..`, the default policy from the environment), mkdir/rename/copy/move/
  delete including every refusal and what each leaves on disk, a copy
  stopped or failing half-way, a move across filesystems (`/dev/shm`),
  permission failures (skipped as root), 255-byte names, text reading and
  the worker.
- `tests/files_view_test.c` (`make test`): the on-screen text.
- `tests/files_lint.sh` (`make test`): layering, no filesystem access outside
  `files_fs.c`, the worker boundary, the no-replace and no-follow rules, the
  policy in every operation, the v1 scope, and the responsive-layout rules.
- `tests/files_app_test.c` via `tests/files_shell_test.sh`: every journey
  tapped and typed, in portrait and landscape, touch targets and the corner
  safe area, a change of orientation while carrying, a folder vanishing
  underneath, a permission failure, and closing the app mid-copy; then the
  real shell opening Files in both orientations with nothing drawn in the
  panel's rounded corners.

On unit A (the gate): the operations on the card's ext4 root, the read-only
places as they are on the device, long copies with the UI unaffected, and
closing mid-copy. Not tested: a vfat mount, and Enter on the keyboard base.
