# Companion: threat model

Status: **draft** for ADR-019 (proposed). A design-time analysis: no code exists, nothing was tested, and no independent review has taken place (open item U10).

## 1. What is protected

| Asset | Where | Why it matters |
| --- | --- | --- |
| A1 The node's voice | meshcored, reached by `mesh.send` | Whoever sends is this node, on the air, to other people |
| A2 Message text, contacts, channel names | meshcored (memory), the gateway journal (memory), the phone (storage) | Private conversation |
| A3 Channel keys, the mesh identity key, Wi-Fi passphrases | meshcored, netd | Not exposed by any API today; must stay so |
| A4 The unit itself | every Doors process is root | A compromised service is a compromised unit |
| A5 The unit's gateway key and paired list | `gatewayd/` state | Impersonating the unit; who is paired |
| A6 The phone's key | Android Keystore | Being that phone |
| A7 Captured traffic | `pos-capture` memory, `/root/Captures` | Other people's data (ADR-016) |
| A8 Availability of the panel | one hart shared with the shell | A busy gateway must not freeze the UI |

## 2. Who attacks

| Adversary | Can | Cannot (assumed) |
| --- | --- | --- |
| N1 A host on the same network | connect to the port, flood it, read and alter traffic in transit, answer on the unit's address | break TLS 1.3; read either private key |
| N2 A host on another network | nothing unless a router forwards the port | - |
| P1 A paired phone that is stolen, or runs a hostile app build | everything its scopes allow | extract the Keystore key; raise its scopes |
| P2 Another app on the phone | read what Android lets it | read the app's private storage or Keystore key on an unrooted phone |
| D1 Someone with the unit's SD card | read `identity.v1` and `paired.v1` | - |
| D2 Someone at the unit's panel | pair a phone, revoke phones, everything else the unit allows | - |
| M1 A stranger on the mesh | send any text to this node | - |

Out of scope: code already running as root on the unit; a rooted phone; the network's own operator beyond N1; the security of sshd on the image (noted in §4).

## 3. Threats and answers

| # | Threat | Answer | Residual |
| --- | --- | --- | --- |
| T1 | N1 calls the API without pairing | Mutual TLS with pinned keys; an unknown key is closed after the handshake, before any application byte is parsed (ADR-019 rule 14) | OpenSSL's handshake is the unauthenticated surface |
| T2 | N1 sits between phone and unit | Each side pins the other's key; pairing carries the unit's fingerprint on the panel, out of band | None known in the design |
| T3 | N1 pairs itself | A window opens only on the unit, for 120 s; the token is on the panel only; a person must tap ALLOW on a sheet showing the candidate's fingerprint | D2 approving a phone that is not theirs. Shoulder-surfing the QR lets an attacker become the candidate first; the fingerprint on the sheet then differs from the owner's phone, and the owner's phone reports that pairing failed |
| T4 | N1 floods handshakes to stall the panel (A8) | 4 handshakes per second, 8 pending, nice 5, no work before the pin check | Handshake cost on the hart is unmeasured (H2); a flood still costs the listener |
| T5 | Hostile frames after authentication crash or take over the gateway (A4) | Only paired clients reach the parser; 64 KiB frames; cJSON; schemas per method; fuzzing and sanitizer suites; capabilities dropped | uid 0: a memory-safety defect reachable by P1 is a root compromise. Accepted until per-service users |
| T6 | P1 uses the gateway as a path to the rest of the root API | Allowlist with result filters; no `shell.*`, nothing that changes the unit (ADR-019 rule 25); scope checked per request | A defect in an upstream service reachable through an allowed method |
| T7 | P1 sends as the node (A1) | That is `mesh_send`. Rate limit 20 per minute; revocation on the unit; the unit shows a mark while a client is connected | Messages sent before the phone is revoked |
| T8 | P1 reads conversations (A2) | That is `mesh_read`. Revocation. On the phone: private storage, no cloud backup, optional app lock | What the phone already stored |
| T9 | Keys or passphrases leak outward (A3) | Upstream never returns them; the gateway has no method that asks; a lint forbids the words `key`, `secret`, `passphrase` in an allowlisted result filter | None in the design |
| T10 | D1 impersonates the unit to its phones | Not prevented: file permissions only (ADR-003, ADR-017). The phone shows the fingerprint; forgetting the unit ends it | Accepted until a device-bound key store |
| T11 | D1 learns who is paired | `paired.v1` holds names and fingerprints, no secret | Accepted |
| T12 | A revoked phone keeps a session or a cached credential | Revocation closes the live session; no tickets, no resumption (rule 10) | None in the design |
| T13 | Either clock is wrong or set back | No decision reads a date (PAIRING.md §2); ordering uses sequence numbers and the unit's monotonic clock | None in the design |
| T14 | M1 attacks the phone through message text | Text is data: plain rendering, no link preview, no automatic fetch, no markup, bounded length; notifications show text only | A platform defect in text rendering |
| T15 | M1 or a busy mesh floods the phone through the gateway | Per-node coalescing of `mesh.node`, activity as one-second bins, bounded queue, then the client is dropped | A dropped session and a resync |
| T16 | A slow or silent phone stalls meshcored or RIFT | The gateway reads upstream continuously and queues per client; pocketipc's 200 ms rule never has cause to fire against it | The gateway itself being stalled is T4 and T5 |
| T17 | A retry transmits a message twice | `client_msg_id` records; unknown outcomes resolved against `mesh.messages` | A gateway restart between the send and the retry (PAIRING.md §7): the app asks the person |
| T18 | The companion becomes a way to capture without anyone at the unit (A7) | ADR-016 amendment 1: a request starts nothing; START is a tap on the panel; the capture ends when the app is left | A paired phone can raise the request sheet (rate limited) |
| T19 | Capture figures identify people | Counts, rates and sizes only; rule 11a; a lint on how `capture.status` is built | How busy a network is, and that it has faults |
| T20 | The service is reachable from the internet | One interface's address, private sources only, closed until paired, disabled by default | A router that forwards the port from a private source address is not detectable |
| T21 | Logs carry secrets or text | Lifecycle lines only (rule 9), held by a lint | - |
| T22 | An older or modified app relaxes verification | The unit never trusts the app's checks: the unit's pin check stands alone. The app's own lint forbids an accept-all trust manager | A hostile build of the app is P1 |
| T23 | A person is tricked into scanning a QR that is not their unit's | The app shows the unit's name and fingerprint before it stores the pin; pairing gives that unit nothing but the phone's public key and name | The phone would then talk to the wrong unit |

## 4. Standing weaknesses this design does not remove

- **Every Doors process is root.** The gateway narrows what reaches them; it does not contain a defect in itself.
- **sshd accepts root by password on the same network** (`docs/hardware/T-DISPLAY-K230.md`, ADR-016). Whoever has that needs no gateway. ADR-019 rule 9b therefore keeps the gateway closed until a device password is set; setting one is not yet something Doors offers (open item U7).
- **No encryption at rest on the unit**, and no firewall in the image.
- **No forward secrecy problem**: TLS 1.3 provides it; noted because peerd's design (ADR-017) does not have it and the two should not be confused.

## 5. Checks that must exist before this carries real traffic

Mirrors ADR-017's list; each is part of an implementation milestone, none exists yet.

1. Known-answer tests for the fingerprint and the pin comparison; a test that a certificate with any date, name or issuer and the right key is accepted, and one with the wrong key and everything else identical is refused.
2. Handshake tests: no client certificate, two certificates, an RSA or P-384 key, TLS 1.2, a resumption attempt, an unknown key outside and inside a window, a second candidate.
3. Pairing tests: wrong token, three wrong tokens, timeout, refusal, a full list, approval after the candidate left.
4. Revocation tests: a live session is closed; the next handshake fails; the list is durable across a kill between the write and the acknowledgement.
5. A fuzzer over the frame reader and every parameter schema; the suites again under ASan and UBSan.
6. An allowlist test against fake services: every method not in the table is code 1 and reaches no upstream; every result is checked against its filter.
7. The lints of `CONTRACTS.md`.
8. On hardware, with a go-ahead: handshake cost and the shell's frame time during a connect and during a handshake flood; the entropy source at first enable; the state directory's durability across a power cut.
