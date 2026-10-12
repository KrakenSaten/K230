# NinjaOne cloud validation — 2026-10-12

Branch `feat/ninjaone-app`, based on current origin/master `1b73e955` (fetched).
No NinjaOne tenant credentials, authenticated captures or attached K230 were used.

## Results

| Check | Actual result |
| --- | --- |
| `make CC=gcc CFLAGS='-O2 -Wall -Wextra -Werror' NINJAONE_CURL=1 ninjaone-test` | PASS: decoder/pagination, missing fields and invalid UTF-8, 250/500/500 caps, errors, secure token input/store, cancellation/watchdog and 20 clean helper cycles; PKCE S256/state; real libcurl synthetic TLS failures, Retry-After seconds/date, redirects/caps and redaction |
| Native simulator shell target | PASS: CMake build against pinned LVGL `59dc7e436ae97a25e32656739ea6a943f9f11b6a`; NinjaOne app/session also checked with `-Wall -Wextra -Werror` |
| `python3 tests/ninjaone_shell_test.py` | PASS: six portrait/landscape × Small/Medium/Large cases; keys search/filter, touch navigation, favorites, details, unknowns, stale retention, close cleanup and actual Apps-folder registration |
| Layout review | Synthetic screenshots inspected, including large-text portrait details and landscape stale details; audits reported no overlap or truncation; scrolling works |
| ASan/UBSan API/token/session test | PASS; no sanitizer finding |
| `tests/home_layout_test` | PASS: 370 checks, including actual NinjaOne registry entry and folder/hue/order |
| `tests/launcher_groups_shell_test.sh` | PASS |
| `tests/launcher_folder_shell_test.sh` | PASS, including its focused `home_folder_test` target |
| `app_icons_test.sh`, `doors_ui_assets_test.sh`, `style_lint.sh`, `license_audit_test.sh` | PASS |
| `build_outputs_test.sh`, `package_sync_test.sh` | PASS |
| Full CMake default target | Stopped at an existing Recorder test compile error: `tests/rec_app_test.c:472`, `_lv_area_intersect` alias refers to unavailable `lv_area_intersect`. No unrelated fix or repeated broad sweep |
| Pinned K230 cross-build | NOT RUN: required `/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/` and SDK sysroot absent. No substitute toolchain used |
| Live OAuth/API/hardware | NOT RUN: requires local Native client registration, tenant access and hardware |

Host checks used staged tools from `/workspace/.k230-host/activate.sh`: GCC 14.2.0,
CMake 3.31.6, libcurl 8.14.1, cJSON 1.7.18 and SDL2 2.32.4. These are host-test
versions, not a change to the pinned image's dependencies. Logs, fixture-generated
certificates, screenshots and audits stay under ignored `work/`.

## Exact next local build and smoke step

Use the existing WSL2 Ubuntu build host and pinned vendor tree described in
[BUILD_ENVIRONMENT.md](../BUILD_ENVIRONMENT.md). After checking out the feature
commit in your local Doors checkout (adjust `/mnt/c/K230` if needed):

```sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export POCKETOS_VENDOR_DIR="$HOME/work/t-display-k230"
/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc --version
/mnt/c/K230/platforms/k230/scripts/apply_to_sdk.sh
/mnt/c/K230/platforms/k230/scripts/build_image.sh "" pocketos-rebuild
```

Build from a clean committed tree: apply exports HEAD, not working edits. The
package already selects `NINJAONE_CURL=1` and installs the helper and original
icon. Do not change pins or invoke the dirty-build override to test these edits.
This instruction builds the package; it does not deploy or publish it.

Then complete the Native/Monitoring-only registration and secure local PKCE
pipeline in [NINJAONE.md](NINJAONE.md#secure-local-setup), and run that document's
live smoke checklist on your chosen test unit. Record only outcomes. Confirm the
global alert contract, loopback/PKCE behavior and lastContact units before treating
those as live-verified. Refresh-token behavior remains deliberately unresolved;
tokens require another local login after expiry/reboot.

## Authentication continuation — 2026-10-12

The initial results above are preserved evidence for commit 9db8bc6. Their
runtime-token-only statements are superseded by this continuation and the updated
setup guide; the earlier six-layout/lifecycle sweeps were not repeated.

Official refresh documentation now establishes a secret-free public refresh
grant: client_secret is optional, required only for confidential clients. Native
redirect registration is fixed, not configurable. The guide now requires the
actual locally displayed callback instead of assuming 127.0.0.1:8765/callback.
The official regional guide confirms app/eu/oc/ca/us2/jp/fed.ninjarmm.com.

New changes: runtime access/refresh/expiry bundle, helper-only renewal at expiry
(the official guide says not before), atomic rotation, exclusive lock, pending
marker preventing ambiguous replay, bounded 429/503 retries and cooldowns,
Disconnect/Forget, explicit renewal progress, stale/disconnected state and
operator-bound tenant favorite namespaces. Global-alert coverage is explicitly
limited even below caps. lastContact units remain unknown and raw.

Reboot persistence is **not implemented**: the binding policy permits only
Wi-Fi and Zabbix exceptions. Proposed ADR-014 supplies the exact narrow amendment,
opt-in storage design and root/physical-card risk for approval. No accepted ADR or
storage policy was rewritten.

| Focused continuation check | Actual result |
| --- | --- |
| `make CC=gcc CFLAGS='-O2 -Wall -Wextra -Werror' NINJAONE_CURL=1 ninjaone-auth-test` | PASS: real libcurl against synthetic TLS OAuth/API; unexpired token skips renewal, expiry renews, mid-snapshot 401 renews once, rotated pair read back, revoked/missing/ambiguous replies fail closed, 429 cooldown and three-attempt bound, cancellation refuses replay, lock excludes competing Forget, interrupted staged write keeps old record, Forget deletes runtime state and reopen remains disconnected; fixed callback parsing, tenant keys and jp/fed origins |
| Changed UI/session strict compilation and simulator shell target | PASS: `-Wall -Wextra -Werror` for changed UI/session; native shell links against existing pinned LVGL |
| `python3 tests/ninjaone_shell_test.py --focused` | PASS: one portrait/Medium case for new tenant favorite separation using identical organization IDs, renewal progress, expired authorization retaining stale snapshot, Disconnect retaining stale data and stopping calls; layout audits for renewed/expired/disconnected reported zero overlap/truncation/clipping |
| Read-only boundary lint / `git diff --check` | PASS |
| Pinned K230 cross-build | NOT RUN: same pinned Xuantie toolchain and SDK sysroot absent; no substituted toolchain or pin changes |
| Live OAuth/API and reboot storage | NOT RUN: no tenant/device access; runtime-only credentials intentionally disappear at reboot; proposed persistent design is not implemented |

New synthetic screenshots/fixture certificates/logs stay under ignored `work/`.
No broad suites or the earlier six-layout matrix were repeated. The earlier
ASan/UBSan result applies to the original implementation, not this continuation.
No live credentials or device access were used. Official global getAlerts/OpenAPI and
Postman sources still did not expose schema/pagination or timestamp units here.
