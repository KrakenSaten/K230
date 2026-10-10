# ADR-009: A browser without a browser engine

Status: Accepted (product owner, 2026-09-27): all decisions below, including
decision 6 as proposed (Browser in CONNECTIONS after Zabbix, with the
first-party globe icon). The acceptance came after:

- the unit A gate (docs/hardware/BROWSER_GATE.md, PASS on `301fadf`, the
  physical keyboard checked by the owner);
- the merge to master (`c7b7a8f`, PR #9).

It was proposed 2026-09-26 on branch `feat/browser-app`.
Date: 2026-09-26 (proposed), 2026-09-27 (accepted)
Deciders: product owner (final), AI engineering partner (author)

## Context

The owner asked for a Browser app: simple websites, links, an address
field, back, forward, reload and stop, HTTPS, readable text-oriented pages
and local admin pages - stable, small and responsive first, compatibility
last. "Not intended to compete with a desktop browser."

Constraints already binding:

- ADR-001: the pinned LILYGO BSP and SDK, Buildroot 2025.02.1, glibc, no
  toolchain or package upgrade without a proposal.
- ADR-002: first-party apps are in-process LVGL screens behind the App API;
  out-of-process apps with DRM master handoff are "a defined path, not a
  v0.1 deliverable", and the handoff is unproven on the Canaan DRM driver.
- ADR-007 (Zabbix) established, for the first app that reads the network,
  that untrusted network input is parsed in an app-scoped helper process and
  never in the shell (option D).
- The shell is one LVGL loop and nothing on it may block (the owner's
  standing rule since Wave and Files).

What the image has (docs/apps/ZABBIX.md §1, re-checked for this ADR in
docs/apps/BROWSER.md §1):

- No web engine of any kind (no WebKit/WPE, Qt WebEngine, Chromium, NetSurf,
  Dillo, Links, Lynx), and no windowing system: LVGL 9.5 on DRM is the only
  graphics stack. CONFIRMED in the build's `.config`.
- libcurl 8.12.1 with OpenSSL 3.4.1 and the Mozilla CA bundle; libjpeg 9;
  libpng 1.6 with headers in the sysroot (for OpenCV); zlib. CONFIRMED.
- 1 GiB RAM (about 930 MB available), a 600 MB rootfs with about 120 MB
  free. CONFIRMED.
- Fonts: IBM Plex bitmaps, Latin-1 and some punctuation; no CJK, no emoji.
  CONFIRMED (`tools/design/gen_fonts.sh` ranges).

## Options

### A. A real engine: WPE WebKit (cog), Chromium/Electron, Qt WebEngine

- Pro: the web as it is, JavaScript included.
- Con: a second graphics stack beside LVGL (EGL/Wayland or a DRM
  backend), tens to hundreds of MB against 120 MB free, a GPU-less C908
  running JavaScript and layout, and DRM master handoff that is unproven.
  Qt WebEngine is Chromium and needs Qt. None is in Buildroot's image today;
  each is a platform change, not an app.
- Rejected: the owner excluded these, and the numbers agree.

### B. A small engine with its own frontend: NetSurf (framebuffer), Links2 -g, Dillo

- Pro: real HTML and CSS layout (NetSurf especially), small by browser
  standards (NetSurf about 3-5 MB with its libraries).
- Con: each draws itself: NetSurf's framebuffer frontend and Links' graphics
  mode need the display (a DRM/fbdev handoff the shell cannot yet do - ADR-002
  point 6), bring their own toolkit, fonts and keyboard handling, and would
  not look or behave like Doors. Dillo needs FLTK and X11. None is in the
  image; each adds packages (NetSurf: libcss, libdom, hubbub, libnsfb,
  libnsgif, libnsbmp, libutf8proc ...) and its own TLS/HTTP stack
  integration to review. Text-mode Links/Lynx need a terminal Doors does not
  have.
- Rejected for v1: it is the out-of-process app path ADR-002 has not built,
  plus new packages, for rendering that the priorities rank below stability
  and memory. It stays the obvious step if CSS layout is ever wanted (see
  Consequences).

### C. A first-party reader: fetch and parse in a helper, draw natively (chosen)

- A helper process, `pos-browser`, owns everything that touches the
  network or untrusted bytes: libcurl (HTTP, HTTPS, redirects under
  Browser's own rules, cookies in memory), a bounded one-pass HTML reader
  that turns a page into a flat document of blocks and text runs, and
  libjpeg/libpng decoding pictures to RGB565 at display size.
- The shell links only the bounded document, the line protocol, the address
  rules, the back/forward list and the remembered state. It draws the
  document with LVGL (labels, span groups for links, images) in the Design
  System, a few milliseconds at a time.
- Pro: no new package and no new graphics stack (libcurl, OpenSSL, libjpeg,
  libpng are already in the image; libpng becomes a build dependency only).
  About 110 KB of helper and 70 KB of shell code on riscv64. Everything
  untrusted is out of the shell; a crash, a hang or a hostile page costs the
  helper, which the app restarts. It looks and behaves like Doors: the
  keyboard rules, both orientations, the theme, the hint. Every limit is
  explicit and tested.
- Con: it is a reader, not a layout engine. No CSS (so no columns, no
  positioning), no JavaScript, no forms, no GIF/WebP/SVG. Pages that need
  JavaScript (single-page apps, bot challenges) show their fallback text or
  nothing useful. That is stated on the start page and in docs/apps/BROWSER.md.

### D. As C, but in the shell (a worker thread)

- Rejected for ADR-007's reasons, which are stronger here: an HTML parser
  and two image decoders on arbitrary internet input inside the panel's
  process.

## Decision

Option C, with these fixed points:

1. **pos-browser is the only program that fetches a page.** It is started
   by the Browser app with the first page (not at open), lives exactly as long
   as the screen, leaves with the shell (`PR_SET_PDEATHSIG`, close-on-exec),
   and is killed by the app's watchdog when it stops answering (no hello in
   3 s; silence past 60 s while loading; a STOP not answered in 4 s). The
   app restarts it for the next page, at most four failures a minute.
2. **Only http and https are fetched**, for the request and every redirect,
   enforced before libcurl (web_url) and in libcurl (`CURLOPT_PROTOCOLS_STR`).
   `file:`, `data:`, `javascript:` and the rest are refused as addresses and
   shown, never followed, as links. An address typed without a scheme becomes
   https; http is used only when written, and after an https connection
   failure (not a certificate failure) only as an offer the person has to take. An https page never
   redirects to http.
3. **TLS verification is always on** (peer and host name, the system CA
   store). There is no switch to turn it off; a failure is an error page that
   says so, and an unset clock is named as the cause when it is one.
4. **Everything is bounded**: 2 MB of page, 256 KB of text, 800 blocks, 500
   links, 16 pictures of at most 560 K pixels each and 1.5 M pixels per page,
   32 back/forward entries, 12 recent pages, 24 bookmarks. Past a limit a page
   is cut and says so.
5. **Persistent state is small and private**: home, last page, recent pages
   and bookmarks in `/var/lib/pocketos/browser/state` (versioned text, 0600
   in a 0700 directory, atomic). No cookies, passwords, form data or page
   content are ever written.
6. **Browser is in the shell by default and on the launcher** in CONNECTIONS
   after Zabbix, with a first-party globe icon (network colour). The owner
   confirmed the place and the icon on 2026-09-27.

## Consequences

Needed now (done on the branch):

- core/web (url, doc, html, proto, history, store, fetch, fake, image),
  tools/browser (pos-browser), apps/browser (session, view, screen).
- Tests: unit, hostile-input and fuzz rounds for the reader and the
  decoders, the protocol, the view, the helper lifecycle against the real
  helper, libcurl against local HTTP/HTTPS servers, the shell in both
  orientations, a lint for the boundaries; all under ASan/UBSan
  (`make browser-san-test`).
- The package passes `BROWSER_CURL=1 BROWSER_IMAGES=1` and depends on
  libpng (already in the image).

Useful soon:

- Hardware gate on unit A (docs/apps/BROWSER.md §11).
- GET forms with one text field (search boxes of sites the owner uses),
  which is cheap on top of this design.
- A text-size setting; "find in page".

Future:

- If CSS layout or JavaScript is ever wanted: NetSurf's core (libcss/libdom)
  behind the same helper, drawing into a buffer the shell shows, or option B
  once ADR-002's out-of-process path exists. Either is a new ADR.

Risks:

- LV_USE_SPAN in the vendor's `lv_conf.h` is ASSUMED on (LVGL's default); the
  device build fails at link time if it is off. Check before the first image.
- Page building cost on the C908 is measured on a PC only (125 ms for 800
  blocks on x86-64 including chunked layout); the gate must measure it.
- Many sites need JavaScript; Browser is useful for text pages, documentation
  and local admin pages, and says what it cannot do.

## Evidence

- VERIFIED (host, 2026-09-26): all first-party code builds and the Browser
  suites pass with `-Werror`, and under ASan/UBSan; libcurl against local
  HTTP/HTTPS servers including refused certificates, redirects and cut
  answers.
- VERIFIED (cross, 2026-09-26): `make all` for riscv64 (Ubuntu's
  riscv64-linux-gnu-gcc 13 against Ubuntu noble riscv64 libraries, not the
  Xuantie toolchain and SDK sysroot), and the DRM shell with LVGL at the
  pinned commit; the riscv64 helper passes its lifecycle and HTTP suites under
  qemu-user.
- DOCUMENTED: libraries in the image (ZABBIX.md §1, CAMERA_PLATFORM_RESEARCH.md).
- ASSUMED: vendor `lv_conf.h` has LV_USE_SPAN 1; performance on the C908.
- Not verified: anything on unit A.

## Amendment 1 (ACCEPTED 2026-10-10): RIFT's basemap through pos-browser

Status: Accepted (product owner, 2026-10-10) for the implemented,
user-enabled OpenStreetMap basemap, after the unit B check below. Proposed the
same day on branch `feat/rift-basemap` (PR #82), with the direction approved
beforehand: RIFT MAP's "fetches nothing" rule replaced by isolated,
user-enabled basemap fetching, OpenStreetMap's standard tiles under its usage
policy, off by default.

Context: RIFT's MAP placed the nodes on a bare graticule and fetched nothing
(docs/apps/RIFT.md, "MAP"; `tests/rift_lint.sh` held it). The owner asked for
a real geographic basemap behind the nodes, with online tiles, a bounded
cache, offline use of tiles already seen and visible attribution - and
nothing untrusted in the shell.

Decision: the tiles come through **this ADR's helper, in a mode of its own**:

- `pos-browser tiles --cache DIR --out DIR --url TEMPLATE`
  (tools/browser/pos_browser_tiles.c, core/web/web_tiles.h): the same binary,
  the same fetcher (libcurl, https only, TLS always verified, the same
  timeouts and byte caps) and the same PNG/JPEG decoder; the shell gets only
  256 x 256 RGB565 pixels in files of names it checks, and links none of the
  fetching, caching or decoding (`tests/browser_lint.sh`).
- **OpenStreetMap's standard tile layer** (`tile.openstreetmap.org`), under
  the OSMF tile usage policy as read on 2026-10-10: a User-Agent of the app's
  own (`DOORS-RIFT/<version> (...; +repository URL)`), never a browser's;
  the server's Cache-Control / Expires honoured, expired tiles revalidated
  with If-None-Match / If-Modified-Since, seven days kept when an answer says
  nothing; **no bulk download, prefetch or offline pack**: only the tiles
  that intersect the map area on screen, nearest the centre first, one at a
  time, and none at all while the basemap is off or MAP is not shown;
  "(c) OpenStreetMap contributors" on the map whenever the basemap is on.
- **Bounded**: a 32 MB / 2048-tile disk cache under
  `/var/lib/pocketos/rift/tiles` (0700, least recently shown pruned first,
  nothing written with less than 64 MB free), at most 32 decoded tiles (4 MB)
  in the shell, 256 KB a tile fetched; after a network failure nothing is
  asked for 30 s, after a refusal by the server (403, 429, 5xx) or a TLS
  failure for 60 s.
- **Lifetime**: the helper runs only while MAP is shown with BASEMAP on; off,
  another section, leaving RIFT and CLOSE RIFT stop it (quit, SIGTERM,
  SIGKILL, bounded) and remove its directory; it leaves with the shell
  (`PR_SET_PDEATHSIG`) and on an exec. A view left before its tiles came
  cancels them; an answer is taken only under the generation in force.
- **Offline**: tiles seen before are shown from the cache with no network
  (marked stale once expired), the graticule where there is no tile, and the
  map says which: off, loading, offline, clock not set, a server error,
  unavailable.

Why this and not another helper: everything the tiles need - a hardened
fetcher, a bounded decoder, the pixel hand-over and its checks - is already
here and already reviewed; a second binary would duplicate it. Why not in the
shell: this ADR's reasons, unchanged.

Not decided here: another tile provider (Kartverket was examined; its terms
for caching are not stated and need its confirmation), offline packs, vector
maps. Each would be a new amendment.

Evidence:

- VERIFIED (host, 2026-10-10): the helper's cache and freshness rules
  (tests/web_tiles_test.c), the shell-side client against the real helper on
  the fake network (tests/rift_tiles_test.c), the Browser suites unchanged,
  all with `-Werror` and under ASan/UBSan; MAP in both orientations in the
  simulator with fake tiles (tests/rift_app_test.c).
- VERIFIED (cross, 2026-10-10): pos-browser and the DRM shell with the pinned
  Xuantie toolchain and SDK sysroot (see the PR).
- VERIFIED (unit B, 2026-10-10, shell e9469ab + pos-browser 3f7e3f2 over the
  v0.3.6 card with the af80d7b DeskBuddy shell's tree): BASEMAP off starts no
  helper and fetches nothing; on, real OpenStreetMap tiles load under the 102
  located nodes, the attribution shows, and the owner confirmed node
  positions, pan, zoom and FIT; leaving MAP stops the helper and coming back
  serves seen tiles from the cache (logged "12 shown, 0 asked of the
  server"); with the tile server blackholed the map says OFFLINE · SAVED
  TILES ONLY and shows seen tiles and the graticule; CLOSE RIFT leaves no
  helper, directory or picture file; a Browser page with pictures loads.
  One defect was found and fixed (e9469ab): the shell's watchdog counted an
  idle helper as silent and killed it at the first pan after more than
  45 s idle; the fix was rechecked on unit B (a pan after about 3 min idle,
  tiles at once, no kill) and has a regression check in
  tests/rift_tiles_test.c (a request after an idle spell is answered by the
  same helper; fails without the fix).
- Not tested on hardware: Wi-Fi fully off (the "offline" check blackholed
  only the tile server's addresses, so the helper saw timeouts, not "no
  route"); an unset clock ("CLOCK NOT SET", no request made). Both are
  covered on the host only (tests/web_tiles_test.c, tests/rift_tiles_test.c).
