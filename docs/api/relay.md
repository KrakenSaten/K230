# DOORS Relay protocol v1 (`doors-relay/1`)

Status: **draft. Phase 1 is built: the Go relay in `relay/` is host-tested only, against Go stand-ins of the unit and the phone.** Nothing is deployed and no relay host exists. The unit's side (gatewayd and its helper) and the app's side are phases 2 and 3, and neither exists yet.

This is the wire protocol of ADR-019 amendment 3 (rules 28 to 42, accepted 2026-11-27), as specified by `docs/design/companion/RELAY_PROPOSAL.md` §4, §5, §8.3 and §13. Where the proposal is silent or ambiguous, this document makes the smallest decision consistent with the amendment and marks it **[Q*n*]**. The decisions are listed in §14 as open questions for the owner.

Threats and required checks: `docs/design/companion/RELAY_THREAT_MODEL.md`. The gateway protocol, which runs unchanged inside a splice: `docs/api/gateway.md`.

## 1. Overview

The relay is a blind pipe. It connects a phone's TCP stream to a unit's, after both have proved their keys, and only for phones the unit has listed. ADR-019's TLS 1.3 session runs end to end inside the splice. The relay holds no key that could read it or stand in for either end (rule 30).

There are three kinds of connection. Each is opened by an end, never by the relay, over TCP to the relay's public port (443):

| Connection | Opened by | Purpose | Authenticated by |
| --- | --- | --- | --- |
| control | the unit's helper, while gatewayd asks for it (rule 29) | the unit's login, its admission list, pairing slot and endorsement; invitations | a unit login signed by the device key over the relay's nonce |
| client | the phone | asking for a session with one unit | a client login signed by the phone's key over the relay's nonce |
| data | the unit's helper, one per invitation | the unit's end of one session | a one-time cookie from an invitation |

Nothing in the protocol names a port, a host or a service (rule 35).

## 2. Conventions

- **Keys** are ECDSA P-256. On the wire a public key is its DER SubjectPublicKeyInfo with an uncompressed point: exactly 91 bytes, in exactly the encoding `x509.MarshalPKIXPublicKey` produces. A non-canonical encoding of the same key is refused, because its fingerprint would differ.
- **A fingerprint** is the SHA-256 of a DER SubjectPublicKeyInfo (PAIRING.md §1). A unit's fingerprint is its `device_id`. "Relay key hash" means the fingerprint of the key the relay presented on the connection.
- **For people**, a fingerprint is shown as its first sixteen hex digits, in upper case and in groups of four (`3F2A 91C4 0B7E D115`). **In logs** it is the same sixteen digits in lower case, without spaces.
- **Signatures** are ECDSA P-256 over SHA-256 of the message, DER-encoded (at most 72 bytes). This is what OpenSSL's `ECDSA_sign` and Android's `SHA256withECDSA` produce. Low-S is not required.
- **Byte strings** are concatenated with `||`. Integers in signed messages are big-endian. A context string is ASCII, followed by one 0x00 byte.
- **In JSON**, SHA-256 values (fingerprints, `device_id`, key hashes, the lookup) are 64 lower-case hex digits. Every other binary value (nonces, keys, signatures, cookies, slots, endorsements) is base64 per RFC 4648 §4: standard alphabet, padded, with no line breaks and no non-zero padding bits. Counters (`n`) are whole numbers from 0 to 2^32−1 **[Q12]**.

## 3. TLS (rules 31, 39)

- **Version:** TLS 1.3 only. The relay refuses TLS 1.2 and older.
- **SNI:** the relay's DNS name, as configured. A missing or different SNI completes the handshake and is then answered `404` (§4).
- **ALPN:** the ends offer `http/1.1` and nothing else. A connection without ALPN gets `404`. A client offering only other protocols fails the handshake.
- **The relay's certificate:** one self-signed certificate for its key, or a CA-issued chain whose leaf carries the same key (rule 39, for middleboxes that check certificates).
  - The self-signed certificate has fixed, informational fields, as the gateway's do (PAIRING.md §2): `notBefore` 1970-01-01T00:00:00Z, `notAfter` 9999-12-31T23:59:59Z, `CN=doors-relay`, a random serial **[Q25]**.
- **No client certificate** on the outer TLS. Each end proves its key with a signed login inside it (§7).
- **No session resumption:** the relay issues no tickets, so every connection presents the leaf.
- **Pinning, at both ends** (not the relay's check):
  - The decision is the **leaf certificate's key alone**: it must be ECDSA P-256, and its fingerprint must be a pin.
  - The rest of a chain is ignored, and so are dates, names, issuers and extensions.
  - The unit accepts only its pins A and B (§9.1). The phone accepts its pin, or takes the provisional path (§9.2).

## 4. The HTTP start

After the handshake the connecting end sends exactly this request (rule 39):

```
GET /doors-relay/1 HTTP/1.1\r\n
Host: <relay name>[:<port>]\r\n
Connection: Upgrade\r\n
Upgrade: doors-relay/1\r\n
\r\n
```

- **Port in Host:** ends write `:<port>` only when the port is not 443. The relay accepts the name with or without its configured public port **[Q13]**.
- **What the relay accepts**:
  - The request line is exactly `GET /doors-relay/1 HTTP/1.1`, in origin form.
  - There are exactly the three headers `Host`, `Connection` and `Upgrade`, each once, in any order, and no other header and no body.
  - Header names, the name in `Host`, and the values `Upgrade` and `doors-relay/1` compare without regard to ASCII case.
  - Every line ends in CRLF.
  - The whole head (request line, header lines and the empty line) is **at most 2048 bytes**.
  - SNI names the relay, and ALPN is `http/1.1`.
- **Nothing more before the answer:** the connecting end writes nothing after the empty line until it has read the 101 and the challenge. Bytes that arrive before the relay answers are treated as anything else, below.
- **The answer to the Upgrade**, byte for byte:

  ```
  HTTP/1.1 101 Switching Protocols\r\n
  Connection: Upgrade\r\n
  Upgrade: doors-relay/1\r\n
  \r\n
  ```

  The relay's `challenge` frame (§6) follows immediately.
- **Anything else** gets the answer an ordinary Go server sends, without a `Date` header, and the connection ends. That covers a well-formed request for another path, method or host, a head that is malformed or longer than 2048 bytes, and a missing or wrong SNI or ALPN **[Q15]**:

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
- **Implementation:** the relay parses the head with Go's `net/http.ReadRequest` on a TLS connection it owns itself, not through `net/http.Server` and `Hijack` as the proposal's §13 suggests **[Q20]**.

## 5. Frames

- After the 101, both directions carry frames: a 4-byte big-endian length, then that many bytes of UTF-8 JSON encoding one object. This is the gateway's framing with a smaller limit.
- **The length is 1 to 4096.** A length of 0 or above 4096 ends the connection.
- **A frame is refused, and the connection ends without an answer, if** it is not valid UTF-8, it is not exactly one JSON object, a member appears twice, a member is missing or unknown for its type, or a value has the wrong type, encoding or length. This holds in both directions **[Q12]**. No version is negotiated inside the protocol: a new version is a new path, `/doors-relay/2`.
- `type` names the frame. The members of each type follow.

## 6. Frame types

| `type` | Members | Sent by | On | Meaning |
| --- | --- | --- | --- | --- |
| `challenge` | `nonce` (32 bytes) | relay | every connection, first | Fresh random bytes for this connection's login |
| `unit` | `spki`, `sig` | unit | control, first | Unit login (§8.1) |
| `prove` | `nonce` (32 bytes) | unit | control, after `unit` and before `register`, at most once | Asks the relay to prove the key it presented |
| `key_proof` | `spki`, `sig` | relay | control | The relay's key and its signature over the unit's nonce (§8.4) |
| `get_backup` | `nonce` (32 bytes) | unit | control, after `unit` and before `register`, at most once | Asks for the backup pin **[Q3]** |
| `backup` | `spki_a`, `spki_hash_b` (hex), `sig` | relay | control | The active key, the backup key's hash, and the active key's signature (§8.5) |
| `register` | `admit` (array of 0 to 8 distinct hex fingerprints), `endorsement` (object `{e, sig}`) | unit | control, once | Makes this the unit's control connection and replaces an earlier one (§7.2) |
| `admit` | `admit` (as above), `n` | unit | control, after `register` | Replaces the admission list |
| `admitted` | `n` | relay | control | The list of the `admit` with this `n` is in force |
| `pair_slot` | `slot` (16 bytes, or `null`) | unit | control, after `register` | Sets the pairing slot, or clears it with `null` |
| `invite` | `cookie` (16 bytes), `client` (hex), `via` (`"admit"` or `"pair"`) | relay | control | A phone wants a session. `client` and `via` are hints **[Q5]** |
| `decline` | `cookie` | unit | control | Refuses that invitation |
| `ping` | `n` | relay or unit | control | Asks for a `pong` with the same `n` |
| `pong` | `n` | relay or unit | control | Answers a `ping` |
| `which` | `lookup` (hex) | phone | client, first, provisional path only | Asks for a unit's endorsement without naming the unit (§8.6) |
| `endorsement` | `spki`, `e`, `sig` | relay | client | The unit's key, its endorsement bytes, and the device key's signature **[Q1]** |
| `client` | `spki`, `device_id` (hex), `sig` | phone | client: first, or after `endorsement` | Client login, naming the unit **[Q2]** |
| `connect` | `slot` (16 bytes; optional, never `null`) | phone | client, after `client` | Asks for the session. A candidate presents the pairing slot |
| `ready` | none | relay | client; data **[Q4]** | The splice begins with the next byte |
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
[prove ──► key_proof]                    at most once, before register
[get_backup ──► backup | unavailable+end] at most once, before register
register ──►                             within 10 s of the TCP accept
   then, in any order:  admit ──► admitted     pair_slot ──►     decline ──►
                        ◄── invite             ping ◄──► pong
```

- **Login.** The relay accepts `unit` only if all of these hold, and otherwise answers `unavailable` and ends the connection:
  - `spki` is a valid P-256 key;
  - its fingerprint is an enrolled unit;
  - `sig` verifies over the unit login message (§8.1), with this connection's nonce and the relay key hash of the key presented on it.
- **Register.** The relay checks the endorsement first. Then the connection becomes the unit's one control connection, carrying the admission list, the endorsement for phones, and no pairing slot. If the unit had an earlier control connection, that one is closed (it is replaced):
  - invitations sent over the earlier connection end, and their phones get `unavailable`;
  - a running session survives if its phone is on the new list, and a candidate's session survives too **[Q8]**.

  The relay accepts the endorsement only if `e` parses as §8.2, names the relay's configured name and public port and the key it presented, and `sig` verifies under the unit's key. Any other endorsement ends the connection, and the unit stays offline **[Q18]**. `register` has no answer **[Q22]**.
- **Admission list.** On `admit`, the relay replaces the list. It then ends the session of a phone no longer on it, and only after that sends `admitted {n}` with the frame's `n`. A phone taken off the list loses its splice within a second (threat model §5.5).
- **Pairing slot.** `pair_slot` sets or clears the unit's one slot. Clearing it refuses new candidates, and a running candidate's session is left alone **[Q27]**.
- **Pings.** The relay sends `ping` every 25 s and answers every `ping` with a `pong` of the same `n`. The unit is expected to do the same. Either end closes after 75 s in which it received no frame **[Q6]**.
- **Prove and backup** are answered only between `unit` and `register`, once each per connection. If no backup key is configured, `get_backup` is answered `unavailable` and the connection ends.
- **Ending.** When the unit's registered control connection ends and is not replaced, the relay forgets the unit's list, slot and endorsement, and ends its sessions and invitations. The unit is offline until it registers again **[Q8]**.

### 7.3 Client connection

```
[which ──► endorsement | unavailable+end]     provisional path only (§9.2)
client ──►
connect ──►        ◄── ready | unavailable+end
(splice)
```

- `client` and `connect` must arrive within 10 s of the TCP accept **[Q24]**. The relay decides only once both are in:
  1. `sig` must verify over the client login message (§8.3), with this connection's nonce, the relay key hash, and `device_id`.
  2. `device_id` must be an enrolled unit with a registered control connection.
  3. **Without `slot`:** the phone's fingerprint must be on the unit's admission list, and the unit's one session must be free or already this phone's.
  4. **With `slot`:** the unit must have a pairing slot, equal to `slot` in constant time, and its one candidate place must be free or already this phone's.
- If all of that holds, the relay **replaces** this phone's earlier session or candidacy at this unit, if any, and sends `invite {cookie, client, via}` on the unit's control connection. It then waits up to 5 s for the `join`. If the `join` comes, both ends get `ready` and the splice begins. Otherwise (a `decline`, no `join` in time, or the unit going offline) the phone gets `unavailable`.
- **Every refusal is the same** frame, byte for byte: the length `00 00 00 16`, then the 22 bytes `{"type":"unavailable"}`, whatever the reason: a bad signature, an unknown unit, an offline unit, a phone not listed, a wrong slot, a place taken, a decline, a timeout (threat model §5.5).
- `which` is answered with the unit's `endorsement` frame if the lookup matches an enrolled unit that is registered. Otherwise it is answered `unavailable`.

### 7.4 Data connection

`join {cookie}` must come within 5 s of the `invite`. The relay answers `ready`, and the splice begins, if the cookie is one it issued and has not been used, and its invitation is still waiting. Otherwise it answers `unavailable` and ends the connection.
- A cookie is 128 random bits and works once.
- It is bound to the unit and the control connection it was sent on: if that connection is replaced or ends, the invitation ends.
- A data connection carries no login.

### 7.5 The splice

- **Start:** after `ready`, each connection carries raw bytes, which the relay copies to the other without looking at them. The relay writes `ready` to the data connection before the client connection. The phone, the TLS client, speaks first.
- **End:** either side closing ends both. So does the splice being ended (replacement, removal from the list, the unit going offline), and so do **90 s in which no byte moved in either direction** (configurable).
- **Inside:** ADR-019's TLS 1.3 session, unchanged, with the phone as client and gatewayd as server.

### 7.6 One session per phone (rule 37)

- **Places:** per unit, the relay holds one session for a listed phone and, separately, one for a pairing candidate **[Q9]**. Pending invitations count.
- **Replacement:** a phone's new `connect` to a unit is counted **after** its own earlier session and candidacy at that unit are removed. When the new one is admitted, the old ones end. A refused `connect` leaves the old ones running **[Q10]**.
- **Another phone** never replaces: it gets `unavailable` until the place is free.

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
| 8.7 | Pair slot | (KDF) | `HKDF-SHA256(IKM = the window's 16 raw token bytes, salt = none, info = "DOORS relay pair slot v1", L = 16)` **[Q11]** |

- **8.1 to 8.3:** the nonce is the relay's `challenge` on that connection, and the relay key hash is the fingerprint of the leaf key the signer was shown on it.
- **8.2:** the name is 1 to 64 characters of `a-z`, `0-9`, `-` and `.`, as dialled. The port is 1 to 65535. `e` is 78 + len bytes. The configuration id is random per set-up at the unit, and `seq` starts at 1 and grows by one per endorsement (`RELAY_PROPOSAL.md` §4.2) **[Q14]**.
- **8.6:** the phone computes the lookup over the key it was shown. The genuine relay computes it, for each enrolled unit, over its own key. A relay that does not hold the key the phone saw therefore cannot match a `device_id` it has not been told.
- **8.7:** "no salt" is RFC 5869's default, HashLen zero bytes. The relay never learns the token.

**Test vectors**: inputs nonce = `00 01 … 1f`, relay key hash = `20 … 3f`, device_id = `40 … 5f`, backup key hash = `60 … 7f`, token = `80 … 8f`. They are checked by `relay/internal/wire` (`TestVectors`), and the lookup and the slot were checked once against Python's `hashlib` and `hmac`.
- lookup = `49c1cfcfa13f22964f87c99ea33aceaa7805ccd95cd928e8995a0edd5fabe5d4`
- slot = `33ec232b74cf3cc0693b1d9205f5829d`
- endorsement with config_id `0102030405060708`, seq 3, name `relay.example.org`, port 443: hex of the context string, `00`, `0102030405060708`, `0000000000000003`, `11`, hex of the name, `01bb`, then `20 … 3f`.
- `ping {n: 7}` as a frame: `00000015` followed by `{"n":7,"type":"ping"}`. Members may come in any order; the relay writes them sorted.

## 9. What the ends check

The relay cannot enforce these checks; phases 2 and 3 must implement them. `relay/internal/fake` implements them in Go for the tests.

### 9.1 The unit (gatewayd and its helper)

- **Accepted relay:** only a leaf key that is A or B (§3), over TLS 1.3, with SNI. A unit login is signed only for a pinned key (rule 31).
- **Set-up (rule 28):**
  1. A probe connection shows the presented key's sixteen digits to the person.
  2. Once the person confirms, a connection presenting that key logs in.
  3. It sends `get_backup` with a fresh nonce.
  4. B is stored only if `SHA-256(spki_a)` is the confirmed key, `sig` verifies over §8.5 with the unit's nonce, and `spki_hash_b` is not A.
  5. A new random configuration id is drawn, and `seq` is set to 1.

  The relay allows 10 s from accept to `register`, so the person's comparison happens between connections **[Q7]**.
- **Promotion:**
  1. When the relay presents B, the unit logs in (B is a pin) and sends `prove` with a fresh nonce.
  2. Only if `SHA-256(spki)` is B and `sig` verifies over §8.4 does it write `A := B; B := none; seq := seq + 1`, durably, at most once per configuration.
  3. It then registers with an endorsement of the new A.
- **Gateway rules:** the gateway's own rules are unchanged inside the splice. A relay session of a paired phone without `remote` is closed after the handshake, whatever the relay admitted (rule 32).

### 9.2 The phone (RelayTransport)

- **Pinned path:** if the leaf key's fingerprint is the stored relay pin, the phone sends `client`, then `connect`.
- **Provisional path:** if there is no pin (a first pairing) or the pin fails (a rotation not yet seen), the phone sends `which {lookup}` (§8.6) and checks the `endorsement`:
  - `SHA-256(spki)` is the unit's pinned fingerprint, or the pairing code's `fp`;
  - `sig` verifies over `e` under `spki`;
  - `e` parses as §8.2;
  - the name and port are those dialled;
  - the key hash is the presented leaf's;
  - the configuration id is the stored one (any, at a first pairing);
  - `seq` is at least the stored one.

  Only then does it store the pin, the id and the sequence, and send `client`. **Before that it has sent the HTTP start and `which`, and nothing else** (threat model §5.3).
- **A new configuration id** reaches the phone only in `gateway.hello` or a new pairing code, never on the provisional path.

## 10. Limits and timeouts

| What | Value | Where it is set |
| --- | --- | --- |
| HTTP head | at most 2048 bytes | protocol |
| Frame | 1 to 4096 bytes of JSON | protocol |
| Admitted phones per unit | 8 | protocol |
| Per unit | 1 control connection, 1 pairing slot, 1 session of a listed phone, 1 candidate session | protocol **[Q9]** |
| TCP accept to the login (control: to `register`; client: to `connect`) | 10 s | protocol **[Q7] [Q24]** |
| Invitation to `join` (the cookie's life) | 5 s | protocol |
| Pings on control | every 25 s; 75 s of silence closes | protocol **[Q6]** |
| Frames from a unit on control | 10 per second (burst 10); more closes | relay **[Q19]** |
| Frames queued to a unit | 32; a unit that does not read is disconnected | relay **[Q19]** |
| One frame write | 10 s | relay |
| New connections per source | 10 per second (configurable) | relay; a source is an IPv4 address or an IPv6 /64 **[Q16]** |
| Unauthenticated connections per source | 64 (configurable); a connection counts until its login (control: until `register`) is accepted | relay |
| Connections in all | 1024 (configurable) | relay **[Q17]** |
| Splice idle | 90 s (configurable) | relay |

Connections refused by a limit are closed at accept, before TLS. The relay limits no byte rate. The helper limits what it reads (`RELAY_PROPOSAL.md` §8.3) **[Q21]**.

## 11. Errors

| Situation | The relay answers |
| --- | --- |
| Anything but the Upgrade request (§4) | `404`, then the end |
| A refused login, a refused `connect` (any reason), an unknown or offline `which`, an unusable cookie, `get_backup` without a backup | `unavailable`, then the end |
| A malformed frame, a frame out of order or of the wrong kind for the connection, a frame above 4096 bytes, too many frames, a timeout | the end, without an answer **[Q23]** |

There is no other error and no error code.

## 12. Logs (rule 41)

- **What the relay logs:**
  - connection lifecycle (`control up`/`down`, `session up`/`down`, `refused` with a fixed reason);
  - fingerprint prefixes (sixteen lower-case hex digits);
  - source addresses truncated to /24 (IPv4) or /48 (IPv6);
  - byte counts and durations;
  - its own start, with its key's short fingerprint.
- **A summary a minute** counts what is not worth a line each: refusals at accept, failed handshakes, `404`s, and connections that ended before a login.
- **Never:** a payload, a full fingerprint, a full address, a nonce, a cookie, a slot, a signature or an endorsement.
- **Enforcement:**
  - every log call of the server is in one file, which a test holds;
  - the log line's fields have fixed types;
  - Go's `net/http` error log, which would write full addresses, is not used.

## 13. The relay program

`relay/` is one Go module, standard library only: `doors-relay serve | keygen | backup-keygen | enroll`. Build, configuration and tools: `relay/README.md`. Admission lists, endorsements and pairing slots live in memory only. A restart forgets them, and units register again.

## 14. Open questions for the owner

These are the decisions this document makes where `RELAY_PROPOSAL.md` is silent or ambiguous, the smallest consistent with ADR-019 amendment 3. Q1 to Q9 shape phases 2 and 3. The rest are recorded for review.

1. **[Q1] The unit's key in `endorsement`.** A phone at a first pairing knows only the code's `fp`, a hash, and cannot verify a signature without the key. So the relay's `endorsement` frame carries the unit's SubjectPublicKeyInfo as given in its login, and the phone checks that it hashes to `fp` before verifying. The proposal's `endorsement {E, sig}` lacks it.
2. **[Q2] `device_id` in `client`.** The client login signs the `device_id` (§4.3 of the proposal), but on the pinned path there is no `which` to name the unit. So `client` carries `device_id`. The relay is already trusted by then: pinned, or verified by endorsement.
3. **[Q3] The backup request.** The proposal names the answer `backup` but not the request. This document adds `get_backup {nonce}`. Both `get_backup` and `prove` are answered only between login and `register`, once each per connection.
4. **[Q4] `ready` on the data connection.** After a good `join` the relay sends `ready` there too (or `unavailable` for a bad cookie), so the helper knows where the splice begins. The proposal lists `ready` for the phone only.
5. **[Q5] `via`** is `"admit"` (listed) or `"pair"` (slot). It is a hint only (rule 30).
6. **[Q6] Pings.** "both | ping/pong | every 25 s" is read as: each end pings every 25 s and answers pings, and each closes after 75 s without a frame.
7. **[Q7] Set-up timing and replacement.** The relay requires `register` within 10 s of accept, and replaces an earlier control connection at `register`, as the proposal says, not at login. So the person compares the digits with the probe connection closed, and the backup is fetched over a new connection presenting the confirmed key. Rule 28 says "over that confirmed connection"; a connection presenting the confirmed key is read as such. The alternative is a longer pre-login window, which is more unauthenticated surface.
8. **[Q8] Unit state lives with its control connection.** When the control connection ends without a replacement, the unit is offline: its sessions and invitations end, and its list, slot and endorsement are forgotten. The relay never routes without the unit's current word. On a replacement, listed phones' and the candidate's sessions survive.
9. **[Q9] Two places per unit.** One session for a listed phone and one for a pairing candidate, counted separately. So a remote phone's session cannot block a relay pairing, nor the other way round. This matches gatewayd's separate relay candidate slot. "1 session" in §13 of the proposal is read as the listed phones' place.
10. **[Q10] Replacement** happens when the new `connect` is admitted, and covers both of the phone's places at that unit. A refused `connect` does not end a working session.
11. **[Q11] HKDF parameters:** IKM = the 16 raw token bytes (not their base64url), salt = none, info = the context string, L = 16.
12. **[Q12] Encodings and strictness:**
    - hex for SHA-256 values, padded standard base64 for everything else;
    - unknown and duplicate members refused in both directions;
    - `n` limited to 32 bits.
13. **[Q13] Host.** The name, or the name with its port. Ends omit `:443`. Exactly three headers, compared without ASCII case.
14. **[Q14] Endorsement bytes:** big-endian integers, and the name limited to `a-z 0-9 - .`. The proposal says "lowercase ASCII".
15. **[Q15] 404, not a TLS alert,** for a missing or wrong SNI or ALPN and for malformed heads. No `Date` header, since the relay needs no clock. Plain TCP gets nothing.
16. **[Q16] A source** is an IPv4 address or an IPv6 /64. Per IPv6 address would be trivially bypassed.
17. **[Q17] A global cap** of 1024 connections (configurable) is added. The proposal names per-source and per-unit limits only, and a small host needs a ceiling.
18. **[Q18] The relay checks endorsements at `register`** (name, port, key, signature). This is not a trust decision, since phones check again, but it stops a broken unit's endorsement from reaching phones.
19. **[Q19]** 10 frames a second from a unit on control, a 32-frame queue to it, and 10 s per write. These mirror the helper's limits in §8.3 of the proposal.
20. **[Q20] No `net/http.Server` + `Hijack`.** The relay owns the TLS connection and parses the head with `net/http.ReadRequest`. This gives an exact 2 KiB bound and one deadline from accept. It also avoids keep-alive, the built-in `OPTIONS *` handler, and an error log with full addresses. Still standard library.
21. **[Q21] No byte rate limit** at the relay. The helper limits its own reads, and the relay's bandwidth is the operator's.
22. **[Q22] `register` is not acknowledged.** A failure ends the connection, and frames are handled in order.
23. **[Q23] Violations end the connection silently.** Only refusals of a well-formed request get `unavailable`.
24. **[Q24] Phones must send `connect` within 10 s of accept.** That window includes the provisional path's endorsement check.
25. **[Q25] The self-signed relay certificate** copies PAIRING.md §2's fixed fields, with `CN=doors-relay`.
26. **[Q26] Enrolment** edits the configuration file. The relay reads it at start only (restart to apply), with no reload signal.
27. **[Q27] Clearing the pairing slot** refuses new candidates but leaves a running candidate session to gatewayd.

## 15. Not validated

- **Not run against the real ends:** no gatewayd, helper or app speaks this yet. Every test runs the Go relay against Go stand-ins (`relay/internal/fake`) and the lying relay (`relay/internal/liar`) on 127.0.0.1.
- **Not run on a network:** no relay host or VPS. Restrictive networks are simulated by a local `CONNECT` proxy and a local TLS interceptor only.
- **One Go version:** built and tested with Go 1.27.4 (linux/amd64, in WSL). `go.mod` says `go 1.24.0`, the first release with `crypto/hkdf`, and `go vet`'s stdversion check passed. Go 1.24 itself was not run.
- **No load or soak test:** the limits are tested one at a time, not under sustained load, and nothing has measured the "tens of units on 64 to 128 MB" estimate (proposal §12).
- **Not covered here:**
  - threat model §5.3 is covered as far as the Go phone goes;
  - §5.7 is covered at the relay only;
  - §5.8, §5.9, §5.11, the C fuzzers of §5.12, and §5.13 belong to phases 2 to 4.
