# DOORS Relay protocol v1 (`doors-relay/1`)

Status: **draft. Phase 1 is built and host-tested only: the Go relay in `relay/`, against Go stand-ins of the unit and the phone.**
- Nothing is deployed, and no relay host exists.
- The unit's side (gatewayd and its helper) and the app's side are phases 2 and 3, and neither exists yet.
- This revision answers the Codex review of 2026-11-27. It follows its resolution (`RESOLUTION-review-1.md`, outside the repository; the same contract is in `RELAY_PROPOSAL.md` §15.3 and the threat model).

This is the wire protocol of ADR-019 amendment 3 (rules 28 to 42, accepted 2026-11-27), as specified by `docs/design/companion/RELAY_PROPOSAL.md` §4, §5, §6.1, §8.3 and §13.
- Where the proposal was silent, the first revision made the smallest decision consistent with the amendment and marked it **[Q*n*]**.
- The owner has accepted every disposition of the review. Q9, Q22 and Q27 were revised. §14 lists them as resolved.

Threats and required checks: `docs/design/companion/RELAY_THREAT_MODEL.md`. The gateway protocol, which runs unchanged inside a splice: `docs/api/gateway.md`.

## 1. Overview

The relay is a blind pipe. It connects a phone's TCP stream to a unit's, after both have proved their keys, and only for phones the unit has listed. ADR-019's TLS 1.3 session runs end to end inside the splice. The relay holds no key that could read it or stand in for either end (rule 30).

There are three kinds of connection. Each is opened by an end, never by the relay, over TCP to the relay's public port (443):

| Connection | Opened by | Purpose | Authenticated by |
| --- | --- | --- | --- |
| control | the unit's helper, while gatewayd asks for it (rule 29), or for a person's set-up | the unit's login, its admission list, pairing slot and endorsement; invitations | a unit login signed by the device key over the relay's nonce |
| client | the phone | asking for a session with one unit | a client login signed by the phone's key over the relay's nonce |
| data | the unit's helper, one per invitation | the unit's end of one session | a one-time cookie from an invitation |

- **One place per unit (Q9):** each unit has one relay place, held by one pending invitation or one splice, of a listed phone or of a pairing candidate.
- Nothing in the protocol names a port, a host or a service (rule 35).

## 2. Conventions

- **Keys** are ECDSA P-256. On the wire a public key is its DER SubjectPublicKeyInfo with an uncompressed point: exactly 91 bytes, in exactly the encoding `x509.MarshalPKIXPublicKey` produces (Q25). A non-canonical encoding of the same key is refused, because its fingerprint would differ.
- **A fingerprint** is the SHA-256 of a DER SubjectPublicKeyInfo (PAIRING.md §1). A unit's fingerprint is its `device_id`. "Relay key hash" means the fingerprint of the key the relay presented on the connection.
- **For people**, a fingerprint is shown as its first sixteen hex digits, in upper case and in groups of four (`3F2A 91C4 0B7E D115`). **In logs** it is the same sixteen digits in lower case, without spaces.
- **Signatures** are ECDSA P-256 over SHA-256 of the message, DER-encoded: one SEQUENCE of two positive, minimally encoded INTEGERs, at most 72 bytes, and nothing after it. This is what OpenSSL's `ECDSA_sign` and Android's `SHA256withECDSA` produce. Low-S is not required.
- **Byte strings** are concatenated with `||`. Integers in signed messages are big-endian.
- **Context strings (Q11):** a signed or hashed message starts with its ASCII context string followed by one 0x00 byte (§8). The pair slot's HKDF `info` is the bare ASCII string, without the 0x00.
- **In JSON:**
  - SHA-256 values (fingerprints, `device_id`, key hashes, the lookup) are 64 lower-case hex digits.
  - Every other binary value (nonces, keys, signatures, cookies, slots, endorsements) is base64 per RFC 4648 §4: standard alphabet, padded, with no line breaks and no non-zero padding bits.
  - Counters (`n`) are unsigned decimal integers (§5).

## 3. TLS (rules 31, 39)

- **Version:** TLS 1.3 only. The relay refuses TLS 1.2 and older.
- **SNI:** the relay's DNS name, as configured. A missing or different SNI completes the handshake and is then answered `404` (§4).
- **ALPN (Q15):** the ends offer `http/1.1` and nothing else. A client that offers no ALPN completes the handshake and gets `404`. A client that offers only other protocols fails the handshake.
- **The relay's certificate:** one self-signed certificate for its key, or a CA-issued chain whose leaf carries the same key (rule 39, for middleboxes that check certificates).
  - The self-signed certificate has fixed, informational fields, as the gateway's do (PAIRING.md §2): `notBefore` 1970-01-01T00:00:00Z, `notAfter` 9999-12-31T23:59:59Z, `CN=doors-relay`, a random serial.
- **No client certificate** on the outer TLS. Each end proves its key with a signed login inside it (§7).
- **No session resumption:** the relay issues no tickets, so every connection presents the leaf.
- **Pinning, at both ends** (not the relay's check). There are two distinct policies (Q25):
  - **Outer relay pin, leaf only:** the leaf certificate's key decides, and it must be ECDSA P-256 with a fingerprint that is a pin. The rest of a chain is ignored, and so are dates, names, issuers and extensions. The unit accepts only its pins A and B (§9.1). The phone accepts its pin, or takes the provisional path (§9.2).
  - **Inner gateway pin:** exactly one certificate, unchanged (ADR-019 rule 11).

## 4. The HTTP start

After the handshake the connecting end sends exactly this request (rule 39):

```
GET /doors-relay/1 HTTP/1.1\r\n
Host: <relay name>[:<port>]\r\n
Connection: Upgrade\r\n
Upgrade: doors-relay/1\r\n
\r\n
```

- **Port in Host:** ends write `:<port>` only when the port is not 443. The relay accepts the name with or without its configured public port (Q13).
- **What the relay accepts, checked on the raw lines (Q13, Q20):**
  - The request line is exactly `GET /doors-relay/1 HTTP/1.1`.
  - There are exactly the three headers `Host`, `Connection` and `Upgrade`, each once, in any order, and no other header and no body.
  - Each header line is a token, `:`, at most one space, and a value of printable ASCII (0x21 to 0x7E) with no other whitespace. There is no obs-fold, and every line ends in CRLF.
  - Header names, the name in `Host`, and the values `Upgrade` and `doors-relay/1` compare without regard to ASCII case.
  - The whole head (request line, header lines and the empty line) is **at most 2048 bytes**.
  - The head starts with its request line: a leading empty line is malformed.
  - SNI names the relay, and ALPN is `http/1.1`.
- **The sender waits (Q13):** the connecting end writes nothing after the empty line until it has read the 101 and the challenge. The relay can detect a violation only when bytes are already read ahead together with the head. It answers those with `404`.
- **The answer to the Upgrade**, byte for byte:

  ```
  HTTP/1.1 101 Switching Protocols\r\n
  Connection: Upgrade\r\n
  Upgrade: doors-relay/1\r\n
  \r\n
  ```

  The relay's `challenge` frame (§6) follows immediately.
- **Anything else** gets the answer an ordinary Go server sends, without a `Date` header, and the connection ends. That covers a well-formed request for another path, method or host, a head that is malformed or longer than 2048 bytes, and a missing or wrong SNI or a missing ALPN:

  ```
  HTTP/1.1 404 Not Found\r\n
  Content-Type: text/plain; charset=utf-8\r\n
  X-Content-Type-Options: nosniff\r\n
  Content-Length: 19\r\n
  Connection: close\r\n
  \r\n
  404 page not found\n
  ```

- **Plain TCP without TLS** gets no HTTP answer: the handshake fails and the connection ends.
- **Implementation (Q20):** the relay owns the TLS connection. It reads the head with its own 2048-byte reader, checks the raw lines, and parses with Go's `net/http.ReadRequest`. There is no `net/http.Server` and no `Hijack`.

## 5. Frames

- After the 101, both directions carry frames: a 4-byte big-endian length, then that many bytes of UTF-8 JSON encoding one object. This is the gateway's framing with a smaller limit.
- **The length is 1 to 4096.** A length of 0 or above 4096 ends the connection.
- **A frame is refused, and the connection ends without an answer, if** (Q12):
  - it is not valid UTF-8, or not exactly one JSON object with nothing after it but whitespace;
  - a member appears twice (also after unescaping), a member is missing or unknown for its type, or `null` appears where it is not allowed;
  - a value has the wrong type, encoding or length;
  - `n` is not an unsigned decimal: `0`, or a non-zero digit followed by digits, at most 4294967295, with no sign, fraction or exponent;
  - a key is not the canonical 91-byte SubjectPublicKeyInfo of a point on P-256;
  - a signature is not DER as §2 says.

  This holds in both directions.
- **Vectors:** `relay/internal/wire/testdata/vectors.json` publishes accepted and refused payloads for the C and Kotlin ends to reuse. They cover duplicates, encodings, `null`s, integer syntax and overflow, and malformed DER.
- **Versions:** none is negotiated inside the protocol. A new version is a new path, `/doors-relay/2`.
- `type` names the frame. The members of each type follow.

## 6. Frame types

| `type` | Members | Sent by | On | Meaning |
| --- | --- | --- | --- | --- |
| `challenge` | `nonce` (32 bytes) | relay | every connection, first | Fresh random bytes for this connection's login |
| `unit` | `spki`, `sig` | unit | control, first | Unit login (§8.1) |
| `prove` | `nonce` (32 bytes) | unit | control, after `unit` and before `register`, at most once | Asks the relay to prove the key it presented |
| `key_proof` | `spki`, `sig` | relay | control | The relay's key and its signature over the unit's nonce (§8.4) |
| `get_backup` | `nonce` (32 bytes) | unit | control, after `unit` and before `register`, at most once | Asks for the backup pin (Q3) |
| `backup` | `spki_a`, `spki_hash_b` (hex), `sig` | relay | control | The active key, the backup key's hash, and the active key's signature (§8.5) |
| `register` | `admit` (array of 0 to 8 distinct hex fingerprints), `endorsement` (object `{e, sig}`) | unit | control, once | Makes this the unit's control connection and replaces an earlier one (§7.2) |
| `registered` | none | relay | control, once | The registration has taken effect: routing to the unit is available (Q22) |
| `admit` | `admit` (as above), `n` | unit | control, after `register` | Replaces the admission list. `n` is greater than every earlier `n` on this connection |
| `admitted` | `n` | relay | control | The list of the `admit` with this `n` is in force, and removed phones' splices have stopped |
| `pair_slot` | `slot` (16 bytes, or `null`) | unit | control, after `register` | Sets the pairing slot, or clears it with `null` |
| `invite` | `cookie` (16 bytes), `client` (hex), `via` (`"admit"` or `"pair"`) | relay | control | A phone wants a session. `client` and `via` are hints without authority (Q5) |
| `decline` | `cookie` | unit | control | Refuses that invitation |
| `ping` | `n` | relay or unit | control | Asks for a `pong` with the same `n` |
| `pong` | `n` | relay or unit | control | Answers a `ping` |
| `which` | `lookup` (hex) | phone | client, first, provisional path only | Asks for a unit's endorsement without naming the unit (§8.6) |
| `endorsement` | `spki`, `e`, `sig` | relay | client | The unit's key, its endorsement bytes, and the device key's signature (Q1) |
| `client` | `spki`, `device_id` (hex), `sig` | phone | client: first, or after `endorsement` | Client login, naming the unit (Q2) |
| `connect` | `slot` (16 bytes; optional, never `null`) | phone | client, after `client` | Asks for the session. A candidate presents the pairing slot |
| `ready` | none | relay | client; data (Q4) | The splice begins with the next byte |
| `unavailable` | none | relay | any | The relay's one refusal; the connection then ends |
| `join` | `cookie` (16 bytes) | unit | data, first | Takes the invitation with this cookie |

`spki`, `spki_a`, `e`, `sig`, `nonce`, `cookie` and `slot` are base64. `e` is 79 to 142 bytes (§8.2). `spki` is the 91-byte form of §2.

## 7. Sequences

### 7.1 Every connection

TLS (§3), then the HTTP start (§4), then the relay's `challenge`. The connecting end's first frame decides what the connection is:
- `unit` makes it a control connection;
- `which` or `client` makes it a client connection;
- `join` makes it a data connection.

Any other first frame ends the connection.

### 7.2 Control connection

```
unit ──► (refused: unavailable, end)
[prove ──► key_proof]                     at most once, before register
[get_backup ──► backup | unavailable+end] at most once, before register
register ──►            ◄── registered    within 10 s of the TCP accept
   then:  admit ──► admitted     pair_slot ──►     decline ──►
          ◄── invite             ping ◄──► pong
```

- **Login.** The relay accepts `unit` only if all of these hold, and otherwise answers `unavailable` and ends the connection:
  - `spki` is an enrolled unit's;
  - `sig` verifies over the unit login message (§8.1), with this connection's nonce and the relay key hash of the key presented on it.
- **Deadline (Q7):** login and `register` must come within 10 s of the TCP accept. On a set-up or promotion connection that covers the `get_backup` or `prove` exchange, and the unit's durable promotion write. If it passes, the connection ends, and the unit dials again with the durable state it has.
- **Register (Q8, Q22).** The relay checks the endorsement first (Q18, input validation only): `e` must parse as §8.2, name the relay's configured name and public port and the key it presented, and verify under the unit's key. Any other endorsement ends the connection, and the unit stays offline.

  Then, in **one critical section** of the unit's state:
  1. the control generation rises by one, and this connection becomes the unit's control at that generation;
  2. the list and the endorsement are this frame's, and the unit has no pairing slot;
  3. a pending invitation ends, and its phone gets `unavailable`;
  4. a candidate's splice ends;
  5. a listed phone's splice ends unless the new list names it.

  After that section, the sockets of the ended splices close and their forwarding stops, and the earlier control connection closes. **Only then** does the relay send `registered`. From `registered` on, routing to the unit is available. The unit treats the relay as available only after it, and sends its full admission snapshot (an `admit`) before anything else, including before its first `pair_slot` (`RELAY_PROPOSAL.md` §6.1).
- **Admission list (finding 2).** `n` must be greater than every earlier `n` on this control connection; the first may be any value, and a non-increasing `n` ends the connection. The list `register` carries is confirmed by `registered`, not `admitted`. On `admit`:
  1. **Under the lock:** check that this connection is still the unit's current control, replace the list, end a dropped phone's pending invitation (its phone gets `unavailable`), and stop a dropped phone's splice.
  2. **Close both legs** of a stopped splice at once, and wait until its copy goroutines have returned. Closing the sockets unblocks any read or write in progress, so the wait is bounded.
  3. **Only then**, and only if this connection is still the current control, send `admitted {n}`.

  A superseded control's `admit` is neither applied nor acknowledged.
- **What an acknowledgement means:** `registered` and `admitted` mean only that this relay reports the update applied. A hostile relay can lie. gatewayd's own `remote` check, and its own close of a revoked phone's sessions, stay authoritative (rule 32).
- **Pairing slot (Q27).**
  - `pair_slot` sets or clears the unit's one slot.
  - A **change** (a new slot, or a clear) ends a pending `pair` invitation, and its phone gets `unavailable`. Setting the same slot again changes nothing.
  - A **running** candidate splice is not ended by a slot change. gatewayd owns its final response, timeout, expiry, revocation and teardown.
- **Decline.** `decline` counts only for a pending invitation issued on this connection's control generation. Anything else is ignored without an answer: a race with expiry or cancellation is normal.
- **Pings (Q6).** The relay sends `ping` every 25 s and answers every `ping` with a `pong` of the same `n`; the unit must do the same. A `pong` is telemetry: it is not matched, and a stray one is ignored. Liveness is any valid frame, and either end closes after 75 s in which it received none.
- **Prove and backup** are answered only between `unit` and `register`, once each per connection. If no backup key is configured, `get_backup` is answered `unavailable` and the connection ends.
- **Queue:** the relay queues at most 32 frames to a unit. A unit whose queue is full, or whose frame write takes more than 10 s, is disconnected (Q19).
- **Ending.** When the unit's registered control connection ends without a replacement, under the same lock:
  - the unit goes offline and forgets its list, slot and endorsement;
  - its pending invitation ends;
  - its splice stops, with both legs closed at once.

### 7.3 Client connection

```
[which ──► endorsement | unavailable+end]     provisional path only (§9.2)
client ──►
connect ──►        ◄── ready | unavailable+end
(splice)
```

- `client` and `connect` must arrive within 10 s of the TCP accept (Q24). That window includes the provisional path's endorsement check (measured in §15, M4). The relay decides only once both are in:
  1. `sig` must verify over the client login message (§8.3), with this connection's nonce, the relay key hash, and `device_id`.
  2. **Q2:** after `which` matched unit U, `device_id` must be U.
  3. `device_id` must be an enrolled unit with a registered control connection.
  4. **Without `slot`:** the phone's fingerprint must be on the unit's admission list.
  5. **With `slot`:** the unit must have a pairing slot, equal to `slot` in constant time.
  6. **The place:** the unit's one place must be free, or held by this same phone (Q9).
- If all of that holds, the relay **replaces** the place's earlier holder if it is this phone (Q10), as a listed phone or as a candidate. It then issues an invitation: cookie, unit, control generation, the slot generation for `pair`, a monotonic deadline of now + 5 s, and the waiting client. It sends `invite {cookie, client, via}` on the unit's current control connection and waits for the `join`.
  - If the `join` comes, both ends get `ready` and the splice begins.
  - Otherwise the phone gets `unavailable`: a `decline`, the deadline, the unit going offline, a replacement, or a slot change.
- **Every refusal is the same** frame, byte for byte: the length `00 00 00 16`, then the 22 bytes `{"type":"unavailable"}`. That holds whatever the reason: a bad signature, an unknown unit, an offline unit, a phone not listed, a wrong slot, a place taken, a decline, a timeout (threat model §5.5).
- **What differs (Q23):** the timing and the outcome, not the bytes. An invitation refused late (decline, deadline) answers later than an immediate refusal, and a successful `which` answers with an endorsement, which tells a prober that holds the relay's key and a `device_id` that the unit is online (the amendment's stated residual).
- `which` is answered with the unit's `endorsement` frame if the lookup matches an enrolled unit that is registered. Otherwise it is answered `unavailable`.

### 7.4 Data connection

`join {cookie}` is decided in **one critical section** (finding 1). It is accepted only if all of these hold:
- the cookie exists and is unused;
- the relay's monotonic clock is before the invitation's deadline;
- the invitation's control generation is the unit's current one;
- for `pair`, its slot generation is the current one;
- its phone still waits.

The cookie is consumed in that section, whatever the answer. Expiry never depends on which goroutine's timer fires first. The relay then answers `ready` and the splice begins, or answers `unavailable` and ends the connection. A refused `join` for a still-pending invitation also ends that invitation, and its phone gets `unavailable`.
- A cookie is 128 random bits and works once.
- A data connection carries no login.

### 7.5 The ready transition and the splice

| State | Meaning |
| --- | --- |
| `invited` | the invitation is sent |
| `joined` | the cookie is consumed; the data leg is here |
| `ready-sent` | `ready` written to the data leg, then the client leg |
| `forwarding` | entered only under the lock, after checking the splice has not been stopped; only then do the copy goroutines start |

- **Stopping at any state** (replacement, removal from the list, the unit going offline, a slot change for an invitation, idleness, either side closing) closes both sockets at once. It is never only a flag that a goroutine sees later. A stopped invitation answers its phone `unavailable`.
- **Q4: `ready` is a byte-stream boundary on each leg.** Bytes the relay has read ahead from a leg past `ready`, or past `connect` and `join`, are forwarded first, in order, and never lost.
- **Forwarding:** each connection carries raw bytes, which the relay copies to the other without looking at them. The phone, the TLS client, speaks first.
- **Inside:** ADR-019's TLS 1.3 session, unchanged, with the phone as client and gatewayd as server.
- **Idle:** 90 s in which no byte moved in either direction ends the splice (configurable).

### 7.6 One place, and replacement (rule 37)

- **One place per unit (Q9).** It holds one pending invitation or one splice, of a listed phone or of a candidate. While it is taken, a `connect` from any other phone gets `unavailable`. So relay pairing is busy while a listed phone's splice runs, and the other way round. This is v1's accepted trade-off, matching the helper's one data connection and gatewayd's one relay socket (`RELAY_PROPOSAL.md` §8.2, §8.3).
- **Replacement (Q10).** A `connect` from the phone that holds the place, as a listed phone or a candidate, replaces its own invitation or splice when the new `connect` is admitted. If the new `join` then fails, the phone has no relay session; that is the accepted availability trade-off. A refused `connect` leaves the old one running. gatewayd replaces a session only after the inner handshake has authenticated the phone and its eligibility on the actual ingress path is checked (§9.1).

## 8. Signed and hashed messages

Every message starts with its context string and a 0x00 byte. No two strings are equal, none contains 0x00, and none starts with 0x20 (the TLS 1.3 CertificateVerify prefix) or 0x30 (an X.509 TBSCertificate). So no byte string is a message of two kinds (`RELAY_PROPOSAL.md` §4.3).

| § | Message | Signed by | Bytes |
| --- | --- | --- | --- |
| 8.1 | Unit login | device key | `"DOORS relay unit login v1"` 0x00 \|\| nonce (32) \|\| relay key hash (32) |
| 8.2 | Endorsement `e` | device key | `"DOORS relay endorsement v1"` 0x00 \|\| u64 config_id \|\| u64 seq \|\| u8 len \|\| name (len bytes) \|\| u16 port \|\| relay key hash (32) |
| 8.3 | Client login | phone key | `"DOORS relay client login v1"` 0x00 \|\| nonce (32) \|\| relay key hash (32) \|\| device_id (32) |
| 8.4 | Key proof | the relay's presented key | `"DOORS relay key proof v1"` 0x00 \|\| the unit's nonce from `prove` (32) |
| 8.5 | Backup | the relay's active key | `"DOORS relay backup v1"` 0x00 \|\| backup key hash (32) \|\| the unit's nonce from `get_backup` (32) |
| 8.6 | Lookup | (hash) | `SHA-256("DOORS relay lookup v1"` 0x00 \|\| presented relay key hash (32) \|\| device_id (32)`)` |
| 8.7 | Pair slot | (KDF) | `HKDF-SHA256(IKM = the window's 16 raw token bytes, salt = none, info = "DOORS relay pair slot v1", L = 16)` |

- **8.1 to 8.3:** the nonce is the relay's `challenge` on that connection, and the relay key hash is the fingerprint of the leaf key the signer was shown on it.
- **8.2 (Q14):**
  - **The name** is a canonical dialable DNS name: 1 to 64 characters in all; labels of 1 to 63 characters of `a-z`, `0-9` and `-`, with no `-` at either end of a label; no empty label, so no `.` at either end.
  - **The port** is 1 to 65535, and `e` is 78 + len bytes.
  - **`seq`** is 1 to 2^63−1; 0 is refused. It starts at 1 and grows by one per endorsement.
  - **The configuration id** is random per set-up at the unit (`RELAY_PROPOSAL.md` §4.2).

  The relay refuses at `register` an endorsement that breaks any of this.
- **8.6:** the phone computes the lookup over the key it was shown. The genuine relay computes it, for each enrolled unit, over its own key. A relay that does not hold the key the phone saw therefore cannot match a `device_id` it has not been told.
- **8.7 (Q11):** "no salt" is RFC 5869's default, HashLen zero bytes. `info` is the exact ASCII string, without a 0x00. The relay never learns the token.

**Test vectors**: inputs nonce = `00 01 … 1f`, relay key hash = `20 … 3f`, device_id = `40 … 5f`, backup key hash = `60 … 7f`, token = `80 … 8f`. They are checked by `relay/internal/wire` (`TestVectors`), and the lookup and the slot were checked once against Python's `hashlib` and `hmac`.
- lookup = `49c1cfcfa13f22964f87c99ea33aceaa7805ccd95cd928e8995a0edd5fabe5d4`
- slot = `33ec232b74cf3cc0693b1d9205f5829d`
- endorsement with config_id `0102030405060708`, seq 3, name `relay.example.org`, port 443: hex of the context string, `00`, `0102030405060708`, `0000000000000003`, `11`, hex of the name, `01bb`, then `20 … 3f`.
- `ping {n: 7}` as a frame: `00000015` followed by `{"n":7,"type":"ping"}`. Members may come in any order; the relay writes them sorted.
- Frame payload vectors (accepted and refused): `relay/internal/wire/testdata/vectors.json` (§5).

## 9. What the ends check

The relay cannot enforce these checks; phases 2 and 3 must implement them. `relay/internal/fake` implements them in Go for the tests.

### 9.1 The unit (gatewayd and its helper)

- **Accepted relay:** only a leaf key that is A or B (§3), over TLS 1.3, with SNI. A unit login is signed only for a pinned key (rule 31).
- **Matching (Q3):** gatewayd matches each `key_proof` and `backup` to its outstanding operation, its fresh nonce, the expected pin, and its set-up or promotion generation. Anything else is discarded.
- **Set-up (rule 28; Q7):**
  1. A probe connection shows the presented key's sixteen digits to the person. The person's set-up dial is allowed by rule 29.
  2. After confirmation, a **fresh connection, pinned to the full SPKI hash from the probe**, logs in.
  3. It sends `get_backup` with a fresh nonce.
  4. B is stored only if `SHA-256(spki_a)` is the confirmed key, `sig` verifies over §8.5 with the unit's nonce, and `spki_hash_b` is not A.
  5. A new random configuration id is drawn, and `seq` is set to 1.
- **Promotion:**
  1. When the relay presents B, the unit logs in (B is a pin) and sends `prove` with a fresh nonce.
  2. Only if `SHA-256(spki)` is B and `sig` verifies over §8.4 does it write `A := B; B := none; seq := seq + 1` in one durable `relay.v1` write, at most once per configuration.
  3. It then registers with an endorsement of the new A, and closes every connection presenting the old A.
- **After `registered`:** the full admission snapshot first, then a `pair_slot` if a window allows the relay. Acknowledgements are matched to the control generation and the outstanding `n`.
- **Pairing commit and recovery:** `RELAY_PROPOSAL.md` §6.1, the two-file rule for `paired.v1` and `relay.v1`.
  - A relay pairing approved with "Through the relay" is answered with `relay: "admitted"` or `relay: "pending"`. `pending` is an outcome, not an error: the pairing stands.
  - `gateway.hello` reports `relay: "admitted" | "pending" | "off"`.
- **Gateway rules:** the gateway's own rules are unchanged inside the splice.
  - A relay session of a paired phone without `remote` is closed after the handshake, whatever the relay admitted (rule 32).
  - A relay attempt never displaces that phone's working LAN session before the inner handshake has authenticated it and its grant is checked (Q10).

### 9.2 The phone (RelayTransport)

- **Pinned path:** if the leaf key's fingerprint is the stored relay pin, the phone sends `client`, then `connect`.
- **Provisional path:** if there is no pin (a first pairing) or the pin fails (a rotation not yet seen), the phone sends `which {lookup}` (§8.6) and checks the `endorsement`:
  - **Q1:** `SHA-256(spki)` is the unit's pinned fingerprint, or the pairing code's `fp`, before anything is verified;
  - `sig` verifies over `e` under `spki`;
  - `e` parses as §8.2;
  - the name and port are those dialled;
  - the key hash is the presented leaf's;
  - the configuration id is the stored one (any, at a first pairing);
  - `seq` is at least the stored one, and **strictly higher** if `e` names another key than the stored pin (Q14).
- **Only then** does it store the pin, the id and the sequence, and send `client` for **the same unit** (Q2). At a first pairing, that state stays provisional until the pairing succeeds.
- **Before acceptance** it has sent the HTTP start and `which`, and nothing else: no login, no slot (threat model §5.3; Q24).
- **A new configuration id** reaches the phone only in `gateway.hello` or a new pairing code, never on the provisional path. After total key loss or a new set-up, recovery is an authenticated `gateway.hello` on the LAN, or a physical re-pairing (`RELAY_PROPOSAL.md` §4.4).

## 10. Limits and timeouts

| What | Value | Where it is set |
| --- | --- | --- |
| HTTP head | at most 2048 bytes | protocol |
| Frame | 1 to 4096 bytes of JSON | protocol |
| Admitted phones per unit | 8 | protocol |
| Per unit | 1 control connection, 1 pairing slot, **1 relay place** (Q9) | protocol, v1 |
| TCP accept to the login (control: to `register`; client: to `connect`) | 10 s | protocol (Q7, Q24) |
| Invitation to `join` (the cookie's life, by the monotonic clock) | 5 s | protocol |
| Pings on control | every 25 s; 75 s without a frame closes | protocol (Q6) |
| Frames from a unit on control | 10 per second (burst 10); more closes | relay (Q19) |
| Frames queued to a unit | 32; a full queue disconnects it | relay (Q19) |
| One frame write | 10 s | relay |
| New connections per source | 10 per second (configurable) | relay; a source is an IPv4 address or an IPv6 /64 (Q16) |
| Unauthenticated connections per source | 64 (configurable); a connection counts until its login (control: until `register`) is accepted | relay |
| Connections in all | 1024 (configurable) | relay (Q17) |
| Tracked sources | 4096 (`max_sources`, configurable); a new source is refused at accept while the table is full and none of the at most 8 entries examined can be removed | relay (Q17) |
| Log queue | 256 lines; refusal lines at most 5 per second in all | relay |
| Splice idle | 90 s (configurable) | relay |

- **Refusal at accept:** connections refused by a limit are closed at accept, before TLS.
- **Source entries:** an entry can be removed once it has no live connection and its rate window has passed. Removal work per accept is bounded (at most 8 entries), and a sweep every 10 s goes over the capped table.
- **Byte rate (Q21):** the relay limits no byte rate, for an owner-operated v1, on the strength of §15's slow-reader and load runs. The helper limits what it reads (`RELAY_PROPOSAL.md` §8.3), and gatewayd's own limits remain phase 2's.

## 11. Errors

| Situation | The relay answers |
| --- | --- |
| Anything but the Upgrade request (§4) | `404`, then the end |
| A refused login, a refused `connect` (any reason), an unknown or offline `which`, an unusable cookie, `get_backup` without a backup | `unavailable`, then the end |
| A malformed frame, a frame out of order or of the wrong kind for the connection, a frame above 4096 bytes, too many frames, a non-increasing `n`, a bad endorsement at `register`, a timeout | the end, without an answer (Q23) |

There is no other error and no error code.

## 12. Logs (rule 41)

- **What the relay logs:**
  - connection lifecycle (`control up`/`down`, `session up`/`down`, `refused` with a fixed reason);
  - fingerprint prefixes (sixteen lower-case hex digits);
  - source addresses truncated to /24 (IPv4) or /48 (IPv6);
  - byte counts and durations;
  - its own start, with its key's short fingerprint.
- **Never on a connection's path:** lines go through a queue of 256 to one writer goroutine. A full queue drops the line and counts it, and refusal lines are limited to 5 a second in all, the rest counted.
- **A summary a minute** counts what is not worth a line each:
  - refusals at accept, by reason, and the source table's size;
  - failed handshakes, `404`s, and connections that ended before a login;
  - unlogged refusals and dropped lines.
- **Never:** a payload, a full fingerprint, a full address, a nonce, a cookie, a slot, a signature or an endorsement.
- **Enforcement:**
  - every log call of the server is in one file, which a test holds;
  - the log line's fields have fixed types;
  - Go's `net/http` error log, which would write full addresses, is not used.

## 13. The relay program

`relay/` is one Go module, standard library only: `doors-relay serve | keygen | backup-keygen | enroll`. Build, configuration and tools: `relay/README.md`.
- **Memory only:** admission lists, endorsements and pairing slots. A restart forgets them, and units register again.
- **Enrolment (Q26)** edits the configuration file and takes effect when the relay restarts. A restart disconnects every unit and phone. Units reconnect with their back-off and register again, and sessions start again from the phones.
- **One writer:** there is one operator writer at a time. `enroll` refuses to write if the file changed since it read it (a lost-update check), and it can simply be run again.

## 14. The decisions Q1 to Q27, resolved

The owner accepted every disposition of the Codex review (2026-11-27), with Q9, Q22 and Q27 settled as revised. This table is the resolution's §10, as this specification and `relay/` carry it.

| Q | Resolution | Here |
| --- | --- | --- |
| 1 | The phone hashes the canonical SPKI against its stored unit pin, or the QR `fp`, before it verifies anything. First-pairing state is provisional until pairing succeeds | §6, §9.2 |
| 2 | After `which` matched unit U, `client.device_id` must be U, else `unavailable` (relay-enforced) | §7.3; test T11 |
| 3 | Kept, once each per pre-register connection. gatewayd matches each answer to its operation, nonce, expected pin and generation | §6, §9.1 |
| 4 | `ready` is a byte-stream boundary on each leg; read-ahead bytes are preserved. Stopping before `forwarding` aborts both legs | §7.4, §7.5; tests T4, T12 |
| 5 | `via` is a hint without authority | §6 |
| 6 | 25 s and 75 s. Liveness is any valid frame. Each end answers a `ping` with a `pong` of the same `n`; a `pong` is telemetry, not matched | §7.2 |
| 7 | A fresh connection pinned to the full SPKI hash from the probe; rules 28 and 29 amended; the 10 s covers the backup or proof check and the durable promotion write | §7.2, §9.1 |
| 8 | Connection-owned state; control changes are atomic for pending invitations and acknowledgements; survival rules for running splices | §7.2; tests T8, T10 |
| 9 | **One relay place per unit** in v1 | §1, §7.6, §10; test T15 |
| 10 | Replacement at an admitted outer `connect`, an explicit availability trade-off; gatewayd replaces only after inner authentication and authorization | §7.6 |
| 11 | Raw 16-byte token, zero salt, `info` without a 0x00, 16 bytes; signed messages are context, then 0x00; the vector kept | §2, §8.7 |
| 12 | `n` unsigned decimal, at most 4294967295; published accept and reject vectors | §5; `testdata/vectors.json`; test T16 |
| 13 | Raw-line rules; the sender's wait-until-101 rule stated apart; the relay detects only bytes already read ahead | §4 |
| 14 | Canonical dialable DNS names; `seq` 1 to 2^63−1; a different key in one configuration needs a strictly higher `seq` | §8.2, §9.2; test T17 |
| 15 | Missing ALPN, or a missing or wrong SNI, gets `404` after TLS; an incompatible offered ALPN fails the handshake | §3 |
| 16 | IPv4 /32 and IPv6 /64, validated by the shared-NAT run | §10; §15 M2 |
| 17 | A configurable global cap; tracked sources capped, with bounded removal work per accept | §10; test T13 |
| 18 | Input validation only; the phone checks again | §7.2 |
| 19 | Defaults kept; queue saturation, slow writes, teardown during writes and generation-safe acknowledgements tested | §7.2, §10; tests T5, T6; §15 M3 |
| 20 | Own TLS socket, bounded head reader, raw-line validation | §4 |
| 21 | No relay byte rate for an owner-operated v1, on the slow-reader and load runs | §10; §15 |
| 22 | **`registered`**, after the replacement's effects | §6, §7.2; test T8 |
| 23 | One refusal frame; invitation failures and successful lookups differ in timing and result | §7.3, §11 |
| 24 | 10 s from accept through `connect`, measured over a slow link; no login or slot before the endorsement is verified | §7.3, §9.2; §15 M4 |
| 25 | P-256 in the 91-byte SPKI form; the outer leaf-only pin and the inner one-certificate pin are distinct | §2, §3 |
| 26 | Restart applies; the disconnect window documented; `enroll` refuses a lost update; one operator writer | §13; test T18 |
| 27 | **A slot change or clear ends pending `pair` invitations**; a running candidate splice is gatewayd's to end | §7.2; test T3 |

## 15. Tested, measured, and not validated

### 15.1 Tests

- **The review's T1 to T18** are in `relay/internal/server/review_test.go`, plus T14 (`TestEnroll` under any umask) and T16 (`wire.TestFrameVectors`).
  - **Deterministic:** an injected monotonic clock decides invitation deadlines. Named race points (`register-committed`, `register-effects-done`, `admit-committed`, `joined`, `ready-sent`) let a test hold the relay between two steps. No test sleeps in the hope of a schedule.
- **Mutation check:** each fix was undone by hand, one at a time, and the suite run.
  - 17 of 19 such mutations fail a test.
  - The other two are equivalent: removing the wait for the copy goroutines after both raw sockets are closed, and removing a `decline`'s generation check while its control-identity check stays.
- **The full suite:** `go vet ./...` and `go test -race -count=1 ./...`, under umask 022 and 077 (`docs/BUILD_ENVIRONMENT.md`, "DOORS Relay").

### 15.2 Measured (2026-11-27)

- **Machine:** AMD Ryzen 7 9700X (8 cores, 16 threads), 15.9 GB visible to WSL2, kernel 6.18.58.1-microsoft-standard-WSL2, Go 1.27.4 linux/amd64.
- **Method:** the relay in-process, over loopback, with the default limits and timings unless said. M1's load ran as a separate process.
- **Code:** `relay/internal/server/measure_test.go`, run with `RELAY_MEASURE=1`.

| | Run | Result |
| --- | --- | --- |
| M1 | **Connection churn**: 60 s at 2000 connections/s from random 127/8 sources (120 000 connections, 35 274 with a TLS handshake); a unit's control (dial to `registered`) and a phone's session (dial to inner TLS) every 250 ms meanwhile | **Resources:** RSS 13.4 MB → peak 21.8 MB; Go heap 1.8 → peak 6.1 MB; 0.63 cores; the source table at its cap of 4096, with no source refused (churned entries were removable); live connections peak 20. **Latency during the churn** (235 each, none failed): unit p50 2.0 ms, p99 5.7 ms; phone p50 3.3 ms, p99 5.0 ms, max 58 ms |
| M2 | **Shared NAT**: 40 phones behind one source, each to its own unit, all at once, with the app's back-off (1 s doubling, jitter) | 10 in at once (the burst of 10); 30 refused at accept by the rate limit and in after retries; 48 refused attempts in all, none never in. Time to session p50 1.0 s, p90 3.4 s, max 3.6 s |
| M3a | **Slow reader, splice**: the phone of one splice stops reading while its unit sends | 7.9 MB went into socket buffers before the unit's writes blocked. Relay Go heap 2.9 → 2.4 MB and RSS 13.9 → 14.8 MB, 5 s after it blocked. Another splice: 733 → 813 MB/s, echo RTT p50/p99 23/68 µs → 25/56 µs: unaffected |
| M3b | **Slow reader, control**: a unit's control stops reading (receive buffer 4 KiB) while a listed phone reconnects to get invitations | **Paced** (about 150/s): the relay wrote 673 KB into the socket buffers, no write timed out and the queue never filled; it closed the control after the silence limit (30 s in this run's test timing; 75 s by default). **Burst** (32 connects at once): the 32-frame queue filled while the writer was behind, and the control was closed after 377 ms and about 1063 invitations ("write queue full") |
| M4 | **Slow link (Q24)**: the provisional path through a delaying proxy | Relay accept to `connect`, against the 10 s deadline: 1.06 s at 300 ms RTT; 3.51 s at 1 s RTT; 8.76 s at 2.5 s RTT. All 15 sessions came up. About 3.5 RTT, so the deadline is passed above about 2.85 s RTT |

**What stays ASSUMED:**
- the proposal's "a 1-vCPU host with 64 to 128 MB serves tens of units". M1 and M3 show one relay process at about 14 to 22 MB with three units on a fast machine; no small host or many-unit run was made;
- behaviour on a real network, a real NAT and a TCP pass-through.

### 15.3 Open items found by the measurements

- **M3b, burst:**
  - **What it shows:** a phone that holds a unit's place, or a candidate with the slot, can close that unit's control connection, by reconnecting fast enough to fill the 32-frame queue. Each reconnect needs a full TLS handshake and a valid login.
  - **What it costs:** remote access for that unit until the helper reconnects, a denial the threat model already grants such a phone (P3) and a hostile relay (R1).
  - **What it needs:** an owner decision on whether the relay should also limit invitations per unit, for example the helper's 1 a second. It is not changed here: Q19's defaults were accepted.
- **M4:** above about 2.85 s RTT, the provisional path cannot reach `connect` within 10 s. The 10 s stays as Q24 accepted it.

### 15.4 Not validated

- **No real ends:** no gatewayd, helper or app speaks this yet. Every test runs the Go relay against Go stand-ins (`relay/internal/fake`) and the lying relay (`relay/internal/liar`) on 127.0.0.1.
- **No real network:** no relay host and no VPS. Restrictive networks were simulated with a local `CONNECT` proxy, a local TLS interceptor and a local delaying proxy only.
- **One Go version:** built and tested with Go 1.27.4 only. `go.mod` says `go 1.24.0`, the first release with `crypto/hkdf`, and `go vet`'s stdversion check passed. Go 1.24.0 is not installed here, and was not installed for this.
- **The fake unit's two-file rule** (T9) is the contract's reference behaviour in Go, not gatewayd's C, and it has no production sandbox.
- **Threat model items left to later phases:** 3 (beyond the Go phone), 7 (beyond the relay), 8, 9, 11, 12's C fuzzers, and 13.
