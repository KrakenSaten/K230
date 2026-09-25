# Zabbix: a monitoring terminal for an existing Zabbix server

**Status: EXPERIMENT.** This lives on branch `experiment/zabbix-dashboard`, which is not merged. The architecture is in ADR-007 and the screens are DS §35; both are PROPOSED.

This is a read-only client. The unit shows what a Zabbix server already knows:

- the worst open severity and the counts;
- the open problems, most severe first;
- the hosts, with the ones that need attention first;
- one host's problems and latest values;
- the state of the connection itself.

It acknowledges nothing, configures nothing and runs no Zabbix component.

Contents:

1. What the platform offers
2. The approaches, compared
3. The Zabbix API it uses
4. Architecture
5. Connection, refresh and staleness
6. Limits
7. Security
8. The screens
9. The fake server and the mock
10. Setting it up on a unit
11. Validation
12. Open items and next steps

---

## 1. What the platform offers

Evidence classes follow AGENTS.md.

- **CONFIRMED:** seen in the build that makes the image.
- **LIKELY:** strongly suggested, not seen directly.
- **UNKNOWN:** not established.

| Area | Finding | Class | Evidence |
| --- | --- | --- | --- |
| Networking | Ethernet (r8152, DHCP by ifupdown) and Wi-Fi (netd, wpa_supplicant, udhcpc) | CONFIRMED | `k230_pocketos_defconfig:16`, `services/netd/wifi_mgr.c` |
| DNS | udhcpc's default.script writes `/etc/resolv.conf` | CONFIRMED | `output/.../target/usr/share/udhcpc/default.script` |
| HTTP client | libcurl 8.12.1 and the `curl` tool; BusyBox wget is HTTP only | CONFIRMED | `.config:2843-2849`; `busybox .config:1009` |
| HTTPS/TLS | libcurl on OpenSSL 3.4.1 with the Mozilla CA bundle in `/etc/ssl/certs`. No mbedTLS, wolfSSL or GnuTLS | CONFIRMED | `curl_config.h:879` `USE_OPENSSL`; `target/etc/ssl/certs/ca-certificates.crt` |
| Time for TLS | No RTC: the wall clock reads 1970 until sntp/ntpd answer | CONFIRMED | `T-DISPLAY-K230.md:31`; `KNOWN_ISSUES.md` |
| NTP over Wi-Fi only | S48sntp runs before S55netd; with Wi-Fi only, ntpd retries later | LIKELY | init script order |
| JSON | cJSON 1.7.18, linked by every Doors service; json-c also present | CONFIRMED | `.config`, `libcjson.so.1.7.18` |
| LVGL | 9.5.0-dev from the vendor package | CONFIRMED | `lv_version.h` |
| LVGL image decoders | lodepng and TJPGD on, libpng and libjpeg-turbo off | CONFIRMED | device `lv_conf.h:963-977` |
| Snapshot on the device | off in the device LVGL; the simulator has it | CONFIRMED | device `lv_conf.h:1077` |
| Fonts | IBM Plex bitmaps compiled in (Latin-1 and a little more; no CJK); FreeType on, but no TTF files in the image | CONFIRMED | `ui/pocketui/fonts`, `lv_conf.h:1005` |
| Scrolling, timers | LVGL flex, scrolling containers and `lv_timer`, used by every app | CONFIRMED | apps/* |
| Persistent settings | `/etc/pocketos/*.conf` and `/var/lib/pocketos/<app>`, with `POCKETOS_*_DIR` overrides | CONFIRMED | `core/pocketpaths.h` |
| Secret storage | only netd's 0700/0600 Wi-Fi store (ADR-003); no encryption at rest, no keyring | CONFIRMED | ADR-003; `docs/ARCHITECTURE.md` |
| Browser or webview | **none**: no WebKit, WPE, cog, Qt, GTK, NetSurf, Lynx, Links, Dillo, Chromium or Firefox | CONFIRMED | `.config` (all "is not set" or absent) |
| Windowing | no X11, Wayland, DirectFB or SDL on the device; LVGL on DRM is the only graphics stack | CONFIRMED | `.config` |
| RAM | 1 GiB; MemAvailable about 930 MB at idle (512 MB CMA counted in it); doors-shell about 13 MB RSS | CONFIRMED | `T-DISPLAY-K230.md:30`, gate records |
| Storage | 600 MB rootfs with about 120 MB free; no data partition | CONFIRMED | `FILES_GATE.md:439` |

**Can the unit render a real web page? No.**

- There is no engine to render one, and no windowing system an engine could
  run in.
- Adding one would be a new graphics stack beside LVGL/DRM, and larger than
  the free space on the rootfs.
- Even with one, the Zabbix frontend's minimum supported width is 1200 px
  (§3). This panel is 568 px wide.

## 2. The approaches, compared

| | A. Frontend in a browser | B. Server-rendered images | C. Native viewer (chosen) | C'. Plain status viewer |
| --- | --- | --- | --- | --- |
| Feasible on this image | no: no engine, no windowing | partly: needs the web session cookie | yes | yes |
| RAM | a web engine, hundreds of MB | image buffers (1 MB per 568x900 RGB565) | helper about 3 MB, app about 150 KB | about the same as C |
| CPU | JavaScript layout on a C908 | PNG decode and scale per refresh | a JSON parse every 30 s (ms) | the same |
| Complexity | a port, not an app | medium: login scraping, decoding | medium (done) | low |
| 568x1232 usability | frontend needs 1200 px | fixed pictures, no targets | built for it | built for it |
| Portrait and landscape | frontend reflow only | re-render server-side | both shapes | both shapes |
| Authentication | password web session | password web session (tokens do not work) | API token (or password) | API token |
| TLS | the engine's | libcurl | libcurl, verified | libcurl |
| Refresh performance | full page | an image per graph | about 17 KB of model per refresh | less |
| Maintenance | follow the frontend | undocumented endpoints | the documented API | the documented API |
| Zabbix 6.0 to 7.4 | yes | chart.php params unstable | yes, by version switch | yes |

- **C'**, the "simpler HTTP/JSON status viewer", is C with fewer screens. The
  cost of C over C' is the host detail and the lists, which is where a
  monitoring terminal earns its place. C' is a subset, so C was built.
- **B** remains possible later as one "graph of this item" view, if a
  password login is acceptable.
- A resident background service (ADR-007 option E) is deferred.

## 3. The Zabbix API it uses

**Current versions** (zabbix.com life-cycle page, 2026-09-25):

- **7.0** is the current LTS (7.0.31).
- **7.4** is the current standard release (7.4.15).
- **6.0** LTS is in limited support.
- **8.0** LTS is in beta (8.0.0beta2).

The viewer speaks to 6.0 and later. It asks `apiinfo.version` first and
shapes every request from the answer.

**Request envelope.** `POST <frontend>/api_jsonrpc.php`, with
`Content-Type: application/json-rpc`, carrying:

```json
{"jsonrpc":"2.0","method":"...","params":{...},"id":N}
```

- A result comes back as `{"jsonrpc":"2.0","result":...,"id":N}`.
- An error comes back as `{"jsonrpc":"2.0","error":{"code":...,"message":"...","data":"..."},"id":N}`.
- Numbers arrive as strings, and the parser accepts both forms.

| Method | Used for | Parameters (as sent) | Versions |
| --- | --- | --- | --- |
| `apiinfo.version` | the version switch, liveness | `[]`, **never** with credentials (7.4 refuses a Bearer header on it) | all |
| `user.login` | password setups only | `username`, `password`; the old `user` parameter was removed in 6.4; sent without any credential (the server refuses one) | 5.4+ |
| `user.logout` | password setups: ending the session when the screen closes | `[]`, with the session as the credential | all |
| `problem.get` | open problems | `output` (eventid, objectid, clock, name, severity, acknowledged, suppressed), `source` 0, `object` 0, `suppressed` false, `sortfield` "eventid", `sortorder` DESC, `limit` 500; `hostids` for one host | 7.0+ validates parameters strictly; this set is valid |
| `problem.get` + `countOutput` | exact totals past the limit | the same filters, with `severities: [s]` or `acknowledged: false` | all |
| `trigger.get` | the hosts of problems (problem.get returns none) | `triggerids`, `output` ["triggerid"], `selectHosts` ["hostid","name"] | all |
| `hostinterface.get` | which hosts are down | `output` ["hostid"], `filter` {available: 2}, `limit` 500 | 5.4+ (availability moved to interfaces) |
| `host.get` + `countOutput` | the total monitored | `monitored_hosts` | all |
| `host.get` | hosts with availability | `output` (hostid, host, name, maintenance_status, and active_available only from 6.2), `selectInterfaces` (type, main, available), `hostids` (the down hosts and the problem hosts first), then `monitored_hosts`, `sortfield` name, `limit` 500 | 6.0+ |
| `item.get` | one host's latest values | `hostids`, `output` (itemid, name, key_, lastvalue, lastclock, units, value_type), `monitored`, `sortfield` name, `limit` 200 | all |

**Authentication**, by version:

- **6.4 and later** (7.0 LTS, 7.2, 7.4, 8.0): `Authorization: Bearer <token>`.
  A `user.login` session id is sent the same way.
- **6.0 and 6.2**: the `auth` member of the body.
- **7.2 and later** refuse the `auth` member outright.
- API tokens exist from 5.4. They are preferred, and are the default
  (`auth=token`).

**User and password** (`auth=password`), as the 7.0 documentation and source
describe it (DOCUMENTED; VERIFIED in the 7.0 source where marked):

- `user.login` returns a session id, which is then sent exactly like a token
  (Bearer from 6.4, `auth` member before). The documentation says so for 7.0.
- The documentation asks that a session be ended with `user.logout`, "to
  prevent the generation of a large number of open session records". The
  helper logs out when the screen closes and after `pos-zabbix check`.
- A wrong password and a blocked account give the same text. Zabbix blocks
  a user after `Login attempts` failures (5 by default) for `Login blocking
  interval` (30 s by default), and keeps counting failures until a login
  succeeds. So a refused password is **never retried on its own**; REFRESH
  NOW or reopening the app tries it once.
- A session ends on the server after the user's auto-logout time, or
  when another client logs the user out. The next request then answers
  "Session terminated, re-login, please." (VERIFIED, 7.0 source) and the
  helper logs in again, silently.
- `user.login` is not available to users in a group with multi-factor
  authentication (DOCUMENTED, 7.0). Such an account needs an API token.
- The session id is kept in the helper's memory only, never on disk.

**Errors.** They are classified by code and text (the strings are from the
source, UNVERIFIED as stable).

| Text | Classified as | Effect |
| --- | --- | --- |
| "Not authorized." (7.x) or "Not authorised." (6.0) | AUTH | AUTH_FAILED |
| "API token expired." | AUTH | AUTH_FAILED |
| "Session terminated, re-login, please." | AUTH | re-login, silently, for password setups (a session that has not yet served a request is not renewed); otherwise AUTH_FAILED |
| "Incorrect user name or password or account is temporarily blocked." | AUTH | AUTH_FAILED, no automatic retry (account lockout) |
| `No permissions to call "x.y".`, unknown parameters, anything else | API | retry with backoff |

**Behaviour checked in the Zabbix source** (not yet against a running
server):

- `"suppressed": false` leaves suppressed problems out: in 7.0 through a
  left join on `event_suppress` with `IS NULL`, in 6.0 through `NOT EXISTS`
  (VERIFIED in source). The parser also drops any that arrive anyway.
- `hostinterface.get` applies `filter` to the `interface` table, and
  `available` is a column of it (VERIFIED in the 7.0 source and schema).
- `lastvalue` is only filled within the frontend's "Max history display
  period" (24 h by default; DOCUMENTED). Items without a value are left out.

**Sources:**

- https://www.zabbix.com/documentation/current/en/manual/api
- https://www.zabbix.com/documentation/7.0/en/manual/api/reference/problem/get
- https://www.zabbix.com/documentation/7.0/en/manual/api/reference/user/login
- https://www.zabbix.com/documentation/7.0/en/manual/api/reference/user/logout
- https://www.zabbix.com/documentation/7.0/en/manual/web_interface/frontend_sections/users/authentication
- the Zabbix 7.0 and 6.0 frontend source (the API classes and the database schema)
- https://www.zabbix.com/documentation/7.0/en/manual/api/reference/host/object
- https://www.zabbix.com/documentation/6.4/en/manual/api/changes
- https://www.zabbix.com/documentation/7.2/en/manual/api/changes
- https://www.zabbix.com/life_cycle_and_release_policy

**Images from the server** (option B) are not reachable with an API token:

- `chart.php` and `chart2.php` authenticate with the frontend's signed
  `zbx_session` cookie only.
- No API method returns a picture.
- Scheduled reports are PDFs sent by email and need Chrome on the server.

## 4. Architecture

```
 doors-shell (LVGL thread)                              pos-zabbix (helper process)
 ┌───────────────────────────────┐   socketpair,      ┌─────────────────────────────────┐
 │ apps/zabbix/zabbix_app.c      │   TAB-separated    │ tools/zabbix/pos_zabbix.c       │
 │   screen, pooled rows         │   lines            │   session loop, commands        │
 │ apps/zabbix/zabbix_view.c     │ <───────────────── │ core/zabbix/zbx_client.c        │
 │   model + words (no LVGL)     │   hello/config/    │   state machine, schedule       │
 │ apps/zabbix/zabbix_session.c  │   state/sets       │ core/zabbix/zbx_api.c (cJSON)   │
 │   spawn, watchdog, MSG_DONTWAIT│ ─────────────────> │ core/zabbix/zbx_config.c        │
 │ core/zabbix/zbx_model.c       │   refresh, detail, │ core/zabbix/zbx_http_curl.c ────┼──> HTTPS ──> Zabbix
 │ core/zabbix/zbx_proto.c       │   scenario, quit   │ core/zabbix/zbx_fake.c (demo)   │    api_jsonrpc.php
 └───────────────────────────────┘                    └─────────────────────────────────┘
```

**The shell** links the bounded model and the line protocol, nothing more.

- It has no JSON parser for server data, no curl, no OpenSSL and no token
  (tests/zabbix_lint.sh).
- It reads the helper's socket only with `MSG_DONTWAIT`, from a 100 ms LVGL
  timer.
- The one wait is destroy() giving the helper 300 ms to leave before
  SIGKILL.

**The helper** lives exactly as long as the screen.

- It leaves on `quit`, on end of input, on SIGTERM, and through
  `PR_SET_PDEATHSIG` when the shell dies.
- A rotation restart is an exec, which that signal does not see. The shell
  closes the app before it, and the socket is close-on-exec, so the helper
  reads end of input.
- It blocks only inside one request, bounded by `timeout_s`.
- Its socket stays blocking. Commands are read with `MSG_DONTWAIT` per call,
  so a burst of a few hundred lines waits for the app instead of failing.
  The large-estate app test found this bug; it is pinned by
  tests/zabbix_session_test.c.

**The line protocol** is core/zabbix/zbx_proto.h.

- Every text field is cleaned: no TAB, no newline, no control character,
  cut at a UTF-8 boundary to the model's size.
- A set (problems, hosts, detail) replaces the previous one only when its
  end line arrives with the same sequence number and count. A helper that
  dies halfway leaves the last complete set on screen.

**Watchdog and restart.**

- The first line must arrive within 3 s.
- While the helper says it is busy, 120 s of silence kills it. That is far
  beyond any request timeout; it is meant for a resolver stuck in the
  kernel.
- A helper that stops is restarted after 2 s, doubling to 30 s. The data
  stays, marked stale, with a banner.

## 5. Connection, refresh and staleness

**States** (core/zabbix/zbx_client.h):

```
UNCONFIGURED ─── stays; OVERVIEW shows how to set up, and TRY THE DEMO
CONNECTING ─ apiinfo.version (+ user.login) ─> ONLINE
ONLINE ─ any failure but AUTH ─> RETRYING ─ after 5,10,20,40,80,120,120… s ─> CONNECTING
any ─ AUTH ─> AUTH_FAILED ─ token: after 300 s; password: never on its own; either: at once on REFRESH ─> CONNECTING
```

**Failures:**

- A password session that ends on the server is logged in again at once,
  silently, as often as the server ends it. A session refused before it has
  served a single request is a real failure.
- A refused password is never tried again on its own, because every try
  counts towards Zabbix's account lockout. The banner says so: `Access
  refused: check the user and password · REFRESH NOW to try again`.
- A refused token is tried again after 300 s; a token does not lock the
  account.
- DNS, connect, timeout, TLS and HTTP failures drop the connection. The next
  try asks the version again.
- API errors, malformed answers and oversized answers keep it. The server is
  there; what it said was wrong.

**Refresh** (defaults, configurable within the bounds in §6):

- **Problems**, and the overview built from them: every 30 s.
- **Hosts**: every 60 s. After each problem refresh the kept hosts are
  re-counted, so per-host problem counts follow the problems.
- **The open host's detail**: when opened, then with every problem refresh.
- **REFRESH NOW** (STATUS): everything at once, at most every 5 s. In
  RETRYING or AUTH_FAILED it tries at once.
- **Pacing**: one request at a time. A healthy round is 7 requests (version
  only when reconnecting), plus 8 count requests only when there are more
  than 500 open problems.

**Stale** (apps/zabbix/zabbix_view.h):

- **When:** a set is stale when the helper is not running, when the last
  attempt failed (RETRYING, AUTH_FAILED, or CONNECTING after a failure), or
  when it is older than twice its interval plus 15 s.
- **What it keeps:** stale data stays on screen and is never blanked.
- **The banner** says why, when the next try is, and how old the data is.
  Example: `Server not reachable · retry in 6s · showing data from 32s ago`.
- **Ages:** problem ages are the server's clock (the HTTP `Date` header at
  the refresh) plus the monotonic time since, so they are right even while
  the unit's own clock still reads 1970.

## 6. Limits

| What | Limit | Where |
| --- | --- | --- |
| Problems kept and shown | 100, most severe first | `ZBX_PROBLEM_MAX` |
| Problems fetched per refresh | 500, newest first; totals exact via countOutput | `ZBX_PROBLEM_FETCH` |
| Hosts kept and shown | 200, attention first | `ZBX_HOST_MAX` |
| Hosts fetched per refresh | 500: down hosts, then problem hosts, then by name | `ZBX_HOST_FETCH` |
| Host detail | 20 problems, 12 values (of 200 items read) | `ZBX_DETAIL_PROBLEM_MAX`, `ZBX_ITEM_MAX` |
| Strings | host 63, problem 127, item 63, value 39, units 15 bytes, cut at UTF-8 boundaries with "..." | `zbx_model.h` |
| Answer size | 2 MiB, else TOO_LARGE, without parsing | `ZBX_RESPONSE_MAX` |
| Protocol line | 1024 bytes, else the helper is killed as broken | `ZBX_LINE_MAX` |
| Request timeout | 10 s (3..30); connect 5 s | `timeout_s`, `ZBX_CONNECT_TIMEOUT_MS` |
| Refresh | problems 30 s (15..600), hosts 60 s (30..3600) | `refresh_s`, `hosts_s` |
| Simultaneous requests | 1 | the helper loop |
| Redirects | none followed (a 3xx is an error) | `CURLOPT_FOLLOWLOCATION 0` |
| App memory | model 57 KB, session 57 KB, rows created on demand up to the limits | measured with `sizeof` on x86-64 |
| Helper memory | 2.2 MB peak RSS (demo), 3.0 MB (1 200 problems, 1 500 hosts) | measured on the host (x86-64, glibc) |

Nothing grows with the server. Every array is fixed, and the rows of the
lists are pools that are reused. The 400-round refresh-and-reconnect soak
runs under ASan and LSan (`make zabbix-san-test`).

## 7. Security

**Configuration**, in `/etc/pocketos/zabbix.conf`:

- `url`, `label`, `auth`, `user`, `verify_tls`, `ca_file`,
  `allow_insecure_http`, `refresh_s`, `hosts_s`, `timeout_s`, `mode`,
  `scenario`.
- It is world-readable like settings.conf, and holds **no secret**.
- A `token=` or `password=` line in it is refused, and says where the secret
  belongs.

**The secret** is `/var/lib/pocketos/zabbix/secret`, containing
`token=...` or `password=...`.

- The directory is 0700 and the file 0600, written atomically (temp file,
  fsync, rename, directory fsync) with `O_EXCL|O_NOFOLLOW`.
- A secret file others may read, or one reached through a symlink, is
  refused.
- It is written only by `pos-zabbix set-secret token|password`, which reads
  one line from stdin. It never passes through argv, the environment or a
  shell history.
- A token must be one word of printable ASCII. A password is kept byte for
  byte, spaces (at its ends too), UTF-8, `=` and `#` included; only a CR or
  LF is refused, since the file holds one line.
- `pos-zabbix clear-secret` removes it.

**Handling the password.** Everything said of the token below holds for the
password and for the session id `user.login` returns. The password goes only
into the `user.login` body, which is wiped after the request. The session id
lives in the helper's memory and is ended with `user.logout` (§3).
tests/zabbix_http_test.sh checks that neither the password nor the token is
in any log, and that the password is not in the running helper's
`/proc/PID/cmdline` or `/proc/PID/environ`.

**Handling the token.**

- It is never logged. tests/zabbix_http_test.sh greps the logs.
- It is never shown: STATUS says "API token (never shown)".
- It never enters the shell, since the helper reads it.
- It goes only into the Authorization header of authenticated requests, and
  never on `apiinfo.version` or `user.login`.
- The header buffer is wiped after each request, and so are the request
  bodies (`auth` member, password) and the answers.

**TLS.**

- HTTPS verifies the peer and the host name, against the system store or
  `ca_file`.
- `verify_tls=0` is possible, and STATUS shows it as a warning.
- `http://` is refused unless `allow_insecure_http=1`, and STATUS then shows
  "None: plain http://" as an error.
- A URL carrying `user:password@`, a query or a fragment is refused.
- **An unset clock:** until NTP has run, a certificate cannot be checked.
  The helper reports it as "device clock not set: the certificate cannot be
  checked" and does not weaken verification.

**Limitations**, stated plainly:

- There is no key store and no encryption at rest (the same as ADR-003's
  Wi-Fi passphrases). Every Doors process runs as root, and anyone holding
  the card can read the token.
- **Recommended:** a dedicated Zabbix user with read-only access to the
  wanted host groups, a role limited to the methods in §3, and a token with
  an expiry.
- ADR-007 asks the owner to accept this as the second exception to the
  "no secret" rule in docs/ARCHITECTURE.md.

## 8. The screens

The app is fullscreen (DS §30.8, NONE). The shell's header carries BACK and
a hint that says **SIMULATED** whenever the data comes from the fake. There
are four tabs, each an equal share of the width and 64 px tall. The
connection banner appears above every tab whenever something is not right.

- **OVERVIEW**
  - The highest open severity as a 40 px word in its tone, with "1 at this
    severity, of 9 open".
  - The count per severity, three by two in portrait and six in a line in
    landscape.
  - Open and unacknowledged counts.
  - Monitored and down hosts, then unknown and in maintenance.
  - The server, the Zabbix version, and when the data was updated.
  - The three most severe problems, each a tap from its host.
  - With nothing set up: the setup steps and **TRY THE DEMO**.
- **PROBLEMS**
  - At most 100 rows, most severe then newest.
  - Each row shows the severity word in its tone, the host, the age and ACK
    on the first line, and the problem name on the second.
  - Tapping a row opens its host.
- **HOSTS**
  - At most 200 rows, attention first: worst problem, then DOWN, then
    UNKNOWN, then UP; maintenance last.
  - Each row shows the name and UP/DOWN/UNKNOWN in its tone, then the
    problem count with the worst severity, and a MAINTENANCE tag.
- **Host detail** (over PROBLEMS or HOSTS)
  - BACK, then the name and "DOWN · 1 problem · DISASTER".
  - Its open problems.
  - The latest values, the stock templates' keys first (ping, CPU, load,
    memory, `/`, uptime, ICMP), formatted with B/bps scaling, uptime in days
    and "Up"/"Down" for ping.
- **STATUS**
  - Server, address, mode (LIVE or SIMULATED plus the scenario), sign-in
    (never the token), encryption, API version, connection, last success,
    last error with its detail, and the refresh intervals.
  - **REFRESH NOW**.
  - In the demo: **SCENARIO: <name>**, which steps through the fake's
    scenarios, and **LEAVE THE DEMO**.

**Severity is never carried by colour alone** (DS §2):

| Severity | Word | Tone |
| --- | --- | --- |
| Disaster | DISASTER | status_error |
| High | HIGH | status_error |
| Average | AVERAGE | status_warn |
| Warning | WARNING | status_warn |
| Information | INFO | secondary |
| Not classified | N/C | secondary |

The tones are the colour-only status roles laid over the label's font role,
so no new style, token or colour is added.

**Screenshots** of every screen, both shapes, are produced by
`ZABBIX_SHOTS=<dir> zabbix_app_test` (§11).

**It is off by default.** The app is built into the shell only with
`-DPOCKETOS_WITH_ZABBIX=ON` (ui/shell/CMakeLists.txt). Without it the
launcher, every existing suite and the image's shell are exactly master's;
`pos-zabbix` is still built and installed, so `pos-zabbix check` works on
the bench. Built in, the app appears on the launcher under MORE with a
symbol, because there is no Doors icon for it yet. It can also be opened
with `doors app open zabbix`.

## 9. The fake server and the mock

`core/zabbix/zbx_fake.c` is a deterministic Zabbix server.

- It uses the same envelopes, strings as numbers, error codes and messages,
  and version rules as the real one:
  - the `auth` member before 6.4 and the header after;
  - 7.2's refusal of `auth`;
  - 7.4's refusal of a header on `apiinfo.version`;
  - 7.0's strict `problem.get` parameters.
- Its data is computed from fixed tables, so a scenario looks the same every
  run.

| Scenario | What it is |
| --- | --- |
| `demo` (default) | 36 hosts, 9 problems of every severity (3 acknowledged), 2 hosts down, 1 unknown, 1 in maintenance |
| `healthy` | 24 hosts, all up, no problems |
| `empty` | no hosts, no problems |
| `large` | 1 500 hosts and 1 200 problems (more than the viewer fetches); long names; a disaster host whose name sorts late |
| `slow` | demo, every answer 4 s late |
| `timeout` | nothing ever answers |
| `refused`, `dns`, `tls` | connection refused, name unknown, TLS failure |
| `auth`, `expired` | "Not authorized.", "API token expired." |
| `apierror` | `No permissions to call "problem.get".` |
| `malformed` | problem.get answers half a JSON document |
| `http500` | the web server answers 500 with HTML |
| `huge` | problem.get answers more than 2 MiB |
| `flap` | 12 answers, then 4 refusals, round and round |
| `drop` | 12 answers, then the server is gone for good (stale data) |
| `old` | a 6.0 LTS server |
| `v74` | a 7.4 server |
| `short` | demo, but every `user.login` session ends after 6 requests (renewal) |

**Sessions.** `user.login` succeeds for the user `demo` with any password;
another user is refused with the real text. `user.login` with a credential
is refused, as the real server does. A session id is checked against the one
issued, ended by `user.logout`, and counted: the mock's
`GET /__mock/stats` answers `logins N logouts N open 0|1 failed_logins N
requests N`, so a test can see that nothing was left open and that a refused
password was not tried again on its own.

Where the fake runs:

- **In the helper:** `pos-zabbix session --fake SCENARIO`, `mode=fake` in
  zabbix.conf, or `POCKETOS_ZABBIX_BACKEND=fake` (the simulator's default).
  Everything it shows is labelled SIMULATED.
- **Behind a real socket:** `tools/zabbix/pos-zabbix-mock [--port N]
  [--scenario S] [--tls CERT KEY]`. It serves the same answers over HTTP or
  HTTPS, so the real libcurl path runs without a Zabbix server.
  `GET /__mock/scenario/NAME` switches the scenario. It is a development
  tool, built by `make test` and never installed.

## 10. Setting it up on a unit

Over SSH on the unit:

```sh
cat > /etc/pocketos/zabbix.conf <<'EOF'
url=https://zabbix.example.com/zabbix/
label=Production
EOF
echo 'PASTE-THE-API-TOKEN' | pos-zabbix set-secret token   # stored 0600, never echoed back
pos-zabbix check                                            # one round; exit 0 = the server answered
```

- **Self-signed server:** add `ca_file=/etc/pocketos/zabbix-ca.pem`.
- **Password instead of a token:** `auth=password` and `user=viewer` in
  zabbix.conf, then type the password where it is neither echoed nor kept in
  the shell history (`printf` is a builtin, so it is no process's argument):

  ```sh
  IFS= read -rs -p 'Zabbix password: ' P; echo
  printf '%s\n' "$P" | pos-zabbix set-secret password; unset P
  ```

  The account must not be in a group with multi-factor authentication
  (§3); use a token for such an account. A wrong password is not tried
  again on its own (§5).
- **The demo, with no server:** `mode=fake` in zabbix.conf, or tap TRY THE
  DEMO.
- **To remove the secret:** `pos-zabbix clear-secret`.

### 10.1 Trying a real server from the development host

`tools/zabbix/try-server.sh` makes one read-only round against a real
server from WSL, before anything goes near a unit. The URL and user are
arguments, and the secret is asked for. Nothing about the server is
committed.

**What it does:**

- It makes a private 0700 directory for the configuration, the secret and
  the log, and removes it on exit.
- It asks for the password (or token) without echo and stores it with
  `pos-zabbix set-secret`, through a pipe from a shell builtin.
- It runs `pos-zabbix check`, which logs in, fetches, and logs out.
- It prints the version, the state, the counts, the ten newest problems and
  the helper's log.

**Step 1: build once.** This host has the libcurl run-time only, so point
the build at a copy of the image sysroot's curl headers, as in
docs/hardware/ZABBIX_EXPERIMENT_VALIDATION.md §2. With `libcurl4-openssl-dev`
installed, `make ZABBIX_CURL=1 tools/zabbix/pos-zabbix` is enough.

```sh
make ZABBIX_CURL=1 ZABBIX_CURL_CFLAGS=-I$HOME/work/curl-headers \
     ZABBIX_CURL_LIBS=-l:libcurl.so.4 tools/zabbix/pos-zabbix
```

**Step 2: run it** with the frontend URL and the user. The URL is the one
the browser shows for the frontend, without `index.php`. The helper appends
`api_jsonrpc.php`.

```sh
tools/zabbix/try-server.sh https://zabbix.example.com/ viewer
tools/zabbix/try-server.sh https://zabbix.example.com/ viewer ca_file=/path/ca.pem   # private CA
tools/zabbix/try-server.sh https://zabbix.example.com/ --token                      # an API token instead
```

**Reading the result:**

- `try-server: ONLINE` means the version, the login, problem.get,
  trigger.get, hostinterface.get and host.get all answered, and the session
  was logged out.
- The state line names the failure otherwise:

| State | Meaning | Next step |
| --- | --- | --- |
| `authfail` / `auth` | wrong user or password, a blocked account, or a group with MFA | Check in the browser before trying again; each try counts towards the lockout (5 by default) |
| `retrying` / `http` | HTTP 404 means the API is not at that path | Try the URL with `/zabbix/` appended |
| `retrying` / `tls` | the certificate is not trusted | Add `ca_file=...` |
| `retrying` / `api` | the user may not call a method | Check the user role's API access |

Run it once per change; never in a loop. Compare the counts with the
frontend's Problems and Hosts pages.

## 11. Validation

Host results, 2026-09-25 (WSL2 Ubuntu 22.04, gcc 11.4):

| Suite | What | Result |
| --- | --- | --- |
| `tests/zbx_model_test` | words, UTF-8 cutting, orders, counts, ages | 52 ok |
| `tests/zbx_proto_test` | every line both ways; damaged, partial and oversized sets refused | 38 ok |
| `tests/zbx_api_test` | request shapes per version; envelope; error classes; malformed shapes; values; Date header; user.logout per version | 67 ok |
| `tests/zbx_config_test` | the conf rules, the URL rules, the secret's modes, a symlink, atomic replace, passwords byte for byte | 54 ok |
| `tests/zbx_client_test` | schedule, backoff sequence, auth lockout, flap, stale, the 6.0 and 7.4 rules, large estate, detail, password re-login, no password retry over 2 h, logout, 200 session renewals, 400-round soak | 93 ok |
| `tests/zabbix_view_test` | banner in every state, stale boundaries, overview counts, rows, STATUS | 53 ok |
| `tests/zabbix_session_test` | the real helper: start, commands, scenario switch, crash, missing binary, hang, abandon mid-request, a burst past the socket buffer, 20 opens with no leaked descriptor | 29 ok |
| `tests/zabbix_http_test.sh` | the real libcurl path against the mock: HTTP scenarios, timeout, refused, DNS, and HTTPS untrusted, `ca_file`, wrong name, `verify_tls=0`; user and password: login, renewal, logout, one failed login only, try-server.sh; token and password in no log | 44 ok (with libcurl) |
| `zabbix_app_test` (CMake) | the screen under a real pointer, portrait and landscape: every tab, row to host and back, scenario switch, offline banner with data kept, auth, unconfigured and demo, missing helper, crash and restart, large estate bounded, long names, 20 opens and closes | 66 ok |
| `tests/zabbix_shell_test.sh` | the real simulator shell opens Zabbix in both orientations, draws the disaster in the error colour, logs no warning, leaves no helper | 19 ok (Zabbix shell); the real-shell half SKIPs on a default shell |
| `tests/zabbix_lint.sh` | the boundaries of §4 and §7 | 0 failures |

### 11.1 Full validation

The full record is docs/hardware/ZABBIX_EXPERIMENT_VALIDATION.md. In
short, from a clean clone of `f489d98` (the password fixes; §7 of the
record):

- **`make all` and `make test`** (`-Werror`) exit 0, with 123 suites at
  0 failures.
- **HTTP and HTTPS with libcurl:** 44 ok.
- **Sanitizers:** all seven Zabbix suites pass under ASan and UBSan.
- **Shell suites:** all 22 pass, the existing ones on a default shell;
  `zabbix_app_test` 66 ok.
- **riscv64:** `make all ZABBIX_CURL=1` (`-Werror`) and both DRM shells
  build with no first-party warning.

**Not tested:**

- **A real Zabbix server**: none was used. The procedure is §10.1.
- **Unit A** (DEVICE UNVERIFIED): not reachable on 2026-09-25.
- **The device's libcurl 8.12.1 at run time**: the host tests use Ubuntu's
  7.81.0. The riscv64 helper is built and linked against 8.12.1, but it has
  not been run.

## 12. Open items and next steps

Against a real server (Zabbix 7.0 LTS preferred, with a read-only token):

1. Run `tools/zabbix/try-server.sh` (§10.1) and compare the counts with the
   frontend's Problems and Hosts pages.
2. Confirm on a running server what §3 verified in the source: the
   `suppressed` filter, the `hostinterface.get` filter, and the error
   strings.
3. For a password setup, check that the helper's log says `logged out`
   (not `logout failed`), and test renewal against a short auto-logout
   on the user's profile.
4. Test a 6.0 server (the `auth` member) and a 7.4 server.

On unit A:

1. Hand-install `doors-shell`, `pos-zabbix` and `pos-zabbix-mock` with a
   rollback, as for Camera.
2. Run the demo in both orientations: scrolling, taps, rotation.
3. Point the helper at the mock on loopback (HTTP and HTTPS) and test
   pulling Ethernet or Wi-Fi.
4. Measure CPU and RSS of the shell and the helper on the C908.
5. Check the behaviour with the clock unset.

For the product:

- a Doors icon;
- DS §35 acceptance;
- ADR-007 decisions 1-3;
- whether problems should ever be acknowledged from the unit (it would be
  the first write);
- whether background alerts (a resident service) are wanted.
