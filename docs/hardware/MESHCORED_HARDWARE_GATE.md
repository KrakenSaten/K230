# The meshcored hardware gate: unit A

**Result: PASS on unit A, 2026-09-19.** The first time `meshcored` has run on
a radio. The complete path was exercised end to end, in both directions, with
two independent third-party MeshCore devices:

```
T-Deck / M5                    other people's MeshCore nodes
   |  over the air, 869.618 MHz
SX1262                         hardware
   |  SPI + GPIO
radiod                         owns the radio, exclusively (ADR-002)
   |  radio.* over pocketipc
meshcored                      owns the MeshCore protocol runtime
   |  mesh.* over pocketipc
a client                       `doors call`, for this gate
```

Scope: this gate is about the **daemon**. The wire format itself was accepted
earlier by the P0 over-the-air gate
([MESHCORE_INTEROP_GATE.md](MESHCORE_INTEROP_GATE.md)), which used a host tool
and no service at all. What is new here is that a long-running protocol
service acquired the radio, applied the profile, learned nodes, transmitted,
kept receiving, restarted without losing its identity, and answered a real
peer.

No implementation was changed, no card was flashed, no image, package or
release path was used, and VERSION stayed 0.0.10.

## Provenance

| Item | Value |
| --- | --- |
| Repository master during the gate | `e2418051694a6273a00a21e9fb1c3e18fa8118d1` |
| Doors version | 0.0.10 (unchanged by this gate) |
| Unit A base runtime | Doors 0.0.10, `BUILD_ID=646dcbb` (the flashed image) |
| Bench-deployed `radiod` | `e241805`, sha256 `06c7d67835c14d7b7261bc9ab0cb82a04ccd2dbd9e3d4b3c4b1c5c3858140a37` |
| Bench-deployed `meshcored` | `e241805`, sha256 `d824449015c7c64a09d89386b2b8416f94bcf072e3ac709ffecc8ccbea608a83` |
| Vendored protocol pins | RIFT `3ca7e3f003bd270587b1d92e33ce999542dcb5b4`, Crypto `37a76b8f7516568e1c575b6dc9268da1ccaac6b6` |
| Toolchain | Xuantie `riscv64-unknown-linux-gnu-gcc` 14.1.1 (V3.0.2 B-20250410), `k230_pocketos_defconfig` sysroot |

The pins are not asserted from the build host: `mesh.info` on the unit
reported them back out of the running service, and they are the pins the
accepted P0 gate used.

The binaries were cross-built from `git archive e241805` in a clean copy, with
a `BUILD_ID` file so the binaries name the commit rather than a dirty working
tree. Both reported `build=e241805` on the unit.

### Why radiod was deployed as well

`meshcored` speaks `radio.acquire`, `radio.configure`, `radio.subscribe` and
`radio.send_async`. The flashed unit build answers two of those with
`unknown method`:

```
doors call radiod radio.send_async   ->  failed (code 1): unknown method radio.send_async
doors call radiod radio.lease        ->  failed (code 1): unknown method radio.lease
```

The asynchronous transmit API and the lease arrived after `646dcbb`
([docs/api/radio.md](../api/radio.md)), so `meshcored` cannot run against the
image on the card. The minimum artifact set for this gate was therefore **two
files**, sent over SSH and installed by hand:

    /usr/sbin/radiod      (e241805, built with ENABLE_SX1262=1)
    /usr/sbin/meshcored   (e241805, built with ENABLE_MESHCORED=1)

`platforms/k230/scripts/deploy.sh` was **not** used: it carries the whole
userspace and the init script, and neither was wanted here. No `S65meshcored`
was installed and no `/etc/default/meshcored` was written, so the service
cannot start by itself and did not survive the session. The rest of the
userspace on the unit is still `646dcbb` - this is a deliberately mixed bench
userspace, not a release.

**The licensing and packaging gate was not weakened, bypassed or exercised.**
`make install`, the Buildroot package and `build_image.sh` were never invoked;
only the two binary targets were built. `meshcored-shipping-check` is a
prerequisite of `install` ([MESHCORED.md](../services/MESHCORED.md)), and
`install` did not run.

### A deployment fault of the gate's own making

Recorded because it is in the logs, and because the next reader would
otherwise take five `ERROR` lines for a defect.

The first `radiod` sent to the unit was **mis-built by this gate**, not by the
tree. `ENABLE_SX1262=1` was added to a second `make` invocation that relinked
without recompiling; `-DPOCKETOS_HAVE_SX1262` is an `ALL_CFLAGS` define, so a
stale `services/radiod/main.o` produced a binary that linked the backend and
then refused to select it:

```
17:06:39Z radiod ERROR backend sx1262 was not compiled in (ENABLE_SX1262=1)
```

`rc=2`, five supervisor restarts over 16 s. The unit's own `radiod` was
restored at 17:07:10Z, the binary was rebuilt from wiped objects, and the
correct one was installed at 17:08:14Z. No transmit happened, `meshcored` was
not running, and nothing else was touched.

Two things are worth keeping from it: **wipe every object before a
differently-configured cross-build**, and `radiod` behaved correctly - it
refused to start and said exactly why, rather than falling back to the mock
backend on a unit whose operator had asked for a real radio.

## The peers

| | Hardware | Standing |
| --- | --- | --- |
| `Tdeck RIFT` | LILYGO T-Deck, RIFT v0.9.5 | the known-good reference peer, same device and **same public key** as the P0 gate |
| `Mstr_m5` | third-party M5 device | a live neighbouring MeshCore node, nothing of ours on it |
| `NO-0589 Oslo/Bjerke` | third-party repeater (`ADV_TYPE` 2) | heard only; a public repeater on the live network |

## Radio profile

Requested on the `meshcored` command line and **read back out of `radiod`**
field by field before any transmit. `mesh.status.radio.profile` and
`radio.status.profile` agreed exactly.

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

These are `meshcored`'s compiled defaults; they were passed explicitly anyway,
so the command line records what was asked for. `radiod` ran on the real
`sx1262` backend, EU868, from the `/etc/default/radiod` the P0 gate left in
place. The owner confirmed an 868 MHz antenna on MMCX1 before any transmit
(BRINGUP_CHECKLIST.md section 5).

## 1. Startup, lease, configure, online

Started by hand, not by an init script:

```
meshcored --name K230-A --frequency-mhz 869.618 --bandwidth-khz 62.5 \
          --spreading-factor 8 --coding-rate 5 --sync-word 0x12 \
          --preamble 32 --tx-power-dbm 2 --verbose
```

| Observation | Value |
| --- | --- |
| `starting` to `online` | **99 ms** (17:08:55.292Z to 17:08:55.391Z) |
| Transitions | `starting` -> `waiting_for_radiod` -> `configuring` -> `online` |
| Lease | acquired, `owner_id 1`; `radio.lease` reported `held: true, owner: "meshcored"` |
| Profile | applied, and read back identical to the table above |
| `radio.radio_state` | `rx` |
| Processes | one, pid 1598; no restart, no crash loop |
| WARN or ERROR | **0** |

`radiod` reported the lease from its own side in the same second:

```
17:08:55.363Z radiod INFO  radio leased by meshcored (owner_id 1)
```

## 2. The identity

Generated on first start, from the host CSPRNG, and **persistent**:

| Item | Value |
| --- | --- |
| Name | `K230-A` |
| Public key | `19f7b327a254fe8a2565578a29a5d40b1202536f2cca3243f869972b59043715` |
| Node hash | `19` |
| File | `/var/lib/pocketos/meshcored/identity.id`, 96 bytes, mode 0600 in a 0700 directory |
| sha256 of the file | `41e8a50a854f9f51446844fd98ce8b35974531559aa1d80999e457eabe678558` |

Unlike the P0 gate's disposable frame identity, **this key is real and it
stays on the unit.** Two independent nodes now hold `K230-A` as a contact, and
the private key lives in the file above. It is this unit's identity until
somebody deletes it.

## 3. Passive receive, before any transmit

Nothing was transmitted for the first 90 s. Ten frames arrived, and a node was
learned from the live network before the owner did anything at all.

| Observation | Value |
| --- | --- |
| Frames in the first 90 s | 10 |
| `rx_delivered` | 10 of 10 |
| `rx_rejected` / `rx_dropped` | **0 / 0** |
| `rx_crc_errors` (radiod) | **0** |

Nodes learned, each from a signed advert that MeshCore verified before adding
the contact:

| Node | Hash | Public key | Type | RSSI | SNR |
| --- | --- | --- | --- | --- | --- |
| `Mstr_m5` | `c0` | `c00dadc0d5e87ed5bde67a8e150e54e92016a700a2223be8d41beff6fc0a0e7b` | 1 (chat) | -31 dBm | 10.75 dB |
| `Tdeck RIFT` | `36` | `367bff1533c80d7c727d7f9505d833e2db5b0d58dca9f03244e8bb56c1133d50` | 1 (chat) | -33 dBm | 12 dB |

**The T-Deck key is byte for byte the one the P0 gate recorded.** The same
physical peer, learned this time by a daemon rather than parsed by a host
tool.

`Mstr_m5` was learned unprompted; `Tdeck RIFT` arrived after the owner sent
one advert from the T-Deck on request.

On direct versus relayed: both nodes reported `path_known: false` at this
point, so `mesh.nodes` reported no `hops`, `direct` or `path_hex` - the API's
documented rule that an unknown value is absent rather than zero. The signal
figures (-33 dBm, 12 dB) match the P0 gate's *direct* reception figures
(-34 to -36 dBm, 11.75 to 13.25 dB) and are consistent with a zero-hop copy,
but **the service did not assert it and neither does this document.** A path
to `Mstr_m5` was learned later, from real traffic; see section 7.

## 4. The K230 advert

**Exactly one advert was transmitted, through `mesh.advert`.** `radiod` was
not bypassed, and the synchronous `radio.send` was not used at any point.

    mesh.advert -> MeshCore -> radio.send_async -> SX1262 -> radio.tx_done

| Item | Result |
| --- | --- |
| `mesh.advert` | `accepted: true` |
| Frame size | **109 bytes** |
| Airtime | **754.688 ms** (`radio.stats.tx_airtime_ms`) |
| `tx_submitted` / `tx_accepted` | 1 / 1 |
| `tx_ok` | **1** - which is `transmitted && rx_resumed`, both true |
| `tx_failed`, `tx_unknown`, `tx_rx_resume_failed`, `tx_refused`, `tx_done_unmatched` | **0** |
| `tx_packets` (radiod) | 0 -> 1 |
| Duty cycle, last hour | 0.021 % |
| State at +1 s and +4 s | `rx` |
| `radiod` pid | 1583, unchanged across the transmit |

The airtime is the same figure the P0 gate recorded for a 109-byte frame on
this profile. The on-air duration itself was not measured; only the computed
value is recorded.

**`tx_id` was not captured.** It travels on `radio.tx_done` and on the
`mesh.activity` event, and the unit had no event subscriber - `doors call` is
request and response only. The outcome was read from the counters and the logs
instead, which distinguish all four transmit results. Capturing `tx_id` needs
a subscribing client, and is listed as unproven below.

### Peer result

The T-Deck created a contact named

    K230-A

and the key prefix it displayed was `19f7b327...`, matching the identity
`meshcored` generated on the unit.

Evidence class: **operator** - the product owner read the T-Deck's own UI and
reported the contact and the matching prefix. No screenshot was taken, and the
peer-side RSSI and SNR for our frame were not recorded, so those remain
UNRESOLVED.

## 5. Receive after transmit

A `radiod` state of `rx` is not evidence that anything can still be decoded,
so the requirement was whole frames, parsed by the protocol core, after the
transmit.

| Observation | Value |
| --- | --- |
| Window | 120 s after the advert |
| `rx_events` / `rx_delivered` | 15 -> 29, every one delivered |
| `rx_rejected` / `rx_dropped` | **0 / 0** |
| `rx_crc_errors` | **0** |
| **`recv_flood`** (MeshCore's own dispatcher) | **14 -> 30**, read just after the window, at `rx_delivered 31` |
| Service state throughout | `online` |

`recv_flood` is counted by the MeshCore runtime, not by the IPC adapter, so
those sixteen frames were **parsed as MeshCore packets** after the K230 had
transmitted. Not merely `state=rx`.

## 6. Restart and identity

The service was stopped once with `SIGTERM` and started again with the same
arguments.

| Observation | Value |
| --- | --- |
| Shutdown | `shutting down (rc=0)`; node table written |
| Lease after stop | released - `radio.lease` reported `held: false` |
| `radiod` after stop | still `rx`, still running, pid unchanged |
| Restart | pid 2043, `meshcored: node K230-A, 2 known node(s)` |
| Public key after restart | `19f7b327...3715` - **the same** |
| `identity.id` sha256 | `41e8a50a...8558` - **unchanged** |
| Lease | reacquired, `owner_id 2` |
| Profile | reapplied, identical field for field |
| `starting` to `online` | 80 ms |
| Real RX after restart | within 15 s: `rx_delivered 1`, **`recv_flood 1`** |

The reloaded nodes carried their `last_advert_timestamp` but no
`last_heard_mono_ms`, `last_snr_db` or `last_rssi_dbm` - those are runtime
only and are documented as not persisted ([mesh.md](../api/mesh.md)). The
behaviour matched the contract.

## 7. Directed text, ACK and path learning

The optional step, attempted because everything above was clean. It went
further than asked, in a direction nobody planned.

**Attempt one, to the T-Deck.** `mesh.send` was accepted with `message_id 1`,
`route: flood` (no path was known), `ack_timeout_ms 15904`. 54 bytes were
transmitted, `tx_ok`. No ACK arrived, the message resolved to **`no_ack`** at
the deadline, and **nothing was retransmitted**. The cause is environmental
and was reported by the owner: **the T-Deck had lost power.** This is the
documented outcome for a peer that is not there, not a fault.

**Then the live neighbour answered instead.** `Mstr_m5` sent a directed text
to the K230:

| Item | Value |
| --- | --- |
| Text | `Hi` |
| From | `Mstr_m5`, `c00dadc0...0e7b` |
| Direction / state | `in` / `received` |
| RSSI / SNR | -34 dBm / 13.5 dB |
| Counter | `recv_direct` |

`meshcored` **answered without being asked**, which is what the protocol owes
a sender and the reason the service ships disabled: a 22-byte transmit at
17:18:37Z carrying the ACK and the return path. The node table then showed the
path it had learned:

    Mstr_m5   path_known: true   hops: 0   direct: true

**The reply then closed the loop.** A second `mesh.send`, to `Mstr_m5`:

| Item | Value |
| --- | --- |
| `route` | **`direct`** - over the path just learned, not flood |
| Frame | 38 bytes, `tx_ok` |
| `ack_timeout_ms` | 6814 |
| Final state | **`acked`** |
| Round trip | **1150 ms** (`mono_ms` 6230322 to `ack_mono_ms` 6231472) |

Directed text in, ACK out, directed text out, ACK in, and a path learned from
real traffic - with a third-party MeshCore device. The P0 gate listed directed
TXT_MSG, ACK and path-learning interoperability as NOT YET PROVEN; against
`Mstr_m5` they are now proven. **Against the T-Deck they are not**, because it
lost power before it could be tried.

## Final counters

`radiod`, from the start of the deployed build to the end of the gate:

| Counter | Value |
| --- | --- |
| `tx_packets` | **4** |
| `rx_packets` | **49** |
| `rx_crc_errors` | **0** |
| `tx_airtime_ms` | 1912.832 |
| `rx_airtime_ms` | 22971.392 |
| `duty_cycle_last_hour_percent` | **0.053 %** |

The four transmits are the advert, the text to the T-Deck, the ACK to
`Mstr_m5` and the reply to `Mstr_m5`. **Every frame that went on the air came
from `meshcored`**; nothing else transmitted during the gate.

`meshcored`, second run, at the end:

| Counter | Value |
| --- | --- |
| `rx_events` / `rx_delivered` | 17 / **17** |
| `rx_rejected` / `rx_dropped` | 0 / 0 |
| `tx_submitted` / `tx_accepted` / `tx_ok` | 3 / 3 / **3** |
| `tx_refused` / `tx_failed` / `tx_unknown` / `tx_done_unmatched` | 0 / 0 / 0 / 0 |
| `sent_flood` / `sent_direct` | **2 / 1** |
| `recv_flood` / `recv_direct` | **15 / 2** |
| `radiod_connects` / `radiod_disconnects` | 1 / 0 |
| `lease_acquired` / `lease_refused` / `lease_lost` | 1 / 0 / 0 |
| `path_payloads_refused` / `nodes_unretained` / `contacts_full` | 0 / 0 / 0 |
| `nodes` / `messages` | **3 / 3** |
| `packets_free` / `packets_total` | 32 / 32 |

The first run ended with 31 receives, 31 delivered, `recv_flood` 30 and one
transmit, `ok`.

## WARN and ERROR

| Source | Count | |
| --- | --- | --- |
| `meshcored`, both runs | **0 WARN, 0 ERROR** | 32 log lines in total |
| `radiod` `e241805`, whole run | **0 WARN, 0 ERROR** | |
| `radiod` log, whole file | 5 ERROR | all `backend sx1262 was not compiled in`, all from this gate's own mis-built binary, 17:06:39Z to 17:06:54Z; see Provenance |
| `doors-shell` | 4 WARN | all `radio.status poll failed`: two `Broken pipe` at the two `radiod` restarts, two 200 ms timeouts during `meshcored` transmits |
| Crash reports | **none** | |
| Unplanned restarts | **none** | `doors-shell` pid 387 was unchanged throughout, and survived both `radiod` restarts |

Six WARN lines that predate the gate (three from a boot-time Wi-Fi join, one
clock handoff, two from the P0 gate) are excluded from the counts above.

## Findings

Two, neither of which blocked the gate. **No code was changed, and nothing was
patched around on the unit.**

### 1. meshcored reports degraded while it is transmitting

`radiod` broadcasts `radio.state "tx"` when a transmit starts
(`services/radiod/main.c`, `set_state`). `meshcored`'s `note_radio_state`
(`services/meshcored/radio_link.c`, the `MCD_DEGRADED` transition) maps
**any** non-`rx` state to `degraded` while online, with no notion that the
non-`rx` state is its own doing. So every transmit produces two `mesh.state`
events and a `degraded` window:

| Frame | Degraded window |
| --- | --- |
| 109 bytes (advert) | 772 ms |
| 54 bytes | 485 ms |
| 22 bytes (ACK) | 319 ms |
| 38 bytes | 401 ms |

Four transmits out of four, each window tracking that frame's airtime.

**Impact is reporting only.** The transmit guard is the adapter's
`radio_online` flag, which entering `degraded` does not clear, so sends are
still accepted; receive continues; the state returns to `online` by itself.
But [mesh.md](../api/mesh.md) gives only two causes for `degraded` - `radiod`
in `error` after a failed receive re-entry, and a `tx_done` with
`rx_resumed: false` - and a self-induced `tx` is neither. A client watching
`mesh.state` will report "the radio is not usable" every time this node
speaks.

This is **not hardware-specific**: the mock backend models a `tx` state too,
so the same transition would occur on the host. It was simply never asserted
by either host suite. Class: **VERIFIED hardware**. The fix, if any, is a
decision for the next phase.

**Resolved after the gate**, on `fix/meshcored-tx-state`: a `tx` reported
while this service has a transmit outstanding is expected and keeps the state
`online`; every other non-`rx` state, and a `tx` with nothing outstanding,
still degrades ([mesh.md](../api/mesh.md), "Service state"). The observation
above is what unit A produced and is left as it was recorded; nothing on the
unit was changed then or since, and the fix is host-tested only - the four
transmit windows in the table have not been re-measured on hardware.

### 2. doors-shell radio.status timeouts during meshcored transmits

The shell's 200 ms poll deadline lapsed twice, once during the advert
(17:13:02.944Z) and once during the directed text (17:20:55.982Z) - two out of
the two transmits that happened while the shell was polling.

**This is recorded as an observation, not as a proven defect.** Whether
`radiod` is genuinely unresponsive for part of an *asynchronous* transmit is
not established by this gate; it needs a measurement that was not taken. The
symptom is the same one the P0 gate saw with the *synchronous* `radio.send`,
where blocking for the airtime was expected. Class: **UNRESOLVED**.

## What this gate proves

PROVEN on real hardware:

- `meshcored` acquires the `radiod` lease, applies the MeshCore profile and
  reaches `online` on a real SX1262.
- A persistent MeshCore identity is generated, stored and reloaded, and
  survives a restart byte for byte.
- Real MeshCore adverts from third-party nodes are received, verified and
  learned as nodes through `mesh.nodes`.
- The reference T-Deck is learned by the daemon, with the key the P0 gate
  recorded.
- One advert is transmitted through `mesh.advert` and `radio.send_async`, and
  completes as `tx_ok` - transmitted, and receiving again.
- A real MeshCore/RIFT peer accepts that advert and shows `K230-A` with the
  matching key prefix (operator).
- Whole frames are decoded after the transmit, by the protocol core.
- The lease is released on a clean stop and reacquired on restart.
- A directed text is received, ACKed unasked, and a return path is learned.
- A directed text is sent over that learned path and is ACKed in 1150 ms.
- 49 receives and 4 transmits with **zero** CRC errors, zero rejected frames,
  zero dropped frames and zero transmit failures.
- No crash, no crash loop, no unplanned restart, and `doors-shell` untouched
  throughout.

## What remains unproven

- **Every failure path.** `tx_failed`, `tx_unknown`, `tx_refused`,
  `tx_rx_resume_failed`, `tx_done_unmatched`, `rx_rejected`, `rx_dropped`,
  `lease_refused`, `lease_lost`, `path_payloads_refused`, `nodes_unretained`
  and `contacts_full` are all still **0** on hardware. They are host-verified
  only.
- **`tx_id` and the `mesh.activity` event stream.** No subscribing client ran
  on the unit.
- **K230 to T-Deck directed text and ACK.** The T-Deck lost power; only the
  advert direction was confirmed there.
- **Multi-hop origination from the K230.** No relayed copy of a K230 frame was
  observed; every confirmed reception of our traffic was zero-hop.
- **Soak, and a busy network.** The service ran for about 12 minutes in total,
  across two runs, on a quiet band.
- **Real duty-cycle limits.** 0.053 % of the last hour is far below anything
  that would exercise the dispatcher's airtime budget.
- **RF power, spectrum and harmonics.** Not measured. Peer-side RSSI and SNR
  for our frames were not captured.
- **The init-script and reboot path.** `S65meshcored` was never installed,
  `/etc/default/meshcored` was never written, and the service was never
  started by `rcS` or carried across a reboot. The supervisor and crash-loop
  paths were not exercised for `meshcored`.
- **Lease contention.** No second client competed for the radio; `meshcored`
  was never made to wait in `waiting_for_lease`.
- **A `radiod` restart while `meshcored` is active**, and therefore the
  reconnect, `tx_unknown` and lease-recovery paths on hardware.
- **Routed path-hash-size interoperability.** The P0 gate's note stands: RIFT
  uses two-byte path hashes, and no K230 frame carrying real path entries has
  been on the air.
- **The `state.v1` quarantine path.** No corrupt node table was encountered.
- Whether `radiod` is briefly unresponsive during an asynchronous transmit
  (finding 2).

## Final unit state

Left deliberately, and exactly:

- **`radiod` `e241805` running** on the real SX1262, pid 1583 under its
  supervisor, `--backend sx1262 --region EU868 --tx-power-dbm 2`, state `rx`.
  The running daemon still carries the MeshCore profile; a `radiod` restart
  returns it to the PocketOS defaults, as the P0 gate also recorded.
- **`meshcored` installed at `/usr/sbin/meshcored`, stopped.** Clean shutdown,
  `rc=0`.
- **The lease is released.** `radio.lease` reports `held: false`; the radio is
  free for any other client.
- **No `meshcored` init script, and no `/etc/default/meshcored`.** The service
  cannot start on its own, and a reboot leaves it off.
- **The `K230-A` identity remains on the unit**, at
  `/var/lib/pocketos/meshcored/identity.id` (0600, in a 0700 directory),
  together with `state.v1` holding three known nodes.
- **The rest of the userspace is still `646dcbb`** - a mixed bench userspace,
  not a release. The flashed card was not touched.
- The unit's original `radiod` is kept at `/root/gate-backup/radiod-646dcbb`
  (sha256 `84ed3a1309bd1336aba99e530a25c69c426de67ece9414cc1b89fa5bdd9517c3`)
  for reversal.
- `/etc/default/radiod` is unchanged from the P0 gate. Disarm command, **for
  reference only - not executed**:

```sh
ssh root@192.168.10.157 "rm /etc/default/radiod && /etc/init.d/S60radiod restart"
```

## Verdict

**meshcored on real hardware: PASS**

Date: 2026-09-19
