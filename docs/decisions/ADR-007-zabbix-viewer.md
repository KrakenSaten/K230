# ADR-007: A Zabbix monitoring viewer, and who talks to the server

Status: Proposed (experiment, branch `experiment/zabbix-dashboard`). Not
accepted, not merged. It asks the product owner for three decisions,
listed under "Decision" below.
Date: 2026-09-25
Deciders: product owner (final), AI engineering partner (author)

## Context

The owner wants to know whether a Doors unit can serve as a dedicated
terminal for an existing Zabbix server: dashboards, open problems and host
state at a glance, in portrait and landscape. It is a client only. Nothing
of Zabbix itself (agent, server, proxy) runs on the unit.

Constraints already binding:

- ADR-001: Linux with the pinned LILYGO BSP, BusyBox init, C11. ADR-002:
  apps are in-process LVGL screens that "never touch hardware", and services
  own resources. The shell is one LVGL loop and nothing on it may block; that
  is the owner's standing rule since Wave and Files.
- docs/ARCHITECTURE.md: "No other Doors component may persist a secret";
  netd's Wi-Fi store under ADR-003 is the one exception. A Zabbix API token
  is a secret.
- AGENTS.md: no toolchain, SDK or dependency change without a proposal.

What the image has (docs/apps/ZABBIX.md §1 lists each item with its
evidence):

- No browser or web engine of any kind. There is no WebKit, WPE, Qt, GTK,
  NetSurf, Lynx, X11 or Wayland. The only graphics stack is LVGL 9.5 on DRM.
  CONFIRMED in the build's `.config`.
- libcurl 8.12.1 with OpenSSL 3.4.1 and the Mozilla CA bundle are present,
  already in the image and its legal manifest. The build uses them for the
  curl tool only. CONFIRMED.
- cJSON 1.7.18 is already linked by every Doors service. CONFIRMED.
- There is no RTC. The wall clock reads 1970 until NTP answers, so a TLS
  certificate's dates cannot be checked before then. CONFIRMED
  (T-DISPLAY-K230.md).

What Zabbix offers (docs/apps/ZABBIX.md §3, from the official
documentation, 2026-09-25):

- A JSON-RPC API at `api_jsonrpc.php` covers everything a viewer needs:
  problems, triggers, hosts, interfaces, items.
- API tokens exist from 5.4. Tokens go in an `Authorization: Bearer` header
  from 6.4; the `auth` member of the body was removed in 7.2.
- The frontend's minimum supported width is 1200 px. It is a PHP application
  built on jQuery, D3 and JavaScript widgets.
- No API method returns an image. `chart*.php` accepts only the frontend's
  signed session cookie, never an API token. Scheduled reports are PDFs sent
  by email and need Chrome on the server.

## Options

### A. The normal Zabbix frontend in a browser on the unit

- Pro: every dashboard as the server defines it, with nothing to maintain on
  the Doors side.
- Con: there is no browser. Adding one means a web engine (WPE WebKit,
  Chromium) with its own graphics stack beside LVGL/DRM, tens to hundreds of
  MB on a 600 MB rootfs with about 120 MB free, and a JavaScript-heavy UI
  built for 1200 px on a 568 px-wide touch panel. It would also need a
  password-based web session, since tokens do not open the frontend.
- Rejected: impractical, and the heaviest option by an order of magnitude.

### B. Server-rendered images (chart.php, map.php, scheduled reports)

- Pro: graphs exactly as the server draws them.
- Con: the image endpoints need a signed frontend session cookie, obtained
  by scripting the login form (undocumented, and it breaks with MFA or SSO),
  and cannot use an API token. map.php returns JSON, not a picture. Reports
  are emailed PDFs. The images would still need decoding and scaling on the
  C908 (LVGL has lodepng and TJPGD), and they carry no touch targets.
- Rejected for v1. It could come back later as a single "graph of this item"
  view on top of option D, if the owner accepts a password login for it.

### C. A native viewer that calls the API from the shell (worker thread)

- Pro: one binary. No process boundary.
- Con: libcurl, OpenSSL and a JSON parser working on untrusted network input
  inside the shell's process; a crash there takes the panel down. The token
  sits in the shell's memory. A worker thread stuck in a resolver has to be
  joined when the app closes, which freezes the panel (the Files lesson).
- Rejected.

### D. A native viewer with an app-scoped helper process (pos-zabbix)

- Pro: this is the pattern Camera uses under ADR-006.
  - The helper exists only while the screen is open.
  - Every network wait happens in the helper; the shell only reads a socket
    that already has data.
  - curl, OpenSSL and cJSON never enter the shell; the shell links only the
    bounded model and a line protocol.
  - The token never enters the shell.
  - `PR_SET_PDEATHSIG` ends the connection if the shell dies. On a rotation
    restart (an exec, which that signal does not see) the shell closes the
    app first, and the socket is close-on-exec.
  - A watchdog kills a stuck helper, and the screen restarts it with backoff.
  - The bench tool (`pos-zabbix check`) and the app are one code path.
- Con: a second process while the screen is open, costing about 3 MB of
  resident memory for the helper. The model is copied through a line
  protocol.
- Chosen.

### E. A resident `zabbixd` service over pocketipc

- Pro: polls in the background, so alerts could reach the lock screen or the
  status bar without the app open. One connection for several consumers.
- Con: a permanent daemon, init script, supervisor entry and versioned
  `monitoring.*` contract, all before the owner has decided whether
  background monitoring is wanted at all. It would also poll the server
  while nobody is looking.
- Deferred. The helper's client layer (core/zabbix) is pure C with no
  process assumptions, so it can move into a service unchanged if that is
  ever decided.

## Decision (proposed)

1. **A native, read-only Zabbix viewer app with the helper of option D.**
   - The app only reads: `apiinfo.version`, `user.login` (for password
     setups), `problem.get`, `trigger.get`, `hostinterface.get`, `host.get`
     and `item.get`.
   - Nothing acknowledges, configures or writes (tests/zabbix_lint.sh holds
     this).
   - Supported servers are 6.0 LTS, 6.4, 7.0 LTS, 7.2 and 7.4. The request
     shape follows the version `apiinfo.version` reports.
2. **libcurl becomes a build dependency of the Doors package.** It was
   already in the image; the same arrangement was used for alsa-lib (Wave)
   and jpeg (Camera). No package is added, upgraded or reconfigured. HTTPS
   verifies the peer and the host name by default, against the system CA
   store or a `ca_file` of the owner's. Plain `http://` needs an explicit
   `allow_insecure_http=1`.
3. **A second secret-store exception to docs/ARCHITECTURE.md.** The API
   token (or a password, for servers where the owner cannot issue a token)
   is kept in `/var/lib/pocketos/zabbix/secret` with ADR-003's protections:
   - a 0700 directory and a 0600 file, written atomically with
     `O_EXCL|O_NOFOLLOW`;
   - refused when others can read it;
   - never in the world-readable `zabbix.conf`, never in argv, the
     environment or a log, and wiped from memory after use;
   - written only by `pos-zabbix set-secret`, which reads it from stdin.

   As with ADR-003, this is permission protection only: there is no
   encryption at rest.

Until the owner decides, all of this lives on the experiment branch only.

## Consequences

Needed now (done on the branch):

- core/zabbix: model, protocol, API, fake server, config, client and
  transport.
- tools/zabbix: `pos-zabbix` and the development-only `pos-zabbix-mock`.
- apps/zabbix: session, view and screen.
- Tests, the lint, docs/apps/ZABBIX.md, and DS §35 (PROPOSED).
- The package links libcurl (`ZABBIX_CURL=1`).

Useful soon:

- An owner-side read-only Zabbix user role that allows only the methods
  above. User roles can restrict the API to listed methods; the version
  that introduced this is UNVERIFIED. 8.0 (beta) makes API access opt-in
  per role, which is DOCUMENTED.
- A Doors launcher icon.
- A setup screen on the unit, if typing a URL on the touch keyboard is
  acceptable. The token is better pasted over SSH.

Future:

- Option E, if background alerts are wanted.
- Option B for single graphs.
- Acknowledging problems, which would be the first write and needs its own
  decision.

Risks:

- **Clock unset:** HTTPS fails until NTP has set the clock. The helper says
  so in words; it does not disable verification.
- **Large estates:** above 500 hosts or problems the lists are the most
  important 500, and the counts come from `countOutput` (exact), except
  unknown and maintenance, which are over the hosts read.
- **Stolen card:** the token is readable by anyone who takes the card out,
  the same as the Wi-Fi passphrases. Give the viewer a token of a read-only
  user with a short expiry.

## Evidence

- **VERIFIED on the host, 2026-09-25.** The helper's real libcurl transport
  was exercised against the mock server:
  - over HTTP;
  - over HTTPS with an untrusted certificate (refused), a `ca_file`
    (accepted), a wrong host name (refused) and `verify_tls=0` (accepted);
  - through every failure the fake can produce.

  See tests/zabbix_http_test.sh; the libcurl on this host is Ubuntu's
  7.81.0.
- **VERIFIED:** the riscv64 build.
  - `make all ZABBIX_CURL=1` against the SDK sysroot (libcurl 8.12.1)
    finishes with no warnings under `-Werror`.
  - pos-zabbix is 116 KB and links `libcurl.so.4`.
  - The DRM shell builds against the sysroot LVGL.
- **DOCUMENTED:** the Zabbix API facts are in docs/apps/ZABBIX.md §3, with
  the official URLs; some error strings are from the source only
  (UNVERIFIED as a stable API).
- **ASSUMED, not tried against a real server:**
  - `"suppressed": false` filters out suppressed problems; the parser drops
    any that come anyway;
  - `hostinterface.get` filters on `available`.
- **NOT RUN:** unit A (not reachable on 2026-09-25) and a real Zabbix server
  (no credentials).
