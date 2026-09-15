# Doors Phase 2: hardware gate

Branch `rebrand/doors-2-identity` (docs/decisions/ADR-005-product-name-doors.md,
"Phase 2 as implemented").

**Result: PASS on unit A, 2026-09-15.** Parts A (A1 to A9) and B (B1 to B5)
pass, the optional rollback B6 passes, and a second deploy over the first is
idempotent. The BusyBox tar behaviour Phase 2 relies on is VERIFIED on the
device. Acceptance of Phase 2 is the product owner's decision.

Not merged, not tagged. VERSION is still 0.0.9. Phase 3 not started.

## Before the bench

1. Use the candidate image below, or build again from the branch tip
   (`apply_to_sdk.sh`, `build_image.sh`; the build runs `verify_image.sh` and
   exports `sysimage-sdcard.img` and `doors-0.0.9-tdisplay-k230-<id>.img.gz`).
2. `sha256sum -c doors-*.img.gz.sha256` in the export directory.
3. Flash with the bench tool. The artefact decompresses to exactly
   `sysimage-sdcard.img`; either writes the same bytes.
4. Before building an older branch in the same SDK, re-apply that branch: the
   applied manifest from this build has been set aside so nothing builds
   without it (KNOWN_ISSUES, "Build environment").

## Part A: fresh flash

Record every result as VERIFIED (operator) or FAIL.

| # | Check | Pass | Tells you, if it fails |
| --- | --- | --- | --- |
| A1 | Boot | the shell (or the vendor launcher, per `/etc/default`) comes up as on v0.0.9; `doors logs --crashes` lists nothing new | not a naming problem; compare with the v0.0.9 image |
| A2 | CLI names | `doors version` prints `Doors 0.0.9 (build <id>)`; `pos version` prints `pos 0.0.9 (build <id>)`, same id | argv[0] handling on the device |
| A3 | Links | `ls -l /usr/bin/pos /etc/pocketos-release /usr/share/pocketos/THIRD_PARTY_NOTICES.txt` shows three symlinks, to `doors`, `doors-release` and `../doors/THIRD_PARTY_NOTICES.txt`; `ls -ld /usr/share/pocketos` is a directory | the rootfs assembly changed the links |
| A4 | Release file | `cat /etc/doors-release` and `cat /etc/pocketos-release` print the same two lines, `0.0.9` and `BUILD_ID=<id>` | |
| A5 | system info | `doors system info` has a `doors` line `0.0.9 (build <id>)`; `pos system info` has the same under `pocketos` | reader or label |
| A6 | sysd | `doors call sysd system.info`: `release_file` is `0.0.9` and `release_build` is `<id>`, equal to `version` and `build` | reader in sysd |
| A7 | hwcheck | `pos-hwcheck /tmp`: the report's `--- release: cat /etc/doors-release` section holds the two lines | hwcheck fallback |
| A8 | Old commands | `pos app list`, `pos radio info`, `pos wifi status`, `pos logs` answer as before; the same with `doors` | |
| A9 | Reboot | reboot; A2 to A6 again; no crash report, no crash loop | |

## Part B: deploy over a PocketOS-era unit

On a card running the v0.0.9 image, or any PocketOS-era image with SSH:

| # | Check | Pass | Tells you, if it fails |
| --- | --- | --- | --- |
| B1 | Before | `ls -l /usr/bin/pos /etc/pocketos-release /usr/share/pocketos/`: regular files; `/usr/bin/doors` absent | |
| B2 | Deploy | `platforms/k230/scripts/deploy.sh <ip>` from this branch's build tree ends with `Doors 0.0.9 (build <id>)`, `pos 0.0.9 (build <id>)`, radio info, system.info and `Done.` | BusyBox tar on the device refused a link: record the exact message |
| B3 | Links after | the three paths are the symlinks of A3 and `/usr/share/pocketos` is still a directory; `/usr/bin/doors`, `/etc/doors-release` and `/usr/share/doors/THIRD_PARTY_NOTICES.txt` are regular files | the DOCUMENTED tar behaviour does not hold on the device |
| B4 | Readers | A4 to A6 on this unit | |
| B5 | Reboot | reboot; the services come up; A2 and A5 again | |
| B6 | Rollback (optional) | put the unit's PocketOS-era files back (its own backup tar), then `rm -rf /usr/bin/doors /etc/doors-release /usr/share/doors`; `pos version` is the old one and `/etc/pocketos-release` a regular file | the documented rollback step is incomplete |

Pass: A1 to A9 and B1 to B5. Then Phase 2 can be put to the owner for
acceptance, and the BusyBox tar statements in ADR-005 move to VERIFIED.

## Result on unit A (2026-09-15)

Every check below was run by script over SSH (key auth, host key read over the
serial console) or on the serial console itself, and printed `ok` or `FAIL`;
no result rests on an operator's reading of the screen. The only operator
actions were the card swap and powering on.

**Image.** Rebuilt from the branch tip rather than using the `50f4c24`
candidate, so the deploy tree and the flashed card are one build: clean WSL
clone at `691b508`, `apply_to_sdk.sh` rc 0 (clean tree, RadioLib `034126e` and
ggwave `a38e38b` clean), `build_image.sh` rc 0 in 227 s, IMAGE GATE: PASS,
`sha256sum -c` of SHA256SUMS.txt and of the artefact's `.sha256`: OK.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `sysimage-sdcard.img` | 763,363,328 | `e1b4df95c8f2ff58eff1e449e3cecf5fe4077ee0765e3f3480ae290e039fdba7` |
| `doors-0.0.9-tdisplay-k230-691b508.img.gz` | 204,599,693 | `8a45e7d9a2f0a4855b762f9a8f68d9571982703f95db7353428e5bbc661395f2` |

Inside the rootfs (debugfs): `/usr/bin/doors` regular 0755, `/usr/bin/pos` ->
`doors`, `/etc/doors-release` regular 0644 `0.0.9` / `BUILD_ID=691b508`,
`/etc/pocketos-release` -> `doors-release`, `/usr/share/doors/THIRD_PARTY_NOTICES.txt`
regular, `/usr/share/pocketos` a directory, its notices ->
`../doors/THIRD_PARTY_NOTICES.txt`.

### Part B: deploy over a PocketOS-era unit

Run first, on unit A as it was: the card flashed with the graphics-branch image
(`0.0.9` / `e999ab1`, the PocketOS-era identity layout).

| # | Result | Evidence |
| --- | --- | --- |
| B1 | **PASS** | `/usr/bin/pos` regular (md5 `1be30456…`), `/etc/pocketos-release` regular `0.0.9` / `BUILD_ID=e999ab1`, `/usr/share/pocketos/THIRD_PARTY_NOTICES.txt` regular; no `doors` name anywhere on the rootfs. Before deploying: a backup tar of all 15 paths `deploy.sh` overwrites, md5 manifests of `/etc/default`, `/etc/pocketos` and `/var/lib/pocketos` (outside `log/`, plus a sentinel file written for the test), the log file list (16) |
| B2 | **PASS** | this branch's `deploy.sh 192.168.10.157`, rc 0 in 5 s: services stopped, archive extracted, services started, then `Doors 0.0.9 (build 691b508)`, `pos 0.0.9 (build 691b508)`, radio info, `system.info`, `Done.`; no tar message |
| B3 | **PASS** | the three links of A3, `/usr/share/pocketos` still a directory; `/usr/bin/doors` (0755), `/etc/doors-release` and `/usr/share/doors/THIRD_PARTY_NOTICES.txt` regular; the notices read the same through the old path; exactly one `doors` binary, one `pos` name and the two release names on the rootfs |
| B4 | **PASS** | A2, A4, A5 and A6 on this unit (`691b508` everywhere, `release_file`/`release_build` equal to `version`/`build`); A8 as `doors` and as `pos`; `/etc/default`, `/etc/pocketos`, `/var/lib/pocketos` byte-identical to before; no log file removed; `sysd`, `netd`, `radiod`, `pocketos-shell` running, 0 restarts, no crash-loop marker, no crash report, 0 ERROR in every service log; shell on DRM. 37 checks, 0 failures |
| B5 | **PASS** | `reboot` on the serial console; `sysd`, `netd`, `radiod`, `pocketos-shell` OK at boot; the same 37 checks, 0 failures |
| B6 | **PASS** | services stopped, the unit's own backup tar extracted, `rm -rf /usr/bin/doors /etc/doors-release /usr/share/doors`, services started: the three paths regular files again, all 15 files byte-identical to the originals, `pos version` = `pos 0.0.9 (build e999ab1)`, `/etc/pocketos-release` regular `e999ab1`, `pos system info` release line; data unchanged, services healthy, 0 ERROR. 18 checks, 0 failures |

**Repeated deploy (idempotence): PASS.** The same `deploy.sh` run again over
the first deploy (rc 0 in 4 s), then the 37 checks again, 0 failures. A
manifest of every deployed path (type, mode, owner, size, md5; link targets)
and of every `doors`/`pos`/release/notices name on the rootfs is identical
before and after, except the old-path notices symlink's own timestamp: tar
recreates a link on each extraction. No duplicate names, no release drift,
data byte-identical.

### Part A: fresh flash

Then the development card was flashed with the image above
(`flash-devcard.ps1`: source gate PASS, target the removable USB card of
62,914,560,000 bytes, reader serial 121220160204, disk 0 excluded, 68.7 s,
read-back differs only at 440..443, `RESULT: FLASH PASS`).

| # | Result | Evidence |
| --- | --- | --- |
| A1 | **PASS** | first boot on fresh defaults: U-Boot splash line, `sysd`, `netd`, `radiod` OK, `pocketos-shell: disabled`, `k230_phone_ui: OK`; `doors logs --crashes` "(none)"; 0 crash reports |
| A2 | **PASS** | `Doors 0.0.9 (build 691b508)` and `pos 0.0.9 (build 691b508)` |
| A3 | **PASS** | `/usr/bin/pos` -> `doors`, `/etc/pocketos-release` -> `doors-release`, `/usr/share/pocketos/THIRD_PARTY_NOTICES.txt` -> `../doors/THIRD_PARTY_NOTICES.txt`, `/usr/share/pocketos` a directory |
| A4 | **PASS** | both release names print `0.0.9` / `BUILD_ID=691b508` |
| A5 | **PASS** | `doors system info`: `doors 0.0.9 (build 691b508)`; `pos system info`: `pocketos 0.0.9 (build 691b508)` |
| A6 | **PASS** | `doors call sysd system.info`: `version` 0.0.9, `build` 691b508, `release_file` 0.0.9, `release_build` 691b508 |
| A7 | **PASS** | `pos-hwcheck /tmp/hwc` rc 0; the report's `--- release: cat /etc/doors-release` section holds `0.0.9` and `BUILD_ID=691b508` |
| A8 | **PASS** | `radio info`, `wifi status`, `logs` as `doors` and as `pos` on first boot; `app list` too once the shell ran (A9); one `doors` binary, one `pos` name, two release names; services healthy, 0 ERROR. 32 checks, 0 failures |
| A9 | **PASS** | panel handed to the Doors shell (`/etc/default/k230_phone_ui` `ENABLE=0`, `/etc/default/pocketos-shell` `ENABLE=1`), `reboot` on the serial console, all four services OK at boot; A2 to A8 again with `app list` and the shell on DRM: 37 checks, 0 failures; `doors logs --crashes` none; `pos-hwcheck` release section again right |

### After the test

- Unit A runs this image, `0.0.9` / `691b508`, with the Doors shell owning the
  panel and the bench SSH key installed. The graphics-branch image it carried
  before Part A was overwritten by the flash.
- The shared SDK target tree was put back into the PocketOS-era layout (the
  three links replaced by copies; `/usr/bin/doors`, `/etc/doors-release`,
  `/usr/share/doors` removed) and the applied manifest set aside as
  `.pocketos-applied.doors2-691b508`: re-apply before any build there.
- Evidence (not in the repository): `out/doors2-691b508/` holds the flash log
  and `hwgate-unitA/` the build log, both deploy logs, every check's output and
  the serial captures of the reboots and the first boot.

## Validation done without hardware (2026-09-15)

Source `50f4c24`, from fresh WSL clones; master `8070379` run the same way for
the baseline. The commit that adds this sheet changes nothing else.

| Check | Branch `50f4c24` | Master `8070379` |
| --- | --- | --- |
| `make all`, `make test` (host, -Werror) | rc 0; 3,488 ok, 0 FAIL, 0 warnings, 75 suites | rc 0; 3,401 ok, 0 FAIL, 0 warnings, 74 suites |
| New or extended suites | `identity_test` (43, including the `system info` release line for all three layouts in a private mount namespace), `paths_test` (release reader), `pocketsys_test` (layouts through system.info), `build_provenance_test` (artefact), `notices_test`: 0 failures | |
| `pos` output against master's binary | 32 command lines (usage, errors, every subcommand without services): byte-identical apart from the build id | |
| Shell (SDL) build, shell tests | 0 warnings; 317 ok, 0 FAIL; CMake says `Doors 0.0.9 build 50f4c24` | 0 warnings; 317 ok |
| riscv64 `make all` (ENABLE_SX1262=1) | rc 0, 4 warnings, all vendor ggwave | rc 0, the same 4 |
| riscv64 DRM/sysroot shell | rc 0, 0 warnings | rc 0, 0 warnings |
| `apply_to_sdk.sh` | rc 0, clean tree, BUILD_ID `50f4c24`, RadioLib `034126e` and ggwave `a38e38b` clean | |
| Image build | rc 0 in 218 s; installed notices match `pocketos.hash`, the old-path link checked; no warning line from first-party sources (the vendor `face_detect`/OpenCV warnings as in the previous image build, one duplicate line more) | |
| `verify_image.sh` | IMAGE GATE: PASS (in the build and on the exported image) | |
| Export | `sha256sum -c` of SHA256SUMS.txt and of the artefact's `.sha256`: OK; the artefact decompresses to the raw image's sha256 | |
| Inside the rootfs (debugfs) | `/usr/bin/doors` regular 0755; `/usr/bin/pos` -> `doors`; `/etc/doors-release` regular 0644, `0.0.9` / `BUILD_ID=50f4c24`; `/etc/pocketos-release` -> `doors-release`; `/usr/share/doors/THIRD_PARTY_NOTICES.txt` regular 0644, identical to the commit's; `/usr/share/pocketos` a directory, its notices -> `../doors/THIRD_PARTY_NOTICES.txt`; `pos-hwcheck`, `pos-spixfer`, `pos-wave`, `pos-supervise`, `pocketos-shell`, `S90pocketos-shell` unchanged in name; settings seed header "# Doors settings"; boot `/logo.xrgb` the vendor splash (`9fd79fee…`) | |
| `pocketos-legal-info` (scratch directory) | rc 0, 0 warnings; licence "Not yet decided (Doors; no licence granted), …"; collected notices identical to the commit's, hash `915971ad…` verified | |
| Shared SDK afterwards | target tree back in the PocketOS-era layout (the three links replaced by copies, `/usr/bin/doors`, `/etc/doors-release`, `/usr/share/doors` removed); applied manifest set aside as `.pocketos-applied.doors2-50f4c24` | |

Candidate image at the time, superseded for the hardware test by the `691b508`
rebuild above, not flashed and not in the repository:
`~/work/doors2-image-50f4c24/` on the WSL build host.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `sysimage-sdcard.img` | 763,363,328 | `53b6a039d063c53e72b0edd02bbb39467bacc28073c74fd153483e7901317b2e` |
| `doors-0.0.9-tdisplay-k230-50f4c24.img.gz` | 204,602,853 | `e2e4dfc16a62269756422346d3b63987f9b81d49682123b10de0fde4bf62c84e` |
| `sysimage-sdcard.img.gz` (vendor build output) | 204,602,873 | `76308d1ce32737d4268578ddd2d59d4ca967d1bbbf7810a71ce87d89da4e4d1a` |

BUILD_INFO says Doors 0.0.9 / `50f4c24`. Pins: BSP `bb831ab`, SDK `22d02c6`.

At the time, every item in parts A and B was still unverified; they passed on
unit A the same day (above).
