# RIFT

The mesh client for Doors: what this node is, what state its radio service is
in, which nodes it has heard, and how a packet would get to one of them.

**Status:** phase 1, on branch `feat/rift-ui-phase1` (2026-09-19). Built for
riscv64 (`make all` and the DRM/sysroot shell, 0 first-party warnings). Run
on the panel against a live mesh: **PASS on unit A, 2026-09-19, build
`d19146b`** (`docs/hardware/RIFT_PHASE1_BENCH_GATE.md`), not merged.
**Nothing in it transmits**, and nothing did — the service counted
`tx_submitted=0` across the whole session. It reads meshcored and draws what
meshcored says.

The design is `docs/design/rift/HANDOFF.md`, approved 2026-09-19; that
package is the contract and this phase implements part of it.

## What it is

Four sections, in the design's fixed order: **ACTIVITY · NODES · COMMS ·
NET**. Phase 1 draws the first two.

- **ACTIVITY** — the radio service's state and the reason for it in the
  service's own words, whether radiod is connected and what state it
  reported, whether the lease is held and whether the node can transmit, how
  many nodes the service holds, which build of meshcored is answering, this
  device's own name, hash and key, the nodes heard most recently, and the raw
  frame feed underneath.
- **NODES** — every node the service holds, as 36 px rows: link glyph, name,
  role, hop strip, hop count, RSSI, SNR (landscape), last heard. Grouped into
  heard within 12 h, not heard for longer, and never heard. A row selects and
  does nothing else; the selection expands in place into the state line, the
  path written out, the signal, and a 56 px action bar. **DETAIL** pushes a
  screen with the link state, the identity, the hop ladder and the path
  changes RIFT has seen.
- **COMMS** and **NET** keep their place in the navigation and say they are
  not in this build. An empty list would read as a quiet mesh.

Landscape (1232 × 568) is a recomposition, not a rotation: NODES becomes the
list beside the selected node's detail, ACTIVITY becomes two columns, and the
extra SNR column appears. Nothing exists in landscape that portrait cannot
show. The split is chosen from the room the app is given, not from the
orientation, so a body shrunk by a keyboard sheet keeps the single column.

## What it is not

- **It does not transmit.** `mesh.send` and `mesh.advert` are the only two
  methods in the API that put a packet on the air, and nothing under
  `apps/rift` names either. `tests/rift_lint.sh` checks the source;
  `tests/rift_ipc_test.c` checks it from the other end, by recording every
  method the service was asked for over a whole run.
- Not a message client: no composer, no conversations, no channels. That is
  COMMS.
- Not a network map: no rings, no relay load, no inferred links. That is NET.
- No command parser. The command line is drawn because it is permanent chrome
  in the approved design and the vertical budget is measured with it there,
  and it is in the DS §9 disabled treatment saying what it is waiting for.
- It stores nothing, opens no device, links no radio or protocol library, and
  owns no colour.

## What it shows, and what it refuses to show

meshcored leaves out what nobody measured (docs/api/mesh.md), and this app
keeps it left out.

| On screen | Means |
| --- | --- |
| `?` in `text_muted` | the service could report this and has not: an RSSI nobody measured, a hop nobody named, an age with no timestamp behind it |
| `—` | the value cannot exist at all: an end-to-end RSSI over relays |
| `DIR` | zero relays. Never `0` |
| `NO PATH` | no route back is known. Not zero hops |
| `NEVER HEARD` | no timestamp at all, which is not the same as heard long ago |
| "cached" | meshcored is not answering and what is on screen is the last thing it said |

A node heard in the future - meshcored's monotonic clock ahead of this app's,
which can only be a fault - is shown as an unknown age and never as a time to
come.

## The path

The full hop list is kept and never truncated; only the drawing compresses.
More than four relays draws the first two, a `+n` cell and the last one, and
the numeric hop count is always printed in its own column beside it.

MeshCore packs a whole number of bytes per hop, so a path divides exactly by
its hop count. When it does not - the path was clipped at `MCD_MAX_PATH`, or
the count and the bytes disagree - the hops are real but none of them can be
named, and they are drawn as unknown rather than sliced into plausible
pieces. The state line then says so: `RELAYED · 3 HOPS · 3 UNKNOWN HOPS`.

A hop hash is named only when exactly one node in the cache matches it. Two
nodes sharing a first byte resolve to nothing, for the same reason
`mesh.node` refuses an ambiguous prefix: a guessed name on a hop is a wrong
route drawn confidently.

## How it talks to meshcored

One connection, asynchronous throughout (`apps/rift/rift_ipc.c`).

Requests are written and forgotten; the reply is matched by id when it turns
up. That is not a flourish - it is what keeps two promises at once. Nothing
blocks the LVGL thread, because there is no call to block in: the only
bounded wait in the module is the connect, and it is shorter than one frame.
And nothing is lost, because `pocketipc_call` discards events while it waits
for its reply, so a client that subscribes *and* calls on one connection
drops whatever arrives during a call.

Consumed: `mesh.info`, `mesh.status`, `mesh.identity`, `mesh.nodes`,
`mesh.node`, `mesh.subscribe`, `mesh.unsubscribe`, and the `mesh.state`,
`mesh.node` and `mesh.activity` events. `mesh.messages` is COMMS and is not
read; a `mesh.message` event that arrives anyway is ignored without being
counted as a fault.

When the service goes away the nodes stay, marked as cached, and the client
reconnects with a backoff doubling from 0.5 s to 5 s - meshcored's own
ceiling is 30 s, which is right for a daemon and wrong for a screen somebody
is looking at. The next connection re-reads the snapshot, so a node the
service has forgotten stops being shown here.

A malformed event is refused whole, counted, and never half-applied, and it
does not drop the connection: one bad event costs one event, and a peer on
the air must not be able to disconnect this app from its own service.

## Structure

| File | |
| --- | --- |
| `rift_model.c/.h` | what is known and how sure it is: the bounded node cache, the activity ring, the service state. No LVGL |
| `rift_format.c/.h` | every string the screens print, and the path arithmetic. No LVGL, no cJSON, no I/O |
| `rift_ipc.c/.h` | the meshcored connection, the framing and the reconnect. No LVGL |
| `rift_app.c/.h` | chrome, sections, layout and lifecycle |
| `ui/rift_widgets.c` | the link glyph, the hop strip, the panel with its caption in the rule, the action bar |
| `ui/rift_activity.c` | ACTIVITY |
| `ui/rift_nodes.c` | the node list, the selection and the landscape split |
| `ui/rift_detail.c` | the selected node in full: one builder for the landscape pane and the portrait DETAIL screen, so the two cannot drift |

## Tests

| | |
| --- | --- |
| `tests/rift_format_test.c` | 92 checks: ages, signal, hop columns, state words, path compression, the inline chain, the ladder, UTF-8 names |
| `tests/rift_model_test.c` | 106 checks: the initial snapshot, duplicate and update events, missing telemetry, malformed input, the bounded cache, the service going away and coming back, ordering |
| `tests/rift_ipc_test.c` | 66 checks against a real socket and a scripted service in a child process: connect, snapshot, events, refusals, the service disappearing, reconnect, and the proof that nothing asked it to transmit |
| `tests/rift_app_test.c` | 98 checks under a real LVGL pointer device: the chrome, both sections, the row that only selects, the pushed detail, the landscape split, and open/leave/open again. Writes the screenshots |
| `tests/rift_shell_test.sh` | the app test, then the real shell opening RIFT in both orientations with a scripted meshcored on a real socket, then with no service at all, then the same fixtures twice for the same pixels |
| `tests/rift_lint.sh` | the boundaries: no transmit, no colour, no device, no store, no monolith, and the gaps this phase leaves |

`tests/fake-meshcored` is a scripted stand-in for the service, built by the
root Makefile and never installed. A negative `last_heard_mono_ms` in its
fixtures means "this long ago", so an age on screen does not depend on how
long the host has been up.

## Known gaps

1. **No launcher icon.** DS §20 wants an A8 tint mask on every launcher tile.
   The approved design package carries the RIFT mark as a design sheet
   (`docs/design/rift/shots/identity-sheet.png`), not as the png-32 artwork
   `tools/design/gen_app_icons.py` generates masks from, so the launcher
   draws the text icon. `tests/app_icons_test.sh` names RIFT as the one
   allowed exception and fails if a second app joins it or if RIFT quietly
   gains one without the generator being told.
2. **The last hop is not named.** The design's `LAST HOP −88 · RPT-NORD`
   needs to know which hop transmitted the frame that was heard. `mesh.*`
   reports the signal of the frame and not its sender, so the detail says the
   signal belongs to the last hop and does not say which one.
3. **Per-hop uncertainty is coarser than the design's.** The design marks an
   individual hop whose hash is missing from an advert path. The API gives a
   hop count and an opaque byte string, so this build can say that a path
   could not be divided into hops - all of them unknown - but not that one
   hop in the middle is.
4. **The chrome is Doors's.** The design's landscape chrome merges the header
   and the section strip into one 56 px row and shrinks the back slab. The
   status bar and the back slab are Doors-owned and RIFT changes nothing
   there, so the app lays out in the body it is given: 56 px of strip and
   56 px of command line inside it, in both orientations. The counts the
   design puts in the header's right caption are at the end of the section
   strip in landscape, and in the list's own footer in portrait.
5. **Path history is only what RIFT saw.** There is no history before the app
   opened, and the panel is headed "PATH CHANGES SEEN BY RIFT" so it is not
   read as the service's record.
