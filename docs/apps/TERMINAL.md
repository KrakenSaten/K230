# Terminal

A real Linux terminal on the device: the account's own login shell on a
pseudo-terminal, typed at with the keyboard base (or the touch keyboard),
shown on a monospace grid. Not a command console: every byte goes through
the kernel's PTY and line discipline, so job control, Ctrl+C, `stty`,
`top`, `vi` and tab completion are the shell's and the tools' own, exactly
as over SSH.

Status: Terminal 0.1 is in master (PR #15), DS §43 (PROPOSED), gate
`docs/hardware/TERMINAL_GATE.md`. **Terminal 0.2 - the text size, the kept
session and the CLI toolbox - is on branch `feat/terminal-power-user`, not
merged**: DS §49 (PROPOSED), gate
`docs/hardware/TERMINAL_POWER_USER_GATE.md`.

## What it does

- **Opens a shell.** The login shell `/etc/passwd` names for the account
  Doors runs under (root, `/bin/sh`, BusyBox ash on the image), started as a
  login shell (`-sh`) in its home directory, so `/etc/profile` sets the
  prompt and `PATH` as it does for an SSH login. `POCKETOS_TERMINAL_SHELL`
  runs another program instead, not as a login shell (used by the tests).
- **Types.** The keyboard base's keys go to the shell: letters and symbols,
  Enter, Backspace, Tab, Esc, the arrow keys, and Ctrl and Alt chords
  (Ctrl+C, Ctrl+D, Ctrl+L, Ctrl+Z, ...). Without the keyboard base, a tap on
  the grid brings up the shell's touch keyboard.
- **Shows.** A grid in the Design System's monospace face, filling the
  app's body inside the corner clearance above the session row, at the
  system's text size (DS §46, §49). Colours from programs (`ls`, prompts)
  are drawn in the theme's tokens and identity accents. On the panel (unit
  A, `stty size`, columns x rows):

  | Text size | Face | Cell | Portrait | Landscape |
  | --- | --- | --- | --- | --- |
  | Small | mono 14 px | 8 x 18 | 65 x 57 | 148 x 20 |
  | Medium | mono 17 px | 10 x 21 | 52 x 49 | 118 x 17 |
  | Large | mono 20 px | 12 x 26 | 43 x 39 | 98 x 14 |

  A change of the setting applies at once, the app open or not: the grid is
  measured again in the new cells and the program is told its new size
  (SIGWINCH), as when a desktop terminal changes its font. What is already
  on the screen is not reflowed; new output wraps at the new width.
- **Scrolls back.** 1000 lines above the screen. Shift+Up and Shift+Down
  move half a screen, a drag moves line by line; the header says
  SCROLLBACK while the view is not live. Any other key goes to the shell and
  brings the view back.
- **Keeps the session.** Leaving the app - the back slab, Back, a
  function key, another app - takes the screen away and nothing else: the
  shell, everything it runs and everything it writes stay, and the next
  open shows the same screen, live. A long command runs on at full speed
  while other apps are used; its output lands in the scrollback.
- **Closes the session.** CLOSE SESSION, in the row under the grid, asks
  first (DS §17.5: Cancel accented, Esc and Back are Cancel); confirmed, the
  shell and everything in its session end, the PTY and the screen's memory
  are released, and the app is left. The next open starts a fresh shell.
- **Ends.** When the shell exits (`exit`, Ctrl+D) or is killed, the screen
  says how, in reverse video, the row says SHELL ENDED, and Enter starts a
  new one on the same screen. If that happens while the app is closed, the
  next open shows how it ended. An ended shell is not kept once the app is
  left: the open after that is a fresh shell.

## Architecture

In the Doors shell's own process, no helper: nothing in the terminal needs
privileges or a library the shell does not have, and a PTY master is just
one more non-blocking descriptor. Four LVGL-free parts, one screen:

| Part | Does |
| --- | --- |
| `apps/terminal/term_screen.c` | the VT parser and the screen: a fixed ring of cells (screen + scrollback), the cursor and modes, damage per row, reports to send back |
| `apps/terminal/term_keys.c` | a key as the bytes a VT100 sends |
| `apps/terminal/term_pty.c` | the PTY, the shell's process, its session and its end |
| `apps/terminal/term_session.c` | the three joined: bounded pumps of output, keys and reports down the PTY, the end announced, Enter restarts |
| `apps/terminal/terminal_app.c` | the session's lifetime and timer, and the LVGL screen: a custom-drawn grid, the session row and its confirmation, keys, touch, layout |

### The session and the screen (terminal_app.c)

One session per Doors shell run, held by the Terminal module rather than by
its screen: the shell's PTY, the VT screen with its scrollback, and one LVGL
timer (`TERMINAL_TICK_MS`, 30 ms) that pumps output into it. The app's
`create()` attaches a screen to the session - starting one if there is none
- and `destroy()` detaches it. While no screen is attached the timer goes
on reading the program's output into the screen, so nothing it writes is
lost and nothing it does is slowed by back-pressure; once the shell has
ended and everything it wrote has been read, the timer pauses until the next
open.

The session ends only in four ways, each through the same bounded close
(term_pty.c below):

| How | Then |
| --- | --- |
| CLOSE SESSION, confirmed | the app is left (after the press, never inside it); the next open is fresh |
| the shell ended and the app is left | the next open is fresh |
| the Doors shell stops (SIGTERM, a supervisor stop) | `app.h` `shutdown`, after the open app is closed |
| the Doors shell re-executes itself for a rotation | the same hook, before the exec: an exec would close the PTY master and leave the session's processes to nobody (the rotated shell reopens Terminal with a new shell) |

The `shutdown` member of `struct pocketos_app` is new and optional; the
Terminal is the only app that sets it. The shell calls it for every app of
its registry once, on the LVGL thread, on its way out.

A text-size change (or a theme or mode change) reaches the screen through
`pos_theme_add_listener`; the timer then takes the new face, measures the
grid again and resizes the session. A change made over IPC re-creates the
open app (DS §46.2): that is a detach and an attach, and the session is the
same.

### The shell's lifecycle (term_pty.c)

1. **Start.** `/dev/ptmx` is opened `O_RDWR | O_NOCTTY | O_CLOEXEC |
   O_NONBLOCK`; the slave is opened close-on-exec; the window size is set
   and the line discipline told that DEL erases and a character is UTF-8.
   Everything the child needs (argv, environment, messages) is prepared
   before `fork()`, because the Doors shell has threads.
2. **The child** calls `setsid()` and takes the slave as its controlling
   terminal (`TIOCSCTTY`), makes it stdin/stdout/stderr, closes every other
   descriptor, sets every signal back to its default and unblocks them all
   (the Doors shell's dispositions must not reach it, or Ctrl+C would not
   interrupt anything), asks for `SIGKILL` if the Doors shell dies
   (`PR_SET_PDEATHSIG`, checked against the race), and `execve`s. Its
   environment is the Doors shell's own less what describes another
   terminal (`TERM`, `COLUMNS`, `LINES`, `SSH_*`), plus `TERM=vt100` and
   `HOME`.
3. **Running.** Reads and writes on the master never block. Keys that the
   PTY will not take now wait in a 4 KB queue; the excess is counted and
   dropped.
4. **The end.** Each tick asks `waitid(..., WEXITED | WNOHANG | WNOWAIT)`:
   the shell's end is seen **without reaping it**, so its pid - which is the
   session id - cannot be reused while the session is cleared. Every live
   process of the session gets SIGHUP and SIGCONT; after 300 ms whatever is
   left gets SIGKILL; only then is the shell reaped, the rest of the output
   read, and the master closed. A job the user moved out of the session on
   purpose (`setsid`) is not the terminal's and is left alone.
5. **Close.** Closing the session (the table above) does the same at once: the master is closed
   (the kernel hangs up the terminal), SIGHUP and SIGCONT to the session,
   up to 300 ms for it to go, SIGKILL for what stays, up to 500 ms to reap
   the shell. A close during a flood takes 1-13 ms on the host and 17 ms
   on unit B.
   A shell stuck in the kernel beyond that is remembered and reaped on the
   next open or close, so it is never forgotten.
6. **If the Doors shell dies** instead, the kernel closes the master: the
   shell gets SIGKILL (the death signal) and its foreground job SIGHUP.

The session's members are found by reading `/proc/<pid>/stat` (field 6) -
only on the way out, never while the shell runs.

### Output, and when it comes faster than it can be drawn

Each tick (30 ms) takes at most 16 KB of output (`TERM_PUMP_BUDGET`). What
a program writes beyond that stays in the kernel's PTY buffer, and once that
is full the program's own `write()` blocks: the program runs at the speed
the screen can take, and no memory grows on this side. The host test
measures it: `yes` writes 0 bytes in 500 ms while nobody reads. A screen
that scrolls as a whole is redrawn at most every 60 ms; a line being typed
redraws its row only. Keys are written the moment they arrive, independent
of output, so Ctrl+C stops a flood at once.

While the app is closed the session's timer takes at most 4 KB a tick
(`DETACHED_PUMP_BUDGET`, about 130 KB a second): far more than a build or
a log writes, so their output is all kept; a program that floods is held by
the same back-pressure rather than taking CPU from the app in front.

### Memory and CPU

The screen and its scrollback are one allocation made when the session
starts: 160 x 1100 cells of 4 bytes, 704,000 bytes. It lives as long as the
session, so it stays allocated while the app is closed and a shell is kept,
and is freed when the session ends. Nothing is allocated for output after
that (the host test feeds 10 MB and checks the heap).

On unit B (docs/hardware/TERMINAL_GATE.md): doors-shell uses 1 % of the CPU
at the launcher and 1 % with the Terminal idle at the prompt; under `yes`
flooding the screen it takes 35 % while `yes` is held to 25 %; RSS goes from
14.9 to 15.5 MB after the first flood and stays there. Terminal 0.2 on unit
A (docs/hardware/TERMINAL_POWER_USER_GATE.md, the production compiler
flags): 2 % idle, open or closed; under `yes` flooding 13 % with the
Terminal open and 4 % with it closed (the detached budget above); RSS flat
over 50 leaves and reopens.

## The VT subset (term_screen.c)

TERM is `vt100`; the screen implements what that asks for and what a shell,
BusyBox and the common tools use:

- **Text:** UTF-8 (a character split across reads is kept; invalid,
  overlong and surrogate sequences become U+FFFD), combining marks take no
  cell, characters above the BMP are one replacement cell. Autowrap with
  the VT100's pending wrap (a full line then CR LF leaves no blank line).
- **C0:** BS, HT, LF/VT/FF, CR, SO/SI, CAN/SUB (abandon a sequence), ESC;
  BEL, NUL, DEL and the rest do nothing.
- **ESC:** 7/8 save and restore the cursor, D index, E next line, M reverse
  index, H set a tab stop, c reset, `( B`/`( 0`/`) B`/`) 0` the ASCII and DEC
  line-drawing sets (drawn with `+ - |`).
- **CSI:** CUU CUD CUF CUB CNL CPL CHA HPA HPR VPA VPR CUP HVP, ED 0/1/2/3,
  EL 0/1/2, ICH DCH ECH IL DL SU SD, REP, CHT CBT TBC, DECSTBM (scroll
  region), CSI s/u, SGR, SM/RM 4 (insert), DECSET/DECRST 1 (application
  cursor keys), 7 (autowrap), 25 (cursor), DSR 5 and 6, DA.
- **SGR:** 0, 1, 4, 7, 22, 24, 27, 30-37, 39, 40-47, 49, 90-97, 100-107,
  38/48;5;n and 38/48;2;r;g;b (folded onto the 16 colours). Bold is drawn
  lighter (the face has no bold weight).
- **Not supported, parsed and ignored:** the alternate screen (1049),
  bracketed paste, mouse modes, cursor shapes, OSC (window titles), DCS,
  APC, PM, SOS, sub-parameters (`38:5:1`), origin mode, double-width
  characters.

Malformed input fails safe: an unknown or malformed sequence prints nothing
of itself and leaves the parser ready for the next text; more than 16
parameters, a parameter over 65535, a CSI over 256 bytes, ESC followed by a
byte above 0x7F, and a string sequence over 4 KB without its terminator are
all abandoned rather than obeyed or waited on. Reports a program asks for
(DSR, DA) queue at most 128 bytes.

## Keys (term_keys.c, pos_input)

| Key | Sends |
| --- | --- |
| a character | its UTF-8 |
| Enter | CR |
| Backspace | DEL (the tty's erase); Ctrl+Backspace BS |
| Tab / Shift+Tab | HT / `ESC [ Z` |
| Esc | ESC |
| arrows | `ESC [ A-D`, or `ESC O A-D` in application cursor mode; with Shift/Alt/Ctrl `ESC [ 1 ; m A-D` |
| Ctrl + letter | its control code (Ctrl+C ETX, Ctrl+D EOT, Ctrl+L FF, Ctrl+Z SUB, ...) |
| Ctrl + `@ [ \ ] ^ _ ?` (and 2-8) | NUL ESC FS GS RS US DEL |
| Alt + key | ESC, then the key |
| Shift+Up / Shift+Down | not sent: the view scrolls |

The keyboard base has no Home, End, Delete or Page keys; their encodings
exist for the touch keyboard and later sources.

**The raw key target.** Everywhere else in Doors, Tab moves focus, Esc
cancels, Enter presses, and Ctrl is not a key at all: the physical
keyboard's driver tracked it but delivered only the letter. Terminal 0.1
adds two things to the input stream (`ui/pocketui/pos_input.h`), both
invisible to every other app:

- `pos_input_push_key_mods()`: the physical keyboard
  (`ui/shell/shell_kbd.c`) sends the modifiers held at each press with the
  key. Ordinary delivery drops them, so a note still gets `c` for Ctrl+C.
- `pos_input_set_raw_target()`: while the terminal's grid is the focused
  object of the group the device delivers to, each key reaches it as one
  flagged value that LVGL does not act on (no focus move, no press, no
  cancel), decoded with `pos_input_raw_decode()`. When a system alert takes
  the keys (DS §18.8) the grid is not focused and delivery is ordinary.

## Tests

| Suite | What |
| --- | --- |
| `tests/term_screen_test.c` | 151 checks: text, wrap, cursor, erase/insert/delete, SGR, region, tabs, line drawing, reports, UTF-8, malformed and unknown sequences, every split of a mixed stream equal to the whole, 1.6 MB of weighted random bytes, the scrollback bound, resize, no allocation over 10 MB |
| `tests/term_keys_test.c` | 44 checks: every key's bytes |
| `tests/term_session_test.c` | a real `/bin/sh` on a real PTY: prompt, computed output, Backspace, Tab, arrows and Esc (seen by `od -c`), TERM and no SSH variables, the window size and a resize, UTF-8 both ways, colours, Ctrl+C, Ctrl+D, Enter restarts, `exit 3`, `kill -9 $$`, background and SIGHUP-ignoring jobs at exit, a program that cannot run, close during a flood (bounded time, nothing left, reaped), close with a SIGHUP-ignoring and a stopped job, 50,000 lines (every byte, no pump over budget, scrollback at its bound), back-pressure (`yes` blocked while unread), Ctrl+C through a flood, 25 opens/closes with no descriptor or child left. Runs on the device too (`TERM_TEST_SHELL`) |
| `tests/terminal_app_test.c` | the app under real LVGL devices, portrait and landscape: focus and the raw target, grid and session row inside the body and the corner clearance, keys through the input stream to the shell (Enter, Backspace, Tab without a focus move, arrows, Esc, Fn+F5, Ctrl+C, the plain letter without Ctrl), Shift+Up/Down and a drag through the scrollback and back, the tap asking for the touch keyboard only without the base, a smaller body telling the shell its size; Small, Medium and Large in both orientations (the cells are the size's face, the columns and rows fill the grid, the shell is told, a long line wraps at the new width, CSI cursor positioning lands on its row and column, the scrollback scrolls, the same shell throughout, a size chosen while closed is the one it opens in); leaving during a flood keeps the session and its output read, reopening finds it, a long command runs to its end with no screen, 10 leaves and returns with no descriptor gained, the shell exiting while closed, Enter restarting, an ended shell let go, CLOSE SESSION's confirmation (Cancel focused, Esc and Back cancel, nothing ends before the confirm), the close within its bound, the shell's exit hook; 12 mixed opens and leaves; no timer/child/descriptor/raw target left; an ordinary field still getting the letter and Tab still moving focus |
| `tests/terminal_shell_test.sh` | the above, and the real shell opening Terminal in both orientations: no fault, open and close logged, and no process carrying the run's environment mark left once the shell is gone; then a job started in the terminal running on while the launcher is up, the same shell after a reopen and a text-size change over IPC, and the shell's SIGTERM ending a session kept in the background |
| `tests/terminal_lint.sh` | registration (top level, not in a folder), icon, only term_pty.c starts processes, the PTY flags, setsid/TIOCSCTTY/death signal/default signals, the non-reaping wait, nothing waits on the LVGL thread's path, one allocation in the screen, only the Terminal takes raw keys, the session's timer made only where a session starts, the shell's exit hook wired, make test wiring |

`make terminal-test` and `make terminal-san-test` (ASan, UBSan, leaks) run
the first three and the lint.

## Limitations

- **One session.** One shell, kept while the app is closed; no tabs, no
  multiplexer. `setsid cmd &` still puts a job outside the session, where
  CLOSE SESSION does not reach it.
- **A rotation, or a Doors shell restart, ends the session.** The Doors
  shell re-executes itself to rotate (attaching or removing the keyboard
  base in the Automatic mode does it too); the session is closed cleanly
  first, and the Terminal reopens with a new shell. Keeping it across the
  exec would mean handing the PTY and the screen to the next image.
- **The session row costs four rows** at Small (64 px) in both
  orientations.
- **Resizing does not reflow.** A text-size change keeps the text where it
  is; a narrower grid hides what is past its edge (until the program
  redraws), as xterm does.
- **VT100 subset, no alternate screen.** Full-screen programs (`vi`, `top`,
  `less`) draw on the main screen and leave their last frame in the
  scrollback; ncurses programs see a VT100 without colour.
- **No copy and paste, no selection, no themes.** The grid's size is the
  system's text size; there is no Terminal font setting of its own.
- **The keyboard base has no Home/End/PgUp/PgDn/Delete/F-keys** (the
  function row is reserved by the keymap), and the touch keyboard has no
  Ctrl, Esc, Tab or arrows.
- **No double-width characters**; the face covers Latin-1 and General
  Punctuation, anything else is drawn as `?`.
- **The raw key target is one object** (the Terminal's grid); no other app
  uses it.

## CLI toolbox

The image carries a small fixed set of command-line tools for the Terminal
and SSH, built from Buildroot's own packages by
`BR2_PACKAGE_POCKETOS_TOOLBOX` (`platforms/k230/package/pocketos/Config.in`,
default on with the Doors package; the defconfig is unchanged). There is no
package manager and nothing prebuilt.

| Tool | Version (Buildroot 2025.02.1) | What for |
| --- | --- | --- |
| htop | 3.3.0 | processes, CPU and memory, interactively |
| nano | 8.2 | an editor that is not vi |
| curl | 8.12.1 (was already in the image) | HTTP from the shell |
| jq | 1.7.1, with Oniguruma 6.9.9 for `test`, `match`, `sub` | JSON, e.g. `doors shell info \| jq .current` |
| strace | 6.13 | what a process asks the kernel |
| iperf3 | 3.18 | network throughput |
| tcpdump | 4.99.5, with libpcap 1.10.5 | packets |

`less` and `tree` are BusyBox's, already in the image; `file` was left out
(its magic database is several megabytes). Licences and upstreams:
`docs/LICENSING.md`, "Power-user CLI toolbox". The tools draw for a
VT100 (`TERM=vt100`): htop and nano work in it without colour.
