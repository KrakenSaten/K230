# meshcore-frame

A host-side tool that builds and takes apart **MeshCore wire frames**, using
the real MeshCore protocol code and the real crypto under it.

It exists for one job: to make K230-to-MeshCore interoperability testable. The
hex it prints is what `pos radio send <hex>` transmits, and the hex radiod
reports in a `radio.rx` event is what it parses. Between those two the tool is
the reference: if a frame it built is not accepted by a MeshCore node, the
disagreement is in the radio profile or the protocol source it was built from,
not in a hand-rolled encoder.

It is **not** an SX1262 tool, it is **not** RIFT's user interface, and it
touches no hardware, no daemon and no socket. It reads a key file, does
arithmetic, and writes to stdout.

## Where the protocol comes from

Nothing about the wire format is reimplemented here. The frames are built and
read by vendored upstream sources:

| What | From | Pinned at |
| --- | --- | --- |
| Framing, path packing, packet hash | `vendor/RIFT` `src/Packet.cpp` | `vendor_rift_commit.txt` |
| Cipher, MAC, SHA-256, hex | `vendor/RIFT` `src/Utils.cpp` | same |
| Ed25519, X25519, identity files | `vendor/RIFT` `src/Identity.cpp` | same |
| Advert app-data | `vendor/RIFT` `src/helpers/AdvertDataHelpers.cpp` | same |
| Ed25519 signing, key pair, ECDH | `vendor/RIFT` `lib/ed25519/` | same |
| AES-128, SHA-256/512, Ed25519 verify | `vendor/Crypto` (rweather/arduinolibs) | `vendor_crypto_commit.txt` |

`vendor/Crypto` is the library MeshCore's own `library.json` names
(`rweather/Crypto ^0.4.0`); MeshCore will not build without it, and neither
will this.

The payload layouts the builders use - what goes into an ADVERT, a TXT_MSG and
an ACK, and in what order - are the ones in `Mesh::createAdvert`,
`BaseChatMesh::composeMsgPacket` through `Mesh::createDatagram`, and
`Mesh::createAck`. Each builder in `mcf_frame.cpp` names the function it
mirrors. The rest of `Mesh` - the dispatcher, the routing tables, the radio,
the packet pool - is not vendored, because none of it decides what a frame
looks like.

`meshcore-frame version` prints both pinned commits, so a bench report can
quote the protocol source it really used.

## Prerequisites

Two ignored checkouts, the same convention `vendor/RadioLib` and
`vendor/ggwave` already follow. From the top of the repository:

```sh
git clone https://github.com/KrakenSaten/RIFT.git vendor/RIFT
git -C vendor/RIFT checkout "$(cat tools/meshcore-frame/vendor_rift_commit.txt)"
git clone https://github.com/rweather/arduinolibs.git vendor/Crypto
git -C vendor/Crypto checkout "$(cat tools/meshcore-frame/vendor_crypto_commit.txt)"
```

The build checks both against the pins before it compiles anything and refuses
to go on if they differ; `MESHCORE_FRAME_ALLOW_UNPINNED=1` overrides that and
says so.

## Build and test

```sh
make meshcore-frame        # the tool
make meshcore-frame-test   # its tests, plain and then under ASan/UBSan
```

It is deliberately outside `make all`, `make test` and `make install`: nothing
it produces is installed or reaches an image, and a Doors build should not
start depending on two more upstream checkouts for the sake of a bench tool.
Its build outputs are git-ignored like every other one in the repository.

## Commands

```
identity new <file>        create a MeshCore identity (96-byte .id, mode 0600)
identity show <file>       print its public key and node hash

advert --key <file> [--name <text>] [--type chat|repeater|room|sensor|none]
       [--lat <deg> --lon <deg>] [--timestamp <unix>] [routing]
txtmsg --key <file> --peer <pubkey-hex> --text <text>
       [--timestamp <unix>] [--attempt <n>] [--txt-type <n>] [routing]
ack --hash <hex>           [routing]

parse <hex> [--key <file>] [--peer <pubkey-hex>]

version                    the tool and the pinned upstream commits
```

Routing, shared by every builder:

```
--route flood|direct|transport-flood|transport-direct   (default flood)
--path <hex>              the hop hashes of a direct route
--path-hash-size <1-3>    bytes per hop hash (default 1; 4 is reserved)
--transport <a> <b>       the two transport codes
```

### An advert, end to end

```sh
tools/meshcore-frame/meshcore-frame identity new /tmp/k230-a.id
tools/meshcore-frame/meshcore-frame advert --key /tmp/k230-a.id --name K230-A
```

```
frame_bytes: 109
frame_hex: 1100...814b3233302d41
header: 0x11
route_type: 1 (flood)
payload_type: 4 (ADVERT)
...
advert.name: K230-A
advert.signature_valid: yes
validation: ok
```

`frame_hex` goes straight to the radio:

```sh
pos radio send 1100...814b3233302d41
```

and anything heard back goes straight into `parse`:

```sh
tools/meshcore-frame/meshcore-frame parse <the payload_hex from radio.rx>
```

`parse` exits non-zero when a frame decodes but does not authenticate - a bad
advert signature, or a MAC that does not match the key pair given - so a bench
script can gate on the command rather than on grepping its output.

## What it refuses

The parser is fed hex from a command line and from the air, so it bounds-checks
the wire layout itself before handing anything to `mesh::Packet::readFrom`,
which trusts its input. It refuses hex that is not exactly two digits a byte,
frames past the 255-byte MTU, payloads past the 184-byte limit, a `path_len`
that claims the reserved four-byte hash size or more path than the frame holds,
and frames with no payload at all. The builders refuse a name that would not
fit the 32 bytes of advert data (MeshCore truncates it silently, which is right
for a node and wrong for a tool asked to produce an exact frame), a text past
MeshCore's limit, and a path on a flood route.

## Endianness

MeshCore copies its multi-byte fields with `memcpy` and never byte-swaps, so
transport codes, timestamps and advert coordinates are host order on the wire -
little endian on every MeshCore target and on the K230. `mcf.h` refuses to
compile on a big-endian host rather than let it produce frames nothing can
read.

## Tests

`tests/meshcore_frame_test.cpp` covers the frames and the crypto against fixed
identities, including a recorded ADVERT vector that fails if the wire format or
the signature path changes under us. `tests/meshcore_frame_cli_test.sh` covers
the command line, the identity file, the exit codes and the hex round trip.
Both run plain and under ASan/UBSan.

One exemption: UBSan's `shift-base` check is switched off for
`vendor/RIFT/lib/ed25519` alone. That implementation - like every ref10-derived
Ed25519, and like the code running on a real MeshCore node - left-shifts signed
limbs that are often negative. Everything else in the build, ours and vendored,
keeps the full address and undefined-behaviour checks.
