# Unit B: Library installed (PR #118)

Date: 2026-10-29. Owner-authorised: the Library-enabled shell, pos-library and
the verified archive stay installed on unit B after the focused check.
Evidence class of everything below: VERIFIED on unit B unless marked.

## What was installed

| | Before | After |
| --- | --- | --- |
| `/usr/bin/doors-shell` | `e892032c` (#111+#112 integration), sha256 `8d4f8fe7...` | `7474d752`, sha256 `8ff0c727a2339e7e78f6ae05b1e8aea8df25e410b332948626932f51a679d0f3` |
| `/usr/bin/pos-library` | absent | `7474d752`, sha256 `8a2dcb5f76c14f605b0b79cf03ac7941159bf5a2457d59dbe2980dc919638258`, 783,184 bytes |
| `/root/Library/wikipedia_en_climate-change_maxi_2026-10.zim` | absent | 190,425,044 bytes, sha256 `882bdbf4...c17e` (docs/apps/library/CONTENT.md), 0644 |

`7474d752` is a local build branch, `device/library-unit-b`: this PR's head
`4e98ac3f` plus a cherry-pick of `e892032c` (DeskBuddy counts Vision reports by
the session's number; the K230's camera sequence stays 0). That fix was on unit
B's previous shell and is not in master, so a plain PR build would have broken
DeskBuddy on this unit. The branch is not pushed.

Build: `git archive 7474d752`, BUILD_ID file, pinned Xuantie gcc 14.1.1,
the Neon QR sysroot used for unit B's earlier shells (read only),
`-DPOCKETOS_WITH_LIBRARY=ON`; pos-library against libzim 9.8.2 (no Xapian) and
zstd 1.5.7 from `tools/library/build_libzim.sh k230`. Both built with 0
warnings. Every undefined dynamic symbol of both binaries resolves against
unit B's own libraries, except zstd's four weak `ZSTD_trace_*` hooks, which are
weak by design.

Nothing else was changed: sysd, netd, radiod, meshcored, pos-vision,
pos-browser, pos-audio-effects, liblvgl and `/usr/share/doors` (50 checksums)
are identical before and after. `settings.conf` and all 648 backed-up state
files are byte-identical. The services were never restarted; only
S90doors-shell was. The root filesystem had already been expanded
(14.2 G, 12.9 G free after the install).

## Rollback

```sh
ssh root@172.16.5.198 /root/rollback-library-7474d752/RESTORE.sh
```

It verifies the backup, puts back doors-shell `e892032c`, moves pos-library
into the kit's `removed/` and restarts the shell only. The collection and the
Library's bookmarks are kept. `RESTORE.sh --with-state` also returns
settings and state files to the backup and moves what the install created into
`created-after/`. Nothing is ever deleted.

**Exercised once**: RESTORE (shell `e892032c` came back, pos-library out),
then `INSTALL.sh` again (shell `7474d752` back). The older kit
`/root/rollback-integration-db111-vision112-e892032c` still returns further,
to master 5011fac9.

## Focused check

The helper on the hardware, before the shell was touched:

| Check | Result |
| --- | --- |
| hello | `zim,titles,img:jpeg,png,webp` (title search only, as built) |
| search "greenhouse effect" links the article | ok |
| article: h1/h2, text, links in and out (out sent not followable) | ok |
| pictures decoded | 16 for the article; a 340x294 file, 0600, exactly w*h*2 bytes |
| internal link, about (date 2026-10-13, source en.wikipedia.org), notfound, an outside address refused | ok |
| network system calls during search and reading (strace: socket, connect, sendto, bind) | none |
| a missing archive; an archive cut to 1 MB | "archive" failure with the reason |

The shell, driven by taps injected through the panel's own input device
(`/dev/input/event2`, slot 0, `tests/hw/touch_slot0_tap.py`, through the
shell's logged calibration). The unit runs landscape, neon, large text; tap
positions came from the simulator with unit B's `settings.conf`. Verified from
shell.log, pos-library.log and the Library's state file. There are no
screenshots: the unit's LVGL has no snapshot.

| Step | Result |
| --- | --- |
| Open Library | start page; no helper started |
| KEYS, type `greenhouse effect` on the touch keyboard, FIND | results page, 3 titles |
| Tap "Greenhouse effect" | article, 619 blocks, 500 links, 16 pictures |
| Tap "Earth's energy budget" (internal link) | that article, 556 blocks, 12 pictures |
| BACK | Greenhouse effect again |
| + | `bookmark https://library.invalid/Greenhouse_effect Greenhouse effect` in `/var/lib/pocketos/library/state` (0600, directory 0700) |
| Leave, reopen | helper ended, no picture directory left; start page with 1 bookmark; the bookmark opens the article |
| ABOUT THIS COLLECTION | opens |
| Reboot | one doors-shell `7474d752`, all services up, no new warning; no helper at boot; Library opens with its bookmark and the article renders; the helper ends on leaving |

A first typing attempt had a wrong key list ("ghreenhouse effect") and found
nothing. It is left in the Library's RECENT list, which the owner can clear.

## Measurements on the K230 (C908, SD card)

| | |
| --- | --- |
| open the archive | 142 ms cold (page cache dropped), 19 ms warm |
| title search | 0-6 ms |
| open an article in the helper (472-800 blocks) | 51-77 ms |
| decode an article's 15-16 WebP pictures | 143-382 ms |
| the shell making a 619-block page with 16 pictures | 649-705 ms, over 12-13 timer ticks (the screen stays responsive) |
| helper peak memory | 18.7 MB over a 4-article bench; 11.4 MB in the shell journey |
| shell peak memory | 19.2 MB |

## Not checked

The pictures' appearance on the panel (no capture on the device); the
"greenhouse gases" link further down the article (it needed scrolling); full
text (not built); portrait on the unit.
