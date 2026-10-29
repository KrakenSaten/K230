# Library (feasibility prototype)

An offline knowledge collection on the unit: search a Kiwix/openZIM archive,
read its articles with their pictures, follow its links, go back, keep
bookmarks - with no network, ever.

Status: **prototype on branch `claude/k230-offline-library-0c9f28`, draft PR.
Host-validated against a real Wikipedia archive; cross-built for the K230
with the pinned toolchain; never run on a unit.** Out of the shell unless
built with `-DPOCKETOS_WITH_LIBRARY=ON`; the helper is not installed and not
in the image. ADR-015 (proposed) records the decision it asks for.

## What it is

```
 doors-shell (Apache-2.0, LVGL thread)            pos-library (GPL-2.0+ via libzim)
 ┌──────────────────────────────────────┐  pipe   ┌───────────────────────────────────┐
 │ apps/library  (profile: one file)    │ ──────▶ │ libzim 9.8.2: open, decompress     │
 │ apps/browser  screen + view, Library │ open/   │   (zstd, xz), title / full-text    │
 │               mode; session (spawn,  │ stop/   │   search                           │
 │               watchdog, poll only)   │ quit    │ core/web: HTML reader, bounded doc │
 │ core/web      doc, proto, store, url │ ◀────── │ libjpeg / libpng / libwebp decode  │
 └──────────────────────────────────────┘ page,   │ no HTTP client, no socket          │
           bookmarks: <state>/library/state pixels└───────────────────────────────────┘
                                                        reads one .zim (read only)
```

- **NOMAD reuse.** Project N.O.M.A.D. provides offline knowledge by running
  `kiwix-serve` in Docker over Kiwix `.zim` archives it downloads. The
  Library reuses the same ecosystem one level lower: **libzim**, the openZIM
  reference reader, and the same archives. Nothing of NOMAD's Docker stack,
  dashboard, databases, AI or marketplace is used.
- **Why this shape.** Browser (ADR-009) already is a bounded reader with a
  helper that owns all untrusted bytes, a line protocol, a renderer for
  headings, text, links and pictures, back/forward and bookmarks. An archive
  entry is HTML; so pos-library answers Browser's protocol and the shell
  links nothing new. libzim, which is GPL and parses untrusted archives,
  never enters the shell.
- **Not a web view.** No CSS, no JavaScript, no forms (Browser's reader).
  Interactive archives, video and PDF are refused with a reason.

## The collection

One archive, read only, never changed:

1. `--archive FILE` (tests);
2. else `$POCKETOS_LIBRARY_ARCHIVE`;
3. else `archive=PATH` in `<config>/library.conf` (`/etc/pocketos/library.conf`);
4. else the first `*.zim`, by name, in `$HOME/Library` (`/root/Library`, on
   Files' first screen).

Replacing the file replaces the collection; Doors is not rebuilt. **Where it
fits:** the root filesystem is 600 MB with about 120 MB free until the owner
runs System > Expand storage (docs/api/system.md), so the prototype's 182 MB
archive needs an expanded card or the USB drive
(`archive=/media/usb/<file>.zim` - FAT32 allows files up to 4 GB). Nothing
here partitions, moves or assumes free space; a missing or unreadable archive
is an error page that says why.

The archive used: docs/apps/library/CONTENT.md (source URL, date, size,
SHA-256, content terms).

## Reading journey

| Step | How |
| --- | --- |
| Open the collection | Start page: OPEN THE COLLECTION (the archive's main page) or ABOUT THIS COLLECTION |
| Search | Type in the field, FIND. Results page: "Titles", and "In the text" with snippets when full text is available. The page says which search ran. |
| Read | Headings, paragraphs, lists, quotes, tables as text, inline links; JPEG, PNG and **WebP** pictures (Wikipedia archives carry WebP). SVG drawings and pictures outside the archive show as a captioned placeholder with the reason. |
| Follow links | A link into the collection opens it. A link out of it is shown muted and refused: "Not opened: ... is outside this collection, and Library works offline". |
| Back / forward | Browser's buttons and history. |
| Bookmark | + on an article; the start page lists bookmarks and recent articles; kept in `<state>/library/state` (0600 in a 0700 directory), apart from Browser's. |
| The collection | ABOUT THIS COLLECTION: title, description, date, source, creator, publisher, language, flavour, licence if the archive states one, counts, search kind, file and size. |

Portrait and landscape are Browser's layouts; themes and text sizes are
Browser's rules (the article keeps one size, DS §46.4). The header hint says
OFFLINE on every page.

## Search

- **Titles** always: libzim's suggestion search.
- **Full text** when libzim is built with Xapian **and** the archive has a
  full-text index (Kiwix's Wikipedia archives do).

The K230 build has **no Xapian**: Xapian and ICU are GPL-2.0+ and
Unicode-licensed Buildroot packages that are not in the image, and ICU's data
alone is about 30 MB. Without Xapian libzim can only match **the start of a
title, case-sensitively**; pos-library also tries the words with the first
letter and then every word capitalised (ASCII only), so `greenhouse effect`
finds "Greenhouse effect". It does not find words in the middle of a title or
in the text, and the results page says so. Full-text search on the unit is the
open prerequisite named in ADR-015.

## Limits

Browser's, unchanged: 800 blocks, 500 links, 16 pictures, 256 KB of text per
page - long Wikipedia articles are cut and say so ("This article is longer
than Library shows"). No scrolling to an anchor: a footnote link reopens its
article at the top. Hidden maintenance categories that Wikipedia hides with
CSS show as a list at the end. One archive at a time. No PDF, EPUB, video or
interactive content.

## Licences

| Component | Licence | Where | In the image? |
| --- | --- | --- | --- |
| Doors code (app, view changes, helper source, tests) | Apache-2.0 | this repository | prototype only |
| libzim 9.8.2 (openzim/libzim @ 26ec526f) | GPL-2.0-or-later (`COPYING`, file headers "or (at your option) any later version") | linked statically into pos-library | **no**; no Buildroot package |
| zstd 1.5.7 | BSD-3-Clause or GPL-2.0 (Buildroot's version and hash) | linked statically into pos-library (K230) | no (`BR2_PACKAGE_ZSTD` off) |
| liblzma (xz 5.6.4), libjpeg, libpng 1.6, libwebp 1.5.0 | 0BSD/public domain, IJG, libpng, BSD-3-Clause | dynamically linked | **yes** (docs/legal/manifest.csv) |
| Xapian 1.4.18 / ICU 70 | GPL-2.0-or-later / Unicode | host build only (full-text measurement) | no |
| The archive | Wikipedia text CC BY-SA 4.0, pictures per Commons file | build host only | no |

Because libzim is GPL-2.0-or-later, **pos-library as built is GPL** (with the
Apache-2.0 web code, GPL-3.0-or-later, the GPL version Apache-2.0 is
compatible with). It is a separate program; the shell only exchanges text
lines and pixel files with it and stays Apache-2.0. Shipping it is the
owner's decision (ADR-015); NOMAD's Apache-2.0 licence covers none of this.

## Building

```sh
# libzim (+ zstd for the K230) in a prefix of their own; the SDK, its sysroot
# and the toolchain are only read:
tools/library/build_libzim.sh host <workdir>     # HOSTDEPS_ROOT=... (zstd, xz, Xapian, ICU dev files)
K230_SYSROOT=<sdk output>/host/riscv64-buildroot-linux-gnu/sysroot \
  tools/library/build_libzim.sh k230 <workdir>

# the helper
make tools/library/pos-library LIBZIM_CFLAGS=-I<prefix>/include \
     LIBZIM_LIBS="<prefix>/lib/libzim.a <prefix>/lib/libzstd.a -llzma"
# the shell with the app
cmake ... -DPOCKETOS_WITH_LIBRARY=ON
```

`make test` runs `tests/library_view_test` and `tests/library_lint.sh` (no
libzim needed). `make library-test LIBRARY_ARCHIVE=...` adds the helper
suite; `tests/library_shell_test.sh` needs the simulator.

## Validation (2026-10-29)

Host: AMD Ryzen 7 9700X (8 cores, 16 threads), WSL2 Ubuntu 22.04, 15.5 GB
RAM, archive on the WSL ext4 disk. Archive: CONTENT.md. All runs from the
branch's tree.

| Check | Result |
| --- | --- |
| `tests/library_view_test` (Library mode of the view, Browser unchanged) | 26/26 |
| Browser's suites with the shared code changed (web_url, web_html, web_proto, web_history, web_store, web_fetch, web_image, web_tiles, browser_view, browser_session, browser_lint) | all pass |
| `tests/library_helper_test.sh`, full-text build | 42/42 |
| the same, title-only build (the K230 configuration) | 42/42 |
| the same under ASan + UBSan (title-only build) | 42/42, no report |
| `tests/library_shell_test.sh` (simulator, portrait, landscape, reopen, light theme, missing archive, no network) | see the PR |
| `tests/library_lint.sh` | all pass |

What the helper suite proves with the real archive: a search returns the real
article; the article has h1/h2 headings, text, followable links into the
collection and links out of it sent as not followable; a WebP picture is
decoded (340 x 294) into a 0600 file of exactly w*h*2 bytes; an internal link
opens its article; the collection's title, date and source; `notfound` for a
missing entry; the main page at its real address. Then **all of it again in a
network namespace with no interface** (`unshare -rn`), and once under
`strace -e trace=socket,connect,sendto,bind`: **no network system call**.
Missing folder, empty folder, missing file, random bytes and an archive cut
to 1 MB are each answered "Collection not available" with the reason; quit,
EOF and SIGTERM end the helper.

### Measurements (host; not K230 numbers)

`pos-library bench` - open, then for each query: search, open the first
result, decode its pictures. Page cache warm (second run of two):

| | full text (libzim + Xapian) | title only (as on the K230) |
| --- | --- | --- |
| open the archive | 2 ms (9 ms first run) | 2 ms (9 ms) |
| search | 40-78 ms (titles 0-5 ms, full text 40-73 ms) | 0 ms |
| open an article (472-800 blocks) | 2-6 ms | 4-6 ms |
| decode its 15-16 pictures | 6-16 ms | 7-16 ms |
| peak resident memory after 4 searches + 4 articles | 89.6 MB (78.7 MB anonymous, 10.9 MB mapped archive) | 20.2 MB (15.2 MB anonymous, 5.1 MB mapped) |
| helper size, stripped | 35 MB (x86-64, ICU data static; host only) | 1.9 MB x86-64; **783 KB riscv64** |

The shell's side (making the page's LVGL objects) is Browser's code and
logged as `library: page made: ...`; numbers in the PR.

### K230

- **Built** (pinned Xuantie gcc 14.1.1, `-mcpu=c908v -mtune=c908`):
  zstd 1.5.7 and libzim 9.8.2 (no Xapian) into a separate prefix against the
  vendor SDK sysroot (read only); `pos-library` (riscv64, 783 KB stripped,
  NEEDED: liblzma, libjpeg, libpng16, libwebp, libstdc++, libm, libgcc_s,
  libc - all in the vendor target tree); the DRM shell with
  `-DPOCKETOS_WITH_LIBRARY=ON` against the Doors sysroot already on the build
  host (read only).
- **Not run**: no unit, no qemu-user on the build host. Nothing here is a K230
  measurement.
- **Uncertain**: decompression and WebP decoding speed on the C908; first open
  of a 182 MB archive from the SD card or a FAT32 USB drive (libzim memory-maps
  it); the shell's page making for 800 blocks (Browser's gate measured it on
  unit A for web pages); title-only search being enough for people.

**Proposed device check** (unit B, about 15 minutes, no flashing): copy
`pos-library` to `/usr/bin`, the DRM shell built with the app to
`/usr/bin/doors-shell` (keep the old one), and the archive to `/root/Library/`
after `df -h /root` shows room; open Library under MORE, search
`greenhouse effect`, open the article, follow "greenhouse gases", BACK,
bookmark, close and reopen; note the `library:` lines in shell.log and
pos-library.log (open, search and page times) and `VmHWM` of pos-library;
then restore the shell.
