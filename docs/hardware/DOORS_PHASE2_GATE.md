# Doors Phase 2: pending hardware gate

Branch `rebrand/doors-2-identity` (docs/decisions/ADR-005-product-name-doors.md,
"Phase 2 as implemented"). Everything that can be checked without a board has
been; the record is at the end. What remains is the hardware test the phase
table requires: flash and boot a unit, and deploy over a PocketOS-era unit.
Until it passes, Phase 2 is implemented but not accepted, and the BusyBox tar
behaviour it relies on stays DOCUMENTED.

Not flashed, not merged, not tagged. VERSION is still 0.0.9.

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

Candidate image, not flashed and not in the repository:
`~/work/doors2-image-50f4c24/` on the WSL build host.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `sysimage-sdcard.img` | 763,363,328 | `53b6a039d063c53e72b0edd02bbb39467bacc28073c74fd153483e7901317b2e` |
| `doors-0.0.9-tdisplay-k230-50f4c24.img.gz` | 204,602,853 | `e2e4dfc16a62269756422346d3b63987f9b81d49682123b10de0fde4bf62c84e` |
| `sysimage-sdcard.img.gz` (vendor build output) | 204,602,873 | `76308d1ce32737d4268578ddd2d59d4ca967d1bbbf7810a71ce87d89da4e4d1a` |

BUILD_INFO says Doors 0.0.9 / `50f4c24`. Pins: BSP `bb831ab`, SDK `22d02c6`.

Still unverified, and only a unit can settle them: every item in parts A
and B.
