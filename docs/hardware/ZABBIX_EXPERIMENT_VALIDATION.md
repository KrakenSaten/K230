# Zabbix viewer experiment: validation record

Branch `experiment/zabbix-dashboard`, from master `d2b2be6` (VERSION
0.0.13, development). Host: WSL2 Ubuntu 22.04, gcc 11.4, Ubuntu's libcurl
7.81.0 at run time. Cross: the SDK's riscv64 toolchain against the
`k230_pocketos_defconfig` sysroot, with libcurl 8.12.1 and OpenSSL 3.4.1.

Every run is from a **clean clone** of the named commit, with `vendor/`
linked read-only. There is no `POCKETOS_LOG_*` in the environment. An earlier
run exported `POCKETOS_LOG_STDERR=0`, which silenced exactly the lines some
suites read, and it is not counted.

**Not run:**

- **Unit A.** It did not answer on .171 or .157 ("No route to host"), and
  its console (COM9) was not attached. The gate is prepared in
  ZABBIX_UNIT_A_GATE.md and has not been run.
- **A real Zabbix server.** There were no credentials or URL. Everything
  server-side is the fake (core/zabbix/zbx_fake.c), in process or behind
  `pos-zabbix-mock` on a real socket.

## 1. `make all` and `make test` (host, `-Werror`)

Final commit: **a1b697e**. `make all` exits 0 and `make test` exits 0, with
123 suite summaries, every one reporting 0 failures. Among them are the
repository gates the change touches: build_outputs, build_deps,
deploy_staging, notices, package_sync, initscript and app_icons.

The Zabbix suites inside `make test`:

| Suite | Checks | Failures |
| --- | --- | --- |
| zbx_model_test | 52 | 0 |
| zbx_proto_test | 38 | 0 |
| zbx_api_test | 65 | 0 |
| zbx_config_test | 49 | 0 |
| zbx_client_test | 80 | 0 |
| zabbix_view_test | 52 | 0 |
| zabbix_session_test | 29 | 0 |
| zabbix_http_test | configuration and secret half | 0 |
| zabbix_lint | boundaries | 0 |

In `make test`, zabbix_http_test's socket half is SKIPPED: this host has no
libcurl headers, so the default build has the fake transport only. The
socket half is §2.

What the runs before the final one found, and how each was fixed:

- **a027158:** `app_icons_test` parsed the `#ifdef`'d registry entry as a
  launcher app without an icon. The registry now takes the experiment
  through a macro (bdf20be).
- **bdf20be:** `build_deps_test` required the transport's objects to be
  rebuilt when the identity changes, because the User-Agent names the
  version (a1b697e).

## 2. The real transport: `tests/zabbix_http_test.sh` with libcurl

`make ZABBIX_CURL=1 ZABBIX_CURL_CFLAGS=-I<the sysroot's curl headers>
ZABBIX_CURL_LIBS=-l:libcurl.so.4`, then the suite: **29 ok, 0 FAIL, 0 SKIP**.

- **Configuration:**
  - `http://` refused without `allow_insecure_http`;
  - a token in zabbix.conf refused;
  - "no secret" says how to set one;
  - set-secret gives 0600/0700;
  - a secret others may read is refused;
  - a token with a space is refused.
- **HTTP** against `pos-zabbix-mock`: online with the demo's 9 problems and
  36 hosts, and the server's clock taken from the Date header.
  - auth and expired go to authfail/auth;
  - http500 to retrying/http;
  - malformed to retrying/malformed;
  - apierror to retrying/api;
  - huge to retrying/toolarge;
  - refused to retrying/connect;
  - old (6.0) and v74 (7.4) go online;
  - timeout gives retrying/timeout after about `timeout_s`;
  - nothing listening is a connect failure;
  - a `.invalid` name is a DNS failure.
- **HTTPS**, against the mock with a certificate made per run:
  - untrusted: refused (tls);
  - trusted through `ca_file`: online;
  - a certificate for another name: refused (tls);
  - `verify_tls=0`: online.
- **The token** is in no log, and the helper never prints it.

## 3. Sanitizers: `make zabbix-san-test`

ASan and UBSan with `-fno-sanitize-recover=all`, the helper built with
them too. All seven suites pass: model 52, proto 38, api 65, config 49,
client 80 (including the 400-round refresh-and-reconnect soak), view 52 and
session 29 (the real sanitized helper: crash, hang, burst, 20 opens). No
leaks were reported.

## 4. The simulator shell and every shell suite (a027158)

There are two shells, each built from the clean clone:

- **Default** (`POCKETOS_WITH_ZABBIX` off, which is master's launcher). It
  ran every existing suite.
- **Zabbix** (`-DPOCKETOS_WITH_ZABBIX=ON`). It ran `zabbix_shell_test`.

Neither build printed a first-party warning.

| Suite | Result | Suite | Result |
| --- | --- | --- | --- |
| alert | 5 ok | files | 15 ok |
| auto_rotation | 37 ok | fleet | 34 ok |
| brightness | 38 ok | kbd | 4 ok |
| calculator | 32 ok | notes | 19 ok |
| calendar | 21 ok | radar | 44 ok |
| camera | 20 ok | rift | 26 ok |
| chrome | 59 ok | settings | 9 ok |
| clock | 27 ok | system_brand | 45 ok |
| display_geometry | 88 ok | system | 21 ok |
| doors | 50 ok | timber | 45 ok |
| wave | 16 ok | **zabbix** (Zabbix shell) | **19 ok** |

Every suite has 0 FAIL.

**zabbix_shell_test** covers:

- `zabbix_app_test`: 66 checks, both orientations, and every fault in
  docs/apps/ZABBIX.md §11.
- LVGL warns about nothing.
- The real shell opens Zabbix with `--open` in portrait and landscape and
  draws the disaster in the error colour.
- No warning but the simulator's missing radiod, and no helper left behind.

On the default shell it runs the app half and SKIPs the real-shell half,
saying why.

The later commits (bdf20be, a1b697e) touch only shell.c's registry text
and the Makefile's identity list. The default shell's registry compiles to
the same list.

## 5. riscv64

From a clean clone of a1b697e, with `BUILD_ID` set to the commit:

- **`make all`:** `ZABBIX_CURL=1 POCKETCAM_JPEG=1` with `-Werror` (the
  package's flags) exits 0 with no first-party warning.
- **The helper:** pos-zabbix is 116 296 bytes and needs `libcjson.so.1`,
  `libm.so.6`, `libcurl.so.4` and `libc.so.6`. All four are in the image.
- **The mock:** pos-zabbix-mock (no TLS) is 83 184 bytes.
- **The DRM shell** against the sysroot LVGL:
  - `POCKETOS_WITH_ZABBIX=OFF` exits 0 with 0 first-party warnings;
  - `POCKETOS_WITH_ZABBIX=ON` exits 0 with 0 first-party warnings, and is
    1 320 936 bytes.
- **Build stamp:** `DOORS_BUILD_ID=a1b697e` is in both `doors-shell` and
  `pos-zabbix`.
- **The gate payload** is in WSL `~/work/zbx-payload-a1b697e/`:

```
7a9d4783b9a912a2eba21eb6718906f762fe16b2618541811b7eb7feade0f7bf  doors-shell   (POCKETOS_WITH_ZABBIX=ON)
315d132172b424d05a32a91fd3d85a6d532833f514ded12ece238a62c31b71bd  pos-zabbix
d4120996c5cf5814075f25151b3dc5a787f01fe1b6b293b25807d4a6da4fce3f  pos-zabbix-mock
```

Nothing was built into an image, and nothing was flashed or deployed.

## 6. Measurements (host, x86-64)

| What | Value |
| --- | --- |
| pos-zabbix peak RSS, demo (36 hosts, 9 problems) | 2.2 MB |
| pos-zabbix peak RSS, large (1 500 hosts, 1 200 problems) | 3.0 MB |
| pos-zabbix peak RSS, huge (a 3 MB answer; the in-process fake builds it, the real transport refuses it unparsed) | 12.6 MB |
| The app's model / session (with staging sets) | 57.5 KB / 57.6 KB |
| One refresh of the large estate on the line protocol | 17 KB |
| Helper CPU for one large round, fake in process | 0.04 s user |

On the C908 the CPU, RSS, the shell's RSS growth and the time to first
build 200 host rows are **not measured**. That is step 7 of the unit A gate.
