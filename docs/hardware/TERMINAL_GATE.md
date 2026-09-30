# Terminal 0.1 hardware gate (unit B)

**Unit B now carries master `bae3695` in full** (updated 2026-09-30 17:12 UTC):
every Doors binary, service, init script and the UI assets from a clean
build of `bae3695`, pushed with `platforms/k230/scripts/deploy.sh` (no
flash, no key hook). `doors shell info` build `bae3695`, `/etc/doors-release`
`BUILD_ID=bae3695`, `/usr/bin/doors-shell` md5 `5a1f8e43…`; `pos-mp3` and
`pos-video` installed for the first time; rotation mode `automatic`
(keyboard base attached: landscape). This includes the Terminal's two
post-review fixes (`0ba691b`), which ran here only as an open-and-close smoke
check (a login shell, nothing left after close, 25 apps on the launcher,
radio `rx`, meshcored online), not through the gate below. Rollback:
`/root/rollback-master-bae3695/RESTORE.sh` (back to what the unit carried
before: doors-shell `3235f38` from the Fleet chat gate and the v0.2.1
helpers and services; removes `pos-mp3` and `pos-video`). Deploy and
verification logs: `C:\K230\out\unitB-bae3695-prep`.

**The gate below ran on feat/terminal `c7e1910`**, the unit then carrying
that production build (`/usr/bin/doors-shell` md5 `f5d6b626…`, rollback
`/root/rollback-terminal/RESTORE.sh`). Its results are about that build.

Run 2026-09-30, 11:36-11:50 UTC, by Claude over SSH. Hot deployment, no
flash. Image v0.2.1 (9dc66c2), kernel 6.6.36, BusyBox 1.37.0 ash as root's
login shell, devpts mounted. Tooling: `C:\K230\out\terminal-gate`
(`g0`-`g4`, `env.sh`), logs in `logs/`, panel captures in `caps/`.

## How keys were typed

The keyboard base's keys cannot be pressed over SSH (TCA8418 on a
bit-banged bus in the shell itself). Steps 2-15 ran on a **gate build of the
same commit** with `POCKETOS_SHELL_TEST_HOOKS`, whose Terminal reads keys
from a FIFO (`/tmp/term-keys`) and pushes them into the input stream with
`pos_input_push_key_mods()` - the call the keyboard's driver makes - so the
path from there (the raw key target, the key encoding, the PTY, the shell)
is the one a physical key takes. The leg before it (TCA8418 -> keymap ->
modifiers) is tested on the host (`shell_kbd_test`: Ctrl+C, Shift+Up and Tab
reach a raw target) and is **left for the owner to press** (below). After
the gate the production build was installed.

## Results

| # | Check | Result |
| --- | --- | --- |
| 0 | Cross-built suites on the unit against its own ash and PTYs: `term_screen_test` 151, `term_keys_test` 44, `term_session_test` 52 | **PASS**, no process left |
| 1 | Terminal opens (`doors app start terminal`), header "Terminal", status cluster | **PASS** (`caps/01-open.png`) |
| 2 | A real shell prompt: `-sh` as the doors-shell's child, `/etc/profile` banner, `[root@canaan ~ ]#` | **PASS** |
| 3 | Keyboard (via the stream, see above): typed commands run | **PASS**; physical keys: **owner** |
| 4 | `uname -a`, `pwd` (`/root`), `ls` (23 entries), `ps` (103 lines), `free`, `df -h` - on the grid and in files | **PASS** (`caps/02`, `caps/03`) |
| 5 | History: Up then Enter runs the last command again (`hist-42` twice) | **PASS** |
| 6 | Tab completion: `ls /et<Tab>` completes to `/etc/` and lists it | **PASS** |
| 7 | Ctrl+C interrupts `sleep 30` (`rc=130`); Ctrl+L clears | **PASS** (`caps/05`) |
| 8 | Long output: `seq 1 20000` done in 0.5 s; `yes` flood, Ctrl+C stops it | **PASS** (`caps/06`) |
| 9 | Scrollback: Shift+Up shows older lines and SCROLLBACK in the header; Shift+Down comes back | **PASS** (`caps/07`, `caps/08`) |
| 10 | Landscape 148 x 24 cells, portrait 65 x 61 (`stty size`); a rotation with the Terminal open and a job running leaves no process; mode restored to automatic | **PASS** (`caps/10`, `caps/11`) |
| 11 | Close while `yes` floods, with `sleep 1000 &` and a SIGHUP-ignoring `sleep 1001 &`: none left, no zombie, PTY closed | **PASS** |
| 12 | Reopen 20 times, half with `sleep 999 &` running | **PASS** |
| 13 | No orphan shell: 0 login shells, no job, 0 zombies after all of it | **PASS** |
| 14 | No FD/resource growth: doors-shell fds 11 before and after, no `ptmx` open, `/dev/pts` holding only `ptmx` (as before the Terminal opened), RSS 14.9 MB flat over the 20 cycles | **PASS** |
| 15 | DOORS shell responsive: same pid throughout, 0 crashes, `doors shell info` answers, launcher shows Terminal in DEVICE after System | **PASS** (`caps/09`) |

CPU (doors-shell, `/proc/<pid>/stat` over 10 s): launcher 1 %; Terminal
idle at the prompt 1 %; `yes` flooding the screen 35 % (and `yes` itself
held to 25 %); idle again after it 1 %. RSS 14.9 MB -> 15.5 MB after the
first flood, then flat. Memory used 101 -> 102 MB.

Noted, not a defect: in `caps/03` a line reads `ee | tee /tmp/tg15566 ...`:
the gate typed the next command while `ps` was printing, the tty echoed it
into the output, and ash redrew over it - what any terminal shows for
type-ahead.

## Left for the owner (unit B on `bae3695`, keyboard base attached)

1. Open Terminal from the launcher (DEVICE, after System).
2. Type `ls` and Enter; `sleep 30`, then Ctrl+C; Up arrow; `ls /et` and Tab.
3. Shift+Up / Shift+Down for the scrollback; Esc in `vi` (`vi /tmp/x`, `:q!`).
4. Leave with the back slab; reopen.
