# Files on unit A: the gate

**Status: RUN 2026-09-23, 19:33–20:19 UTC. Everything that was run PASSES on
`2bccc5b`; step 8 (the keyboard base) is NOT RUN and needs the owner's hands;
DS §33 and the landscape launcher are the owner's decisions.**

**Unit A carries `2bccc5b`** (`/usr/bin/doors-shell` sha256 `d99e954f…`,
`icon-files.bin` `c3cee7e8…`; everything else RC1 `9a4afeb`). Left at home,
rotation Automatic (landscape, keyboard base attached), not locked, all five
services up, `meshcored` online. Rollback to RC1's shell:
`sh /root/rollback-files/RESTORE.sh` (copy sha256 `d5b78286…`).

The gate was prepared for `6465e38` and started on it. It found three defects
(F1–F3 below); they were fixed in `2bccc5b`, which was built the same way
(twice, byte-identical), installed over `6465e38` at 20:15 UTC, and the checks
the defects touched were run again on it. Nothing else changed between the
two, so the other `6465e38` results stand for `2bccc5b`.

## Result

| | |
| --- | --- |
| **PASS** | Install and identity (both builds); Files from the launcher in both orientations; browsing `/root`, `/`, `/etc` (through a link), `/var/lib/pocketos`; the viewer (14 KB text, scrolled); a binary refused; the touch keyboard for New folder (DONE) and Rename; the empty-name caption, portrait and landscape; Copy, a duplicate `readme (2).txt`, Move up a folder, Delete with Cancel then Delete, a folder deleted with its contents — each confirmed on disk; the read-only places (`/root/.ssh`, `/etc`, `/etc/pocketos`, `/tmp` itself, `/var/lib/pocketos` and `meshcored` in it) show only Open and Copy, and a tap on the disabled Delete does nothing; the landscape details pane and actions, Sort through all four, the carry bar in the pane; nothing in the rounded corners; long copies (below) |
| **FIXED ON THE GATE** | F1 long names wrapped out of their rows; F2 the selection outline ran through the glyph; F3 closing Files mid-copy held the shell 2.6 s and logged the wrong outcome — all three re-verified on `2bccc5b` |
| **NOT RUN** | Step 8, Enter on the keyboard base (its keys cannot be injected remotely: TCA8418 over I2C). The same path is host-tested (`files_app_test`: Enter pushed into the key stream creates the folder, once) |
| **OWNER** | DS §33; the landscape launcher (7.1 below) |

## Findings

**F1 — long names wrapped (6.7, 7.3). FIXED `2bccc5b`.** The 235-character
name took three lines, ran out of its 64 px row and hid its caption, in both
orientations (`caps/p-6.3b-entered.png`, `caps/l-7.2b-files-gate.png`).
`LV_LABEL_LONG_DOT` on a label that sizes its own height wraps instead of
ending in `…`. Every single-line label is now one line of its font tall. The
host test had only checked the row existed; `files_app_test` now checks every
row's text stays inside its row in both orientations, and fails 3 checks with
the old code. On `2bccc5b`: one line, `…`, caption shown
(`caps/p-R5-rows.png`, `caps/l-R2-selected.png`). The landscape details pane
(two lines then `…`) was right from the start.

**F2 — selection outline through the glyph (6.6). FIXED `2bccc5b`.** Cosmetic.
Rows now keep 8 px inside and the list 4 px top and bottom
(`caps/l-R2-selected.png`).

**F3 — closing Files mid-copy blocked the shell (9.3). FIXED `2bccc5b`.** On
`6465e38`, Back pressed at once after Paste here on a 40 MB copy: the
copy was in the page cache and the final `fsync` cannot be interrupted, so
`destroy()` waited 2.6 s (log 19:56:12.111 → 14.741), the copy completed, and
the log said "it was stopped". Data safe (identical, no partial), UI not. A
copy now flushes every 4 MB. On `2bccc5b` the same test: home **245 ms**
after Back (20:16:48.167 → .411), copy stopped, nothing left behind, log
"it was stopped and nothing was left half done".

**Observations (not defects of this branch):**

- 7.1 **The landscape launcher** wraps DEVICE onto a second line, as DS §33.4
  says, and **Lock and Controls are entirely below the fold** until the
  launcher is scrolled (`caps/l-7.1a-launcher.png`, `l-7.1b-launcher-scrolled.png`).
  Owner's decision.
- After Copy, Move, New folder or Rename the result is selected, so the next
  tap on its row opens it rather than selecting it. As designed; worth
  knowing (seen at 9.1).
- `/root/.config`, `/root/.ssh` and several `/` entries show 1970 dates:
  their real mtimes, from before the clock was set.
- Notes' empty-state glyph box: not checked here (Notes not opened).

## Deviations from the procedure

- **Free space**: 121.8 MB on `/` (step 1), under the 150 MB of trap 5. Step 9
  ran scaled: 24 MB for the responsiveness copy (twice) and 40 MB (not 96)
  for close-mid-copy.
- **9.1 measured, not only watched**: the capture tool takes ~8 s, too slow to
  see a copy in progress, so the shell's IPC (served on its LVGL thread) was
  timed every 100 ms instead: idle mean 20 ms / max 30 ms; during a 24 MB copy
  18 / 30; during a 40 MB copy on `2bccc5b` 17 / 30. The UI thread was never
  held.
- **Taps were injected** (evdev, 150 ms holds, `rift_tap.py`) and every screen
  captured with `kmsgrab`; the owner did not operate the unit. All captures:
  `out/files-gate-6465e38/caps/` on the build host (not committed).
- Step 4's `app start` also unlocks the shell, by design of `shell.home`.


## The build the unit must carry — check this before anything else

A physical check is only evidence about the build on the glass
(`docs/hardware/FLEET_LANDSCAPE_GATE.md` is why this section comes first).
Before anything is written down, `doors shell info` must say **`2bccc5b`**,
and the right-hand column is filled in at step 3.

| On the unit | The gate needs | Last recorded (v0.0.11 RC1 gate, 2026-09-23) | Filled in at step 3 |
| --- | --- | --- | --- |
| `/usr/bin/doors-shell` | **`2bccc5b`**, sha256 `d99e954f…` (this branch) | `9a4afeb` (RC1 image) | **`2bccc5b`**, `d99e954f…` (first `6465e38`, `f7ba3d90…`) |
| `/usr/share/doors/ui/icon-files.bin` | **new**, sha256 `c3cee7e8…` | absent | `c3cee7e8…` |
| everything else (`radiod`, `sysd`, `netd`, `meshcored`, `doors` CLI, the other art, `/etc/doors-release`) | **not replaced**: `git diff 9a4afeb..2bccc5b` touches nothing they are built or installed from | `9a4afeb` | not replaced |
| Rotation mode | record at step 1, restore at step 10 | Automatic, keyboard base attached (landscape), locked | Automatic (landscape); portrait for steps 6 and the F1 re-check; restored |

`/etc/doors-release` keeps saying `9a4afeb`: only the shell is swapped. The
shell reports its own build in `shell.info`, which is the one that counts.

**The build under test is `feat/files-app` at `2bccc5b`** (first `6465e38`), from origin/master
`63a276c` (the v0.0.11 release commit; its image is RC1 `9a4afeb`, and the
commits between are docs only). VERSION stays `0.0.11`. Not merged, and not to
be merged unless this gate passes. Commits after `2bccc5b` on the branch change
this sheet and nothing else.

## What this gate is for

| | |
| --- | --- |
| **HOST VERIFIED** | `files_fs_test` 179 checks, `files_view_test` 36, `files_app_test` 85 (portrait and landscape, every journey tapped and typed), `files_lint`, `files_shell_test`; `make test` green; the launcher and shell suites with thirteen apps (`doors_shell_test`, `chrome_shell_test`, `display_geometry_shell_test`, `notes_shell_test`, `wave_shell_test`, `calculator_shell_test`, `home_layout_test`, `app_icons_test`, `doors_ui_assets_test`) green. |
| **HOST VERIFIED** | The target build: riscv64 `doors-shell` from a `git archive` of `6465e38`, and again of `2bccc5b`, 0 objects before the build, 0 warnings, each built twice and byte-identical. See The payload. |
| **NOT VERIFIED — this gate** | Everything on the panel: Files in both orientations, touch on the real controller, the keyboard for names, the operations on the unit's own ext4 root filesystem, the read-only places as they exist on the device, the worker thread keeping the UI alive during a long copy, and the thirteen-app launcher (DS §33.4). |

What the branch adds (details in `docs/apps/FILES.md`, layout in DS §33):
a file explorer — browse, open text read-only, New folder, Rename, Copy, Move,
Delete (confirmed), Sort — that may change things only inside `/root`, `/home`,
`/tmp`, `/mnt`, `/media` and never Doors' own directories or `/root/.ssh`; and
Files on the launcher in DEVICE, which makes the landscape launcher wrap.

## On the panel, and not this branch's

| What you may see | Whose | |
| --- | --- | --- |
| Notes' empty state ("No notes yet") draws a hollow box instead of its file glyph | Notes, since its empty state was built: a text role set after the symbol font replaces it | found while building Files; Files' own rows are fixed; Notes is not changed here |
| A shell `WARN radio.status poll failed: timed out after 200 ms` | the status bar's poll of `radiod` while the radio is busy | pre-existing |
| Landscape top corners 50 px | master since `4e2c9b6` (v0.0.11) | as designed |

## Read before starting — the traps

1. **Only the shell restarts.** `S90doors-shell stop/start` restarts the shell
   and nothing else; `meshcored`, `radiod` and their state are not touched by
   any step here. Never run `deploy.sh` for this gate.
2. **The shell starts locked.** After step 3 the lock screen is up: swipe up
   (or Enter on the keyboard base) before looking for the launcher.
3. **A rotation change restarts the shell** in place and it comes back on the
   launcher, so Files is closed and reopened. Nothing the gate checks lives
   across that except "carried" state, which is host-verified only (below).
4. **Files may delete for real** inside `/root`. Every change the procedure
   asks for is inside `/root/files-gate/`, made at step 5 and removed at
   step 10. Do not practise on anything else in `/root`: `/root/app` is the
   vendor's application folder and is *not* protected by Files.
5. **Space.** Step 9 writes a 32 MB file and a copy of it. Step 1 records the
   free space on `/`; if it is under 150 MB, skip step 9 and write NOT RUN.
6. If taps are injected rather than made by hand, hold them 150 ms.

## The payload

Two files, on the build host (WSL) in **`~/work/files-gate-out-2bccc5b/`**
(`$OUT` below), with `SHA256SUMS` beside them. No image is built, `deploy.sh`
is not used, nothing under `/etc` is written.

| Path on the unit | sha256 | Size |
| --- | --- | --- |
| `/usr/bin/doors-shell` | `d99e954f45e867e938515218bce169d05753c119a1102d1f7f2597de9e2fa0cc` | 1 243 968 |
| `/usr/share/doors/ui/icon-files.bin` | `c3cee7e8d32f0bdb43e51be70aac66f1c1264a2b23ae179543e02dece5ffc688` | 27 660 |

Provenance: `git archive 2bccc5b`, `BUILD_ID` file `2bccc5b` beside `VERSION`
(as `apply_to_sdk.sh` writes it), `cmake -S ui/shell` with the SDK's
`toolchainfile.cmake`, `-DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot`,
target `pocketos-shell`, installed as `doors-shell`, not stripped (as the
earlier shell gates). Built twice in separate trees: byte-identical. The
recipe is `gate_payload.sh` in the session scratchpad; it is the commands
above. Libraries it needs on the unit: `libcjson.so.1 libm.so.6 liblvgl.so.9
libpthread.so.0 libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1
libc.so.6`. The sysroot build has always linked `pthread`, so this is not
knowingly a new dependency; step 1 checks each one on the unit.

`6465e38` is `095890b` (the Files commit) plus one fix found while preparing
this gate: New folder and Rename now commit on Enter from any keyboard (the
field's ready event, DS §17.4), not only on the touch keyboard's Done — so
Enter on the keyboard base works (step 8). Host-tested: `files_app_test` 82
checks, `files_shell_test`, `files_lint`.

`2bccc5b` is `6465e38` plus the three fixes this gate found (F1–F3, at the
top). Host-tested: `files_app_test` 85 checks, `files_fs_test` 179,
`files_view_test` 36, `files_lint`, `files_shell_test`.

---

# The procedure

Ten steps. 1–4 are remote and need nobody; 5 makes the test folder; 6–8 are
the panel, portrait then landscape, by hand; 9 is the long copy; 10 cleans up
and leaves the unit. From the build host (WSL):

```bash
A=root@192.168.10.157                      # unit A on the bench; confirm
KH=$HOME/work/rc1-gate-ssh/known_hosts     # the host key pinned after the RC1 flash
SSHO="-i $HOME/.ssh/pocketos_bench -o UserKnownHostsFile=$KH -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=8"
OUT=$HOME/work/files-gate-out-2bccc5b
RB=/root/rollback-files
u() { ssh $SSHO $A sh -s -- "$@"; }        # runs the script on stdin on the unit
```

Panel captures, when wanted: copy `out/rc1-gate/c_capture.sh` to
`/tmp/gate-tools/` on the unit and use `out/rc1-gate/cap.sh <name>` (kmsgrab;
the frame comes back in the shell's logical orientation).

### 1. Survey — the "before" reading

```bash
u <<'EOF'
echo "== release: $(tr '\n' ' ' < /etc/doors-release)"
echo "== shell: $(doors shell info | tr -d ' \t\n' | grep -oE '"(build|current)":"[^"]*"|"(width|height)":[0-9]+|"icons_(art|fallback)":[0-9]+' | tr '\n' ' ')"
echo "== rotation: $(doors call shell shell.rotation | tr -d ' \t\n' | grep -oE '"(rotation_mode|orientation|keyboard)":"[^"]*"' | tr '\n' ' ')"
sha256sum /usr/bin/doors-shell
ls -l /usr/share/doors/ui/ | grep -c '\.bin$'; ls /usr/share/doors/ui/icon-files.bin 2>&1
df -h / | tail -1
echo "== services: $(for s in radiod sysd netd meshcored doors-shell; do printf '%s:%s ' $s "$(pidof $s | wc -w)"; done)"
for l in liblvgl.so.9 libcjson.so.1 libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1 libpthread.so.0; do
    f=$(ls /usr/lib/$l /lib/$l 2>/dev/null | head -1); echo "lib $l: ${f:-MISSING}"
done
ls -ld /root/.ssh /var/lib/pocketos /root/app 2>&1
grep -v '^#' /etc/default/doors-shell 2>/dev/null
EOF
```

**Stop** if any library is `MISSING`, if `doors-shell` is not running exactly
once, or if the shell's build is not `9a4afeb` (then this sheet's "before" is
wrong: record what it is and ask). Write down the rotation mode.

### 2. Rollback copy and `RESTORE.sh`

```bash
u <<'EOF'
RB=/root/rollback-files
mkdir -p $RB
[ -f $RB/doors-shell ] || cp -p /usr/bin/doors-shell $RB/doors-shell
cat > $RB/RESTORE.sh <<'X'
#!/bin/sh
# Put back the doors-shell this gate replaced and remove the art it added.
RB=/root/rollback-files
/etc/init.d/S90doors-shell stop; sleep 1
cp -p $RB/doors-shell /usr/bin/doors-shell
rm -f /usr/share/doors/ui/icon-files.bin
sync
/etc/init.d/S90doors-shell start; sleep 3
sha256sum /usr/bin/doors-shell
echo "shell $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
X
sha256sum $RB/doors-shell
EOF
```

The hash printed must be step 1's. The copy is taken once; a second run keeps
the first.

### 3. Install, and the build identity

```bash
(cd "$OUT" && sha256sum -c SHA256SUMS) &&
ssh $SSHO $A '/etc/init.d/S90doors-shell stop; sleep 2' &&
tar -C "$OUT" --owner=0 --group=0 --numeric-owner -cf - doors-shell icon-files.bin |
  ssh $SSHO $A 'tar -C /tmp -xf - &&
    install -m 0755 -o 0 -g 0 /tmp/doors-shell /usr/bin/doors-shell &&
    install -m 0644 -o 0 -g 0 /tmp/icon-files.bin /usr/share/doors/ui/icon-files.bin &&
    rm -f /tmp/doors-shell /tmp/icon-files.bin && sync &&
    sha256sum /usr/bin/doors-shell /usr/share/doors/ui/icon-files.bin' &&
ssh $SSHO $A '/etc/init.d/S90doors-shell start'
u <<'EOF'
sleep 3
echo "shell:  $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"|"(width|height)":[0-9]+|"icons_(art|fallback)":[0-9]+|"locked":[a-z]+' | tr '\n' ' ')"
echo "shells: $(pidof doors-shell | wc -w)"
grep -E 'start .*build=|launcher: ' /var/lib/pocketos/log/shell.log | tail -2
EOF
```

| Must be true | |
| --- | --- |
| both installed hashes match the payload table | **write them into the table at the top now** |
| shell `"build":"2bccc5b"`, exactly one `doors-shell` | |
| `icons_art` 13, `icons_fallback` 0 | the new portal icon was found |
| the log says `launcher: 4 group(s), 13 app(s), <orientation>` | Files is registered |
| `"locked":true` | trap 2 |

### 4. Remote checks — Files opens, and nothing faults

```bash
u <<'EOF'
for o in portrait automatic; do
    doors call shell shell.rotation mode=$o >/dev/null; sleep 4
    doors app start files >/dev/null; sleep 1
    echo "$o: $(doors shell info | tr -d ' \t\n' | grep -oE '"current":"[^"]*"|"(width|height)":[0-9]+' | tr '\n' ' ')"
    doors app home >/dev/null; sleep 1
done
grep -cE 'open app files' /var/lib/pocketos/log/shell.log
grep -E ' ERROR |assert' /var/lib/pocketos/log/shell.log | tail -3
ls /var/lib/pocketos/log/crash* 2>/dev/null | tail -2
EOF
```

| Must be true | |
| --- | --- |
| both lines say `"current":"files"`, portrait then the original orientation | |
| no `ERROR`, no assert, no new crash report | |

If the unit was locked, `app start` opens behind the lock; that is fine here.
The rotation is back to Automatic at the end — **if step 1 recorded something
else, put that back instead.**

### 5. The test folder

```bash
u <<'EOF'
G=/root/files-gate
rm -rf $G; mkdir -p $G/docs $G/empty
printf 'Hello from Files on unit A\nsecond line\n' > $G/docs/readme.txt
printf 'abc' > $G/notes.txt
printf 'a\000b' > $G/data.bin
seq 1 3000 > $G/numbers.txt                      # longer than one screen
L=$(printf 'L%.0s' $(seq 1 230))-name.txt; printf 'long' > "$G/$L"
printf 'bl\303\245b\303\246r' > "$G/bl$(printf '\303\245')b$(printf '\303\246')r.txt"
ln -s /etc $G/to-etc
ls -la $G
EOF
```

### 6. Portrait, on the panel (by hand)

`ssh $SSHO $A 'doors call shell shell.rotation mode=portrait'`, unlock, and
open Files **from the launcher** (DEVICE, the third icon). It opens in `/root`.

| # | Do | Expect |
| --- | --- | --- |
| 6.1 | Look at the launcher first | DEVICE holds Settings, System, Files; Files' portal icon is the folder glyph in the files colour (same hue as Notes); portrait does not scroll |
| 6.2 | In Files: the whole screen | path bar with ↑ and `/root`; Sort: Name and New folder; the list; five actions across the foot, greyed; nothing in the rounded corners; the folder and file glyphs drawn (not hollow boxes) |
| 6.3 | Tap `files-gate`, tap it again | selected (outline), then entered; path `/root/files-gate` |
| 6.4 | Scroll the list with a finger | only the list scrolls; the bars stay |
| 6.5 | Tap `numbers.txt` twice | the viewer: Close, the name, the numbers; scrolling works; Close returns |
| 6.6 | Select `data.bin`, Open | the status line: not a text file; the list stays |
| 6.7 | The long name and `blåbær.txt` | the long one ends in `…` inside its row; the Norwegian letters are drawn |
| 6.8 | New folder → type `Photos` on the keyboard → Done | the keyboard comes up with the field focused; after Done it goes down, `Photos` is selected, the line says Created |
| 6.9 | New folder → Create with the field empty | the caption under the field: Type a name; Cancel |
| 6.10 | Select `notes.txt` → Rename → erase, type `todo` → Rename | the row says `todo`; step 10's `ls` confirms |
| 6.11 | Select `todo` → Copy → enter `docs` → Paste here | Copied; `docs/todo` exists and `todo` is still in `files-gate` |
| 6.12 | In `docs`: select `readme.txt` → Copy → Paste here | Copied as `readme (2).txt` |
| 6.13 | `readme (2).txt` (already selected) → Move → ↑ → Paste here | Moved; it is in `files-gate`, not in `docs` |
| 6.14 | Select `readme (2).txt` → Delete → Cancel, then Delete → Delete | Cancel keeps it; the second removes it, Deleted |
| 6.15 | Select `Photos` → Delete → Delete | folder gone |
| 6.16 | Select `to-etc` | Rename, Move and Delete enabled (the link itself is in `/root`); **do not delete it**; tap it again: it opens `/root/files-gate/to-etc` and New folder is greyed with "Read-only: system area" |
| 6.17 | ↑ to `/root`, select `.ssh` | Open and Copy only; Rename, Move, Delete greyed |
| 6.18 | ↑ to `/`, open `var`, `lib`, `pocketos` | browsable; inside `pocketos` New folder is greyed and the line says Read-only: protected Doors or private data |
| 6.19 | At `/`, select `etc` and `tmp` | both: Open and Copy only (`/tmp` is a writable root and cannot itself be renamed or deleted) |
| 6.20 | Back slab | the launcher; Files is closed |

Remote confirmation of 6.8–6.15 before going on:

```bash
u <<'EOF'
cd /root/files-gate && ls -la . docs && cat docs/todo && echo && ls -a docs | grep -c files-partial
EOF
```

Expect: `todo` in both folders (bytes `abc`), no `notes.txt`, no `Photos`, no
`readme (2).txt` anywhere, `docs/readme.txt` intact, and **0** partial copies.

### 7. Landscape, on the panel (by hand)

`ssh $SSHO $A 'doors call shell shell.rotation mode=automatic'` (keyboard base
attached → landscape; or `mode=landscape` without it), unlock.

| # | Do | Expect |
| --- | --- | --- |
| 7.1 | The launcher | CONNECTIONS, WORKSPACE, PLAY on the first line, **DEVICE (Settings, System, Files) on a second line**, and the launcher scrolls to Lock and Controls (DS §33.4). Record whether this is acceptable: it is the owner's decision (below) |
| 7.2 | Open Files, go to `/root/files-gate` | ↑, path, Sort and New folder in one bar; the list on the left; the details pane on the right saying Nothing selected; the actions under it, greyed |
| 7.3 | Select the long name | the pane: the name on two lines then `…`, `TXT file · 4 B`, Modified …, Can be changed |
| 7.4 | Select `to-etc` then `docs` | the pane follows; a folder shows Folder without a size |
| 7.5 | New folder, with the keyboard up | the field, Cancel and Create in one row above the keyboard; Create empty shows the caption there, readable; Cancel |
| 7.6 | Sort four times | Type, Size (largest first), Date (newest first), Name |
| 7.7 | Select `todo` → Copy | the pane shows Copy "todo", Paste here, Cancel; Cancel |
| 7.8 | Everything in the corners | nothing in the 50 px top corners or the 30 px bottom ones |

### 8. The keyboard base (if attached)

With Files open in landscape: New folder, type a name **on the base's keys**
(not the touch keyboard), Enter. Expect the same as touch typing: the folder
is made. Then remove it with Delete. If the base is not attached, NOT RUN.

### 9. A long copy — the UI stays alive, and closing stops it cleanly

```bash
u <<'EOF'
dd if=/dev/urandom of=/root/files-gate/big.bin bs=1M count=32 2>&1 | tail -1
sha256sum /root/files-gate/big.bin
EOF
```

On the panel (either orientation): in `files-gate` select `big.bin` → Copy →
enter `empty` → Paste here, and **while the status line says Copying**, scroll
the list and tap rows.

| # | Expect |
| --- | --- |
| 9.1 | the list scrolls and rows select during the copy; the actions are greyed until it finishes; then Copied |
| 9.2 | `sha256sum /root/files-gate/empty/big.bin` equals the original's |

Then again with a larger file, closed mid-copy:

```bash
u <<'EOF'
dd if=/dev/zero of=/root/files-gate/bigger.bin bs=1M count=96 2>&1 | tail -1
EOF
```

Select `bigger.bin` → Copy → enter `docs` → Paste here, and **at once** press
the back slab.

| # | Expect |
| --- | --- |
| 9.3 | Files closes within about a second; the launcher is usable |
| 9.4 | `ls -a /root/files-gate/docs` shows no `bigger.bin` and no `.files-partial-*` (unless the copy finished before the back press: then a whole `bigger.bin`, same size, and no partial) |
| 9.5 | `grep 'files: closed while' /var/lib/pocketos/log/shell.log` shows the stop, if it was still running |

### 10. Clean up, and leave the unit

```bash
u <<'EOF'
rm -rf /root/files-gate
doors call shell shell.rotation mode=automatic >/dev/null   # or step 1's mode
sleep 4
echo "shell: $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"|"(width|height)":[0-9]+' | tr '\n' ' ')"
grep -E ' ERROR |assert' /var/lib/pocketos/log/shell.log | tail -3
df -h / | tail -1
EOF
```

Then either **leave the unit on `2bccc5b`** (the default for a passed gate,
until the next build replaces it) and say so at the top of this sheet, or run
`sh /root/rollback-files/RESTORE.sh` to go back to RC1's shell. Either way,
record which in "Results".

---

## Results

| Step | Result | Evidence |
| --- | --- | --- |
| 1 Survey | PASS | RC1 `9a4afeb`, shell `d5b78286…`, all libraries present, one shell, Automatic/landscape/keyboard present, 121.8 MB free; `step1-survey.log` |
| 2 Rollback copy | PASS | `/root/rollback-files/doors-shell` `d5b78286…` = step 1, `RESTORE.sh` written |
| 3 Install / identity | PASS ×2 | `6465e38` (`f7ba3d90…`) at 19:34; `2bccc5b` (`d99e954f…`) at 20:15. Both: 13 icons from the art, 0 fallback, `launcher: 4 group(s), 13 app(s), landscape, 104 px cells (wrapped) … scrolls`, locked at start, one shell |
| 4 Remote open, both orientations | PASS | `current: files` at 568×1232 and 1232×568; no ERROR, no WARN, no crash report |
| 5 Test folder | PASS | `step5-fixture.log` |
| 6 Portrait (6.1–6.20) | PASS, F1 and F2 fixed | 6.1 DEVICE = Settings, System, Files, folder portal icon, no scroll; 6.2–6.6, 6.8–6.20 as the table says, each operation confirmed on disk; 6.7 failed on `6465e38` (F1), passes on `2bccc5b` |
| 7 Landscape (7.1–7.8) | PASS, F1 fixed | 7.1 wraps and scrolls (owner); 7.2–7.8 as the table says; 7.3's rows had F1 |
| 8 Keyboard base | NOT RUN | owner: Files → New folder, type on the base, Enter → the folder is made; then Delete it |
| 9 Long copy / close mid-copy | PASS on `2bccc5b`, F3 fixed | 9.1 IPC 17–18 ms mean during copies; 9.2 sha256 / `cmp` identical (24 MB ×2, 40 MB); 9.3–9.5 above |
| 10 Clean up, unit left on | PASS | `/root/files-gate` removed, Automatic restored, 5 services, `meshcored` online 181 nodes, `identity.id` `41e8a50a…` unchanged, 0 ERROR, 0 crash reports; unit left on `2bccc5b`; `step10-asleft.log` |

## Host only (not checked here)

Carried Copy/Move surviving a change of shape (on the device a rotation
restarts the shell, trap 3); a folder vanishing underneath and permission
failures (the unit runs as root, which is allowed everything the kernel
allows); a move across filesystems (the unit has one writable filesystem on
the card); symlinked-directory copy and the mount-point refusal.

## For the owner

1. **Accept or reject DS §33 (Amendment Q)** on this gate.
2. **The landscape launcher** (7.1): accept the wrap and scroll that thirteen
   apps produce under §31.3's existing rule, or ask for one row (a smaller
   `HOME_CELL_MIN_W`, or Files in another group — neither is in this branch).
3. Merge `feat/files-app` only after both.
