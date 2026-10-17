# Unofficial NinjaOne monitoring client

Reviewed against official documentation on 2026-10-12. Synthetic host validation
passes; **live OAuth, tenant API behavior and K230 hardware remain untested**.
The app is read-only and is not NinjaOne-endorsed.

## Everyday behavior

NinjaOne is in the launcher's Apps folder, after MP3, with original Doors artwork.
Overview shows active alerts and affected devices. Organizations supports name
search and twelve local favorites. Organization/device detail shows the loaded
alerts, API-reported online/offline status and raw lastContact. Severity filtering
uses the exact API string, case-insensitively; missing fields say Unknown.
Lists show twenty rows per page and scroll. Existing portrait/landscape and
Small/Medium/Large evidence is preserved in the validation record.

**Refresh data** updates monitoring data. **Renewing sign-in** is a separate
helper progress state; no token appears in the UI. Connected, setup, Sign in
again, access denied, renewal failure and disconnected states are explicit.
Failed requests retain the last complete snapshot marked **STALE**, including
last success and “Last observed” device status. A tenant change clears the old
snapshot and organization/device selection before displaying the new tenant.
Disconnect retains the last snapshot explicitly stale and stops polling.

Credentials survive app close/reopen within the same boot. After access-token
expiry, the helper uses the documented public refresh grant, without a client
secret. The official guide says to request renewal **only after expiry**; we
follow that rule rather than renewing early. While open, the next monitoring
check is scheduled no later than the recorded expiry, then authorization renews
before any API GET. A 401 during a snapshot permits one renewal and a fresh
snapshot; a replacement that still receives 401 requires sign-in again. Normal monitoring checks run 120 seconds after success.
Closing the screen cancels its helper and stops requests. Reboot erases `/run`
credentials: another workstation login is still required.

Renewal/import/Forget share an exclusive nonblocking helper lock. A rotated
access/refresh pair and expiry replace one runtime record atomically. Definite
429/503 renewal failures retry at most three times, with at least 60 seconds
between attempts and bounded Retry-After (up to 24 hours). Exhausted retries,
revoked grants, malformed responses and ambiguous transport/interruption require
Sign in again. A pending marker prevents replaying an old token after a crash
that may have consumed it at NinjaOne. No rotation grace period is assumed.
A response without a refresh token is deliberately refused: the documented
example includes one, but omission/reuse guarantees remain unverified locally.

Monitoring failures use the existing 60–3600 second backoff; Retry-After can
extend it to 24 hours. Manual Refresh data respects rate/renewal cooldowns.
Setup, Sign in again, access denied and disconnected states stop automatic
checks. Refresh data can detect a newly provisioned login. API traffic remains
GET-only; OAuth POSTs go solely to `/ws/oauth/token`. There are no device actions,
alert changes, scripts or remote control.

## Limits and tenant favorites

Snapshots contain at most **250 organizations, 500 devices and 500 alerts**, with
2 MiB per API response. Counts always describe the snapshot. Every connected
snapshot says **Limited snapshot** because global-alert pagination/completeness
is unverified, including responses below the cap. Missing devices remain Unknown;
organization filtering cannot classify alerts outside the loaded device list.
lastContact stays a numeric API timestamp: no unverified seconds/milliseconds
conversion is performed. Last-success dates use the unit's local timezone.

Favorites are nonsecret settings keyed by a 96-bit SHA-256 prefix of the region
and an explicit stable nonsecret tenant identifier supplied during login. Use a
unique identifier for each tenant, and reuse it on subsequent logins; do not use
an organization ID, which can overlap between tenants. This is an operator-bound
namespace, not an API-verified tenant identity. Switching identifiers clears
selections and loads only that namespace's favorites. Demo uses separate keys.
Old unnamespaced favorites are not migrated, since their tenant is unknown.
Legacy access-token-only provisioning cannot save tenant favorites.

## Official evidence and remaining uncertainties

| Source | Documented capability / constraint |
| --- | --- |
| [Native registration](https://app.ninjarmm.com/apidocs-beta/authorization/create-applications/native-apps) and [PKCE](https://app.ninjaone.com/apidocs-beta/authorization/flows/authorization-code-flow-with-proof-key-for-code-exchange-pkce) | Native Authorization Code, S256 verifier/challenge, state and secret-free code exchange; `/ws/oauth/authorize` and `/ws/oauth/token` examples |
| [Refresh grants](https://app.ninjarmm.com/apidocs-beta/authorization/refresh-tokens) | Enable Refresh Token grant and request offline_access; client_secret is optional and required only for confidential clients; expires_in seconds; response example includes rotated refresh_token; dashboard revocation invalidates related tokens; renew only after expiry |
| [Admin OAuth guide](https://www.ninjaone.com/docs/application-programming-interface-api/oauth-token-configuration/) and [integration guide](https://www.ninjaone.com/docs/integrations/how-to-set-up-api-oauth-token/) | Native redirect URLs are **not configurable**; Native uses localhost. Exact URI/port/path is not established by these public pages |
| [Official regional guide](https://www.ninjaone.com/blog/how-to-set-up-an-api-server-in-ninjaone/) (updated 2026-09-16) | app/us2 (US), ca (Canada), eu (Europe), oc (Asia-Pacific Southeast), jp (Japan), fed (FedRAMP Moderate), all `.ninjarmm.com`; origin must match tenant login |
| [API index](https://app.ninjarmm.com/apidocs/) | GET /v2/alerts, active triggered conditions; organization/device list endpoints |
| [Organizations](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getOrganizations) and [devices](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getDevices) | Arrays; IDs, names, organizationId, boolean offline, numeric lastContact; pageSize and after (last ID) |
| [Device alerts](https://app.ninjarmm.com/apidocs-beta/core-resources/operations/getDeviceAlerts) | Shared uid/deviceId/message and optional string severity; no verified severity enum |

The global getAlerts page remains JavaScript-only. The official OpenAPI JSON
was inaccessible (earlier download HTTP 403); the official NinjaOne Postman
workspace did not expose the schema in the available reader. **Global response
shape/pagination and lastContact units remain unconfirmed.** The existing shared
alert-field decoder stays conservative: one fetch, no invented cursor/query,
Unknown optional values, reject unsupported envelopes. No human date conversion.
The regional origin list is documented; live Native support in each region is
untested. The previous unconfirmed app.ninjaone.com API origin is no longer
accepted. No numeric API quota is inferred.

Exact remaining OAuth uncertainties: the tenant's fixed Native callback URI;
actual secret-free PKCE + offline_access acceptance; refresh-token omission,
rotation/reuse/lifetime semantics on that tenant. There is no assumed device-code
flow. Do not resolve these by embedding a shared secret or weakening TLS.

## Secure local setup

1. Create a **Native** client under Administration > Apps > API > Client App IDs.
   Enable **Authorization Code**, **Refresh Token**, and **Monitoring only**.
   Do not enable Management, Control or Implicit. Use a restricted viewer account.
2. Inspect/copy the exact **fixed Native redirect URI** in your tenant's
   registration. Do not try to register a custom callback. If the URI cannot be
   determined from the registration, obtain that single value from NinjaOne's
   tenant documentation/support before login. The utility accepts HTTP localhost
   or 127.0.0.1 and binds that exact IPv4 port/path; other callbacks are refused.
   If the fixed port is unavailable/privileged on the workstation, resolve the
   listener arrangement locally rather than substituting an unregistered URI.
3. Configure `/etc/pocketos/ninjaone.conf` on the unit with exactly one nonsecret
   line, for example `base=https://eu.ninjarmm.com`. Accepted HTTPS origins are
   app/eu/oc/ca/us2/jp/fed.ninjarmm.com. Use the tenant's login region. HTTP,
   arbitrary origins and redirects are refused. The system clock must be correct
   for TLS; use ordinary NTP, never bypass certificate checks.
4. On a trusted workstation with Python 3 and a desktop browser, with shell
   tracing disabled, run the secure local pipeline (replace only nonsecret
   placeholders):

   ```sh
   set -o pipefail
   python3 tools/ninjaone/provision.py \
     --base https://eu.ninjarmm.com --client-id YOUR_NATIVE_CLIENT_ID \
     --redirect-uri 'EXACT_FIXED_NATIVE_REDIRECT' --tenant-id YOUR_UNIQUE_TENANT_ID |
     ssh root@UNIT /usr/bin/pos-ninjaone set-credentials
   ```

   Sign in only on NinjaOne's browser page. Never send secrets in chat, put them
   in argv/environment, or tee/capture the pipe. Client ID, callback URI and
   tenant identifier are nonsecret. The utility validates state, scopes, bearer
   type, token sizes and expires_in; verified TLS/no redirects; bounded response;
   only a credential bundle goes to stdout, which refuses a terminal. Python/
   urllib memory copies cannot be guaranteed wiped; use a trusted workstation.
5. Check the pipeline's exit status, open NinjaOne and select **Refresh data**.
   Normal expiry now renews locally during the same boot. Use **Disconnect /
   Forget credentials** to delete the runtime login and stop requests. This is
   local deletion, not server revocation; use Administration > Apps > OAuth
   Tokens > Revoke separately if needed. Failed deletion is reported, not hidden.

The runtime credential record is `/run/pocketos/ninjaone/credentials`: directory
0700, file 0600, owner checked, no links/extra hard links, bounded same-build
format, O_EXCL/O_NOFOLLOW staging, fsync/rename/directory fsync. The helper disables
core dumps/dumpability and wipes owned token/header/JSON buffers. Credentials never
enter shell memory, UI, IPC frames, favorites, argv, environment or logs.
Permissions are **not encryption** or protection from root/physical access.
Runtime-only storage introduces no new persistent-secret exception. `/run` must
be the normal volatile runtime root, not redirected to persistent storage.
Legacy `set-token` remains available for bounded access-token-only input; it
replaces any bundle and has no renewal or tenant favorites.

Persistent storage is prohibited by the current policy. A concrete opt-in
permission-only exception and crash-safe implementation design are proposed in
[ADR-014](../decisions/ADR-014-ninjaone-credential-persistence.md), awaiting product
owner approval. No persistent secret, remember toggle or reboot restoration is
implemented or claimed.

## Implementation and focused checks

The shell links only the bounded model/session; curl/cJSON/authentication stay
in the one-shot helper. A fixed-size validated IPC frame contains no credentials;
a separate nonsecret progress frame identifies sign-in renewal. The shell reads
at most 64 KiB per 50 ms timer pass and cancels/reaps on close. Existing lifecycle
watchdog (120 seconds), request timeouts (5 seconds connect / 10 seconds total),
128 MiB helper limit, FD closure and parent-death handling remain intact. Runtime
locking survives cancellation safely; interrupted renewal requires sign-in again.

Live mode remains default. Synthetic mode requires POCKETOS_NINJAONE_BACKEND=fake
and always labels DEMO / synthetic; it never activates implicitly on failure.
Fixtures include renewing and tenant2 in addition to the existing error/data cases.
The test TLS origin/CA injection exists only in the test executable, not a shipped
CLI/config option.

```sh
make CC=gcc CFLAGS='-O2 -Wall -Wextra -Werror' NINJAONE_CURL=1 ninjaone-auth-test
cmake --build out/cloud-shell --target pocketos-shell -j4
python3 tests/ninjaone_shell_test.py --focused
```

See [validation record](NINJAONE_VALIDATION.md) for the new focused checks and
preserved earlier UI/lifecycle evidence. The pinned K230 cross-build remains
unavailable in this environment; no toolchain pin or dependency was changed.

## Shortest remaining local checklist

1. Inspect the fixed Native callback; run the pipeline once with your restricted
   viewer. Confirm no secret is required and a refresh token is issued. Record
   only success/failure and field presence, never credential values/bodies.
2. Leave the app open through expiry: confirm renewing → connected, rotation
   behavior, then dashboard revoke → Sign in again. Forget, reopen and confirm
   disconnected; reboot currently requires another login.
3. From the tenant's official OpenAPI/reference or a local read-only inspection,
   establish global alert schema/pagination and lastContact units. Compare only
   the loaded snapshot to NinjaOne; do not treat counts as complete tenant totals.
4. Use the pinned local build procedure in the validation record. Hardware smoke
   remains local, outside this task. Decide ADR-014 separately for reboot login.
