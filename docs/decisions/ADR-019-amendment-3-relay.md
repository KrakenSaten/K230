# ADR-019 amendment 3: an optional, self-hosted DOORS Relay

Status: **Accepted** by the product owner on 2026-11-27, as revised (rules
28 to 42), with the five decisions of the "Decision record".

- The five design concerns the owner named are resolved in rules 31, 34, 37,
  38 and 39 (detail in `docs/design/companion/RELAY_PROPOSAL.md` §4-§9).
- A focused independent architecture and security review, and the
  reviewer's re-check of the revision, are answered in `RELAY_PROPOSAL.md`
  §15. The first pass found 2 blockers, 6 major and 11 minor points; the
  re-check found 4 more major problems and 3 minor ones.
- A second independent review (Codex, 2026-11-27) found the architecture GO
  and asked that the capacity, pairing-commit and recovery semantics be
  settled before phase 2. The owner accepted its findings. Rules 28, 29,
  31 to 34, 37 and 38 were tightened accordingly (`RELAY_PROPOSAL.md` §15.3).
- **Acceptance of the design does not mean an implementation exists or is
  validated.** Nothing is built or deployed. Each phase of
  `RELAY_PROPOSAL.md` §16 needs its own go-ahead, and the independent
  security review of the code (phase 5) comes before any release.
- The threats are in `docs/design/companion/RELAY_THREAT_MODEL.md`.

Date: 2026-11-27
Deciders: product owner (final), AI engineering partner (author)

Amends:
- ADR-019 (accepted 2026-11-21): rules 2, 3, 5 and 13, and the "Revisit
  when" paragraph;
- `docs/design/companion/CONTRACTS.md` §1 and §2;
- the Settings > Companion page of ADR-019 amendment 2 (accepted 2026-11-25,
  on draft PR #190, not yet in master).

Every other rule is unchanged, and the rules below say where they must keep
holding.

## Context

ADR-019 rule 5 binds the gateway to one interface's address and accepts
only loopback, RFC 1918 and link-local sources: Companion works only where
the phone and the unit reach each other directly.

On 2026-11-27, at the Companion Settings 2.0 hardware gate (Unit B, build
`d189c2c1`), the owner's phone and Unit B shared "SkullerudPark", a
guest-style network the owner does not administer. The phone read the
pairing code, then found no answer:
- gatewayd listened on the unit's address there (`10.0.2.143:47231`);
- during the phone's attempt, no frame from the phone reached the unit (no
  TCP SYN, no ARP request);
- the unit's ARP requests for the phone went unanswered, while the router
  answered and other clients' broadcasts arrived.

That points at client isolation but was **not proven**. Establishing it is
phase 0, and it does not block this work (owner, 2026-11-27).

The owner's terms (2026-11-27):
- optional, lightweight, **self-hosted**, reached by **outbound TCP 443**;
- **local first**;
- end-to-end TLS 1.3, mutual authentication, the existing identities,
  physical pairing, pinning, scopes and revocation preserved;
- the relay neither decrypts application traffic nor exposes arbitrary
  device services;
- **off by default**;
- **minimal**: no unnecessary infrastructure, dependencies or features.

## Options

| | A. First-party blind relay, TLS inside TLS | B. Mesh VPN (WireGuard + Headscale/Tailscale, Nebula, ZeroTier) | C. TURN (coturn) | D. Reverse tunnel (frp, rathole, SSH -R) | E. Syncthing's relay server, reused | F. Local only: unit joins the phone's hotspot (F1), or hosts its own access point (F2) |
| --- | --- | --- | --- | --- | --- | --- |
| Gateway TLS end to end, unchanged | yes | yes, inside the tunnel | yes | yes | yes | yes |
| Exposes only the gateway | yes, by protocol | no: an IP path to every service of the unit, which has no firewall | yes | only in secret/visitor modes | yes | yes |
| New daemon or runtime on the K230 | a small unprivileged helper; OpenSSL already in the image | yes (Go daemon or kernel module, TUN) | a TURN client in C | yes | a client | none for F1; hostapd and udhcpd (on the image) for F2 |
| Android | a transport in the app | a VPN service; one VPN per phone | a TURN client | another client | a client in the app | an app change (F1); none (F2) |
| Clockless | yes | expiry to be disabled; Nebula certificates carry validity | yes with static credentials | yes | yes | yes |
| Works on an isolating network without internet | no | no | no | no | no | yes |

## Decision

**Option A for remote access**, with **option F pursued in phase 0**. A new
section:

### 7. Remote transport through a DOORS Relay

28. **Off by default, set up only at the unit.**
    - Remote access is a second switch under Settings > Companion. It is off
      on every unit and every image: `GATEWAYD_RELAY=0` in
      `/etc/default/gatewayd`, per unit, never shipped.
    - It is turned on only from the unit's own screen, while the gateway is
      on (so with a device password, rule 9b).
    - **Setting up the relay:**
      1. The person types its DNS name (at most 64 characters) and port.
      2. The unit opens a probe connection, and shows the key the relay
         presents as sixteen hex digits in groups of four.
      3. The person compares them with what the relay printed at install,
         and confirms. The confirmed key becomes the active pin.
      4. The relay's backup key fingerprint is taken from the relay over a
         fresh connection presenting the confirmed key (checked against its
         full SHA-256, not the sixteen digits). It is signed by the
         confirmed key over a nonce of gatewayd's. gatewayd verifies that
         signature itself, and accepts a backup only during set-up.
      5. The backup check and the durable write of the set-up happen on that
         connection before it registers, within the relay's deadline. If the
         deadline passes, the unit dials again with what it has written.
    - No QR decoder is added. No relay is built in, and Doors operates none.
29. **Outbound only, closed until asked, and independent of the LAN.**
    - The gateway listens nowhere but rule 5's address. Relay traffic
      arrives only over connections the unit opens to the configured relay
      on TCP 443.
    - The unit holds a relay connection only while a paired phone holds
      `remote`, while a pairing window allows the relay (rule 34), or while
      the person sets up the relay at the unit (rule 28).
    - **Only reasons of address spare relay sessions.** A change of address,
      or an interface without private IPv4 (CGNAT, a public address, IPv6
      only, wlan0 down), closes the LAN listener but leaves relay sessions
      running and relay pairing possible.
    - Every reason of the gateway's own closes relay sessions too, and makes
      the helper disconnect: the switch off, no device password (rule 9b),
      `identity_fault`, `state_fault`.
    - Rule 5 is amended accordingly.
30. **The relay is a pipe below the gateway's TLS.**
    - ADR-019's TLS 1.3 session (rules 10 to 12, 14) runs end to end and
      unchanged between phone and unit.
    - The relay holds no key that could decrypt it or stand in for either
      end.
    - Nothing the relay or the helper says is part of any trust decision
      about it. The helper's `via` and `client` are hints: gatewayd decides
      by the channel a session arrived on and by the inner TLS key.
31. **Which relay is genuine** (concern 1: rotation and recovery without a
    LAN).
    - **The unit pins two relay keys:** an active one, and a backup whose
      private half the operator keeps offline. It signs a relay login only
      for a key that is one of its pins.
    - **Promotion to the backup is one-way:**
      - it happens only after the relay proves it holds the backup key, by
        signing a nonce of gatewayd's under its own context string;
      - it is written durably: the new active pin, the removal of the old
        one and the next sequence number are one write, and a restart at any
        point leaves the old state or the new one, never a mix;
      - every connection presenting the old key is closed, and nothing is
        signed for it again;
      - it happens at most once per configuration.

      A new backup is added only by a person at the unit.
    - **Phones pin the relay too**, from the unit. In `gateway.hello` the unit
      tells each paired phone the relay's name and port, its current key and
      the current **endorsement**. An endorsement is a domain-separated
      statement signed by the unit's device key:
      - the relay's name and port;
      - the SHA-256 of its key;
      - a sequence number.
    - The unit endorses only its own pins, never a key a relay or the helper
      proposes.
    - **When a phone's relay pin fails** (a rotation it has not yet seen),
      **or during a first pairing**, the phone accepts the presented key
      provisionally and verifies an endorsement for it against the unit key
      it pins: the one stored, or the pairing code's `fp`. Until that check
      passes it sends only the HTTP start of rule 39, and a lookup value
      bound one-way to the presented key. Then it pins the new key.
    - **Configuration id and sequence number:**
      - Each relay configuration on the unit has a random 64-bit
        configuration id, carried in every endorsement and in
        `gateway.hello`.
      - On the provisional path a phone accepts only an endorsement with the
        configuration id it has stored. At a first pairing it accepts any id,
        and stores it.
      - Within an id, the phone keeps the highest sequence number it has
        accepted, and refuses lower ones.
      - A new id, and with it a sequence starting again at 1, reaches a phone
        only over an authenticated gateway session or a new pairing code. So
        old endorsements of a reconfigured unit stay refused.
    - Phones therefore follow a rotation (the promotion of the backup)
      through the relay alone. Only the unit's pins need a person at the
      unit: to add a backup, or to recover when both keys are lost.
    - **After both keys are lost, or a new set-up,** the unit has a new
      configuration id, and a phone cannot adopt it on the provisional path.
      It recovers only through an authenticated `gateway.hello` on a path it
      can still use (the LAN), or by pairing again at the unit. Publishing a
      new backup key at the relay installs nothing on a unit.
    - **Rule 13 is amended** with exactly one exception: the provisional
      acceptance of a relay's certificate above. It is confined to the app's
      relay transport by its lint, and is never used for a gateway
      connection.
    - **The outer TLS authenticates the relay only.** Each end then signs a
      fresh relay nonce under its own context string. The unit does so on its
      control connection; a data connection is authenticated by its one-time
      cookie.
    - The relay may present a certificate chain (rule 39). The helper and the
      phone check the relay's pin on the **leaf key alone**, and ignore the
      rest of the chain: no CA, no dates (rule 12).
32. **Admission is the unit's word, checked again by the gateway.**
    - The relay connects a phone to a unit only if that unit lists the
      phone's fingerprint, or as the relay-path candidate of a window that
      allows the relay (rule 34).
    - The unit lists only paired phones that hold `remote`. It sends the
      whole list, never a change to it: after every registration, which the
      relay acknowledges, and after every change.
    - The relay acknowledges a list only once it is in force and the
      sessions of phones taken off it have stopped forwarding. An
      acknowledgement is matched to its update and to the current control
      connection, and means only that this relay says so.
    - The gateway closes, after the handshake, a relay session from a paired
      phone without `remote`, whatever the relay admitted. Its own check and
      its own close stay authoritative.
33. **A scope `remote`, never a default** (owner's decision 3).
    - It is given or taken per phone on the unit: as its own switch on the
      approval sheet, shown off, or later on the phone's page.
    - It is kept in the relay's state file, not in the paired list, so an
      older gatewayd that does not know it is unaffected by a rollback.
    - **A grant counts only for a key in the paired list.** The two files
      are written in a fixed order, and the relay's file records the paired
      list before and after each change to it. At start, a paired list that
      matches neither (a rollback, or an edit) drops every grant. So a
      revocation followed by a new pairing never revives a grant
      (`RELAY_PROPOSAL.md` §6.1).
    - Taking it away, or revoking the phone, is written first, then ends
      that phone's relay session at the gateway, then updates the relay's
      list.
34. **First pairing through the relay** (owner's decision 4; concern 2).
    - **Opening the window.** A window still opens only on the unit (rule
      20), for at most 120 s. The pairing page has a switch, **"Also through
      the relay", off by default**, for a phone that cannot reach the unit
      locally.
    - **When that switch is on:**
      - the code gains the relay's address (`r`). The relay is told an
        identifier derived one-way from the token, and never learns the
        token;
      - a phone that cannot reach the unit locally verifies the relay's
        endorsement against the code's `fp`, and arrives through the relay;
      - **a relay candidate has its own candidate slot and its own count of
        wrong tokens**, so the relay path cannot block or close a LAN
        pairing;
      - a window that is relay-only (no private address) leaves `h` out of
        the code.
    - **The approval sheet** says the phone is connecting **through the
      relay**, and has a "Through the relay" switch shown off. The person
      compares keys and approves, exactly as on a LAN.
    - **Pending invitations end when the window closes or changes.** A
      candidate whose session already runs is left to the gateway, which
      owns its answer, timeout and teardown.
    - **One relay place per unit in v1.** Relay pairing can be busy while a
      remote phone's relay session runs, and the other way round. The LAN
      is never affected.
    - **With `remote` switched on there:** the pairing is committed first.
      The unit then updates the relay's list and waits a few seconds for its
      acknowledgement. The phone is told it is paired, with remote access
      either confirmed or **pending**. Pending is not a failure: the pairing
      stands, and the list is sent again whenever the unit registers.
    - **Without it:** the phone is told it is paired without remote access.
      It tries the relay again only when the person asks, and is then
      admitted once `remote` has been given at the unit.
    - Pairing never grants `remote` implicitly.
35. **Nothing but the gateway.**
    - The relay protocol names a unit, never a port, host or service.
    - What arrives through the relay enters gatewayd's session handling and
      nothing else.
36. **Local first, and said.**
    - The phone tries the local network first. It uses the relay when that
      fails, or when it is not on Wi-Fi, and only for a unit whose relay it
      knows and, after pairing, only with `remote`.
    - The app and the unit show which path each session uses.
    - `CONTRACTS.md` §2 is amended: local connections stay bound to Wi-Fi
      and local addresses; relay connections use the default network and
      reach only the configured relay.
37. **One session per phone** (concern 3: races and replacement).
    - The phone runs the gateway handshake on **one path at a time**. The
      first path whose transport is ready (LAN TCP, or the relay's `ready`)
      gets it, and the other attempt is closed before any gateway byte is
      sent.
    - The gateway keeps today's rule: a new session of a paired phone
      replaces its older one at the end of the handshake, before the
      session limits are counted.
    - The helper and the relay do the same: a new session of a phone that
      already has one replaces it, and is not refused by their limit of one.
      The relay replaces when the new request is admitted; if that new
      session then fails, the phone has none through the relay.
    - **A relay session displaces nothing until it is authenticated and
      eligible.** The gateway decides by the inner key and the channel the
      session actually arrived on. A relay session of a phone without
      `remote`, or outside a relay window, never replaces that phone's LAN
      session.
    - A relay candidate's slot is released when its session ends, so the
      phone's reconnect after pairing is not refused.
38. **Isolation from a hostile relay** (concern 4: CPU and event-loop
    starvation).
    - **The helper.** Every relay connection is handled by `gatewayd-relay`.
      `S70gatewayd` starts it under `pos-supervise` whenever
      `GATEWAYD_RELAY=1`, never forked by gatewayd (gatewayd has dropped its
      capabilities). At its start the helper:
      - drops to a dedicated uid and gid, reserved in the image;
      - drops every capability, sets `no_new_privs`, installs a seccomp
        filter;
      - sets memory and descriptor limits, and runs below gatewayd's priority.

      It cannot read the device key (a test holds that).
    - **It stays idle until asked.** It holds no network connection until
      gatewayd asks for one (rule 29).
    - **It never exits because a relay misbehaved**: it closes and backs off.
      So the network cannot drive it into `pos-supervise`'s crash-loop stop.
    - **The line to gatewayd** is an abstract-namespace Unix socket. Each end
      checks the other's `SO_PEERCRED` uid: gatewayd the helper's, and the
      helper gatewayd's, since any local process can take an abstract name.
      - It carries a fixed set of small frames, at a bounded rate.
      - Every frame carries the generation of the relay connection it is
        about, and the operation it answers. gatewayd acts only on the
        current generation and an outstanding operation, with the expected
        pin.
      - gatewayd bounds its own work per loop turn on this line and on relay
        handshakes, so a compromised helper cannot make it do more.
      - gatewayd reads it without a control buffer, so a descriptor sent to
        gatewayd is discarded.
      - gatewayd writes to it non-blocking, through a bounded queue.
    - **Descriptors only go out.** For each relay session gatewayd makes a
      non-blocking socketpair, keeps its end, and sends the other to the
      helper. gatewayd never receives a descriptor.
    - The helper holds no private key.
    - **Relay budget inside ADR-019 §6:** at most one relay socket (a
      session or its handshake), 1 relay handshake per second, and back-off
      after failures. The relay and the helper keep the same single place.
      A LAN phone always finds a slot. That LAN service stays responsive
      during a relay flood is to be measured, not assumed.
    - **Byte limits.** The helper limits the bytes it reads on every relay
      connection before decrypting them.
    - **Consequence:** a relay, or a compromised helper, can stop remote
      access, and nothing else.
39. **Restrictive networks** (concern 5).
    - Every relay connection has the shape of HTTPS: TLS with SNI (the relay
      has a DNS name) and ALPN `http/1.1`, and its first message is an
      HTTP/1.1 Upgrade request. Any other request gets an ordinary `404`.
    - The relay may present a CA-issued certificate for its pinned key, which
      satisfies middleboxes that check certificates. The decision stays the
      leaf key (rule 31).
    - The phone uses the network's explicit HTTP proxy (`CONNECT`). PAC proxies
      are not supported, and neither is a proxy for the unit in v1.
    - A network that intercepts TLS, or that still wants a sign-in, fails the
      pins: the app says so and does not connect. No bypass.
    - A TLS-terminating proxy or CDN in front of the relay is not supported.
40. **Clockless.**
    - Nothing on the unit depends on a date.
    - Sequence numbers and nonces where order or freshness matter; monotonic
      time for every timer.
41. **Logs.**
    - The relay logs connection lifecycle, fingerprint prefixes, addresses
      truncated (IPv4 to /24, IPv6 to /48) and byte counts. Never payload or
      full addresses.
    - gatewayd and the helper log relay state changes under rule 9.
42. **What the relay operator learns** is said in Settings when remote access
    is turned on: which phone keys reach which unit, when, from which
    networks, and how much they exchange. Never what.

## Relation to accepted ADRs

- **ADR-019**: these hold unchanged end to end:
  - rules 10 to 12 and 14 (transport);
  - 15 to 19 (keys);
  - 20 to 24 (pairing, revocation, scopes);
  - 25 to 27;
  - 9 and 9b.

  Amended:
  - rule 2 (a second process, the helper, handles relay connections on
    gatewayd's behalf);
  - rule 3 (gatewayd also owns `relay.v1`);
  - rule 5 (rule 29);
  - rule 13 (one confined exception, rule 31).

  Rule 8: the helper runs unprivileged from its start. PAIRING.md §7's
  replacement is unchanged.
- **The device key**, besides TLS and its own self-signed certificate, signs
  endorsements and relay logins. Each has its own context string, so no
  signature of one kind can stand for another (`RELAY_PROPOSAL.md` §4.3).
- **ADR-019 amendments 1 and 2**: unchanged, except that Settings >
  Companion gains:
  - the remote-access card and the relay set-up;
  - the per-phone and approval-sheet switches;
  - the pairing page's "Also through the relay".
- **ADR-016 amendment 1**: unchanged. A remote START still needs the tap.
- **ADR-003, ADR-017, `docs/ARCHITECTURE.md`**: no new secret on the unit.
- **ADR-007**: no QR decoder, no network or crypto code in the shell.
- **AGENTS.md**: no library added to the image or the app. The relay is a
  new first-party Go program using the standard library only (owner's
  decision 2).

## Consequences

Needed for implementation (`RELAY_PROPOSAL.md` §16), each a later step with
its own go-ahead:
- the protocol and the Go server;
- the helper and gatewayd's changes;
- Settings;
- the app's relay transport;
- phase 0 alongside phase 1;
- the hardware gate;
- an independent security review of the code before release.

Risks accepted with this amendment:
- A phone with `remote` can reach the unit from anywhere. That is the
  feature. `remote` is never a default.
- A photographed pairing code works through the relay for the rest of its
  window, but only while that window's relay switch is on, and still needs
  the approval and key comparison on the unit.
- The relay operator sees metadata (rule 42) and can deny service.
- A phone that has not seen a newer endorsement can be shown a stolen older
  relay key: metadata and denial, never content.
- A prober that already knows a unit's 256-bit `device_id` and the relay's
  key can learn from the relay whether that unit is online.
- With one relay place per unit, a remote phone's session makes relay
  pairing busy for its duration, and the other way round.
- The relay's acknowledgement of a list is its own word. A lying relay can
  claim a list is in force; the gateway's own checks decide.
- After a rollback, or any change to the paired list made without the
  relay's file, every phone loses `remote` until it is given again.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-27 | Requested: an optional self-hosted relay over outbound TCP 443, local-first, everything of ADR-019's trust model preserved, off by default; design only | Product owner |
| 2026-11-27 | **Approved in principle**: (1) the hybrid local-first, self-hosted relay architecture; (2) the relay server in Go; (3) `remote` disabled by default and controlled per phone; (4) first-time pairing through the relay supported, with physical approval and fingerprint confirmation; (5) phase 0 alongside phase 1, the SkullerudPark troubleshooting not blocking relay work. To be resolved before implementation: key rotation and recovery without a LAN, first relay pairing and the transition to `remote`, LAN/relay races and session replacement, isolation from a hostile relay including CPU and event-loop starvation, and restrictive networks; then a focused independent review. Keep the implementation minimal | Product owner |
| 2026-11-27 | Independent architecture and security review: 2 blockers (helper privilege; descriptor direction), 6 major, 11 minor. All answered in this revision (`RELAY_PROPOSAL.md` §15) | Reviewer; author |
| 2026-11-27 | The reviewer's re-check of the revision: both blockers and majors 5, 6, 8 resolved; 3, 4, 7 partly. 4 new problems (an unverified backup pin, replacement not carried to the helper and relay, relay sessions outliving a closed gateway, rollback after reconfiguration) and 3 minor. All answered here (§15.2) | Reviewer; author |
| 2026-11-27 | **ADR-019 amendment 3 accepted as revised** (rules 28 to 42), with the answers to the review and its re-check. Each phase of `RELAY_PROPOSAL.md` §16 still needs its own go-ahead | Product owner |
| 2026-11-27 | Second independent review (Codex) of this design (PR #192, `e00a3ba1`) and of phase 1 (PR #194, `81e19918`): architecture GO; phase 1 NO-GO (invitations, revocation acknowledgement); phase 2 NO-GO until capacity, acknowledgement and IPC contracts are settled. Five material findings, and a disposition for each of the 27 open wire questions | Reviewer |
| 2026-11-27 | **Accepted the review's findings and dispositions**, with: one relay place per unit in v1 (Q9); an explicit registration acknowledgement (Q22); pending invitations ended when the pairing window closes or changes (Q27). Rules 28, 29, 31 to 34, 37 and 38 tightened (`RELAY_PROPOSAL.md` §15.3). Phase 2 still needs its own go-ahead | Product owner |
| 2026-11-27 | Independent re-review at `3d05c05c` (#192) and `ca450707` (#194). This design: GO, with one wording fix (proposal §8.3, finding F2). Phase 1: NO-GO, narrowly (F1: `invite` can precede `registered`). Phase 2: NO-GO until F1 is fixed and F2 decided. The five original findings are resolved. Open findings, validation status and next steps: `docs/design/companion/RELAY_REVIEW_RESOLUTION.md` §15-§17. Development stopped here at the owner's request | Reviewer; product owner |
