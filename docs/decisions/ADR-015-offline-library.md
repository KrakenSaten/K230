# ADR-015: An offline library from Kiwix archives

Status: **Proposed** (feasibility prototype, branch
`claude/k230-offline-library-0c9f28`). Nothing here is accepted, nothing is
in the shell by default and nothing is in the image.
Date: 2026-10-29
Deciders: product owner (final), AI engineering partner (author)

## Context

The owner asked whether the unit can be a pocket offline knowledge device -
encyclopaedia articles, reference material, books - in the spirit of Project
N.O.M.A.D. (Crosstalk-Solutions/project-nomad), and for a bounded prototype
that proves it with a real collection rather than a mock.

What NOMAD is (read at commit 54ee30a, 2026-10-05): an Apache-2.0 web
dashboard (AdonisJS) that installs Docker containers, among them
`kiwix-serve`, and downloads curated Kiwix `.zim` archives from
download.kiwix.org (`collections/kiwix-categories.json`, `wikipedia.json`).
The knowledge itself is Kiwix's: openZIM archives read by libzim. Its Docker
stack, MySQL/Redis, Ollama and app marketplace are not wanted here (owner).

Binding constraints: ADR-001 (pinned SDK, no new package without a
proposal), ADR-002 (in-process screens, hardware and untrusted parsing out of
the shell), ADR-009 (Browser: a first-party reader, untrusted bytes in a
helper, bounded document, no LAN server), ADR-013 (Doors is Apache-2.0,
third-party code keeps its terms).

## Options

1. **kiwix-serve (libkiwix) on the unit, Browser pointed at localhost.** A
   permanent HTTP server (LAN exposure unless firewalled), libkiwix's
   dependencies (libmicrohttpd, pugixml, mustache, ICU, Xapian, libcurl) and
   Browser fetching from it. Rejected: the owner excluded a background server,
   and Browser would have to fetch from localhost, widening its boundary.
2. **A ZIM parser of our own.** The format is documented, but clusters are
   xz or zstd, entries redirect, titles and full text are indexed in
   embedded Xapian databases. Rejected: the owner asked for established ZIM
   support, and it would re-implement libzim.
3. **libzim in a helper of the Library's own, speaking Browser's protocol;
   Browser's screen in a Library mode (chosen).**
4. **libzim inside the shell.** Rejected for ADR-009's reasons: archive
   parsing, decompression and HTML of any archive are untrusted input, and
   libzim is GPL.

## Decision (proposed)

Option 3:

1. **pos-library** (tools/library/pos_library.cpp) is the only program that
   opens an archive. It links libzim 9.8.2 (reader only), zstd, liblzma, the
   HTML reader and the picture decoders, and **no HTTP client and no socket**.
   It is started with the first page and ends with the screen, exactly like
   pos-browser (the same session code: PR_SET_PDEATHSIG, close-on-exec,
   watchdog).
2. It answers Browser's line protocol (core/web/web_proto.h, two failure
   kinds added) with the same bounded documents, so the shell links nothing
   new: Browser's view and screen gain a **Library mode** (apps/browser,
   `struct browser_profile`), and the app is one file (apps/library).
3. **Addresses** are `https://library.invalid/<entry path>`; `.invalid` never
   resolves (RFC 6761). Links that leave the collection are marked not
   followable by the helper and refused by the view.
4. **Content is a file, not a build input**: the first `*.zim` in
   `$HOME/Library`, or `archive=` in `<config>/library.conf`. No download,
   no catalogue, no network.
5. **Search**: titles always; full text where libzim has Xapian and the
   archive has an index. The K230 build has no Xapian (see Consequences).
6. **Licences**: libzim (and Xapian, where built) are GPL-2.0-or-later, so
   pos-library as built is distributed under the GPL (GPL-3.0-or-later when
   combined with the Apache-2.0 web code, which is compatible with GPLv3
   only); it stays a separate program that the Apache-2.0 shell talks to over
   a pipe. Whether Doors ships a GPL helper is the owner's decision and a
   precondition of any release.

## Consequences

Done on the branch (docs/apps/LIBRARY.md has the evidence): the helper; WebP
decoding (libwebp, already in the image) for the helper only; the view's
Library mode with tests; the app behind `-DPOCKETOS_WITH_LIBRARY=ON`; build
scripts that put libzim and zstd in a prefix of their own; host validation
against a real Wikipedia archive; riscv64 builds of the helper and the DRM
shell with the pinned toolchain.

Needed before acceptance:

- the owner's licence decision on a GPL-2.0+ helper (and THIRD_PARTY
  entries, notices and legal-info for libzim and zstd);
- a Buildroot package for libzim (none exists in Buildroot 2025.02.1; zstd
  does) or a vendored build, i.e. a proposal under ADR-001;
- a longer use of the unit-B install by the owner (the focused device check
  passed, docs/validation/UNIT_B_LIBRARY.md).

## Evidence

- VERIFIED (host, 2026-10-29): the reading journey, search, pictures,
  links, bookmarks, failures and helper lifecycle against
  `wikipedia_en_climate-change_maxi_2026-10.zim` (SHA-256 checked against
  Kiwix's), also with no network interface and with no network system call
  (docs/apps/LIBRARY.md "Validation").
- VERIFIED (cross, 2026-10-29): libzim 9.8.2 without Xapian, zstd 1.5.7,
  pos-library and the DRM shell with the app build with the pinned Xuantie
  toolchain; the helper's shared libraries are all in the vendor target tree.
- DOCUMENTED: libwebp 1.5.0, libpng, libjpeg and xz are in the image
  (docs/legal/manifest.csv); Buildroot 2025.02.1 has zstd, Xapian 1.4.27 and
  ICU 73 packages and no libzim package; libzim's licence (`COPYING`, headers).
- VERIFIED (unit B, 2026-10-29, docs/validation/UNIT_B_LIBRARY.md): the
  helper's journey and the shell's reading journey by injected touch, a
  reboot, and the rollback; archive open 142 ms cold, articles 51-77 ms,
  pictures 143-382 ms per article, helper peak 18.7 MB. Left installed at the
  owner's request.
- ASSUMED: behaviour from a FAT32 USB drive; portrait on the unit.

Not decided here: full-text search on the K230 (Xapian 1.4.27 and ICU 73 are
Buildroot packages, but ICU's data alone is about 30 MB against a 600 MB root
filesystem with about 120 MB free); a launcher place and icon; more than one
collection; downloads.
