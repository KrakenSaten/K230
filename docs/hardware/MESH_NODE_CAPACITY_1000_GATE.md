# 1000-node capacity on unit B: the gate

**Unit B carries meshcored `c7a9d36`** (sha256 `cf1722eb…`, from the image
build of that commit). Everything else is as found: doors-shell and sysd
`393cdb1`, the rest of userspace `16667c2`. Rotation automatic (270,
landscape). Real node table, 260 nodes and growing. Rollback at
`/root/rollback-nc1000/RESTORE.sh`.

PR #43, `feat/mesh-node-capacity-1000` at `c7a9d36`. Only `meshcored` is
swapped. The PR's RIFT changes are comments only, so the shell already on the
unit is the right one to test it with.

**Verdict: PASS**, run by Claude over SSH from the build host,
2026-10-03 15:00-15:38 UTC.

## The build the unit must carry

| On the unit | The gate needs | Found (14:50 UTC) | After the deploy |
| --- | --- | --- | --- |
| `/usr/sbin/meshcored` | `c7a9d36` | `16667c2`, md5 `bb3834fd` | `c7a9d36`, sha256 `cf1722eb…` = the build's; `mesh.info` build `c7a9d36` |
| doors-shell, sysd | unchanged | `393cdb1` | `393cdb1` |
| radiod, netd, release file | unchanged | `16667c2` | `16667c2` |
| `/etc/default/meshcored` | untouched | `MESHCORED_ENABLE=1`, `MESHCORED_NAME=K230-B`, `MESHCORED_TX_POWER_DBM=2` | unchanged; argv `--tx-power-dbm 2 --name K230-B` |
| mesh name | unchanged | `gatetest99` (stored, an earlier owner rename) | `gatetest99` |
| Rotation mode | as found | automatic, 270 | automatic, 270 |

## What this gate is for

| | |
| --- | --- |
| **HOST VERIFIED** (`c7a9d36`, clean clones) | 999 nodes held, the 1000th kept, the 1001st turned away; 1000 persisted and reloaded; `state.v1` refuses 1001; `mesh.nodes` of 1000 nodes at its longest is 431 KB, inside 1 MiB, and reaches a reader draining every 100 ms. See PR #43. |
| **THIS GATE** | The same meshcored on unit B: its full table of 256 reloads and grows past 256 on the real mesh, survives a reboot, and a table of 1000 (the unit's real nodes plus synthetic ones) loads on the device, answers `mesh.nodes`, and reaches the real shell's RIFT. |
| **NOT ASKED OF THIS GATE** | 1000 real nodes. The mesh around the bench does not have them; the top 741 of the 1000 are synthetic. |

## Board facts this gate settles

| Fact | Before | Now |
| --- | --- | --- |
| meshcored's stack limit | ASSUMED 8 MiB | **VERIFIED** on unit B: `Max stack size 8388608` in `/proc/<meshcored>/limits` |
| Unix socket send buffer | ASSUMED = host | **VERIFIED** on unit B: `net.core.wmem_default` and `wmem_max` 212,992 |
| `python3` on the image | - | 3.13.3, used for the on-unit probe |

## Steps

The kit is in `out/nc1000-gate/` on the build host (not in the repository):
`snap.sh` (runs on the unit, writes `/root/gate-1000/<label>/`),
`b2-deploy.sh`, `b3-check.sh`, `b4-grow.sh`, `b6-reboot.sh`,
`b5-full.sh fill|probe|restore`, `fill1000.py`, `nodes_probe.py`,
`b7-health.sh`, and `c*.sh` for the panel captures in `caps/`.

0. **Build.** `imgbuild.sh c7a9d36` (apply + `build_image.sh` from a clean
   clone). The meshcored is taken from the target tree.
1. **Baseline**, read-only.
2. **Deploy.** `snap pre`; then stop `S65meshcored` (the old build writes its
   table on the way out). Make the rollback from the binary and the three
   state files as they are then. Install, start, `snap boot`.
3. **Reload.** Compare `pre` and `boot`: identity, channels and settings must
   be byte-identical; every key must be back; services clean.
4. **Growth.** Poll every 2 minutes, for up to 40 minutes, until `nodes` is
   above 256. `state.v1` must be `44 + 148 × nodes`.
5. **Reboot.** `meshcored` must be started by `S65meshcored` at boot, and
   every key must come back.
6. **Full table.** Stop meshcored and keep the real `state.v1`. Write the
   real nodes plus synthetic ones up to 1000: keys `ee ee …`, names of
   31 × `0xff` (sanitised to U+FFFD each), 64-byte paths. Start meshcored.
   Then the probe, RIFT on NODES for 3 minutes, and restore the real
   `state.v1`.
7. **Health.** 10 minutes with RIFT on NODES.

## Results

| Step | Result | Evidence |
| --- | --- | --- |
| 0 build | **PASS** | `IMAGE GATE: PASS`, `BUILD_ID=c7a9d36`; apply rc 0, build_image rc 0 (6 min). The only warnings are the existing OpenCV header ones. meshcored sha256 `cf1722eb…`, md5 `2d3ed1fe`, bss 298,008 B |
| 1 baseline | **PASS** | 14:50 on `16667c2`: **256 nodes, the table full**, `contacts_full` 3 and `nodes_unretained` 3 since 13:16. `state.v1` 37,932 B; meshcored RSS 3,712 kB; root fs 3 % used |
| 2 deploy | **PASS** | 15:00:11. Rollback `/root/rollback-nc1000/` holds meshcored `50922b10…`, `state.v1` `a0c33fd0…`, `channels.v1` `3306297a…` and `settings.v1` `0ae4ec29…`, with `SHA256SUMS` and `RESTORE.sh`. Online at 15:00:18 |
| 3 reload | **PASS** | identity and channels JSON identical; `identity.id` `e68e7cfd…`, `channels.v1` and `settings.v1` sha256 unchanged; **256 of 256 keys back**; `contacts_full` 0, `nodes_unretained` 0; all five services `running=1 crashloop=0 restarts=0`; 0 crash files. meshcored RSS 3,712 → 3,968 kB at 256 nodes |
| 4 growth | **PASS** | 256 → 257 (15:04) → 258 (15:08) → **259 (15:14)**. `contacts_full` and `nodes_unretained` stayed **0**; the old build had turned 3 away in the two hours before. `state.v1` 38,376 B = 44 + 148 × 259 exactly |
| 5 reboot | **PASS** | reboot 15:15:15; meshcored online 15:15:40 (lease, configure, online within 42 ms), started by `S65meshcored` under `pos-supervise`, same argv. **259 of 259 keys back**; identity and channels identical; services clean; 0 crash files |
| 6 full table | **PASS** | real 259 + synthetic 741 = **1000**, `state.v1` 148,044 B. Loaded and online within about 2 s; "not loaded", "refused" and "corrupt" in the log: 0. See below |
| 7 health | **PASS** | 15:23-15:33 with RIFT on NODES. **0 WARN/ERROR** in shell, radiod and meshcored since the reboot, 0 frame/size/timeout lines, 0 restarts, 0 crash files. CPU 0 % for shell, meshcored and radiod at the sample. Table 259 → 260, `state.v1` 38,524 B = 44 + 148 × 260. RSS: see Findings |

### Step 6 on the device

| Measured on unit B | 1000 nodes (259 real + 741 synthetic at their longest) |
| --- | --- |
| `mesh.nodes` reply | **368,391 B**, count 1000. Built and sent in **90 ms** to a reader at full speed. 1 MiB limit, margin 680,185 B |
| reader draining every 50 / 100 / 150 ms | **all OK**: whole reply in 138 / 233 / 332 ms; no reader dropped |
| meshcored memory | RSS 5,608 kB after the probe and 5,920 kB after 3 minutes; `VmHWM` 6,268 kB. At 256 it was 3,968 kB |
| real shell, RIFT on NODES | **`1000 KNOWN`** (`caps/c3-nodes-1000-25s.png`). After 3 minutes, `HEARD < 12 H · 5` newest first (three repeaters, then `Mstr_m5` and `Mstr_k230` direct) over `NEVER HEARD · 995` (`caps/c4-nodes-1000-3min.png`). The session stayed `meshcored connected` throughout: no reconnect line in nine 20 s snapshot cycles. Shell RSS 18,436 kB, CPU 0-7 % |
| restore | real `state.v1` sha256 checked and put back; 259 of 259 keys, **0 synthetic left** |

## Findings

1. **The board matches the host where the risk was.** A 368 KB reply to a
   reader draining every 150 ms - slower than RIFT's 100 ms - arrived whole
   on the device, as it did on the host. pocketipc's 200 ms budget was not
   reached at the sizes measured. A shell stalled for longer than 200 ms
   during a reply above about 215 KB would still lose the connection and
   reconnect; that case was not provoked.
2. **meshcored RSS rose 256 kB in the 10-minute window** (4,224 →
   4,480 kB, `VmHWM` equal to `VmRSS` both times) while the table grew by
   one node and RIFT asked for `mesh.nodes` every 20 s. The follow-up
   samples are under "RSS follow-up". A rise that stops is the allocator
   settling at the largest reply's high-water mark; one that keeps rising
   would be a leak. The host ASan suites report no leak.
3. **Last heard is runtime-only**, so straight after any restart every node
   is NEVER HEARD until it is heard again (known: RIFT follow-up F4). Not
   this branch's.

## RSS follow-up

| UTC | meshcored VmRSS | VmHWM | nodes |
| --- | --- | --- | --- |
| 15:23 | 4,224 kB | 4,224 kB | 259 |
| 15:33 | 4,480 kB | 4,480 kB | 260 |
| 15:38 | 4,480 kB | 4,480 kB | 260 |

The rise stopped. Finding 2 reads as the allocator settling, not a leak. It
is still only 15 minutes; a soak was not run.

## Not covered on hardware

- **1000 real nodes**, and the time it takes a real mesh to reach them.
  741 of the 1000 were synthetic, written into `state.v1`. They are
  well-formed records that load like any other, but nothing on the air
  touched them.
- **A shell stalled for more than 200 ms during a large reply** (Finding 1).
- **Flash writes over time.** `state.v1` is up to 148 KB, rewritten at most
  every 10 s while nodes change. There was no soak.
- **One-byte hash collisions at 1000.** Inbound direct messages from a node
  that is the ninth to share a hash cannot be matched. This was not provoked;
  no direct messages were sent in this gate.
- **Fleet at 1000.** Fleet keeps 64 nodes and was not opened.

## Rollback (unit)

```sh
sh /root/rollback-nc1000/RESTORE.sh
```

This checks the saved copies, stops meshcored, and installs the `16667c2`
meshcored. It also puts back the pre-gate `state.v1` (256 nodes): the old
build refuses a file of more than 256 nodes and would move it aside. Then it
starts meshcored. Nodes learned during the gate are lost, by design.

## As left

15:38 UTC: meshcored `c7a9d36` on unit B, online, lease held, 260 real nodes
and nothing turned away, identity, channels and settings as they were. RIFT is
open on NODES, rotation automatic. The rollback is at `/root/rollback-nc1000/`
and the snapshots are at `/root/gate-1000/`. The SDK tree
`~/work/t-display-k230` carries the `c7a9d36` apply: re-apply master before
building anything else from it.
