# Unofficial NinjaOne monitoring client

First version for DOORS, reviewed against official documentation on 2026-10-12.
Offline validated; **live login, tenant behavior and K230 hardware are unverified**.
This is not a production-ready or NinjaOne-endorsed application.

## Behavior and limits

NinjaOne is in the current launcher's **Apps** folder, after MP3, in the tools
hue (the older CONNECTIONS group no longer exists). Its three linked nodes and
alert marker are original Doors artwork, generated through the SVG/PNG/A8 and
portal-icon pipeline. No NinjaOne logo or supplied branding is copied.

Overview shows active alerts and affected devices. Organizations has a
case-insensitive name search and twelve local favorites. Selecting an organization
shows its loaded devices and alerts; selecting a device shows name, organization,
API-reported online/offline status, last contact and active alerts. Alerts accepts
an exact, case-insensitive severity string; blank means all, `Unknown` includes
missing severity. Raw values are displayed without invented numeric ranks or
severity mappings. Organization and severity filters are local to the snapshot.
Missing optional fields are unknown. Device details use the basic device list's
verified fields; no additional per-device request is necessary.

The screen supports touch, the common Tab/Enter/text-key stream, Back, portrait,
landscape and Small/Medium/Large text. Long pages scroll; lists show twenty rows
at a time with Previous/Next. The app includes loading, empty, setup, auth-expiry,
access-denied, rate-limit, malformed/oversized-response and network/TLS states.
After an error the previous complete snapshot stays, marked **STALE**, with its
last successful update time. Stale device status says **Last observed**. After
four minutes without success a retained snapshot is stale even during loading.
Last-success times follow the device's configured local timezone. Last contact
is displayed as the numeric API timestamp because its unit was not established by
the available official field reference. An unset clock must be fixed
through ordinary NTP/system configuration, not by weakening TLS.

A snapshot holds at most **250 organizations, 500 devices and 500 alerts**.
Counts describe the loaded snapshot, not a guaranteed whole-tenant total. A limit
is shown explicitly; a full final device page is conservatively marked limited.
An alert whose device lies outside the device cap remains visible under an unknown
device. Organization filtering cannot classify those alerts, and says so for a
limited snapshot. Detail shows the first twenty device alerts; the paged Alerts
list retains the rest. Names/messages are bounded and sanitized UTF-8.

Refresh runs every **120 seconds after success**, only while the app is open.
Manual Refresh cancels an earlier refresh and starts new work. Failures back off
60, 120, 240, 480, 960, 1920, then 3600 seconds; Retry-After can extend this to a
maximum of 24 hours. Manual Refresh respects rate-limit backoff. Setup errors,
401 and 403 stop automatic retries until manual Refresh or reopening. No tenant
request, timer or monitoring cache survives closing the app. Only favorite IDs
persist in the existing nonsecret settings store. Demo favorites use a separate
key. Clear favorites before changing the device to another tenant: IDs are
unique within a tenant, not across all tenants.

There are no script, reboot, reset, acknowledgement, remote-control or write
operations. API traffic is GET-only. The local workstation's OAuth token exchange
is POST to the authorization server, and changes no tenant monitoring resource.

## API evidence and remaining uncertainty

Official sources (public reference, including its current beta documentation):

| Source | Verified contract used |
| --- | --- |
| [Public API index](https://app.ninjarmm.com/apidocs/) | `GET /v2/alerts`, active triggered conditions; organizations and devices list endpoints |
| [List organizations](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getOrganizations) | `/v2/organizations`, array; `id`, `name`; `pageSize`, `after` as the previous organization's ID |
| [List devices](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getDevices) | `/v2/devices`, array; `id`, `organizationId`, `displayName`, `systemName`, boolean `offline`, numeric `lastContact`; `pageSize`, `after` as previous device ID |
| [Device alert contract](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getDeviceAlerts) | Active alerts have `uid`, `deviceId`, `message`, optional string `severity`; no severity enum is specified here |
| [Native app registration](https://app.ninjarmm.com/apidocs-beta/authorization/create-applications/native-apps) | Native platform, Authorization Code; Monitoring is read-only monitoring and organization structure; directs native clients to PKCE |
| [PKCE flow](https://app.ninjaone.com/apidocs-beta/authorization/flows/authorization-code-flow-with-proof-key-for-code-exchange-pkce) | Random verifier, S256 challenge, state; example endpoints `/ws/oauth/authorize` and `/ws/oauth/token`; code exchange lists verifier and client ID without a client secret |
| [Refresh tokens](https://app.ninjarmm.com/apidocs-beta/authorization/refresh-tokens) | Refresh-token exchange is described, but its client-secret requirements are not reconciled for a public native client |
| [Legacy authorization overview](https://app.ninjarmm.com/apidocs/?links.active=authorization) | Older text describes fewer flows; the specific native/PKCE reference is more detailed |
| [Admin OAuth setup](https://www.ninjaone.com/docs/application-programming-interface-api/oauth-token-configuration/) | Administration > Apps > API client registration and scopes |

Organizations/devices request 100 rows, advance with the final ID, stop on a
short page or model limit, and reject repeated/non-increasing IDs and oversized
pages. Alert pagination is **not assumed**: `/v2/alerts` is fetched once, under a
2 MiB response cap, retaining at most 500 entries. No unverified filter query
syntax is sent.

The global alerts endpoint is verified in the official index; its full operation
page was JavaScript-only in the available reader. Decoding uses the officially
documented shared alert fields above and treats severity as optional. Confirm
that global responses match this contract and confirm lastContact timestamp units
in the local smoke test before enabling a human date conversion. This is an
explicit uncertainty, not evidence from a live response. The OpenAPI JSON download
returned HTTP 403 in this environment. The sources did not establish a numeric
NinjaOne rate quota or guaranteed Retry-After behavior; 429 handling and both
standard header formats are defensive client behavior, tested locally.

The appropriate installed-client flow is **Native Authorization Code with PKCE**,
not a shared-secret client-credentials integration. The flow page's `/oauth/...`
prose and `/ws/oauth/...` examples differ; this implementation follows the explicit
request examples. Loopback redirect acceptance, secret-free exchange on this
tenant/region and global alert schema must still be verified locally. There is no
assumed device-code support. Refresh grants are neither requested nor implemented
because public-client refresh requirements remain unresolved. Do not work around
a tenant failure by embedding a shared secret or disabling certificate checks.

## Secure local setup

1. In NinjaOne, create a **Native** client under Administration > Apps > API >
   Client App IDs. Enable **Authorization Code** and **Monitoring only**; do not
   select Management, Control, Implicit or Refresh Token. Use a dedicated viewer
   account with access only to the organizations needed. Verify those account
   permissions locally; administrator access is only for registration.
2. Register `http://127.0.0.1:8765/callback` exactly. Confirm that your tenant
   accepts the loopback redirect and PKCE without a secret. If it refuses either,
   resolve that with NinjaOne's tenant documentation/support before live use.
   No hosted backend is required or introduced.
3. On the unit, configure the matching documented region base as the **only line**
   of `/etc/pocketos/ninjaone.conf`. For an EU tenant, from your trusted local host:

   ```sh
   printf '%s\n' 'base=https://eu.ninjarmm.com' |
     ssh root@UNIT 'cat > /etc/pocketos/ninjaone.conf'
   ```

   Accepted bases are HTTPS origins for app/eu/oc/ca/us2.ninjarmm.com and
   app.ninjaone.com. Use the origin appropriate to your tenant, not a custom URL.
   Redirects are refused so the bearer cannot be forwarded elsewhere.
4. On a trusted workstation with Python 3 and its desktop browser, run:

   ```sh
   set -o pipefail
   python3 tools/ninjaone/provision.py \
     --base https://eu.ninjarmm.com --client-id YOUR_NATIVE_CLIENT_ID |
     ssh root@UNIT /usr/bin/pos-ninjaone set-token
   ```

   Client ID is nonsecret. Sign in only in NinjaOne's browser page. Never put
   credentials into chat, command arguments or environment variables. Keep shell
   tracing disabled and do not tee or capture token output. The utility checks
   state and `scope=monitoring`, uses verified TLS, refuses redirects, bounds its
   response, and emits only the token into the pipe. It refuses terminal stdout.
   Its in-memory Python/urllib copies cannot be guaranteed wiped; use a trusted
   local workstation. No refresh token is requested or saved.
5. Confirm the pipeline succeeded, then open NinjaOne and tap **Refresh**. After
   expiry, rerun the login pipeline and Refresh. Provision again after reboot.

`set-token` writes `/run/pocketos/ninjaone/access-token` (or the existing runtime
root override), directory **0700**, file **0600**, atomic O_EXCL/O_NOFOLLOW write,
fsync/rename. Reads reject links, extra hard links, wrong owners, broad permissions
and oversized/invalid bearer values. `/run` is runtime storage, not the persistent
state store: this introduces no new persistent-secret exception to ADR-003/007.
The protection is Unix permissions, **not encryption**; all current DOORS processes
run as root. Tokens never enter shell UI memory, argv, environment, logs or app
screenshots. The helper disables core dumps and dumpability; owned token/header
buffers are wiped. TLS peer and hostname checks are mandatory.

## Implementation and offline checks

`core/ninjaone` contains the bounded model, helper-only JSON decoder and runtime
token store. `pos-ninjaone snapshot` is a one-shot helper per refresh: HTTP, JSON
and authentication bytes stay there, following ADR-007/009. Each request has a
5-second connect timeout, 10-second total timeout and 2 MiB cap. Helper address
space is capped at 128 MiB. The shell reads at most 64 KiB per timer pass into a
fixed-size, validated snapshot frame, replaces data only after a complete success,
and kills a helper after 120 seconds. Cancellation closes the old pipe and
kills/reaps that child, so a late reply cannot affect a later screen. Child FDs
are closed, pipes are close-on-exec and PDEATHSIG handles parent death. Destroy on
rotation/re-exec closes the app first. Teardown waits at most 50 ms to reap after
SIGKILL; an uninterruptible kernel task is an OS-level limitation.

Live mode is the default in both simulator and release builds. Explicit test mode:

```sh
make CC=gcc NINJAONE_CURL=1 tools/ninjaone/pos-ninjaone
POCKETOS_NINJAONE_HELPER="$PWD/tools/ninjaone/pos-ninjaone" \
POCKETOS_NINJAONE_BACKEND=fake POCKETOS_NINJAONE_FAKE=demo \
SDL_VIDEODRIVER=dummy out/cloud-shell/pocketos-shell --open ninjaone
```

All fake responses are synthetic, pass through the real decoder and carry **DEMO /
synthetic** on screen. Scenarios: demo, large, empty, missing, expired, denied,
rate, network, malformed, hang. Live disconnection never enables demo implicitly.

Focused checks (native libcurl, cJSON, Python, OpenSSL, SDL2 and pinned LVGL):

```sh
make CC=gcc CFLAGS='-O2 -Wall -Wextra -Werror' NINJAONE_CURL=1 ninjaone-test
cmake -S ui/shell -B out/cloud-shell -DLVGL_DIR="$PWD/vendor/lvgl" -DPOCKETOS_DISPLAY=sdl
cmake --build out/cloud-shell --target pocketos-shell -j4
python3 tests/ninjaone_shell_test.py
```

Tests cover pagination/caps/unknown fields/UTF-8, secure token provisioning,
helper cancellation/watchdog/reaping, PKCE's S256 reference vector and state
rejection; actual curl against a synthetic TLS server for HTTP failures, limits,
Retry-After, certificate/hostname verification and redaction; and real shell touch,
keyboard search/severity filtering, favorites, stale data and launcher membership
in both orientations at all three text sizes. Layout audits and synthetic screenshots
stay in ignored `work/`; no authenticated captures belong in the repository.

See [validation record](NINJAONE_VALIDATION.md) for results and exact local build.

## Live smoke test after local build

- Complete Native/PKCE registration and the local pipeline. Record only status
  and field availability, never tokens or response bodies.
- Compare loaded organizations, active alerts and affected devices with NinjaOne;
  check global alert fields, pagination order, `lastContact` timestamp units and
  raw severity values. Confirm the scope grants only reads.
- Open organization/device detail, search, filter a returned severity, and check
  favorites. Check unknown fields and limited-snapshot explanations.
- In portrait/landscape and all text sizes, scroll/tap and use Tab/text/Enter/Back.
- Disconnect networking: last success stays, STALE appears and status becomes
  Last observed. Restore networking and Refresh. Let a token expire: automatic
  retries stop; renew locally and Refresh. Check access-denied behavior with a
  restricted viewer and do not deliberately flood a tenant to induce 429.
- Close/rotate/reopen: confirm no remaining pos-ninjaone process, no API requests
  while closed, and no token in logs, screenshot or crash report.

Tenant login/API confirmation and the pinned cross-build/hardware smoke test are
remaining local work. No merge, device deployment or release-asset change was done.
