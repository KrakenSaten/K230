# RIFT Channels on unit A: the two-node on-air gate

**Status, 2026-09-21: A, B and C are ON-AIR VERIFIED. Only D remains.**

| | |
| --- | --- |
| The local, device-only half | **RUN and GREEN** — "The device run" |
| A — T-Deck → unit A | **ON-AIR VERIFIED.** First shown accidentally, then executed properly inside part C: deliberate sends with the thread closed, unread 0 → 1 twice, cleared on opening. |
| B — unit A → T-Deck | **ON-AIR VERIFIED** — executed deliberately |
| C — channel and direct together | **ON-AIR VERIFIED** |
| D — restart with both kinds live | channel-only restart verified twice; with a peer and direct traffic, **not run** |

Group frames built by this code cross real RF to an independently built
MeshCore implementation and back, on the MeshCore profile at 2 dBm; and a
channel conversation and a direct one run side by side on the same node with
their own delivery semantics intact. What has not been shown is that both
survive a restart of the service together.

**The build under test is `feat/rift-channels`**, from origin/master
`a120f8b`. VERSION stays `0.0.10`. Not merged, and not to be merged unless
this gate passes.

Scope: MeshCore group channels, end to end — `protocols/meshcore` →
`meshcored` → `mesh.*` → RIFT COMMS. Direct messages must be unchanged, which
is half of what part C checks.

## What is already proven, and what this gate is for

Keep these apart. Everything above the line has been demonstrated; nothing
below it has.

| | |
| --- | --- |
| **HOST VERIFIED** | The protocol, the service, the IPC and the app, on a build host: **1 865 named checks**, 0 failures, from a clean rebuild — plus the lints and the two `meshcored` integration suites, which count failures rather than checks. The RIFT suites run again under ASan + UBSan with leak detection. Two whole `meshcored` processes exchange channel messages over a mock air. The channel key never leaves the service. Channels survive a restart. |
| **HOST VERIFIED** | The target build: `libmeshcore-riscv64.a`, and `meshcored`, `radiod` and `doors-shell` linked for riscv64 from wiped objects. |
| **DEVICE VERIFIED**, 2026-09-21 | The service and the app on unit A itself: the channel created with the predicted hash, persisted, restarted, removed and re-added; the key confined; one real frame transmitted on the SX1262; and every string on the panel read off its own DRM plane. "The device run" below. |
| **ON-AIR VERIFIED**, one direction, **outside the procedure** | The T-Deck's `#doorsbench` frame received and decrypted by unit A. Proves the two implementations derive the same key from the channel name. Evidence, not a gate result — "The unplanned reception". |
| **ON-AIR VERIFIED**, the other direction, **executed deliberately** | A group frame unit A put on the air, received on `#doorsbench` by the peer — part B below. |
| **STILL NOT VERIFIED** | That channels and direct messages stay correct alongside each other (C), and that both survive a restart together (D). |

The accepted P0 interop gate
([MESHCORE_INTEROP_GATE.md](MESHCORE_INTEROP_GATE.md)) proved adverts and
direct text against the same peer, on the same profile; it says nothing about
group frames, which are a different payload type and a different routing mode.

## The peers

| | Unit A | The second node |
| --- | --- | --- |
| Hardware | LILYGO T-Display-K230 | LILYGO T-Deck |
| Software | Doors, `feat/rift-channels` | RIFT v0.9.5 — the known-good interoperable peer |
| Role | the node under test | the reference implementation |

The second node needs **no modification and no new firmware**. RIFT v0.9.5
already has channels; this gate only asks it to join one.

## The bench channel

Both ends must hold the same key. The shortest way to arrange that is a
**hashtag channel**, where the key is derived from the name — so nobody types
a key on the T-Deck at all.

| | |
| --- | --- |
| Name | `#doorsbench` |
| Key | `SHA-256("#doorsbench")[0..15]` = `3de64ba5520d2ab2657e254a73c0892c` |
| Key, base64 | `PeZLpVINKrJlfiVKc8CJLA==` |
| Channel hash on the air | `9a` |
| Key length | 128-bit |

The derivation is the peer firmware's, not ours:
`MyMesh::addGroupChannelHashtag` takes the first 16 bytes of SHA-256 over the
name **including** its leading `#`, and publishes its own test vector
(`"#test"` → `9cd8fcf2…`). `tests/meshcore_core_test.cpp`,
`test_hashtag_channel_vector`, checks that our SHA-256 reproduces that vector
and that the two values in this table are what our crypto actually produces —
so if the two ends disagree on the bench, that test says the arithmetic was
not the reason.

> **This key is not a secret and must never be treated as one.** It is
> derived from a public name, it is written in a file in a git repository, and
> the peer firmware's own UI warns that anyone who knows the name can read the
> traffic. It is a bench channel for one test. Step 9 removes it.

## Before you start

Unit A must be reachable over SSH and on the real radio backend.

**As of 2026-09-21 the payload is already installed and `#doorsbench` is
already configured** — see "The device run". Steps 1 to 3 below are the record
of how it got there and what to do on a unit that has been reflashed since;
they do not need repeating on a unit still in that state. Step 1's survey is
worth running anyway, because it is the rollback record.

`/etc/default/radiod` decides the backend, and a reflash removes it —
`S60radiod` then defaults to `RADIOD_BACKEND=mock`, which cannot transmit. On
2026-09-21 it was found already set to `sx1262`/`EU868`/2 dBm by the interop
gate and **was not modified**. Confirm it rather than assume it.

## The payload

Three files, built from the branch tip for riscv64. `platforms/k230/scripts/deploy.sh`
is **not** used: it carries the whole userspace and the init scripts, and
neither is wanted. No `S65meshcored` is installed and no `/etc/default/meshcored`
is written, so `meshcored` cannot start by itself and does not survive a
reboot — which is what makes this reversible.

| Path on the unit | Why |
| --- | --- |
| `/usr/bin/doors-shell` | RIFT is compiled into the shell; the channel screens are in here |
| `/usr/sbin/meshcored` | the service with channels |
| `/usr/sbin/radiod` | unchanged by this branch, and carried anyway: `meshcored` needs `radio.send_async` and the lease, which arrived after the image on the card, and installing the matching one is cheaper than finding out at step 3b |

**The licensing gate is not touched.** `make install`, the Buildroot package
and `build_image.sh` are never invoked; only the binary targets are built.
`meshcored-shipping-check` is a prerequisite of `install`, and `install` does
not run. See `docs/LICENSING.md` open item 9.

The artefacts and their hashes are in **Provenance** at the end of this sheet.

---

# The procedure

Ten steps. Steps 1–3b are typing and a self-check; 4–8 are the test with the
second node; 9–10 put it back. Only steps 4 to 7 need both radios and a pair
of eyes.

Throughout: `A=root@<unit A address>`.

### 1. Survey — record what is there before anything changes

```bash
ssh $A 'cat /etc/doors-release; echo ---; cat /etc/default/radiod 2>/dev/null || echo "NO /etc/default/radiod (backend defaults to mock)"; echo ---; doors radio info | head -8; echo ---; doors call sysd system.info | head -6; echo ---; ls -l /usr/bin/doors-shell /usr/sbin/radiod /usr/sbin/meshcored 2>&1; echo ---; sha256sum /usr/bin/doors-shell /usr/sbin/radiod /usr/sbin/meshcored 2>/dev/null; echo ---; ls /run/pocketos/*.crashloop 2>/dev/null || echo "no crashloop markers"'
```

Keep that output. It is the rollback record and the "before" health reading.

If `/etc/default/radiod` does not say `RADIOD_BACKEND=sx1262`, write it:

```bash
ssh $A 'printf "RADIOD_BACKEND=sx1262\n" > /etc/default/radiod && /etc/init.d/S60radiod restart && sleep 3 && doors radio info | head -5'
```

### 2. Keep a rollback copy on the unit

```bash
ssh $A 'mkdir -p /root/rollback && cp -a /usr/bin/doors-shell /root/rollback/ && cp -a /usr/sbin/radiod /root/rollback/ 2>/dev/null; ls -l /root/rollback'
```

### 3. Install the payload

From the build host, with the artefacts in `$OUT` (see Provenance):

```bash
ssh $A '/etc/init.d/S90doors-shell stop' && tar -C "$OUT" --owner=0 --group=0 --numeric-owner -cf - doors-shell meshcored radiod | ssh $A 'tar -C /tmp -xf - && install -m 0755 -o 0 -g 0 /tmp/doors-shell /usr/bin/doors-shell && install -m 0755 -o 0 -g 0 /tmp/meshcored /usr/sbin/meshcored && install -m 0755 -o 0 -g 0 /tmp/radiod /usr/sbin/radiod && rm -f /tmp/doors-shell /tmp/meshcored /tmp/radiod && sync && sha256sum /usr/bin/doors-shell /usr/sbin/meshcored /usr/sbin/radiod && /etc/init.d/S60radiod restart && sleep 3 && /etc/init.d/S90doors-shell start'
```

Confirm the three hashes match Provenance. Then start the service and join
the channel:

```bash
ssh $A 'nohup meshcored --name K230-A --frequency-mhz 869.618 --bandwidth-khz 62.5 \
          --spreading-factor 8 --coding-rate 5 --sync-word 0x12 \
          --preamble 32 --tx-power-dbm 2 --verbose \
          >/tmp/meshcored-bench.log 2>&1 & sleep 4; doors call meshcored mesh.status | head -20'
ssh $A "doors call meshcored mesh.channel_add 'name=#doorsbench' 'key=PeZLpVINKrJlfiVKc8CJLA=='"
```

`nohup` and the redirect are not decoration: the process has to outlive the
SSH session that started it, and it has no init script to hold it. The profile
is given explicitly rather than left to the defaults — they are the same seven
values, and a gate that assumed them would not be evidence that they were
used. It is the same invocation the accepted `meshcored` hardware gate ran.

`mesh.status` must reach `"state":"online"` with the lease held. If it says
`degraded` or the lease is elsewhere, stop here: nothing below will transmit.

`doors call` takes `key=value` pairs rather than a JSON object, and a value
that parses as a number is sent as one — which is why `channel=0` below is a
slot and not the string `"0"` (`tools/pos/pos_radio.c`,
`pos_params_from_kv`). The base64 key keeps its trailing `==`: only the first
`=` separates the pair.

The answer must carry `"channel_hash":"9a"` and `"key_bits":128`, and must
**not** contain the key.

Local checks worth doing while you are here (none of them needs the peer):

```bash
ssh $A 'doors call meshcored mesh.channels; echo ---; ls -l /var/lib/pocketos/meshcored/; echo ---; ls /var/lib/pocketos/meshcored/*.tmp 2>/dev/null || echo "no stray .tmp"; echo --- ; grep -rlF -e "PeZLpVIN" -e "3de64ba5" -e "3DE64BA5" /tmp/meshcored-bench.log /var/log/ 2>/dev/null || echo "KEY NOT FOUND IN ANY LOG (this is the wanted answer)"'
```

`channels.v1` must be `-rw-------` and the key must not be in the log.

### 3b. The transmit path, before the peer is involved

One command, and it is worth the thirty seconds: it separates "unit A cannot
transmit" from "the two nodes are not on the same channel", which are the two
ways the rest of this sheet can fail and which look identical from the T-Deck.

```bash
ssh $A "doors call meshcored mesh.send channel=0 'text=bench tx check'; sleep 3; doors call meshcored mesh.status | head -30; doors call meshcored mesh.messages | tail -30"
```

| Must be true | |
| --- | --- |
| the answer has `"route":"flood"`, `"ack_expected":false` | |
| and **no** `ack_timeout_ms` | there is no ACK to time out |
| `counters.tx_ok` went up by one | the bytes left the radio |
| the message reads `"state":"sent_flood"` | and stays there |
| `radio_state` is back to `rx` | |

**This is LOCAL TX PATH VERIFIED and nothing more.** A frame was modulated and
sent; whether anything received it is not knowable from this end, and a
channel frame is never acknowledged. Do not write it down as delivery.

If `tx_ok` did not move, stop: the backend is wrong or the lease is not held,
and steps 5 and 6 cannot work.

### 4. Second node — join the same channel

On the T-Deck, RIFT v0.9.5:

1. **COMMS** → add channel (or the system menu's channel item).
2. Screen **NEW CHANNEL** → type `doorsbench` — **no `#`**, the firmware adds
   it — then ENTER.
3. Screen **CHANNEL KEY** → choose the first option, **"Hashtag - open
   topic"** → ENTER.

The T-Deck now holds `#doorsbench`. Nothing else on it changes.

### 5. A — second node → unit A

Send a message on `#doorsbench` from the T-Deck. On unit A, in RIFT → COMMS:

| Must be true | |
| --- | --- |
| a `#doorsbench` row is in the conversation list, with the `#` glyph | |
| the message appears **once** | not twice, and not in a direct conversation |
| the unread pill increments | |
| opening the thread clears it | |
| the sender's name is shown as a **claim** — with a trailing `?` | it is unauthenticated, and that is the design |
| the header says `CHANNEL · HASH 9a · FLOOD` | `9a` is the agreement between the two nodes |
| nothing anywhere says DELIVERED or ACK | |

### 6. B — unit A → second node

Compose in RIFT's COMMS on the open `#doorsbench` thread and send.

| Must be true | |
| --- | --- |
| the T-Deck receives it, on `#doorsbench` | read its screen |
| RIFT shows `SENT · FLOOD · NO ACK ON CHANNELS` | |
| RIFT **never** shows DELIVERED, ACKED, or a NO ACK that implies a timeout | |
| the route column on the row says `FLOOD` | |

### 7. C — interleaving

In this order, so both kinds are live at once:

1. a channel message (either direction),
2. a **direct** message between unit A and the T-Deck,
3. another channel message.

| Must be true | |
| --- | --- |
| each message lands in its own conversation | no crossover |
| the direct one gets an ACK and reads `DELIVERED` | direct semantics unchanged |
| the channel ones do not | |
| both conversations keep their own unread counts | |

### 8. D — restart

```bash
ssh $A 'killall meshcored; sleep 2; nohup meshcored --name K230-A --frequency-mhz 869.618 \
          --bandwidth-khz 62.5 --spreading-factor 8 --coding-rate 5 --sync-word 0x12 \
          --preamble 32 --tx-power-dbm 2 --verbose \
          >>/tmp/meshcored-bench.log 2>&1 & sleep 5; doors call meshcored mesh.channels'
```

| Must be true | |
| --- | --- |
| `#doorsbench` comes back, in the same slot, with hash `9a` | it was written on the join, not on a timer |
| RIFT reconnects on its own | |
| the channel row is still there, and still a channel | |
| a message each way still works, both kinds | |

Then the post-test health reading:

```bash
ssh $A 'doors call sysd system.status | head -30; echo ---; ls /run/pocketos/*.crashloop 2>/dev/null || echo "no crashloop markers"; echo ---; grep -c ERROR /var/log/pocketos/*.log 2>/dev/null; echo ---; doors radio info | head -6'
```

| Must be true | |
| --- | --- |
| `radiod`, `sysd`, the shell all alive | |
| no crashloop marker | |
| no ERROR burst that is not explained | |
| the SX1262 backend still healthy | |

### 9. Leave the bench channel

```bash
ssh $A 'doors call meshcored mesh.channel_remove channel=0; doors call meshcored mesh.channels'
```

Delete it on the T-Deck too (system menu → delete channel).

### 10. Stop the service, or roll back

`meshcored` was started by hand and has no init script, so stopping it is the
whole of the undo:

```bash
ssh $A 'killall meshcored; sleep 1; doors call meshcored mesh.info 2>&1 | head -2'
```

**Full rollback**, if anything went wrong:

```bash
ssh $A '/etc/init.d/S90doors-shell stop; killall meshcored; cp -a /root/rollback/doors-shell /usr/bin/doors-shell; cp -a /root/rollback/radiod /usr/sbin/radiod; rm -f /usr/sbin/meshcored /var/lib/pocketos/meshcored/channels.v1; sync; /etc/init.d/S60radiod restart; /etc/init.d/S90doors-shell start; sha256sum /usr/bin/doors-shell /usr/sbin/radiod; ls -l /var/lib/pocketos/meshcored/'
```

Those two hashes must match the "before" reading from step 1.

> **Do not delete `/var/lib/pocketos/meshcored/`.** It holds `identity.id`,
> which is this node's MeshCore private key — generated on first start,
> persistent, and **the thing every peer knows it by**
> (`19f7b327…` as of the accepted `meshcored` gate, and the T-Deck holds a
> contact for it). Removing it would not reset the unit; it would make it a
> different node that every peer has to rediscover, and the key cannot be
> recovered. Only `channels.v1` — the bench channel, and nothing else — is
> removed above.

Nothing else was changed: no init script was added, no `/etc/default/meshcored`
was written, no image was built and the SD card was never touched.

---

## The device run — 2026-09-21

Unit A, `192.168.10.157`. Everything in this section is **DEVICE VERIFIED**:
observed on the unit, over SSH or off its own DRM plane. Nothing here rests on
the build host.

### What was there first, and what changed

| | Before | After |
| --- | --- | --- |
| `/etc/doors-release` | `0.0.10`, `646dcbb` | untouched |
| `/usr/bin/doors-shell` | `3d5d3fe2…` | `37ce3f60…` |
| `/usr/sbin/meshcored` | `d8244490…` (`e241805`) | `6d6f3cf4…` |
| `/usr/sbin/radiod` | `06c7d678…` (`e241805`) | `e2371703…` |
| `/etc/default/radiod` | `sx1262`, `EU868`, 2 dBm | **untouched — nothing was configured** |
| `identity.id` | `19f7b327…`, sha256 `41e8a50a…` | **untouched**, same mtime |
| `channels.v1` | absent | `#doorsbench`, 80 bytes |

The three installed hashes match the build host byte for byte, and all three
report `0.0.10` / `765a3a3` when asked — `doors-shell` through `shell.info`,
`radiod` through `radio info`, `meshcored` through `mesh.info`. The `doors`
CLI still reports `646dcbb`: it was not part of the payload and was not
replaced.

Rollback copies of all three are on the unit at `/root/rollback-channels/`.

### Health

`meshcored` reached `online` **82 ms** after start (`starting` →
`waiting_for_radiod` → `configuring` → `online`), took the radio lease
(`owner_id 1`), and applied the profile exactly: 869.618 MHz, 62.5 kHz, SF8,
CR5, sync word 18 (`0x12`), preamble 32, 2 dBm, CRC on. 32 nodes restored from
`state.v1`. **0 WARN and 0 ERROR** from `meshcored` across the whole session.

No crashloop markers. `radiod`, `sysd`, `netd` and `doors-shell` all alive
before and after.

The two `ERROR` matches in `/var/log/messages` at this point were **not** what
an earlier revision of this sheet said they were. They are `ntpd` lines —
`kernel reports TIME_ERROR: 0x41: Clock Unsynchronized`, at `daemon.info`
level — and the grep matched `ERROR` inside `TIME_ERROR`. The sheet previously
attributed them to the Goodix touch-firmware messages, which are real boot
noise but log as `[GTP-ERR]` and `user.err` and match nothing. Corrected here
rather than quietly: a gate sheet that misidentifies its own log lines is
teaching the next reader to misread them.

### Channels, on the device

| Check | Result |
| --- | --- |
| `mesh.channel_add` | slot 0, `channel_hash 9a`, `key_bits 128`, `text_limit 152`, `ack_expected false` |
| The hash | **`9a`** — the value `test_hashtag_channel_vector` predicts on the host, derived independently here |
| `text_limit 152` | 160 − `len("K230-A: ")`; the node's own name, not a constant |
| `mesh.channels` | one channel, `count 1`, `max 8`, `persistent true` |
| `mesh.channel channel=0` | the same object |
| An empty slot | refused, code 2, "there is no channel in slot 3" |
| The same key twice | refused, code 2 |
| A 32-byte key with an all-zero upper half | **refused**, code 2, with the reason — the load/add asymmetry fixed in `765a3a3`, demonstrated on hardware |
| `channels.v1` | 80 bytes = 12-byte header + one 68-byte record, `-rw-------`, `root:root` |
| its directory | `drwx------`, `root:root` |
| stray `.tmp` | none |

### The key does not leave the service

Proven on the unit, with a control that makes the negative meaningful:

- **Control:** the key's bytes **are** found in `channels.v1`, so the search
  can fail.
- `mesh.info`, `mesh.status`, `mesh.identity`, `mesh.nodes`, `mesh.channels`,
  `mesh.channel`, `mesh.messages` — **clean**, none carries it.
- `/tmp/meshcored-bench.log` (a `--verbose` run) and `/var/log/messages` —
  **clean**.
- Nothing on the panel shows it (see the capture).

### Restart, remove, re-add

Restarting `meshcored` brought `#doorsbench` back in the same slot with the
same hash, and the log said so: `node K230-A, 32 known node(s), 1 channel(s)`.
Identity unchanged.

`mesh.channel_remove` answered `key_forgotten: true`, and the key was then
**absent from `channels.v1`**, which had shrunk to its 12-byte header. That is
the claim being true at the moment it is made, which is what the immediate
persist in `765a3a3` is for. Re-adding restored slot 0 and hash `9a`.

### The transmit — LOCAL TX PATH VERIFIED

One real frame, on the antenna the owner confirmed on MMCX1.

```
mesh.send channel=0 text=doors bench tx 1
  -> {"accepted":true,"message_id":1,"route":"flood","channel":0,"ack_expected":false}
```

No `ack_timeout_ms` in the answer, because there is no ACK to time out.

| Counter | Before | After |
| --- | --- | --- |
| `tx_submitted` | 0 | 1 |
| `tx_accepted` | 0 | 1 |
| `tx_ok` | 0 | **1** |
| `tx_failed`, `tx_refused` | 0 | 0 |
| `sent_flood` | 0 | 1 |

`meshcore: tx 37 bytes` in the log; `radio_state` back to `rx`. The message
holds `state: sent_flood`, `ack_expected: false`, and stays there.

**This is LOCAL TX PATH VERIFIED and not delivery.** 37 bytes were modulated
and `radiod` reported the transmit complete. Whether anything received it is
not knowable from this end, and never will be for a channel frame.

### What the panel actually says

Read off unit A's own DRM plane (`ffmpeg -f kmsgrab`), not from a description:
[`shots/rift-channels-unitA-comms-2026-09-21.png`](shots/rift-channels-unitA-comms-2026-09-21.png).
Landscape, so this is the three-pane layout.

| Element | On the panel |
| --- | --- |
| List row | `# #doorsbench` with the `#` glyph, route column `FLOOD` |
| Thread header | `# #doorsbench  CHANNEL · HASH 9a · FLOOD` |
| Our message | byline `you`, body `K230-A: doors bench tx 1` |
| its caption | `SENT · FLOOD · NO ACK ON CHANNELS` |
| Received message | byline **`T-Deck-RIFT?`** — with the claim marker |
| its caption | `RECEIVED · −27 dBm · SNR 12.0` |
| List note | `0 direct · 1 channel · nothing acknowledges a channel message` |
| Thread note | `This history is the radio service's, and it does not survive a restart of it.` |
| Route pane | `A channel is a shared key, not a route. Messages are flooded to every node that holds the same key; there is no path to show and nothing acknowledges them.` |
| | `HASH 9a · 128-BIT KEY` |
| | `1 SENT · NOTHING ACKNOWLEDGES A CHANNEL` |
| Command line | `TO #doorsbench · TAB TO WRITE · ↑↓ CHOOSE` |

**The words `DELIVERED` and `ACKED` do not appear anywhere on the panel.** The
only occurrences of "ACK" are `NO ACK ON CHANNELS` and `NOTHING ACKNOWLEDGES A
CHANNEL`.

The product owner separately confirmed the physical rendering — no clipping,
no wrong glyph, no wrong state — and asked that the exact values be read from
the device rather than from their report. They were; that is this table.

## The unplanned reception

**A channel message arrived over the air from a second node, and it was not
part of this procedure.**

At 15:37, between the local transmit and the panel capture, `meshcored`
received, MAC-verified and decrypted a `#doorsbench` frame:

```
id 2, direction in, kind channel, channel 0, channel_name "#doorsbench",
channel_hash "9a", sender_name "T-Deck-RIFT", text "T-Deck-RIFT: test",
state "received", ack_expected false, snr_db 12, rssi_dbm -27
```

Counters at that point: `rx_events 25`, `rx_delivered 25`, `rx_rejected 0`,
`rx_dropped 0`, `recv_flood 25`, and **`channel_frames_unmatched 19`** — so
nineteen group frames in range belonged to channels this node does not hold,
and were correctly ignored, while the one that matched decrypted.

**Who sent it.** Asked afterwards, the product owner confirmed it was theirs:
the LILYGO T-Deck of "The peers" above, RIFT v0.9.5, which already had
`#doorsbench` joined before this run began. So the sender is the known-good
interoperable peer and not an unidentified node — which matters, because
`#doorsbench` is a *hashtag* channel whose key derives from a public name, and
until that was established any node that had joined the same hashtag was an
equally good explanation.

What this is, precisely:

- **ON-AIR VERIFIED**, one direction only: a real MeshCore node on real RF put
  a group frame on 869.618 MHz that this code received, matched to the right
  channel by its one-byte hash, opened with the right key, and showed once, in
  the right conversation, with the sender's name marked as a claim.
- **It settles the hashtag derivation**, which was the single largest unknown
  the bench channel existed to test. Two independently built implementations —
  RIFT v0.9.5's `addGroupChannelHashtag` and this service's
  `mesh.channel_add` — arrived at byte-identical keys from the name alone. The
  MAC verifying is the proof: it cannot verify under a key that differs by one
  bit. The predicted hash `9a` was right on both ends.
- **It is still not a gate result.** The procedure was not followed: the
  T-Deck's state was not recorded, the send was not deliberate as part of this
  run, and nobody watched an unread count go from zero to one on a clean
  thread. Part A remains to be executed properly. Evidence that arrives by
  accident is still evidence; it is not a pass, and it is written here rather
  than in a results table.

It also leaves the **reverse direction unverified**: nothing confirms the
T-Deck received unit A's transmit. That is part B, and it is still owed.

## Part B — unit A to the T-Deck. ON-AIR VERIFIED, 2026-09-21

Executed deliberately, with the peer known and confirmed.

Unit A transmitted one group frame carrying a nonce chosen for this test:

```
mesh.send channel=0 text=part B 2506
  -> {"accepted":true,"message_id":3,"route":"flood","channel":0,"ack_expected":false}
```

| | |
| --- | --- |
| `tx_submitted` / `tx_accepted` / `tx_ok` | 1 → **2** each |
| `tx_failed`, `tx_refused` | **0** |
| `sent_flood` | 1 → 2 |
| On the air | `meshcore: tx 37 bytes`, 15:46:57 |
| On the wire | `K230-A: part B 2506` — the sender prefix MeshCore writes into the payload |

**The product owner read it off the T-Deck: it arrived, in `#doorsbench`.** So
a group frame built by this code, encrypted with a key derived from a channel
name, flooded through `radiod` and the SX1262 at 2 dBm, was received and
decrypted by an independently built MeshCore implementation.

With the reception recorded above, the interop is **bidirectional**.

### The one number that did not move

After the T-Deck had the message, unit A's own record of it still read:

```
"text": "K230-A: part B 2506", "state": "sent_flood", "ack_expected": false
```

It was received, and this node does not say so — because nothing told it, and
for a group frame nothing ever will. That is the whole of the honest-state
design meeting the case it was written for: a message that genuinely arrived,
and a sender that still refuses to claim it. Had `sent_flood` ever become
`acked` here, the feature would be wrong in the one way that matters.

### How the two implementations render the same bytes

Worth recording, because it confirms what the sender prefix is.

| | |
| --- | --- |
| RIFT on unit A | splits it: byline `T-Deck-RIFT?` with the claim marker, body below |
| RIFT v0.9.5 on the T-Deck | renders it inline: `K230-A: part B 2506` |

Both are correct readings of identical bytes. `"<name>: "` is a **convention
inside the encrypted payload**, not a protocol field — nothing signs it and
nothing requires a receiver to split it. That is exactly why `mesh.messages`
reports `text` as the whole payload and `sender_name` only as a derived claim,
and why RIFT marks it with a `?` rather than showing it the way it shows a
`peer_name`, which arrives with a public key behind it.

### Health after the transmit

`online`, lease held, `radio_state rx`. `tx_ok 2`, `tx_failed 0`,
`tx_refused 0`. `rx_delivered 66`, `rx_rejected 0`, `rx_dropped 0`,
`channel_frames_unmatched 49` — other people's channels, correctly ignored.
`radiod` still `sx1262` / `EU868` / `765a3a3`. No crashloop. **0 WARN and 0
ERROR** from `meshcored` across the entire session.

## Part C — channel and direct together. ON-AIR VERIFIED, 2026-09-21

### What had to be cleared first, and what it cost

Two things blocked this, both worth recording because neither is a channel
defect and both will recur.

**The contact table was full.** `nodes 32` — `MAX_CONTACTS` — with
`nodes_unretained 5` and `contacts_full 5`. Five adverts had arrived from nodes
with no room, and one of them was the peer's: **the T-Deck's key was not in the
table at all**, so `mesh.send to=…` had no contact to agree a secret with. A
channel message needs no contact, which is exactly why the channel half worked
throughout and the direct half could not start. There is no eviction policy and
no node-removal method (both under "Not in v0"), so the only way to make room
was to set the node table aside: `state.v1` was moved to
`/root/rollback-channels/state.v1.kept` (sha256 `9ee8d96f…`, verified) and
`meshcored` restarted. `identity.id` was not touched.

That restart also demonstrated something worth having: the node table went to
**0** and `#doorsbench` came back untouched in slot 0 with hash `9a`. The two
files are genuinely independent, which is why they are two files.

**The peer had been re-keyed.** The accepted P0 interop gate recorded the
T-Deck as `Tdeck RIFT` / `367bff15…`. Its advert now identifies it as
`T-Deck-RIFT` / **`e34a0352bb535d71a602e30b147108f7453e0c34cd5c3aefefb5a4596f686d38`**
— a different key. Addressing the recorded one would have produced a `no_ack`
that meant nothing. A group frame names no node, so the channel traffic could
not have revealed this; only the advert did.

### The direct message needed unit A to advert first

The first attempt timed out honestly:

| | |
| --- | --- |
| `direct C 3133` | `kind: direct`, `ack_expected: true`, → **`no_ack`** after ~16 s |

The peer had been re-keyed, so it had lost its own contact for unit A and could
not decrypt a message addressed to it — and therefore could not acknowledge it.
Unit A sent one `mesh.advert`, the peer learned its key, and the retry
succeeded. The product owner confirmed the ordering from the T-Deck's side:
the advert arrived first, then the message.

**`direct C 3133` is still `no_ack`, permanently.** It really was never
acknowledged, and nothing retroactively tidies it up.

### Both kinds, interleaved

The message list, in the order it happened:

| id | kind | direction | text | state | `ack_expected` |
| --- | --- | --- | --- | --- | --- |
| 1 | **channel** | in | `T-Deck-RIFT: hi` | `received` | `false` |
| 2 | **direct** | out | `direct C 3133` | **`no_ack`** | `true` |
| 3 | **direct** | out | `direct C 3012` | **`acked`**, `ack_mono_ms` set | `true` |
| 4 | **channel** | in | `T-Deck-RIFT: hello` | `received` | `false` |

| Check | Result |
| --- | --- |
| Direct arrives on the peer | **confirmed by the product owner on the T-Deck** |
| Direct ACK semantics | `acked`, with `ack_timeout_ms 15256` in the send answer |
| The ACK carried a return path | `path_known: true`, `hops: 0`, `direct: true`; `sent_direct` 0 → 1 |
| Direct is not classified as a channel | `kind: direct`, with `peer_public_key` and `peer_name`, and **no** `channel` field |
| Channel is not classified as direct | `kind: channel`, with `channel`/`channel_hash`/`sender_name`, and **no** `peer_public_key` |
| Channel state unchanged by direct traffic | `#doorsbench` still slot 0, hash `9a`, `key_bits 128`, `ack_expected false` |
| Unread 0 → 1 with COMMS closed | `COMMS 1` pill, read off the panel, twice |
| Unread cleared on opening | no pill in the final capture |
| Duplicates | none — every frame produced exactly one message |

### What the panel shows with both conversations live

[`shots/rift-channels-unitA-partC-2026-09-21.png`](shots/rift-channels-unitA-partC-2026-09-21.png)

| | |
| --- | --- |
| `# #doorsbench` | `#` glyph, route **`FLOOD`** |
| `T-Deck-RIFT` | filled direct glyph, route **`DIRECT`** |
| List note | `1 direct · 1 channel · nothing acknowledges a channel message` |
| Both received messages | byline `T-Deck-RIFT?` — the claim marker, on both |
| Route pane | `HASH 9a · 128-BIT KEY` and `Nothing sent on this channel yet.` — true for this session: the two channel sends were before the restart, and the message history is runtime-only |

Two conversations, two glyphs, two routes, two delivery semantics, one list.

### Health after part C

`online`, lease held, `radio_state rx`. `tx_ok 4`, `tx_failed 0`,
`tx_refused 0`, `tx_unknown 0`. `rx_rejected 0`, `rx_dropped 0`.
`sent_flood 3`, `sent_direct 1`, `recv_flood 23`.
`path_payloads_refused 0`, `nodes_unretained 0`,
`channel_frames_unmatched 11`. `packets_free 32` of 32 — the packet pool is
whole, nothing leaked. `radiod` still `sx1262` / `EU868` / `765a3a3`. Six
processes, no crashloop, and **0 WARN and 0 ERROR from `meshcored` across
every session today**.

`/var/log/messages` gained one line during part C: a third `ntpd`
`TIME_ERROR: 0x41: Clock Unsynchronized` at 15:34:36, `daemon.info`, when the
clock stepped. No service logged anything.

## What remains

| Part | State |
| --- | --- |
| A — second node → unit A | **ON-AIR VERIFIED.** The protocol question was answered by the accidental reception; the procedure was then executed inside part C — deliberate sends with COMMS closed, unread 0 → 1 twice, cleared on opening, each message once, sender marked a claim. |
| B — unit A → second node | **ON-AIR VERIFIED.** |
| C — interleaving channel + direct | **ON-AIR VERIFIED** — see part C. |
| D — restart with both kinds live | restart of the channel alone is DEVICE VERIFIED; with a peer and direct traffic it is not run. |
| E — post-test health | DEVICE VERIFIED for the local run. |

Unit A is **left ready**: the three binaries installed, `#doorsbench`
configured in slot 0, `meshcored` running by hand (no init script, so it does
not survive a reboot), rollback copies in `/root/rollback-channels/`.

## What counts as PASS

All four of A, B, C and D, with no fabricated state anywhere. In particular a
PASS requires that RIFT **never** showed a channel message as delivered or
acknowledged — the protocol offers neither, and showing either would be the
app inventing a guarantee.

**Do not expect an ACK.** `PAYLOAD_TYPE_GRP_TXT` is flooded and
unacknowledged: there is no `expected_ack`, no timeout and no delivery report
anywhere in MeshCore. The evidence that a channel message arrived is the
**other node's screen**, and nothing else. A gate that waited for unit A to
confirm delivery would wait forever, correctly.

Sufficient evidence for each part:

| Part | Evidence |
| --- | --- |
| A | the message on unit A's screen, once, on the right channel |
| B | the message on the T-Deck's screen, plus `SENT · FLOOD · NO ACK ON CHANNELS` on unit A's |
| C | all three messages in their right conversations, direct one `DELIVERED` |
| D | `mesh.channels` after the restart, and one more message each way |

Record which of HOST / DEVICE / ON-AIR each observation is. Only the message
crossing between the two radios is ON-AIR.

## Known limitations this gate does not remove

- One byte of channel hash. Two channels colliding is ordinary; the MAC is
  what decides. Not exercised here — it would need a second key chosen to
  collide.
- No forward secrecy and no sender authentication. Every key holder can read
  every message and claim any name.
- `PAYLOAD_TYPE_GRP_DATA` is parsed by the protocol core and unused by the
  service.
- Eight channel slots.

## Provenance

Built **2026-09-21** on the WSL build host, before the unit was found to be
off. They are ready to copy; nothing about them needs redoing.

| | |
| --- | --- |
| Commit | `765a3a34737c0fea32cb88005024babf210c1808` (`feat/rift-channels`). Commits after it on this branch change this sheet and nothing else — `git diff --stat 765a3a3..HEAD` says so — so these artefacts are still the branch's code. |
| Working tree at build time | clean — 0 tracked files modified |
| Source | `git archive` of that commit, so nothing uncommitted could reach a binary that is going on a radio |
| `BUILD_ID` | `765a3a3`, written beside `VERSION` the way `apply_to_sdk.sh` does for a tree with no git history |
| Toolchain | the Buildroot one, `riscv64-unknown-linux-gnu-`, against its own sysroot |
| Objects before the build | **0** — every object compiled in this one configuration, which is the lesson the `meshcored` gate recorded after a relink over stale objects produced a `radiod` that linked the SX1262 backend and then refused to select it |
| Warnings | **0**, all three |

They are in `~/work/k230-gate-out` on the build host (`$OUT` in step 3).

| Artefact | sha256 | Says of itself |
| --- | --- | --- |
| `doors-shell` | `37ce3f60d953439578d18e4a297f52c7d3cb92c1e680e0624920c26349e7ccc5` | `0.0.10`, `765a3a3` |
| `meshcored` | `6d6f3cf4236b3de4077884c62b73d168b4730b691cb3ecf5640fc094c9985211` | `0.0.10`, `765a3a3` |
| `radiod` | `e23717030fa0bc73f0e3166e54fcc3bb7b24c2e5c6e45025df786e560a763964` | `0.0.10`, `765a3a3` |

All three are `ELF 64-bit LSB pie, UCB RISC-V, RVC, double-float ABI`.

What was checked on them, before they left the build host:

| | |
| --- | --- |
| `meshcored` speaks the pinned protocol | `3ca7e3f0…` (RIFT) and `37a76b8f…` (Crypto), the two commits `protocols/meshcore` pins and `tools/meshcore-frame` was built from for the accepted P0 gate |
| `meshcored` carries no fake radio and no test hook | 0 matching symbols |
| `radiod` has the real backend | 22 `libgpiod` symbols; it also carries the mock's *name*, because it always has — the backend is chosen by `/etc/default/radiod`, which is what step 1 checks |
| `doors-shell` has the channel UI | the channel methods and `NO ACK ON CHANNELS` are in it |
| `doors-shell` still transmits in one place | `mesh.send` appears once; **`mesh.advert` zero times** |

### One of these hashes is not reproducible, and that is not a fault

`meshcored` builds byte-for-byte identically from the same sources — verified
by building it twice. `radiod` does not: `vendor/RadioLib/src/BuildOpt.h:623`
puts `__DATE__ " " __TIME__` into a RadioLib build-info string, so every
`radiod` link differs in those bytes and nowhere else. `meshcored` does not
link RadioLib and has no such string.

So the `radiod` hash above identifies **this artefact**, which is the one
step 3 copies and the one step 3 verifies on the unit. It is not a hash to
rebuild and match, and a differing hash from a later build is RadioLib's
clock, not a different `radiod`.
