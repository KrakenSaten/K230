# RIFT Visual Path Builder — implementation proposal

Status: PROPOSED (not an approved ADR, not implemented on hardware)
Date: 2026-10-18
Owner: DOORS / RIFT
Companion: [interactive, **non-transmitting** concept](../../design/rift/visual-path-builder.html)

## Outcome

From a direct-message conversation, choose a sequence of repeater nodes on the
RIFT map. As each repeater is selected, draw its ordered connection from the
previous waypoint, number the hop, and update a readable path. The recipient
is fixed by the conversation; it must never silently change. The finished
route is passed to the **actual** MeshCore sender only after a deliberate SEND.

This is a user-selected *proposed forwarding path*, not a measurement of
radio connectivity, and it must be labelled that way. RIFT must never display
an exploratory drawn line as a proven RF link.

**Non-goals for v1:** automatic route optimisation, estimated link quality
or distance without evidence, mesh-wide neighbour discovery, modifying the
node's persistent learned route, routing channel messages, or background RF
probing.

## Existing implementation and the blocking gap

- `apps/rift/ui/rift_mapview.c` already supports touch hit-testing, marker
  drawing, drag-pan, zoom, FIT, and the optional OSM basemap. Its geographical
  marker set is limited to nodes with advertised coordinates.
- `apps/rift/rift_map.{c,h}` provides projection and hit-testing without LVGL.
- `apps/rift/rift_model.h` holds nodes under **full 64-character public keys**,
  plus claimed coordinates and learned `path_hex`. It does not expose an
  authenticated repeater-to-repeater adjacency matrix.
- `apps/rift/ui/rift_netview.c` provides a topology-oriented view built from
  **observed hop counts**, not physical neighbour edges.
- `apps/rift/ui/rift_thread.c` contains the composer and SEND action.
  `apps/rift/rift_ipc.h` promises a single intentional `mesh.send` writer.
- **Current protocol blocker:** `docs/api/mesh.md` defines `mesh.send` with
  `to` or `channel`, `text`, or a standalone `resend` id. There is no
  explicit path parameter; `services/meshcored/api.c` delegates routing to
  the runtime, which chooses flood vs learned direct route. Selecting nodes
  only in the UI currently cannot change what goes over the air.

Do **not** ship a `USE PATH` button connected to existing `mesh.send` as if
it used the chosen route. Introduce and test a backward-compatible explicit
route contract in `meshcored` first.

## Interaction contract

1. Open any **direct** conversation; compose or retain the message text.
   Use **SET PATH** next to the composer or from its route details. The
   current recipient is shown read-only in the builder.
2. Show the same cached node inventory as MAP/NODES. Selecting a repeater
   appends its **full public key** to the ordered draft, never a list index,
   short hash, or display name. The source is implicit (this device).
3. Immediately draw a line/arrow from the previous selected waypoint to the
   newly chosen repeater; label each waypoint with its sequence number.
   The segment toward the recipient is only a visually distinct **preview**
   until the user finishes the route. Text reads
   `YOU > RPT-NORD > RPT-7 > HYTTA` and `2 RELAYS`.
4. **UNDO** removes the last repeater/segment; **CLEAR** clears all repeaters;
   tapping a selected waypoint allows removing it and all following
   waypoints (explicit confirmation for truncation), or replacing it from
   that position. Duplicate nodes, the sender, and the recipient cannot be
   inserted as relays. Non-repeaters are not selectable as intermediate hops.
5. Tap the recipient or **FINISH PATH** to finish the candidate. A candidate
   with zero repeaters is legal only when the service validates an explicit
   zero-relay direct path; it must **not** be interpreted as a flood.
6. Service-side path preview validates against the **current** node table.
   `USE PATH` stages a validated candidate on the correct conversation;
   exiting without applying restores the original selection and text.
7. On **SEND**, revalidate against the same recipient and service
   capabilities, and attach the path to this one transmission. A stale,
   absent, ambiguous, overlong or otherwise invalid route **refuses** the
   submission without silently falling back to flood or a learned path.
   The text remains intact and an explicit error is shown.
8. The user may change back to **AUTO** at any time. AUTO uses the existing
   implementation unchanged. A channel conversation is always AUTO/FLOOD
   and does not show SET PATH.

The path editor must preserve draft state through layout recomposition and
must work at all supported screen sizes. Map interaction, pan/zoom, selector,
path list, and accessible actions have the same semantics on every display.
Respect the existing Doors theme tokens, focus semantics, and 56 px primary
touch targets. The map must not steal keyboard focus from a message composer
on return.

## Map/data semantics

- **Geographic mode:** draw nodes only where the node's advert supplied valid
  `lat` and `lon`. Never invent coordinates or plot an unlocated relay at
  an interpolated position. A selected but unlocated waypoint is visible
  in the ordered path list with a location-unknown badge, not fabricated
  on the basemap.
- **Topology mode:** offer a network diagram showing *all* cached nodes.
  Its layout coordinates are presentation-only, computed deterministically
  from public keys or stored user positioning. Clearly label it
  `TOPOLOGY — POSITIONS NOT GEOGRAPHIC`. Its drawn path edges are user
  intentions, not discovered RF adjacency.
- Maintain these evidence classes in the UI: `PLANNED` (every new segment),
  `OBSERVED ROUTE` (actual learned route data, if available), and
  `UNVERIFIED LINK` where two adjacent repeaters have no separately proven
  neighbour relationship. A hop count from an advert is not a discovered
  per-edge link.
- Show node name, full identity on demand, type, last seen, and RSSI/SNR
  only when the service supplied them. Last-hop RSSI/SNR is **not** an
  end-to-end or inter-repeater measurement.
- When the node list changes, retain waypoint **keys**, not array positions.
  If a waypoint disappears, mark the draft invalid and require review;
  never remap it to a different node.
- The route length bar shows both the count of repeater nodes and the
  encoded bytes after backend validation. Never infer that each hop is one
  byte: `mesh.path_hash` supports 1, 2, or 3-byte prefixes, and the MeshCore
  path has an existing bounded representation (`MCD_MAX_PATH = 64`).

## Suggested mesh service contract — to be reviewed before implementation

Feature discovery, optional result on `mesh.info`:

```json
{"capabilities": {"explicit_direct_path": true, "preview_direct_path": true}}
```

New *non-transmitting* `mesh.path_preview` request:

```json
{
  "to": "<64 hex public key of recipient>",
  "via": ["<64 hex repeater key #1>", "<64 hex repeater key #2>"]
}
```

Success should return a canonical route identifier (or compact path token),
ordered relay identities, effective hash width, packed byte count and the
service's validation evidence. The preview MUST NOT mutate learned paths,
create a packet, or reserve a TX slot; its result may be stale immediately.
The actual source of truth remains SEND-time validation.

Optional direct-message form of `mesh.send` **only after the backend is
proven**:

```json
{
  "to": "<64 hex recipient key>",
  "text": "Hello",
  "via": ["<64 hex repeater key #1>", "<64 hex repeater key #2>"]
}
```

The above `via` is a **proposal** for API review, not a supported method
today. Define and test the exact wire contract before RIFT enables it:

- `via` only with `to`, never with `channel` or `resend`. Missing `via`
  means existing AUTO behavior. An empty `via` explicitly requests a
  zero-relay direct route, not an automatic flood.
- Use full canonical keys; reject malformed keys, duplicates, repeated
  recipient, sender as relay, deleted nodes and non-repeater waypoints.
- The service resolves the selected full keys to MeshCore's **actual**
  encoded hashes and computes the packed route. Validate max path length,
  path-hash width, packet payload/airtime limits and any hash-prefix
  collisions that would make the requested ordering ambiguous. Reject,
  never silently route to the first colliding node. This must be checked
  using pinned upstream code, not guessed in RIFT.
- Verify the MeshCore upstream sending path can inject a **per-message**
  explicit route while retaining correct ACK/expected-ACK behaviour.
  Never temporarily overwrite a contact's learned path: another pending
  message or resend must not inherit the override; never persist the
  override as a learned route.
- Keep one-message-to-one-request association through replies, timeouts,
  reconnects and orphan resend. In particular, protect against overlapping
  RESEND actions and stale responses mutating another message's route.
- Return `route: "explicit"` (separate from `direct` and `flood`)
  with the canonical path metadata; add outgoing-message metadata only
  after the service actually accepted that path. Acceptance is not
  transmission; an ACK, not a drawn line, indicates delivery.
- Older services lacking the capability must offer only AUTO. A caller
  submitting `via` to an older service must receive a clear refusal,
  never silently discard the field.

The current handler's parameter validation also needs an explicit check
against unknown routing fields so legacy servers cannot misinterpret an
unsupported `via` request as ordinary AUTO.

## Implementation sequence

| Gate | Layer / likely paths | Deliverable | Proof |
| --- | --- | --- | --- |
| P0 | `docs/api/mesh.md`, `services/meshcored/{api.c,mesh_runtime.cpp,mesh_runtime.h}` | Feasibility spike proving pinned MeshCore can send an explicit per-message path without learned-path mutation; protocol decision | Host service/harness tests + on-air two-repeater gate |
| P1 | `apps/rift/rift_path_builder.{c,h}` (new), `tests/rift_path_builder_test.c` | LVGL-free ordered full-key draft, undo/clear/edit, node-disappearance invalidation, deterministic adapter for drawing | Native host tests, sanitizer pass |
| P2 | `apps/rift/ui/rift_mapview.c`, new path-editor UI | Touch selection, live segments/arrows, topology fallback, waypoint editor, dynamic layout, focus/zoom behaviour | UI fixtures, screenshot tests, full theme matrix |
| P3 | `apps/rift/{rift_ipc*,rift_model*,rift_messages*}`, COMMS | Capability negotiation, preview and SEND integration, message-scoped path and refusal handling | Fake meshcored IPC tests, concurrency/restart tests |
| P4 | Device | Manual path A > repeater 1 > repeater 2 > recipient, invalid-path refusal and ACK evidence | K230 bench RF gate with logs |

A branch can implement P1/P2 as a **non-sending editor**, but must keep
`USE PATH` unavailable until P0/P3 are proven and integrated. No implicit
RF probes or test packets on screen opening, node selection or route preview.

## Acceptance checklist

- [ ] Node selection renders the ordered segment immediately; undo/clear
      removes the corresponding geometry and list entries.
- [ ] Choice stored as full public keys; rename, reorder and refresh do not
      redirect the draft to a different node.
- [ ] Non-repeater, duplicate, recipient-as-relay, sender-as-relay, removed,
      collision/ambiguous, length-overflow and stale destination are handled.
- [ ] Geographic view does not fabricate locations; topology view labels
      presentation-only placement. Line meanings are explicit.
- [ ] Pan/zoom, drawing and gesture cancellation coexist without accidental
      selects; controls fit all supported screen geometries.
- [ ] Draft message text is preserved through entry, cancellation, service
      reconnect and re-layout.
- [ ] AUTO and channel message handling are unchanged.
- [ ] Only a deliberate SEND emits RF; opening/preview/edit/USE PATH do not.
- [ ] Service rejects unknown and unsupported manual routing without AUTO
      fallback; SEND-time revalidation is mandatory.
- [ ] Explicit route is bound to the correct message and never leaks into
      concurrent sends, retries, RESEND or persistent contact paths.
- [ ] At least two physical repeaters demonstrate the exact selected on-air
      path and a valid recipient ACK; proof is captured in a hardware gate.

## Prototype and evidence

`docs/design/rift/visual-path-builder.html` is a **standalone click-through
design prototype only**. It uses synthetic, deliberately fictional mesh nodes
and never talks to meshcored or a radio. Its controls demonstrate incremental
drawing, order, undo, clear, map/topology switching and local staging of
a draft. It is *not* implementation evidence or a shipping feature.
