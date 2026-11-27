# Companion: threat model of the DOORS Relay

Status: **draft** for ADR-019 amendment 3 (Proposed, 2026-11-27). A
design-time analysis: nothing is implemented or tested, and no independent
review has taken place. It extends `THREAT_MODEL.md`, whose assets, adversaries
and threats T1 to T23 still apply to every session, whichever path carried it.

## 1. What else is protected

| Asset | Where | Why it matters |
| --- | --- | --- |
| A9 Remote availability | the relay; the unit's control connection | Remote access stops when either fails; the LAN path must not |
| A10 Who talks to whom | the relay's memory and logs | Which phone reaches which unit, when, from where, how much |
| A11 The relay's key | the relay's host | Standing in for the relay to units and phones |
| A12 The unit's session slots and the hart | gatewayd (ADR-019 §6), one hart shared with the shell (A8) | Relay traffic must not starve LAN phones or the panel |

## 2. Who else attacks

| Adversary | Can | Cannot (assumed) |
| --- | --- | --- |
| R1 The relay, compromised or run by someone hostile | see metadata (A10); drop, delay, reorder or inject bytes; admit anyone to a unit's handshake; refuse service; flood the unit with invitations | break TLS 1.3; read or forge either end's key; learn a pairing token; see an approval |
| I1 Any host on the internet | reach the relay's port; flood it; guess device ids | reach a unit (no listener faces it) |
| N3 The network between an end and the relay (guest Wi-Fi, carrier, captive portal) | see that a TLS connection to the relay's address on 443 exists, its ALPN, timing and volume; block it; redirect it | read it; pass the relay's pin |
| P3 A paired phone with `remote`, stolen or with a hostile app build | everything its scopes allow, **from anywhere the relay is reachable** | extract the Keystore key; raise its scopes; revoke itself back in |

Out of scope, as before: code already running as root on the unit, a rooted
phone, and the security of the relay host's own operating system beyond what
R1 covers.

## 3. Threats and answers

| # | Threat | Answer | Residual |
| --- | --- | --- | --- |
| R-T1 | I1 reaches a unit through the relay | Outer TLS requires a key; the relay routes only fingerprints a connected unit admitted (rule 32); one answer, `unavailable`, for unknown, offline and not-admitted, so no oracle | The relay's own TLS handshake is its unauthenticated surface |
| R-T2 | R1 reads application traffic | The gateway's TLS 1.3 runs end to end inside the splice; the relay has no key for it (rule 30) | Metadata, A10 (R-T15) |
| R-T3 | R1 impersonates the unit to a phone, or a phone to the unit | Inner mutual pins unchanged (ADR-019 rules 11-13); the relay cannot finish either side of the inner handshake | None in the design |
| R-T4 | R1 lets strangers reach the unit's handshake | gatewayd's pin check closes an unknown key outside a window before any application byte (rule 14); the relay's admission is advisory only | The unit's handshake becomes reachable to whomever R1 sends (bounded by R-T13) |
| R-T5 | R1 pairs a phone of its choice | A pairing needs the token, which is only on the panel, and approval on the panel showing the candidate's key (rules 21-22, amendment 2). R1 sees only a slot id derived one-way from the token (rule 34) | During an open window R1 can put its own candidate first and block the owner's pairing: the panel shows a key that does not match the owner's phone, as T3 |
| R-T6 | A pairing slot id leaks | It grants only being invited as a candidate while the window is open; the token and the approval are still required | None beyond R-T5 |
| R-T7 | A data connection is hijacked or an invitation replayed | The cookie is 128 random bits, one-time, bound to the unit's authenticated control connection, valid for seconds, and never seen on the wire (outer TLS) | A hijack can only fail the inner handshake: a denial |
| R-T8 | N3 or DNS points an end at a fake relay (captive portal, DNS hijack, routing) | The relay's key is pinned at both ends, no CA, no fallback (rule 31); the app says "relay key mismatch" and does not connect | Denial of remote access |
| R-T9 | P3 uses a stolen phone from far away | `remote` is off by default and per phone (rule 33); revoking on the unit closes the relay session at once and removes the phone from the relay's list; the unit shows which phone is connected and through which path | What it did before revocation; revocation needs someone at the unit (by design, as rule 23) |
| R-T10 | Someone blocks the LAN path to force the relay | Both paths carry the same inner TLS, so security is equal; the app and the unit show the path (rule 36) | The relay operator then sees the session's metadata |
| R-T11 | The relay is abused as an open proxy | The protocol names a unit, never a host, port or service (rule 35); only enrolled units; per-unit quotas | None in the design |
| R-T12 | I1 floods the relay | Limits per source, per unit and global (`RELAY_PROPOSAL.md` §7) | Remote access is unavailable; the LAN path is unaffected |
| R-T13 | R1 floods the unit with invitations, or keeps its slots busy, to starve LAN phones (A12) | Invitations are bounded by the gateway's own slots and handshake rate (rule 38). **Relay sessions get their own budget, below ADR-019 §6's totals**: proposed at most 1 pending relay handshake and 1 relay session, so a LAN phone always has a slot | A hostile relay can keep remote access busy; LAN unaffected |
| R-T14 | Double handshakes (outer and inner) cost the hart (A8) | Rate limits as above; nice 5; the cost is measured at the hardware gate (open item H2) | Unmeasured |
| R-T15 | The relay learns who talks to whom (A10) | Self-hosted by the owner; said in Settings (rule 40); logs carry fingerprint prefixes, not names | Accepted. A later step could give each unit a separate relay key so relay logs cannot be joined with LAN pairings |
| R-T16 | N3 learns that a Doors relay is used | ALPN `doors-relay/1` is visible in the ClientHello (no ECH); content is not | Accepted; a neutral ALPN would only hide the name, not the pattern |
| R-T17 | A wrong or reset clock breaks trust | Nothing on the unit reads a date: pins, nonces, monotonic timers (rule 37) | None in the design |
| R-T18 | A revocation races a live relay session | gatewayd closes the inner session itself (rule 23), then publishes the new list; the relay closes the splice; the next connection is not admitted | None in the design: gatewayd is authoritative |
| R-T19 | A11 is stolen | The thief is a relay (R1): metadata and denial, no decryption, no impersonation of either end. Rotation: a new fingerprint configured on the unit; phones learn it inside the authenticated session; the old one is removed | Metadata until rotation |
| R-T20 | Someone enrols another unit on the owner's relay | Enrolment is the relay operator's act, per device key; another unit gains nothing over this unit's sessions | Relay capacity |
| R-T21 | The relay's logs leak secrets | No secret crosses the relay in clear; rule 39 and a lint on the server's log calls | - |
| R-T22 | A phone without the scope reaches the unit through the relay | Not in the unit's list (rule 32), and even if R1 admits it, the gateway knows it is not admitted for relay sessions and closes it after the handshake | None in the design |

Design points this model adds to the proposal:
- the separate relay budget of R-T13;
- the gateway's own check that a relay session's client holds `remote`
  (R-T22): the gateway does not rely on the relay for that either.

## 4. What does not change

- Every Doors process is root; the gateway narrows, it does not contain.
- The device key is protected by file permissions only (T10).
- The pre-authentication surface on the unit is OpenSSL's TLS 1.3 handshake.
  It is now reachable through the relay, but only for phones the relay admits
  (or anyone, if the relay is hostile, bounded by R-T13).

## 5. Checks that must exist before this carries real traffic

1. **Outer TLS:** the relay's pin accepted with any date, name or issuer, and
   refused with the wrong key; TLS 1.2, a missing client certificate, an
   RSA key, a second certificate all refused.
2. **Admission:**
   - an unknown phone, a not-admitted phone and an offline unit all get the
     same `unavailable`;
   - a phone removed from the list loses its splice within a second.
3. **Pairing through the relay:**
   - slot open, slot withdrawn, a second candidate;
   - a candidate with the slot but a wrong token;
   - approval on the panel with the fingerprint compared;
   - the relay never sees the token (a test that greps the relay's memory and
     logs in a test build).
4. **Hostile relay tests,** with a test relay that lies:
   - it admits a stranger;
   - it injects bytes into a splice;
   - it replays a cookie;
   - it floods invitations;
   - it claims another unit's id.

   Each must end in a refused handshake or a declined invitation, with the
   LAN slot still free.
5. **Revocation and scope:**
   - `remote` taken: the live relay session closes and the next one is refused;
   - LAN sessions of the same phone are unaffected unless revoked.
6. **Limits:** the relay's per-source, per-unit and per-session limits under
   load; the unit's relay budget (R-T13).
7. **A fuzzer** over the relay's frame reader and gatewayd's control-frame
   reader, with sanitizers.
8. **On hardware**, with a go-ahead:
   - pairing and sessions through the relay from an isolating network and
     from mobile data;
   - the handshake cost and the shell's frame time;
   - a relay restart, a unit reboot and a network change;
   - a flood against the relay while a LAN phone keeps working.
