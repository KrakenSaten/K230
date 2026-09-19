# The MeshCore over-the-air interoperability gate: unit A

**The unit carried build `646dcbb`** (`Doors 0.0.10 (build 646dcbb)`), not the
repository master the gate was run from. Read "Provenance" below before
anything else: the mismatch is real, it is bounded, and the reason the result
still applies is a tree-hash identity rather than an assumption.

**Result: PASS on unit A, 2026-09-19.** Bidirectional over-the-air
interoperability with a known-good MeshCore/RIFT peer, on the real MeshCore
radio profile, at 2 dBm. One advert out, accepted by the peer as a contact;
the peer's own advert in, parsed and signature-verified by Doors tooling; and
a third frame that showed the live mesh relaying that advert onward.

Scope: radio path only. No implementation change, no `radiod` change, no
`meshcore-frame` change, no VERSION change. `riftd` was not started and the
MeshCore/RIFT core port was not started.

## Purpose

To prove that a MeshCore frame built by Doors tooling can travel

    meshcore-frame → pos radio send → radiod → SX1262 → over the air
      → MeshCore/RIFT peer

and be recognised there as a genuine MeshCore ADVERT — and that traffic
travels back the other way afterwards.

This closes the row that
[BRINGUP_SESSION_2026-09-07.md](BRINGUP_SESSION_2026-09-07.md) §20 left open:
"Reception of the packet by another node | not checked (the MeshCore node
cannot decode the PocketOS profile) | UNRESOLVED". That first transmit used
the PocketOS default profile deliberately, so no MeshCore node could have
heard it. This gate used MeshCore's own profile, so one could.

## Provenance

| Item | Value |
| --- | --- |
| Repository master during the gate | `7ffa9ba5cd8f6ce8e014c36a4b7ffd922f26117b` |
| Unit A runtime build | `646dcbb` |
| Doors version | 0.0.10 (unchanged by this gate) |
| Host tool | `tools/meshcore-frame`, built at master `7ffa9ba` |
| Vendored protocol pins | RIFT `3ca7e3f003bd270587b1d92e33ce999542dcb5b4`, Crypto `37a76b8f7516568e1c575b6dc9268da1ccaac6b6` |

### Why a hardware result on `646dcbb` applies to `7ffa9ba`

The unit was not reflashed for this gate, so it still carried the build from
the Clock rotation-state gate of 2026-09-18
([CLOCK_ROTATION_STATE_GATE.md](CLOCK_ROTATION_STATE_GATE.md)). Six commits
sit between that build and the master the gate was run from. The claim is not
that the two builds are the same; it is that **nothing this gate exercised
differs between them.**

Verification method, as run:

```sh
git diff --name-only 646dcbb..7ffa9ba
```

51 files. None of them is under `services/`, and none is under `tools/pos`,
which is where the `pos radio` subcommand lives (`tools/pos/pos_radio.c`). The
delta is `apps/`, `ui/pocketui`, `ui/shell/CMakeLists.txt`, `tests/`, `docs/`,
the top-level `Makefile`, `README.md`, `.gitignore`, and
`tools/meshcore-frame` itself — the host tool, which is not on the unit.

Corroborated by git tree identity, which is stronger than reading a file list:

| Path | at `646dcbb` | at `7ffa9ba` |
| --- | --- | --- |
| `services/radiod` | `343ce8be95d11ed6c6ee306b0c8b67364ee49a77` | `343ce8be95d11ed6c6ee306b0c8b67364ee49a77` |
| `tools/pos` | `10640d2b13497c176c1a68f27d282d6fafdc0e06` | `10640d2b13497c176c1a68f27d282d6fafdc0e06` |

`git diff --stat 646dcbb..7ffa9ba -- services/radiod tools/pos` is empty. The
radio daemon and the CLI that drove it are byte-identical across the gap, so
the hardware result transfers to master without qualification.

What the mismatch does **not** cover: the six commits' own subject matter —
the shared PocketUI layout guard, the app layout passes, the pocketipc SIGPIPE
test, and the host tool's tests — none of which this gate tested on hardware,
and none of which it claims to have.

## The peer

| Item | Value |
| --- | --- |
| Hardware | LILYGO T-Deck |
| Firmware | RIFT v0.9.5 |
| Standing | the known-good interoperable peer |

This is the reference device for MeshCore interoperability: a shipping RIFT
build on its own hardware, configured by its own UI, with nothing of ours on
it. Any disagreement between it and Doors is Doors' to explain.

## Radio profile

Applied with `pos radio configure` and read back from `pos radio status`
field by field before any transmit.

| Field | Value |
| --- | --- |
| Frequency | 869.618 MHz |
| Bandwidth | 62.5 kHz |
| Spreading factor | SF8 |
| Coding rate | 4/5 (`coding_rate=5`) |
| Sync word | 0x12 (`sync_word=18`) |
| Preamble | 32 |
| CRC | enabled |
| TX power | 2 dBm |

Doors' EU868 regional policy **accepted 2 dBm** at 869.618 MHz: the region
guard allows 863.000 to 870.000 MHz and at most 14 dBm requested output, so
the profile sat inside it with a wide margin on power. The guard is a safety
net and not regulatory compliance; duty cycle and ERP remain the operator's
(`docs/api/radio.md`). Measured duty cycle for the whole gate was 0.021 % of
the last hour.

The profile is not persisted. Every `radiod` start returns to the PocketOS
defaults, so these values were set for this session and will not survive a
restart.

## Pre-TX RX proof

Before transmitting anything, unit A listened on the MeshCore profile for 100
seconds.

| Observation | Value |
| --- | --- |
| Ambient MeshCore packets | 10 |
| CRC errors | 0 |
| RSSI | approximately −66 to −72 dBm |
| SNR | approximately 4.75 to 13 dB |
| Frequency error | approximately −223 Hz |
| `radiod` state throughout | `rx` |

Four of the ten frames were then fed to `meshcore-frame parse`. All four
decoded as genuine MeshCore traffic and returned `validation: ok` with exit
code 0 — two GRP_TXT frames (payload type 5) and two RESPONSE frames (payload
type 1), all flood-routed, with path hash counts of 4, 6, 9 and 12 and
payloads of 35 to 76 bytes.

Two things follow, and they are worth separating. The obvious one: **the K230
was already listening to the live MeshCore network before the transmit test**,
so a later silence from the peer could not be blamed on a deaf receiver. The
less obvious one: the tool's understanding of the wire format was confirmed
against real traffic from third-party nodes, generated by no part of this
project, before it was ever asked to build a frame of its own.

## The generated K230 identity

| Item | Value |
| --- | --- |
| Name | `K230-A` |
| Public key | `e1710aa062e6aefc04ad35118ba1e81f07cb0bec983276af3a0082015daeb274` |
| Node hash | `e1` |
| Frame length | 109 bytes |
| Packet hash | `f869ba46dec858e8` |
| Payload type | 4 (ADVERT) |
| Advert type | 1 (chat) |
| Route | flood, zero hops |
| Advert timestamp | 1789802607 (2026-09-19T07:23:27Z) |
| `advert.signature_valid` | yes |
| `validation` | ok |
| `parse` exit code | 0 |

The identity was **disposable and created only for this test**, with
`meshcore-frame identity new` into a scratch file at mode 0600. No personal or
existing identity was used, and the key above is spent: it exists in this
document and in a temporary file, and nothing depends on it.

The frame was round-tripped before it was transmitted — the printed hex was
fed back through `meshcore-frame parse`, which reproduced the payload type,
the name, the signature validity and the packet hash, and exited 0.

## K230 → T-Deck

**Exactly one ADVERT frame was transmitted.** Nothing was sent twice, no retry
was needed, and `radiod` was not bypassed at any point.

| Item | Result |
| --- | --- |
| `radio.send` | succeeded, rc 0 |
| Bytes | 109 |
| Airtime | 754.688 ms |
| `tx_packets` | 0 → 1 |
| `tx_airtime_ms` | 754.688 |
| Duty cycle, last hour | 0.021 % |
| State immediately after the send | `rx` |
| State at +1 s | `rx` |
| State at +3 s | `rx` |
| `radiod` pid | `13766` before and after — unchanged |
| `radiod` restart | none |
| Crash report | none |
| WARN or ERROR in the `radiod` log | **0**, across the whole boot and both log files |

The airtime is the Semtech formula's value for 109 bytes at SF8 / BW 62.5 /
CR 4/5, preamble 32, explicit header, CRC on. The on-air duration itself was
not measured; only the computed figure is recorded, as in §20 of the bring-up
session. `radio.send` is synchronous in v0, so `radiod` was unresponsive for
about that long and then answered normally.

The wall clock of the send itself was not captured — the bench script did not
record it, for the same reason §20 gives (BusyBox `date` has no `%N`). It is
bounded by the surrounding receive events: after 07:31:21Z and before
07:35:10Z. The advert therefore went out roughly eleven minutes after its own
timestamp, which is addressed under "Frame age" below.

### Peer result

The T-Deck created a contact named

    K230-A

and the key prefix it displayed matched `e1710aa0…`.

**This is the primary P0 PASS criterion, and it is met.** Evidence class:
**operator** — the product owner read the T-Deck's own UI and reported the
contact and the matching prefix. No screenshot was taken and the peer's RSSI
and SNR for our frame were not recorded, so the peer-side signal figures are
UNRESOLVED. What is established is identity: a frame this project generated,
signed with a key this project generated, was accepted by an independent
MeshCore implementation as a named contact with the right key.

## T-Deck → K230

The reverse direction, run after the K230 transmit. The owner sent an advert
from the T-Deck; unit A received it through `radiod`, and `meshcore-frame`
parsed it.

| Item | Value |
| --- | --- |
| Name | `Tdeck RIFT` |
| Node hash | `36` |
| Public key | `367bff1533c80d7c727d7f9505d833e2db5b0d58dca9f03244e8bb56c1133d50` |
| Payload type | 4 (ADVERT) |
| Advert type | 1 (chat) |
| `signature_valid` | yes |
| `validation` | ok |
| Direct reception RSSI | approximately −34 to −36 dBm |
| SNR | 11.75 to 13.25 dB |

A **second copy of the same advert** then arrived by a different path:

| Item | Value |
| --- | --- |
| Packet hash | `90513da12b1c8572` — identical to the direct copy |
| Signature and timestamp | identical to the direct copy |
| Path hash appended | `3006` |
| Path hash count | 1 |
| Relayed RSSI | approximately −68 dBm |

Same frame, one hop later, 30-odd dB weaker, with an intermediate node's hash
written into its path. What that independently demonstrates:

- **RX remained healthy after the K230 transmit.** Not inferred from a
  counter: whole frames arrived, decoded and verified, after the TX.
- **Real MeshCore flooding is visible from unit A** — a third-party node took
  the peer's advert and rebroadcast it where the K230 could hear both copies.
- **The parser understands both zero-hop and relayed traffic**, and returns
  the same payload and the same packet hash for a frame whose path has grown.
- **Path metadata is present in live frames** and is read correctly, which is
  what makes the interoperability note below a real finding rather than a
  theoretical one.
- **RSSI and SNR metadata remain functional** after a transmit, across a
  60 dB spread of signal levels.

This was not the primary gate criterion, and it exceeded what was asked of it.

## Final counters

| Counter | Value |
| --- | --- |
| `tx_packets` | 1 |
| `rx_packets` | 56 |
| `rx_crc_errors` | **0** |
| `radiod` state at the end | `rx` |
| WARN or ERROR, both log files | **0** |
| `radiod` restarts during the gate | 0 after the one deliberate restart that enabled the backend |
| Crash reports | none |
| `doors-shell` processes | 2 (supervisor and daemon), alive throughout |

56 receives and one transmit, with not a single CRC error in either the
pre-transmit listening or the three windows after it.

## Interoperability note: path hash size

Recorded as a requirement for later work, **not** acted on here.

RIFT v0.9.5 emits adverts with

    path_hash_size = 2,  path_len = 0x40 at zero hops

while `meshcore-frame` currently defaults to

    path_hash_size = 1,  path_len = 0x00 at zero hops

For a zero-hop ADVERT this changes no actual path bytes, because there are
none: both encodings say "no path", and only the size field in the header byte
differs. The generated frame was accepted by RIFT, so advert interoperability
is unaffected and this gate is not weakened by it.

However: **any future DIRECT or routed frame carrying actual path entries must
match the network's path-hash-size behaviour.** A one-byte-per-hop path
written where the network expects two would be misread hop for hop. The
relayed copy above is the live proof that this network really does use
two-byte hops — `3006` is one hash, not two.

The tool already exposes the knob:

    --path-hash-size 2

so this is a matter of choosing it in the P1/P2 test procedures, not of
changing the tool. **The tool was not changed in this documentation task.**

## Frame age

Worth recording because it could have produced a false failure and did not.
The advert was transmitted about eleven minutes after the timestamp inside it.
Checked against the vendored protocol source rather than assumed:

- `mesh::Mesh::onRecvPacket` (`vendor/RIFT/src/Mesh.cpp`, PAYLOAD_TYPE_ADVERT)
  validates the Ed25519 signature and consults `wasSeen`. It applies no
  absolute test against local time.
- `BaseChatMesh::onAdvertRecv` (`vendor/RIFT/src/helpers/BaseChatMesh.cpp`)
  compares `timestamp <= from->last_advert_timestamp` as a replay check
  **only for a contact it already holds**. A new contact reaches
  `populateContactFromAdvert` with no freshness test at all.

So a stale advert from an unknown node is accepted, and `K230-A` was unknown.
One consequence for anyone repeating this: a retry of a byte-identical frame
would be dropped by the peer's `wasSeen` cache once it has been received, so a
genuine retry needs a freshly generated frame.

## radiod backend state

The v0.0.10 reflash had restored `radiod` to the **mock** backend, because
`/etc/default/radiod` was absent and `/etc/init.d/S60radiod` defaults to
`RADIOD_BACKEND=mock` when it cannot read that file. The `radiod` log confirms
it: every start recorded on the unit, across every boot in the log, reads
`backend=mock`. The real backend had been enabled there on 2026-09-07 and the
reflash took the file with it.

For this gate, `/etc/default/radiod` was created with

    RADIOD_BACKEND=sx1262
    RADIOD_REGION=EU868
    RADIOD_TX_POWER_DBM=2

and `radiod` was restarted once, after the product owner confirmed an 868 MHz
antenna on MMCX1 — the port that §19.2 of the bring-up session identified as
the SX1262's. The init script's own comment makes that confirmation the
precondition for a transmit, and it was obtained before the backend was
enabled, not after.

**The SX1262 backend initialised successfully on the first attempt**:
`pos radio info` returned `chip: sx1262`, state `rx`, and no
`SX1262 begin failed` appeared. The `doors-shell` survived the `radiod`
restart, which is the pocketipc SIGPIPE fix working in place on hardware.

**Unit A was intentionally left configured with the real SX1262 backend after
the gate**, matching what §20 of the bring-up session did.
`/etc/default/radiod` persists, and the MeshCore profile remains active in the
running daemon, so the next `pos radio send` genuinely transmits. The profile
itself is not persisted and a restart returns it to the PocketOS defaults at
2 dBm; the backend choice does persist.

Disarm command, **for reference only — not executed in this task**:

```sh
ssh root@192.168.10.157 "rm /etc/default/radiod && /etc/init.d/S60radiod restart"
```

## What this gate proves

PROVEN:

- K230 SX1262 TX is received by a real MeshCore/RIFT peer.
- A Doors-generated MeshCore ADVERT is wire-compatible.
- An Ed25519 signature produced by Doors tooling is accepted by RIFT.
- `radiod` transmits on the MeshCore profile.
- `radiod` returns to RX after TX.
- The K230 receives MeshCore traffic after TX.
- T-Deck → K230 traffic works.
- Live relayed MeshCore traffic is parsable.
- RSSI and SNR metadata remain functional.
- No CRC errors were observed.
- No `radiod` crash or unplanned restart occurred.

NOT YET PROVEN:

- Asynchronous TX.
- Sustained TX soak.
- `riftd`.
- A MeshCore protocol daemon.
- Directed TXT_MSG interoperability.
- ACK interoperability.
- Path-learning interoperability.
- Multi-hop transmission originating from the K230.
- Restart recovery under `riftd`.
- Long-duration unattended mesh operation.

Two further limits, recorded so the list above is not read as more than it is:
radiated power, spectrum and harmonics were not measured, and the peer-side
signal figures for our frame were not captured.

## P0 verdict

**P0 — SX1262 / MeshCore over-the-air interoperability: ACCEPTED**

Date: 2026-09-19
