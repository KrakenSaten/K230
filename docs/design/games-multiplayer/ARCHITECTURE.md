<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# Multiplayer platform - architecture (M1)

Status: **direction approved by the product owner, 2026-11-20.** This
document is the design record, not the binding text: where it and ADR-017 or
ADR-018 differ, the ADRs govern. Of it, the link profile and the save per
transport are built, peerd as an opt-in service no image includes, and
Fleet's link to it (M2-PLAN.md, packages 1-4); Fleet's screens for it are
written and in review (package 5); nothing else is.
Evidence classes follow AGENTS.md. Everything marked PROPOSED is a design
choice of this package, not a fact about the repository.

## 1. What exists today

| Fact | Class | Source |
| --- | --- | --- |
| Fleet plays two-player matches over LoRa between units A and B: 46-ply match, close/reopen, meshcored restart, hard reboot and a 179 s RF cut all recovered | VERIFIED hardware | `docs/hardware/FLEET_MULTIPLAYER_GATE.md` (2026-09-26) |
| Fleet already has a transport seam: `struct fleet_link_ops` (poll, send, recv, self_key, state, peers, peer_name, advertise, retry_base, close), with a mesh link and an in-process loop link | VERIFIED source | `apps/fleet/link/fleet_link.h` |
| The match state machine is pure C and treats the link as lossy, duplicating, reordering datagrams; it persists before it transmits and resumes from `match.v1` | VERIFIED host + hardware | `docs/apps/FLEET_MULTIPLAYER.md` |
| meshcored carries opaque app datagrams by port (1..15), keeps a 32-datagram inbox per run, and proves the sender's key | VERIFIED host + hardware | `docs/api/mesh.md`, "App datagrams" |
| Retry and airtime-governor figures are compile-time constants of the match (`FLEET_RETRY_*`, `FLEET_GOV_*`); only the retry base is taken from the link | VERIFIED source | `apps/fleet/net/fleet_match.h` |
| A player is the node's meshcored public key; `match.v1` refuses another identity | VERIFIED source | `FLEET_MULTIPLAYER.md`, "Identities", "Persistence" |
| meshcored is disabled per unit by default (`MESHCORED_ENABLE=0`) | DOCUMENTED | ADR-008 |
| Games are in-process shell apps; an app is created when opened and destroyed when left. The shell restarts in place on rotation | DOCUMENTED | ADR-002, `ui/shell/app.h`, DS §21.2 |
| The shell can raise a notice over any screen and open an app from it | VERIFIED source | `pocketos_shell_notify`, `ui/shell/app.h` |
| The shell does not link OpenSSL, curl or cJSON; network-facing code lives in helpers or services | DOCUMENTED | ADR-007 |
| OpenSSL 3.4.1 is already in the image | DOCUMENTED | ADR-007, `docs/LICENSING.md` |
| netd manages Wi-Fi only; Ethernet is unmanaged, gets a random MAC per boot, and both default routes share metric 0 | DOCUMENTED / VERIFIED | `docs/api/network.md`, `docs/hardware/T-DISPLAY-K230.md` |
| The hub's lobby tile is one inert widget, built so it can go live without re-laying out the wall | VERIFIED source | DS §60.4, `ui/shell/hub.c` |
| Poker, 2048, Relay Nine and RotorTerror have no network code | VERIFIED source | grep of `apps/` |

ADR-008 was Accepted by the owner on 2026-11-20 and reconciled with the
hardware gates (ADR-008-RECONCILIATION.md); when this document was first
written it still read "Proposed".

### Genuine gaps

1. No transport other than LoRa. No LAN discovery, no IP datagram path.
2. No player identity when meshcored is off.
3. The governor and retry schedule are LoRa's, compiled in.
4. No shared lobby: Fleet's lobby screen is Fleet's own.
5. No invitation reaches a player who does not have that game open.
6. No way to open a game *into* a multiplayer state from outside it.
7. No game registry that says what a game can do linked.

## 2. Options

**A. Shell-level lobby with lightweight game adapters.** The lobby is a shell
screen; games register a small adapter; transports are code linked into the
shell.

**B. A multiplayer daemon with a session API.** A supervised `gamed` owns
discovery, sessions, invitations and transports; games are its clients.

| | A: shell lobby + adapters | B: session daemon |
| --- | --- | --- |
| Fleet's verified code | untouched; the lobby drives it | the session would move behind IPC, or exist twice |
| Sessions survive a shell restart (rotation) | not needed: Fleet persists and resyncs | yes, but turn-based games do not need it |
| Background invitations | the shell is always running and has notices | yes |
| Untrusted LAN input | parsed in the root, DRM-master shell | parsed outside the shell |
| Crypto for LAN | would pull a crypto library into the shell (against ADR-007) | available in the daemon |
| New moving parts | none | a service, an API, a supervisor entry, a second owner of match state |
| Cost of being wrong | low | high: a session API designed with one game as evidence |

B's session API is the speculative part. Fleet is the only evidence for what
a session is, and Fleet's session is deliberately the game's own (ADR-008
decision 2: "Fleet owns its protocol"). ADR-008 itself set `gamed` aside as
"not needed for a turn-based match whose state is persisted", to be
revisited "if background invites are wanted" - which §6 answers without one.

A's weakness is real and specific: a LAN socket inside the shell means
untrusted network bytes parsed by the process that owns the display, sockets
that die on every rotation, and no place for cryptography.

## 3. Recommendation: A, with transports as services

**Sessions belong to games. The lobby belongs to the shell. Transports belong
to services.** That is option A for everything about play, plus one small
transport service for IP, built to the shape meshcored already has.

```text
                       doors-shell (one process)
 ┌────────────────────────────────────────────────────────────────────┐
 │  Games Hub ──▶ Lobby (shell screen)          Fleet (app)           │
 │                  │  play_table_ops             │ fleet_session     │
 │                  ▼                             ▼                   │
 │            game adapters ◀── registry ──▶ match + match.v1         │
 │                  │                             │                   │
 │                  └────────── play_link ────────┘   (the seam)      │
 │                     │                    │                         │
 │               link: mesh            link: peer                     │
 └─────────────────────┼────────────────────┼─────────────────────────┘
              pocketipc│                    │pocketipc
                  meshcored              peerd  (NEW, small)
                  mesh.app_*             peer.app_*   same shapes
                       │                    │
                    radiod            UDP on wlan0 / eth0
                    SX1262            (later: a relay connection)
```

Why this is the simplest thing that works:

- **No session daemon and no session API.** A game's match state has exactly
  one owner, the game, as today.
- **`peerd` is not a game service.** It carries opaque datagrams between
  identities, exactly as meshcored does: `peer.app_send`, `peer.app_inbox`,
  a `peer.app` event, `peer.nodes`. It knows nothing about games, tables or
  lobbies. Fleet's LAN link is then a sibling of `fleet_link_mesh.c` with a
  different service name; the match code does not change.
- **The inbox does the hard parts for free.** A datagram that arrives while
  the shell restarts for a rotation, or while the game is closed, waits in
  the service and is read by id afterwards - the behaviour the hardware gate
  already verified for LoRa.
- **The boundary matches ADR-002 and ADR-007.** Services own I/O; the shell
  reads JSON from a local socket; crypto libraries stay out of the shell.
- **A hybrid is what this already is**, and no more of one is justified. If
  a real-time game later needs sessions that survive a shell restart, that
  game's needs can be designed then, behind the same `play_link` seam.

The cheaper alternative - a UDP module inside the shell, about a third of the
code - is recorded in README "Alternatives". It is not recommended: it fails
the three points above and saves one init script.

## 4. Ownership

| Owner | Owns | Never |
| --- | --- | --- |
| **Games Hub** (shell, DS §60) | the lobby tile: live summary, entry to the lobby | sessions, sockets |
| **Lobby** (shell screen, PROPOSED `ui/shell/lobby*.c`) | home, HOST, JOIN, the table (waiting room), invitation cards, resume cards, link status; listening for invitations while no game is open | match state, rules, any game's packets beyond "is this an invitation" |
| **play library** (PROPOSED `core/pocketplay/`, pure C) | the `play_link` seam, the game registry, the remembered-players store, the lobby's model | LVGL, game rules |
| **Link adapters** (`play_link_mesh`, `play_link_peer`, the loop link) | talking to one transport service; the link's profile | retries, sessions |
| **meshcored** (exists) | LoRa: identity, crypto, routing, inbox | games |
| **peerd** (PROPOSED) | IP: identity, discovery, authenticated datagrams, inbox; later the relay route | games, tables, lobbies |
| **A game** | its protocol, its handshake if it has one, fairness, persistence, reconnection, every in-match screen | discovery, picking a transport, another game's port |

## 5. Interfaces (sketches, PROPOSED)

### 5.1 The link: Fleet's seam, promoted

`play_link` is `fleet_link.h` unchanged in meaning, plus two things the
second transport makes necessary.

```c
enum play_transport { PLAY_T_LOOP, PLAY_T_MESH, PLAY_T_PEER_LAN, PLAY_T_PEER_RELAY };

struct play_link_profile {        /* what a protocol may assume of this link */
    enum play_transport transport;
    uint32_t retry_base_ms;       /* mesh: 7000 or est_timeout_ms; LAN: ~500 */
    uint32_t retry_cap_ms;        /* mesh: 60000;  LAN: ~5000 */
    uint32_t gov_burst;           /* mesh: 6;      LAN: 0 = unmetered */
    uint32_t gov_refill_ms;       /* mesh: 5000 */
    uint32_t gov_hour_ms;         /* mesh: 90000;  LAN: 0 = no airtime budget */
    uint16_t max_payload;         /* mesh: 160;    LAN: 1024 */
    uint8_t  open_tables;         /* 1: peers can be told of a table (LAN) */
    uint8_t  identity_proved;     /* 1: "from" is cryptographically proved */
};

struct play_link_ops {            /* fleet_link_ops, plus profile() */
    void (*poll)(void *ctx, int64_t now_ms);
    enum play_link_send (*send)(void *ctx, const uint8_t to[32], const uint8_t *buf, size_t n);
    int  (*recv)(void *ctx, uint8_t from[32], uint8_t *buf, size_t *n);
    int  (*self_key)(void *ctx, uint8_t key[32]);
    enum play_link_state (*state)(void *ctx);
    int  (*peers)(void *ctx, struct play_link_peer *out, int max);
    const char *(*peer_name)(void *ctx, const uint8_t key[32]);
    int  (*advertise)(void *ctx);
    const struct play_link_profile *(*profile)(void *ctx);
    void (*close)(void *ctx);
};
```

The contract stays what the hardware gate verified: **unreliable, unordered,
possibly duplicated datagrams between proved 32-byte identities; every call
returns at once.** Reliability is the protocol's, end to end, because a
protocol that must survive a restart needs it there anyway. No transport is
asked to be TCP, and none is.

The mesh profile is today's constants exactly; `fleet_mp_sim_test` must give
the same figures after the change as before it. That is the regression
proof that LoRa behaviour is preserved.

### 5.2 Game registration

One static record per multiplayer game, compiled in, like the app table.

```c
struct play_game {
    const char *app_id;            /* "fleet" */
    uint8_t  port;                 /* 1..15, the same number on every transport */
    uint8_t  proto_min, proto_max; /* versions this build speaks */
    uint8_t  players_min, players_max;
    uint32_t transports;           /* bit per play_transport this game supports */
    uint32_t traits;               /* TURN_BASED | RESUMABLE | OWN_HANDSHAKE | ... */
    const struct play_table_ops *table;                 /* pre-game, driven by the lobby */
    int (*resume_info)(struct play_resume *out);        /* from the game's own save; no I/O on a link */
    int (*classify)(const uint8_t *pkt, size_t n,
                    struct play_knock *out);            /* "is this an invitation, of which version?" pure */
    int (*launch)(enum play_intent intent);             /* open the app into RESUME / SOLO */
};
```

Port registry (PROPOSED, kept in this package until an ADR owns it): 1 Fleet
(ADR-008); 2 reserved for a shared table protocol (M4); 3-14 games; 15
experiments. Fifteen ports is small on purpose - it is MeshCore's nibble.

### 5.3 The table: what the lobby drives

The lobby's waiting room is a view over one interface. It does not know
whether the game brought its own handshake.

```c
struct play_seat  { uint8_t key[32]; char name[32]; enum play_seat_state state; /* EMPTY, INVITED, JOINED, READY, LEFT */ };
struct play_table { enum play_table_phase phase;   /* OPEN, FILLING, STARTING, HANDED_OFF, FAILED */
                    struct play_seat seat[PLAY_SEATS_MAX]; int seats; int tries, tries_max; char reason[48]; };

struct play_table_ops {
    void *(*open)(struct play_link *link, int host);
    int   (*invite)(void *t, const uint8_t peer[32], const char *name);   /* host */
    int   (*answer)(void *t, int accept);                                 /* guest */
    int   (*leave)(void *t);                                              /* cancel / close table */
    void  (*poll)(void *t, int64_t now_ms);
    void  (*snapshot)(void *t, struct play_table *out);
    void  (*close)(void *t);
};
```

Two implementations are foreseen, and **M2-M3 build only the first**:

1. **Fleet's**, a thin wrapper over `fleet_session`: `invite` is
   `fleet_session_invite`, `answer` is accept/decline, and the snapshot maps
   INVITING / INVITED / ACCEPTING / DEPLOY to seat states. No new packet goes
   on the air; the frames are the ones the gate measured.
2. **A shared table protocol** on port 2 for games with no handshake of
   their own (N seats, READY, START). It is designed in M4 against a real
   second game, not now.

### 5.4 The handoff is the save file

When Fleet's table reaches DEPLOY, the match is already in `match.v1` (it is
a match from that moment; ADR-008 decision 10). The lobby then closes its
table and calls `launch(PLAY_INTENT_RESUME)`. Fleet opens, loads `match.v1`
and resumes by the path the gate exercised a dozen times. Nothing is handed
over in memory, so a rotation or a crash between the two steps is the same
case as any other reopen.

Fleet chooses its link **by file**: each transport has its own save
(`match.v1` for LoRa, unchanged; `match-lan.v1` for LAN), so the transport is
known before a byte is decoded and a LAN match can never disturb a LoRa save.
An earlier draft chose by the saved key; decoding under the wrong identity
raises a store fault today, so that was dropped (M2-PLAN §1.1, §1.2).

### 5.5 peerd's surface (PROPOSED, mirrors `mesh.*`)

| Method / event | Meaning |
| --- | --- |
| `peer.status` | state, `run_id`, interfaces in use, visible or not |
| `peer.identity` | this device's public key and name |
| `peer.set_visible {on}` | bind, announce and listen; off closes the sockets |
| `peer.nodes` | identities heard: key, name, route (`lan`), `heard_ms` |
| `peer.announce {port, tag}` / `peer.withdraw` | include "a table is open for port N" in this device's beacon |
| `peer.tables` | tables other devices announce |
| `peer.resolve {code}` | manual join: turn a table code or address into a node |
| `peer.app_send {to, port, payload_hex}` | as `mesh.app_send`; result `accepted`, `route`, `est_timeout_ms` |
| `peer.app_inbox {port, after_id}` | as `mesh.app_inbox`: last 32, this run |
| event `peer.app` | a datagram, as `mesh.app` |

## 6. Session lifecycle

```text
            LOBBY (shell)                          GAME (app)
 ┌────────────────────────────────┐      ┌──────────────────────────────┐
 │ DISCOVER ──▶ TABLE ──▶ START ──┼─────▶│ MATCH: ACTIVE ⇄ PAUSED ──▶ END│
 │    ▲          │  ▲             │ save │   ▲                     │    │
 │    │      FAILED │             │ file │   └── resume (intent) ──┘    │
 │    └── home ◀────┴── invitation│      │  result stays until put away │
 └────────────────────────────────┘      └──────────────────────────────┘
```

| Phase | Owner | Persisted | Ends by |
| --- | --- | --- | --- |
| Discover | lobby | no | choosing HOST or JOIN |
| Table (open, filling) | lobby, through the game's `table` ops | no (as ADR-008 decision 10) | start, cancel, leaving the screen, no answer |
| Start / handoff | game writes its save; lobby launches it | **yes, before the lobby lets go** | the game opening |
| Match | game | yes, before every send | the game's own END |
| Paused | game | yes | RESUME from the lobby or the game |
| Result | game | until put away | the player |

Identifiers and versions:

| Thing | Form | Chosen by |
| --- | --- | --- |
| Player / device | a 32-byte public key **per transport** (meshcored's on LoRa, peerd's on IP), shown as name + 4 hex digits | the service |
| Match / session | the game's own (`sid`, 24 bits, for Fleet) | the game's host |
| Table tag | 32 random bits in a LAN beacon; the 4-character table code is derived for humans | the host's lobby |
| Game protocol version | inside the game's packets (Fleet: the top two bits of `vt`) | the game |
| Advertised version | `proto_min..proto_max` in the LAN beacon, so JOIN can say NEEDS UPDATE before anyone tries | registry |
| peerd wire version | one byte in every peerd frame | peerd |

**No transport switching.** A match lives and dies on the link it began on;
its identity, peer key and retry profile are that link's. Moving a match
would mean proving that two keys are one player, which nothing here does.
The lobby may offer a *new* match on another link; it never migrates one.

**One match per game** stays the rule (ADR-008 decision 4). The lobby shows
at most one resume card per game.

### Invitations: background listening, app-owned sessions

- A session exists only inside a game or an open table. The lobby's
  invitation listener creates none.
- While no game holds the port, the shell **listens only**: it reads new
  datagrams for registered ports from the two inboxes (`after_id`, which is
  non-destructive, so the game still sees them) and asks the game's
  `classify`. A fresh invitation - this service run, newer than the game's
  retry window - raises a shell notice; a tap opens the lobby's card.
- **It never transmits and never answers.** ACCEPT and DECLINE go out only
  from a finger, through the game's table ops, which is when the session is
  first engaged. ADR-008 decision 5 ("no transmission without a user
  action") is kept to the letter. What changes is the sentence beside it,
  that Fleet "does not even read the inbox" before the player engages:
  reading moves to the shell. That needs the owner's approval
  (PROPOSALS.md, item 3) and is M4, not M2.

## 7. Transports

### 7.1 LAN (Wi-Fi and Ethernet) - M2

| | PROPOSED |
| --- | --- |
| Discovery | a signed UDP beacon every 2 s while visible, to the subnet broadcast address of each interface in use, TTL 1; carries key, name, wire version and at most one table announcement (`port`, versions, seats, tag). No mDNS: nothing in the image provides it and one fixed message does not need it |
| Reach | unicast UDP to the address a beacon or an authenticated datagram last came from; the endpoint follows the key, so Ethernet's per-boot MAC and a new DHCP lease are survivable |
| Manual join | the host's table screen shows a 4-character code (the host's address within the shared subnet, with a check character) and its address; JOIN > ENTER TABLE CODE sends one unicast probe |
| When it cannot work | client isolation on an access point blocks broadcast and unicast alike; the lobby says so ("This network may keep devices apart") rather than spinning |
| Both interfaces up | peerd uses each interface it is given a socket on and answers where it heard; route metrics are not its business |
| Reliability | none at this layer, by design (§5.1). On a LAN the game's own retry profile makes a lost datagram cost about half a second |
| Privacy | silent by default. peerd binds nothing until `peer.set_visible`; the lobby sets it while it or a LAN table is open and for the life of a LAN match, and says VISIBLE while it is |

Port numbers, the beacon layout and the code alphabet are M2's to fix in an
ADR; none is invented here.

### 7.2 LoRa / MeshCore - unchanged

- Fleet keeps `mesh.app_*` port 1, its packets, governor, fairness and
  persistence. The lobby adds **no frame of its own** to LoRa: no presence,
  no table announcements, no keepalives. Players are those meshcored has
  heard; MAKE VISIBLE stays the one zero-hop advert, from a button.
- Hosting on LoRa is therefore a direct invitation to one player, not an
  open table. The lobby says so on the link card.
- The lobby shows the hour's airtime ("AIRTIME 12 / 90 S") from the game's
  governor, and that the mesh service is off when it is. It starts and
  enables nothing (ADR-008 decision 8).
- RIFT is unaffected: nothing new is transmitted, app datagrams never enter
  `mesh.messages`, and the shell listener only reads an inbox.
- A shared table protocol on LoRa (M4+) would have to earn its airtime per
  frame, as chat did.

### 7.3 Online - future, outline only

```text
 peerd ──TLS──▶ rendezvous/relay ◀──TLS── peerd       route: "relay"
   └────────── same end-to-end sealed datagrams ───────────┘
```

- A third **route inside peerd**, not a new stack: the same identities and
  the same sealed datagrams, carried by an outbound TLS connection to a
  relay. Games and the lobby see one more `play_transport`.
- **Relay first.** Turn-based traffic is tiny; a relay always works behind
  NAT. UDP hole punching is an optimisation to measure later, not a
  requirement.
- **The device key is the account.** The server authenticates a key by
  challenge, routes by key, and sees ciphertext. No password, e-mail or name
  is required; friend codes are key fingerprints.
- Matchmaking and invitations are messages to the server, outside games.
- Not designed here: the server, its hosting, abuse handling, cost. Online
  is opt-in per device and off by default.

## 8. Identity and trust

| | LoRa | LAN / relay (PROPOSED) |
| --- | --- | --- |
| Identity | meshcored's key | peerd's key, made once, stored 0600 under the state directory |
| Proof of sender | MeshCore: AES + 2-byte MAC between contacts | a pairwise key from the two identities; every datagram sealed (AEAD) with a counter |
| First contact | hearing an advert | hearing a signed beacon |
| Trust model | trust on first use | trust on first use |

- **Remembered players** (`play/friends.v1`, PROPOSED): key, transport, last
  name, first seen, last played, count. A remembered name arriving with a
  different key is shown as a different player and flagged; the store never
  silently rebinds a name. This is the whole foundation for friends.
- One person has two keys today, one per transport. The record is a list so
  the two can later be joined by a proof; nothing claims they are joined now.
- Primitives come from libsodium in peerd, never first-party curve code.
  The construction is ADR-017 (Proposed), which supersedes the sketch above
  where they differ.

## 9. Security boundaries and limits

| Boundary | Rule |
| --- | --- |
| Network -> peerd | the only parser of untrusted IP input. Fixed-size frames, versioned; a bad frame is dropped silently. On-link sources only |
| peerd -> shell | pocketipc JSON on a local socket, as meshcored. The shell never sees an unauthenticated datagram |
| Service -> game | the game validates every field of every packet, as Fleet does (length, version, sid, sender is the session's peer) |
| Lobby -> game | function calls through the registry; the lobby cannot send on a game's port except through that game's table ops |
| Names | UTF-8, no control characters, 24 bytes shown; the checker Fleet's chat already has |

PROPOSED limits: peerd holds 32 nodes, 16 tables, a 32-datagram inbox;
payload at most 1024 bytes; per-source and global rate limits; unknown-key
traffic limited to beacons and one probe a second. The lobby lists at most
8 tables and 8 players. Every process runs as root today (docs/api/network.md,
"Trust model"); peerd does not improve or worsen that, and should drop
privileges if the platform ever gains the means.

## 10. Persistence, reconnection, recovery

Unchanged from what the gate verified, and deliberately not generalised:
each game persists its own match before it transmits, resumes from its save,
and resyncs by its own protocol. The platform's rules are only these:

| Event | Result |
| --- | --- |
| Shell restart (rotation), app closed | datagrams wait in the service inbox; the game resumes from its save |
| Service restart (`run_id` changes) | link adapters start again from `after_id` 0; the game's retries cover the gap |
| Address change on LAN | the endpoint follows the key at the next beacon or datagram |
| Link lost | the game pauses; never a forfeit by silence (ADR-008 decision 6). The lobby's resume card says paused, and why |
| Table fails before start | nothing was saved; the lobby says one sentence and returns to JOIN or HOST |
| Version mismatch | seen in the beacon on LAN (NEEDS UPDATE); on LoRa the game's own DECLINE(version) |
| Save unreadable or other identity | the game's existing "void" path; the lobby offers no resume |

## 11. Self-review

Findings from one pass over this document, and what was done.

1. *An earlier draft put a UDP socket in the shell.* Dropped: ADR-007's
   boundary, rotation, and crypto all argue for a service (§3).
2. *"Reliable communication" was first read as TCP.* A connection that dies
   on every rotation would need its own resume protocol on top; the verified
   datagram contract already is one (§5.1).
3. *A generic table protocol was sketched for M2.* Deferred to M4: with one
   game it would be a framework built on a guess, and on LoRa it would cost
   airtime Fleet's handshake does not (§5.3).
4. *READY for Fleet.* Fleet has no READY packet. The table shows ACCEPT as
   READY and the host as ready by hosting; the mockups are drawn that way.
5. **Open risk: retry counts double as human patience.** Fleet's INVITE
   gives up after 6 tries, about three minutes on LoRa. A LAN profile with a
   0.5 s base would give up on a person in seconds. The profile must keep
   human waits (invite, accept) long and link waits (shot, result) short;
   M2 has to split the two and prove the mesh figures did not move.
6. **Open risk: two identities per player** is honest but will surprise
   people ("Anna" twice). The UI shows the link beside the name everywhere.
7. **Open risk: peerd's cryptography** is the one piece that is new and
   easy to get wrong. It gets its own ADR and review in M2.
8. **Not verified:** that broadcast discovery works on the owner's networks
   (M2 gate). The `match.v1` question is settled by a save per transport
   (M2-PLAN §1.1).
