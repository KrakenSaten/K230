# PocketNotes hardware smoke, unit A, 2026-09-10

First hardware validation of the v0.0.8 text-input line: the logical key
stream (M3), the touch keyboard (M4) and PocketNotes (M5). Everything before
this was host evidence.

**Verdict: PASS.** Nineteen operator checks, no STOP condition, no defect
found. No transmission at any step; radiod stayed on the mock backend.

| | |
|---|---|
| Runtime | `0.0.8`, `BUILD_ID=0b16f0e` (master `0b16f0eab08d355fe672c92f34726b993c6f4b58`) |
| Installed by | `deploy.sh` over SSH to 192.168.10.157, from the Buildroot target tree rebuilt at that commit |
| Unit | A |
| Operator | owner, at the panel |
| Remote evidence | SSH from the development host, taken between operator actions |

**Deployed, not flashed.** No card reader was attached. The seven binaries,
three init scripts and `/etc/pocketos-release` are the same ones the image
carries, and the deployment survived both a reboot and a cold power cycle
below, so the init scripts are exercised. The v0.0.7 rollback tar remains at
`/root/v007-backup/pocketos-v0.0.7.tar`.

**A reboot was performed before the smoke began, deliberately.** A shell
started by `deploy.sh` inherits sshd's umask 0077, which is what made
PocketTimber's store read 0700/0600 and told us nothing about a real card
(`POCKETTIMBER_D3_V008_RETEST.md`). Rebooting first meant the Notes store was
created by a shell started by `rcS` with umask 0022, so the permissions below
are representative.

## Pre-flight, before the operator touched anything

| Check | Result |
|---|---|
| Identity | `0.0.8` / `0b16f0e` |
| Services | `sysd`, `radiod`, `pocketos-shell` all running, `crashloop: false`, 0 restarts |
| Crash markers | none; 0 crash reports; 0 ERROR lines |
| Apps registered | radio, system, fleet, radar, timber, **notes** |
| System info | answers; kernel 6.6.36 riscv64 |
| Notes store | absent — correct, nothing written yet |
| Shell umask | 0022 (boot-started) |
| **VmRSS baseline** | **11,776 kB**, launcher, before Notes |

## The nineteen checks

| # | Check | Verdict | Evidence |
|---|---|---|---|
| 1 | Notes tile appears and opens | PASS | operator: instant, correct; remote: `current: notes`, `open app notes` logged |
| 2 | Empty state correct | PASS | operator: dashed panel, disc, "No notes yet", nothing clipped |
| 3 | New note opens editor and keyboard | PASS | operator: both, placeholder and blinking caret; remote: store still absent |
| 4 | Editor usable above the keyboard | PASS | operator: keyboard ~20%, header+actions ~15%, rest for the note; no scrolling |
| 5 | Type a sentence with a thumb | PASS | operator: `bench test`, some mis-keys, all self-corrected |
| 6 | Shift one-shot and lock | PASS | operator: all three states worked, armed vs locked distinguishable; ~1 mis-key in 10 |
| 7 | `?123` and æ ø å | PASS | operator: all three correct on the panel, easy to find |
| 8 | Second line with Enter | PASS | operator: key reads ENTER, line break, both lines visible |
| 9 | Backspace and its repeat | PASS | operator: one per tap, clear pause, controllable, **no overshoot on release** |
| 10 | Done returns to the list | PASS | operator: keyboard away, list back |
| 11 | First line becomes the title | PASS | operator: title and æøå correct, timestamp present |
| 12 | Reopen and edit | PASS | operator: both lines, keyboard returned |
| 13 | Leave via shell Back with unsaved edits | PASS | remote: file 32 → 35 bytes, ends `second line ok` |
| 14 | Delete → Cancel | PASS | remote: file byte-identical, still 1 note; operator: editor and keyboard recovered |
| 15 | Delete → confirm | PASS | remote: 0 note files, directory intact, no `.tmp` |
| 16 | Create the persistence note | PASS | `persistå`, 9 bytes, md5 `a84913316cfd0219701cb2c48a310686` |
| 17 | Warm reboot (from System Status) | PASS | all three store md5s identical |
| 18 | Power off + cold power cycle | PASS | all three md5s identical; `EXT4-fs: recovery complete` on both partitions |
| 19 | Regression: Timber, Fleet, Radar, System | PASS | operator: all four normal; remote: each logged open and close, 0 ERROR |

Throughout: **0 ERROR lines, 0 crash reports, no crashloop markers**, and
`sysd`, `radiod` and `pocketos-shell` running with 0 restarts.

## Storage, measured

```
/var/lib/pocketos/notes/            drwxr-xr-x  root:root   0755
/var/lib/pocketos/notes/note-00000001.txt  -rw-r--r--  root:root   0644
```

| | |
|---|---|
| Path | `/var/lib/pocketos/notes/note-%08u.txt`, as designed |
| Permissions | **0755 directory, 0644 file**, matching Fleet and Radar exactly |
| Stray `.tmp` | **0**, at every check |
| Encoding | æ = `c3 a6`, ø = `c3 b8`, å = `c3 a5`; line break `0a` |
| Id reuse | deleting note 1 freed the id; the next note took it |
| Free space | 130 MB on `/` after the smoke |

The atomic write is proven on hardware rather than inferred. After the cold
power cut the kernel logged `EXT4-fs (mmcblk1p2): recovery complete` — the
journal *did* replay — and the note came back byte-identical with no
temporary left behind.

## DEV-1: are 52 px keys usable?

This smoke is the first real evidence for the approved deviation, whose
justification rests on a mis-key being immediately visible and correctable.

**It holds, and the qualification matters.** The operator typed `bench test`,
`ABC`, `æøå`, `second line ok` and `persistå` with a thumb and produced the
intended text every time — but mis-keys happened and were corrected, at
roughly **one per ten characters** during the Shift sequence. Nothing was
lost, Backspace covered every case, and no irreversible action is bound to a
reduced-size key.

So DEV-1's conditions are met on hardware. What this does not say is that
52 px is *comfortable*: a one-in-ten mis-key rate is workable for a note and
would be poor for a password field. Any future screen that puts an
irreversible action behind typed input should re-read this number first.

## Memory

| Point | VmRSS |
|---|---|
| Launcher, before Notes (boot-started shell) | 11,776 kB |
| Notes list open | 11,904 kB |
| **Editor + full keyboard open** | **11,904 kB** |
| List with one note | 11,904 kB |
| After the whole smoke, fresh boot | 12,544 kB |

Building the 31-key keyboard and the editor cost **nothing measurable** —
the same 11,904 kB with the sheet up as with it down. The figures across
boots (11,776 / 12,032 / 12,544) are different processes with different
histories and are not a trend; no threshold is asserted.

## Open, and not blocking

- **A doubled capital in the first note.** The stored text read
  `bench testAABC…` where the operator had reported `ABC`. Investigated: the
  operator confirmed typing an extra `A`. Not a Shift defect — a one-shot
  Shift is consumed by the first character, so a second tap of the same key
  would have produced a lower-case `a`. Recorded because the discrepancy was
  found by reading the bytes rather than by watching the glass, which is the
  reason to read the bytes.
- **Deployed, not flashed.** Nothing here depends on that, and the
  permissions question that made it matter for Timber was settled by
  rebooting first. A flashed card would still be the stronger artefact for a
  release gate.

## Not covered here

Notes at its limits (64 notes, the 4096-byte cap), an unreadable note on a
real card, the keyboard under Outdoor mode's 20 px body, and the physical
keyboard, which does not exist yet.
