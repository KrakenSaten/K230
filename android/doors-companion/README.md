# DOORS Companion (Android)

The phone side of the Doors Companion Gateway (ADR-019). Milestone M3: the
foundation. It pairs with one unit, keeps an authenticated TLS 1.3 session to
it while the app is in front, and shows the unit's status. RIFT and
PacketShark are navigation placeholders; nothing in them talks to the unit.

Application id `app.doors.companion`, min SDK 29, Kotlin and Jetpack Compose,
Apache-2.0. The contracts it is built to are in the Doors repository:
`docs/api/gateway.md`, `docs/design/companion/PAIRING.md` and `CONTRACTS.md`
section 2.

## Layout

| Path | What |
| --- | --- |
| `app/src/main/java/app/doors/companion/core` | No Android imports: the fingerprint, the pairing code, the pinned trust manager, the TLS handshake, framing, and the session loop (pair, reconnect, back-off, revocation). Runs on a host JVM, which is how it is tested against a real gatewayd |
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
- `SecurityLintTest` fails the build's tests if a second trust manager, a name
  verifier, an older protocol, cleartext, a build variant, a telemetry
  dependency or an extra permission appears.

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

On an emulator the harness is at `10.0.2.2:47231`: take the URI from
`gateway.pair_begin` on the control port, replace `h=` with `10.0.2.2`, paste
it in the app, then send `gateway.pair_decide {"allow": true}`.

## Not in M3

Mesh messaging (RIFT), capture (PacketShark), the optional background
connection service and its notification, more than one paired unit, and
anything measured on a K230 or a physical phone.
