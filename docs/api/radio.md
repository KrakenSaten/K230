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
  variables with T-Display K230 defaults (see the file header). Compiles for
  riscv64; not yet run on hardware.
- `mock`: in-process simulation for development and tests. Computes real
  LoRa time-on-air, keeps statistics, and delivers packets injected with
  `mock.inject_rx`.

## Methods

### radio.info

Result: `chip` ("sx1262" or "mock"), `backend`, `api_version`,
`capabilities`: `frequency_min_mhz`, `frequency_max_mhz`, `tx_power_min_dbm`,
`tx_power_max_dbm`, `max_payload`, `modulations` (["lora"]), `cad` (bool),
`region` (string, e.g. "EU868").

### radio.status

Result: `state` (`off`, `idle`, `rx`, `tx`, `error`), `profile` (current
profile object, see radio.configure), `uptime_s`.

`rx` is reported only while the backend confirms the transceiver is in
receive mode. If re-entering RX fails after a transmit, CAD or packet read,
the state becomes `error`, a `radio.state` event is sent, and radiod retries
at most once per second until RX is back (then `rx` again). `radio.send`
still transmits in state `error`; its result reports the transmit, not the
receive state. On the mock backend `mock.set {key: "rx_failing", value: 1}`
simulates this; on the SX1262 the paths are compiled but unverified until
hardware testing (DOCUMENTED, not VERIFIED).

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
| tx_power_dbm | int | -9 to 22 | 14 |
| crc | bool | | true |

Result: the applied profile. Errors: 2 on out-of-range values, 3 when the
region guard rejects the frequency or power.

Region guard (v0): only `EU868` exists, allowing 863.000 to 870.000 MHz and
at most 14 dBm requested output power. This is a safety net, not regulatory
compliance: duty-cycle limits, ERP with antenna gain, and sub-band rules are
the operator's responsibility. Airtime accounting (below) helps with that.

### radio.send

Params: `payload_hex` (string, 1 to 255 bytes). Result: `airtime_ms`
(computed time-on-air), `bytes`. Error 5 if a transmission is in progress.

v0 behaviour, stated explicitly: `radio.send` is synchronous. radiod does
not answer other requests or deliver events while the packet is on air,
because the SX1262 backend blocks in RadioLib's `transmit()`. The blocking
time is bounded by the profile (Semtech formula, `tests/airtime_test.c`):
about 1.3 s for the EU868 default profile with 255 bytes, about 9 s at
SF12/BW125 with 255 bytes, and up to about 225 s in the extreme corner
(SF12, BW 7.8 kHz, CR 4/8, 255 bytes). Callers that need the daemon
responsive keep spreading factors and payloads small.
A `timeout_ms` parameter is refused with error 2; it was documented earlier
but never implemented, and will only return with an asynchronous TX path.

Integer profile fields (`spreading_factor`, `coding_rate`, `sync_word`,
`preamble_length`, `tx_power_dbm`) must be integral JSON numbers: `7` and
`7.0` are accepted, `7.9` is refused with error 2 rather than truncated.

### radio.stats

Result: `tx_packets`, `rx_packets`, `rx_crc_errors`, `tx_airtime_ms`,
`rx_airtime_ms`, `tx_airtime_last_hour_ms`, `duty_cycle_last_hour_percent`,
`last_rssi_dbm`, `last_snr_db`, `last_frequency_error_hz`.

### radio.cad

Result: `activity` (bool). Error 6 if the backend cannot do CAD.

### radio.rssi

Result: `rssi_dbm` (instantaneous channel RSSI). Error 6 if unsupported.

### radio.subscribe / radio.unsubscribe

No params. Result: `{"subscribed": true|false}`.

### mock.inject_rx (mock backend only)

Params: `payload_hex`, optional `rssi_dbm`, `snr_db`. Delivers a packet as if
received. Error 6 on real hardware.

### mock.set (mock backend only)

Params: `key` (string), `value` (integer). Debug knobs for tests; currently
`rx_failing` (1 = the mock cannot enter receive mode). Error 6 on real
hardware, error 2 for an unknown key.

## Events

- `radio.rx`: `payload_hex`, `bytes`, `rssi_dbm`, `snr_db`,
  `frequency_error_hz`, `timestamp_ms`, `airtime_ms`.
- `radio.tx_done`: `bytes`, `airtime_ms`, `timestamp_ms`.
- `radio.state`: `state`.

## Time-on-air

`airtime_ms` uses the Semtech LoRa formula (SX1276/SX126x application
notes): symbol time 2^SF / BW, preamble (n + 4.25) symbols, payload symbols
8 + max(ceil((8L - 4SF + 28 + 16 CRC - 20 IH) / (4 (SF - 2 DE))) * (CR + 4), 0)
with low-data-rate optimisation DE=1 when the symbol time exceeds 16 ms
(SF11/SF12 at 125 kHz), explicit header (IH=0). The mock and the sx1262
backend share this implementation; `tests/airtime_test.c` pins reference
values.

## Not in v0

FSK, LoRaWAN, frequency hopping, multiple radios, per-application arbitration
(first client wins, later ones share the same profile), and persistence of
the profile across restarts.
