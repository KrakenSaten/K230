<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# M2 implementation plan: Fleet over the local network (PROPOSED)

Status: plan only, 2026-11-20. No code is written. Follows the owner's
conditional approval of M1: architecture approved; peerd subject to a
security ADR and review; Fleet's existing multiplayer UI; LoRa behaviour
preserved; A/B hardware validation; background invitations deferred to M4,
so **ADR-008's behaviour is not changed in M2**. The ADRs are
[ADR-017](../../decisions/ADR-017-peer-datagrams.md) (peerd and its security)
and [ADR-018](../../decisions/ADR-018-fleet-link-profile-and-saves.md) (link
profiles, a save per transport), both Proposed.

## 1. The four questions, resolved

### 1.1 Choosing the transport before a saved match is decoded

**By file, not by content.** Each transport has its own save file:

| Transport | File | Written by |
| --- | --- | --- |
| LoRa | `fleet/match.v1` (as today) | the mesh session only |
| LAN | `fleet/match-lan.v1` (new; same 655-byte codec) | the LAN session only |

Why not sniff the saved key: the link learns its own key asynchronously,
after its first poll, and decoding a save under the wrong identity is not
harmless today - `fleet_match_save_decode` returns -2, the session sets
`identity_changed` and `store_fault` (VERIFIED source,
`apps/fleet/link/fleet_session.c`), and the next match on that link would
overwrite the file. A path decides the transport before any byte is read.

On open Fleet stats both files and decodes neither across transports:

| `match.v1` | `match-lan.v1` | Multiplayer offers |
| --- | --- | --- |
| no match | no match | LORA or LOCAL NETWORK |
| match in hand | - | RESUME (LoRa), as today; LOCAL says "An engagement with X is under way on LoRa." |
| - | match in hand | RESUME (LAN); LORA says the same of the local network |
| both in hand | | cannot arise: see below |

**One match per device stays** (ADR-008 decision 4). A session on one
transport refuses to start while the other file holds a match between
DEPLOY and REVEAL. An invitation arriving on the idle transport is not
read, exactly as an invitation is not read today while Fleet's multiplayer
is not engaged; its sender sees "no answer".

### 1.2 Existing LoRa saves: compatible and protected

- `match.v1` keeps its name, size, codec, magic and version. No migration.
- **No LAN code path can name it.** The store takes the file from the
  session's transport; `tests/fleet_lint.sh` gains a rule that only the mesh
  session reaches `FLEET_STORE_MATCH_FILE`.
- A **golden-file test**: a `match.v1` written by the current master build
  (mid-battle, with a pending shot and tombstones) is committed as a fixture
  and must load, restore and re-encode byte for byte after M2.
- `fleet_mp_sim_test` and `fleet-mp-e2e` must reproduce today's figures
  with the mesh profile (ADR-018): same frames per ply, same airtime.
- **Downgrade is safe**: a pre-M2 shell never looks at `match-lan.v1` and
  finds `match.v1` exactly as it left it.
- The LAN file is 0600 and written temp-flush-rename-dirsync like `match.v1`.

### 1.3 peerd during rotation, reconnection and an active LAN match

peerd is a supervised service like meshcored, independent of the shell. A
rotation is the shell's `execv` in place (`ui/shell/shell.c`); it does not
touch peerd. What matters is what peerd keeps doing while no client is
connected:

| Mechanism | Rule |
| --- | --- |
| **Visibility lease** | `peer.set_visible` is a lease of 30 s, renewed by the link's poll while Fleet's multiplayer is engaged. It outlives a rotation (seconds) and lapses by itself when Fleet is closed. No lease, no beacons |
| **Kept peers** | `peer.keep {key}` while a LAN match is in hand: a persisted record (key, replay state, last address; ADR-017 §5). peerd loads it at start, stays bound and accepts sealed datagrams from that key with no client attached. Fleet asserts it when a match starts and **again whenever it opens or resumes a saved LAN match** (idempotent, so a record lost to a fault is restored), and releases it when the match ends or is put away; there is no timed expiry |
| **Inbox** | the last 32 datagrams of this run, at most 8 per sender, a kept peer's never pushed out by another's (ADR-017 §5), read by `after_id`: packets that arrive during a rotation or while Fleet is closed wait, as on LoRa |
| **Endpoint follows the key** | an accepted frame from a kept key that is newer than any seen moves its address - for a signed frame only when the address it states is its source (ADR-017 §7): a new DHCP lease or Ethernet's per-boot MAC needs nothing from the player |
| **Lookup** | while a send to a kept key has no fresh address, peerd asks for it: the last known address by unicast and one broadcast "who holds key K", at most once per 5 s, only while something is waiting to be sent |
| **peerd restart** | `run_id` changes and the inbox is empty; kept records are reloaded from disk, so the opponent's next datagram is accepted at once. The link starts again from `after_id` 0 and renews its lease on its next poll. Fleet's own retries cover what was in the lost inbox, as they covered the meshcored restart in the LoRa gate |
| **Fleet closed, peer keeps playing** | datagrams wait in the inbox; RESUME reads them. If peerd also restarted meanwhile they are gone and the peer's retries deliver them after RESUME |
| **Network gone** | the link reports it; Fleet says "out of reach, paused, not lost" and CHECK LINK probes, as on LoRa |

peerd binds nothing while it has neither a lease nor a kept peer, and with
a kept peer but no lease it serves that peer only (ADR-017 §5). Peers behind
address translation are not supported.
Whether it starts at boot is a per-unit flag (`PEERD_ENABLE`, default off,
like `MESHCORED_ENABLE`), set by hand on units A and B for the gate.

### 1.4 Discovery and manual joining when broadcast is blocked

| Network | What happens | What the player sees |
| --- | --- | --- |
| Broadcast and unicast work (most home networks; Wi-Fi and cable on one segment) | beacons list players within 2 s | PLAYERS IN RANGE, as on LoRa |
| **Broadcast blocked, unicast works** | nothing is heard. The lobby shows this device's address; the other player enters it under ADD BY ADDRESS. peerd sends a signed, challenge-bound probe by unicast, three tries over 3 s; the signed answer must echo the challenge and come from that address, and gives the prober a node proved live; the responder's proof comes with the first sealed exchange (ADR-017 §5), and the endpoint then follows the key | after 6 s of silence: "No players heard on this network. Enter the other device's address: this one is 192.168.1.42." |
| **Client isolation** (broadcast and unicast blocked) | the probe gets no answer | "No answer from 192.168.1.42. This network may keep devices apart. Try another network, a cable, or LoRa." No spinner, no silent retry loop |
| Different subnets, routed | as "unicast works" if the route exists (ADR-017 allows unicast from another subnet) | the same |
| No network | the link says so | "Wi-Fi is off and no cable is connected." |

M2 uses a dotted IPv4 address typed on the keyboard base or touch keyboard.
The four-character table code of the M1 mockups is the lobby's (M3). IPv4
only in M2. A match in hand whose peer's address is lost under blocked
broadcast falls back to the last known address, then to ADD BY ADDRESS; the
match itself is unaffected.

## 2. Work packages

| # | Package | Contents | Proof |
| --- | --- | --- | --- |
| 0 | **ADRs** | ADR-017 (peerd and its security) reviewed and accepted; ADR-018 (link profile, save per transport) accepted | owner's decision. **Gate for everything below** |
| 1 | **Link profile** | retry and governor figures move from constants to a profile read from the link; human waits split from link waits; mesh profile equals today's constants | `fleet_match_test`, `fleet_mp_sim_test` and `fleet-mp-e2e` unchanged in result; a LAN-profile simulator run |
| 2 | **Save per transport** | store path by transport; one-match-per-device across both; lint rule; golden fixture | `fleet_save_test`, `fleet_session_test`, `fleet_lint.sh` |
| 3 | **peerd** | identity, beacons, probe, sealed datagrams, lease, kept peers, lookup, inbox, limits; `peer.*` over pocketipc; `docs/api/peer.md`; init script, disabled by default | unit tests with crafted frames and known-answer vectors; two real peerd processes on loopback and on a mock network with loss, duplication and blocked broadcast; ASan/UBSan; a fuzz run of the frame parser |
| 4 | **`fleet_link_peer.c`** | a sibling of `fleet_link_mesh.c` on `peer.*`: lease and keep on poll, address entry | `fleet_link_test` extended with a scripted peerd; nothing asked before the first poll |
| 5 | **Fleet UI, minimal** | a LORA / LOCAL NETWORK choice on the existing Multiplayer panel; the existing lobby screen with the LAN link; ADD BY ADDRESS and this device's address; the sentences of §1.4. No new screen | `fleet_view_mp_test`, `fleet_app_test`, `fleet_shell_test.sh` in both shapes |
| 6 | **End to end, host** | `fleet-lan-e2e`: two Fleet players through two peerd processes - clean; lossy; app killed and resumed; peerd restarted; address changed mid-match; broadcast blocked with manual join | each ends with one win, one loss, both fleets verified, identical records |
| 7 | **Hardware gate, A + B** | §3 | written gate, run with the owner's go-ahead |

Order: 0, then 1 and 2 (Fleet only, no new service, each mergeable alone
and each a LoRa no-op), then 3, 4, 5, 6, 7. Packages 1 and 2 can be reviewed
before the security ADR is settled, because neither depends on peerd.

Not in M2: the shared lobby, the hub tile, table codes, background
invitations, remembered players, the play library, any second game, IPv6,
online, and any change to meshcored, radiod or Fleet's packet format.

## 3. Hardware gate (units A and B)

Prepared as a document in M2; run only when the owner says so. Non-persistent
kit as in the earlier gates, rollback script on each unit.

1. **Silence**: Fleet open, multiplayer not engaged: peerd bound to nothing,
   zero packets (counters and a capture on the LAN).
2. **Discovery** on Wi-Fi: each unit lists the other within a few seconds.
3. **Full match over Wi-Fi**: one win, one loss, both verified, identical logs.
4. **Full match with A on Ethernet and B on Wi-Fi.**
5. **Recovery**, each mid-match: Fleet closed and reopened; rotation on both
   units during a pending shot; peerd restarted; reboot straight after FIRE;
   Wi-Fi off for 3 minutes; a new DHCP lease or cable replug (new MAC).
6. **Blocked broadcast**: beacons dropped by a filter on one unit; join by
   address; then all traffic dropped: the isolation message, no hang.
7. **LoRa regression**: the P7 minimum gate's core again on the same build -
   invite, a full match, close/reopen, meshcored restart; the saved
   `match.v1` from before the install still resumes; `mesh.messages` empty.
8. **Coexistence**: a LoRa match saved, then LOCAL offered and refused with
   the right sentence, and the reverse.

## 4. Risks carried into M2

| Risk | Handling |
| --- | --- |
| The security construction | ADR-017 chooses an established one and names its review; no code before acceptance |
| libsodium is a new dependency | approved by the owner 2026-11-20; packaged in the pinned Buildroot (1.0.18, ISC); the minimal build's exports are ADR-017's first check |
| Human waits shortened by a fast profile | ADR-018 separates them; a simulator invariant that the invite window is the same on both profiles |
| Broadcast behaviour on real access points is ASSUMED | measured in gate steps 2, 4 and 6 |
| Refusing the second transport while a match is in hand may annoy | one sentence and RESUME; relaxing it later needs no format change |
