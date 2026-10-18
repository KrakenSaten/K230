# Explicit MeshCore path P0 — read-only backend checkpoint

Date: 2026-10-18
Status: **IMPLEMENTED BUT NOT BUILD-VERIFIED**; manual-path transmission remains disabled.
Design reference: [RIFT Visual Path Builder](../apps/rift/VISUAL_PATH_BUILDER.md).

## Grounded feasibility result

Pinned MeshCore RIFT commit: `3ca7e3f003bd270587b1d92e33ce999542dcb5b4`.

- `BaseChatMesh::sendMessage(const ContactInfo&, ...)` composes one encrypted,
  ACK-tracked text packet. It selects the direct path from the **passed
  contact's** `out_path` and `out_path_len`, or floods if unknown.
- `Mesh::sendDirect()` **copies** the chosen path into the packet; it does not
  mutate the contact or require changing MeshCore's global route table.
- Therefore a per-message, temporary `ContactInfo` copy with a validated
  explicit `out_path` is a promising route to manual TX without mutating a
  persistent learned path. **This is an architectural inference, not
  a passing RF or host test.**
- `Packet::path_len` packs 1..3 hash bytes per relay into bits 6..7 and
  a relay count (0..63) into bits 0..5. `MCD_MAX_PATH` is 64 bytes.
- Each hop is addressed by a **prefix** of the full public key. Validate
  collisions; a full key in the editor alone cannot prevent ambiguous
  on-air forwarding.

## Changes in this PR

1. `mcd_runtime_preview_path` validates a proposed explicit direct route
   against the service's current inventory. The input is one exact
   recipient public key and ordered full repeater public keys.
2. The validator checks recipient identity/type, relay identity/type,
   duplicate waypoints, prefix collisions against other known repeaters,
   configured path-hash width, count and byte bounds, and encodes the
   candidate path as MeshCore's packed length plus bytes.
3. The non-transmitting `mesh.path_preview` IPC method reports the
   candidate's `path_hex`, `relays`, `path_bytes` and `hash_bytes`.
   `valid` means **locally encodable**, not RF connectivity or delivery.
4. `mesh.info.capabilities` explicitly states
   `manual_path_preview: true` and `explicit_direct_send: false`.
5. The `mesh.send` handler now refuses `via` immediately, rather than
   allowing an older route-unaware sender to silently use AUTO instead.
6. Runtime tests are added for zero relay, invalid/missing relay, self,
   recipient, duplicate, overflow, path hash widths and preview not
   transmitting.

## Limitations

- No packet with a user-selected route is sent by this change.
- The test changes have been reviewed as source but **not executed**.
- Nothing proves that a selected repeater can physically hear the previous
  relay, or that the chain can reach the destination.
- A prefix absent from the current node inventory can still exist on the
  mesh, so collision checking is a fail-closed best effort, not an
  end-to-end guarantee.
- `mesh.path_preview` doesn't reserve a route: the sender must revalidate
  at send time, not trust an old preview result.
- The default `mesh.send`, direct/flood and channel behaviour is unchanged
  except for deliberate refusal of the previously unsupported `via` input.

## Next mandatory gate: real explicit send

1. Add a *per-message* transient `ContactInfo` with the previewed path;
   never overwrite `contacts[].out_path` or call `resetPathTo()`.
2. Revalidate full keys and route bounds at send time; exact recipient
   matching only. Preserve automatic send behaviour when `via` is absent.
3. Record the requested route **per outgoing message**, including the path
   that was actually used; ensure a pending request cannot overwrite
   another message's route.
4. Explicitly define RESEND semantics: either persist and retry the same
   chosen path with a new attempt, or reject with a clear error.
   **Never** silently revert an explicit path to AUTO during RESEND.
5. Test real packet's `ROUTE_TYPE_DIRECT`, `path_len`, `path[]`,
   unchanged learned route, received message, ACK and overlapping sends
   in the host's mock-air harness.
6. Run `make ENABLE_MESHCORED=1 meshcored` and applicable
   `meshcored_runtime_test` / harness / lint gates in the pinned Linux
   checkout, then the K230 on-air test with repeaters.

This checkpoint is intentionally safer than enabling an unverified TX
path on production hardware.


## Follow-up implementation in the same draft PR (2026-10-18)

A first per-message explicit-send implementation has now been added to this
branch, **not built or RF-validated**. The earlier status description above
refers to the original read-only checkpoint.

- `mesh.send` accepts `{"to":"<full key>","text":"...","via":["<full repeater key>"]}`
  in addition to unchanged automatic sends without `via`.
- The service revalidates the route on SEND, uses a transient
  `ContactInfo` copy and passes it to MeshCore's existing
  `BaseChatMesh::sendMessage` to construct a DIRECT packet.
- Accepted messages record their explicit packet path and reply with
  `route:"explicit"`. A retry through `mesh.send(resend)` is rejected for
  these messages rather than using a different learned route.
- Runtime source tests cover a selected-repeater send, unchanged learned
  contact path, per-message metadata and refusal of RESEND.

**Unverified:** native compile, tests, on-air route, retransmit/ACK behaviour
and actual RF delivery. Leave PR as Draft until these pass. In particular,
a successfully encoded route does not prove that physical repeater links
exist. Full source/test quality gate required before enabling the RIFT UI.
