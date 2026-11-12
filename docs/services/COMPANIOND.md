# companiond: a MeshCore phone app over Wi-Fi (prototype)

Status: **prototype, host-tested only.** Built by name (`make companiond`),
not in `make all`, not installed into the image, no init script. Nothing on a
unit runs it unless somebody starts it by hand. **No part of it has run on a
K230 or against the official MeshCore app**; see "Evidence" and the hardware
acceptance procedure at the end.

companiond lets the official MeshCore phone app use a Doors unit the way it
uses a MeshCore companion radio flashed with upstream's Wi-Fi firmware: the app
connects over TCP on the local network and sees **this node** - its identity,
its contacts, its channels, its messages. RIFT keeps working beside it.

```
SX1262 -> radiod -> [radio.*] -> meshcored -> [mesh.*] -> RIFT
                                         \--> [mesh.*] -> companiond -> TCP -> MeshCore app
```

meshcored remains the only owner of the radio, the identity, the contacts and
the channels. companiond is one more pocketipc subscriber, like RIFT: it opens
no radio, links no MeshCore source, holds no key and has no identity of its
own.

## Which client, and the pinned protocol

| | |
| --- | --- |
| Client | The official **MeshCore** app (developer Liam Cottle), the one RIFT's docs already refer to. The owner uses it on **both Android and iOS**. It is closed source. |
| Its TCP support | **DOCUMENTED, not tested.** The App Store release notes record a Wi-Fi connection type: connection history saved (1.36.0, 2025-12-27), "auto reconnect on wifi connections" (1.45.0), the connection type shown in settings and a Wi-Fi settings screen (1.51.0). The firmware it is made for listens on TCP port 5000 (`TCP_PORT`, upstream `examples/companion_radio/main.cpp`). Nothing here says the Android and iOS builds differ in this; nothing here has checked that they do not. |
| Framing and commands | **meshcore-dev/MeshCore @ `81dbae592ffad40b4212bc0b37e82b38da08f4bc`** (companion firmware v1.18.0, `FIRMWARE_VER_CODE` 14): `src/helpers/wifi/SerialWifiInterface.cpp` (framing), `src/helpers/BaseSerialInterface.h` (`MAX_FRAME_SIZE` 176), `examples/companion_radio/MyMesh.cpp` (commands, replies, pushes), `docs/companion_protocol.md`. |
| Client behaviour reference | **meshcore-dev/meshcore.js @ `26193c059fcd1d0524f1ad5b19fd88287ecc5c61`**, the official JavaScript library by the same author as the app: `src/connection/tcp_connection.js` (the same framing, resynchronising on a bad header byte), `connection.js` (`onConnected` sends `DEVICE_QUERY` first; `getChannels` asks slot after slot **until the first error**). |
| Not the reference | `vendor/RIFT` (KrakenSaten/RIFT 0.9.5) is a fork; its `SerialWifiInterface.cpp` is byte-identical to the pinned upstream one apart from line endings, but the pin above is upstream's. |

The app's own startup sequence cannot be read from its source. What is
implemented is the sequence upstream documents (`companion_protocol.md`:
`APP_START`, `DEVICE_QUERY`, `SET_DEVICE_TIME`, `GET_CONTACTS`, `GET_CHANNEL`
for each slot, `SYNC_NEXT_MESSAGE`) and meshcore.js performs. Anything the app
sends beyond it is answered as below, and the acceptance procedure is where it
is found.

## The wire

Each frame is a type byte, a 16-bit little-endian length and the payload. The
app sends `<` (0x3c), companiond sends `>` (0x3e). Exactly as upstream:

- a frame of another type, or longer than 176 bytes, is read past by its
  length and dropped; a header whose type or length is zero is three bytes
  dropped (`hasReceivedFrameHeader`);
- frames split across TCP segments and several frames in one segment are
  both handled; the decoder never holds more than one 176-byte frame, however
  long a length the peer announces (`tests/companiond_wire_test.c`, every
  split from one byte at a time to the whole stream, and a 64 KiB announcement);
- commands are taken one at a time: while one waits for meshcored, the next is
  not read, so replies keep the order of the commands. Pushes (0x80 and up)
  may come between them at any time, as upstream's do.

## What is supported

| Command | Answer | From |
| --- | --- | --- |
| `DEVICE_QUERY` (22) | `DEVICE_INFO`: version code 14; max contacts 255 (510 - a byte cannot say meshcored's 1000); max channels = `mesh.channels` `max`; BLE PIN 0; build date; model `LILYGO T-Display K230 (Doors)`; version `Doors <meshcored version>`; client repeat 0 (meshcored forwards nothing); path hash mode = `mesh.path_hash` bytes - 1 | `mesh.channels`, `mesh.path_hash` |
| `APP_START` (1) | `SELF_INFO`: chat node; the configured TX power, of 22 dBm maximum (SX1262, DOCUMENTED); the public key; location 0,0 (this node's adverts carry none); multi-ACKs 0; no location in adverts; telemetry off; contacts added automatically (meshcored keeps every advert); frequency, bandwidth, SF and CR of the configured profile; the name | `mesh.identity`, `mesh.info` `requested_profile` |
| `GET_DEVICE_TIME` (5) | the system clock, read | - |
| `GET_CONTACTS` (4) | `CONTACTS_START` (count), one `CONTACT` per node, `END_OF_CONTACTS` (newest lastmod). Written as the socket drains, never all at once | `mesh.nodes` |
| `GET_CONTACT_BY_KEY` (30) | `CONTACT`, or `ERR NOT_FOUND` | `mesh.node` |
| `GET_CHANNEL` (31) | `CHANNEL_INFO` for every slot below `max` (an empty slot as upstream sends one: no name, zero secret); `ERR NOT_FOUND` past the table | `mesh.channels` |
| `SYNC_NEXT_MESSAGE` (10) | the oldest held message (`CONTACT_MSG_RECV(_V3)` / `CHANNEL_MSG_RECV(_V3)`, V3 when the app asked for protocol 3 or later), or `NO_MORE_MESSAGES` | the queue |
| `SEND_TXT_MSG` (2), plain text | `SENT` (flood flag, a tag, meshcored's ACK timeout), later `SEND_CONFIRMED` with that tag when the ACK matches | `mesh.send` `to` / `resend` |
| `SEND_CHANNEL_TXT_MSG` (3) | `OK`, and nothing after: a channel message is an unacknowledged flood (upstream answers the same) | `mesh.channels`, `mesh.send` `channel` |
| `GET_DEFAULT_FLOOD_SCOPE` (64) | none (one byte): meshcored writes no transport codes | - |
| `EXPORT_PRIVATE_KEY` (23), `IMPORT_PRIVATE_KEY` (24) | `DISABLED`, which is what upstream answers when either is compiled out | - |
| **Everything else** | `ERR UNSUPPORTED_CMD` | - |

"Everything else" includes, deliberately, every command that changes the node:
`SET_DEVICE_TIME` (6), `SET_ADVERT_NAME` (8), `SET_ADVERT_LATLON` (14),
`SET_RADIO_PARAMS` (11), `SET_RADIO_TX_POWER` (12), `SET_TUNING_PARAMS` (21),
`SET_OTHER_PARAMS` (38), `SET_DEVICE_PIN` (37), `SET_PATH_HASH_MODE` (61),
`SET_CHANNEL` (32), `ADD_UPDATE_CONTACT` (9), `REMOVE_CONTACT` (15),
`RESET_PATH` (13), `IMPORT_CONTACT` (18), `SET_DEFAULT_FLOOD_SCOPE` (63),
`SET_AUTOADD_CONFIG` (58), `SET_FLOOD_SCOPE_KEY` (54), `REBOOT` (19),
`FACTORY_RESET` (51), the signing commands (33-35) and `RUN_CLI_COMMAND` (66) -
and every one that would transmit something other than a message: adverts (7),
sharing a contact (16), raw data and packets (25, 65), logins, status,
telemetry, binary, path-discovery and anonymous requests (26, 27, 39, 50, 52,
57), traces (36), control data (55), channel datagrams (62), CLI text to a
repeater (`SEND_TXT_MSG` with a type other than plain). **None is ever
answered OK**, and none reaches meshcored (`tests/companiond_e2e_test.py`
checks the fake service's method log). The clock is the one the app tries to
set on every connect: it belongs to Doors (NTP and Settings), so the app is
told the command is unsupported. Whether the app carries on after that is the
first thing the acceptance procedure checks.

Pushes sent: `MSG_WAITING` (0x83) when a message is queued while an app is
connected; `SEND_CONFIRMED` (0x82); `ADVERT` (0x80), `PATH_UPDATED` (0x81) and
`CONTACT_DELETED` (0x8F) from meshcored's `mesh.node` events (`discovered`,
`path`, `removed`), so the app refreshes the contact.

## Delivery semantics

What the app is told is what meshcored knows, and nothing more:

- **Direct message.** `SENT` means meshcored accepted it (its `sent_flood` /
  `sent_direct`), exactly as upstream's `SENT` means the firmware queued it. The
  four-byte tag in it is companiond's own, not MeshCore's ACK CRC (meshcored
  does not report that); the app only matches it against `SEND_CONFIRMED`, and
  `SEND_CONFIRMED` is sent **only** when meshcored reports the message `acked`,
  with the round trip from meshcored's ACK time. `no_ack` produces nothing, as
  upstream produces nothing: the app's own timer, from the timeout in `SENT`
  (meshcored's `ack_timeout_ms`), runs out.
- **The app's retry.** The app resends with the same timestamp and text and the
  attempt one higher. That becomes meshcored's `resend` of the same message -
  same text, attempt one higher, a fresh ACK wait - and a new tag. A late ACK
  for any attempt confirms the newest tag. A retry meshcored will not do (the
  message is still waiting for its ACK, or already acked) is `ERR BAD_STATE`.
- **A frame sent again.** The last 16 messages the app sent are remembered by
  recipient (or slot), app timestamp and text. The same frame again - the app
  reconnecting and repeating a command whose reply it never saw - is answered
  from the record and **not transmitted again**. A retry from before the
  record (another run of companiond, or more than 16 sends ago) is sent as a
  new message, as upstream would.
- **Channel message.** `OK` and nothing after, ever: there is no ACK in the
  protocol. Upstream silently truncates a long one; companiond refuses one
  longer than the channel's `text_limit` with `ERR ILLEGAL_ARG`.
- **Refusals.** Text that is not well-formed UTF-8, holds a NUL or a control
  character other than newline and tab, or is over 160 bytes: `ILLEGAL_ARG`,
  never sent. No node matching the six-byte prefix (or more than one: meshcored
  refuses an ambiguous prefix rather than guess): `NOT_FOUND`. An empty channel
  slot: `NOT_FOUND`. meshcored busy, radio not available, or eight direct
  messages already waiting for ACKs: `TABLE_FULL`, upstream's "unable to send".
  meshcored not connected, or not answering within 5 s: `BAD_STATE`.
- **The timestamp.** meshcored stamps outgoing messages with its own clock; the
  recipient sees that, not the phone's. The app's timestamp is used only to
  recognise a repeated frame.

## Reconnects, the queue and RIFT

- Received messages are queued from meshcored's `mesh.message` events **whether
  or not an app is connected**, and the queue survives the app disconnecting
  and reconnecting. Each is delivered once, by `SYNC_NEXT_MESSAGE`. A message
  raised twice by meshcored is queued once.
- The queue holds **64** messages (upstream: 16). When full, the oldest channel
  message makes room, as upstream's does; with none, the oldest direct message
  does (upstream drops the new one instead). Every eviction is logged.
- The queue is memory only: stopping companiond loses what it held, and a
  restarted companiond does not refill it from `mesh.messages`. A message
  received while companiond was not running is not offered to the app.
- **RIFT is not affected.** meshcored broadcasts every event to every
  subscriber; companiond's subscription takes nothing from RIFT's, and RIFT's
  history (`mesh.messages`) is not drained or changed. What the phone sends,
  RIFT shows, because meshcored records it. What RIFT sends the app does not
  show (upstream tells an app only about its own sends). companiond reads
  meshcored continuously, so pocketipc's backpressure rule never has cause to
  drop it.
- meshcored restarting: companiond reconnects with a 0.5 s to 30 s backoff,
  answers `BAD_STATE` meanwhile, and on a new `run_id` forgets its send records
  (a new run's message ids start at 1 again); the queue keeps what it holds.
- A second connection from an allowed address **replaces** the first, as
  upstream's `SerialWifiInterface` does: a phone that changed networks leaves a
  dead socket behind, and refusing the new one would lock its owner out until
  TCP keepalive (60 s idle + 3 x 10 s) noticed.
- An app that stops reading is dropped when 64 KiB of output is waiting or
  nothing has drained for 10 s; a contact listing is written only as the
  socket drains, so a long one does not count against that.

## Security: what this exposes, and what limits it

**The MeshCore companion protocol over TCP has no authentication and no
encryption.** Upstream's Wi-Fi firmware has none either, and the app has
nothing to answer a challenge with, so companiond invents none. Anyone who can
open a TCP connection to the port, from an address it accepts, can:

- read every received message companiond is holding (and then they are gone
  from the queue for the owner's phone);
- send direct and channel messages **as this node**, on the air;
- list the node's contacts and channel names, and learn its public key.

On the network, every byte - message text, contacts - travels in clear text;
Wi-Fi encryption protects it only from outside the network, not from other
members of it.

What limits it, all of which a compatible client is unaffected by:

- **Off unless started.** Not installed, no init script.
- **One named address.** `--listen ADDR:PORT` is required, and `0.0.0.0` and
  public addresses are refused: the service is on one interface the operator
  chose, not on every interface the unit has or later gets.
- **Allowed networks.** `--allow CIDR` is required (up to 8). A connection from
  anywhere else is closed at once and logged. Independently of the list, only
  loopback, RFC 1918 and link-local sources are ever accepted, so a router port
  forward or a mistyped list does not put the service on the internet.
- **One client.** One connection at a time (a new one replaces it).
- **Nothing that changes the node.** Every setting, clock, radio, identity,
  key, contact and channel command is refused (above). The private key cannot
  be exported.
- **Channel keys are withheld.** meshcored reports no channel key to anyone
  (docs/api/mesh.md), and companiond does not ask for one. The well-known
  Public channel's key is public and is sent; **for every other channel the
  16-byte secret in `CHANNEL_INFO` is sent as zeros**. That is not the key: an
  app that shares such a channel (QR code, link) shares nothing usable, and the
  owner must not do it from the app. Sending and receiving on the channel work,
  because both go by slot. Whether the app does anything else with the secret
  is not known (closed source) - see "Open questions".

## Known limitations

- **Received route unknown.** meshcored does not report the route a received
  message took, so the path-length byte of every received message is `0xFF`,
  which the app may show as "direct". A fix is an additive `mesh.message` field
  in meshcored.
- **SNR unknown is 0.** The frame has no "unknown"; a message whose SNR radiod
  did not report goes as 0 dB.
- **`GET_CONTACTS` ignores `since`** and always sends the whole list:
  meshcored keeps no modification time. A contact's `lastmod` is when this node
  last heard it (monotonic, put on the system clock), or 0 if not heard since
  meshcored started.
- **No battery or storage report** (`GET_BATT_AND_STORAGE` is unsupported).
  sysd has a battery reading; wiring it in is a follow-up.
- **Not in the image.** Shipping it means an init script (disabled by default,
  like S65meshcored), `install`, and `tools/release/check_rootfs.sh` and
  `tests/image_contents_test.sh` coverage. That waits for acceptance.

## Open questions for the owner

1. **Private channel keys.** Should the phone hold the real keys of the node's
   private channels (as it would with upstream firmware)? That needs a
   meshcored method that discloses a key - against the rule in
   docs/api/mesh.md - and sends shared secrets in clear text over the LAN to
   any allowed client. The prototype says no.
2. **The clock.** If the app refuses to work when `SET_DEVICE_TIME` is
   unsupported, the smallest answer is to reply `OK` only when the phone's time
   is within a few seconds of the unit's (setting nothing). Not done: it is
   still an OK for something not done.

## Running it (bench only)

Build for the unit with the pinned toolchain, as for any Doors binary
(docs/BUILD_ENVIRONMENT.md), from a copy of the tree:

```sh
make CC=<cross>gcc CFLAGS="--sysroot=$SYSROOT -O2 -Wall -Wextra" LDFLAGS="--sysroot=$SYSROOT" companiond
```

On a unit with meshcored running, with the unit's Wi-Fi address and network:

```sh
ip -4 addr show wlan0                 # e.g. 192.168.1.50/24
/tmp/companiond --listen 192.168.1.50:5000 --allow 192.168.1.0/24
```

It logs to `/var/lib/pocketos/log/companiond.log`: connections, refusals,
sends, duplicates answered, evictions. It never logs message text, keys or
contacts. Stop it with Ctrl-C or SIGTERM.

## Evidence

| Claim | Class |
| --- | --- |
| Framing: split, combined, wrong-direction, oversized, empty frames; UTF-8 and text rules | VERIFIED host: `tests/companiond_wire_test.c` (plain and ASan/UBSan) |
| Startup sequence, contacts, channels, queue, sends, ACKs, retries, duplicates, refusals, reconnect, replacement, meshcored restart, access rules | VERIFIED host: `tests/companiond_e2e_test.py` - the real companiond (plain, then built with ASan/UBSan) between `tests/fake-meshcored` and a client written from the pinned framing |
| RIFT coexistence | VERIFIED host: a second pocketipc subscriber on the same fake meshcored receives every event companiond does |
| riscv64 build | VERIFIED: compiles warning-free with the pinned Xuantie 14.1.1 toolchain against the SDK sysroot |
| The official app connects, completes its startup and works | **not tested** |
| meshcore.js or meshcore_py against companiond | **not tested** |
| Anything on a K230, anything on the air | **not tested** |

`make companiond-test` runs all of the host checks.

## Hardware acceptance procedure

On one unit with meshcored running and RIFT open, the phone on the same Wi-Fi.
Parts A and B transmit nothing; part C transmits and needs the owner's go-ahead
and a peer node.

**A. Connect (no transmission).**
1. Start companiond as above. In the app: Connect, Wi-Fi tab, the unit's
   address, port 5000. PASS: the app reaches its main screen showing the
   unit's name and public key prefix; companiond's log shows the connection
   and `refused SET_DEVICE_TIME`. FAIL here = the clock answer; stop and report
   what the app shows.
2. Contacts: the app's list matches RIFT's CONTACTS (names, count).
3. Channels: Public and every RIFT channel appear in the same slots; no
   phantom channels after the last one.
4. Settings screens: change nothing. Try one setting (e.g. the name): the app
   reports a failure; RIFT and `doors call meshcored mesh.identity` show it
   unchanged.

**B. Receive (no transmission by this unit).** Have a peer send one direct
message and one Public message.
5. Both appear in RIFT and in the app, once each.
6. Disconnect the app (airplane mode), have the peer send another, reconnect:
   it arrives in the app once, and was in RIFT all along.

**C. Send (transmits; owner's go-ahead).**
7. Direct message from the app to the peer: delivered on the peer; the app
   shows it delivered when the ACK returns; RIFT shows it sent from this node.
8. Public channel message from the app: received on the peer; RIFT shows it.
9. Toggle Wi-Fi off and on while a message is pending: no second copy arrives
   on the peer for one press of Send.

Record for each: app version, platform (Android and iOS separately), PASS/FAIL,
and companiond's log lines - not message text.
