# Companion: pairing, certificates, revocation and reconnect

Status: **draft, not implemented.** Details ADR-019 (accepted 2026-11-21) rules 10 to 24. The wire methods are in `docs/api/gateway.md`.

## 1. Identities

| | Unit | Phone |
| --- | --- | --- |
| Key | ECDSA P-256, one per unit | ECDSA P-256, one per app install |
| Kept in | `$POCKETOS_STATE_DIR/gatewayd/identity.v1`, 0600, root | Android Keystore, non-exportable, hardware-backed where offered |
| Fingerprint | SHA-256 of the DER SubjectPublicKeyInfo, 32 bytes | the same |
| Shown to people as | 16 hex digits, groups of four | the same |
| Pinned by | each paired phone | the unit's `paired.v1` |

The fingerprint of the unit's key is its `device_id`. There is no other device identity (the unit has no serial number; `docs/design/companion/M0-PROPOSAL.md` §3).

## 2. Certificates on a unit without a clock

The unit's wall clock reads 1970 until NTP answers, and a phone's clock is the phone owner's to set. So nothing may depend on a date, and nothing may be "skipped" either. The rule:

1. A certificate is only the TLS container of a public key. Each side sends exactly one, self-signed.
2. **The trust decision is the pin and nothing else** (ADR-019 rule 11): one certificate in the message; its key is ECDSA P-256; the SHA-256 of its SubjectPublicKeyInfo equals a pinned fingerprint, compared in constant time. TLS 1.3's CertificateVerify proves the peer holds the private key; that check is the TLS library's and is never touched.
3. No certificate authority is trusted, so no chain is built and no field of the certificate other than the key takes part: not the validity dates, not the subject, not the issuer, not extensions.
4. So that no library and no later reader has a date to trip over, the fields are fixed: `notBefore` 1970-01-01T00:00:00Z, `notAfter` 9999-12-31T23:59:59Z (RFC 5280's "no well-defined expiration"), subject `CN=doors-gateway` or `CN=doors-companion`, a random serial. They are informational. A receiver that finds other values does not care; a receiver must never be changed to care.
5. Expiry is replaced by **revocation** (§5) and by replacing a key (§6).

What makes this safe rather than a bypass:

- The verifier is **narrower** than standard validation, not wider: standard validation accepts any certificate a trusted CA issued for the name; this accepts one key.
- It is the only verifier. There is no fallback path, no "insecure" flag, no debug build that accepts another key, and neither side calls into the platform's default trust store for this connection. On the unit, `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT` is always set and the pin check runs in the certificate verification callback; on the phone, a trust manager constructed for one pinned fingerprint is the only one the connection's `SSLContext` has. Lints on both sides refuse `SSL_VERIFY_NONE`, an empty or accept-all trust manager, and a hostname verifier that returns true (`CONTRACTS.md` §1, §2).
- No session resumption in v1: every connection repeats the full check, so there is no cached decision to go stale.

## 3. Pairing

```
 Person at the unit              gatewayd                         Phone
 Settings > Companion >
 Pair a phone  ───────────────▶  window opens (120 s)
                                 token T = 16 random bytes
 panel shows QR  ◀─────────────  doors-pair:1?h=<ip>&p=<port>&fp=<unit fingerprint>&t=<T>&n=<unit name>
                                                                  scans the QR
                                 ◀──── TLS 1.3, phone pins fp ───  presents its own key
                                 unknown key + window open
                                 = pairing candidate
                                 ◀──── gateway.hello ────────────
                                 ◀──── gateway.pair {T, name} ───
                                 T matches (constant time)
 panel: "Pixel 8  3F2A 91C4 0B7E D115
 wants to pair"  [scopes] ALLOW / REFUSE
 taps ALLOW  ─────────────────▶  writes paired.v1, flushes
                                 ──── {paired: true, scopes} ───▶  stores unit fingerprint, name, address
                                 window closes                    reconnects as a paired client
```

Rules:

- A window opens only from the unit (Settings, or `gateway.pair_begin` on the local socket). It never opens from the network, at boot, or by itself.
- One candidate at a time. A candidate may call `gateway.hello` and `gateway.pair` and nothing else, has 30 s to present the token, and is dropped on a wrong one. Three wrong tokens close the window.
- The token proves the phone saw this panel. **The tap is the decision**: no tap before the window ends is a refusal. The phone's name is the phone's claim and is shown as such; the fingerprint is what the person can compare with the app's own screen.
- The QR code carries the unit's fingerprint, so the phone has authenticated the unit before it sends the token; a host in the middle can complete neither handshake.
- The token exists in the gateway's memory and on the panel for the life of the window, is never stored and never logged, and is useless afterwards.
- A phone that is already paired and scans a new code for the same unit is shown as "already paired" on the panel; allowing it replaces its scopes.
- The ninth client is refused until one is removed.
- **Bench path before the Settings page exists:** `doors call gatewayd gateway.pair_begin` prints the URI on a root console; the app accepts a pasted URI; `gateway.pair_decide` is the tap. The local socket is root-only, so this is still an action on the unit.

Not in v1: pairing by a typed code. Without the QR code the phone cannot authenticate the unit first, and making that safe needs a PAKE or a careful comparison by eye (open item U8).

## 4. Scopes

Chosen on the approval sheet; defaults `status`, `mesh_read`, `mesh_send` on, `capture_view` and `capture_control` off. Changed later only on the unit (`gateway.set_scopes`). A live session learns of a reduction by `gateway.scopes` and is held to it from that moment: the gateway checks the scope on every request, not at connect.

## 5. Revocation

- On the unit: Settings > Companion > the client > Remove (`gateway.revoke`). The pin is removed and `paired.v1` is flushed before the action is acknowledged; a live session of that client receives `gateway.revoked` and is closed; its next handshake fails as an unknown key.
- From the phone: Forget device calls `gateway.unpair` when the unit is reachable, then deletes the unit's pin and, if the person chooses, its stored messages. When the unit is not reachable the phone forgets anyway and the unit's entry stays until removed there.
- A lost phone: remove it on the unit. Its key cannot be extracted from the Keystore, and without the pin on the unit it opens nothing.
- There is no revocation list and no expiry: the paired list is the whole truth, and it lives on the unit.

## 6. Key replacement

- The unit's key is replaced only by an explicit operator action (ADR-019 rule 16), which also empties the paired list. Every phone then fails its pin check, says "this is not the unit you paired", and offers only to forget it and pair again. A phone never accepts a new key for a known unit by itself.
- The phone's key is lost with the app's data (uninstall, clear storage). The phone pairs again as a new client; the old entry is removed on the unit.
- A re-flashed or re-imaged unit that lost its state directory is a new unit to every phone.

## 7. Connection life cycle

**Connect.** TCP to the last known address and port; TLS; pin check; `gateway.hello`; compare `proto` and `capabilities`; `gateway.subscribe`; then per feature: `mesh.sync` with the stored cursor, `capture.status`, the status reads.

**Find the unit again** when the last address fails:

1. Wait for a Wi-Fi network (the socket is bound to the Wi-Fi network explicitly, so a hotspot or a LAN without internet is not bypassed over mobile data).
2. Try the last address.
3. Probe the phone's own Wi-Fi subnet, at most a /24, for the port; attempt TLS only where the port answers; accept only the pinned key. The phone's certificate is sent only after the unit's key has been verified, so a stranger on that port learns nothing about the phone.
4. If nothing answers: "not on this network", and retry on the next network change or by hand.

**Back off.** 1 s doubling to 60 s with jitter, reset by a network change or by the person. No reconnect loop runs without either the app in front or the optional connection service.

**Keep alive.** The phone pings every 30 s; either side closes after 90 s of silence. TCP keepalive is not relied on.

**A second connection from the same client replaces the first**: a phone that changed networks leaves a dead socket, and refusing the new one would lock its owner out until the timeout. A request of the old connection still waiting upstream finishes without an answer, and a send it carried is recorded under its `client_msg_id`.

**Resume.**

| The phone sees | Meaning | It does |
| --- | --- | --- |
| same `journal_id` and `run_id` | nothing restarted | `mesh.sync {after_seq}` |
| `gap: true` | the journal wrapped while it was away | keeps what it has, marks a gap in the conversation |
| new `journal_id` | the gateway restarted | syncs without a cursor; merges by `(run_id, id)` |
| new `run_id` | meshcored restarted; ids start at 1 | syncs without a cursor; earlier messages stay under the old `run_id`; a message still waiting for an ACK is shown as outcome unknown |
| new `boot_id` | the unit restarted | all of the above; stored `mono_ms` values are no longer comparable and are shown by the phone's own receive time |

**A send in flight when the connection dropped** is repeated once after reconnect with the same `client_msg_id`. The gateway answers from its record, so one tap is one transmission. If the gateway restarted in between, its record is gone: the app does not repeat blindly but looks for the message in the first `mesh.sync` (same recipient or channel, same text, after the send time) and asks the person if it is not there.

**The unit goes away** (Wi-Fi off, service disabled, power): the app shows the last known state with its age. Nothing is queued for sending while disconnected in v1; a message is sent when the person can see it go.

**Upstream goes away** (`gateway.upstream`): the session stays; methods of that service answer code 9; the app greys the feature.
