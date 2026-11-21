# DOORS Companion (Android)

The phone side of the Doors Companion Gateway (ADR-019). It pairs with one
unit, keeps an authenticated TLS 1.3 session to it while the app is in front,
shows the unit's status (M3), reads the mesh through the gateway into a
history kept on the phone (RIFT, M4.1), and sends messages a person writes
while the session is open (M4.2). PacketShark is a navigation placeholder.

Application id `app.doors.companion`, min SDK 29, Kotlin and Jetpack Compose,
Apache-2.0 (the repository's `LICENSE`). It lives in `android/doors-companion`
and builds by itself with Gradle: the firmware's `make` does not look in here,
and nothing here needs the firmware toolchain. The contracts it is built to
are in this repository, from its root:
`docs/api/gateway.md`, `docs/design/companion/PAIRING.md` and `CONTRACTS.md`
section 2.

## Layout

| Path | What |
| --- | --- |
| `app/src/main/java/app/doors/companion/core` | No Android imports: the fingerprint, the pairing code, the pinned trust manager, the TLS handshake, framing, and the session loop (pair, reconnect, back-off, revocation). Runs on a host JVM, which is how it is tested against a real gatewayd |
| `.../core/Mesh.kt`, `MeshSync.kt` | The mesh as the gateway passes it on, and the reader: subscription, cursor, pages, events, breaks in continuity. Runs inside an open session and ends with it |
| `.../core/MeshSend.kt` | Sending: the text rules, the states of a send, the request, and what is done about a send without an answer. Runs inside an open session and ends with it |
| `.../data` | The mesh history on Room: messages, nodes, channels, the cursor, the breaks; and what the phone sent (`outbox`) and has not sent (`drafts`) |
| `.../device` | The Android Keystore identity, the Wi-Fi-bound socket transport, the stored pin |
| `.../ui` | The Doors theme and the screens |
| `harness/` | The host gateway test environment: builds the reference `gatewayd` test-hooks binary and runs it between fake services (WSL). Not part of the app |

## Trust, in one place

- `core/PinnedTrustManager.kt` is the only trust manager: one certificate, a
  P-256 key, SHA-256 of its SubjectPublicKeyInfo equal to the pin, constant
  time. No authority, date or name takes part and the platform store is never
  asked.
- `core/GatewayTls.kt` is the only place a TLS context is made: TLS 1.3 only,
  a fresh context per connection, and the result read back after the handshake.
- `device/KeystoreIdentity.kt`: the phone's P-256 key is generated in the
  Android Keystore, signing only, not exportable; StrongBox when the phone has
  one.
- `device/WifiTransport.kt`: every socket is made by the Wi-Fi network's own
  factory. Addresses are IPv4 literals on private ranges; nothing is resolved.
- A waiting loop never ends a pairing. Only the unit's own word over an
  authenticated connection (`gateway.revoked`, or `paired: false` in hello) is
  shown as a removal; no answer, a timeout, a lost session and a door closed
  after the handshake are retried with back-off. Another key at the address
  stops the loop and leaves the pin as it was.
- The loop tries the saved addresses only. Looking across the phone's /24 is
  a button on the Connection screen: one sweep, at most 8 TLS attempts, the
  pinned key only. A stored pin is never replaced; forgetting comes first.
- `SecurityLintTest` fails the build's tests if a second trust manager, a name
  verifier, an older protocol, cleartext, a build variant, a telemetry
  dependency or an extra permission appears.

## RIFT, reading (M4.1)

Built to `docs/api/gateway.md` section 7 as gatewayd serves it at `44ad0f35`:
`mesh.sync`, `mesh.nodes`, `mesh.node`'s node shape, `mesh.channels`,
`mesh.identity`, and the topics `mesh` and `mesh.activity`. The existing
session carries it; there is no second connection, service or notification.

- **The phone is the history.** The unit keeps its last 512 message events in
  memory and nothing on storage, so what the phone was given is kept in
  `mesh.v1.db` (no-backup directory; emptied when the unit is forgotten).
- **Identity.** A message is `(unit, run_id, id)`: meshcored numbers from 1
  again on every start, so the run is part of the name and an old run's
  messages stay what they were. A state change updates the row. A direct
  thread is its peer's public key; a channel is its slot and on-air hash.
- **Cursor.** `(journal_id, run_id, seq)`, advanced only in the same
  transaction as the messages it stands for, and only along consecutive `seq`.
  An event that is not the next entry is not applied: it starts a sync from
  the cursor. A first sync, and one after the journal changed, reads from the
  journal's oldest entry in pages of 100.
- **Breaks are said, and kept.** `gap: true`, another `run_id` and another
  `journal_id` each leave a mark in the stored history at the place they were
  found. A mark is never removed and nothing is ever shown as recovered: the
  first is "messages are missing here", the others "continuity unknown".
- **What it cannot know** (gatewayd's own limit): when meshcored was away
  within one run and more passed than it still holds (64 direct, 64 channel),
  the gateway reports no gap and neither can the phone.
- Times shown are the phone's estimate (its own clock minus the unit's
  monotonic age of the message); the sender's timestamp is stored, not used.

## RIFT, sending (M4.2)

Built to `docs/api/gateway.md` section 8 as gatewayd serves it at `ed08e918`:
`mesh.send` to a node or a channel, and `mesh.resend`. No route is chosen in
the app (`via` is never sent). No service, notification or permission is
added; a send lives in the session's scope like the reader does.

- **Stored first.** A message gets its `client_msg_id` (a UUID) and is written
  to `outbox` before a byte goes to the connection; its draft is removed in the
  same transaction.
- **A send is one of five things**: sending, accepted (the unit named the
  message it became), no answer (uncertain), outcome unknown, not sent.
  Delivery is none of them: it is the state of the mesh message the send
  became, read from the journal under `(run_id, message_id)`. So an event that
  arrives before the answer to the send is simply the message, and the answer
  cannot take a state back.
- **What is shown**: "Accepted" is the unit having taken it, "Delivered" is the
  recipient's own acknowledgement, "No acknowledgement" its absence at the
  deadline. A channel message is "Accepted", "On the air", "Heard back", and
  never delivered; the composer says so. A message still waiting for its
  acknowledgement when the mesh service started again is "Outcome unknown".
- **No answer is not failure.** An unanswered send is asked about once by the
  app, under the same id and only within two minutes of the tap. The gateway
  answers that from its record, or looks before it sends, so the repeat cannot
  transmit twice; if the gateway never had the message, that repeat is its
  one transmission, and the thread says so while it is pending. After that
  only a person asks ("Ask the unit"), or lets it go. A send found on its way
  when the app starts is treated the same.
- **After a gateway restart nothing is repeated.** Its records are gone
  (another `journal_id`), and a repeat would be a new message. The phone looks
  in what it synced (same thread, same words, learned after the tap, not
  already another send's) and calls what it does not find "outcome unknown".
- **Outcome unknown is final** for that id. "Send as new" is a person's choice,
  made after being told it may arrive twice. "Not sent" (the unit refused: no
  room, too long for the channel, rate, not permitted) can simply be tried
  again: nothing went out.
- **Resend** (`mesh.resend`) is offered on an outgoing direct message of the
  current run that got no acknowledgement, and asked before it is done.
- **Nothing waits to be sent.** Send is enabled only while the session is open,
  the gateway names `mesh.send`, the unit grants `mesh_send`, and the mesh is
  live. Otherwise what is typed stays a draft (`drafts`, per thread, kept
  through lost connections and restarts of the app). Nothing is sent on
  reconnect that was not already handed to a connection. Without the
  capability or the scope the composer is a statement, not a control.
- **One tap, one message.** The button is spent at the tap, and a second send
  in a thread is not taken while the first is on its way.
- **Text** is 1 to 160 UTF-8 bytes, a channel's own `text_limit` when it is
  less, no control character but newline and tab; counted in bytes as typed.
- **Rate.** `limits.send_per_min` from hello (20). The phone counts its own
  requests and holds Send when the window is full; the gateway's code 10 is
  shown as "not sent".

Not covered by a test that runs: the Room store itself (`RoomSendStore`, the
migration) is exercised on the emulator only, where a version 1 database with
its history was opened by this build.

Known limits: the gateway's at-most-once is per id while it runs and keeps 64
ids per phone, so a send left unanswered for more than 48 later sends is not
asked about but looked up; a match by words can take the unit's own message
with the same words for the phone's (the gateway does the same); a thread that
holds only an unsent message is reached from its node's page, not the list.

## Build and test

```sh
cd android/doors-companion
source /c/Android/toolchain/env.sh        # or any JDK 17 and Android SDK 36
./gradlew :app:testDebugUnitTest          # unit tests and the lint
./gradlew :app:assembleDebug
```

Against the real gateway (WSL, with gcc, libssl-dev, libcjson-dev, python3):

```sh
harness/run-gateway.sh [checkout with gatewayd M4.1]    # in WSL; leave running
./gradlew :app:testDebugUnitTest -Ddoors.gateway.ctl=127.0.0.1:47232
```

The harness's meshcored is a fake written from `docs/api/mesh.md`
(`harness.mesh` on the control port: `seed`, `message`, `state`, `bulk`,
`restart`, `down`, `up`, `demo`; for sending `send_mode`, `release`, `sends`,
`auto`). It takes `mesh.send` as the gateway's own suite models it and counts
what it was made to transmit. `MeshIntegrationTest` and `SendIntegrationTest`
drive the real gatewayd with it; the second needs a checkout with gatewayd
M4.2 (`harness/run-gateway.sh <checkout>`).

On an emulator the harness is at `10.0.2.2:47231`: take the URI from
`gateway.pair_begin` on the control port, replace `h=` with `10.0.2.2`, paste
it in the app, then send `gateway.pair_decide {"allow": true}`.

## Not yet

Choosing a route for a message (`via`, `mesh.path_preview`), capture (PacketShark), the optional background
connection service and its notification, more than one paired unit, and
anything measured on a K230 or a physical phone.
