<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# M2 implementation plan: Fleet over the local network

Status, 2026-11-21: packages 0, 1, 2, 3 and 4 are on master (§2). peerd
(package 3) is there as an opt-in build that no image includes and nothing
starts; putting it in an image is a step of its own, not yet done. The LAN
link (package 4) is there. Package 5, Fleet's screens for it, is written and
in review (a draft PR), not merged. Package 6 onwards is not started and waits for the owner's go-ahead. Nothing has
been deployed to hardware. Follows the owner's
conditional approval of M1: architecture approved; peerd subject to a
security ADR and review; Fleet's existing multiplayer UI; LoRa behaviour
preserved; A/B hardware validation; background invitations deferred to M4,
so **ADR-008's behaviour is not changed in M2**. The ADRs are
[ADR-017](../../decisions/ADR-017-peer-datagrams.md) (peerd and its security)
and [ADR-018](../../decisions/ADR-018-fleet-link-profile-and-saves.md) (link
profiles, a save per transport), both Accepted 2026-11-20.

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
| 0 | **ADRs** | ADR-017 and ADR-018 | **done**: both Accepted 2026-11-20. Implementation goes in its own PR; hardware deployment needs explicit approval |
| 1 | **Link profile** | retry and governor figures move from constants to a profile read from the link; human waits split from link waits; mesh profile equals today's constants | **done** (PR 154, master `048b1c15`): `fleet_match_test`, `fleet_mp_sim_test` and `fleet-mp-e2e` unchanged in result; LAN-profile tests |
| 2 | **Save per transport** | store path by transport; one-match-per-device across both; lint rule; golden fixture | **done** (PR 154) except that the app does not yet apply the one-match rule (below): `fleet_save_test`, `fleet_session_test`, `fleet_lint.sh`, two golden fixtures |
| 3 | **peerd** | identity, beacons, probe, sealed datagrams, lease, kept peers, lookup, inbox, limits; `peer.*` over pocketipc; `docs/api/peer.md`; init script, disabled by default | **done** (PR 156, master `680fc5cc`) **except the init script and image wiring** (step 3b, below): known-answer vectors, negative and restart tests, two real peerd processes on loopback, ASan/UBSan, a fuzz run of the parser and receive path. Not run on a mock network with loss between processes: that is package 6's |
| 4 | **`fleet_link_peer.c`** | a sibling of `fleet_link_mesh.c` on `peer.*`: lease and keep on poll, address entry | **done** (PR 159, master `c92f93a7`), not opened by the app: `tests/fleet_link_peer_test` against a scripted peerd (nothing asked before the first poll), the session's `match_peer` in `fleet_session_test`, ASan/UBSan, and opt-in against two real peerd processes |
| 5 | **Fleet UI, minimal** | a LORA / LOCAL NETWORK choice on the existing Multiplayer panel; the existing lobby screen with the LAN link; ADD BY ADDRESS and this device's address; the sentences of §1.4. No new screen | **written, in review** (draft PR): `fleet_view_mp_test`, `fleet_app_test` section 10 and `fleet_shell_test.sh` section 11 in both shapes, against the virtual opponent and a scripted peerd |
| 6 | **End to end, host** | `fleet-lan-e2e`: two Fleet players through two peerd processes - clean; lossy; app killed and resumed; peerd restarted; address changed mid-match; broadcast blocked with manual join | each ends with one win, one loss, both fleets verified, identical records |
| 7 | **Hardware gate, A + B** | §3 | written gate, run with the owner's go-ahead |

Order: 0, then 1 and 2 (Fleet only, no new service, a LoRa no-op), then 3,
4, 5, 6, 7.

As built, package 5 (`docs/apps/FLEET_MULTIPLAYER.md`, "The local network in
the app"):

- **The network is offered only where peerd's socket exists** when Fleet
  opens, or where `match-lan.v1` holds a match. The plan did not say; without
  this every device would show a way in that leads to "the service is not
  running", and no image has peerd. On such a device Fleet's screens are
  unchanged to the byte.
- **The choice is two buttons on one row** of Command's MULTIPLAYER panel,
  not a control in the lobby, so it costs the panel no height and the
  transport is chosen before any service is asked anything.
- **One session at a time.** Picking the other transport closes the link in
  use and opens the other with its own save.
- **With a match in hand the other transport's button is not shown**, where
  §1.1 has it stay and say "An engagement with X is under way on LoRa."
  Command shows RESUME MATCH alone and says which transport the match is
  on: one button that does something, rather than one that explains why it
  does nothing.
- **Another identity's save is remembered** in an empty file beside it
  (`match.v1.stale`, `match-lan.v1.stale`), on a device with both transports
  only: without it such a save would be taken for the match in hand at every
  start and hold the other transport back.
- **For the owner to rule on**: visibility lasts while the network's session
  is engaged, Command included; and a match in hand on a transport whose
  service has been switched off blocks the other one.
- `fleet_link_peer_present()` was added to the link, and the link now lets
  peerd's outstanding answers arrive (60 ms at most) before it writes its
  request to hide and closes: without that the request was lost whenever
  Fleet closed between a question and its answer. A defect of package 4,
  found by this package's tests.
- The virtual opponent's link gained `self=N`, another key for this device,
  to test a save from another identity.
- Not done: any run on a device; the touch keyboard over the address field
  on a panel; a stale reply to a replaced probe (the scripted peerd still
  cannot delay one).

As built, package 4 (PR 159; `docs/apps/FLEET_MULTIPLAYER.md`, "The local
network link"): the link is its own test binary, `tests/fleet_link_peer_test`, with
its own scripted peerd, rather than an extension of `fleet_link_test`. The
session gained one optional link op, `match_peer`, through which it says who
the match in hand is with; the link keeps that peer and releases every other,
which assumes Fleet is peerd's only client. Having no interface to announce
on does not take the link down: a real peerd bound to one address announces
on none and still exchanges datagrams, which the opt-in check against two
real peerd processes (`make ENABLE_PEERD=1 ... fleet-link-peer-real`) showed.
An independent reviewer session read it against ADR-017, ADR-018 and the
real peerd in two passes; one blocker and four major findings were fixed
before the merge, and the last small commit was not re-read. Left for later
packages: package 5 must read `fleet_link_peer_proved()` before it presents
a player as present, and should test a stale reply to a replaced probe;
package 6 should cover a match whose END is still owed and an opponent
released at the end of a match whose last packet was lost.
Not done: any run on a device; the app does not open the link.

As built, package 3 (PR 156; `docs/api/peer.md` is its specification):

- **No init script and no image wiring.** peerd is built only on request
  (`make ENABLE_PEERD=1 SODIUM_DIR=<prefix> peerd`); `make all`, `make test`
  and `make install` are unchanged and no defconfig selects libsodium. That
  integration is its own step, **3b**, before the hardware gate: select
  libsodium in the Doors defconfig, wire `ENABLE_PEERD` into the Buildroot
  package, add an init script disabled by default (`PEERD_ENABLE=0`), build
  an image. It needs an image build and the owner's go-ahead.
- **Buildroot's default for libsodium is the full build, not the minimal
  one.** `BR2_PACKAGE_LIBSODIUM_FULL` defaults to yes in the pinned
  Buildroot 2025.02.1; ADR-017 says the default is minimal, and that
  sentence of its evidence is wrong. The decision itself stands - minimal,
  which is what peerd was built and tested against - so step 3b must set
  `# BR2_PACKAGE_LIBSODIUM_FULL is not set`.
- The wire format, UDP port 47230 and the `peer.*` method set were fixed
  with the code, as ADR-017 left them to be. `peer.resolve {code}` of
  ARCHITECTURE §5.5 is `peer.probe {address}`: table codes are the lobby's
  (M3).
- Two deviations from ADR-017 §10, both stated in `docs/api/peer.md`: no
  per-source or global receive rate limit (a source address can be forged;
  there are budgets for unknown keys, known keys and each known key
  instead), and the write cap for the kept list is global rather than per
  peer.
- Review: an independent reviewer session read the code against ADR-017 in
  two passes. One blocker and two major defects in the delivery of a kept
  peer's datagrams, and a set of minor ones, were fixed before the merge.
  The last small round of fixes was not re-read. No human review.

Evidence for package 3: published test vectors for the primitives;
small-order, mixed-order and non-point keys refused; the negative tests
ADR-017 names; restart, power-cut and persistence-fault cases; 400,000
fuzzed inputs under ASan and UBSan with nothing forged accepted; two real
peerd processes on loopback; a riscv64 cross-build. Not done: any run on a
device, discovery between two machines, and ADR-017's two open evidence
items (the random source at first boot, the state directory's filesystem).

As built, packages 1 and 2 (PR 154) differ from the rows above in three
ways, each recorded in docs/apps/FLEET_MULTIPLAYER.md, "Link profiles and a
save per transport":

- On a fast link an INVITE or an ACCEPT is repeated at the link's pace to a
  deadline equal to the mesh's window, and the lobby shows the time left
  instead of a try count (the owner's ruling on ADR-018 point 2).
- The duplicate-reply limit is part of the profile (mesh 2 s, LAN 200 ms).
- **One match per device** is enforced by the app from package 5: the
  other transport is not offered while either save holds a match, and the
  session is told. A save made under an identity its transport no longer has
  holds neither back once the link has said who the device is (as built,
  package 5).

Evidence for packages 1 and 2: two `match.v1` fixtures generated from master
`1767f09d` round-trip byte for byte; the Fleet simulator's output is
identical to that build's; `fleet-mp-e2e`, the riscv64 cross-build and the
full `make test` (no failure master does not also have on the build host)
were run on the commit that was merged (`8951608f`). Not run: an image build, anything on
hardware.

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
