# Companion: threat model of the DOORS Relay

Status: **draft** for ADR-019 amendment 3, **accepted** by the owner on
2026-11-27 as revised. Revised after the first independent review and its
re-check, and after the second (Codex) review (`RELAY_PROPOSAL.md` §15).
R-T42 to R-T50 come from the second review. A design-time analysis for the
unit and the app: only the relay (phase 1, PR #194) is implemented, and it
is host-tested only.

- It extends `THREAT_MODEL.md`. Its threats T1 to T23 still apply to every
  gateway session, whichever path carried it.
- Rule numbers are the amendment's; section numbers are `RELAY_PROPOSAL.md`'s.

## 1. What else is protected

| Asset | Where | Why it matters |
| --- | --- | --- |
| A9 Remote availability | the relay; the helper; the control connection | Remote access stops when they fail; the LAN path must not |
| A10 Who talks to whom | the relay's memory and logs | Which phone reaches which unit, when, from which network, how much |
| A11 The relay's keys (active, backup) | the relay host; the operator's offline store | Standing in for the relay |
| A12 The unit's session slots, gatewayd's loop, the hart | ADR-019 §6, rule 7; the shell (A8) | Relay traffic must not starve LAN phones, upstream services or the panel |
| A13 The pins and the endorsement sequence | `relay.v1` on the unit; each phone's store | Keep old or foreign relay keys out |

## 2. Who else attacks

| Adversary | Can | Cannot (assumed) |
| --- | --- | --- |
| R1 A relay, compromised or hostile | see metadata; drop, delay or inject bytes; admit anyone to a unit's handshake; refuse service; flood the helper; offer its own pairing candidate | break TLS 1.3; forge a unit's or a phone's signature; learn a pairing token; see an approval; prove a key it does not hold |
| R2 The helper, compromised through a parser bug | everything R1 can, from inside the unit, under its own unprivileged id and seccomp filter; send gatewayd its small frames | read the device key or `/etc/shadow`; obtain an endorsement or promotion of a key that is not proven and pinned; block gatewayd; act as root |
| I1 Any internet host | reach the relay's port; flood it; send HTTP or TLS of any kind | reach a unit |
| N3 The network between an end and the relay | see SNI, ALPN, timing, volume and a proxy's `CONNECT` target; block; redirect; intercept TLS with its own CA | read relay sessions; pass the unit's pins or the phone's pin and endorsement check |
| P3 A paired phone with `remote`, stolen or hostile | its scopes, **from anywhere the relay is reachable** | extract its Keystore key; raise its scopes |
| S1 Someone who photographs a pairing code | become a candidate during that window | finish pairing without approval on the panel |

## 3. Threats and answers

| # | Threat | Answer | Residual |
| --- | --- | --- | --- |
| R-T1 | I1 reaches a unit | Every connection signs a relay nonce with a key; only admitted fingerprints are routed (rule 32) | The relay's TLS and HTTP start (Go) are its unauthenticated surface |
| R-T2 | R1 reads application traffic | ADR-019's TLS runs end to end inside the splice (rule 30) | Metadata (R-T15) |
| R-T3 | R1 impersonates either end | Inner mutual pins unchanged | None in the design |
| R-T4 | R1 lets strangers reach the unit's handshake | Pin check, rule 14; the gateway refuses a relay session without `remote` (rule 32) | Reachable within §8.2's budget |
| R-T5 | R1 pairs a phone of its choice | The token is on the panel only; R1 sees a one-way slot id; approval on the panel shows the key | R1 can offer its own candidate and occupy the **relay** candidate slot; the LAN slot is unaffected (R-T34) |
| R-T6 | A slot id leaks | Only candidate status during an open relay window | None beyond R-T5 |
| R-T7 | A data connection is hijacked, or an invitation replayed | One-time 128-bit cookies, valid 5 s, sent only over the helper's TLS | A denial |
| R-T8 | The unit is pointed at a fake relay | The helper accepts only pinned keys; gatewayd signs logins only for pinned keys (rule 31) | Denial |
| R-T9 | A phone is pointed at a fake relay | Normally the phone pins the relay key it learned from the unit. On the provisional path nothing goes out before the endorsement check but the HTTP start (the Host the SNI already shows) and a lookup bound to the presented key (§4.2) | The fake relay learns the phone's address, and could test candidate `device_id`s against the lookup |
| R-T10 | An endorsement is forged | Needs the unit's device key | None in the design |
| R-T11 | A stolen old relay key with its old endorsement (rollback) | Promotion removes the old pin from the unit. Each phone keeps its highest sequence, and only an authenticated gateway session resets it | A phone that missed the newer endorsement can be shown the old key: metadata and denial, never content |
| R-T12 | Signatures of the device key are confused | Distinct prefixes (§4.3) | None in the design |
| R-T13 | R1 or R2 starves LAN phones of slots | Relay budget inside ADR-019 §6; a LAN slot always free; the relay budget counts after replacement (§7.2) | Remote busy; LAN unaffected |
| R-T14 | R1 or R2 starves gatewayd's loop or the hart | No relay parsing in gatewayd. Descriptors only go out, non-blocking. Reads are bounded (`GW_READ_BATCH`). The helper runs at nice 10, with byte and frame limits before decryption, and 1 relay handshake per second | The helper's share of the hart (H2). gatewayd's synchronous `upstream_call` is a pre-existing stall for any phone (§8.4) |
| R-T15 | The relay learns who talks to whom | Self-hosted; said in Settings (rule 42); addresses truncated in logs (rule 41) | Accepted. Separate relay identities later (§17) |
| R-T16 | N3 learns that a Doors relay is used | HTTPS-shaped; the path is inside TLS; a proxy sees `CONNECT name:443` | The pattern of a long-lived connection; the relay's name |
| R-T17 | A wrong clock | Pins, sequences, nonces, monotonic timers (rule 40) | None in the design |
| R-T18 | A revocation races a relay session | gatewayd writes the change, closes the session itself, then sends the list. The relay acknowledges only after both legs of the splice have stopped (R-T43) | None: gatewayd decides |
| R-T19 | R2 abuses its line to gatewayd | An abstract socket admitted by `SO_PEERCRED`; fixed frames of at most 256 bytes and 20/s, read without a control buffer. Logins are signed only for pinned keys, 1/s. Endorsements come only from gatewayd's configuration. The backup pin is accepted only with a signature by the confirmed active key, at set-up. Promotion needs a `key_proof` under the pinned backup. gatewayd never receives a descriptor | R2 can log in as the unit to a pinned relay (it already holds that connection), and deny remote access |
| R-T20 | A parser bug in the relay path gives root, or the device key | The helper runs under its own id, without capabilities, with `no_new_privs`, seccomp and limits; it is started outside gatewayd's capability drop (rule 38). gatewayd parses no relay frame | A kernel exploit from the helper's sandbox (out of scope, as for every process) |
| R-T21 | A stalled second attempt ends a working session | The phone never runs two gateway handshakes for one unit at once (rule 37) | A hostile app build (P1) can do so to its own sessions |
| R-T22 | The phone flaps between paths | One handshake at a time; the LAN is retried only on a network change, with back-off | A brief relay session before the LAN wins |
| R-T23 | `remote` granted by mistake | The approval-sheet switch is shown off and needs its own tap; the sheet says "through your relay" | A person's error; it can be taken away on the phone's page |
| R-T24 | The relay withholds `admitted` after a relay pairing | The pairing is committed first. gatewayd waits at most 3 s, then answers `pending`. The phone retries twice, then backs off. The list is sent again after every registration (§6.1) | Remote access pending for that phone: a denial, never a lost pairing |
| R-T25 | P3 from far away | `remote` never a default; revocation at the unit is immediate on the relay too | What it did before revocation |
| R-T26 | N3 intercepts TLS (captive portal, its own CA) | Pins or the endorsement check fail; the app says why; no bypass | No remote on that network |
| R-T27 | The relay is abused as an open proxy | No host, port or service in the protocol; only enrolled units | None in the design |
| R-T28 | Someone enrols another unit on the owner's relay | The operator's act; units isolated from each other | Capacity |
| R-T29 | Logs leak secrets or locations | No secret in clear; addresses truncated; a lint on log calls | - |
| R-T30 | Settings is set up with a relay the owner does not run | Set-up only on the unit's screen; the person compares the 16-digit fingerprint with the one the relay printed | A person who confirms a stranger's relay gives it metadata |
| R-T31 | S1 uses a photographed code from anywhere | The relay path of a window is off unless switched on for that window; approval and key comparison on the panel still decide | During a relay-enabled window, a remote stranger can become the relay candidate (and be refused on the panel) |
| R-T32 | R1 or R2 abuses rotation (re-accept the old key, burn the backup, wear the card) | One-way promotion, only after a `key_proof` under the pinned backup key, written once per configuration; the old pin removed | None in the design |
| R-T33 | A phone without `remote` cannot learn of a later grant, and is locked out on isolating networks (availability) | "Try through the relay" on the person's tap is admitted once `remote` is given | The person must tap |
| R-T34 | R1 blocks LAN pairing through the shared candidate slot or strike count | Separate relay candidate slot and strike count; relay strikes close only the relay path | None in the design |
| R-T35 | A rollback of gatewayd meets an unknown scope bit and goes to `state_fault` | `remote` is kept in `relay.v1`, which an older gatewayd ignores | Remote is off after a rollback |
| R-T36 | A LAN address change ends relay sessions, or no private address stops remote | Only reasons of address spare relay sessions and relay-only windows (rule 29) | None in the design |
| R-T37 | R2, or a relay reached before confirmation, plants an attacker's key as the backup pin, then promotes it and captures the unit's relay (found in the re-check) | `backup` is signed by the human-confirmed active key over a fresh gatewayd nonce, verified by gatewayd, and accepted at set-up only (§4.1) | None in the design |
| R-T38 | Relay sessions outlive a gateway that must close (no password, `identity_fault`, `state_fault`): rule 9b bypassed for remote phones | Every reason of the gateway's own closes relay sessions and disconnects the helper; only reasons of address spare them (§7.2) | None in the design |
| R-T39 | After a lost `relay.v1` and a new set-up, old endorsements of a stolen key pass again because the sequence restarts | A random configuration id in every endorsement; a new id reaches phones only over an authenticated session or a pairing (§4.2) | None in the design |
| R-T40 | R1 crashes the helper repeatedly until `pos-supervise` stops it for good | The helper never exits on relay misbehaviour; it closes and backs off (§8.1) | A bug that crashes it is still a crash: remote access stops until the next start, and the LAN is unaffected |
| R-T41 | R1 or R2 refuses a returning phone through the limit of 1 | The relay, the helper and gatewayd all replace the same phone's session (§7.2) | None in the design |
| R-T42 | A data connection joins with a cookie issued on a superseded control connection, after its deadline, or for a pairing window that has closed or changed (second review, finding 1) | Each invitation holds its control generation, its slot generation and a monotonic deadline. `join` checks and consumes them in one critical section. A replacement, a control loss or a slot change ends pending invitations within its own critical section. `decline` counts only on its own generation (`docs/api/relay.md` §7) | None in the design; the relay is trusted for routing (denial only) |
| R-T43 | The relay acknowledges a revocation while the splice still forwards, or acknowledges for a superseded control (finding 2) | Both legs closed, and forwarding stopped, before `admitted`. `forwarding` is entered only under the lock. Acknowledgements only for committed updates on the current generation, matched to `n` | A lying relay can acknowledge falsely. gatewayd's own close and `remote` check decide (rule 32) |
| R-T44 | The relay offers two places while the unit has one, so a candidate is admitted and then refused, or a remote session starves relay pairing unpredictably (finding 3) | One place at the relay, the helper and gatewayd. Busy is uniform and said. Separate LAN and relay candidate state | Relay pairing is busy during a remote session (accepted, ADR "Consequences") |
| R-T45 | A crash, a rollback or a pairing again revives a stale `remote` grant (finding 4); a rollback gatewayd rewrites `paired.v1` byte for byte so that its digest matches (re-review F5) | The two-file rule with grants before and after, sealed with `paired.v1`'s identity (digest, device, inode, size, change and modification time). Recovery fails closed: the grants after only under an intact seal; an interrupted change keeps only grants both before and after; anything else drops every grant. A grant counts only for a key in `paired.v1` (§6.1) | After a rollback or an interrupted change, phones lose `remote` until it is given again. A rewrite that keeps every identity field to the nanosecond needs root on the unit; on coarse timestamps without a real-time clock, an inode reused within one tick with identical bytes is conceivable (ASSUMED) |
| R-T51 | A phone, or anyone holding the slot, connects from many sources so that the unit's answers to the invitations exceed the control's frame limit and cut it, or the invitations fill its queue (re-review F2, M3b) | The relay invites a unit at most once a second and refuses the rest with `unavailable` before inviting; the helper acts on at most one a second and drops the rest silently, with no `decline` (§8.3; `docs/api/relay.md` §10) | A listed phone that reconnects within a second of another invitation is refused once and retries |
| R-T52 | Unauthenticated connections from a few sources fill the global cap and lock units and phones out (re-review F3: 16 sources × 64 reach 1024; 16 IPv6 /64s fit in one /60) | **Not resolved: a mandatory gate before any public deployment** (`docs/api/relay.md` §15.6). Owner-operated, non-public relays only until it is resolved | Until then, an unauthenticated flood from 16 sources can deny the relay to everyone |
| R-T46 | A phone is told it has remote access that the relay never admitted, or retries create a second pairing (finding 4) | An explicit `pending` outcome. A full snapshot after every `registered`. Retries are ordinary authenticated sessions, with no second entry or approval | Pending until the relay acknowledges |
| R-T47 | I1 exhausts the relay's memory or CPU through source churn, or stalls it through log output (finding 5) | A hard cap on tracked sources with bounded sweep work per accept. A bounded, non-blocking log queue that drops and counts. Rate-limited refusal lines. Churn, shared-NAT and slow-reader runs measured (`docs/api/relay.md` §15) | While the source table is full, new sources are refused: an availability cost under a distributed flood |
| R-T48 | R2 replays or misroutes helper frames (a proof, backup or acknowledgement of an old connection), or a local process takes the abstract socket name first | Every frame carries the generation and its operation, and gatewayd acts only on current ones with the expected pin. Both ends check `SO_PEERCRED`. gatewayd bounds its own work per loop turn (§8.1) | R2 can deny remote access (R-T19) |
| R-T49 | A relay session of a phone without `remote` displaces its working LAN session | Eligibility is checked on the actual ingress path after the inner handshake, before any replacement (§7.2; rule 37) | None in the design |
| R-T50 | After both relay keys are lost, or a new set-up, phones are stranded, or adopt the new configuration on the provisional path | A new configuration id reaches a phone only through an authenticated `gateway.hello` or a pairing. The runbook says a published backup installs nothing on a unit (§4.4) | Phones away from the LAN have no remote access until they return to it, or are paired again |

## 4. What does not change

- gatewayd and every other Doors service but the helper are root. The
  gateway narrows what reaches them.
- The device key is protected by file permissions (T10). Whoever reads the
  card can also endorse a relay of their own; they can impersonate the unit
  already.
- gatewayd's pre-authentication surface is OpenSSL's TLS 1.3 handshake.
  Through the relay it is reachable only within the relay budget.

## 5. Checks that must exist before this carries real traffic

1. **The unit's pins:**
   - only `A` or `B` is accepted, with a single certificate or a CA chain,
     whatever the dates;
   - TLS 1.2, an RSA key and a missing SNI are refused;
   - logins are signed only for pinned keys.
2. **The backup and promotion:**
   - a `backup` without a valid signature by the confirmed `A`, or outside
     set-up, is refused;
   - `B` is promoted only after a valid `key_proof`, and only once;
   - `A` is refused afterwards;
   - a hostile helper's false `presented {B}`, without a proof, changes
     nothing and writes nothing;
   - promotion is one write: a fault-injection test restarts at every point
     and finds the old state or the new one, never a mix;
   - after promotion every connection presenting the old `A` is closed;
   - set-up and adding a backup run over a fresh connection pinned to the
     full confirmed hash, within the relay's deadline (R-T50 runbook).
3. **Endorsements and the phone:**
   - the pinned path is used when the key is known;
   - on the provisional path, each wrong field (signature, name, port, key,
     configuration id, lower sequence) is refused;
   - after a new set-up (new id), an old endorsement with a higher sequence
     is refused;
   - a relay with a CA chain passes the leaf-only pinned path;
   - nothing beyond the HTTP start and the bound lookup is sent before
     acceptance;
   - the unit never endorses an unpinned key;
   - a sequence reset happens only through `gateway.hello`.
4. **Rotation with the phone away:** `A` to `B` through the relay alone, then
   `A` is refused.
5. **Admission:**
   - unknown, not admitted and offline all get `unavailable`;
   - a removal ends the splice within a second;
   - `admitted` is sent only after both legs of a removed phone's splice are
     closed, also when forwarding is blocked or the splice is in its `ready`
     transition;
   - a superseded control's update is neither applied nor acknowledged, and
     a non-increasing `n` closes the connection;
   - `registered` comes after a replacement's effects;
   - a paired phone without `remote` that a lying relay passes through is
     closed by the gateway.
6. **Pairing:**
   - a window with the relay off has no `r` and no slot;
   - with it on: a slot, a relay candidate and a LAN candidate together, and
     three wrong tokens on the relay path (the LAN path still open);
   - `remote` on with `admitted` first, off with the message, and the later
     tap after a grant;
   - a withheld `admitted`: the pairing stands, the answer is `pending`, a
     retry creates no second entry, and the snapshot after the next
     `registered` admits the phone;
   - the two-file rule's three start cases, and a revocation followed by a
     pairing again without `remote` (no grant revived);
   - a slot clear or change ends pending candidate invitations, and a
     running candidate session is left to gatewayd;
   - one place: a remote phone's session makes a relay candidate busy, and
     the reverse;
   - the token absent from the relay's memory and logs (a test build);
   - the code under 512 characters with a 64-character name.
7. **Races:**
   - LAN and relay ready together: one gateway handshake;
   - replacement at the end of the handshake, before the limits;
   - a phone returning through the relay replaces its dead relay session at
     the relay, in the helper and in gatewayd;
   - a relay session of a phone without `remote` never displaces its LAN
     session;
   - `join` against expiry, control replacement and slot changes, decided
     deterministically at the relay;
   - a send in flight is transmitted once.
8. **Isolation:**
   - the helper runs under its id, with no capabilities, `no_new_privs`,
     seccomp and its limits, and cannot open `identity.v1` or `/etc/shadow`;
   - `SO_PEERCRED` refuses any other peer, in both directions (a squatter
     on the abstract name gets nothing from the helper);
   - helper frames of an old generation, or for no outstanding operation,
     change nothing;
   - the list of local sockets and network destinations the helper can
     reach is recorded and reviewed;
   - gatewayd never accepts a descriptor;
   - a lying relay and a hostile helper (floods, oversized frames, signing
     above the rate) end in refusals, declines or a restart, while pocketipc
     reads stay within budget and a LAN phone stays served, with its latency
     measured.
9. **The listener:**
   - relay sessions survive an address change, and a relay-only window opens
     without a private address (with no `h` in the code);
   - with the password removed (`passwd -d`), `identity_fault`,
     `state_fault` or the switch off, relay sessions close and the helper
     disconnects;
   - the helper survives a relay that misbehaves without exiting.
10. **Restrictive networks:**
    - an HTTP proxy with `CONNECT`;
    - a captive portal (no attempt);
    - TLS interception (fails closed, with the message);
    - a plain HTTP request (`404`);
    - a CA-issued chain for the pinned key (accepted).
11. **Rollback:** an older gatewayd with a `relay.v1` present starts normally,
    with remote off.
12. **Fuzzers** over the relay's HTTP start and frames (Go), and the helper's
    and gatewayd's helper-line readers (C, with sanitizers). The relay's
    rejection vectors (duplicates, encodings, `null`s, integer overflow,
    malformed DER) are shared test data the C and Kotlin readers must also
    refuse.
13. **The relay's own resources:** the source table never exceeds its cap;
    a full table refuses at accept; sweep work per accept is bounded; a
    blocked log writer never stalls a connection; connection churn, a shared
    NAT and slow readers are measured (`docs/api/relay.md` §15).
14. **On hardware**, with a go-ahead: `RELAY_PROPOSAL.md` §16, phase 4.
