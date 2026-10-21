# Explicit MeshCore path P0 — read-only backend checkpoint

Date: 2026-10-18, updated 2026-10-21
Status: **HOST-TESTED, K230 CROSS-BUILT, RUNS ON UNIT B; ZERO-RELAY DIRECT
TO A STOCK MESHCORE COMPANION PASSED ON AIR (2026-10-21); REPEATER
FORWARDING NOT RF-VALIDATED.** Explicit
DIRECT send is implemented (see the follow-up section). The original
read-only checkpoint below is kept as written. Build evidence and the
physical acceptance procedure are at the end of this file.
Design reference: [RIFT Visual Path Builder](../apps/rift/VISUAL_PATH_BUILDER.md).

## Grounded feasibility result

Pinned MeshCore RIFT commit: `3ca7e3f003bd270587b1d92e33ce999542dcb5b4`.

- `BaseChatMesh::sendMessage(const ContactInfo&, ...)` composes one encrypted,
  ACK-tracked text packet. It selects the direct path from the **passed
  contact's** `out_path` and `out_path_len`, or floods if unknown.
- `Mesh::sendDirect()` **copies** the chosen path into the packet; it does not
  mutate the contact or require changing MeshCore's global route table.
- Therefore a per-message, temporary `ContactInfo` copy with a validated
  explicit `out_path` is a promising route to manual TX without mutating a
  persistent learned path. **This is an architectural inference, not
  a passing RF or host test.**
- `Packet::path_len` packs 1..3 hash bytes per relay into bits 6..7 and
  a relay count (0..63) into bits 0..5. `MCD_MAX_PATH` is 64 bytes.
- Each hop is addressed by a **prefix** of the full public key. Validate
  collisions; a full key in the editor alone cannot prevent ambiguous
  on-air forwarding.

## Changes in this PR

1. `mcd_runtime_preview_path` validates a proposed explicit direct route
   against the service's current inventory. The input is one exact
   recipient public key and ordered full repeater public keys.
2. The validator checks recipient identity/type, relay identity/type,
   duplicate waypoints, prefix collisions against other known repeaters,
   configured path-hash width, count and byte bounds, and encodes the
   candidate path as MeshCore's packed length plus bytes.
3. The non-transmitting `mesh.path_preview` IPC method reports the
   candidate's `path_hex`, `relays`, `path_bytes` and `hash_bytes`.
   `valid` means **locally encodable**, not RF connectivity or delivery.
4. `mesh.info.capabilities` explicitly states
   `manual_path_preview: true` and `explicit_direct_send: false`.
5. The `mesh.send` handler now refuses `via` immediately, rather than
   allowing an older route-unaware sender to silently use AUTO instead.
6. Runtime tests are added for zero relay, invalid/missing relay, self,
   recipient, duplicate, overflow, path hash widths and preview not
   transmitting.

## Limitations

- No packet with a user-selected route is sent by this change.
- The test changes have been reviewed as source but **not executed**.
- Nothing proves that a selected repeater can physically hear the previous
  relay, or that the chain can reach the destination.
- A prefix absent from the current node inventory can still exist on the
  mesh, so collision checking is a fail-closed best effort, not an
  end-to-end guarantee.
- `mesh.path_preview` doesn't reserve a route: the sender must revalidate
  at send time, not trust an old preview result.
- The default `mesh.send`, direct/flood and channel behaviour is unchanged
  except for deliberate refusal of the previously unsupported `via` input.

## Next mandatory gate: real explicit send

1. Add a *per-message* transient `ContactInfo` with the previewed path;
   never overwrite `contacts[].out_path` or call `resetPathTo()`.
2. Revalidate full keys and route bounds at send time; exact recipient
   matching only. Preserve automatic send behaviour when `via` is absent.
3. Record the requested route **per outgoing message**, including the path
   that was actually used; ensure a pending request cannot overwrite
   another message's route.
4. Explicitly define RESEND semantics: either persist and retry the same
   chosen path with a new attempt, or reject with a clear error.
   **Never** silently revert an explicit path to AUTO during RESEND.
5. Test real packet's `ROUTE_TYPE_DIRECT`, `path_len`, `path[]`,
   unchanged learned route, received message, ACK and overlapping sends
   in the host's mock-air harness.
6. Run `make ENABLE_MESHCORED=1 meshcored` and applicable
   `meshcored_runtime_test` / harness / lint gates in the pinned Linux
   checkout, then the K230 on-air test with repeaters.

This checkpoint is intentionally safer than enabling an unverified TX
path on production hardware.


## Follow-up implementation in the same draft PR (2026-10-18)

A first per-message explicit-send implementation has now been added to this
branch, **not built or RF-validated**. The earlier status description above
refers to the original read-only checkpoint.

- `mesh.send` accepts `{"to":"<full key>","text":"...","via":["<full repeater key>"]}`
  in addition to unchanged automatic sends without `via`.
- The service revalidates the route on SEND, uses a transient
  `ContactInfo` copy and passes it to MeshCore's existing
  `BaseChatMesh::sendMessage` to construct a DIRECT packet.
- Accepted messages record their explicit packet path and reply with
  `route:"explicit"`. A retry through `mesh.send(resend)` is rejected for
  these messages rather than using a different learned route.
- Runtime source tests cover a selected-repeater send, unchanged learned
  contact path, per-message metadata and refusal of RESEND.

**Unverified:** native compile, tests, on-air route, retransmit/ACK behaviour
and actual RF delivery. Leave PR as Draft until these pass. In particular,
a successfully encoded route does not prove that physical repeater links
exist. Full source/test quality gate required before enabling the RIFT UI.


## Build and host validation (2026-10-19)

Build host: Windows 11 + WSL2 Ubuntu 22.04 (user `kraken`, gcc 11.4.0,
make 4.3). `libcjson-dev`/`libcjson1` 1.7.15-1ubuntu0.1 and `jq` 1.6 are not
installed system-wide on this host and sudo needs a password, so the Ubuntu
packages were fetched with `apt-get download` and extracted into
`~/work/meshcored-p0/hostdeps/root`, reached only through `CPATH`,
`LIBRARY_PATH`, `LD_LIBRARY_PATH` and `PATH`. Build flags were not changed.
The CI runner installs the same libcjson version with apt.

Pinned sources (VERIFIED): MeshCore RIFT `3ca7e3f003bd270587b1d92e33ce999542dcb5b4`,
Crypto `37a76b8f7516568e1c575b6dc9268da1ccaac6b6`, clean checkouts.

### Host sequence (the workflow's three steps, mock radio)

| Commit | `meshcore-frame` | `services/radiod/radiod` | `meshcored-test` |
| --- | --- | --- | --- |
| `0072555` (PR head as received) | pass | pass | pass |
| `14039ac` (adds the RF helper) | pass | pass | pass |

At `0072555`, in a git clone of the commit: `meshcored_runtime_test` 653
checks, 0 failures, plain and ASan/UBSan; util 72, txmap 58, store 177,
repeater 108, rxlog 72, all 0 failures in both builds; `meshcored_lint`
108/0; `meshcored_source_identity_test` 17/0; `meshcored_service_test` and
`meshcored_harness_test` 0 failures. The sanitizer settings and the
ed25519 `shift-base` exemption are unchanged.

A `git archive` export (no `.git`) fails exactly one lint check, "its mode
is 0755 in git", because that check reads the mode with `git ls-files`.
The file is `100755` in git. Host tests are therefore run in a git checkout;
the export is used for the cross-build only.

### K230 cross-build

Toolchain: Xuantie-900 gcc 14.1.1 V3.0.2 (B-20250410),
`/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2`.
Sysroot: the pinned SDK's (`22d02c6`, BSP `bb831ab`) Buildroot sysroot
`output/k230_canmv_t_display_rm69a10_defconfig/host/riscv64-buildroot-linux-gnu/sysroot`
(glibc 2.33, cJSON 1.7.18). The `k230_pocketos_defconfig` output on this host
has no staging tree with cJSON, so the vendor defconfig's sysroot, which the
Doors defconfig is composed from, was used. Flags mirror Buildroot's
`TARGET_CFLAGS` for that config:

```sh
SR=<sysroot above>
X=/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-
F="--sysroot=$SR -mcpu=c908v -mtune=c908 -mrvv-v0p10-compatible -mrvv-auto-vectorize"
B="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -g0 -D_FORTIFY_SOURCE=1"
make -j8 ENABLE_MESHCORED=1 CC=${X}gcc CXX=${X}g++ AR=${X}ar \
     CFLAGS="$F $B -Wall -Wextra" CXXFLAGS="$F $B" LDFLAGS="$F" \
     meshcored tests/hw/pos-ipc-json
```

Run in a fresh `git archive` export of the commit, with `BUILD_ID` written
beside `VERSION`. Result (VERIFIED): both commits build with no compiler
warning. ELF64 RISC-V, RVC, double-float ABI, interpreter
`/lib/ld-linux-riscv64-lp64d.so.1`; meshcored needs `libcjson.so.1`,
`libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, highest symbol
versions `GLIBC_2.33` and `GLIBCXX_3.4`, and embeds its BUILD_ID and both
vendor commits. The helper needs only `libcjson.so.1`, `libm.so.6` and
`libc.so.6`.

| Commit | Artifact | SHA-256 |
| --- | --- | --- |
| `0072555` | meshcored | `4ae2a338270f39ac2cf0523cc957cd29214b8900c5133529ce5c5c223ab5e461` |
| `14039ac` | meshcored (RF candidate) | `c0af8f32535fba3ee730e107231c621cd9ab6db770ae8fd07c10349c7a91f2cd` |
| `14039ac` | pos-ipc-json | `31e75db0c293c305c60fdfdad7d63d4d83afdbe63885abadb9b5f4e8c8651b9c` |

meshcored's sources are identical at both commits; the binaries differ by
the embedded BUILD_ID. Artifacts and logs are kept on the build host under
`~/work/meshcored-p0/<commit>/` (`artifacts/`, `logs/`). At the time of
this build nothing had run on a K230; the `14039ac` candidate was later
installed on unit B (see "On-device run, unit B only").

### Rehearsal of the RF procedure (host, mock air)

`tests/hw/mesh_explicit_path_rf.sh` was run at `14039ac` against two real
meshcored processes on the meshcored harness's stand-in radiod and air,
with `SSH` replaced by a stand-in that reaches each process's socket. Every
phase passed: `check`; `preview` (zero relays and six invalid routes, no TX,
no message); `refuse` (four invalid `via`, a prefix, a channel, and `resend`
with `[]`, `null`, a string and an array; no TX, no message); `send`
without `RF_TX=yes` refused; an explicit zero-relay `send` (delivered, ACK
matched, requested route recorded, learned route unchanged, RESEND refused
without TX); an `auto` control (delivered, ACK matched, route learned); and
an explicit `send` again with a learned route present (unchanged
afterwards). Relay routes cannot be rehearsed without a repeater.

## Physical RF acceptance procedure (prepared; RF steps not run)

Only the backup, install and no-TX checks on unit B have been run (see
"On-device run, unit B only"). No step that transmits has been run. Every
step that changes a unit or transmits needs the owner's go-ahead first.
This procedure assumes two Doors units. With the equipment actually
available (unit B and an M5 companion), follow "Bench plan: unit B sender,
M5 companion recipient" at the end of this file.

### Equipment (DOCUMENTED, to be re-checked at the start)

- Unit A: eth0 `192.168.10.157`, Wi-Fi `192.168.10.171` (2026-09-25,
  docs/hardware/DEPLOY_OVER_WIFI_GATE.md); `.171` did not answer ping on
  2026-10-05 (docs/hardware/RIFT_COMMS_RELIABILITY_GATE.md).
- Unit B: Wi-Fi `192.168.10.187` (docs/hardware/FLEET_MULTIPLAYER_GATE.md).
  meshcored enabled, sx1262, EU868, 2 dBm. Superseded on 2026-10-19: unit B
  is now `Doors-aa90` at `172.16.5.198` (see the on-device run).
- Repeater BREVDUE, key prefix `5997dbcb…`, heard direct by unit B at
  RSSI -31 on 2026-10-05 (docs/hardware/RIFT_REPEATER_CONTROL_GATE.md). It
  was not in unit B's node list on 2026-10-19. Its full key, type and
  current presence come from `check`. Its firmware's
  support for 1/2/3-byte path hashes is not documented.
- Radio profile per docs/services/MESHCORED.md (869.618 MHz, BW 62.5 kHz,
  SF8, CR 4/5). The path hash width in use is read by `check`; this
  procedure does not change it.

### Candidate and helper

`meshcored` and `pos-ipc-json` built from `14039ac` (hashes above), and
`tests/hw/mesh_explicit_path_rf.sh` run from the build host (needs `jq` and
root SSH keys for both units).

### Pre-flight (read-only on the units)

```sh
ssh root@<unit> 'cat /etc/doors-release; sha256sum /usr/sbin/meshcored;
  doors call meshcored mesh.info; doors call meshcored mesh.status; df -h /tmp /root'
```

### Back-up and install (changes the units; needs approval)

For each unit, after copying `meshcored`, `pos-ipc-json` and `SHA256SUMS`
to `/tmp/doors-rf/`:

```sh
cd /tmp/doors-rf && grep -v mesh_explicit SHA256SUMS | sha256sum -c &&
/lib/ld-linux-riscv64-lp64d.so.1 --list ./meshcored && ./meshcored --help > /dev/null &&
mkdir -p /root/rollback-explicit-path &&
cp -p /usr/sbin/meshcored /root/rollback-explicit-path/meshcored &&
/etc/init.d/S65meshcored stop &&
cp -a /var/lib/pocketos/meshcored /root/rollback-explicit-path/state &&
install -m 0755 meshcored /usr/sbin/meshcored &&
/etc/init.d/S65meshcored start
```

`/root/rollback-explicit-path/RESTORE.sh`, written with the backup:

```sh
#!/bin/sh
/etc/init.d/S65meshcored stop &&
install -m 0755 /root/rollback-explicit-path/meshcored /usr/sbin/meshcored &&
/etc/init.d/S65meshcored start
# ./state is restored by hand only if needed, with meshcored stopped:
#   rm -rf /var/lib/pocketos/meshcored && cp -a ./state /var/lib/pocketos/meshcored
```

This PR does not change meshcored's stored state format. pos-ipc-json stays
in `/tmp` (tmpfs) and is gone after a reboot.

### Test sequence

These phases need two Doors units. For unit B with an M5 companion, see
the bench plan at the end of this file.

```sh
export EXPECT_BUILD=14039ac
T=tests/hw/mesh_explicit_path_rf.sh
$T check   <A> <B> <repeater full key ...>     # no TX
$T preview <A> <B> <repeater full key ...>     # no TX
$T refuse  <A> <B> <repeater full key ...>     # no TX
RF_TX=yes $T send <A> <B>                      # zero-relay DIRECT
RF_TX=yes $T send <A> <B> <repeater key>       # one selected repeater
RF_TX=yes $T auto <A> <B>                      # AUTO control
```

If the two units do not know each other yet, `check` says so; an advert
(`doors call meshcored mesh.advert`) on each unit is then a separate,
approved transmission.

Pass criteria per send: `route: explicit` with the previewed `path_hex` and
`path_hops`; B receives the text once, from A; A's message reaches `acked`;
A's learned route to B is the same before and after; RESEND of the explicit
message is refused without a transmission. A `no_ack` is a result to record,
not to retry: RESEND of an explicit message is disabled in this phase.

Requested and observed are reported separately. A message's `route`,
`path_hex` and `path_hops` are only what was requested. The script prints
the frames both units logged. Forwarding through the selected repeater is
shown only when B logs the text packet (same `hash`) arriving direct with
fewer remaining hops than A sent it with, or when A hears its own packet
repeated (`own: true`). Units in range of each other can also hear the
original transmission directly. The ACK may return by B's learned route or
by flood and need not retrace the selected hops.

## On-device run, unit B only (2026-10-19)

Only unit B was available; unit A was offline. This run covers installation
and the checks that cannot transmit. It does not cover delivery, ACK over
the air or forwarding.

Unit B's identity and address differ from the older gate documents. The
owner confirmed which unit this is (VERIFIED, 2026-10-19):

- LAN `172.16.5.198`, serial console on the CH342's first channel (COM3 on
  the build PC).
- Doors 0.3.6, `BUILD_ID=9ad64a1`, glibc 2.33, cJSON 1.7.18.
- Mesh identity `Doors-aa90`, key `aa907fc8…`.
- Path hash 1 byte. 163 stored nodes: 140 repeaters, 13 chat, 10 room.
- BREVDUE (`5997dbcb`) and unit A (`19f7b327`) are not in its node list.

Pre-flight and staging, read-only (VERIFIED):

- Before installation the counter `rx_events` stayed 0 for about 45 minutes.
  The old meshcored's `tx_submitted` rose from 0 to 2 in that time, with
  `sent_direct` 1 and no message. The cause was not identified, and it did
  not come from these tests, which made only read calls at that point.
- The candidate and the helper resolve all their libraries on the unit
  (`ld.so --list`).
- Between `9ad64a1` and `14039ac`, only this PR's changes touch meshcored,
  radiod's API, pocketipc or protocols/meshcore. The stored state format is
  unchanged.
- Side effect: `meshcored --help` writes a `start version=… build=…` line to
  `/var/lib/pocketos/log/meshcored.log` before it prints its usage. It does
  not start a service.

Installation (owner-approved):

- `/usr/sbin/meshcored` `7b1ad94f…` was copied to
  `/root/rollback-explicit-path/`, together with `RESTORE.sh`, a copy of
  `/var/lib/pocketos/meshcored` taken while meshcored was stopped, and
  `SHA256SUMS`.
- The candidate `c0af8f32…` (`14039ac`) was then installed and started with
  `S65meshcored`.
- It came up `online` with the same identity and all 163 nodes. `mesh.info`
  reports build `14039ac` with both capabilities `true`, and `tx_submitted`
  was 0 after start.

No-TX checks with `pos-ipc-json` on the unit: 27 checks, 0 failures. Keys
are taken from B's own node list.

- Previews:
  - `[]` gives zero relays and an empty path.
  - One repeater gives `4a`.
  - Three repeaters in order give `4a03bb` (3 bytes).
- Refusals, each with error 2:
  - In `preview`: a repeater whose 1-byte hash collides with another known
    repeater, a duplicate relay, self, the recipient as relay, a chat node
    as relay, `via` given as `null` or as a string, a key prefix as
    recipient, an unknown recipient, and a repeater as recipient.
  - In `send`: `via` given as `null` or as a string, `via` with a prefix,
    `via` with a channel, and `[]` and one repeater to an unknown recipient.
    These sends all used a non-existent recipient, so no defect could reach
    a third-party node.
  - In `resend`: `resend` combined with `via` given as `[]`, `null`, a
    string, or a repeater.
- Across all checks `tx_submitted` stayed 0, no message was created, and
  the recipient's learned route did not change.
- One check failed on its first run. The fault was in the check script: it
  expected the word "collision", but the service's correct error text is
  "a relay hash matches another known repeater". The script was fixed and
  re-run.

State left on unit B: the candidate stays installed and running. To undo,
run `/root/rollback-explicit-path/RESTORE.sh`. The helper and the check
script are in `/tmp/doors-rf` and disappear at reboot.

Still open: a second node for `send`/`auto`, a repeater both units hear,
and an explanation of B's 0 `rx_events`.

## Acceptance readiness (2026-10-21, repository records only)

- PR head `1597aec`. Between `14039ac` (the installed candidate) and
  `1597aec` only this file changed (`git diff --stat 14039ac 1597aec`), so
  no runtime source changed after the candidate was built. Host CI passed on
  `1597aec` (run 39244376847) and on `f88e966`, `0072555` and earlier heads.
- Forwarding rule at the pinned RIFT `3ca7e3f` (VERIFIED, source,
  `src/Mesh.cpp` `onRecvPacket`): a DIRECT packet with relays left is
  forwarded only by the node whose hash matches the first relay, which
  removes itself from the path; every other node, including the recipient,
  releases it without decrypting it. B therefore accepts a one-relay
  explicit text only after a relay has forwarded it, even when A and B are
  in range of each other. A zero-relay DIRECT packet is never forwarded, so
  its delivery shows the direct A to B link.
- A node with the same hash prefix as the selected repeater but absent from
  the sender's node list would also forward. A powered-off repeater run
  supports attribution in a controlled test; it cannot rule out every such
  node.
- meshcored has no automatic retry or flood fallback: an unanswered message
  becomes `no_ack` at its deadline, and RESEND of an explicit message is
  refused (`mesh_runtime.cpp`, `resend`).

## Bench plan: unit B sender, M5 companion recipient (2026-10-21)

Equipment the owner has: **unit B** as the instrumented sender (the
`14039ac` candidate per the 2026-10-19 record), **M5** running stock
MeshCore companion firmware as the recipient, observed in its phone app.
A repeater is not confirmed. No second Doors unit is needed, and nothing
is installed on the M5.

### What the existing tooling can do here

`tests/hw/mesh_explicit_path_rf.sh` reads both identities with
`mesh.identity` over SSH before any phase, so no phase runs with an M5 as
host B. That is a harness assumption, not a protocol requirement. The table
separates them.

| Script assumption | Needed by the protocol? | Here |
| --- | --- | --- |
| SSH and `pos-ipc-json` on the recipient | no | M5 app, read by the owner |
| Recipient key from its `mesh.identity` | the full key is needed | read from the M5 app |
| "B knows A" via recipient's `mesh.node` | yes: the recipient needs the sender as a contact to decrypt (`searchPeersByHash`, `BaseChatMesh.cpp`) | Doors-aa90 in the M5's contacts, checked in the app |
| Receipt from recipient's `mesh.messages` | receipt must be observed | the token in the M5 app |
| Recipient's receive log | no; forwarding evidence only | not available; the sender's log is |
| Sender calls (`preview`, `send`, `messages`, `node`, `status`, receive log) | yes | the same `pos-ipc-json` calls on unit B by hand |

Protocol needs that remain: the same radio profile on both; unit B knows
the M5 by its full key as a chat node (type 1); the M5 knows Doors-aa90;
and the M5 ACKs a direct text it decrypts. At pinned RIFT the recipient
ACKs a DIRECT text with `sendAckTo`, by its learned path or by flood
(VERIFIED, source). That stock companion firmware does the same is
ASSUMED until observed. Unit B's path hash is 1 byte (VERIFIED
2026-10-19), so a zero-relay path is the plain length byte 0.

### Direct-delivery test (zero relays): run 2026-10-21, see result below

On the build host: `H=/tmp/doors-rf/pos-ipc-json`, `KM=<M5 full key>`,
`TOKEN=rfp0-direct-<date>-<random>`, `ssh root@172.16.5.198 "$H ..."`.
If unit B has rebooted, `/tmp/doors-rf` is gone and copying the helper back
(`31e75db0…`) is a device change that needs approval.

Read-only, no TX:

1. `cat /etc/doors-release; sha256sum /usr/sbin/meshcored` is `c0af8f32…`;
   `$H mesh.info` reports build `14039ac` and both explicit-path
   capabilities `true`.
2. `$H mesh.status`: `online`; `radio.profile` frequency, bandwidth, SF and
   CR equal the M5's settings in the app. Stop on any difference; this test
   does not change either profile.
3. `$H mesh.node '{"node":"<KM>"}'`: `public_key` equals `KM` exactly and
   `type` is 1. Record `path_known`, `hops`, `path_hex` as the **before**
   route. If unit B does not know the M5, the owner sends an advert from the
   M5 app; this also shows unit B's receive path works (`rx_events` rises),
   which has been open since 2026-10-19. Stop if it does not rise.
4. In the M5 app, Doors-aa90 is a contact whose key starts `aa907fc8`. If it
   is not, `$H mesh.advert` on unit B is a transmission and needs approval.
5. `$H mesh.path_preview '{"to":"<KM>","via":[]}'`: `valid`, 0 relays,
   `transmitted` false.

Transmit, only with the owner's explicit approval for this one send:

6. Start `$H -l 60 mesh.subscribe '{"rx_log":true}'` on unit B, output
   saved on the build host. Read `$H mesh.status` counters.
7. `$H mesh.send '{"to":"<KM>","text":"<TOKEN>","via":[]}'`: `accepted`,
   `route` `explicit`. Note `message_id`. Send nothing else.
8. Poll `$H mesh.messages` until that id is `acked`, `no_ack` or `failed`.
   Do not resend.
9. Read `$H mesh.status` counters and `$H mesh.node '{"node":"<KM>"}'`
   (**after** route). `$H mesh.send '{"resend":<id>}'` is refused with
   `tx_submitted` unchanged.

Record three things separately:

- **Recipient observation:** the M5 app shows `TOKEN` once, from
  Doors-aa90.
- **Sender transmission evidence:** the message has `route` `explicit` and
  no path; `sent_direct` and `tx_submitted` rose by 1 and `sent_flood` did
  not (any other change explained, e.g. by an advert); no `own: true` flood
  copy of the text in unit B's receive log.
- **ACK state:** the message is `acked`; unit B's receive log shows the ACK.

Pass: all three, and `path_known`, `hops`, `path_hex` the same before and
after. A `no_ack` with the token shown on the M5 is an ACK-path finding,
not a pass. A `no_ack` without it is recorded and not retried.

What a pass proves: the candidate sends an explicit zero-relay DIRECT text
that a stock MeshCore companion at the M5's firmware version decrypts as
from Doors-aa90; the returned ACK is matched to that explicit message; the
explicit send left unit B's learned route to the M5 unchanged; and unit B
and the M5 have a direct radio link at that placement (a zero-relay DIRECT
packet is not forwarded at pinned RIFT; for other repeater firmware this is
ASSUMED).

What it does not prove: forwarding through any selected repeater; routes
with relays or 2/3-byte hashes on the air; behaviour when a selected route
cannot be used; interoperability with other companion firmware versions;
or which path the ACK took.

### Direct-delivery result (2026-10-21, owner-approved, one transmission)

**PASS** for zero-relay explicit DIRECT from unit B to a stock MeshCore
companion. All observations are VERIFIED on this run.

Read-only state before the send (07:43 UTC):

- Unit B rebooted on its own before this session: meshcored stopped
  2026-10-20 13:37 UTC and started again about 06:33 UTC. The installed
  binary is still `c0af8f32…`, and `mesh.info` reports build `14039ac`
  with both capabilities `true`.
- Radio profile: 869.618 MHz, 62.5 kHz, SF8, CR 4/5, sync word 18,
  2 dBm. Path hash 1 byte.
- Received from the M5's advert, the only node heard in this run:
  `Mstr_m5`, type 1, key
  `c00dadc0d5e87ed5bde67a8e150e54e92016a700a2223be8d41beff6fc0a0e7b`
  (the owner confirmed it in the M5 app), 0 advert hops, RSSI -25 dBm,
  SNR 11.5 dB, heard 07:33:15 UTC. The advert does not carry the M5's
  firmware version or radio settings. `rx_events` was 3, so unit B's
  receive path works in this run.
- Unit B's route to the M5: `path_known: false`.
- The reboot had removed the helper. With approval, `pos-ipc-json`
  (`31e75db0…`, hash checked on the unit) was copied back to
  `/tmp/doors-rf`. The `via: []` preview was valid, with 0 relays, an empty
  path and `transmitted: false`. Counters and route did not change.

The send (07:50:22 UTC): `mesh.send` to the M5 with `via: []` and the text
`rfp0-direct-20261021-0f19a3`. Nothing else was sent: no resend and no
advert.

- **Recipient observation:** the owner saw the text exactly once in the M5
  app, from Doors-aa90.
- **Sender transmission evidence:**
  - The reply was `accepted`, `route: explicit`, and the stored message
    has `path_hops: 0`, `path_hex: ""`, `attempts: 1`.
  - `tx_submitted` went from 3 to 4, `sent_direct` from 1 to 2 and
    `sent_flood` stayed at 2.
  - The receive log showed one 38-byte transmission and no `own: true`
    copy.
- **ACK state:**
  - The message became `acked` 1.2 s after the send.
  - The ACK arrived as a 0-hop flood at RSSI -27 dBm and SNR 12.25 dB.
    That fits the M5 having no route back to unit B.
  - The ACK frame used a 2-byte path hash size and was still matched.
- **Learned route:** `path_known: false` before and after.

What this run shows and what it does not is set out under "What a pass
proves" above. It is one message at one placement.

**Unexplained transmissions on unit B (separate from the result):**

- At 07:44 all TX counters were 0.
- By 07:50, before the test send, `tx_submitted` was 3: `sent_flood` 2,
  `sent_direct` 1, and no message stored.
- This session made only read calls and the preview in that time. The
  cause is not identified. It is similar to the 2026-10-19 observation on
  the old 0.3.6 daemon.

### Repeater test (pending: no suitable repeater confirmed)

Not run and not accepted. It needs a repeater R that the owner controls and
can power off, whose full key is read from R itself, which unit B knows as
type 2, and whose `[R]` preview is valid (no known hash match). Steps 1 to
9 as above with `via` `[R]`, after a fresh learned-route baseline.
Forwarding evidence on the sender side is unit B hearing its own text
repeated (`own: true`, same `hash`, 0 hops left); the message's requested
route fields are not evidence. That the M5 decrypts only after forwarding
is VERIFIED for pinned RIFT and ASSUMED for stock firmware.

Negative control, same send with R powered off. Keep the three records
separate; none is sufficient alone:

- **Recipient:** the M5 app does not show the token.
- **Sender:** `route` `explicit`; `sent_direct` +1, `sent_flood` +0; no
  `own: true` copy of the text in unit B's receive log.
- **ACK state:** `no_ack`. On its own this does not show that nothing was
  delivered or that no AUTO fallback happened: an ACK can be lost, and a
  delivered message can still go unacknowledged.

Together these support the result of this controlled test, namely that
step's delivery came through R and that an unusable selected route was not
replaced by AUTO. They do not rule out, in general, an unknown repeater
that shares R's hash prefix and was quiet or out of range during the run.

### Restore

Nothing in the direct test changes unit B beyond the message it stores.
The candidate stays installed until the owner decides;
`/root/rollback-explicit-path/RESTORE.sh` restores the 0.3.6 daemon.
