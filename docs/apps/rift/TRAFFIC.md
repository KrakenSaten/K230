# RIFT MAP: TRAFFIC

Under review in PR #110 (not in v0.3.8). MeshScope's local traffic analysis,
delivered as a mode of RIFT's MAP rather than a separate launcher app.
Inspired by CoreScope's packet/map/node workflow and implemented with DOORS
code: no upstream CoreScope code or artwork. Original code is Apache-2.0.

## Where it is

**RIFT > MAP > TRAFFIC.** TRAFFIC is a button in MAP's bar, beside BASEMAP,
lit while it is on. It is not a sixth tab: the strip keeps ACTIVITY, NODES,
COMMS, MAP and SYSTEM. With TRAFFIC off, MAP is the ordinary map with its
NODE panel, exactly as before.

With TRAFFIC on, the panel beside the map (landscape) or under it (portrait)
becomes the **TRAFFIC** panel:

- **Status:** LISTENING, or SERVICE LOST · HELD PACKETS ONLY, or THIS
  MESHCORED KEEPS NO RECEIVE LOG; receptions, repeats, receptions in the
  last minute and how full the ring is (`RING n/1000`).
- **OBSERVED PATH · LIVE** or **· PICKED PACKET:** hops, how many resolved,
  and - whenever there are any - how many were unknown, ambiguous or had no
  location, and how many lines were drawn; then the packet's type, RSSI and
  SNR "AT THIS DEVICE".
- **PACKETS:** six receptions at a time, newest first, repeats included
  (time, type, state, size, RSSI, SNR, and HOPS, AHEAD for a direct packet's
  remaining route, or TRACE SAMPLES). **LIVE · NEWER · OLDER** under them.
- Tap a packet for its full RX LOG detail, with **‹ LIST** and **SHOW PATH**.
- A marker tapped on the map selects the node, as on the plain map (RIFT's
  one node selection, shared with NODES and NET); the panel names it and
  counts the held observed paths it is in. **NODE DETAIL ›** opens RIFT's own
  node detail under NODES. There is no second node browser.
- The panel ends with what the drawing can and cannot establish.

Back and Esc step out: the packet's detail to the list, then TRAFFIC off.

## Layout

The map keeps a fixed share of MAP whatever the panel says. In landscape the
panel is 400 px wide and as tall as MAP; in portrait it is 46 % of MAP's
height. Its words, list and detail scroll inside it, so incoming packets,
long decoded text and wrapping cannot move, shrink or grow the map. Map
controls stay in MAP's bar; the scale bar and, with BASEMAP on, the
OpenStreetMap attribution stay on the map. In portrait the zoom buttons and
FIT take the touch minimum (56 / 64 px) so TRAFFIC fits beside BASEMAP; the
shapes legend moves to the NODE panel, as it already did beside BASEMAP.

Nothing in TRAFFIC moves the map except **SHOW PATH**, a reader's press,
which fits the picked packet's located hops into view; after it, as after a
pan or zoom, arriving packets leave the map where it is until FIT. The one
thing that changes MAP's height is RIFT's own command line appearing when
meshcored stops answering, as it does on every RIFT screen.

The panel is built the first time TRAFFIC is turned on; the plain map costs
one more object for the button.

## Observed paths are not routes

TRAFFIC draws what this receiver heard. The Visual Path Builder
(VISUAL_PATH_BUILDER.md) prepares a route to send a message on. They are
kept apart:

- The panel says OBSERVED PATH, and its note says the path is inferred,
  is not a verified route and is never used to send.
- Nothing in TRAFFIC calls the route model or the send path
  (`tests/rift_lint.sh` checks it). An observed path cannot become a route.
- A route draft open in the builder is **kept, not drawn** while TRAFFIC is
  on: the builder's panel, numbers and lines are hidden, map taps select
  nodes instead of adding relays, and the panel says ROUTE DRAFT KEPT with
  **BACK TO ROUTE**. TRAFFIC off shows the builder again with the draft as it
  was. **SET ROUTE** from a conversation always opens the builder, never
  TRAFFIC. NODE DETAIL is not offered while a draft is kept, because leaving
  MAP is the builder's CANCEL (as it always was).
- The composer's text is untouched by any of this.

## What the drawing can establish

This is one receiver's view, from RIFT's RX LOG ring (`rift_rxlog.h`):

- A flood's path hashes are matched against RIFT's node list by prefix, at
  the width the packet reported (one, two or three bytes). No match is
  UNKNOWN; two or more is AMBIGUOUS, whether or not the others have a
  location. A unique match is still an inference from the node cache.
- A line joins two **adjacent** hops only when both resolved to one node
  with an advertised location. A missing hop is never bridged, and neither
  this device nor the sender is added as an endpoint: the path gives neither
  place.
- A direct packet's path is the **remaining** route, not one travelled, and
  is never drawn. TRACE path bytes are SNR samples, never node identities.
- RSSI and SNR are this device's reception of the packet, not of each link.
- LIVE is the newest flood path heard in the last 8 s, and never one from
  before a service gap (a mark in the ring).

## Data, retention and lifecycle

TRAFFIC opens nothing of its own. It reads RIFT's session: the model's node
cache and the RX LOG ring that RIFT's one meshcored subscription has always
filled (`mesh.subscribe {"rx_log": true}`).

- **Retention:** the newest 1000 ring entries (receptions and RIFT's own
  service-gap marks), about 360 KB allocated once with the session. The ring
  fills from the moment RIFT's session starts, including while TRAFFIC is
  off, while RIFT is on another tab and while RIFT runs in the background.
  It is emptied by RX LOG's CLEAR and is gone with CLOSE RIFT or a restart of
  the shell - a rotation re-executes the shell, so it is one. There is no
  disk history, replay, export or background service.
- **The picked packet** is a copy: new packets, eviction and CLEAR leave its
  detail and path as they were. Picking a packet holds the list's place;
  LIVE lets go. If a thousand newer receptions push the held place out of the
  ring, the list returns to the newest and says so.
- **Node resolution** is done again on every draw against the node list as
  it stands, so a picked packet's hops follow node-list changes.
- **Service loss:** the panel says SERVICE LOST · HELD PACKETS ONLY and keeps
  the ring; the ring records the gap. On reconnect RIFT's client
  resubscribes as it always does; TRAFFIC adds nothing to that.
- **Ownership:** no timer, subscription, socket or tile helper of its own.
  It is repainted by RIFT's refresh (on every change and once a second);
  the basemap is MAP's single helper. The screen half - panel, selection,
  list place, TRAFFIC on or off - is built with MAP and released with it
  when RIFT is left; reopening RIFT starts with TRAFFIC off.

## Passive behaviour

Turning TRAFFIC on or off, picking packets, paging, SHOW PATH, LIVE, map
controls and tab changes ask the service for nothing. Tapping a marker is
RIFT's ordinary node selection, which reads that node (`mesh.node`). RIFT's
own explicit sends and management actions (COMMS, SYSTEM, repeater control)
are unchanged and are not reachable from the TRAFFIC panel. RIFT is not put
into any read-only mode.

## Implementation

| File | |
| --- | --- |
| `apps/rift/rift_scope.c/.h` | the analysis: prefix resolution at the reported width, travelled path or not, the live path, hop counts and drawn segments, window counts, a node's path count. No LVGL, no connection |
| `apps/rift/ui/rift_scope_view.c/.h` | the TRAFFIC button, panel and drawing over MAP's canvas |
| `apps/rift/ui/rift_mapview.c` | hosts it: the button in the bar, the panel in place of NODE's, the drawing after the markers |
| `apps/rift/rift_map.c` | `rift_map_fit_nodes`, for SHOW PATH |

The standalone MeshScope app, its launcher entry and the observer-only mode it
needed on a second RIFT client are gone: one client, one subscription.

## Validation

See PR #110 for the run record. Host and simulator layers:

- `tests/rift_scope_test.c` (`make tests/rift_scope_test`): resolution
  and collisions at each width, flood/direct/TRACE semantics, the live path
  and service gaps, segment counting without bridging, window counts, map
  fitting to a path.
- `tests/rift_app_test.c`, `RIFT_APP_TEST_ONLY=traffic` (also in the full
  run): against the scripted meshcored on a real socket with its receive
  log - TRAFFIC from MAP with the five tabs unchanged, a reception through
  RIFT's own single subscription, a route draft and the composer kept, live
  path lines and rings, map and panel bounds unchanged under ten alternating
  long and short packets, a panned map not moved by arrivals, a pick
  surviving arrivals and CLEAR, SHOW PATH and LIVE, node selection, tabs
  without duplicate objects or timers, BACK TO ROUTE, the plain map, both
  orientations at Large, Back, service loss and return, close and reopen,
  CLOSE RIFT. The service's own method log is checked: only reads.
- `tests/rift_traffic_shell_test.py`: the real simulator shell with the
  scripted meshcored, both orientations at Large, the method log checked.
- `tests/rift_lint.sh`: TRAFFIC calls no send, write, route or management
  path, and its analysis has no LVGL or connection.

### Hardware

- The first standalone MeshScope was reported working on a K230 by the owner
  (2026-10-24, **VERIFIED by owner report**).
- **Unit B, 2026-10-26, test shell 17bb30ac** (cross-built with the pinned
  Xuantie 14.1.1 toolchain against the installed QR-enabled LVGL, run from
  `/root/rift-traffic-17bb30ac` with the established start/restore kit, not
  installed): RIFT opened over IPC; the owner tapped MAP, TRAFFIC, dragged the
  map and selected a marker in landscape at Large (Neon theme, BASEMAP on) and
  again in portrait after an IPC rotation, and reported both looked right.
  Device screenshots show the TRAFFIC panel beside and under the map with the
  OpenStreetMap attribution and scale bar on the map. meshcored's counters
  stayed at `tx_submitted 0` throughout. `RESTORE: OK`: installed shell,
  LVGL, meshcored (same process), settings.conf and RIFT prefs as before.
  **VERIFIED** for layout, touch entry and no transmission.
- **Not verified on hardware:** behaviour under incoming traffic. The unit
  heard nothing in the hour before and during the check (`rx_events 0`), and
  no traffic was manufactured. Packet rows, picking, SHOW PATH and drawn
  paths are verified in the simulator and against the scripted service only.
