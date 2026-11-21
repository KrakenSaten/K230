# DOORS Companion (Android)

The phone side of the Doors Companion Gateway (ADR-019). It pairs with one
unit, keeps an authenticated TLS 1.3 session to it while the app is in front,
shows the unit's status (M3), and reads the mesh through the gateway into a
history kept on the phone: RIFT, read-only (M4.1). It sends nothing to the
mesh. PacketShark is a navigation placeholder.

Application id `app.doors.companion`, min SDK 29, Kotlin and Jetpack Compose,
Apache-2.0. The contracts it is built to are in the Doors repository:
`docs/api/gateway.md`, `docs/design/companion/PAIRING.md` and `CONTRACTS.md`
section 2.

## Layout

| Path | What |
| --- | --- |
| `app/src/main/java/app/doors/companion/core` | No Android imports: the fingerprint, the pairing code, the pinned trust manager, the TLS handshake, framing, and the session loop (pair, reconnect, back-off, revocation). Runs on a host JVM, which is how it is tested against a real gatewayd |
| `.../core/Mesh.kt`, `MeshSync.kt` | The mesh as the gateway passes it on, and the reader: subscription, cursor, pages, events, breaks in continuity. Runs inside an open session and ends with it |
| `.../data` | The mesh history on Room: messages, nodes, channels, the cursor, the breaks |
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

## RIFT, read-only (M4.1)

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

## Build and test

```sh
source /c/Android/toolchain/env.sh
./gradlew :app:testDebugUnitTest          # unit tests and the lint
./gradlew :app:assembleDebug
```

Against the real gateway (WSL, with gcc, libssl-dev, libcjson-dev, python3):

```sh
wsl -- bash /mnt/c/Android/doorscompanion/harness/run-gateway.sh    # leave running
./gradlew :app:testDebugUnitTest -Ddoors.gateway.ctl=127.0.0.1:47232
```

The harness's meshcored is a fake written from `docs/api/mesh.md`
(`harness.mesh` on the control port: `seed`, `message`, `state`, `bulk`,
`restart`, `down`, `up`, `demo`). `MeshIntegrationTest` drives the real
gatewayd with it.

On an emulator the harness is at `10.0.2.2:47231`: take the URI from
`gateway.pair_begin` on the control port, replace `h=` with `10.0.2.2`, paste
it in the app, then send `gateway.pair_decide {"allow": true}`.

## Not yet

Sending to the mesh (M4.2), `mesh.path_preview`, capture (PacketShark), the optional background
connection service and its notification, more than one paired unit, and
anything measured on a K230 or a physical phone.
