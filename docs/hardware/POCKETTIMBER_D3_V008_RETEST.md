# PocketTimber D3: focused retest on the v0.0.8 base

PocketTimber has been rebased from its v0.0.5 branch point onto v0.0.7
master (`6561b50`) as milestone M0 of v0.0.8. This sheet is the hardware
gate that stands between that rebase and M1, the merge to master.

**No transmission at any step; radiod stays on the mock backend.** Commands
run as root on serial.

| | |
| --- | --- |
| Candidate | `pockettimber-v0.0.8-candidate` |
| Source | `aa7137b` on `pockettimber-engine` (clean) |
| Base | v0.0.7 master `6561b5024ff8006d5bdaefda813ca86653409fb4` |
| Vendor BSP / SDK | `bb831ab358b66f5bd9a87ecd7c580fee4537492e` / `22d02c6b6783a57a3aca7eb3160e313e772cb710` (pinned, enforced by `apply_to_sdk.sh`) |
| Build UTC | `2026-09-10T17:47:30Z`, defconfig `k230_pocketos_defconfig` |
| Raw image | `out/k230-v0.0.8-timber-candidate/sysimage-sdcard.img`, 763,363,328 bytes, SHA-256 `3b0aae7d4604962fbbb2372c406d466f056817fc787e14e305a7d890d144bb06` |
| Compressed | `sysimage-sdcard.img.gz`, 204,443,358 bytes, SHA-256 `01f5ff70424c5b15b37d8d94adb468894810e9b9a112ce36f073273539077ce4` |
| Identity on the device | `/etc/pocketos-release` → `0.0.8` with `BUILD_ID=aa7137b`; `pos version` → `pos 0.0.8 (build aa7137b)` |

Offline verification, before the card exists: `/etc/pocketos-release` reads
`0.0.8` / `aa7137b`; `S50sysd`, `S60radiod` and `S90pocketos-shell` are all
0755 in the rootfs; `pos`, `pocketos-shell`, `radiod`, `sysd`,
`pos-supervise`, `pos-hwcheck` and `pos-spixfer` are all present; the shell
names five apps (`fleet`, `radar`, `radio`, `system`, `timber`); the shell
was linked from all fifteen Timber engine, app and UI objects and from the
four converted sprites, so the image carries the art rather than the
placeholder blocks; the PocketOS sources contributed no compiler warnings.

## What D3 has already established, and where that evidence lives

D3 ran on candidate 2 (runtime `0.0.5`, build `e3d5be0`) on 2026-09-09 and
got most of the way. On the owner's pause checkpoint the following had
passed, with 0 ERROR and 0 crash, and no blocking PocketTimber defect
known: the input and pull track, the seat and push-back, the viewport with
nothing to scroll, collapse and the result screen, `record.v1` creation and
format, BEST update and persistence, warm reboot, cold power cycle, the
corrupt-record cases (12- and 20-byte), storage path and permissions, and
an `S90` service restart.

Both candidate sheets were blank templates in this repository until
2026-09-10, when their verdict tables were reconstructed from the session
record so the merge carries its own evidence. Each reconstructed table says
so at the top, names the commits that corroborate it, and marks values that
were never separately observed rather than inventing them. **They are
reconstructions, not bench transcripts, and the operator should correct
anything that disagrees with their recollection.**

Three D3 items were still open at the pause: **repeated runs with VmRSS
watched**, the **final regression smoke**, and the **summit attempt**.

## What this sheet asks for

Timber's own sources came through the rebase byte-identical to the
pre-rebase tag `pockettimber-prerebase-v0.0.8`, so the game is not the
thing that changed. The platform under it is: v0.0.6 and v0.0.7 landed
since candidate 2 was built, bringing `sysd` as a fourth process, the
System Status screen, the supervisor state file, and `system.reboot` /
`system.poweroff` driven through init. So this is a short sheet: the three
items D3 never finished, plus the two survival cases whose **mechanism**
has changed underneath a passing result.

**Operator note, from the candidate 1 and 2 sessions: the block is never
dragged. Pulls happen on the dedicated track below the table.** Two bench
sessions were lost to this before it was understood.

### 1. Launch and identity on the new base

- [ ] `pos version` → `pos 0.0.8 (build aa7137b)`;
      `cat /etc/pocketos-release` → `0.0.8` and `BUILD_ID=aa7137b`.
- [ ] Launcher shows **five** tiles. Timber opens: standby, the rendered
      sprites, `BEST 0` on a fresh card, the bottom row inside the panel.
- [ ] One short run to a collapse, to confirm nothing about play changed on
      the new base: `record.v1` appears, 52 bytes, BEST updates.

### 2. Repeated runs, with memory watched (D3 item 12, never done)

- [ ] Five runs back to back without leaving the app. Before the first and
      after the fifth: `grep VmRSS /proc/$(pidof pocketos-shell)/status`.
      Record both. A steadily climbing RSS across five runs is a finding.
- [ ] The runs counter at offset 16 of `record.v1` advances by five:
      `hexdump -C /var/lib/pocketos/timber/record.v1`.
- [ ] No crash, no `/run/pocketos/*.crashloop`, `grep -c ERROR
      /var/lib/pocketos/log/shell.log` → 0.

### 3. Record survival across the v0.0.7 power paths (mechanism changed)

Candidate 2 proved survival across a warm reboot and a cold power cycle,
but it did so on a v0.0.5 image, where neither `system.reboot` nor
`system.poweroff` existed. Both are new code between the game and the
filesystem, so both are re-run here.

- [ ] `md5sum /var/lib/pocketos/timber/record.v1`; note it.
- [ ] **Restart from the System Status screen.** After the boot: Timber in
      the launcher, BEST unchanged, `md5sum` identical.
- [ ] **Power off from the System Status screen** (operator-attended, as in
      `V0.0.7_BLOCK2C_SMOKE.md`). Leave the unit disconnected for about a
      minute before reconnecting — reconnecting within seconds leaves it
      off, which is PMIC behaviour and not a fault. After the boot: BEST
      unchanged, `md5sum` identical.

### 4. Summit attempt (D3 item 6, never done)

The summit is a completed top layer at 36 layers. Two facts bound this:
the spec states it "is not reached in practice" once the D3 shift lean is
in (`docs/apps/POCKETTIMBER.md`), and `$POCKETTIMBER_SCREEN` has no summit
state, so the image carries no way to open the app near one.

- [ ] One careful run aiming at it: pull from alternate sides so the lean
      cancels, place against the lean, let the sway settle before each
      pull. Record the layers reached and the cause.
- [ ] If reached: RESULT title `STILL STANDING`; the summits counter at
      offset 28 of `record.v1` reads `01 00 00 00`.
- [ ] If not reached in two attempts: record **NOT REACHED** with layers
      and cause. This is not a failure of the build. The owner then decides
      between letting `timber_replay_test` and `timber_rules_test` stand
      for the summit — both reach it from a constructed tower, and both
      pass on this commit — and adding a bench hook, which is code and so
      belongs after M0, not inside it.

### 5. Regression smoke (D3 item 13, never done; extended for v0.0.7)

- [ ] Fleet: play one turn, leave, reopen → Resume offers it.
- [ ] Radar: one run to completion; leave, reopen → BEST shown.
- [ ] Radio: opens, the chip row shows the mock backend, rows populate;
      `pos radio info` answers.
- [ ] `ls -l /var/lib/pocketos/fleet/save.v1 /var/lib/pocketos/radar/record.v1 /var/lib/pocketos/timber/record.v1`
      → all three present.
- [ ] `pos system info` and `pos system status` answer; `release_build`
      matches `pos version`.
- [ ] System Status opens: vitals, storage, network, services, radio and
      identity populate.
- [ ] Services healthy from the supervisor state file: `sysd`, `radiod`
      and `pocketos-shell` all `running=true`.
- [ ] `ls /run/pocketos/*.crashloop` → none.

## Flash and boot

Flash with the generic tool, which identifies the card by hardware, not by
what is on it:

```powershell
powershell -ExecutionPolicy Bypass -File C:\K230\tools\flash-devcard.ps1 -Image C:\K230-wt\timber\out\k230-v0.0.8-timber-candidate\sysimage-sdcard.img -Sha256 3b0aae7d4604962fbbb2372c406d466f056817fc787e14e305a7d890d144bb06
```

Only on `RESULT: FLASH PASS`. Boot with the serial console open. On a fresh
card the panel must be handed over once (`ENABLE=0` in
`/etc/default/k230_phone_ui`, `ENABLE=1` in `/etc/default/pocketos-shell`,
`reboot`): `S90pocketos-shell` ships disabled by default and refuses to
start while the vendor launcher owns the panel.

The image has no `stat`, `timeout` or `hd` applets; use `ls -l`, `md5sum`
and `hexdump -C`.

## STOP conditions

Stop at the first of: a button that does not answer when it is drawn
enabled; a block part way out that no drag on the track moves; a block
pushed back that leaves the turn locked; a tap or drag that scrolls the
screen instead of acting; anything clipped at the bottom row; a record file
that does not survive a restart or a power off; a crash
(`/var/lib/pocketos/log/crash-shell-*`, the launcher appearing by itself,
`/run/pocketos/*.crashloop`). Collect `pos logs shell -n 50` before
touching anything else.

## Verdict

### How the candidate got onto unit A (2026-09-10)

**Deployed, not flashed.** No card reader was attached, so the candidate went
on with `platforms/k230/scripts/deploy.sh 192.168.10.157` from the M0
Buildroot target tree, which carries the same seven binaries, the three init
scripts and `/etc/pocketos-release` that the flashed image carries. The card
in the unit was the v0.0.7 release card; its PocketOS binaries were backed up
on the board first:

```
/root/v007-backup/pocketos-v0.0.7.tar   777,216 bytes, 11 entries
md5 4890a642c8f9e80bde05d1eaa175ca7b
rollback: tar -C / -xf /root/v007-backup/pocketos-v0.0.7.tar && reboot
```

The deployed shell is byte-identical in size to the built one (801,208 B,
md5 `3aa7cc950f5d38a34d7ad6310fe90cdf`). **If any result below looks like an
install artefact rather than a Timber behaviour, flash the raw image and
repeat before believing it.**

### Part A — completed 2026-09-10, no touch required

| Section | Item | Verdict | Observed |
| --- | --- | --- | --- |
| 1 | Identity | **PASS** | `/etc/pocketos-release` → `0.0.8`, `BUILD_ID=aa7137b`; `pos version` → `pos 0.0.8 (build aa7137b)` |
| 1 | Shell live on the panel | **PASS** | DRM 568×1232, `current: home`, theme ice / normal, "input device attached (touch)" |
| 1 | Five apps registered | **PASS** | `pos shell info` lists radio, system, fleet, radar, **timber** |
| 5 | `pos system info` / `status` | **PASS** | both answer; `pocketos 0.0.8 (build aa7137b)`, kernel 6.6.36 riscv64, temp 49.2 °C |
| 5 | Supervisor health | **PASS** | sysd 1059, radiod 1077, pocketos-shell 1099 — all `running: true`, `crashloop: false`, `restarts: 0` |
| 5 | Crash and crashloop markers | **PASS** | no `/run/pocketos/*.crashloop`, 0 crash reports, 0 ERROR lines in `shell.log` |
| 2 | Shell VmRSS baseline | **recorded** | **12,288 kB** RSS (VmSize 46,316 kB) at the launcher, before any Timber run |

Free space at the time: 136,540,160 bytes on `/`. No Timber state yet
(`/var/lib/pocketos/timber` absent, as expected on a card that has never run
the app).

### Part B — run by the operator at the panel, 2026-09-10

Every item here needs a finger on the panel and was performed by the
operator. Synthetic input was deliberately **not** injected at any point:
`tests/timber_input_test.c` already drives the real LVGL evdev parser with
the GT9895 event grammar, and the spec is explicit that what was still
needed is a real finger on the pull track, which no test can stand in for.

**Two kinds of evidence appear below and they are not the same thing.**
*Operator* is what a person saw on the panel. *Remote* is what was measured
over SSH afterwards — the record file decoded byte by byte, and the shell
log, which is append-only across boots. Where both exist they agree.

| Section | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| 1 | Timber launches from the tile | **PASS** | operator; remote: `open app timber` at 18:06:50 |
| 1 | Layout correct, no clipping, bottom row inside the panel | **PASS** | operator |
| 1 | Pull-track behaviour works | **PASS** | operator |
| 1 | Blocks seat and push back correctly | **PASS** | operator |
| 1 | Run to collapse; `record.v1` written; BEST updates | **PASS** | operator; remote: record written 18:09:17, and the shell read back "best score 434 over 1 run(s)" on reopen at 18:07:50 |
| 2 | Repeated runs, no visible issue | **PASS, 2 runs** | operator; remote: `runs` = 2, `collapses` = 2. **The five this sheet asked for were not run** — see the note below |
| 2 | VmRSS across the repeated runs | **NOT MEASURED** | the process that ran them (pid 1099) is gone; see the note below |
| 3 | Record survives `system.reboot` from System Status | **PASS** | operator; remote: System Status opened 18:09:28, "stopping on signal" 18:09:41, and the next boot logged "best score 2208 over 2 run(s)" — the record read back intact |
| 3 | Record survives `system.poweroff` + cold power cycle | **PASS** | operator; remote: System Status opened 18:11:00, "stopping on signal" 18:11:04, and the boot after the cycle again logged "best score 2208 over 2 run(s)" |
| 4 | Summit | **NOT REACHED** | operator; remote: `summits` = 0. Not a failure — see below |
| 5 | Fleet launch and basic interaction | **PASS** | operator; remote: `open app fleet` … `close app fleet`, about 13 s |
| 5 | Radar launch and basic interaction | **PASS** | operator; remote: `open app radar` … `close app radar` 18:10:59 |
| 5 | System Status screen opens and populates | **PASS** | operator; remote: opened before both power actions |
| 5 | Supervisor and service health after the cycle | **PASS** | remote: sysd 235, radiod 253, pocketos-shell 274, all `running: true`, `crashloop: false`, `restarts: 0` |
| — | Storage path, no stale temporary file | **PASS** | remote: `/var/lib/pocketos/timber/record.v1`, nothing else in the directory |
| — | Storage permissions | **re-observe on a flashed card** | remote: 0700/0600, which is the deploy session's umask, not Timber's doing — see below |
| — | No crash, no crashloop, no errors | **PASS** | remote: 0 crash reports, 0 ERROR lines in `shell.log`, no `/run/pocketos/*.crashloop`, both stops logged as "stopping on signal" |

### The record file, decoded

`/var/lib/pocketos/timber/record.v1`, 52 bytes, md5
`735c1dff6f30254ce8a75f967e157265`, written 2026-09-10T18:09:17Z, no stray
temporary file beside it. Decoded against the field table in
`docs/apps/POCKETTIMBER.md`:

| Offset | Field | Value |
| --- | --- | --- |
| 0 | magic | `PTR1` |
| 4 | format version | 1 |
| 6 | best score | 2208 |
| 10 | best height | 22 layers |
| 12 | best streak | 0 |
| 14 | best pulls | 10 |
| 16 | **runs** | **2** |
| 20 | lifetime pulls | 12 |
| 24 | **collapses** | **2** |
| 28 | **summits** | **0** |
| 32 | causes: tip, jolt, placement, sway | 1, 1, 0, 0 |
| 48 | FNV-1a checksum | `c1 78 38 91` |

Every run is accounted for: two runs, both ended with the tower down, one by
tip and one by jolt, no summit. The counters, the two "best score N over M
run(s)" log lines and the operator's account all agree.

### Storage path and permissions: the observed modes are a deploy artefact

The path is right and there is no stray temporary file. The **modes are not
representative of a flashed card**, and this is the install artefact this
sheet warned about, so it is written out rather than filed as a defect.

| Store | Created by | Directory | File |
| --- | --- | --- | --- |
| `timber/` | shell pid 1099, started by `deploy.sh` over SSH | `drwx------` 0700 | `-rw-------` 0600 |
| `radar/` | shell pid 273, started by `rcS` at boot | `drwxr-xr-x` 0755 | `-rw-r--r--` 0644 |
| `fleet/` | shell started by `rcS` at boot | `drwxr-xr-x` 0755 | `-rw-r--r--` 0644 |

`apps/timber/timber_store.c` and `apps/radar/radar_store.c` both call
`mkdir(work, 0755)` and `fopen(tmp, "wb")` — identical code, so the
difference is not in the app. It is the umask:

```
shell started by rcS at boot (pid 274)   Umask: 0022
init and sysd                            Umask: 0022
an SSH session, which is what deploy.sh ran under   umask 0077
```

Confirmed directly: a directory and a file created in an SSH session on this
board come out `drwx------` and `-rw-------`, the exact modes Timber's store
has. `0755 & ~0077 = 0700`, `0666 & ~0077 = 0600`.

Radar's store is the control: identical store code, created by a
boot-started shell on this same build, and it is 0755/0644. On a flashed
card Timber's store would be too. The reconstructed candidate 2 sheet
records 0644 from a flashed card, which agrees.

**Item 11 is therefore PASS on path and on the absence of a stale temporary
file, and its permissions must be re-observed on a flashed card** — or after
letting a boot-started shell recreate the store — before anyone quotes a
mode for Timber. Nothing here calls for a code change.

### Two items that are short of what this sheet asked for

**Repeated runs: two, not five.** The ROADMAP's D3 wording is "repeated runs
stable"; the number five was this sheet's own choice. Two completed runs
with no crash, no error and a correct counter is evidence of stability, but
it is thinner evidence than five. **Owner's call whether that closes the
item.**

**VmRSS across the runs was not captured.** The baseline in Part A
(12,288 kB) was taken on pid 1099, which is also the process that ran both
runs — but it was never sampled again before the reboot ended it, and RSS
does not survive a process. What can be said is only this:

| Measurement | Process | State | VmRSS |
| --- | --- | --- | --- |
| Before any Timber run | 1099 | launcher | 12,288 kB |
| After the reboot and cold cycle | 274 | Timber open, three samples 3 s apart | 12,416 kB, unchanging |

Those are different processes with different histories, so the 128 kB
between them is the cost of having Timber's screen up, **not** a leak
measurement. No threshold is asserted here and none should be read in.

Closing this properly is a five-minute bench task and needs no new code:
sample `grep VmRSS /proc/$(pidof pocketos-shell)/status` over SSH, then play
five runs without leaving the app, sampling between each. It can be done
whenever the unit is next in front of someone.

### Summit

**NOT REACHED**, and by the owner's ruling of 2026-09-10 that is not a
failure. The spec states the summit is not reached in practice once the D3
shift lean is in, and `$POCKETTIMBER_SCREEN` has no summit state, so the
image carries no way to open near one. No debug hook was added. The summit
logic rests on the deterministic host coverage, which reaches it from a
constructed tower and passes on this commit: `tests/timber_rules_test.c`
(the summit reached, everything refused after) and
`tests/timber_replay_test.c` (a session through the summit replayed to the
same summit).

## Verdict

**D3 PASS** on build `aa7137b`, 2026-09-10, unit A.

Operator: owner (manual panel testing). Remote measurements over SSH from
the development host. No STOP condition was hit at any point.

Three things are carried rather than blocking, none of them a PocketTimber
defect and none needing a code change:

1. The VmRSS observation across repeated runs was not captured.
2. Two completed runs against this sheet's five.
3. Timber's store permissions must be re-observed on a flashed card; what
   this card shows is the deploy session's umask.

The summit is closed as NOT REACHED, with the deterministic host coverage
standing for it by the owner's ruling.

This clears the gate for M1: freeze PocketTimber v1 and merge
`pockettimber-engine` to master.
