# Browser: unit A integration gate

**Unit A carries this branch's build `301fadf`.** It runs Automatic rotation
(landscape), with Browser open. These files were replaced
or added together:

| File | Change | md5 |
| --- | --- | --- |
| `/usr/bin/doors-shell` | replaced | `107a3190…` (stripped) |
| `/usr/bin/pos-browser` | added | `c86d25bf…` |
| `/usr/bin/pos-camera` | replaced | `b42b6164…` |
| `/usr/share/doors/ui/icon-browser.bin` | added | `b87a69ca…` |

- `pos-camera` is master's own helper; the camera-gallery gate's `f6fe537`
  build had the same md5. The rebased shell carries master's gallery code, and
  the unit's v0.1.0 helper does not speak that protocol, so the two had to go
  in together.
- `e8999f6` (the tip before this sheet) differs from `301fadf` only in
  `tests/web_image_test.c`. Nothing on the unit changes with it.
- Nothing else was touched: services, CLI, init scripts, other art and
  settings.

**Rollback:** `/root/rollback-browser/RESTORE.sh`. It puts back shell
`f2c22f1` (master `dd3809b` code, md5 `05a4d09a…`) and the v0.1.0
`pos-camera` (md5 `b3220ffc…`). It also removes `pos-browser`, the icon,
`/var/lib/pocketos/browser` and any `/run/pocketos/browser.*`. All four were
absent before the gate.

Result: **PASS**. The owner checked the physical keyboard in landscape on
`301fadf`: typing and Enter **PASS** (§3 #3).

The keyboard base is a TCA8418 that the shell polls over I2C
(`keyboard: TCA8418 ready, polling every 15 ms`). It is not an evdev device,
so `/proc/bus/input/devices` does not list it. There is also no
`/dev/uinput`, so it cannot be scripted from SSH.

Run 2026-09-27 06:33-07:18Z by Claude over SSH, with touch injected at the
evdev layer (`tests/hw/touch_slot0_tap.py`) and the panel read with
`kmsgrab`. Evidence is in `out/browser-gate/` (not committed): scripts, logs
and `caps/*.png`.

## 1. Build

| What | Result |
| --- | --- |
| Toolchain | pinned Xuantie gcc 14.1.1 (V3.0.2 B-20250410), SDK sysroot `k230_pocketos_defconfig` |
| `make all` as `pocketos.mk` builds it (`ENABLE_SX1262 POCKETCAM_JPEG ZABBIX_CURL BROWSER_CURL BROWSER_IMAGES ENABLE_MESHCORED`), `-Werror` | rc 0, 0 first-party warnings, after fix `301fadf` (below) |
| DRM shell (CMake, sysroot LVGL) | rc 0, 0 warnings |
| `pos-browser` | 106 KB (93 KB stripped); NEEDED libcurl.so.4, libjpeg.so.9, libpng16.so.16 |

Two integration defects were found and fixed. The cloud branch had only
seen Ubuntu gcc 13 and a host with the image libraries installed:

1. `301fadf`: **the Xuantie gcc 14 stopped `make all`** in
   `core/web/web_image.c` (`scaler_row`). It vectorizes the RGB565 output
   loop for RVV (`-march=rv64imafdcv…`) with a masked division, then reports
   the masked-off lanes as `-Wmaybe-uninitialized`.
   - `-fno-tree-loop-vectorize` silences it; rewriting the arithmetic does
     not.
   - The fix marks that one loop `#pragma GCC novector`, for gcc ≥ 14 only.
     The output is unchanged; the image suites pass.
2. `e8999f6`: **`make test` in the default configuration** (`BROWSER_IMAGES=0`)
   stopped on `-Werror` in `tests/web_image_test.c`: `mutate_rounds()` is
   unused without a decoder. It and `rnd()` are now under the same guard as
   their callers.

## 2. The device provides what Browser needs

| What | Found | Class |
| --- | --- | --- |
| libcurl | `libcurl.so.4.8.0` = 8.12.1 (sysroot `curlver.h`), resolved by `ldd /usr/bin/pos-browser` | VERIFIED |
| OpenSSL | `libssl.so.3` / `libcrypto.so.3`, 3.4.1 (sysroot); CA store `/etc/ssl/certs`, 281 entries | VERIFIED |
| libjpeg | `libjpeg.so.9.6.0` (IJG 9f) | VERIFIED |
| libpng | `libpng16.so.16.46.0` (1.6.46) | VERIFIED |
| `LV_USE_SPAN` | `1` in the sysroot `lvgl/lv_conf.h`; the sysroot `liblvgl.so.9` exports 26 `lv_spangroup_*`; the device `liblvgl.so.9.5.0` holds `lv_spangroup_get_span_by_point`; pages with links draw on the unit (§3) | VERIFIED |
| Features | `pos-browser features` → `net,img:jpeg,png` | VERIFIED |

`pos-browser dump https://example.com` read the page in 0.19 s, and
`https://badssl.com/` loaded. Certificates are checked. The helper refused
each bad one with the libcurl reason:

- `expired.badssl.com`: "certificate has expired"
- `wrong.host.badssl.com`: "no alternative certificate subject name matches"
- `self-signed.badssl.com`: "self-signed certificate"
- `untrusted-root.badssl.com`: "self-signed certificate in certificate chain"

## 3. On the unit

| # | Check | Result |
| --- | --- | --- |
| 1 | Opens in landscape (1232×568) and portrait (568×1232); launcher icon loads (`art: icon-browser 96x96`) | PASS; start page 19-23 ms, no helper until the first page |
| 2 | URL entry, landscape: KEYS brings the touch keyboard, `example` + `?123` `.` + `com`, DONE goes | PASS |
| 3 | **Physical keyboard in landscape** | **PASS (owner, on `301fadf`)**: typing into the address field, Enter goes |
| 4 | URL entry, portrait: a tap on the field brings the touch keyboard; `neverssl.com`, DONE goes, keyboard goes away | PASS |
| 5 | `https://example.com` | PASS: SECURE, 3 blocks, 1 link, page made in 0 ms |
| 6 | HTTPS validation | PASS (§2) |
| 7 | Link | PASS: example.com "Learn more"; Wikipedia "Search" → `Special:Search` (52 blocks, 7 ms) |
| 8 | Scrolling | PASS: swipes scroll a 623-block page and open no link |
| 9 | BACK / FORWARD / RELOAD / HOME | PASS: BACK to the previous page, FORWARD enabled and returns, RELOAD makes the page again, HOME shows the start page. STOP (✕) replaces RELOAD while loading. STOP itself was exercised only by closing the app mid-load (§4, cycles 5/10/15/20). |
| 10 | Bookmark survives restart | PASS: `+` on a Wikipedia page → `/var/lib/pocketos/browser/state` (0600, dir 0700). Still listed after an app close/reopen, after the shell's rotation restart, and after `S90doors-shell restart`. |
| 11 | Wi-Fi drop during a load, recovery | PASS (§5) |
| 12 | Rotation both ways while open | PASS: landscape→portrait with a page, and portrait→Automatic (landscape) with Wikipedia open. Each time the shell restarted in place (same pid), the helper and its `/run/pocketos/browser.*` went, and the shell came back home as for every app. Browser reopened fine in the new orientation. |
| 13 | 20 open/close cycles | PASS (§4) |
| 14 | No orphan `pos-browser`, no stale `/run/pocketos/browser.*` | PASS after every close, rotation and service restart |
| 15 | No crashes, no supervisor restart growth | PASS: 0 crash files; `supervise-doors-shell.log` unchanged by the cycles (only the gate's own service restart added its line); `dmesg` clean of segfault/OOM/unhandled signal; shell.log has no ERROR; pos-browser.log has no WARN/ERROR in 25 sessions |
| 16 | Pictures | PASS: a JPEG (Wikimedia `JPEG_example_flower.jpg`, 36 KB) and a PNG (Google logo, 13 KB) decoded by the helper and drawn |

On the IANA link: example.com's only link, `iana.org/domains/example`,
redirects `https://www` → **`http://www.iana.org/help/example-domains`**
(checked with `curl -I`). Browser stops there with "Insecure redirect not
followed", as designed (never https → http). It is the site's redirect, not
a defect. The error page shows only the target's origin.

## 4. Twenty open/close cycles

Each cycle ran the same steps:

1. `doors app start browser` in portrait.
2. A tap on the Example bookmark, 3 s to load. Every 5th cycle instead
   tapped a 10 s delayed request and closed after 1.5 s, with the load
   running.
3. `doors app home`.
4. Checks after 1.5 s.

| | Start | After 20 |
| --- | --- | --- |
| doors-shell pid | 823 | 823 |
| doors-shell RSS | 15 104 KB | 15 232 KB (one +128 KB step at cycle 16, then flat) |
| `pos-browser` after each close | - | none, all 20 |
| `/run/pocketos/browser.*` after each close | - | 0, all 20 |
| supervisor lines / crash files / zombies | 10 / 0 / 0 | 10 / 0 / 0 |

Helper RSS while open: 6.9-7.2 MB.

## 5. Wi-Fi drop during a load

The unit has Ethernet and Wi-Fi on the same LAN, with the default route on
eth0 first. The test ran detached on the unit (`wifi_drop_unit.sh`):

1. It removed eth0's default route, so internet went over wlan0 only.
2. It tapped a bookmark for a request the server answers after 10 s.
3. 4 s later it ran `doors wifi off`.

| Time (Z) | Event |
| --- | --- |
| 06:48:59 | `open https://httpbin.org` (load running, helper up) |
| 06:49:03 | Wi-Fi off |
| 06:49:20 | `failed: timeout`. The page says "No answer in time … the network may be slow or the connection lost" with TRY AGAIN; the helper stays up (same pid) |
| 06:49:51 | `doors wifi on`; connected by 06:49:55 |
| 06:49:59 | RELOAD. The request completed over Wi-Fi: the server's JSON answer came back and Browser declined it as not a page. |
| 06:50:19 | HOME, then Example: page made, SECURE |

The eth0 default route was then restored in its original order (eth0 first,
then wlan0) and checked with `ping`.

Not done: `netd` restart during a load, and a page opened with no network at
all.

## 6. Performance on the C908

| What | Value |
| --- | --- |
| Start page | 19-23 ms |
| example.com, page made | 0 ms, 1 tick |
| Wikipedia Main Page (623 blocks, 500 links, 10 pictures, 42 KB text) | 204-357 ms over 6-9 ticks |
| doors-shell RSS | 14.8 MB with example.com; 15.4-15.7 MB with Wikipedia; 15.1-15.2 MB across the 20 cycles |
| pos-browser RSS | 7.0 MB after an HTTPS page; 8.0 MB with Wikipedia; 7.3-7.5 MB with one picture |
| CPU, Wikipedia open and idle | doors-shell 0-1 %, pos-browser 0 % (`top`) |
| CPU while loading/building Wikipedia | doors-shell peak 18 %, pos-browser peak 8 % (1 s samples) |
| CPU while swipe-scrolling | doors-shell 3-7 %. The rest of the total in those samples is the python touch injector. |
| Free memory during the gate | about 824-896 MB available |

Frame rate while fling-scrolling and the 800-block page were not measured.

## 7. Not covered here

- STOP tapped on a running load; HTTPS before NTP ("Clock not set"); netd
  restart during a load; frame rate.
- An upstream outage of about a minute (07:14-07:16Z) failed curl and
  Browser alike. It was not the unit (ping and example.com fine right after).
  The picture check was repeated after it.
- `make test`: `build_deps_test` fails on `core/pocketcam/pocketcam_exif.c`,
  **identically on master `2d24cef`**, from the camera gallery. It is not
  Browser's and was left alone. Every other step passes.
