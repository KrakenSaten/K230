# radio.* API v0 (radiod)

Status: draft, api_version 0. Transport: pocketipc, socket `radiod.sock`.

`radiod` owns the LoRa transceiver exclusively (ADR-002). On the T-Display
K230 that is an SX1262 on `/dev/spidev0.0` with reset, busy, DIO1 and power
GPIOs (DOCUMENTED, see docs/hardware/T-DISPLAY-K230.md). Applications never
open those devices.

## What the hardware can and cannot do

SX1262 facts (DOCUMENTED, Semtech datasheet):

- LoRa and (G)FSK modulation, 150 MHz to 960 MHz, up to +22 dBm output.
- Per-packet RSSI and SNR, instantaneous channel RSSI, channel activity
  detection (CAD), frequency error estimate.
- It is a packet radio. It is not a software-defined radio and cannot
  produce a spectrum view or demodulate arbitrary signals. Any "scan" in
  PocketOS is a sequence of narrowband RSSI or CAD samples.

v0 exposes LoRa only. FSK is a possible later addition behind the same API.

## Backends

- `sx1262`: real hardware, RadioLib 7.7.1 (MIT) with the PocketOS HAL on
  spidev and libgpiod v2 (`services/radiod/hal_linux.cpp`,
  `backend_sx1262.cpp`). Wiring and module parameters come from environment
  variables with T-Display K230 defaults (see the file header). It has run on
  unit A: receive, transmit, RX re-entry and bidirectional MeshCore
  interoperability are VERIFIED (see below).
- `mock`: in-process simulation for development and tests. Computes real
  LoRa time-on-air, keeps statistics, and delivers packets injected with
  `mock.inject_rx`.

### What the sx1262 backend has done on hardware

Two runs on unit A, both with radiod on the `sx1262` backend. The transcripts
and the full evidence tables are in the two documents named.

2026-09-07 bring-up, docs/hardware/BRINGUP_SESSION_2026-09-07.md §19-20. The
K230 received the ambient Norwegian MeshCore network (58 packets, 0 CRC
errors, RSSI -15 to -109 dBm across a three-round antenna test that also
identified MMCX1 as the RF port), then transmitted once: 8 bytes at 2 dBm on
the PocketOS default profile, airtime 123.904 ms, state `rx` again afterwards.
VERIFIED: the backend receives and transmits on this board. Left open there,
because that transmit deliberately used the PocketOS profile which no MeshCore
node can demodulate: whether another node decodes what the K230 sends.

2026-09-19 MeshCore over-the-air interoperability gate, P0 ACCEPTED,
docs/hardware/MESHCORE_INTEROP_GATE.md. Unit A on build 646dcbb (Doors
0.0.10), peer a LILYGO T-Deck on RIFT v0.9.5, on MeshCore's own profile:
869.618 MHz, 62.5 kHz, SF8, CR 4/5, sync 0x12, preamble 32, CRC on, 2 dBm.
VERIFIED by that gate:

- Real MeshCore receive. A 100 s listen before any transmit took 10 ambient
  packets with 0 CRC errors; four were parsed and returned `validation: ok` as
  genuine third-party MeshCore traffic.
- Real transmit. Exactly one 109-byte ADVERT, `radio.send` rc 0, airtime
  754.688 ms computed.
- Another MeshCore implementation decoded that transmission. The T-Deck
  created a contact named `K230-A` with the matching key prefix, the gate's
  primary PASS criterion. The gate classes the peer-side reading as operator
  evidence, the owner reading the T-Deck's own UI; the peer's RSSI and SNR for
  our frame were not captured and stay UNRESOLVED.
- RX re-entry after the transmit: state `rx` immediately, at +1 s and at +3 s.
- Reverse-direction traffic after the transmit. The peer's own advert arrived
  and was signature-verified (`Tdeck RIFT`, RSSI about -34 to -36 dBm), and a
  second copy of the same frame arrived one hop later, relayed by a
  third-party node, at about -68 dBm. Whole frames decoded after the TX, not a
  counter inferred to be healthy.
- Bidirectional MeshCore interoperability, as the sum of those two directions:
  a frame this project generated and signed was accepted by an independent
  implementation, and frames that network produced were received and validated
  here.

Gate counters: 1 TX, 56 RX, 0 CRC errors, state `rx` throughout, 0 WARN or
ERROR in either radiod log, no crash and no unplanned restart.

Neither run measured radiated RF power, spectrum or harmonics; those remain
UNRESOLVED. The RX-failure paths under radio.status are a separate and weaker
case, DOCUMENTED rather than VERIFIED.

## Methods

### radio.info

Result: `chip` ("sx1262" or "mock"), `backend`, `api_version`,
`capabilities`: `frequency_min_mhz`, `frequency_max_mhz`, `tx_power_min_dbm`,
`tx_power_max_dbm`, `max_payload`, `modulations` (["lora"]), `cad` (bool),
`region` (string, e.g. "EU868").

### radio.status

Result: `state` (`off`, `idle`, `rx`, `tx`, `error`), `profile` (current
profile object, see radio.configure), `uptime_s`.

The state values are unchanged. `tx` now covers an asynchronous transmission
as well as a synchronous one, so it can be observed from another connection
while a packet is on the air - which on the mock backend it can be, because
the daemon keeps answering. Ownership is asked for separately with
`radio.lease` rather than added here, so nothing that parses this result
changes.

`rx` is reported only while the backend confirms the transceiver is in
receive mode. If re-entering RX fails after a transmit, CAD or packet read,
the state becomes `error`, a `radio.state` event is sent, and radiod retries
at most once per second until RX is back (then `rx` again). `radio.send`
still transmits in state `error`; its result reports the transmit, not the
receive state. On the mock backend `mock.set {key: "rx_failing", value: 1}`
simulates this.

On the SX1262 the two halves of this carry different evidence, and they should
not be read as one claim:

- RX re-entry **succeeding** after a transmit is VERIFIED on unit A on two
  dates. On 2026-09-07 the state was `rx` immediately after the send and again
  1 s later, the backend's `enter_rx()` after `transmit()` reporting
  `is_receiving` (docs/hardware/BRINGUP_SESSION_2026-09-07.md §20). On
  2026-09-19 it was `rx` immediately, at +1 s and at +3 s, and whole MeshCore
  frames were received and validated afterwards
  (docs/hardware/MESHCORE_INTEROP_GATE.md).
- The **failure** path is DOCUMENTED, not VERIFIED. The transition to `error`,
  the `radio.state` event that announces it, and the once-per-second retry
  until RX is back are compiled but have never been exercised on hardware.
  Nothing has yet made `enter_rx()` fail on the SX1262, and the mock knob above
  is the only place those paths run.

### radio.configure

Params (all optional; omitted fields keep their value):

| Field | Type | Range (LoRa, SX1262) | EU868 default |
| --- | --- | --- | --- |
| frequency_mhz | number | region dependent | 869.525 |
| bandwidth_khz | number | 7.8 to 500 | 125 |
| spreading_factor | int | 5 to 12 | 9 |
| coding_rate | int | 5 to 8 (4/5 to 4/8) | 5 |
| sync_word | int | 0x00 to 0xFF (0x12 private, 0x34 public) | 0x12 |
| preamble_length | int | 1 to 65535 | 8 |
| tx_power_dbm | int | -9 to 22 | 2 (radiod `--tx-power-dbm`, `RADIOD_TX_POWER_DBM` in /etc/default/radiod) |
| crc | bool | | true |

Result: the applied profile. Errors: 2 on out-of-range values, 3 when the
region guard rejects the frequency or power.

Region guard (v0): only `EU868` exists, allowing 863.000 to 870.000 MHz and
at most 14 dBm requested output power. This is a safety net, not regulatory
compliance: duty-cycle limits, ERP with antenna gain, and sub-band rules are
the operator's responsibility. Airtime accounting (below) helps with that.

The profile is not persisted. Every radiod start returns to the defaults
above with the power given by `--tx-power-dbm` (2 dBm unless
`/etc/default/radiod` says otherwise), never to the region maximum; the
start-up profile is validated like `radio.configure`, so an out-of-range
`--tx-power-dbm` stops radiod with exit code 2 before the radio is touched.
Raising the power is a per-session operator action after the antenna on the
SX1262 port has been confirmed (docs/hardware/BRINGUP_CHECKLIST.md §5).

### radio.send

Params: `payload_hex` (string, 1 to 255 bytes). Result: `airtime_ms`
(computed time-on-air), `bytes`, `tx_id`. Error 5 if a transmission is in
progress, error 3 if another client holds the radio lease.

`radio.send` is synchronous: it does not answer until the packet has gone
out. It is the same state machine as `radio.send_async` with the wait moved
inside the request, and it raises the same `radio.tx_done` event; `tx_id`
names that event. The blocking time is bounded by the profile (Semtech
formula, `tests/airtime_test.c`): about 1.3 s for the EU868 default profile
with 255 bytes, about 9 s at SF12/BW125 with 255 bytes, and up to about
225 s in the extreme corner (SF12, BW 7.8 kHz, CR 4/8, 255 bytes). radiod
answers nothing else during that time.

A failed re-entry into receive is **not** reported as a failed send here.
The packet went out, which is what this call answers; the receive state is
in `radio.tx_done`, in `radio.status` and in the `radio.state` event that
has already been sent.

A `timeout_ms` parameter is refused with error 2. The asynchronous path
below is what replaced it: a deadline on a transmit whose completion is the
point would report failure for a packet that went out.

Integer profile fields (`spreading_factor`, `coding_rate`, `sync_word`,
`preamble_length`, `tx_power_dbm`) must be integral JSON numbers: `7` and
`7.0` are accepted, `7.9` is refused with error 2 rather than truncated.

### radio.send_async

Params: `payload_hex` (string, 1 to 255 bytes). Result: `accepted` (always
`true`), `tx_id`, `bytes`, `airtime_ms` (the time-on-air this profile will
take for this length, computed before the packet goes out).

Returns as soon as the transmission has been **accepted**. Nothing in the
result says the packet went out; the radio has not been touched yet. The
outcome arrives later as a `radio.tx_done` event carrying the same `tx_id`.

Errors: 2 for a payload that is not 1 to 255 bytes of hex, 5 when a
transmission is already in flight, 3 when another client holds the lease. A
refused request allocates no `tx_id`, so there is never a completion for a
transmission that was never accepted.

**Exactly one `radio.tx_done` per accepted request**, on every path. The
cases, and what each one does:

| Case | Result | Event |
| --- | --- | --- |
| Refused before TX (bad payload, BUSY, no lease) | error response | none; no `tx_id` was allocated |
| The backend refuses to start the transmit | accepted | `ok: false`, `result: "tx_failed"`, `transmitted: false` |
| The transmit itself fails | accepted | `ok: false`, `result: "tx_failed"`, `transmitted: false` |
| Transmitted, RX re-entry failed | accepted | `ok: false`, `result: "rx_resume_failed"`, `transmitted: true`, `rx_resumed: false`, state `error` |
| Transmitted, receiving again | accepted | `ok: true`, `result: "ok"` |
| The submitting client disconnects while it is on air | accepted | the transmit **runs to completion**, is counted, and the event goes to whoever is still subscribed |
| radiod restarts during the transmit | — | no event; see below |

`ok` is `transmitted && rx_resumed`, which is the strict reading of success:
the backend completed the transmit *and* the expected receive state was
restored. `transmitted` is reported separately on purpose. A daemon asking
"must I send this again?" reads `transmitted`; collapsing the two would tell
it that a packet which went out perfectly well had failed, and the answer to
that is a retransmission - twice the airtime, for a fault in the receive
path.

The completion is never emitted before RX re-entry has been checked: radiod
asks the backend, updates its own state, and only then sends the event, so
the `state` field in `radio.tx_done` is something that has actually been
looked at.

**Restart.** A `tx_id` is meaningful only on the connection that created it.
radiod persists no transmit state, so a restart delivers no completion for a
transmission that was in flight - but a restart also closes every
connection, so a client cannot mistake a new `tx_id` for an old one. A
client whose connection drops must treat the outcome as unknown. On a clean
stop (SIGTERM/SIGINT) radiod finishes a packet already on the air before
exiting, rather than abandoning a keyed transceiver.

#### One at a time

There is no queue. One transmission is in flight; a second request is
refused with error 5 and a message naming the `tx_id` that holds the radio.
Nothing is ever silently dropped, overwritten or held back to be sent at a
moment the caller cannot predict - a protocol daemon that gets an error
knows its packet did not go out, and one whose packet was quietly discarded
does not. A daemon that wants a queue keeps it on its own side, where it
knows its own priorities; radiod does not.

While a transmission is in flight, `radio.configure` and `radio.cad` are
refused with error 5: both would disturb a packet that is already going out.
Reads (`radio.info`, `radio.status`, `radio.stats`, `radio.rssi`,
`radio.channel`, `radio.lease`) stay available.

#### What "asynchronous" does and does not mean per backend

The contract above holds on every backend: the request is answered before
the packet goes out, and the completion follows.

What differs is whether radiod serves other clients meanwhile.

- `mock`: a real asynchronous transmit (`tx_begin`/`tx_poll`). The daemon
  answers everyone throughout. **VERIFIED host/mock**
  (`tests/radiod_async_test.sh` measures both the reply latency and the
  requests served while the packet is on the air).
- `sx1262`: no asynchronous transmit. RadioLib's `transmit()` does not
  return until the packet has left, so radiod falls back to the blocking
  `send()` and is unresponsive for the airtime, exactly as it was before.
  What changed is *where* the wait happens - the service loop rather than
  the request handler - so the submitting client already has its answer.
  Making the SX1262 itself asynchronous means `startTransmit()` with a
  DIO1-driven completion, which changes the exact transmit sequence this
  board's only two successful on-air runs used, and is **UNRESOLVED**
  pending hardware validation.

The fallback path is not untested: the mock's `tx_async=0` knob turns off
its asynchronous transmit so the blocking path - the one the real hardware
takes - is exercised by both `tests/radiod_tx_test.c` and
`tests/radiod_async_test.sh`. That is host coverage of the code path, not
evidence about the SX1262.

### radio.stats

Result: `tx_packets`, `rx_packets`, `rx_crc_errors`, `tx_airtime_ms`,
`rx_airtime_ms`, `tx_airtime_last_hour_ms`, `duty_cycle_last_hour_percent`,
`last_rssi_dbm`, `last_snr_db`, `last_frequency_error_hz`.

### radio.acquire / radio.release / radio.lease

A generic ownership boundary, so one long-running protocol daemon can hold
the configured radio session without a second client reconfiguring or
transmitting on it underneath. It is an **exclusivity boundary, not a
scheduler**: there is no timeslicing, no priority and no sharing between
protocols.

It is opt-in. While nobody holds a lease every client may do everything,
which is what every caller written before this existed expects. The boundary
appears the moment someone asks for it.

`radio.acquire` - params: `owner` (optional string, under 64 characters, an
informational label). Result: `held`, `mine`, `owner`, `owner_id`,
`since_mono_ms`.

- The holder may acquire again and gets the same `owner_id` back. A daemon
  that is unsure whether a reconnect kept the lease asks again; refusing
  that would make recovery harder than it needs to be. The label is
  replaced, the identity and start time are not.
- Another client gets error 5, with the current owner named in the message.
- `owner_id` is fresh for each lease, so a client cannot mistake the lease
  it holds now for one it held before.

`radio.release` - no params. Result: the lease state. Error 3 unless this
connection holds it, **including when nobody does**: releasing a lease
somebody else is relying on is exactly the accident the lease exists to
prevent, so it is refused rather than quietly ignored.

`radio.lease` - no params, always available. Result: `held`, `mine`, and
when held `owner`, `owner_id`, `since_mono_ms`.

**Ownership is per connection.** radiod identifies clients by a
per-connection id that is never reused while it runs, not by file
descriptor: the kernel hands the same descriptor to the next client the
moment one closes, and state keyed by descriptor can be inherited by a
stranger.

**Disconnect.** Closing the connection releases the lease and broadcasts
`radio.lease` with `reason: "client_gone"`. A daemon that crashes without
releasing does not lock the radio for good. A transmission that client
submitted is *not* cancelled - see `radio.send_async`.

**Restart.** No lease survives a restart and none is reconstructed. A
restart closes every connection, so radiod cannot tell which of the clients
that come back is the one that held it, and a lease handed to the wrong one
is worse than no lease. The daemon asks again when it reconnects.

**What the lease gates**, when one is held:

| Gated (owner only, error 3 otherwise) | Always available |
| --- | --- |
| `radio.configure` | `radio.info`, `radio.status`, `radio.stats` |
| `radio.send`, `radio.send_async` | `radio.rssi`, `radio.channel`, `radio.lease` |
| `radio.cad` | `radio.acquire`, `radio.release`, `radio.subscribe`/`unsubscribe` |

Reads are never gated. A radio a daemon owns must not become a radio nobody
can diagnose. `mock.*` is test-only and ungated.

### radio.channel

No params. A **passive** observation of the channel: it never takes the
radio out of receive, so it is safe to poll.

Result: `mono_ms` (when the sample was taken), `transmitting` (bool),
`cad_supported` (bool), and three pairs:

| Known flag | Value, present only when the flag is true |
| --- | --- |
| `rssi_known` | `rssi_dbm` |
| `noise_known` | `noise_dbm` |
| `activity_known` | `busy` |

**Unknown information stays explicitly unknown.** The flags are the point:
"unknown" and "quiet" are different answers, and a protocol daemon deciding
whether to transmit would read a fabricated `busy: false` as permission. A
value is absent whenever its flag is false. While `transmitting` is true
everything is unknown - the radio is the one making the noise.

What each backend reports:

- `mock`: everything known (a simulation is the one thing that can honestly
  say it knows its own channel). `mock.set channel_unknown=1` makes it
  report nothing, which is the shape a real backend produces.
  **VERIFIED host/mock**.
- `sx1262`: `rssi_known: true` only. **Compile verified**; the RSSI read is
  `getRSSI(false)`, the same GetRssiInst call `radio.rssi` has made on unit
  A, so the value itself rests on existing hardware evidence. The other two
  are deliberately false:
  - **Noise: UNRESOLVED.** The SX1262 has no noise measurement. The RSSI
    read while nothing is arriving *is* the noise floor, but the chip cannot
    tell us nothing is arriving, so calling that number "noise" would be a
    guess dressed as a measurement.
  - **Busy: UNRESOLVED.** CAD is the obvious candidate and is the wrong one
    twice over. `scanChannel()` detects a **LoRa preamble at the currently
    configured modulation** - not FSK, not another spreading factor, and not
    a packet whose preamble has already passed - so a quiet CAD result is
    not a quiet channel. And running it takes the radio out of receive and
    back, which a passive status read may not do to a service that is in the
    middle of receiving. Deriving `busy` from RSSI instead needs a threshold
    calibrated on this board with this antenna, and nobody has measured one.

So `radio.channel` on real hardware currently answers "the instantaneous
RSSI is X, and I do not know whether the channel is busy". That is less than
a caller might want and it is what is true. Turning it into a real busy
signal needs hardware validation and probably a larger radio state-machine
change; it is recorded in **Not in v0** below.

### radio.cad

Result: `activity` (bool). Error 6 if the backend cannot do CAD, 5 while a
transmission is in flight, 3 without the lease when one is held.

An **active** LoRa-preamble check, asked for by name. It takes the radio out
of receive and puts it back, and it detects only a preamble at the
configured modulation. It is deliberately not folded into `radio.channel`:
the cost and the narrowness are the caller's to accept.

### radio.rssi

Result: `rssi_dbm` (instantaneous channel RSSI). Error 6 if unsupported.
Unchanged, and not gated by the lease; `radio.channel` is the newer shape
and says what it does not know.

### radio.subscribe / radio.unsubscribe

No params. Result: `{"subscribed": true|false}`.

### mock.inject_rx (mock backend only)

Params: `payload_hex`, optional `rssi_dbm`, `snr_db`. Delivers a packet as if
received. Error 6 on real hardware.

### mock.set (mock backend only)

Params: `key` (string), `value` (integer). Debug knobs for tests. Error 6 on
real hardware, error 2 for an unknown key.

| Key | Effect |
| --- | --- |
| `rx_failing` | the mock cannot enter receive mode |
| `rx_fails_after_receive` | receive is lost while handing over a good packet |
| `configure_fail_stage` | 0-3: which stage of configure fails |
| `configure_fail_once` | the stage above applies to the next configure only |
| `tx_async` | 0 = no asynchronous transmit, so radiod uses the blocking `send()` - the path the SX1262 takes |
| `tx_delay_ms` | how long a transmit stays on the air |
| `tx_fail` | the transmit itself fails |
| `tx_fail_begin` | the backend refuses before transmitting |
| `tx_rx_fails_after` | transmitted, then could not re-enter receive |
| `channel_unknown` | the backend can say nothing about the channel |

`mock.inject_rx` also takes an optional `mono_ms`, so a test can place a
packet at a monotonic time it would otherwise need 49 days of uptime to
reach. It exists on that path and nowhere else: no real packet can carry a
time radiod did not read from the clock itself.

## Events

- `radio.rx`: `payload_hex`, `bytes`, `rssi_dbm`, `snr_db`,
  `frequency_error_hz`, `timestamp_ms`, `mono_ms`, `airtime_ms`.
- `radio.tx_done`: `tx_id`, `ok`, `result`, `transmitted`, `rx_resumed`,
  `state`, `bytes`, `airtime_ms`, `mono_ms`, `timestamp_ms`, and `error`
  when not `ok`. See `radio.send_async`.
- `radio.state`: `state`.
- `radio.lease`: `held`, `reason` (`acquired`, `released`, `client_gone`),
  `mono_ms`, and `owner`/`owner_id` while held.

All additions are additive: every field the v0 events carried is still
there, with the same name and meaning.

## Time

Two clocks, and they are not interchangeable.

- `timestamp_ms` is `CLOCK_REALTIME`. Comparable with other machines, and it
  jumps: NTP steps it, an operator sets it, and it runs backwards when
  either does. On this board it starts at 1970 on every boot and moves by
  decades the moment the network comes up.
- `mono_ms` is `CLOCK_MONOTONIC`. Milliseconds since boot. Not comparable
  with other machines, and it never jumps or runs backwards - the only
  property a receive timestamp needs.

**Subtract `mono_ms`, never `timestamp_ms`.** A protocol daemon measuring an
interval across the first NTP step of a boot would otherwise get decades.

`mono_ms` is 64-bit the whole way. At 32 bits it wraps after 49.7 days of
uptime, which is well within a handheld's uptime and turns an interval into
about 49 days. It is carried as a JSON number, and a double represents every
integer exactly up to 2^53. The practical upper bound is the JSON printer's,
not the type's: cJSON prints with `%1.15g`, so integers of more than 15
significant digits can come back off by one. That bound is 10^15 ms, about
31700 years of uptime. `tests/radiod_tx_test.c` pins the exactness up to it
and records the bound.

Every `radio.rx` carries a `mono_ms`. A backend that does not stamp one gets
it stamped by radiod when the packet is drained, so none can reach a client
without one. **VERIFIED host/mock** for the mock backend and the daemon's
own stamping; **compile verified** for the SX1262's own stamp, which is
taken beside the wall-clock one it already took.

## Time-on-air

`airtime_ms` uses the Semtech LoRa formula (SX1276/SX126x application
notes): symbol time 2^SF / BW, preamble (n + 4.25) symbols, payload symbols
8 + max(ceil((8L - 4SF + 28 + 16 CRC - 20 IH) / (4 (SF - 2 DE))) * (CR + 4), 0)
with low-data-rate optimisation DE=1 when the symbol time exceeds 16 ms
(SF11/SF12 at 125 kHz), explicit header (IH=0). The mock and the sx1262
backend share this implementation; `tests/airtime_test.c` pins reference
values.

## Evidence summary for this revision

What the asynchronous transmit, the lease, the monotonic receive timestamp
and the channel status rest on. None of it has been near a radio.

| Feature | Class | Basis |
| --- | --- | --- |
| Async TX contract: accepted before the packet goes out | **VERIFIED host/mock** | `tests/radiod_async_test.sh` times the reply against the completion |
| Daemon stays responsive during a transmit | **VERIFIED host/mock** | same test counts requests served while on air - **mock backend only** |
| Exactly one `radio.tx_done` per accepted request, on every outcome path | **VERIFIED host/mock** | `tests/radiod_tx_test.c` outcome table, both transmit paths |
| BUSY behaviour, no queue, nothing dropped | **VERIFIED host/mock** | both tests |
| Payload lifetime across an async transmit | **VERIFIED host/mock** | the mock compares the caller's buffer at completion; the check is itself proved to bite |
| Lease semantics (acquire, duplicate, conflict, wrong-owner release, disconnect, restart) | **VERIFIED host/mock** | `tests/radiod_tx_test.c` and `tests/radiod_async_test.sh` |
| `mono_ms` is a different clock from `timestamp_ms`, monotonic, 64-bit, exact on the wire past 2^32 and 2^40 | **VERIFIED host/mock** | `tests/radiod_tx_test.c`, `tests/radiod_async_test.sh` |
| `radio.channel` contract and the unknown-stays-unknown shape | **VERIFIED host/mock** | `tests/radiod_async_test.sh`, both directions of `channel_unknown` |
| The same code compiled for the SX1262 backend and riscv64 | **compile verified** | `make sx1262-objs`, riscv64 cross-build |
| SX1262 RSSI underneath `radio.channel` | **DOCUMENTED** | the same `getRSSI(false)` call already run on unit A; the new wrapper has not been |
| SX1262 async transmit | **UNRESOLVED** | not implemented; needs `startTransmit()` + DIO1 completion and hardware validation |
| A real channel-busy signal on the SX1262 | **UNRESOLVED** | needs a calibrated threshold or a CAD-in-the-state-machine design, both needing hardware |
| Any of this on unit A | **UNRESOLVED** | no hardware was touched in this change |

The transmit and receive behaviour that *was* hardware VERIFIED - the two
runs described above - is unchanged: the SX1262 backend's `send`,
`configure`, `receive`, `enter_rx`, SPI/GPIO ownership and RESET/BUSY/DIO1
handling are byte-for-byte what they were. What was added to it is one
passive RSSI reader and one extra timestamp field.

## Not in v0

FSK, LoRaWAN, frequency hopping, multiple radios, RF timeslicing or
arbitration between protocols (the lease is exclusivity, not scheduling),
persistence of the profile or the lease across restarts, a transmit queue,
an asynchronous transmit on the SX1262, and a truthful channel-busy signal
on real hardware.
