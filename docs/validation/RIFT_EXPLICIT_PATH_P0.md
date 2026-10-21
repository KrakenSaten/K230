# Explicit MeshCore path P0 — read-only backend checkpoint

Date: 2026-10-18, updated 2026-10-21
Status: **HOST-TESTED, K230 CROSS-BUILT, RUNS ON UNIT B (no-TX checks
pass); NOT RF-VALIDATED.** Explicit
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
The order to follow at the bench is the "Bench checklist" at the end of
this file.

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

The phases are listed here; the bench order, which runs `auto` first and
adds a negative control, is in the "Bench checklist" below.

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
  A's node list would also forward. Step 7 below separates the two.
- meshcored has no automatic retry or flood fallback: an unanswered message
  becomes `no_ack` at its deadline, and RESEND of an explicit message is
  refused (`mesh_runtime.cpp`, `resend`).

## Bench checklist

Roles and placement: **A** sender (a second Doors unit), **B** recipient
(unit B), **R** one repeater the owner can power off. All three may stand
on one bench in range of each other. Nothing else needs to move.

Identify the nodes:

- **B**: `Doors-aa90`, `172.16.5.198`, key `aa907fc8…` (VERIFIED
  2026-10-19). Confirm `sha256sum /usr/sbin/meshcored` is `c0af8f32…` and
  `/tmp/doors-rf/pos-ipc-json` is present (it is lost at reboot; recopy it,
  `31e75db0…`).
- **A**: the owner points out the unit; key prefix `19f7b327` is
  DOCUMENTED only. `check` prints both full keys; use those, not addresses
  from older gate documents.
- **R**: read R's full public key from R itself (its own console or admin
  interface), not from a node list. A node list entry does not show that R
  is powered on or in range now.

Set `T=tests/hw/mesh_explicit_path_rf.sh`, `EXPECT_BUILD=14039ac`, and
`R=<R's full key>`. Each step marked TX needs the owner's go-ahead for that
step and `RF_TX=yes`.

1. Pre-flight on A and B (read-only commands above). Back up and install the
   candidate on A as on B, with its own `RESTORE.sh` (changes A; approval).
2. `$T check A B $R`. All `ok`. R's entry on A is type 2; note its
   `advert_hops` and `last_rssi_dbm`. If A and B do not know each other, or
   A does not know R, one advert per unit (`doors call meshcored
   mesh.advert`, TX) and an advert from R triggered by the owner, then
   `check` again. Stop if B's `rx_events` (`mesh.status`) does not rise
   when A adverts: B's receive path, open since 2026-10-19, must work first.
3. `$T preview A B $R` and `$T refuse A B $R`. All `ok`. If the preview of
   `[R]` is refused for a hash match, R cannot be selected at this hash
   width; stop and choose another R.
4. TX: `RF_TX=yes $T auto A B`. Control: `acked`, B receives once. Note A's
   learned route to B.
5. TX: `RF_TX=yes $T send A B`. Zero-relay DIRECT.
6. TX: `RF_TX=yes $T send A B $R`. One selected repeater.
7. Owner powers R off. Read `doors call meshcored mesh.status` on A, then
   TX: `RF_TX=yes $T send A B $R`, then read `mesh.status` on A again.
   Negative control.
8. Owner powers R on. Decide whether the candidate stays; otherwise run
   `/root/rollback-explicit-path/RESTORE.sh` on each unit.

Pass:

- Steps 2 and 3: 0 failures.
- Steps 5 and 6: 0 failures (accepted as `explicit`, requested `path_hex`
  and hops recorded, `acked`, B receives the text exactly once from A,
  learned route unchanged from step 4, RESEND refused without TX).
- Step 6 forwarding evidence: B's observed text packet has `path_hops` 0
  with the same `hash` as the send, and/or A logs its own packet repeated
  (`own: true`). The message's `route`, `path_hex` and `path_hops` alone are
  not evidence.
- Step 7: the script's ACK and receipt checks fail as expected; the message
  is `explicit` and `no_ack`; B logs no text packet with that `hash` and
  holds no copy of the text; A's `tx_submitted` rose by one for the send
  (any other rise is matched to an advert in the logs); the learned route
  is unchanged; RESEND is refused without TX. This shows that an unusable
  selected route is not replaced by AUTO, and that step 6's forwarding came
  from R.

Fail: any other outcome in steps 2 to 6, delivery or an extra
transmission in step 7, or a learned route that changes after an explicit
send. Record a `no_ack` in steps 5 or 6 as a result; do not retry it.

Keep each run's `$OUT` directory (`ipc.log`, `A-rx.jsonl`, `B-rx.jsonl`)
with the record. The 2026-10-19 observations on B before the install (no
`rx_events`, two unexplained `tx_submitted`) belong to the old 0.3.6
daemon and are not results of this candidate.
