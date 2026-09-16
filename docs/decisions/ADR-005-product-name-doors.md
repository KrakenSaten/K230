# ADR-005: Product name Doors

Status: Accepted for Phases 1 and 2 (product owner, 2026-09-15). Phase 2 is
implemented as recorded under "Phase 2 as implemented"; its hardware gate
passed on unit A on 2026-09-15 (docs/hardware/DOORS_PHASE2_GATE.md). The
graphics integration's hardware gate also passed on unit A on 2026-09-15
(docs/hardware/DOORS_GRAPHICS_GATE.md). Both are merged to master.
**Phase 3 is implemented on `rebrand/doors-3-shell` and recorded under
"Phase 3 as implemented"; its hardware gate
(docs/hardware/DOORS_PHASE3_GATE.md) has not run and it is not merged.**
Phase 4 has not started and needs the product owner's explicit approval
before it does.
Date: 2026-09-15
Deciders: product owner (final), AI engineering partner (author)

## Context

The platform was called PocketOS from 2026-09-04 through v0.0.9. The product
owner has approved a new name, **Doors**, with the brand direction
"Threshold": doorway, portal, opening, transition. A finished graphics
package exists; it is integrated in a separate session, not by this record.

The old name lives in two very different kinds of text:

- **what a user reads**: the status bar wordmark, the System app's identity
  row and dialogs, crash reports, the documentation;
- **what other code, scripts, stored data and operators depend on**: C
  prefixes, file-system paths, the `pos` CLI, service and init-script names,
  environment variables, the Buildroot package. On origin/master `a2e10d1`
  (third_party excluded) `pocketos` appears in 165 files, `POCKETOS` in 156,
  `pos_` in 115, `POS_` in 79 and `pocketui` in 77.

Almost none of the second kind is ever seen by a user, and some of it
carries data. Facts that constrain any rename:

- `/var/lib/pocketos` is on the persistent root filesystem and holds Wi-Fi
  credentials (ADR-003), notes, game saves and logs. A `deploy.sh` update
  keeps it; a reflash replaces it (DOCUMENTED, platforms/k230/README.md and
  the init scripts).
- `/run/pocketos` is a tmpfs (VERIFIED, docs/hardware/V0.0.7_BLOCK2A_SMOKE.md)
  but it is where every binary, test and tool finds the IPC sockets.
- `deploy.sh` unpacks a tar over `/` and never deletes. A renamed init script
  would leave the old one beside it, and two shells would compete for DRM
  master (DOCUMENTED, platforms/k230/scripts/deploy.sh).
- `/etc/default/pocketos-shell` is written by hand on each unit and holds the
  panel-ownership switch; a shell service that stopped reading it would leave
  the panel dark (DOCUMENTED, platforms/k230/README.md).
- Three Pocket Games branches are open and use the internal identifiers
  (`$POCKETOS_STATE_DIR`, the app API).
- The boot splash is the vendor's `logo.xrgb`, not PocketOS artwork
  (VERIFIED, docs/hardware/BRINGUP_SESSION_2026-09-07.md).

## Options

### A. Rename everything in one change

- Con: the largest possible change, for identifiers nobody sees; conflicts
  with every open branch; needs data migration and compatibility aliases on
  day one.

### B. Rename what users see; keep internal names; migrate the rest in stages (chosen)

- Pro: small reviewable blocks. The risky parts (paths, services) each get
  their own migration, validation and hardware test. Code keeps its history
  and stays greppable against the evidence sheets that cite it.
- Con: for a while the source carries two names: "Doors" in text, `pocket*`
  and `pos` in identifiers. This record is what explains the split.

### C. Wait

- Con: every milestone adds more PocketOS text and documentation, and the
  compatibility work only grows once units are in other people's hands.

## Decision

1. **Doors is the product and operating-system name.** It is written "Doors"
   in prose and UI text. The status bar wordmark is set in the caption style,
   in capitals, as "DOORS"; DS §9 is updated in place to say so. Wordmark
   artwork, the compact mark and the boot splash come from the graphics
   package, in their own session.
2. **"Pocket" never names the platform again.** It may still name a
   collection, a library or the form factor.
3. **Brand hierarchy:**

   | Name | Role |
   | --- | --- |
   | Doors | the product and operating system |
   | Doors Design System | the design system's name in prose; its file stays `docs/design/POCKETOS-DS-v0.1.md` |
   | PocketUI | the internal UI toolkit (`ui/pocketui`); keeps its name |
   | Pocket Games | the game collection; a sub-brand under Doors |
   | PocketLink | roadmap working title; named when work on it starts |
   | Fleet, Radar, Timber, Notes, Clock, Calendar, Calculator, Wave, Radio, Settings, System | app names, without a prefix (the launcher already shows them this way) |

4. **Internal identifiers stay stable for now.** Renaming them is not part of
   the rebrand:
   - C prefixes and symbols: `pos_*`, `POS_*`, `pocketui_*`, `POCKETUI_*`,
     `pocketipc_*`, `pocketlog_*`, `pocketaudio_*`, `pocketos_*` (paths, the
     shell API, `struct pocketos_app`, `POCKETOS_APP_API_VERSION`), header
     guards;
   - source directories: `ui/pocketui`, `core/pocketipc`, `core/pocketlog`,
     `core/pocketaudio`, `tools/pos`;
   - IPC socket names and method namespaces (they carry no product name);
   - build-host and test variables (`POCKETOS_ALLOW_DIRTY_BUILD`,
     `POCKETOS_ALLOW_PIN_DRIFT`, `POCKETOS_VENDOR_DIR`, `POCKETOS_OUT_DIR`,
     `POCKETOS_TOOLCHAIN_CC`, test hooks).

   On-disk format markers (`PFS1`, `PRR1`, `PTR1`, `pocketclock 1`) are file
   formats, not branding, and are never renamed.
5. **Compatibility-sensitive names are deferred**, each to a phase with its
   own migration and validation:
   - `/etc/pocketos-release`, the `pos` CLI, `/usr/share/pocketos` (Phase 2);
   - `pocketos-shell`, `S90pocketos-shell`, `/etc/default/pocketos-shell` and
     the supervisor name (Phase 3);
   - `/var/lib/pocketos`, `/etc/pocketos`, `/run/pocketos` and the
     `POCKETOS_*` variables the binaries read (Phase 4, decided together with
     the data-partition work, if at all);
   - the Buildroot package `pocketos` and `k230_pocketos_defconfig` (Phase 4,
     on a fresh SDK tree).

   Rules for those phases, fixed now: `pos` stays a permanent alias of
   `doors`; `/etc/pocketos-release` stays a symlink at least until v0.1.0 and
   readers fall back to it; state moves by an atomic rename on the same
   filesystem with the old path left as a symlink, never by copy or merge; a
   renamed shell service falls back to the old `/etc/default` file; `deploy.sh`
   deletes the files a rename replaces.
6. **History is not rewritten.** `docs/hardware/`, release and RC sheets,
   tags v0.0.1 to v0.0.9, commit messages and past BUILD_INFO files keep the
   name they were written under. ADR-001 to ADR-004 are amended when needed,
   never rewritten. Document files that other documents cite by path keep
   their names (`POCKETOS-DS-v0.1.md`, `POCKETFLEET.md` and the rest).
7. **Copyright headers** ("PocketOS authors") are unchanged. They belong to
   the licence decision (docs/LICENSING.md), not to the rebrand.
8. **The first Doors release is v0.0.10.** Its scope is Phases 1 and 2 and
   the graphics integration. Phase 3 joins only if its hardware test passes;
   otherwise it moves to the next version. Phase 1 does not change VERSION.

## Staged migration

| Phase | Scope | Validation | Hardware |
| --- | --- | --- | --- |
| 1. Visible branding | this record; the status bar wordmark, System identity row, Restart and Power off dialog titles, crash report header and simulator window title; current prose in README.md, AGENTS.md, docs/ARCHITECTURE.md, docs/ROADMAP.md and platforms/k230/README.md; DS §9 wordmark text | host tests, shell tests, riscv64 and DRM builds, simulator screenshot of System | not required |
| 2. Release and build identity, CLI | `/etc/doors-release` with `/etc/pocketos-release` as a symlink; `doors` with `pos` as a symlink; `/usr/share/doors`; notices wording regenerated with `pocketos.hash` (the owner approves the legal wording; no licence is added); BUILD_INFO.txt and a release image named `doors-<version>[-rcN]-tdisplay-k230-<build_id>.img.gz`; remaining text (settings and Wi-Fi file headers, CMake status line, LoRa test payload) | the above, plus Buildroot legal-info, image build, `verify_image.sh` | yes: flash and boot a unit |
| Graphics integration | wordmark and compact mark, boot splash (`logo.xrgb`, 568 × 1232 XRGB8888), System and launcher branding, with a DS amendment | image build, screenshots | yes: splash on glass |
| 3. Shell service | `doors-shell` and `S90doors-shell`, falling back to `/etc/default/pocketos-shell`; `DOORS_*` spellings for the operator display and touch overrides, mapped in the init script; supervisor name; `deploy.sh` removes replaced files | init-script, supervisor and sysd tests | yes, mandatory: fresh flash, deploy over a PocketOS-era unit, two reboots, rollback |
| 4. Optional internal cleanup | state, config and runtime directories with migration and an ADR-003 amendment; `DOORS_*_DIR`; `pos-*` helper names; Buildroot package and defconfig | full suite and image | yes, with real data on a unit |

## Phase 2 as implemented

Branch `rebrand/doors-2-identity`, from master `8070379`; merged to master
(`2caba9e`) and accepted by the product owner on 2026-09-15. VERSION stays
0.0.9; v0.0.10 is not tagged by this phase.

### Compatibility model

| PocketOS-era name | Doors name | What is on disk | Who reads it, and how |
| --- | --- | --- | --- |
| `/etc/pocketos-release` | `/etc/doors-release` | one regular file, `/etc/doors-release`; the old name is a same-directory symlink to it, so the two cannot disagree | `pocketos_release_read()` (`core/pocketpaths.c`), used by sysd's `system.info` and by `doors`/`pos system info`, and the same rule in `pos-hwcheck`: the new name when it exists, the old name only when the new one is absent (ENOENT, which a dangling link also gives). A new file that exists but cannot be read is an error, never a reason to report the old one. Format unchanged: the bare version on line 1, then `BUILD_ID=` |
| `/usr/bin/pos` | `/usr/bin/doors` | one binary, installed as `doors`; `pos` is a same-directory symlink to it | the binary reads its own name (`tools/pos/pos_cli.h`). As `doors`: every usage line and message says doors, `doors version` prints `Doors <version> (build <id>)`, `doors system info` labels the release line `doors`. As `pos`, or any other name: output byte-identical to v0.0.9 (`pos <version> (build <id>)`, the `pocketos` label, the same usage text) |
| `/usr/share/pocketos/THIRD_PARTY_NOTICES.txt` | `/usr/share/doors/THIRD_PARTY_NOTICES.txt` | the file under the new directory; `/usr/share/pocketos` stays a directory holding a symlink `../doors/THIRD_PARTY_NOTICES.txt` | nothing at runtime; the directory held nothing else and nothing stateful, which is why it could move |
| `sysimage-sdcard.img` (vendor build output) | `doors-<version>[-rcN]-tdisplay-k230-<build_id>.img.gz` and `.sha256` | both exported; the artefact is `gzip -n` of the verified image, read back before export; `-rcN` from `POCKETOS_RELEASE_RC` | flashing either writes the same bytes |

Unchanged on purpose: `pos-supervise`, `pos-hwcheck`, `pos-spixfer`,
`pos-wave`, `tools/pos` and its build output `tools/pos/pos`, every C
identifier in decision 4, PocketUI, `pocketos-shell` and every service and
init-script name, `/var/lib/pocketos`, `/run/pocketos`, `/etc/pocketos`, the
`POCKETOS_*` variables, the applied-manifest keys, the Buildroot package and
defconfig names, and the "PocketOS authors" copyright lines (decision 7).

Also in this phase: BUILD_INFO.txt says "Doors <version> image" and labels
the source line "Doors"; THIRD_PARTY_NOTICES.txt opens with the unchanged
no-licence statement under the Doors name, naming the former name and the
copyright lines (regenerated with `pocketos.hash`); `POCKETOS_LICENSE` reads
"Not yet decided (Doors; no licence granted)"; the CMake status line,
apply and deploy messages, the settings and Wi-Fi store headers, the Buildroot
package prompt, and the Radio app and pair-test LoRa payloads ("Doors test 01\n"
and "Doors 01", each the length of the payload it replaced, so airtime is
unchanged).

### Upgrade and rollback

- **Fresh flash**: the layout above, from `make install` in the image build.
- **`deploy.sh` onto a PocketOS-era unit**: the archive carries the links as
  links. BusyBox tar 1.37.0, the one in the image, unlinks an existing
  non-directory before extracting an entry, so the unit's regular
  `/usr/bin/pos`, `/etc/pocketos-release` and old notices file become the links
  (VERIFIED on unit A, 2026-09-15, over a PocketOS-era card, and idempotent on a
  second deploy; source: `archival/libarchive/data_extract_all.c`, and links
  whose target contains `..` are created after every other entry,
  `unsafe_symlink_target.c`). It cannot unlink a directory, so no directory is
  turned into a link; `/usr/share/pocketos` stayed a directory on the device.
- **Rolling a Doors unit back with a PocketOS-era deploy** (an older checkout's
  `deploy.sh`): its archive writes regular files over the three links and, as
  always, deletes nothing, so `/usr/bin/doors`, `/etc/doors-release` and
  `/usr/share/doors` stay behind with the newer content. The older binaries do
  not read them, but `doors` would still run the newer binary and a later
  single-binary bench deploy would read the stale `/etc/doors-release`. After
  such a rollback: `rm -rf /usr/bin/doors /etc/doors-release /usr/share/doors`.
  VERIFIED on unit A (gate B6): regular files extracted over the three links
  replace them, and after the `rm -rf` the unit is byte-identical to its
  PocketOS-era files and reports its old build.
- **An SDK tree shared with older branches**: `apply_to_sdk.sh` never deletes
  from Buildroot's target tree either. A PocketOS-era source built in an SDK
  that has had this phase installed carries the same three stale paths into its
  image (and its `printf > /etc/pocketos-release` writes through the link).
  Remove `usr/bin/doors`, `etc/doors-release`, `etc/pocketos-release` and
  `usr/share/doors` from `output/k230_pocketos_defconfig/target` before such a
  build.

### Hardware test (required for acceptance, per the table above)

On unit A: flash the Phase 2 image and boot it; `doors version` and
`pos version`; `cat /etc/doors-release`; `ls -l /usr/bin/pos
/etc/pocketos-release /usr/share/pocketos/`; `doors system info` and `pos
system info` (release line and label); `doors call sysd system.info`
(`release_file`, `release_build`); the release section of `pos-hwcheck`; a
reboot and the same again. Then `deploy.sh` from this branch onto a card
running the PocketOS-era image, and the same checks, to confirm the BusyBox tar
behaviour above on the device. The bench checklist and the host validation
record are in docs/hardware/DOORS_PHASE2_GATE.md. **Passed on unit A,
2026-09-15**, with a second deploy and the rollback as well; the results are in
the same sheet.

## Phase 3 as implemented

Implemented on `rebrand/doors-3-shell` from master `15b1b7e`, 2026-09-16.
VERSION stays 0.0.9 and no internal identifier is renamed: `POCKETOS_*`
variables, PocketUI, pocketipc, pocketlog, pocketaudio, `/var/lib/pocketos`,
`/run/pocketos` and `/etc/pocketos` are all untouched (Phase 4, if ever).

Renamed, and nothing else:

| Was | Is |
| --- | --- |
| `/usr/bin/pocketos-shell` | `/usr/bin/doors-shell` |
| `/etc/init.d/S90pocketos-shell` | `/etc/init.d/S90doors-shell` |
| supervised as `pocketos-shell` | supervised as `doors-shell` |
| `/var/run/pocketos-shell-supervise.pid` | `/var/run/doors-shell-supervise.pid` |
| `/run/pocketos/pocketos-shell.{pid,state,crashloop}` | `/run/pocketos/doors-shell.{pid,state,crashloop}` |
| `supervise-pocketos-shell.log` | `supervise-doors-shell.log` |
| `/etc/default/pocketos-shell` | `/etc/default/doors-shell`, with the old path still read when the new one is absent |

`shell.sock`, `shell.log`, `shell.stdio.log` and the crash reports keep their
names: they are the shell's, not the service's, and renaming them would move
files a reader of every earlier bench sheet knows by name.

The CMake target and the build artefact are still called `pocketos-shell`;
the installed path is what carries the identity. That is the same rule as
decision 2 - internal names stay until a phase says otherwise - and it keeps
the shell tests, which take the binary through `SHELL_BIN`, unchanged.

**One shell, always.** There is no `pocketos-shell` symlink: a compatibility
name here would be a second way to start a second DRM owner. Instead the new
service refuses to start when a PocketOS-era shell is running, or when its init
script is installed and enabled and would start beside it at the next boot;
`deploy.sh` removes the old init script before the old binary and then proves
exactly one shell, one init script and one supervisor state exist; the build
refuses a target tree or an image that carries both.

**Settings.** `/etc/default/doors-shell` is the file. A PocketOS-era
`/etc/default/pocketos-shell` is read only when the new one does not exist, as
a whole file, never merged with it - and the service says which file it read
and when it ignored the other, so a hand-edited old file is never dropped in
silence. `POCKETOS_*` spellings inside those files are unchanged; the
`DOORS_*` aliases sketched in the migration table above are not part of this
phase.

**Rollback.** `platforms/k230/scripts/rollback_phase3.sh` takes the Doors
identity off a unit - stop, prove nothing is running, carry the settings back
to the old name when only the new one exists, remove init script, then binary,
then runtime state - and leaves the unit with no shell service, which is the
only state a pre-Phase-3 `deploy.sh` can install exactly one into.

Hardware gate: docs/hardware/DOORS_PHASE3_GATE.md. **Not run.** Not merged.

## Consequences

- From Phase 1 the device says Doors on screen. From Phase 2 the release file,
  the CLI (`doors`), the notices and the image artefact carry the Doors name,
  while `pos` and the old paths keep answering. The System Services row
  (`pocketos-shell`) still says PocketOS until Phase 3.
- Developers read "Doors" in prose and `pocket*` or `pos` in code; decisions
  2 and 4 are the explanation. A search for "doors" also finds the Wave test
  payload `DOORS` and comments about "the app's only door to the
  filesystem"; neither is the product name.
- The open Pocket Games branches merge as before: they use identifiers this
  record keeps.
- Evidence recorded under the PocketOS name stays valid; the name does not
  change what was measured.

Migration: none in Phase 1. No path, file format, service, command or
environment variable changes, and units need no action.

## Evidence

- Reference counts: `git grep` on origin/master `a2e10d1`, `third_party/`
  excluded.
- `/run/pocketos` is a tmpfs: VERIFIED (docs/hardware/V0.0.7_BLOCK2A_SMOKE.md,
  cited in tools/supervise/pos-supervise).
- Boot splash loaded by U-Boot from the boot partition as `/logo.xrgb`:
  VERIFIED (docs/hardware/BRINGUP_SESSION_2026-09-07.md); it must be exactly
  568 × 1232 XRGB8888 or U-Boot skips it: DOCUMENTED (vendor U-Boot overlay,
  `board/canaan/common/logo/k230_logo.c`).
- `deploy.sh` extracts without deleting; `/etc/default/pocketos-shell` is
  written by hand: DOCUMENTED (platforms/k230/scripts/deploy.sh,
  platforms/k230/README.md).
