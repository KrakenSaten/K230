# protocols/meshcore — the portable MeshCore protocol core

A static library, `libmeshcore.a`, holding the MeshCore protocol runtime and
the smallest Linux adaptation layer it needs. It is the protocol and nothing
else: no radio driver, no service, no IPC, no UI.

Nothing links it yet. It exists so that the MeshCore service, when it is
written, has a protocol layer that was ported once, deliberately, with tests —
rather than one assembled inside a daemon.

## What it owns

The MeshCore wire format and runtime, compiled from upstream unchanged:

| Compiled from `vendor/RIFT/src` | what it is |
| --- | --- |
| `Packet.cpp` | the wire frame: header, path, payload, packet hash |
| `Utils.cpp` | SHA-256, AES-128, encrypt-then-MAC, hex |
| `Identity.cpp` | Ed25519 keys and signatures, X25519 agreement |
| `Dispatcher.cpp` | receive/transmit scheduling, airtime budget, duty cycle |
| `Mesh.cpp` | payload types, routing, advert/datagram/ack/path construction |
| `helpers/StaticPoolPacketManager.cpp` | the fixed packet pool and the in/out queues |
| `helpers/AdvertDataHelpers.cpp` | advert app-data: node type, name, position |
| `helpers/TxtDataHelpers.cpp` | string and float helpers the above use |
| `helpers/BaseChatMesh.cpp` | contacts, directed messages, ACKs, return paths |

Header-only and also in the boundary: `MeshCore.h`, `helpers/SimpleMeshTables.h`
(the duplicate table), `helpers/ContactInfo.h`, `helpers/ChannelDetails.h`,
`helpers/UTF8Helpers.h`.

### Group channels

`MAX_GROUP_CHANNELS` is **defined**, so `BaseChatMesh`'s channel table and the
group send and receive paths are compiled. A channel in MeshCore is a
pre-shared key and nothing else:

| | |
| --- | --- |
| identity | a 16- or 32-byte secret. Two nodes are on the same channel when they hold the same bytes. |
| routing hint | one byte, `SHA-256(secret)[0]`, carried in the clear at the head of the frame (`Mesh.cpp:227`). Collisions are ordinary — a receiver tries up to four matching channels and lets the MAC decide (`Mesh.cpp:236-246`). |
| name | **local**. It is never transmitted, and two nodes on one channel routinely call it different things. |
| receive | `Mesh::onRecvPacket` → `searchChannelsByHash()` → `Utils::MACThenDecrypt` → `BaseChatMesh::onGroupDataRecv` → `onChannelMessageRecv()`. |
| send | `BaseChatMesh::sendGroupMessage()` → `Mesh::createGroupDatagram()` → `sendFlood()`. |
| sender identity | none. `sendGroupMessage()` writes `"<name>: "` into the *encrypted payload* (`BaseChatMesh.cpp:492`); nothing signs it. The name on a channel message is a claim. |
| acknowledgement | none. `PAYLOAD_TYPE_GRP_TXT` is flooded and unacknowledged: there is no `expected_ack`, no timeout and no delivery report. |

Two first-party files make that possible, both in `compat/`:

- **`mc_channels.h`** holds the `MAX_GROUP_CHANNELS` definition. It is a
  header rather than a `-D` because the macro decides `BaseChatMesh`'s
  *layout* (`channels[MAX_GROUP_CHANNELS]`), and two makefiles compile
  against that class — this library and `services/meshcored`. Given through
  `-D` in two places, a change to one would produce a library and a service
  that disagree about where every member after `channels` lives: a
  one-definition-rule violation the linker cannot see. It is reached the one
  way upstream guarantees — `BaseChatMesh.h:3` and `ChannelDetails.h:3` both
  include `<Arduino.h>` before expanding it, and `compat/Arduino.h` includes
  `mc_channels.h`. There is no include order in which a translation unit sees
  the class without first seeing the value.
- **`base64.hpp`**, because `BaseChatMesh.cpp:878` includes `<base64.hpp>`
  inside the channel guard and upstream satisfies it with an Arduino library
  it does **not** vendor (`densaugeo/base64 @ ~1.4.0`, `platformio.ini:180`).
  There is therefore no upstream source here to be faithful to. Ours differs
  from densaugeo's in two deliberate ways, both recorded in the file: it
  writes no terminator past the decoded bytes (densaugeo's `output[len] = 0`
  runs one byte past a full 32-byte `GroupChannel::secret`), and the
  capacity-less three-argument form caps at 32 bytes — the size of the only
  destination any caller in this boundary passes — instead of overrunning it.
  It is strict: a character outside the RFC 4648 alphabet is a refusal, not a
  zero byte, because a decoder that maps unknown characters to `0` turns a
  mistyped key into a *different* key that still appears to work.

`BaseChatMesh::addChannel()` is **not called** by this library or by
`services/meshcored`. Upstream's own firmware says why not
(`examples/companion_radio/MyMesh.h:343-347`): it writes at `num_channels`,
which counts only channels added through that method and stays 0 for channels
restored from storage, so it silently overwrites one. Channels are installed
through `setChannel()`, which derives the hash and takes a slot.

### Contact table

`MAX_CONTACTS` is **1000** here, not upstream's 32, set in
**`compat/mc_contacts.h`** and reached through `compat/Arduino.h` exactly as
`mc_channels.h` is, because it sizes `BaseChatMesh`'s contact table and so
its layout (`contacts[MAX_CONTACTS+MAX_ANON_CONTACTS]`, `BaseChatMesh.h:64`).
It is an `#error` rather than an `#ifndef` if anything else defines it. A
bigger table does not change addressing: MeshCore still matches an inbound
direct packet by a one-byte hash and tries at most eight contacts that share
it (`MAX_SEARCH_RESULTS`, `BaseChatMesh.h:12`), and the table is still a flat
array searched linearly. At 1000 contacts about four share each hash value
on average, so a ninth sharing one - whose direct messages cannot then be
matched - is rare but possible.

Crypto, from the two libraries MeshCore itself uses:

- `vendor/RIFT/lib/ed25519` (orlp's ref10 Ed25519): `keypair.c`, `sign.c`,
  `key_exchange.c`, `fe.c`, `ge.c`, `sc.c`, `sha512.c`. `seed.c` is left out —
  randomness comes from the host — and so are `verify.c`, which MeshCore does
  not call, and `add_scalar.c`, which nothing here calls.
- `vendor/Crypto/libraries/Crypto` (rweather): `Crypto.cpp`, `Hash.cpp`,
  `BlockCipher.cpp`, `AESCommon.cpp`, `AES128.cpp`, `SHA256.cpp`, `SHA512.cpp`,
  `BigNumberUtil.cpp`, `Curve25519.cpp`, `Ed25519.cpp`.

## What it explicitly does not own

Left out of the boundary on purpose, all present in the upstream tree:

- **Radio hardware.** `helpers/radiolib/*` (the SX1262, SX1268, SX1276, LLCC68,
  LR1110, LR2021 and STM32WL wrappers), RadioLib itself, SPI and GPIO.
  `mesh::Radio` stays an abstract interface, unchanged.
- **Board support.** `helpers/ESP32Board.*`, `NRF52Board.*`, `stm32/STM32Board.h`,
  `esp32/TBeamBoard.*`, `MeshadventurerBoard.h`, `RefCountedDigitalPin.h`,
  `ExternalWatchdogManager.h`, and every FreeRTOS or ESP-IDF call.
- **Display and input.** All of `helpers/ui/*` — the OLED, e-paper and ST77xx
  drivers, the T-Deck keyboard, touch, trackball and speaker, the buzzer,
  `UIScreen.h`, `DisplayDriver.h`.
- **RIFT's UI.** `examples/companion_radio/ui-rift/*` — `RiftLogic`,
  `RiftClock`, `RiftMutes`, `UITask`. This library is not RIFT.
- **Links other than the mesh.** `helpers/esp32/SerialBLEInterface.*`,
  `SerialWifiInterface.*`, `ESPNOWRadio.*`, `nrf52/SerialBLEInterface.*`,
  `ArduinoSerialInterface.*`, `MultiSerialInterface.h`, `helpers/bridges/*`,
  `helpers/ethernet/*`. The companion protocol is not needed by the protocol
  core and is not here.
- **Arduino storage.** `helpers/IdentityStore.*`, `stm32/InternalFileSystem.*`,
  and `SimpleMeshTables`' `#ifdef ESP32` save/restore, which needs `<FS.h>`.
  Persistence is the future service's business, over Doors' own storage.
- **Node configuration and CLI.** `helpers/CommonCLI.*`, `ConfigSerializer.*`,
  `ClientACL.*`, `TransportKeyStore.*`, `RegionMap.*`.
- **Sensors and RTC hardware.** `helpers/sensors/*`, `AutoDiscoverRTCClock.*`,
  `RTC_RX8130CE.*`.
- **Channel *policy*.** Group channels themselves are in (see above), but
  which channels a node holds, what they are called, whether they survive a
  restart and who may add one are not this library's business. It compiles
  the table and the two code paths; `services/meshcored` owns the rest.

## Upstream

| | |
| --- | --- |
| MeshCore protocol | `KrakenSaten/RIFT`, branch `rift-tdeck`, commit `3ca7e3f003bd270587b1d92e33ce999542dcb5b4` |
| Crypto | `rweather/arduinolibs`, commit `37a76b8f7516568e1c575b6dc9268da1ccaac6b6` |

Both are recorded in `vendor_rift_commit.txt` and `vendor_crypto_commit.txt`,
and four separate things check them — because until recently the lint could
report "pinned, clean" about a library that was neither:

1. **The Makefile validates the pins before it compiles anything.** Not
   alongside: `protocols/meshcore/build/vendor-id.stamp` is a real
   prerequisite of every object, so under `make -j` too, nothing is compiled
   until both checkouts have been checked.
2. **The stamp also carries the commits actually checked out**, so moving
   either tree to another revision rebuilds *every* object rather than the
   few files that happen to differ. A `libmeshcore.a` holding objects from
   two revisions of the protocol is not a thing this build can produce.
3. **`MESHCORE_ALLOW_UNPINNED=1` is recorded, not just permitted.** It remains
   a deliberate escape hatch for working against a different revision, but the
   stamp then says `pinned=no` and `tests/meshcore_lint.sh` refuses to call
   the result pinned.
4. **`tests/meshcore_lint.sh` checks the trees on disk**, not only the text
   files: each checkout is at its pinned commit, each compiled tree has no
   local edits and no added files, and the two pin files match
   `tools/meshcore-frame` — the tool whose frames passed the accepted P0
   on-air gate. If they ever diverge, that gate's evidence stops carrying over
   to this library, and the lint says so.

**Local divergence from upstream: none.** Not one vendored file is edited,
copied or patched. `tests/meshcore_lint.sh` checks that `vendor/RIFT/src`,
`vendor/RIFT/lib/ed25519` and `vendor/Crypto/libraries/Crypto` — the three
trees this library actually compiles — are at the pinned commits with no
local edits and nothing added, and that nothing under `protocols/meshcore/`
shadows a vendored filename. The Crypto tree is held to the same standard as
the protocol tree because AES-128, SHA-256, the HMAC and `Ed25519::verify` all
come from it: an edit there changes what goes on the air as much as an edit to
`Mesh.cpp` would. Everything the port needs is supplied from outside those
trees, through the headers MeshCore already includes.

`tests/meshcore_build_deps_test.sh` is what keeps those claims honest. It
builds out of tree against throwaway clones and then breaks each rule in turn
— a header changed, a tree moved off its pin, a Crypto source edited, a file
added, `make -j` against the wrong revision — and fails if the build or the
lint lets any of them through.

The two upstream checkouts are not vendored into this repository; they are
ignored clones, as `vendor/RadioLib` and `vendor/ggwave` are:

```sh
git clone https://github.com/KrakenSaten/RIFT.git vendor/RIFT
git -C vendor/RIFT checkout 3ca7e3f003bd270587b1d92e33ce999542dcb5b4
git clone https://github.com/rweather/arduinolibs.git vendor/Crypto
git -C vendor/Crypto checkout 37a76b8f7516568e1c575b6dc9268da1ccaac6b6
```

## Platform seams

MeshCore reaches its platform through four abstract classes it defines itself,
plus the Arduino `Print`/`Stream` pair. Implementations live in `port/` and
`compat/`; `port/mc_port.h` is the whole public surface.

| Seam | Implementation | Notes |
| --- | --- | --- |
| `mesh::MillisecondClock` | `mcport::MonotonicClock` | `CLOCK_MONOTONIC`. See below. |
| `mesh::RTCClock` | `mcport::SystemRTCClock` | `CLOCK_REALTIME` seconds. `setCurrentTime()` is accepted and ignored: the system clock is sysd's. `isSet()` reports whether time sync has happened. |
| `mesh::RNG` | `mcport::HostRNG` over `mcport::randomBytes()` | `getrandom(2)`, falling back to `/dev/urandom`. Radio noise is **not** an entropy source here. `randomBytes()` returns failure; `HostRNG::random()` cannot (the interface returns void) and aborts rather than handing MeshCore predictable bytes. |
| logging | `mcport::setLogSink()` / `logWrite()` | Levels mirror pocketlog's, in pocketlog's order, so a service wires the two together with a cast. Defaults to stderr. This library does **not** depend on `core/pocketlog`. |
| `Print` / `Stream` | `compat/Stream.h` | `MemStream` over a byte buffer, which is how a MeshCore `.id` file is read and written, plus a `Serial` over stderr. The default build leaves `MESH_PACKET_LOGGING` off, but `make CXXFLAGS="-O2 -DMESH_PACKET_LOGGING=1"` compiles and links cleanly (checked at this commit, not on every run), which is what `Print::print(int, base)` and `Print::printf()` are there for. |
| `<Arduino.h>` | `compat/Arduino.h` | The C library the four vendored includers actually want, plus `ltoa()`, which avr-libc has and glibc does not. No pin, bus, timer or RTOS API, and deliberately no `millis()`. |
| Crypto's global `RNG` | `compat/rng_host.cpp` | rweather's library declares `extern RNGClass RNG` and defines it over Arduino entropy sources. Supplied here over the host CSPRNG. |

### The clock is genuinely 64-bit

`mesh::MillisecondClock::getMillis()` returns `unsigned long`, and
`mesh::Dispatcher` compares two of them by casting the difference to signed
(`(long)(now - deadline) > 0`). On an ESP32 both types are 32 bits and that
cast is what survives the counter wrapping every 49.7 days. On riscv64 and on
the x86-64 host both are 64 bits.

A 32-bit counter widened into that 64-bit type would be **worse than the
ESP32**: it would still wrap at 2^32, and the signed difference would then be
a value near −2^32, which is not greater than zero — so every deadline set
before the wrap reads as *not yet reached*, and the dispatcher stops
transmitting instead of recovering. So the port uses a real monotonic 64-bit
millisecond value, `mc_port.h` `static_assert`s that `unsigned long` is at
least 64 bits, and `tests/meshcore_port_test.cpp` demonstrates both halves:
the real clock crossing 2^32 correctly, and a deliberately wrapping clock
failing in exactly that way.

### …but not every deadline in the library is

Worth stating plainly, because "the clock is 64-bit" invites the conclusion
that nothing inside can wrap, and that is not what it means. Three separate
widths are in play:

- **The platform clock is a genuine 64-bit monotonic millisecond value.**
  `mcport::MonotonicClock` reads `CLOCK_MONOTONIC`; it does not wrap, does not
  go backwards and is not affected by the wall clock being set.
- **`mesh::Dispatcher`'s own long-lived deadlines are `unsigned long`**, which
  on riscv64 and on the x86-64 host is 64 bits — `next_tx_time`,
  `outbound_expiry`, `next_floor_calib_time`, the duty-cycle window
  (`Dispatcher.h:120-130`), all compared through
  `millisHasNowPassed()`. Those really do inherit the full range.
- **`mesh::PacketManager`'s queue scheduling is `uint32_t` on both sides** and
  is not affected by any of that. `queueOutbound(..., uint32_t scheduled_for)`,
  `queueInbound(..., uint32_t scheduled_for)`, `getNextOutbound(uint32_t now)`
  and `getNextInbound(uint32_t now)` all truncate, and `PacketQueue` compares
  what is left with `(int32_t)(_schedule_table[j] - now) > 0`
  (`StaticPoolPacketManager.cpp:16` and `:26`).

That last one is **correct, not broken**: the deadline is truncated going in,
"now" is truncated the same way coming out, and the signed difference is
modular arithmetic that stays right across the 2^32 boundary for any interval
under about 24.8 days. Nothing in this library comes near it: the inbound
delay is capped at `MAX_RX_DELAY_MILLIS`, 32 seconds
(`Dispatcher.cpp:11`), and a retransmit delay is carried in the low 24 bits
of a `DispatcherAction` (`Dispatcher.h:108`), about 4.7 hours at the absolute
most. The only unbounded way in is a caller passing a large `delay_millis` to
`Mesh::sendFlood()`, `sendDirect()` or `sendZeroHop()`, and nothing here does.

The point is only that a 64-bit platform clock does not remove every 32-bit
scheduling window from the library; it removes the dispatcher's. A future
service that queued a packet further ahead than ~24.8 days would be relying on
a range this interface does not have, and would need to say so rather than
assume the clock underneath it settles the matter.
`tests/meshcore_port_test.cpp` covers the outbound queue across that boundary,
and the delayed inbound queue on the other side of it.

## Relationship to radiod

There is no radio here, and no connection to `services/radiod`. The intended
shape:

```
services/radiod/          generic SX1262 / LoRa hardware service (unchanged by this work)
        ↓ IPC
MeshCore service          a future process: protocols/meshcore + a radiod adapter
        ↓ IPC
apps/rift/                a future presentation layer
```

`mesh::Radio` is left exactly as upstream declares it. The real adapter will
implement it over radiod's IPC and live in that service, not in this library —
which is also why radiod is not being shaped around MeshCore: a Meshtastic
protocol service could sit on the same radiod without either knowing about the
other.

## Building and testing

From the top of the repository:

```sh
make meshcore-core         # libmeshcore.a
make meshcore-core-test    # the three suites, plain then sanitised, then the
                           # lint, then the build-integrity check
make meshcore-core-riscv64 CROSS=/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-
```

It is not part of `make all` or `make test`, for the same reason
`tools/meshcore-frame` is not: it needs two upstream checkouts a Doors build
does not, and nothing links it yet. It installs nothing and reaches no image.

Both the host and riscv64 builds are clean with `-Wall -Wextra` and no
first-party warnings. The vendored trees are compiled as their authors wrote
them and reached with `-isystem`, so their warnings do not drown ours.

### Test coverage

| Suite | Checks | Covers |
| --- | --- | --- |
| `tests/meshcore_core_test.cpp` | 129 | packet encode/decode (flood, direct with path, transport codes), `path_len` bit packing, the duplicate table, hex, UTF-8 truncation, Ed25519 sign/verify and tamper rejection, X25519 agreement, AES-128 + MAC round trip, every single-bit MAC and ciphertext corruption rejected, the zero-length seal boundary, SHA-256 against the published vector, advert app-data, the base64 channel-key decoder (RFC 4648 vectors, every refusal, the capacity, the absent terminator) |
| `tests/meshcore_port_test.cpp` | 110 | the monotonic clock, dispatcher timing at 2^32−1 / 2^32 / 2^32+1, the outbound queue across the same boundary, the widened-32-bit failure demonstration, the host RNG — both sources, the fallback, the interrupted, short and zero returns, and both failing — the wall clock, the log sink, a full mesh node running a loop with no hardware, and the delayed inbound queue |
| `tests/meshcore_smoke_test.cpp` | 111 | two nodes over an in-memory air: signed ADVERT both ways, forged advert rejected, flood text A→B, PATH+ACK back, a second **directed** text and its ACK, and a third node that has heard the adverts, holds real contact keys for both, captures the directed frame off the air and still cannot open it. Then **channels**: the hash derived from the key and not the name, a message across, the sender not hearing itself, an identical frame deduplicated, a listener with a real key of its own excluded, a deliberately colliding one-byte hash refused by the MAC, a node holding both colliding keys opening it once under the right one, the empty-slot debt and its guard, a 128-bit key and the two derivations that differ — and a direct message still working afterwards |
| `tests/meshcore_lint.sh` | 19 | the boundary itself, statically: no LVGL/DRM/GPIO/RadioLib/SPI/RTOS symbol demanded, no Arduino timing call, `clock_gettime` actually used, no test hook in the shipped library, no vendored file edited, added or shadowed **in either tree**, both checkouts at their pinned commits, the build not having bypassed the pin, pins matching `meshcore-frame`, nothing installed |
| `tests/meshcore_build_deps_test.sh` | 35 | the build itself, by building: vendored headers reaching the dependency files across both trees, a changed header rebuilding what included it, a moved checkout rebuilding everything, `make -j` compiling nothing before the pins are validated, and the lint failing on each vendor mutation in turn |

The first four run twice, plain and under ASan + UBSan; the build-integrity
one runs once, since it drives its own builds. Every test uses the real
crypto; there is no mock AES or mock SHA-256 anywhere. That is a deliberate
departure from upstream's own native test environment, which builds with
`-I test/mocks` where `AES128::encryptBlock()` has an empty body and `SHA256`
is an xor-and-rotate toy — useful for testing `Packet` without a crypto
library, useless for asking whether a MAC rejects a forgery.

Tests ported from upstream's googletest suites, assertion for assertion, onto
this repository's `check()` harness: `test_path_len` (6 cases), `test_mesh_tables`
(8), `test_utils` (5), `test_utf8_helpers` (8). Upstream's remaining native
cases are out of this boundary: `test_rift_logic` (331 cases) tests
`examples/companion_radio/ui-rift/RiftLogic.h`, which is RIFT's UI;
`test_rift_air_log`, `test_rift_cli_secret` and `test_config_serializer` test
companion, CLI and config-storage code that is not compiled here; and
`test_base64_key` and `test_kiss_modem` test libraries this library does not
build.

### Sanitizer exemptions

Two, both narrow and both about vendored code:

1. `-fno-sanitize=shift-base` for `vendor/RIFT/lib/ed25519` only. orlp's ref10
   Ed25519 does its field and scalar arithmetic by left-shifting signed 64-bit
   limbs that are often negative — formally undefined, universally an
   arithmetic shift, and what runs on a real MeshCore node. About forty sites
   in `sc.c` and `fe.c`, nowhere else in the build. Same exemption, same files,
   as `tools/meshcore-frame`.
2. `lsan.supp`, suppressing exactly two constructors in
   `vendor/RIFT/src/helpers/StaticPoolPacketManager.cpp`. See that file.

Everything else, ours and vendored, keeps the full address and
undefined-behaviour checks.

## Known hardening debt

Recorded, not fixed. P1A is a faithful baseline; changing upstream's behaviour
is separate work, and silently changing it while claiming a faithful port
would be the worst of both. Each item has a regression test asserting **what
the code does today** — when a fix lands, those tests are expected to fail and
should be rewritten to assert the fixed behaviour.

1. **`Packet::readFrom()` reads past a short buffer.**
   `vendor/RIFT/src/Packet.cpp:65-85` consumes the header, four
   transport-code bytes and `path_len`, then `memcpy`s up to 189 bytes of
   path, all before its first comparison of `i` against `len` (line 80). A
   caller passing a buffer shorter than the packet the bytes describe gets an
   out-of-bounds read and only then `false`.
   Reachable in this boundary through `BaseChatMesh::importContact()`
   (`BaseChatMesh.cpp:560`), which passes a caller-supplied length straight
   through. **Not** an over-the-air path: `Dispatcher::tryParsePacket()` does
   its own bounds-checked parse and does not call `readFrom()`.
   Tests: `meshcore_core_test.cpp`, `test_debt_readfrom_short_buffer`.

2. **PATH `extra_len` underflow.**
   `vendor/RIFT/src/Mesh.cpp:172`, `uint8_t extra_len = len - k;`. Nothing
   checks `k <= len`. A decrypted PATH payload that declares a longer path
   than it carries makes the subtraction negative, and the `uint8_t`
   truncation turns it into a large positive length handed to
   `onPeerPathRecv()` with a pointer near the end of the buffer. A crafted
   payload reaches `extra_len = 207` over a 184-byte buffer.
   Requires a valid MAC, so the sender must be an already-agreed contact —
   which is why it is debt and not an emergency.
   Tests: `meshcore_smoke_test.cpp`, `test_debt_path_extra_len_underflow`,
   which crafts the payload and encrypts it with the real shared secret, so it
   drives the genuine code path.

3. **`Utils::fromHex()` validates length only.**
   `vendor/RIFT/src/Utils.cpp:218-229` checks that the string is
   `dest_size * 2` characters and nothing else; a non-hex character becomes
   `0` and the call still returns `true`. `Utils::isHexChar()` exists beside
   it for callers to validate with, so this may be deliberate — but a caller
   that trusts the bool gets bytes nobody typed.
   Tests: `meshcore_core_test.cpp`, `test_hex`.

4. **`StaticPoolPacketManager` and `PacketQueue` have no destructors.**
   Both allocate with `new` and never free. Harmless in the shape MeshCore was
   written for and in the shape a service will use it — one manager for the
   life of the process, a fixed pool — but it means a packet manager cannot be
   recycled at runtime. Suppressed for LeakSanitizer; see `lsan.supp`.

5. **An unused channel slot is a channel whose key is 32 zero bytes.**
   `BaseChatMesh::searchChannelsByHash()`
   (`vendor/RIFT/src/helpers/BaseChatMesh.cpp:367-376`) walks all
   `MAX_GROUP_CHANNELS` slots and compares hash bytes. A slot with no channel
   in it is all zeroes, so its hash byte is `0` and its secret is 32 zero
   bytes — and any frame whose channel-hash byte is `0` therefore matches
   every free slot and is offered to that key. The key is not a secret, so
   anybody can build such a frame, and an unguarded node accepts the message
   on a channel it never joined.
   Upstream's firmware does not meet this because its slot 0 always holds the
   public channel and its screens skip empty slots; a service whose table
   starts empty meets it on the first frame. `services/meshcored` overrides
   `searchChannelsByHash()` to offer only occupied slots.
   Tests: `meshcore_smoke_test.cpp`, `test_channels` — the fault on an
   unguarded subclass and the fix on a guarded one, side by side.

6. **`setChannel()` guesses the key length from its content.**
   `vendor/RIFT/src/helpers/BaseChatMesh.cpp:896-906` hashes over 16 bytes
   when `secret[16..31]` are all zero and over 32 otherwise, so a genuine
   256-bit key whose upper half happens to be zero is hashed as a 128-bit one
   — and `addChannel()`, which uses the *decoded* length instead, would hash
   the same key differently. Two nodes could then derive different hashes for
   one key and never route to each other. `services/meshcored` refuses such a
   key rather than joining a channel its peers may hash differently.
   Tests: `meshcore_smoke_test.cpp`, `test_channels`.

7. **`BaseChatMesh::onAdvertRecv()` never sets `is_new`.**
   `vendor/RIFT/src/helpers/BaseChatMesh.cpp:154` declares `bool is_new = false`
   and line 198 passes it to `onDiscoveredContact()` without it ever being
   assigned, so a contact added for the first time is reported as not new.
   A UI-notification quirk rather than a protocol fault, and upstream's to
   decide — noted so that whoever writes the service does not build on the
   flag. `meshcore_smoke_test.cpp` deliberately does not depend on it.

Copyright (c) 2026 PocketOS authors. Licensed under the Apache License, Version 2.0 (see LICENSE at the repository root).
