# RIFT Channels on unit A: the two-node on-air gate

**Status: NOT RUN.** This is the sheet for a test that has not happened. No
part of it has been on a radio, and nothing below may be read as a result.

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
| **HOST VERIFIED** | The protocol, the service, the IPC and the app, on a build host: 1 900+ checks across the suites, plain and under ASan + UBSan with leak detection. Two whole `meshcored` processes exchanging channel messages over a mock air. The channel key never leaving the service. Persistence across a restart. |
| **HOST VERIFIED** | The target build: `libmeshcore-riscv64.a`, and `meshcored`, `radiod` and `doors-shell` linked for riscv64 from wiped objects. |
| **NOT VERIFIED — this gate** | That a channel frame this code puts on a real SX1262 is received and decrypted by a second, independently built MeshCore node — and the reverse. |

Nothing in the channel path has been on a radio. The accepted P0 interop gate
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

Unit A must be reachable over SSH and on the real radio backend. **It was not
reachable while this sheet was written** (no host answered at
`192.168.10.157`, and a sweep of `192.168.10.0/24` found no board), so it was
never touched: nothing was deployed, nothing was configured, and the unit is
in whatever state it was last left in.

That means step 1 below is a survey, not a formality. In particular
`/etc/default/radiod` decides the backend, and a reflash removes it —
`S60radiod` then defaults to `RADIOD_BACKEND=mock`, which cannot transmit.
The interop gate left unit A configured for `sx1262`; confirm it rather than
assume it.

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
| `/usr/sbin/radiod` | only if step 1 shows the unit's `radiod` predates `radio.send_async` |

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

Filled in when the artefacts are built; see the commit that adds this section.
