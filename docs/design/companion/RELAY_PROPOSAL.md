# Companion: a self-hosted DOORS Relay (proposal)

Status: **proposal** for ADR-019 amendment 3, which is Proposed and was
approved in principle on 2026-11-27. This revision answers the independent
review (§15). Nothing is implemented.

- ASSUMED marks estimates still to be measured.
- Claims about other projects are from their public documentation, to be
  checked against a pinned version before use.
- Rule numbers (28 to 42) are the amendment's.

## 1. Goals, non-goals, and what "minimal" means here

**Goals:**
- A paired phone reaches its unit where the local network does not let them
  meet.
- Local first.
- ADR-019's trust model unchanged end to end.
- Self-hosted.
- Outbound TCP 443 from both ends.
- Off by default.

**Non-goals:**
- A service run by Doors.
- Background push.
- Direct internet peer-to-peer.
- Anything on the unit but the gateway.

**Minimal:**
- No library added to the unit or the app.
- No QR decoder.
- One small unprivileged program on the unit (`gatewayd-relay`), started by
  the existing init script.
- One Go program off the unit, standard library only.
- No database, accounts, web UI, CDN, WebSocket framing, multiplexing, UDP or
  hole punching (§17).

## 2. The owner's decisions (2026-11-27)

1. The hybrid local-first, self-hosted relay architecture is approved in
   principle.
2. The relay is written in Go.
3. `remote` is off by default and controlled per phone.
4. First-time pairing through the relay is supported, with physical approval
   and fingerprint confirmation.
5. Phase 0 runs alongside phase 1.

## 3. Architecture

```
 phone (any network)              relay (owner's host :443, Go)          unit
 ───────────────────              ─────────────────────────────          ─────────────────────────────────────
                                  ┌───────────────────────────┐          gatewayd-relay  (S70gatewayd starts it
 app ─HTTPS-shaped TLS;        ──►│ endorsements (unit-signed)│◄── control under pos-supervise; own uid,
      relay pinned from the       │ admission lists           │    connection   no caps, seccomp, nice 10)
      unit, or endorsement-       │ pairing slots             │                   │ Unix socket, SO_PEERCRED;
      checked; nonce signed       │ splices two TCP streams   │◄── data           │ socketpair ends go
      by the phone's key          └───────────────────────────┘    connection     ▼ OUT from gatewayd only
                                                                              gatewayd (unchanged TLS server
 inside the splice:  phone ══════ ADR-019 TLS 1.3, mutual pins, unchanged ═══ on its end of the socketpair)
```

## 4. Which relay is genuine: keys, rotation, recovery (rule 31)

### 4.1 The unit's pins

- `relay.v1` in gatewayd's state directory (0600) holds:
  - the relay's name and port;
  - the active pin `A` and the backup pin `B`;
  - the endorsement sequence number;
  - the fingerprints of phones holding `remote`.

  It is written by temp-flush-rename.
- **Set up at the unit** (rule 28):
  1. The person types the name and port.
  2. The helper connects and reports the presented key.
  3. Settings shows its fingerprint as sixteen hex digits in groups of four,
     and the person compares and confirms.
  4. gatewayd sends a fresh nonce. The relay answers
     `backup {spki_a, spki_hash_b, sig}`, where `sig` is by the confirmed
     key `A` over `"DOORS relay backup v1" 0x00 || spki_hash_b || nonce`.
  5. gatewayd checks that `spki_a` hashes to the confirmed `A`, and that the
     signature verifies. Only then is `B` stored, and only during set-up.
  6. A random 64-bit **configuration id** is drawn, and `seq` is set to 1.
- **Why the backup is signed.** The helper connects before anyone has
  confirmed the relay. If it were compromised, it could otherwise show the
  genuine `A` and slip in an attacker's key as `B`.
- **The helper accepts a relay only if its leaf key is `A` or `B`.** It
  checks the leaf alone; a CA chain behind the leaf is ignored. gatewayd
  signs a unit login only for a key that is one of its pins.
- **Promotion to `B`, one-way:**
  1. When the helper reports `B`, gatewayd sends a fresh 32-byte nonce.
  2. The relay must return `key_proof {spki, sig}`: the signature, by the key
     it presented, over `"DOORS relay key proof v1" 0x00 || nonce`.
  3. gatewayd checks that the SPKI hashes to `B` and that the signature
     verifies.
  4. Only then does it write `A := B; B := none`, in one durable write, once
     per configuration.
- A compromised helper cannot fake that proof, and cannot burn the backup.
  Repeated reports cost nothing after the first, and nothing at all without a
  proof. With `B` promoted, the old active key is no longer accepted: a thief
  holding it gets nothing from the unit.

### 4.2 The phones' trust

- **Normal case.** In `gateway.hello` the unit tells each paired phone the
  relay `name:port`, the configuration id, the SHA-256 of the current key,
  and the current endorsement. The phone pins that key, checked on the
  **leaf certificate alone**. That is the same `PinnedTrustManager` class as
  for the gateway, in a leaf-only mode used only by `RelayTransport`: today's
  class demands exactly one certificate (`PinnedTrustManager.kt:32`), and a
  relay with a CA chain must not fall onto the provisional path every time.
- **Rotation not yet seen, or a first pairing.** The phone uses the
  provisional context: one leaf P-256 certificate, accepted provisionally
  (rule 13's one exception).
  1. It sends the HTTP start, then `which {lookup}`, where
     `lookup = SHA-256("DOORS relay lookup v1" 0x00 || presented_key_hash || device_id)`.
  2. It receives `endorsement {E, sig}` and checks it:
     - the signature verifies under the unit key it pins (or the code's `fp`);
     - the name and port are those dialled;
     - the key hash equals the presented leaf;
     - the configuration id equals the stored one (any, at a first pairing);
     - the sequence is at least the stored one.
  3. Then it pins the key, stores the id and the sequence, and continues.

  Nothing else is sent before that check. The lookup is bound to the
  presented key, so a fake relay cannot turn it into a `device_id`; the
  genuine relay computes it for its enrolled units.
- **The endorsement:**
  ```
  E = "DOORS relay endorsement v1" 0x00 || u64 config_id || u64 seq || u8 len || name (lowercase ASCII, at most 64) || u16 port || 32-byte SHA-256(relay SPKI)
  sig = ECDSA-P256-SHA256(device key, E)
  ```
- **Configuration id and sequence:**
  - Each configuration made at the unit has a random 64-bit id. Its `seq`
    starts at 1 and grows by one with each endorsement.
  - If `relay.v1` is lost while the device key exists, that is a fault, and
    remote access stays off. A person's new set-up draws a **new** id.
  - On the provisional path, a phone accepts only its stored id, and within
    it only a sequence at least the stored one.
  - A new id reaches a phone only in `gateway.hello`, over an authenticated
    gateway session (LAN, or a still-pinned relay session), or in a new
    pairing. Old endorsements of a reconfigured unit therefore stay refused,
    even though its sequence started again at 1.

### 4.3 Uses of the device key

| Use | Message prefix |
| --- | --- |
| TLS 1.3 CertificateVerify | 64 × 0x20, `"TLS 1.3, server CertificateVerify"`, 0x00 |
| Its self-signed certificate | an X.509 TBSCertificate (DER, begins 0x30) |
| Endorsement | `"DOORS relay endorsement v1"` 0x00 |
| Unit login | `"DOORS relay unit login v1"` 0x00 \|\| 32-byte nonce \|\| 32-byte relay key hash |

- Each prefix differs from the others before its first 0x00, or in its first
  byte. No byte string is a message of two kinds.
- The phone's key signs `"DOORS relay client login v1"` 0x00 || nonce ||
  relay key hash || `device_id`. The phone's key is already a signing key
  (`KeystoreIdentity`, SHA256withECDSA).
- The relay's key signs `"DOORS relay key proof v1"` 0x00 || nonce, and its
  confirmed active key signs `"DOORS relay backup v1"` 0x00 || backup hash
  || nonce (§4.1).

### 4.4 Rotation and recovery

| Event | What happens | LAN needed? | Person at the unit? |
| --- | --- | --- | --- |
| Planned rotation | The operator brings `B` online. The helper reports it; gatewayd checks `key_proof`, promotes `B`, and endorses it with `seq+1`. Phones whose pin fails verify the endorsement through the relay and re-pin | no | only to add a new backup |
| `A` stolen | As above. The unit no longer accepts `A` at all. A phone that has seen `seq+1` refuses `A`'s old endorsement. One that has not can be shown `A`: metadata and denial, never content | no | to add a backup |
| `A` and `B` lost | The unit cannot connect | - | yes: set up again. Phones follow by endorsement |
| Address change | Change the DNS record | no | no |
| Relay renamed | Phones learn the new name in `gateway.hello` (LAN, or a relay session before the rename), or from a new pairing code | for phones that missed it | to configure the unit |
| `relay.v1` lost | Fault; remote off. A new set-up draws a new configuration id, so old endorsements stay refused (§4.2) | - | yes |

## 5. The relay protocol (rules 35, 39)

### 5.1 Every connection

1. TLS 1.3 with SNI (the relay's DNS name) and ALPN `http/1.1`. The relay
   presents its key in a single self-signed certificate, or a CA-issued chain
   for the same key (rule 39).
2. Then:
   ```
   GET /doors-relay/1 HTTP/1.1
   Host: <relay name>
   Connection: Upgrade
   Upgrade: doors-relay/1
   ```
   answered `101 Switching Protocols`. Anything else is answered `404`.
   Headers are at most 2 KiB.
3. Then length-prefixed JSON frames, at most 4 KiB, of a fixed set of types.

### 5.2 Frames

| Connection | From | Frame | Meaning |
| --- | --- | --- | --- |
| all | relay | `challenge {nonce}` | 32 random bytes |
| control | unit | `unit {spki, sig}` | Unit login (§4.3) |
| control | relay | `backup {spki_a, spki_hash_b, sig}` | The backup pin, signed by the confirmed active key over the unit's nonce; at set-up only (§4.1) |
| control | relay | `key_proof {spki, sig}` | Answer to `prove {nonce}` from the unit (§4.1) |
| control | unit | `register {admit[], endorsement}` | At most 8 admitted fingerprints; replaces an earlier control connection |
| control | unit | `admit {admit[], n}` and relay `admitted {n}` | Replace the list, and confirm it |
| control | unit | `pair_slot {slot or null}` | `slot = HKDF-SHA256(token, "DOORS relay pair slot v1")[0..16]`, sent only while a window allows the relay |
| control | relay | `invite {cookie, client, via}` | A phone wants a session. `client` and `via` are hints |
| control | unit | `decline {cookie}` | - |
| control | both | `ping/pong {n}` | Every 25 s; 75 s of silence closes |
| client | phone | `which {lookup}` and relay `endorsement {E, sig}` or `unavailable` | Only on the provisional path (§4.2) |
| client | phone | `client {spki, sig}`, then `connect {slot?}`; relay `ready` or `unavailable` | - |
| data | unit | `join {cookie}` | No login: the cookie is one-time, 128 bits, bound to the control connection, and valid for 5 s |

Nothing names a port, host or service.

## 6. First pairing through the relay, and `remote` (rules 33, 34)

1. On the unit: PAIR A PHONE. The page has **"Also through the relay"**,
   off.
   - **Off:** a LAN-only window, as today.
   - **On:** the code gains `r=name:port`, gatewayd sends `pair_slot`, and
     the helper holds a control connection for the window. Old apps read
     only `h`, `p`, `fp`, `t` and `n` (`PairingUri.kt`), so they ignore `r`.
   - The code stays within the app's 512-character limit, and gatewayd
     checks its `snprintf` for truncation.
   - If the interface has no private IPv4, the window is relay-only, and the
     code leaves `h` out. New apps then use `r`; old apps report that there
     is no address.
2. **Candidates.** The phone tries `h:p` locally; if that fails, it reaches
   the relay, verifies the endorsement against `fp`, logs in, and sends
   `connect {slot}`. **gatewayd keeps one candidate slot and one count of
   wrong tokens for the LAN, and another pair for the relay**
   (`gw_pair.[ch]`). A relay candidate's three wrong tokens close the relay
   path of the window, not the window.
3. Inside the spliced gateway TLS, everything is ADR-019's.
4. **The approval sheet** shows "Connecting **through your relay**", the
   candidate's name and key with "Same key on the phone" (amendment 2), the
   usual scope switches, and "Through the relay", **shown off**.
5. **Approved with "Through the relay" on:**
   1. `relay.v1` gains the phone.
   2. `admit {.., n}` is sent, and gatewayd waits for `admitted {n}` (at most
      3 s) before answering `{paired: true, scopes: [.., "remote"]}`.
   3. The candidate's relay slot is released when its session ends.
   4. The phone reconnects through the relay as a paired phone. If no
      `admitted` came, it retries twice within 10 s.
6. **Approved without it:** the answer has no `remote`. The app says:
   "Paired. Remote access is off for this phone: it reaches the unit on the
   same network. If it is switched on at the unit, tap 'Try through the
   relay'." The relay is tried again only by that tap, and the tap is admitted
   once `remote` has been given at the unit. Nothing else changes.
7. **Refused, expired, or a wrong token:** as ADR-019.
8. Later, `remote` is given or taken on the phone's page. Every change sends
   `admit`, and ends a relay session the phone no longer may hold.

## 7. Paths, races and session replacement (rules 36, 37)

### 7.1 The phone

```
start ─► on Wi-Fi? ─yes─► LAN TCP to last address (then /24 probe) ──┐   first transport ready gets
            │                                                         ├─► the ONE gateway handshake;
            no ─► relay (after 1.5 s head start for the LAN, or ──────┘   the other attempt is closed
                  at once without Wi-Fi; only for a unit whose             before any gateway byte
                  relay the phone knows and, once paired, only
                  with `remote` or on the person's tap)
```

- **Fallback.** If the winner's gateway handshake fails its pin, the phone
  tries the other path. A LAN address whose pin failed is forgotten.
- **Back to the LAN.** While on the relay, a Wi-Fi change makes the phone
  try the LAN once, then with PAIRING.md's back-off. Success replaces the
  relay session (§7.2).
- **Never two at once.** Two gateway handshakes for the same unit never run
  together.

### 7.2 The unit

- **Replacement is unchanged** (`main.c` around 720-733): a new session of a
  paired phone replaces its older one at the end of the handshake, before
  the session limits are counted.
  - The phone's one-handshake rule removes the case of a stalled attempt
    ending a working one.
  - Replacement before counting keeps PAIRING.md §7's promise that a phone
    that changed networks is not locked out.
- **The relay budget counts after replacement**, everywhere along the
  path. A phone returning through the relay replaces its own dead relay
  session, rather than being refused by a limit of 1:
  - **the relay:** a new `connect` from a client that already has a splice
    to that unit ends the old splice;
  - **the helper:** a new invitation for a client that already has a data
    connection closes the old one and its socketpair end, and gatewayd sees
    the old session end;
  - **gatewayd:** the new inner handshake replaces the old session, before
    counting.
- **Only reasons of address spare relay sessions** (rule 29). When the
  gateway closes for a reason of its own (the switch off, `password_required`,
  `identity_fault`, `state_fault`), it closes its relay sessions and tells the
  helper to disconnect, exactly as it closes its listener. Without this,
  `passwd -d` would close the LAN within seconds while remote phones kept
  their sessions (`main.c:593-598`, `main.c:3747`).
- **A relay cannot create or replay a gateway session.** It holds no phone
  key, and TLS 1.3 needs fresh ECDHE.

## 8. Isolation from a hostile relay (rule 38)

### 8.1 The helper process

- **Started by `S70gatewayd`** under `pos-supervise`, beside gatewayd,
  whenever `GATEWAYD_RELAY=1`. gatewayd has dropped its capabilities and
  cannot start it.
- **Idle until asked.** It opens no network connection until gatewayd asks
  for one (rule 29).
- **Never exits because of a relay.** A misbehaving relay makes it close
  that connection and back off. It never exits for that reason, so the
  network cannot drive it into `pos-supervise`'s stop after 5 restarts in
  60 s (`tools/supervise/pos-supervise`).
- **At start**, before any network I/O, it:
  - `setgroups(0)`, `setresgid` and `setresuid` to a fixed dedicated id. The
    image has no per-service users, so a number is reserved for it; to be
    checked at phase 2;
  - drops every capability;
  - sets `no_new_privs`;
  - installs a seccomp filter: the socket, poll, read and write calls, and
    the few that OpenSSL and the resolver need;
  - sets `RLIMIT_AS` 32 MiB (ASSUMED sufficient), `RLIMIT_NOFILE` 32, and
    nice 10.
- **It needs** only `/etc/resolv.conf` and the socket. A test checks that it
  runs under its own id and cannot open `identity.v1` or `/etc/shadow`.
- **Its verify callback** checks the relay's leaf key against `A`/`B` and
  ignores the rest of a chain.
- **The line to gatewayd** is an **abstract-namespace** Unix socket
  (`@doors-gatewayd-relay`). A path socket would not do: it would be root's
  0660 file, and gatewayd has no `CAP_CHOWN` to open it to the helper's
  group.
  - gatewayd admits only a peer whose `SO_PEERCRED` uid is the helper's
    reserved id.
  - gatewayd reads the line with `recv()` and no control buffer, so a
    descriptor the helper sends is discarded by the kernel.
  - It writes to the line non-blocking, through a bounded queue.
  - **From the helper**, at most 256 bytes each and 20 per second, else the
    connection is closed:
    - `presented {spki_hash}`;
    - `proof {spki, sig}`;
    - `sign_login {nonce, relay_key_hash}`, signed only if the hash is a pin,
      at most 1 per second;
    - `want_session {cookie, via, client}`;
    - `state {..}`.
  - **From gatewayd:** `admit`, `pair_slot`, `endorsement`, `signature`,
    `prove {nonce}`, and `session {cookie}` **with one end of a fresh
    non-blocking socketpair (SCM_RIGHTS, gatewayd to the helper only)**.
- **The descriptor direction.** gatewayd keeps the other end. The two ends
  are separate open file descriptions, so the helper cannot change
  gatewayd's end's flags. gatewayd never receives a descriptor.

### 8.2 Budgets in gatewayd

Inside ADR-019 §6:
- 1 relay session;
- 1 relay handshake pending;
- 1 relay handshake per second;
- after n failed relay handshakes in a row, no relay session for 2^n s
  (capped at 5 min);
- a `want_session` beyond the budget is answered `decline`.

Before the inner handshake completes, the only work is OpenSSL's TLS 1.3
server handshake, as for a LAN socket. Per-session reads are already bounded
by `GW_READ_BATCH` (`main.c:2898`), so the loop is not changed.

### 8.3 Budgets in the helper

**Bytes read before decryption:**
- control connection: 1 KiB/s on average, with a 4 KiB burst. Its frames
  are small and rare, and this keeps a hostile relay to tens of MB a day on
  a metered link;
- data connection: 256 KiB/s per direction;
- otherwise it stops reading, and TCP pushes back.

**Frames:**
- control connection: 10 per second; more, or a malformed one, closes it and
  backs off;
- invitations: at most 1 per second acted on, the rest declined;
- data connections: 1 at a time; a new one for the same phone replaces it
  (§7.2).

**Timeouts:**
- TLS and the Upgrade: 10 s;
- `join`: 5 s;
- idle control connection: 75 s;
- idle session: 90 s.

### 8.4 What is left

- **A hostile relay or a compromised helper can:** deny remote access, use
  the helper's share of the hart at nice 10, and learn metadata.
- **They cannot:**
  - starve LAN phones (they keep their own slot);
  - starve gatewayd's loop (no relay parsing in it, no blocking descriptor,
    bounded reads);
  - stall the shell (the helper runs at nice 10, and relay handshakes are
    limited to 1/s);
  - reach root or the device key.
- **Not caused by the relay:** gatewayd's synchronous `upstream_call` (up to
  about 400 ms, `main.c:799-842`) can stall its loop for any paired phone,
  LAN or relay. It is pre-existing, and is recorded for a separate fix.

## 9. Restrictive networks (rule 39)

| Network does | Design answer |
| --- | --- |
| Allows only 80/443 | 443 |
| Inspects SNI and ALPN | SNI = the relay's DNS name; ALPN `http/1.1` |
| Validates server certificates (some middleboxes) | The relay may present a CA-issued chain for its pinned key, issued with the key reused at renewal (for example certbot `--reuse-key`). The decision stays the leaf key, with no dates |
| Checks that 443 carries HTTP | A genuine HTTP/1.1 Upgrade; non-Doors requests answered `404` |
| Requires an explicit HTTP proxy | Phone: `LinkProperties.getHttpProxy()`, via `CONNECT`. PAC: not supported. Unit: no proxy in v1 |
| Captive portal before sign-in | The app sees that the network is not `VALIDATED`, or is `CAPTIVE_PORTAL`, and says "Sign in to this network first" |
| Intercepts TLS | Pins fail; the app says "This network inspects encrypted traffic; the relay cannot be used here." |
| Drops idle TCP | 25 s pings |
| Blocks by reputation | Not addressed |

Not in v1:
- WebSocket framing: useful only if TLS ends before the relay, which pinning
  excludes.
- A CDN or TLS-terminating proxy. An SNI pass-through such as nginx `stream
  ssl_preread` is fine.
- HTTP/2.
- Domain fronting.

## 10. Unit changes (phase 2)

- **sysd:**
  - writes `GATEWAYD_RELAY=0|1` for Settings;
  - runs `S70gatewayd relay-start|relay-stop` from a fixed argv, as for the
    gateway switch.
- **`S70gatewayd`:** starts `gatewayd-relay` under `pos-supervise` whenever
  `GATEWAYD_RELAY=1`, and stops it otherwise. Whether it connects is
  gatewayd's word over the line (rule 29), so no file is watched.
- **The image:** reserves the helper's uid and gid (numeric, no login).
- **gatewayd:**
  - `relay.v1`;
  - the helper's socket;
  - socketpairs going out;
  - §8.2;
  - the `remote` check after the handshake;
  - the second candidate slot;
  - endorsements and `key_proof`;
  - unit-login signing;
  - relay sessions kept across changes of address, and closed with the
    gateway for its own reasons (§7.2);
  - relay-only windows;
  - the backup signature check at set-up, and the configuration id.
- **API:**
  - `gateway.status`: `relay {state, pin_short, backup_set, seq}`, and `via`
    per session;
  - `gateway.clients`: `remote`;
  - `gateway.scope`: `remote`;
  - `gateway.hello`: `relay {name, port, config_id, key_hash, endorsement}`
    for paired phones;
  - local `gatewayd.sock` methods for set-up: `gateway.relay_probe` (returns
    the presented key's short fingerprint), `gateway.relay_confirm` and
    `gateway.relay_set`.
- **Settings:**
  - a REMOTE ACCESS card: the switch; set-up by typing the name and
    comparing the fingerprint; the state; the sentence of rule 42;
  - "Also through the relay" on the pairing page;
  - "Through the relay" on the approval sheet and each phone's page.
- **Rollback.** An older gatewayd ignores `relay.v1`, and the paired list
  carries no new scope bit. A rollback therefore just turns remote access
  off.

## 11. Android changes (phase 3)

- **`RelayTransport`:**
  - uses the default network and the network's HTTP proxy;
  - honours `CAPTIVE_PORTAL` and `VALIDATED`;
  - uses a pinned context when the relay's key is known: the existing
    `PinnedTrustManager`, in a leaf-only mode reserved for the relay;
  - uses the provisional context only as §4.2 says.
- **The lint is amended:**
  - the trust managers are `PinnedTrustManager` (the gateway mode requires
    exactly one certificate; the leaf-only mode is reachable only from
    `RelayTransport`), and one provisional class used only in
    `RelayTransport`;
  - TLS 1.3 only;
  - no platform trust store;
  - no hostname verifier that returns true.
- **Stored per unit:** the relay's name and port, the configuration id, the
  key hash, and the highest sequence.
- **Pairing:** read `r`. The UI shows the path, the "remote off" message of
  §6, and "Try through the relay".

## 12. Resource consumption

| Where | Cost | Class |
| --- | --- | --- |
| Unit, remote wanted, idle | One more process (C, OpenSSL), a few MB RSS. One TLS connection and a 25 s ping, about 0.3 MB/day. Nothing at all while no phone holds `remote` and no window allows the relay | ASSUMED |
| Unit, per relay session | Two handshakes: outer, in the helper; inner, in gatewayd. At most one relay handshake per second. Double encryption of at most 256 KiB/s | ASSUMED; H2 |
| Unit, traffic | Status and RIFT: bytes per second. Vision about 35 KB/s while shown (`M6-STATUS.md`) | DOCUMENTED |
| Phone | Nothing in the background | ASSUMED |
| Relay | Go; a 1-vCPU host with 64-128 MB RAM serves tens of units | ASSUMED |

## 13. The relay server (Go)

- **Standard library only:** `crypto/tls`, `net/http` (Upgrade with
  `Hijack`), `crypto/ecdsa`, `crypto/hkdf` (Go 1.24 and later).
- **Configuration:**
  - the listen address;
  - the key files (active, and backup when promoted), or a CA-issued
    certificate for the active key;
  - the enrolled unit fingerprints;
  - the limits: per source address, 10 new connections per second and 64
    unauthenticated; per unit, 1 control connection, 8 admitted phones, 1
    pairing slot and 1 session (a new `connect` from the same phone replaces
    its splice); per session, 90 s idle.
- **Memory only:** admission lists, endorsements, pairing slots.
- **Logs:** rule 41.
- **Tools:**
  - `doors-relay keygen`: prints the 16-digit and full fingerprints;
  - `doors-relay backup-keygen`: makes the backup key. Its private half
    stays offline, and only its hash goes into the relay's configuration,
    for the active key to sign at a unit's set-up;
  - `doors-relay enroll <device_id>`.
- **Tests:** Go tests with fake units and phones, `-race`, native fuzzing of
  the HTTP start and frames, and the repository's Python harness end to end
  with the real gatewayd.
- **Build:** `go build` from the repository, as a separate artefact.

## 14. Existing solutions compared

| | Model | Gateway TLS end to end | Only the gateway reachable | On the K230 | On Android | Clock | Licence (to confirm) | Fit |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tailscale + Headscale + DERP | WireGuard mesh, DERP relays | yes | **no**: IP reachability (sshd too); no firewall in the image | Go daemon or kernel WireGuard | VPN app; one VPN per phone | key expiry to disable | BSD-3-Clause | Poor |
| WireGuard + hub | Hub VPN | yes | no | kernel or userspace | VPN app | none | GPL-2.0 / MIT | Poor |
| Nebula | Overlay | yes | no | Go daemon | VPN app | **certificates carry validity** | MIT | Poor |
| ZeroTier | Overlay | yes | no | C++ daemon | VPN app | - | BSL 1.1 (recent) | Poor |
| coturn (TURN) | Allocation relay | yes | yes | TURN client (heavy) | TURN client | static credentials | BSD-3-Clause | Fair |
| frp / rathole | Reverse forwarding | yes | only in secret modes | second daemon | visitor client | none | Apache-2.0 | Fair to poor |
| SSH reverse tunnel | Port forwarding | yes | needs careful configuration | autossh, another key | SSH client | none | BSD | Poor |
| Syncthing relay | Blind relay, invitations | yes | yes | client | client | none | MPL-2.0 | Good model; admits anyone |
| iroh relay | QUIC with relay | becomes QUIC | yes | Rust library | Rust library | none | MIT / Apache-2.0 | Poor |
| **Minimal first-party** | Blind relay, Syncthing-style invitations, unit-published admission, unit-signed endorsements | **yes** | **yes** | unprivileged helper; OpenSSL already present | a transport | none | Apache-2.0 | **Chosen in principle** |

## 15. Independent review (2026-11-27) and answers

### 15.1 The review

A reviewer with no part in the design read the three documents against
ADR-019, PAIRING.md, THREAT_MODEL.md, CONTRACTS.md and the code at
`6a8ef974`. Verdict: "The architecture is sound and the right choice over a
VPN … It is not ready for implementation."

Found sound:
- the blind relay, with the inner TLS untouched and the pin check unchanged
  (`gw_tls.c:313-340`);
- the endorsement concept, the sequence rule, the binding to host, port and
  key, the domain separation, and logins bound to the relay's key;
- the one-way pairing slot, and physical approval unchanged;
- `remote` never a default, and checked again by the gateway;
- `r` backward compatible (`PairingUri.kt:41-49`);
- the Keystore key already allows SHA256withECDSA;
- clockless throughout, fail-closed on interception, Go standard library only.

**Blockers:**

| # | Finding | Answer |
| --- | --- | --- |
| 1 | gatewayd drops its capabilities and sets `no_new_privs` at start (`main.c:3608`), so a helper it forks stays uid 0 and could read `identity.v1` | The helper is started by `S70gatewayd` under `pos-supervise`, drops to a dedicated id with seccomp, and is checked by `SO_PEERCRED`. A test holds that it cannot read the key (§8.1; rule 38) |
| 2 | Receiving descriptors from the helper lets it block gatewayd (it clears `O_NONBLOCK`, or passes a FIFO) | gatewayd makes the socketpairs and sends ends out. It never receives a descriptor (§8.1) |

**Major:**

| # | Finding | Answer |
| --- | --- | --- |
| 3 | Rotation: re-accepting `A` after `B`, a forged `seen {B}`, writes in the loop, and a sequence reset after a lost `relay.v1` | One-way promotion, after a `key_proof` by the presented key, written once. The old pin is removed. A lost `relay.v1` is a fault (§4.1, §4.2) |
| 4 | Replacing at `hello` collides with the session limits (lockout for up to 90 s) | Replacement stays at the end of the handshake, before the limits. The phone's one-handshake rule covers the stalled-attempt case. The relay budget counts after replacement (§7.2) |
| 5 | A hostile relay can hold the one candidate slot, or burn the three strikes, and block LAN pairing | Separate candidate slot and strike count for the relay path (§6, step 2) |
| 6 | A phone paired without `remote` on an isolating network can never learn of a later grant | "Try through the relay" on the person's tap. It is admitted once `remote` is given (§6, step 6) |
| 7 | Relay sessions tied to the LAN listener (`listener_close`, `closed_reason`, pairing needing the listener) | Relay sessions and relay-only windows are independent of the listener (rule 29; §6, step 1) |
| 8 | The provisional relay trust manager contradicts rule 13 and the lint; `which` leaked the `device_id` before verification | Rule 13 is amended with one confined exception. Phones normally pin the relay from `gateway.hello`, and use the provisional path only for a first pairing or an unseen rotation. `which` sends a lookup bound to the presented key (§4.2; §11) |

**Minor:**

| # | Finding | Answer |
| --- | --- | --- |
| 9 | No QR decoder in the repository | No camera set-up: type the name, compare 16 digits, confirm (rule 28) |
| 10 | Sign logins only for pinned keys; specify the client login; data-connection logins redundant | All three adopted (§4.3, §5.2) |
| 11 | Certificate-checking middleboxes; "the unit's network is the owner's" contradicted the Context; PAC | CA-issued chain allowed, decision by leaf key. The unit has no proxy in v1, without the claim. PAC not supported (§9) |
| 12 | 10 frames/s × 4 KiB on the control connection is too much on a hotspot | A byte limit before decryption, lowered after the re-check to 1 KiB/s with a 4 KiB burst (§8.3) |
| 13 | Keep "closed until asked" | No relay connection unless a phone holds `remote`, or a window allows the relay (rule 29) |
| 14 | `which` is an oracle for online units | Bound lookup. The residual is stated in the ADR's risks |
| 15 | Pairing code length; unchecked `snprintf` (`main.c:3170`) | Name at most 64 characters; check for truncation (§6, step 1) |
| 16 | A rollback would put `state_fault` on a new scope bit (`gw_store.c:79`) | `remote` lives in `relay.v1`; rollback means remote off (§10) |
| 17 | Rules 2 and 3 touched; the self-signed certificate missing from domain separation | Listed (ADR "Relation"; §4.3) |
| 18 | Reads are already bounded (`GW_READ_BATCH`); the real stall is the synchronous `upstream_call` | Loop change dropped. `upstream_call` recorded as pre-existing (§8.4) |
| 19 | A photographed code works from anywhere; full addresses in logs are a location history | Per-window "Also through the relay", off by default. Truncated addresses (rule 41) |

### 15.2 The reviewer's re-check of the revision

The same reviewer re-read the revision against its findings and the code.

- **Resolved:** blockers 1 and 2; majors 5, 6 and 8.
- **Partly resolved:** majors 3, 4 and 7.
- **Checked and sound:** `key_proof`, the bound lookup, the provisional
  exception to rule 13, the separate relay candidate slot, the per-window
  relay switch, socketpair ends sent out only, and replacement kept at the
  end of the handshake.

The new findings, and the answers:

| # | Finding | Answer |
| --- | --- | --- |
| N1 (major) | The backup pin was never verified: a compromised helper could show the genuine `A` and store an attacker's `B`, then promote it with a valid `key_proof`, and every phone would follow | `backup` is signed by the confirmed `A` over a gatewayd nonce, verified by gatewayd, and accepted at set-up only (§4.1; rule 28) |
| N2 (major) | Replacement stopped at gatewayd: the helper's and the relay's limit of 1 refused a returning phone for up to 90 s | The relay and the helper replace the same phone's splice or data connection too (§7.2, §8.3, §13; rule 37) |
| N3 (major) | "Anything that closes the listener" kept relay sessions open with no password, or with a faulty state | Only reasons of address spare relay sessions. `password_required`, `identity_fault`, `state_fault` and the switch close them (§7.2; rule 29) |
| N4 | A sequence that starts again at 1 after a lost `relay.v1` reopens rollback | A random configuration id in every endorsement and in `gateway.hello`. A new id reaches phones only over an authenticated session or a pairing (§4.2; rule 31) |
| N5 | The helper's lifecycle was not implementable (nothing watched the `.want` file; `pos-supervise` stops after 5 restarts in 60 s) | It runs whenever `GATEWAYD_RELAY=1`, idles until gatewayd asks, and never exits on relay misbehaviour (§8.1, §10; rule 38) |
| N6 | The helper could not reach a root 0660 socket | An abstract-namespace socket, admitted by `SO_PEERCRED` only; the uid reserved in the image (§8.1, §10) |
| N7 | A CA chain broke the pinned path (`PinnedTrustManager` needs exactly one certificate) | Leaf-only checks for the relay, in a leaf-only mode of the same class and in the helper's verify callback (§4.2, §8.1, §11; rule 31) |
| Note | Omit `h` in relay-only windows; read the helper's line without a control buffer; write it non-blocking | All adopted (§6, §8.1) |

**Notes:** the helper's `via` and `client` are hints, said in rule 30. The
idea of an endorsement naming a new host was left out (§17).

## 16. Phases (each with its own go-ahead)

| Phase | What | Done when |
| --- | --- | --- |
| **0. Local paths** (with 1) | (a) The SkullerudPark root cause, with phone-side evidence. (b) F1: the unit on the phone's hotspot. Today's app binds only to networks the phone *joined*; allow the phone's own tethering subnet only (README H3/H12). (c) F2, the unit's access point on `wlan1`, in its own ADR | Recorded; F1 works on the Flip8; F2 decided |
| **1. Protocol and server** | `docs/api/relay.md`; the Go server and its tools; Go tests, `-race`, fuzzing; a lying test relay | Green; protocol and server reviewed |
| **2. Unit** | The helper (id, seccomp, limits); `S70gatewayd` and sysd; gatewayd's `relay.v1`, socket, socketpairs, budgets, `remote`, two candidate slots, endorsements, `key_proof`, listener independence; Settings; host integration with the real Go relay; cross-build | Off by default; the helper cannot read the key; a revoked phone loses its relay session within a second; the lying relay and a hostile helper gain nothing; a LAN phone is served during a flood |
| **3. Android** | `RelayTransport`; pinned and provisional contexts; endorsements; proxy and captive portal; paths; pairing with `r`; UI; lint | JVM tests against the real relay and gatewayd |
| **4. Hardware gate** | Unit B on an isolating network, the phone on mobile data: first pairing through the relay with the keys compared; `remote` off, then the tap after it is given; revocation; reboot; relay restart; promotion of the backup with the phone away; Vision and PacketShark; handshake cost and frame time; a relay flood with a LAN phone working | PASS per part, owner present |
| **5. Release** | Independent security review of the code; the relay's install guide | Owner's acceptance |

## 17. Left out on purpose (minimal)

- A relay run by Doors; accounts; a web UI; persistence on the relay.
- Several relays per unit.
- A proxy for the unit; PAC.
- QR set-up of the relay.
- WebSocket framing, HTTP/2, a CDN.
- UDP and hole punching; multiplexing.
- Background connections on the phone.
- Remote revocation or scope changes: they stay at the unit.
- Separate relay identities per unit.
- Endorsements that name a new host, to follow a rename without the unit.
- The fix of `upstream_call` (pre-existing, separate).
