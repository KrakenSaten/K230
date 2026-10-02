# Terminal 0.2 gate: text size, kept session, CLI toolbox

**Unit A as left (2026-10-02 10:38Z): `doors-shell` production build of
`68d5d11` on branch `feat/terminal-power-user` (md5 `09c10955…`, no test
hooks), rotation mode Automatic (landscape, keyboard base attached), text
size Small, no Terminal session open; userspace otherwise master `9eda04e`
as deployed on 2026-10-02, plus the seven toolbox tools from the `c655e2b`
image build (23 files).** Rollback: `/root/rollback-term-power/RESTORE.sh`
puts back `doors-shell` `1df9438` (md5 `48159d74…`), `settings.conf` and
`/etc/default/doors-shell` as found, and removes the 23 toolbox files
listed in `TOOLBOX-ADDED`. Unit B was not touched.

Branch base: origin/master `83bfdf3`. Not merged, no PR.

## Builds

| What | Source | Notes |
| --- | --- | --- |
| Gate shell, pass 1 | `aac3411`, test hooks | functional pass; this build was compiled without Buildroot's `-O2` (a gate-script mistake), so its CPU figures are not used |
| Gate shell, pass 2 | `48925cb`, test hooks, Buildroot CFLAGS | the functional checks repeated, and the CPU figures |
| Shell left on unit A | `68d5d11`, production (no hooks), Buildroot CFLAGS | `strings` finds no key hook |
| Image | `c655e2b` (`out/doors-0.2.1-c655e2b-toolbox`, sysimage SHA-256 `9039084c…ce561d`) | IMAGE GATE PASS; **not flashed**: its toolbox files were copied onto the unit once |

The test hooks (`POCKETOS_SHELL_TEST_HOOKS`, gate builds only) give the
Terminal's key FIFO, `/tmp/term-keys`, through which keys enter the input
stream as the keyboard base's driver pushes them (key and modifiers). Taps
are real: multitouch slot-0 events written to the touch controller's evdev
node (`tests/hw/touch_slot0_tap.py`). Kit: `C:\K230\out\term-power-gate`
(not in the repository).

## Text size (both passes, same results)

`stty size` from the shell inside, the layout audit (`shell.audit`), and a
capture at each size. The size was changed over IPC with the Terminal open,
which re-creates the app: **the same shell (one pid) through all six.**

| Orientation | Small | Medium | Large | Audit |
| --- | --- | --- | --- | --- |
| Landscape 1232 x 568 | 20 rows x 148 cols | 17 x 118 | 14 x 98 | 0 clipped, truncated, overlapping, zero at each |
| Portrait 568 x 1232 | 57 x 65 | 49 x 52 | 39 x 43 | the same |

Captures: the colours (`ls`, a red/green line) and the prompt at each size;
a long `ls -la` wraps at 43 columns in portrait Large as it should.

## Kept session

| Check | Result |
| --- | --- |
| A counting loop (`echo tick $n` once a second) started; Terminal left; Calculator used with real taps for about 30 s | the loop counted on (4 -> 33 in pass 2, 4 -> 39 in pass 1), the shell pid alive throughout |
| Terminal reopened | the same shell pid; the ticks written while away on the screen; typed input runs (`back-5`); Ctrl+C stopped the loop (count frozen); Shift+Up shows SCROLLBACK, Shift+Down returns |
| 20 leaves and reopens with a background job, then 30 more | doors-shell fds, RSS (15,160 kB) and threads unchanged; one shell pid; the job still there |
| The shell exits (`exit 3`) with the app open | ENDED in the header, SHELL ENDED in the row; the job ended with it; ptmx closed. Leaving lets the session go (fds back to the baseline) |
| The shell exits by itself while the app is closed (`sleep 2; exit`) | nothing left, no zombie; reopening shows the end; Enter starts a new shell (new pid) |
| A rotation (Automatic <-> Portrait, the Doors shell re-executes) with a session and a `sleep 1000` job in the background | both gone after the exec, no zombie, ptmx 0; the shell log shows `close app`, `restarting in place`, `start`; Terminal reopens with a new shell |

## CLOSE SESSION (real taps)

| Check | Result |
| --- | --- |
| Tap CLOSE SESSION (landscape Small) | the §17.5 dialog in place of the grid, CANCEL accented and focused; audit clean |
| Tap CANCEL | back to the grid; the same shell and its background job still running; the shell still answers |
| Tap CLOSE SESSION, then CLOSE SESSION in the dialog | home; the shell and `sleep 1000` gone; no child of doors-shell, no zombie; ptmx 0, `/dev/pts` back to 1, doors-shell fds back to the baseline |
| Open Terminal again | a fresh shell (new pid), an empty screen |
| Portrait Large, with `yes > /dev/null &` running (pass 1) | dialog audit clean; confirm ends the shell and `yes`, nothing left |
| Production build `68d5d11` | open, leave for Settings, reopen = the same pid; closed by touch, nothing left |

## Keyboard (through the key hook)

Typing, Backspace (`abX⌫c` -> `abc`), Enter, Ctrl+C (`rc=130`). Into `od -c`:
Up/Down/Left/Right `ESC [ A/B/D/C`, Esc, Tab `\t`, Alt+x `ESC x`, Fn+F1
`ESC O P`, Fn+F5 `ESC [ 1 5 ~`, Fn+F12 `ESC [ 2 4 ~`. The keyboard base's own
keys were not pressed: **physical typing and Fn+F-keys are left for the
owner** (the driver path to the input stream is covered by
`tests/kbd_shell_test.sh`).

## Stability and CPU (pass 2, production compiler flags)

- doors-shell: 2 % CPU idle with the Terminal open and closed; under `yes`
  flooding 13 % with the Terminal open and 4 % with it closed (the detached
  output budget, 4 KB a tick). RSS 14.6-16.2 MB over the whole gate, flat
  across 50 leaves and reopens.
- No doors-shell crash or unplanned restart (the pid changed only at the
  deploys; rotations re-execute in place), no crash report, no new ERROR
  line in `shell.log` (2 old ones before and after), no zombie at any point.

## CLI toolbox

Seven tools, 23 files, 4,572,536 bytes on the root file system (htop
313 kB, nano 166 kB, jq 348 kB + oniguruma 582 kB, strace 1.49 MB, iperf3
195 kB, tcpdump 1.21 MB + libpcap 273 kB; curl was already there). Free space
on unit A went from 57,220 to 52,700 kB.

| Tool | On unit A |
| --- | --- |
| htop 3.3.0 | runs in the Terminal (Medium portrait) and over SSH; see the finding below |
| nano 8.2 | edited and saved a file from the Terminal (Ctrl+O, Ctrl+X) |
| curl 8.12.1 | `--version` (OpenSSL 3.4.1) |
| jq 1.7.1 | `length`, `test`, `sub` (Oniguruma) on JSON; `doors shell info \| jq` |
| strace 6.13 | `-c true`, `-e trace=openat` |
| iperf3 3.18 | loopback server and client, about 6 Gbit/s |
| tcpdump 4.99.5 | captured ICMP on `lo` |

**Finding: htop 3.3.0 aborts now and then.** Three of about thirty runs
during the gate ended in htop's own `abort()` with its backtrace
(`xSnprintf` under `Process_writeField`) - one in the Terminal and two
under a plain Python PTY on the unit with no Doors code involved, so it is
htop's, not the Terminal's. None in 21 quiet runs or in 6 runs with
`/bin/true` spawned in a loop, none under `strace`; all three while SSH
commands were coming and going. Trigger ASSUMED (a process appearing or
exiting during htop's scan), not identified. A newer htop is a package
upgrade and needs a proposal.

The Terminal's rendering of htop was checked separately: htop's raw output
(8 s, recorded under a plain PTY at 52 x 49 and 148 x 20) replayed into
`term_screen.c` gives a screen identical to tmux 3.2a's, cursor included.

## Host validation of `68d5d11` (clean clone, focused; no full make test)

`make all` 0 first-party warnings; riscv64 `doors-shell` (DRM, sysroot
LVGL, Buildroot CFLAGS) 0 first-party warnings. `make terminal-test`:
term_screen 161/0, term_keys 57/0, terminal_lint 0 failures, term_session
52 checks with 1 failure in the clean-clone run ("no job of its session
outlives it", counted the instant the end is announced, while three builds
loaded the machine), then 5/5 clean on the branch and 5/5 on master
`83bfdf3` once idle; `term_session.c`, `term_pty.c` and that test are
unchanged by this branch. terminal_app_test 217 checks 0 failed,
terminal_shell_test 0, kbd_shell_test 0 (shell_kbd_test 50 checks),
hw_actions_shell_test 0, package_sync_test 0, notices_test 0, style_lint 0.

## Left for the owner

- Physical keys on the keyboard base: typing, arrows, Fn+F-keys in the
  Terminal.
- CLOSE SESSION and the dialog under a real finger, and the look of the
  session row.
- Flashing the `c655e2b` image is not needed for this branch; the unit has
  its toolbox files.

## Follow-up: the first frame's cursor (2026-10-02, `0330d93`)

On unit B a new session's first frame left a cursor-coloured block at the
top-left once the login banner (which starts with a new line) moved the
cursor away without writing row 0. Fixed in `0330d93`; `terminal_app_test`
now checks the flushed pixels of that cell (144 cursor pixels before, 0
after). Unit A runs the production build of `0330d93` (md5 `b229c364…`,
11:52Z): first frame with the banner clean, the cursor only at the prompt;
open, leave, reopen kept the shell; CLOSE SESSION by touch left nothing;
`settings.conf` and `identity.id` unchanged. **Unit B was left on `2ace5b7`**:
the owner was using it (RIFT open, a Terminal session running, settings
changed), so its shell was not restarted. Rollbacks unchanged.
