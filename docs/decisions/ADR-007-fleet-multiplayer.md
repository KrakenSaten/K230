# ADR-007: Two-player Fleet over the mesh, and app datagrams in meshcored

Status: Proposed (2026-09-25), on branch `claude/fleet-multiplayer-protocol-95evn7`.
Awaiting the product owner. Nothing here is binding until it is Accepted.
Date: 2026-09-25
Deciders: product owner (final), AI engineering partner (author)

## Context

PocketFleet (docs/apps/POCKETFLEET.md) is single player. The owner asked for
a two-player match over LoRa between two Doors devices, designed for an
unreliable link: loss, duplication, delay, reordering, outages, an app that
is closed and reopened, a service restart and, where practical, a reboot. The
airtime it uses must stay modest, because the channel is shared with a
MeshCore mesh.

Constraints already binding:

- ADR-001/ADR-002: apps never touch hardware. radiod owns the SX1262;
  meshcored holds radiod's lease for as long as it runs and is the only
  client allowed to transmit while it does (docs/api/radio.md, lease).
- ADR-002 v0.1 lifecycle: an in-process app is created when opened and
  destroyed when left. There is no background app (ui/shell/app.h).
- meshcored ships in every image and is disabled per unit
  (`MESHCORED_ENABLE=0`, docs/services/MESHCORED.md).
- `mesh.send` carries text only (1 to 160 bytes, no control characters), and
  every message it sends or receives lands in `mesh.messages`, which RIFT
  shows (docs/api/mesh.md).

Facts that shape the decision:

- MeshCore has an addressed, encrypted, authenticated datagram that nothing
  in Doors uses: `PAYLOAD_TYPE_REQ`. meshcored's `onContactRequest()`
  answers nothing (services/meshcored/mesh_runtime.cpp). Upstream uses the
  first data byte as a request type, 0x00 to 0x07 in the pinned tree
  (vendor/RIFT@3ca7e3f, `BaseChatMesh.h`, `examples/*/MyMesh.cpp`).
  DOCUMENTED.
- A REQ is framed as header + path length + path + dest hash + src hash +
  2-byte MAC + AES-128 blocks over `tag(4) | data` (`Mesh.cpp:488`,
  `Packet.cpp:37`). A datagram whose data is at most 12 bytes is therefore a
  22-byte frame at zero hops, the same size as the ACK unit A measured
  (docs/hardware/MESHCORED_HARDWARE_GATE.md). DOCUMENTED, and the size is
  VERIFIED hardware for the ACK only.
- On the MeshCore profile (SF8, 62.5 kHz, CR 4/5, preamble 32, CRC), the
  repository's own `lora_airtime_ms()` gives 304.1 ms for 22 bytes and
  386.0 ms for 38 bytes; the same function reproduces the 754.688 ms unit A
  recorded for a 109-byte advert. VERIFIED (computed and cross-checked).
- The shell links no cryptographic hash. meshcored links SHA-256 from
  vendor/Crypto but does not expose it. VERIFIED (source).

## Options

### Transport

A. **Fleet on `radio.*`.** Refused by the lease while meshcored runs, and it
   would need its own identity, encryption and routing. Breaks the
   app -> service -> radio rule in spirit. Rejected.

B. **Text over `mesh.send` with a prefix.** Needs no change to meshcored,
   and rides the hardware-verified TXT+ACK path. But every game packet is a
   chat message in RIFT, a T-Deck peer shows it as junk, control characters
   are refused so the binary has to be base64, and the transport ACK doubles
   the frames. Kept only as a fallback.

C. **`TXT_TYPE_CLI_DATA`.** Repeater administration semantics. Rejected.

D. **A game daemon (`gamed`).** Would give background invites, at the cost
   of a new supervised service. Not needed for a turn-based match whose
   state is persisted. Later, if background invites are wanted.

E. **Generic app datagrams in meshcored, on MeshCore `PAYLOAD_TYPE_REQ`.**
   One small, protocol-level addition to meshcored that is not Fleet-specific:
   `mesh.app_send`, `mesh.app_inbox` and a `mesh.app` event, addressed by a
   port number. It reuses the identity, contact authentication, AES + MAC,
   routing, path learning, the duplicate table and the asynchronous transmit
   path meshcored already has, all of which are VERIFIED on hardware for
   direct text.

### Fairness

1. Trust both clients. Nothing to build; a modified client can lie freely.
2. **Whole-board commitment** (salted SHA-256 at deployment, opened at the
   end) plus cheap plausibility checks during play. Lies are *detected*.
3. Per-cell commitments with a Merkle proof on every answer. Lies are caught
   on the shot, but every answer grows by about 100 bytes: roughly +80 %
   airtime per ply. Rejected.

## Decision

Proposed, with defaults for the questions the owner has not ruled on yet.
Each default is marked so it can be overturned without touching the rest.

1. **Transport is option E.** meshcored gains a generic app-datagram surface
   (docs/api/mesh.md, "App datagrams"). The first byte of REQ data is
   `0xD0 | port`, outside every upstream request type. Fleet is port 1. No
   transport ACK: reliability is end to end in the app. A datagram that
   arrived by flood is answered with a small RESPONSE receipt so the sender
   learns a direct path, as upstream does for CLI data. *(Default.)*
2. **Fleet owns its protocol; meshcored knows nothing about Fleet.** The
   protocol and its state machine are pure C under `apps/fleet/net/` (no
   LVGL, no IPC, no filesystem, lint-enforced). One module,
   `apps/fleet/link/fleet_link_mesh.c`, speaks `mesh.app_*`, in RIFT's
   non-blocking, id-matched style.
3. **Fairness is option 2.** SHA-256 is first-party code in
   `apps/fleet/net/fleet_sha256.c` (FIPS 180-4, tested against the NIST
   vectors), used only for the commitment. Not a new dependency, and not in
   `core/`, because nothing else needs it. *(Default.)*
4. **Two players, one match per device, turn based**, standard rules: the
   guest (the invited player) fires first. *(Default.)*
5. **Opening Fleet transmits nothing.** A transmission comes only from a
   user action (Invite, Accept, Decline, Cancel, Fire, Resume, Check link,
   Forfeit) or from an obligation of a match the user is playing while its
   screen is open. No keepalives.
6. **The match is paused, never forfeited, by silence.** No shot clock and
   no expiry. *(Default.)*
7. **Airtime governor in the protocol**: at most 6 frames in a burst,
   refilled one per 5 s, and at most 90 s of estimated Fleet airtime per
   rolling hour per device (2.5 %). *(Default; constants.)* The first draft
   said 60 s and one per 10 s; the simulator showed that stalling ordinary
   long matches on a lossy link (docs/apps/FLEET_MULTIPLAYER.md, "Airtime").
   The owner may prefer the stricter figure and accept the stalls.
8. **Multiplayer requires meshcored enabled on the unit.** Fleet says so
   when it is not; it never starts or enables anything. *(Default.)*
9. **A failed save in multiplayer** warns and plays on for the session.
   *(Default.)*
10. **Pre-start states are not persisted.** An invite that has not become a
    match is lost when Fleet closes; the peer is answered `END(unknown)`.

The full protocol is docs/apps/FLEET_MULTIPLAYER.md.

## Consequences

Needed now (cloud-only, P0 to P6): the codec, state machine and simulator;
Fleet's lobby, the multiplayer Battle and Result; `match.v1`; meshcored's
app datagrams; a simulator multiplayer run over two whole meshcored
processes. No hardware is touched before P7.

Useful soon: P7 on unit A and unit B (docs/hardware/FLEET_MULTIPLAYER_GATE.md);
a per-run identifier in `mesh.status`, so a client can tell one meshcored run
from the next without reading `uptime_s`.

Future: background invites (a daemon or a shell-level listener, not decided
here); rematches; other rules variants (the INVITE carries a rules byte).

Risks:

- The MeshCore datagram MAC is 2 bytes. A forged packet passes it once in
  65,536 tries and must then also match the 24-bit session, the ply and the
  exact expected state. Adequate for a game; not an authentication claim.
- A T-Deck (RIFT firmware) receiving a Doors REQ: its `onContactRequest()`
  answers only the types it knows. ASSUMED to ignore `0xD1`; P7 checks it.
- Pre-start invites are lost when Fleet closes (ADR-002 has no background).
- The board clock is 1970 until NTP; nothing here uses the wall clock for a
  decision, only `CLOCK_MONOTONIC`.

Migration cost if option B is chosen instead: the codec, state machine,
simulator, UI and persistence are unchanged; only `fleet_link_mesh.c` and the
meshcored addition change.

## Evidence

| Claim | Class | Basis |
| --- | --- | --- |
| REQ framing and sizes | DOCUMENTED | vendor/RIFT@3ca7e3f `Mesh.cpp`, `Packet.cpp`, `BaseChatMesh.cpp` |
| 22-byte frame on air | VERIFIED hardware (ACK only) | MESHCORED_HARDWARE_GATE.md |
| Airtime figures | VERIFIED (computed) | `services/radiod/airtime.c`, cross-checked against the gate's advert |
| Upstream REQ types 0x00-0x07 | DOCUMENTED | pinned tree, `simple_*` and `companion_radio` examples |
| T-Deck ignores an unknown REQ type | ASSUMED | not tested; P7 |
| Everything built in P1-P6 | VERIFIED host | the suites named in docs/apps/FLEET_MULTIPLAYER.md |
