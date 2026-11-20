<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# ADR proposals for M2 (drafts for the owner)

Drafts, 2026-11-20. Neither is an ADR yet: on approval each becomes a
numbered file in `docs/decisions/` and is completed there. No accepted ADR
is edited. ADR-008's behaviour is unchanged by both.

---

## ADR-P1: IP peer datagrams (`peerd`) and their security

### Context

Fleet needs a second transport with the contract its mesh link has: lossy
datagrams between proved 32-byte identities, an inbox that outlives the
client (ARCHITECTURE §3, §5.5; M2-PLAN §1.3). The shell must not parse
untrusted network input or link OpenSSL (ADR-007). The owner's condition:
**an established cryptographic construction, no custom handshake without
justification and validation, and a review before implementation.**

### Requirements

1. A long-term identity per device; a datagram's sender is proved.
2. Confidentiality and integrity per datagram (a Fleet layout is secret
   until the reveal).
3. **Stateless across restarts**: either side's service or device may
   restart at any time and the next datagram must still be accepted. This is
   the property the verified resume behaviour depends on.
4. Discovery beacons are authenticated (a name cannot be claimed for
   another key).
5. Replays cannot do harm. Games already treat duplicates as normal; the
   transport must still bound them.
6. Trust on first use, as MeshCore contacts are; a changed key is a
   different player.
7. Small, reviewable, and free of first-party cryptographic primitives.

Not required in M2: forward secrecy for game traffic, anonymity on the LAN,
protection against a party already on the device (everything runs as root).

### Options

| | A. libsodium, static keys | B. Noise (IK or XX) on OpenSSL primitives | C. DTLS 1.2 (OpenSSL), pinned raw keys |
| --- | --- | --- | --- |
| Construction | `crypto_box` (X25519 + XSalsa20-Poly1305) between the two long-term keys; beacons signed with `crypto_sign` (Ed25519) | a specified handshake, then transport keys | a standard protocol |
| Custom handshake | **none**: there is no handshake | none invented, but the framework is implemented here | none |
| Restart (req. 3) | inherent: nothing to re-establish | a new handshake after every restart, plus rules for packets in flight | a new handshake; sessions per peer on one UDP socket |
| Forward secrecy | no | yes | yes |
| Own security-critical code | nonce and replay handling, framing | the Noise state machine, key schedule, rekey, replay, framing | certificate and pinning glue, multiplexing peers over one socket, timers |
| Validation | the library's own test suite; known-answer tests for framing | published Noise test vectors, plus review of the state machine | interoperability with `openssl s_client`/`s_server` |
| Dependency | **new package** (libsodium, ISC licence), linked by peerd only | none new (OpenSSL 3.4.1 is in the image) | none new |
| Fit to a datagram inbox | exact | workable | awkward: connection-shaped |

### Proposed decision

**Option A.** The owner approved libsodium as a new dependency on
2026-11-20; option B stays on record as the alternative. The ADR, its
independent review and the known-answer tests are still required before
any implementation.

- A is the only option with no handshake at all, which is what "do not
  invent a custom handshake" and requirement 3 both point at. Its cost is no
  forward secrecy: someone who later extracts a device's key could read
  captured game traffic. For a game's shots and a fleet layout that is
  revealed at the end of every match, that is proposed as acceptable, and it
  is the owner's call.
- Random 192-bit nonces remove nonce bookkeeping across restarts. Replay is
  bounded by a per-peer window of recently seen nonces for the current run;
  what passes is handed to a game that already ignores duplicates.
- The long-term identity is an Ed25519 key; the box key is derived by the
  library's own conversion. The 32-byte identity is what games see.
- libsodium is packaged in the pinned SDK's Buildroot 2025.02.1:
  `package/libsodium`, version 1.0.18, licence ISC (VERIFIED locally,
  2026-11-20, in the build host's SDK tree). It is not yet selected in the
  Doors defconfig. Buildroot builds it `--enable-minimal` unless
  `BR2_PACKAGE_LIBSODIUM_FULL` is set; that the minimal build exports every
  function the construction needs (in particular the Ed25519-to-X25519
  conversion) is **not verified** and is the ADR's first check.

To be fixed in the ADR itself, before any code: the frame layout and version
byte, UDP port, beacon contents and rate, probe and lookup frames, limits
(nodes, tables, inbox, payload, rates), key storage (0600 under the state
directory, never logged), the `peer.*` method set (`docs/api/peer.md`), and
the evidence table.

### Review before implementation

1. The owner reads and accepts the ADR.
2. One independent review of the construction and frame layout against the
   requirements above (a second reviewer or a dedicated review session that
   did not write it), with findings recorded in the ADR.
3. Known-answer vectors and a parser fuzz target exist before peerd carries
   a game packet.

### Consequences

A new supervised service and init script, disabled per unit by default; a
new API document; under option A a new package and licence entry
(`docs/LICENSING.md`, `THIRD_PARTY_LICENSES.md`). No change to meshcored,
radiod, netd or the shell's link set. Reverting is removing the service and
one link file.

---

## ADR-P2: Fleet link profile, and a save per transport

### Context

Fleet's retry schedule and airtime governor are constants sized for LoRa
(`FLEET_RETRY_*`, `FLEET_GOV_*`, `apps/fleet/net/fleet_match.h`); only the
retry base comes from the link. A saved match is bound to the identity of
the link that made it, and decoding it under another identity raises a store
fault (`fleet_session.c`). A second transport needs different pacing and
must not be able to disturb a LoRa save.

### Proposed decision

1. **A link profile.** The link reports, once, the figures the match may
   assume: link-wait base and cap, governor burst, refill and hourly budget
   (zero meaning unmetered), and maximum payload. The match reads them
   where it read constants.
2. **The mesh profile is today's constants, exactly.** LoRa behaviour does
   not change; the simulator and end-to-end suites must give today's
   results. This is a refactor on LoRa, not a tuning.
3. **Human waits are separate from link waits.** How long an INVITE or an
   ACCEPT waits for a person is the same on every transport (today's roughly
   three minutes); only the spacing of the repeats within it follows the
   link. Waiting for a RESULT, a COMMIT or a SYNC follows the link.
4. **The LAN profile**: link waits of about half a second doubling to a few
   seconds; no airtime budget; the same try counts and the same "paused,
   never forfeited" rule. Figures are set by the simulator, as LoRa's were.
5. **One save file per transport**: `match.v1` for LoRa, untouched in
   format and reachable only from the mesh session; `match-lan.v1`, the same
   codec, for LAN. The transport is chosen by file before decoding
   (M2-PLAN §1.1).
6. **One match per device** remains (ADR-008 decision 4), across both files.
7. **A player is the key of the transport the match runs on.** A match
   never changes transport.

### Relation to ADR-008

Decisions 4, 5, 6, 8, 9 and 10 are kept as written. Decision 7's numbers
are kept as the mesh profile. "Identities" gains the sentence in point 7.
Nothing about reading the inbox changes (deferred to M4).

### Consequences

`fleet_match` takes a profile at init; `fleet_store` takes a transport;
`tests/fleet_lint.sh` gains two rules (two link files allowed; only the mesh
session names `match.v1`). A golden `match.v1` fixture is added. No packet,
no save format and no screen of the LoRa path changes.

### Open

- Whether ADR-008 is first moved from Proposed to Accepted, since this
  builds on it (PROPOSALS.md item 1).
- Whether "one match per device" should later become "one per transport";
  the two-file layout allows either without a format change.
