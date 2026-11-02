# A clean SDK output builds with this branch (2026-11-02)

The check PR #115 names before merging: a build of master with
`fix/fresh-build-fortran-toolchain` in an SDK output that has never been
built, the image gate, and a comparison of the image's file set with 0.3.9's.
Nothing was flashed or booted.

## What was built

| | |
| --- | --- |
| Source | `47e1fa3c`: local merge of master `79852ca0` into this branch at `0615f2a0` (not pushed; the branch's history is unchanged) |
| Host | this PC's WSL2 Ubuntu 22.04, pinned Xuantie toolchain in `/opt/toolchain` (read only) |
| Vendor tree | a copy of the pinned tree (BSP `bb831ab`, SDK `22d02c6`) in `~/work/clean115/t-display-k230`, SDK reset to its commit with `git checkout -- . && git clean -fdx -e dl`: **no `output/` directory**. Only Buildroot's download cache (`dl/`) was kept; the existing SDK output was not used or touched |
| RadioLib / ggwave / MeshCore / Crypto | `034126e` / `a38e38b` / `3ca7e3f` / `37a76b8` (clean), Vision R0 sha256 `94a20ac0...d268` |
| Commands | `apply_to_sdk.sh`, `build_image.sh`, `verify_image.sh` as in docs/releases/v0.3.9.md "Reproducing", step 2 only (step 1 is what this branch makes unnecessary) |
| Time | apply 15:32Z, image 17:03Z (1 h 31 min) |
| Logs | WSL `~/work/clean115/{apply,build,verify}.log` |

## Result

- **Build: PASS.** Buildroot's external toolchain check passed with Fortran
  selected, and `camera_rtsp_demo` installed into the new
  `/root/app`. These were the two points where a clean output stopped
  (docs/releases/v0.3.8.md).
- **Image gate (`verify_image.sh`): PASS**: boot files, one shell service,
  R0 with the pinned hash, every service from build `47e1fa3c`.
- `sysimage-sdcard.img` 763,363,328 bytes (the same size as 0.3.9),
  sha256 `65b428d1dc813a28a556c7f82869277c2edd18f50908197107d77001c0ecf011`.
- The output's `.config` has `BR2_TOOLCHAIN_EXTERNAL_FORTRAN=y`, and there is
  **no `libgfortran*`** in the target tree or the image.

## File set compared with 0.3.9

The root partitions of this image and of 0.3.9's `c03795ce` image were
extracted with `debugfs rdump` (no mount) and listed with type, size and a
content hash.

- **Paths:** identical, except one file only in the new image:
  `/usr/share/doors/ui/icon-rotorterror.bin`, from master's RotorTerror
  (PR #119), not from this branch. No path is missing, and no file changed
  type or symlink target.
- **Contents:** 1,947 of 8,187 files differ. 1,781 are Python bytecode
  (`__pycache__`, which embeds source timestamps). The rest are build-host
  differences: build dates (BusyBox, OpenSSL "built on", kernel module
  `__DATE__`), the longer build directory (`/clean115`, 9 characters, in
  Perl's and Python's config files, numpy's config and coremark-pro's logs,
  and Buildroot's padding of the path in binaries), and Doors' own
  programs and `/etc/doors-release`, which are master's newer source.
- No difference was found that comes from Fortran being selected.

## Not covered

- The image was not booted. It is a test build of master, not a release
  candidate; the next release image includes this branch and needs its own
  fresh-card test.
- The comparison covers the root partition only. Boot files are covered by
  the image gate.
