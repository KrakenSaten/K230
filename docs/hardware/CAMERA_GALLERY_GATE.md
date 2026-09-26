# Camera gallery: unit A integration gate

**Unit A carries build `f6fe537`** (branch `feat/camera-gallery`):
`/usr/bin/doors-shell` (stripped md5 `f44b878e…`) and `/usr/bin/pos-camera`
(md5 `b42b6164…`), replaced together. **Unit A is hung** since about
2026-09-26 23:38Z: its serial console (COM9) answers nothing and it is off
the network. It needs a power cycle by hand. After the power cycle it boots
this build (the files are on the root filesystem). Rollback of **both**
binaries, to the shell `f2c22f1` (== master `dd3809b` code, md5 `05a4d09a…`)
and the v0.1.0 image's `pos-camera` (md5 `b3220ffc…`):
`/root/rollback-camera-gallery/RESTORE.sh`. `state-before.tar` and
`camera-md5.txt` are next to it.

Result: **NOT PASSED.** The unit hung during the 9th–10th of ten memory
cycles (§6). The cause is not established. Everything the gate checked
before the hang passed. Run 2026-09-26/27 by Claude, with no owner present.
Evidence is in `out/camera-gallery-gate/` (not committed).

The photo library was restored before the hang. It holds exactly the 8
photos it had before the gate, all md5-verified against `camera-md5.txt`.
`/root/Pictures` holds one byte-identical export of
`IMG_20260925_054027_0006.jpg`, with that photo's time.

Scope: Camera only (apps/camera, core/pocketcam, tools/camera, their tests
and docs). Unchanged: the shell's chrome, status cluster, viewport, launcher,
`vendor/`, VERSION, and the GC2093/vvcam capture path.

## 1. Baseline

- The branch was already rebased by the cloud session onto current master
  `dd3809b` (tip `2e3fc43`; range-diff against the reported `63fb6aa` shows
  only a Makefile context line). No rebase was needed.
- Checkpoints: `backup/camera-gallery-2e3fc43-pre-integration`, and
  `backup/camera-gallery-63fb6aa-cloud` (local branches).
- Unit A before the gate (`g0-state.log`):
  - shell `f2c22f1`, `pos-camera` from v0.1.0;
  - 8 photos, 97 MB free;
  - no crash reports;
  - Automatic landscape, home, locked.

## 2. Integration review and fixes (`f6fe537`)

Three review passes (parsing, storage/export, helper lifecycle). Each finding
was checked against the code, and only real defects were fixed:

- **Export identity.** "Same name and same size" was taken to mean the same
  photo. Photo numbers come back after the newest photo is deleted, so a
  different photo was reported "Already in Files" and never copied. Now the
  bytes are compared: the same photo is "already"; anything else is kept, and
  the copy goes to `-2`, `-3`, …
- **Export temporaries.**
  - Before: the temporary had a fixed name and was never swept, so a helper
    killed mid-copy left a hidden partial JPEG in `~/Pictures`.
  - Now: the name carries the helper's pid, and each new library helper sweeps
    leftover temporaries.
- **Export path cut to 95 characters.** The same code was a
  `-Wformat-truncation` error that broke `make all` under `-Werror` with the
  pinned Xuantie GCC 14 and host GCC 11.
- **Deleting a photo that is already gone** failed forever, so its entry could
  never be removed. It now counts as deleted.
- **Deadlines.** The library helper answers requests in order. A delete queued
  behind slow decodes got 3 s in total and was killed as hung. Each answer now
  starts the window of whatever waits behind it.
- **Slideshow.** It stopped only on a tap on its picture box: in landscape the
  side margins did nothing. The touch box now spans the body except the status
  line. A first attempt made the whole frame clickable; the app test rejected
  it as an overlapping touch target, and it was reworked.
- **Camera layout.** The camera screen was laid out again while the gallery
  had the body.
- **EXIF.**
  - The block is now zeroed first: one uninitialised byte was written into
    every photo whenever VERSION has an even length.
  - Dates before 1995 are treated as unknown, in EXIF and in names (an unset
    camera clock gives 1970 or 1980), and name dates must be real dates.

Mutation check: each fix was reverted in turn, and a new test fails every time
(5 of 5).

Not changed, recorded for later:

- A number is reused after the newest photo is deleted. This existed before;
  the export fix makes it harmless there.
- A helper that survives SIGKILL is not reaped later (pre-existing session
  code).
- `finish()` can signal a pid that was already reaped (pre-existing, tiny
  window).
- The written EXIF has no ExifVersion tag. GDI+ reads it anyway.
- The helper keeps up to about 37 MB after a 12 MP progressive decode, because
  glibc keeps freed heap. The ceiling is bounded and only lasts while the
  gallery is open.
- Unsupported-but-valid JPEG types are shown as "damaged".

## 3. Host validation (WSL clean clone of `f6fe537`)

Suite results:

| Suite | PPM | libjpeg 9f |
| --- | --- | --- |
| pocketcam | 106 | 105 |
| pocketcam_gallery | 77 | 90 |
| camera_state | 66 | 66 |
| camera_layout | 126 | 126 |
| camera_gallery | 90 | 90 |
| camera_session | 131 | 131 |

- libjpeg 9f was built privately from the SDK's own `jpegsrc.v9f.tar.gz`; the
  image has libjpeg.so.9.6.0. Nothing was installed on the host.
- All suites pass in both builds.
- ASan/UBSan (PPM and 9f): clean when run on their own; 5 of 5 runs pass
  131/131.
  - Inside the full script, twice, `a crash is a crash` / `no child left`
    failed. Those two checks are unchanged since master.
  - The deliberately aborting helper took more than 3 s to die. This matches
    WSL's `core_pattern` pipe (`|/wsl-capture-crash`), which ignores
    `ulimit -c 0`.
  - Classified as environmental.
- `camera_lint` passes.
- `camera_shell_test` passes, including `camera_app_test` 185/185 in portrait
  and landscape with no LVGL warnings.
- `chrome_shell_test` and `display_geometry_shell_test` pass.
- Host `make all` passes with `-Werror`.
- Pinned Xuantie GCC 14.1.1: `make all` (`ENABLE_SX1262`, `POCKETCAM_JPEG`,
  `ZABBIX_CURL`, `ENABLE_MESHCORED`, `-Werror`) gives 0 first-party warnings.
  `pos-camera` needs `libjpeg.so.9`. The DRM/sysroot shell has 0 warnings.

## 4. Deployment

- Tool: `g1_deploy.sh`.
- It refuses while a `pos-camera` runs.
- It copies the old binaries and a state tar into
  `/root/rollback-camera-gallery/` and writes `RESTORE.sh`, which restores
  both binaries together so the protocol is never mixed.
- Both copies were md5-verified.
- The shell was stopped for the swap and started again. `doors shell info`
  reported `f6fe537`.
- The earlier rollback directories were left untouched.

## 5. Checks passed on unit A

Fixtures: IMG_0101–0120 (orientations 2/3/6/8, truncated, garbled, empty,
text, a 4032×3024 progressive JPEG, a 9000×120 over-cap JPEG, 10 plain
copies), made from one of the unit's own photos.

- **Capture and preview.**
  - Live preview works: consecutive frames differ.
  - Real GC2093 JPEG stills were taken in landscape and in portrait.
  - The EXIF read back by the gallery matches the name
    ("Taken 2026-09-26 23:11:54").
- **Sensor ownership (sampled every 10 ms).**
  - PHOTOS ends the session helper before the library helper starts (gone at
    1635 ms, library up at 1838 ms).
  - The library helper never holds a `/dev/video` fd.
  - CAMERA takes the sensor again.
- **Ten PHOTOS ↔ CAMERA trips.**
  - Every trip: the sensor was taken again within 100 ms, the library helper
    had 0 video fds, and there was 1 helper at most.
  - Same shell pid, supervisor restarts 0, no zombies.
  - The preview was live afterwards.
- **Gallery.**
  - 29–30 photos, newest first. Pages: 10 per page in landscape, 12 in
    portrait. NEWER and OLDER are disabled at the ends.
  - Legacy photos: the date comes from the name.
  - Fixtures with no date: "Date unknown: the clock was not set".
  - Orientations 3, 6 and 8 are correct against the upright original.
  - Truncated and garbled files show what libjpeg recovered.
  - Empty and text files show "Cannot show".
  - The over-cap file shows "The picture is too large to show".
- **Delete.**
  - The confirmation appears; CANCEL keeps the file.
  - DELETE removes it, and the next older photo is shown.
  - A file removed over SSH shows "The file is gone", and DELETE then removes
    its entry.
  - Deleting the final photo leaves "No photos yet", and SLIDESHOW disappears.
- **Slideshow.** Five minutes, 80 decodes, portrait:
  - Interval min 3934, median 4020, max 4058 ms; the show wraps and skips
    unreadable photos.
  - Shell RSS went 16.9 → 19.1 MB in the first 10 s, then stayed flat.
  - A tap in the margin beside the picture stops it at once. A tap on the
    status line does not, by design.
  - With only unreadable photos it says "No photo could be shown" and returns
    to the grid.
- **Export.**
  - The copy is byte-identical and keeps the photo's mtime.
  - Exporting again says "Already in Files: …" and writes nothing.
  - A same-name, same-size file with random bytes was kept, and the copy was
    saved as `…_0006-2.jpg`.
  - No temporaries were left.
  - The copies appear in Files → /root → Pictures.
- **EXIF, independent decoders.**
  - Windows GDI+ decodes the export at 1920×1080.
  - Orientation 1, Software "Doors 0.1.0", DateTime and DateTimeOriginal
    2026:09:26 23:11:54.
  - A stdlib parser agrees: IFD0 is sorted, DateTimeOriginal is in the Exif
    IFD, and the markers are SOI, APP0, APP1.
- **Rotation.**
  - Landscape → portrait during a running slideshow, and portrait → landscape
    from a photo view.
  - The shell restarted in place, no helper was left, and the shell RSS came
    back to 14.6 MB.
- **Leaving the app** from the grid, a photo view and a running slideshow:
  home, no helper, no zombie. Camera reopens and takes the sensor.
- **Timings on the C908**, from the helper's open-file trace:

  | Operation | Time |
  | --- | --- |
  | Thumbnail of a 1080×1920 JPEG | ≈65 ms |
  | Screen picture | ≈135 ms |
  | Page of 9–10 thumbnails | 0.75–1.1 s |
  | 12 MP progressive decode | ≈0.58 s (helper RSS peak 37 MB) |

- **Logs.**
  - No shell WARN or ERROR since the deploy, and no crash reports.
  - dmesg shows only boot-time lines and the normal vvcam release messages.

Not tested on the unit:

- keyboard control: Camera has none, documented;
- low free space: host hook only;
- an unset device clock: host suites and undated names only; the clock was
  not changed;
- PHOTOS tapped during the camera's open (review item). It was avoided on
  purpose because of the earlier whole-unit vvcam lock-up.

## 6. The hang

Loop (`g21_rss.sh`): unlock, then repeat about every 10 s:

1. `doors app start camera`, wait 3 s;
2. PHOTOS, wait 3 s;
3. a photo, wait 2 s;
4. back;
5. read the shell RSS at home.

What happened:

- Home RSS: 15.4, 16.3, then 16.8 MB for cycles 2–9, a plateau.
- During cycle 9 one SSH connection timed out at banner exchange. The next
  one worked (RSS read).
- During cycle 10 every connection failed with "No route to host".
- Unit A's MAC dropped out of the host's ARP table (both .171 and .157).
- COM9, the unit's console (VERIFIED in the bring-up record), prints nothing
  in response to a newline.
- Unit B (COM12, .187) was unaffected.

That is roughly the 27th camera open since the deploy (10 trips, reopens,
and 9 of these cycles).

What is known:

- The camera close path the gallery uses is the app's own:
  `camera_session_abandon()` with the destroy grace. Leaving Camera on master
  uses the same path.
- The gallery does make camera open/close far more frequent.
- The earlier Camera gate recorded a whole-unit lock-up in the vendor
  vvcam/ISP stack (STREAMOFF+STREAMON on one open node; fixed by
  close/reopen; cause LIKELY, not proven).
- Whether this is the same class of fault, a new one, or unrelated (power,
  Wi-Fi driver) cannot be told without the console output.

Next:

1. Power-cycle.
2. Keep a serial capture on COM9 running.
3. Repeat the camera open/close cycle with the gallery out of the loop
   (`doors app start camera` / back only, same cadence) and with it.
4. Compare.

**Merge recommendation:** NOT READY until the hang is explained or shown not
to depend on this branch.
