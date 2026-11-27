# ADR-019 amendment 3: an optional, self-hosted DOORS Relay

Status: **Proposed**. Nothing is decided, built or deployed. The design is
in `docs/design/companion/RELAY_PROPOSAL.md`, the threats in
`docs/design/companion/RELAY_THREAT_MODEL.md`.
Date: 2026-11-27
Deciders: product owner (final), AI engineering partner (author)
Amends: ADR-019 (accepted 2026-11-21) rule 5 and its "Revisit when"
paragraph; `docs/design/companion/CONTRACTS.md` §2 "Network"; the Settings >
Companion page of ADR-019 amendment 2 (accepted 2026-11-25, on draft PR
#190, not yet in master). Every other rule of ADR-019 and its amendments is
unchanged, and the rules below say where they must keep holding.

## Context

ADR-019 rule 5 binds the gateway to one interface's address and accepts
only loopback, RFC 1918 and link-local sources: Companion works only when
the phone and the unit reach each other directly on one network. Its
"Revisit when" names "the service is to be exposed beyond a local network
(not intended)".

On 2026-11-27, at the Companion Settings 2.0 hardware gate (Unit B, build
`d189c2c1`), the owner's phone and Unit B were both on "SkullerudPark", a
guest-style network the owner does not administer. The phone read the
pairing code, then found no answer. Seen on the unit:

- gatewayd opened its window on the unit's address on that network
  (`10.0.2.143:47231`), as designed;
- during the phone's attempt, the unit received no frame from the phone: no
  TCP SYN, and no ARP request for its address;
- the unit's ARP requests for the phone were never answered, while the
  router answered and other clients' ARP broadcasts arrived.

This points at client-to-client filtering ("client isolation") but was
**not proven**: there was no phone-side evidence (no ADB), and a scripted
repeat of the survey ran on the wrong network. Whatever that network's
cause, guest and public networks commonly isolate clients, and the product
owner asked on 2026-11-27 for a transport that still works there.

The owner's terms (2026-11-27):
- an optional, lightweight, **self-hosted** relay, reached by **outbound
  connections on TCP 443**;
- **local-first** transport selection, with the relay as fallback;
- end-to-end TLS 1.3, mutual authentication, the existing identities,
  physical pairing, pinning, scopes and revocation all preserved;
- the relay neither decrypts application traffic nor exposes arbitrary
  device services;
- remote connectivity **off by default**.

## Options

| | A. First-party blind relay, TLS inside TLS | B. Mesh VPN (WireGuard + Headscale/Tailscale, Nebula, ZeroTier) | C. TURN (coturn) | D. Reverse tunnel (frp, rathole, SSH -R) | E. Syncthing's relay server, reused | F. Local only: unit joins the phone's hotspot (F1), or hosts its own access point (F2) |
| --- | --- | --- | --- | --- | --- | --- |
| Gateway TLS end to end, unchanged | yes | yes, inside the tunnel | yes | yes | yes | yes |
| Exposes only the gateway | yes, by protocol | no: an IP path to every service of the unit, which has no firewall | yes | only in secret/visitor modes | yes | yes |
| New daemon or runtime on the K230 | no: a gatewayd module, OpenSSL already linked | yes (Go daemon or kernel module, TUN) | a TURN client in C | yes | a client in gatewayd | none for F1; hostapd and udhcpd (on the image) for F2 |
| Android | a transport in the app | a VPN service; one VPN per phone | a TURN client | another client | a client in the app | an app change (F1); none (F2) |
| Clockless | yes | expiry to be disabled; Nebula certificates carry validity | yes with static credentials | yes | yes | yes |
| Works on an isolating network without internet | no | no | no | no | no | yes |

Comparison in detail: `RELAY_PROPOSAL.md` §9.

## Decision (proposed)

**Option A for remote access**, and **option F evaluated in parallel** as
the local fallback that needs no server and no internet. A new section:

### 7. Remote transport through a DOORS Relay

28. **Off by default, and turned on only at the unit.**
    - Remote access is a second switch under Settings > Companion. It is off
      on every unit and every image: `GATEWAYD_RELAY=0` in
      `/etc/default/gatewayd`, per unit, never shipped.
    - It can be turned on only from the unit's own screen, only while the
      gateway is on (so only with a device password, rule 9b), and only with
      a relay configured on the unit.
    - No relay is built in, and Doors operates none: the owner names their own.
29. **Outbound only.** The gateway never listens on any address other than
    rule 5's. Relay traffic arrives over a connection that gatewayd itself
    opened to the one configured relay, on TCP 443. Rule 5 then reads:
    "...accepts only loopback, RFC 1918 and link-local sources, **and
    sessions that arrive over its own connection to the configured relay
    (rule 29)**".
30. **The relay is a pipe below the gateway's TLS.**
    - Between a phone and the unit, ADR-019's own TLS 1.3 session (rules 10
      to 14) runs end to end, unchanged, over the relay exactly as over a
      LAN: same keys, same pins, same full handshake, no resumption, no
      bypass.
    - The relay holds no key that could decrypt that session or stand in
      for either end.
    - Nothing the relay says is part of any trust decision on the unit or
      on the phone.
31. **The relay is pinned too.**
    - Each connection to the relay is its own TLS 1.3 session, in which the
      relay's P-256 key is pinned exactly as rules 11 and 12 pin a peer: no
      CA, no names, no dates.
    - The unit authenticates to the relay with its device key, so its
      `device_id` is its address on the relay. A phone authenticates with
      its Keystore key.
    - The relay's address and fingerprint are configured on the unit. A
      phone receives them only from the unit: inside the gateway's
      authenticated session, or in a pairing code. Never from the network.
32. **Admission is the unit's word, and only advisory.**
    - The relay connects a phone to a unit only if that unit, over its own
      authenticated connection, lists the phone's fingerprint, or while that
      unit has a pairing window open (rule 34).
    - The unit's list carries only phones that are paired **and** hold the
      scope `remote` (rule 33).
    - The relay's check keeps strangers off the unit's handshake. The
      decision remains the gateway's own pin check, which no relay can change.
    - The gateway also checks, itself, that a session which arrived through
      the relay belongs to a phone holding `remote`. If not, it closes the
      session after the handshake, whatever the relay admitted.
33. **A scope `remote`.**
    - A phone may reach the unit through the relay only with the scope
      `remote`. It is never part of a new pairing's defaults, and is given
      or taken per phone on the unit (rule 24).
    - Taking it away, or revoking the phone, updates the relay's list at
      once and ends that phone's relay sessions. The gateway closes them as
      well (rule 23).
34. **Pairing stays physical.**
    - A window still opens only on the unit (rule 20), lasts at most 120 s,
      and is approved on the panel (rules 21, 22).
    - With remote access on, the window is also announced to the relay under
      an identifier derived one-way from the window's token. A phone that
      scanned the code can then reach the unit through the relay.
    - The relay never learns the token, and holding the identifier gives
      nothing the token does not.
    - The pairing code may carry the relay's address and fingerprint.
35. **Nothing but the gateway.**
    - The relay protocol names a unit, never a port, host or service.
    - What arrives through the relay enters gatewayd's session handling and
      nothing else.
    - The relay is not a proxy and cannot be made one.
36. **Local first, and said.**
    - The phone tries the local network first. It uses the relay when that
      fails or when it is not on Wi-Fi, and moves back to the local network
      when it can.
    - The app and the unit show which path a session uses.
    - `CONTRACTS.md` §2 "Bind its sockets to the Wi-Fi network explicitly"
      is amended: **local** connections stay bound to Wi-Fi; a **relay**
      connection uses the phone's default network and only ever reaches the
      configured relay.
37. **Clockless.**
    - Nothing on the unit depends on a date: no expiring tokens, certificates
      or credentials.
    - Nonces wherever freshness is needed, and monotonic time for every timer.
    - The relay may keep time for its own rate limits.
38. **Bounded.**
    - The unit opens at most one control connection to the relay.
    - Relay sessions have their own budget inside §6's totals: at most one
      relay handshake pending and one relay session (proposed), so a phone
      on the LAN always finds a slot, whatever a relay sends.
    - Relay sessions are held to §6's handshake rate.
    - The relay enforces its own limits (`RELAY_PROPOSAL.md` §7).
    - A misbehaving relay can stop remote access, and nothing else.
39. **Logs.** The relay logs connection lifecycle, fingerprint prefixes,
    addresses and byte counts, never payload. gatewayd logs relay state
    changes, under rule 9.
40. **What the relay operator learns** is said in Settings when remote access
    is turned on: which phone keys reach which unit, when, from which
    addresses, and how much they exchange. Never what.

## Relation to accepted ADRs

- **ADR-019**: these hold unchanged, end to end:
  - rules 10 to 14 (transport);
  - 15 to 19 (keys);
  - 20 to 24 (pairing, revocation, scopes);
  - 25 to 27 (what a client can never do);
  - 9 and 9b.

  Rule 5 is amended by rule 29. The "Revisit when" item on exposure beyond a
  local network is answered here in its narrow form: outbound to a pinned
  relay, never a listener.
- **ADR-019 amendment 1** (Vision): the `vision` scope is unchanged. A
  preview through the relay costs it about 35 KB/s while shown
  (`RELAY_PROPOSAL.md` §5).
- **ADR-019 amendment 2** (product integration): Settings > Companion gains
  the remote-access switch, the relay's configuration and the scope switch.
- **ADR-016 amendment 1** (capture for a companion): unchanged. A remote
  START still needs the tap on the panel.
- **ADR-003, ADR-017, `docs/ARCHITECTURE.md`** (secrets): no new secret is
  stored. The relay's address and fingerprint are public values.
- **ADR-007**: Settings may draw or read a QR code (with the unit's camera)
  to enter the relay. No network or crypto code enters the shell.
- **AGENTS.md** (dependencies): option A adds no library to the image. The
  relay server is a new first-party program (`RELAY_PROPOSAL.md` §8).

## Consequences

Needed now: this amendment, the proposal and the threat model, and an
independent review of all three.

Needed for implementation, each a later step with its own go-ahead
(`RELAY_PROPOSAL.md` §10):
- the relay protocol and server, with host tests and a fuzzer;
- gatewayd's relay connector and the `remote` scope;
- Settings;
- the app's relay transport and path selection;
- the hardware gate, on an isolating network and on mobile data;
- an independent security review before any release.

Risks accepted with this amendment, if it is accepted:

- A paired phone with `remote` can reach the unit from anywhere the relay
  is reachable. That is the feature. The scope is off by default and per
  phone, and revocation reaches the relay at once.
- The relay operator sees metadata (rule 40) and can deny service.
- The unit keeps one outbound connection open while remote access is on.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-27 | Requested: an optional self-hosted relay over outbound TCP 443, local-first, everything of ADR-019's trust model preserved, off by default; design only, no implementation | Product owner |
