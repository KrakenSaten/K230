# MeshScope for DOORS

First implementation, under review. A local MeshCore traffic analyser inspired
by CoreScope's packet/map/node workflow, implemented with DOORS code. It is not
a port of the upstream Go server or JavaScript UI and contains no upstream
CoreScope code or artwork. Original code is Apache-2.0.

Open **Apps > MeshScope**. `meshcored` must already be enabled on the unit.
Opening this app never enables a service, changes radio configuration or sends
a packet. RIFT can keep its own background session; the two clients have
separate subscriptions and caches.

## First version

- **MAP** places nodes at their advertised positions using RIFT's Web Mercator
  projection. Drag to pan; +/− zoom; FIT follows the current node extent.
  The latest received flood path is highlighted for eight seconds. Tap a node
  for its details. No position is invented for a node without coordinates.
- **MAP ON/OFF** controls the existing OpenStreetMap tile helper. Off by default;
  the coordinate map and packet list need no network. Tiles require a working
  network and the existing `pos-browser` helper. Attribution stays visible while
  the basemap is on. Leaving MAP stops its helper; closing the app frees it.
- **PACKETS** shows receptions, including duplicates, time, type, size, RSSI,
  SNR and hop count. Tap a row for the full existing RX-log detail. A selection
  is copied, so new traffic or eviction cannot silently replace its detail.
  SHOW PATH opens that packet directly on the map; LIVE returns to live data.
- **NODES** lists the current cached nodes, with details for last heard, available
  signal readings, advertised position and public key. Path participation is a
  count within the held packet window, not a lifetime counter.
- Both orientations, shared theme roles, Large text, scrollable lists/details,
  keyboard-focusable controls and the shell's Back action.

## Map layout and controls

The map owns a stable viewport. Landscape uses a right-hand inspection panel;
portrait places the same panel below the map. The panel scrolls independently
when its text does not fit, so incoming packets, connection messages and text
wrapping cannot move or shrink the map. FIT and LIVE have visible active states.
The coordinate view includes a geographic grid, a distance scale and highlighted
route nodes. OpenStreetMap attribution is anchored inside the map when enabled.

## What the display can establish

This is one receiver's view. A short path hash is matched against the current
node list, using the reported one-, two- or three-byte prefix. Two matches are
ambiguous even if only one has a known position. Lines connect **adjacent**
resolved and located hops only; they never jump across a missing hop. Counts of
unknown, ambiguous and unlocated hops accompany the map. Even unique prefix
matches are inferred from the current node cache, not proof of identity.

A direct packet's path is its **remaining route**, so it is shown in packet
text but never drawn as a travelled path. TRACE path bytes are SNR values and
are likewise not mapped as nodes. Source/receiver endpoints are not appended
speculatively. RSSI/SNR are the local receiver's measurements, not measurements
for all the inferred links.

Capture lasts only while MeshScope is open. The bounded RX ring holds at most
1,000 entries including connection markers; old entries are evicted. The node
cache is bounded by RIFT's existing 1,000-node limit. Closing the app or rotating
through the shell's process restart clears the session. There is no disk history,
replay, MQTT export, remote CoreScope connection, channel-key configuration,
packet transmission or background capture in this version.

## Implementation

`apps/meshscope/scope_model.*` resolves path prefixes and calculates window
statistics. `scope_app.c` owns the UI and lifecycle. Existing `rift_model`,
`rift_rxlog`, `rift_ipc`, `rift_map` and `rift_basemap` implementations are reused;
MeshScope does not include RIFT's UI or use its live session. The IPC client is
polled every 100 ms; it only uses its automatic read/subscription path. The
screens refresh on log changes or once per second. No new package, service,
public API, toolchain change or hardware access is introduced.

## Validation

Host commands:

```sh
make tests/meshscope_test tests/rift_rxlog_test tests/rift_ipc_test tests/fake-meshcored tests/home_layout_test
./tests/meshscope_test
./tests/rift_rxlog_test
./tests/rift_ipc_test
./tests/home_layout_test
cmake -S ui/shell -B out/meshscope-shell
cmake --build out/meshscope-shell --target pocketos-shell meshscope_ui_test -j2
mkdir -p out/meshscope-shots
SHOTS_DIR="$PWD/out/meshscope-shots" out/meshscope-shell/meshscope_ui_test
SHELL_BIN="$PWD/out/meshscope-shell/pocketos-shell" SHOTS_DIR="$PWD/out/meshscope-shots" python3 tests/meshscope_shell_test.py
bash tests/style_lint.sh
```

The UI test feeds parsed fixtures to the production UI with its transport timer
paused. It checks both orientations at Large text, bounds, details, selection
surviving ring clearing, SHOW PATH/LIVE navigation and theme changes. It also
checks that alternating short and long information leaves the map bounds
unchanged, with minimum map heights in both orientations. It does not prove IPC integration.
The shell test separately drives a real simulated shell and scripted meshcored
over Unix sockets, then checks the recorded methods against a read-only allowlist.
The focused GitHub Actions workflow runs both layers and retains screenshots.

Local evidence (2026-10-24): host simulator and UI test built with the pinned
LVGL; model test passed; RX-log regression 71 checks passed; launcher geometry
369 checks passed; UI fixture test and style lint passed. Unix-socket tests are
blocked by this execution environment (`Operation not permitted`), so no local
IPC integration pass is claimed. Cross-compilation with the pinned K230 SDK and
the revised UI on a K230 have not been verified by this session. No release image has been built or
flashed. The owner tested the first version on a K230 on 2026-10-24 and reported that it
worked so far (**VERIFIED by owner report**, not a comprehensive hardware gate).
That test identified the shallow landscape map and text-driven movement addressed
by this layout revision. Performance of the revised layout on hardware remains
unmeasured.

The shared RIFT transport runs in read-only observer mode for MeshScope. An
explicit request allowlist rejects mutations, including repeater logout, so
opening or reconnecting MeshScope does not clear another app's remote session.
