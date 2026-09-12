# wifi.* API v0 (netd)

Status: draft, api_version 0. Transport: pocketipc, socket `netd.sock`.

Started on the device by `/etc/init.d/S55netd` under `pos-supervise`.
`netd` is the network service ADR-002 names; v0 serves Wi-Fi for one wireless
interface (default `wlan0`, `NETD_INTERFACE` in `/etc/default/netd`).
Ethernet is still brought up by the vendor's ifupdown (`S40network`), and
`system.status.network` (docs/api/system.md) keeps reporting every interface.

netd starts and owns `wpa_supplicant` and the DHCP client (BusyBox `udhcpc`)
for that interface, and keeps the networks a person has joined under the
rules of `docs/decisions/ADR-003-wifi-credentials.md`. Hardware facts behind
it: `docs/hardware/WIFI_2026-09-12.md`.

## The shape of a request

Every method answers at once. Scanning and joining take seconds, so they are
started by a request and followed through `wifi.status` / `wifi.networks`.
That is what lets the shell call netd from the LVGL thread under its 200 ms
UI deadline.

Requests that change something reject parameters they do not know (code 2),
as `system.reboot` does. Clients must ignore unknown result fields.

SSIDs are bytes. Every SSID in a result comes twice: `ssid`, display text
(valid UTF-8, with any invalid byte or control character shown as `?`), and
`ssid_hex`, the exact bytes. Requests take one of `ssid` (1..32 bytes of
UTF-8 text) or `ssid_hex` (2..64 hex digits). A zero-length or all-NUL SSID is
a hidden network and is never listed or accepted by name.

## Methods

### wifi.status

No parameters.

| Field | Type | Meaning |
| --- | --- | --- |
| api_version | int | 0 |
| interface | string | the wireless interface netd manages |
| available | bool | false when there is no such interface or another wpa_supplicant owns it |
| enabled | bool | the user's on/off choice, remembered |
| state | string | `unavailable`, `off`, `starting`, `disconnected`, `connecting`, `obtaining_ip`, `connected`, `failed` |
| reason | string or null | with `unavailable`: `no_interface`, `interface_busy`; with `failed`: `auth_failed`, `not_found`, `assoc_failed`, `timeout`, `dhcp_failed`, `supplicant_failed` |
| ssid, ssid_hex | string or null | the network being joined or joined |
| bssid | string or null | access point, when associated |
| frequency_mhz | int or null | when associated |
| signal_dbm, signal_bars | int or null | RSSI of the association, and 0..4 bars (≥ -55, -66, -77, -88 dBm) |
| ipv4 | string or null | the interface's address once DHCP has bound |
| security | string or null | security of the associated network when it is a saved one |
| scanning | bool | a scan is in progress |
| saved_count | int | networks in the store |
| store | string | `ok`, `damaged` (unparseable, kept aside at the next change), `unreadable` (never overwritten), `unwritable` (the last write failed) |
| wpa3_supported | bool or null | whether the driver reports SAE; null until wpa_supplicant answers |

`failed` is kept until the next `connect`, `disconnect` or `set_enabled`, or
until an association completes. `dhcp_failed` keeps the association (`ssid`,
`bssid`) visible; the DHCP client keeps trying in the background and a later
lease turns the state into `connected`.

### wifi.set_enabled

Params `{enabled: bool}`, required. Turns Wi-Fi on or off and remembers the
choice. On starts wpa_supplicant and joins saved networks automatically; off
stops the DHCP client (releasing the lease) and wpa_supplicant, and brings the
interface down. Result: as `wifi.status`. Error 4 when the choice could not be
stored, or when the store could not be read at start (it is never replaced).
Wi-Fi is **off** on a device that has never been turned on.

### wifi.scan

No parameters. Starts a scan. Result `{scanning: true}`; results arrive in
`wifi.networks`. Error 6 when unavailable, 3 when off, 5 while wpa_supplicant
is still starting, 4 when it did not accept the scan.

### wifi.networks

No parameters. The last scan.

| Field | Type | Meaning |
| --- | --- | --- |
| scanning | bool | a scan is in progress |
| scan_failed | bool | the last scan did not complete (driver refusal or 15 s timeout) |
| age_s | int or null | seconds since the last results; null before the first |
| hidden_count | int | BSSes with no SSID, not listed |
| networks | array | one entry per SSID, strongest first |

Each network: `ssid`, `ssid_hex`, `signal_dbm`, `signal_bars`,
`frequency_mhz` (of its strongest BSS), `security` (`open`, `wep`, `wpa`,
`wpa2`, `wpa2/wpa3`, `wpa3`, `owe`, `enterprise`), `supported` (whether netd
can join it on this hardware), `needs_passphrase`, `saved`, `connected`.
Access points that share an SSID are one network; its strongest BSS gives
the signal, frequency and security.

### wifi.connect

Params: `ssid` or `ssid_hex` (required), `passphrase` (string),
`allow_open` (bool), `hidden` (bool), `security` (string, only with `hidden`).
Result `{accepted: true, ssid, ssid_hex, security}`; follow `wifi.status`.

- The network must be in the last scan, unless `hidden` is true, in which case
  `security` (`open`, `wpa`, `wpa2`, `wpa3`) says what it is.
- `wpa`, `wpa2` and `wpa2/wpa3`: a passphrase of 8..63 printable ASCII
  characters. Omitted or empty for a **saved** network, the saved passphrase
  is used.
- `open`: refused with code 3 unless `allow_open` is true. netd never joins an
  unencrypted network on its own. A passphrase with it is code 2.
- `wep`, `enterprise`, `owe`, and `wpa3` on a driver without SAE: code 6.
- A new request replaces a join still in progress.
- On success the network is saved (most recent first); on `auth_failed` it is
  removed from wpa_supplicant and nothing is saved.

Error 2 for any malformed or oversized parameter, 3 for the open-network
policy and when off, 5 while starting, 6 when unsupported or unavailable, 4
when wpa_supplicant refused the network. The passphrase is never echoed in an
error.

### wifi.disconnect

No parameters. Leaves the current network and cancels a join in progress. The
network stays saved; netd does not rejoin it until the next `connect`, the
next `set_enabled` on, or the next start. Result: as `wifi.status`.

### wifi.forget

Params: `ssid` or `ssid_hex`. Removes a saved network from the store and from
wpa_supplicant (leaving it if connected). Result `{forgotten: true, persisted,
saved_count}`; `persisted` false when the store could not be written. Error 2
for a network that is not saved.

### wifi.saved

No parameters. `{networks: [{ssid, ssid_hex, security, hidden}], store}`.
Never a passphrase.

## Errors

| Code | Meaning |
| --- | --- |
| 1 | unknown method |
| 2 | invalid or malformed parameters |
| 3 | policy: Wi-Fi is off, or an open network without `allow_open` |
| 4 | wpa_supplicant or the store failed |
| 5 | busy: Wi-Fi is starting |
| 6 | unsupported: no interface, interface owned by another wpa_supplicant, or a security netd cannot join |

A frame that is not valid JSON closes the connection; netd stays up.

## Behaviour worth knowing

- **Reconnect.** With Wi-Fi on, every saved network is handed to
  wpa_supplicant at start and it joins the best one in range by itself, after
  a reboot, a netd restart, a supplicant crash or the interface coming back.
- **Supplicant health.** A wpa_supplicant that exits or stops answering is
  restarted with backoff (2, 4, 8, 16, 32 s); after five failures without a
  minute of running the state is `failed` / `supplicant_failed` until the
  next `set_enabled`.
- **Foreign supplicant.** If a wpa_supplicant netd did not start names the
  interface (for example the vendor's `ifup wlan0` stanza), netd reports
  `unavailable` / `interface_busy` and does not start a second one. It checks
  again every two seconds.
- **Timeouts.** A join that neither completes nor fails in 30 s ends as
  `timeout` (or `not_found` when the network never answered); DHCP has 20 s
  from association before `dhcp_failed`. Both are `--connect-timeout-s` /
  `--dhcp-timeout-s` options of netd.
- **Routes and DNS** come from BusyBox's default udhcpc script, which tags its
  resolv.conf lines per interface. It adds a default route per interface
  without a metric, so with Ethernet and Wi-Fi both up the second route may be
  refused or may share the first's metric; which interface carries traffic is
  not managed in v0 and is unmeasured.
- **Regulatory domain** is not set by netd; the driver uses its built-in
  channel plan (docs/hardware/WIFI_2026-09-12.md).

## Trust model

As for system.*: `netd.sock` is 0660 in the 0770 runtime directory, every
PocketOS process runs as root, and anything that can reach netd is already
root. What netd adds is ADR-003: the passphrase stays out of logs, results,
command lines and the environment, and the store is root-only.

## Clients

- `pos wifi status | on | off | scan | list | connect <ssid> [--ssid-hex]
  [--open] [--hidden SECURITY] | disconnect | saved | forget <ssid>
  [--ssid-hex]`. `connect` reads the passphrase from standard input (echo off
  on a terminal); an empty line rejoins a saved network.
- `pos call netd wifi.status`. Note that `pos call ... passphrase=...` puts the
  passphrase on pos's own command line; use `pos wifi connect`.
- The Settings app (apps/settings).

## Test hooks

`tests/netd-testhooks` is netd with `services/netd/netd_sys.c` compiled with
`-DNETD_TEST_HOOKS=1`: it reads `NETD_TEST_ROOT` (fake `sys/class/net` and
`proc`), `NETD_TEST_WPA_SUPPLICANT` and `NETD_TEST_UDHCPC`. The shipped
`services/netd/netd` does not contain those names (`tests/netd_test.sh`
checks with `strings`).

## Not in v0

Events (`wifi.subscribe`), Ethernet management, static IPv4, IPv6, route
metrics, proxy settings, enterprise and WEP networks, WPS, access-point mode,
a regulatory-domain setting, and more than one wireless interface.
