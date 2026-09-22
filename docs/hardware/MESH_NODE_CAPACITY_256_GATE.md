# 256-node interim capacity on unit A: the gate

**Status: PASS, 2026-09-22 17:38–18:16 UTC.** Steps 0–10 all passed on unit
A, which is **left on `b41be37`** with 37 nodes, RIFT open on NODES, rotation
automatic (landscape, keyboard present). Two things the run could not show
are recorded under "Not covered on hardware": a table above 64, and anything
approaching 256. One pass criterion was wrong and is corrected below
(meshcored's RSS; see Findings).

## The build the unit must carry — check this before anything else

Every observation below is about **`b41be37`** only if the unit is running
it. After step 3, `doors shell info`, `mesh.info` and `/etc/doors-release`
must all say `b41be37`. Fill in the right-hand column as you go.

| On the unit | The gate needs | Last recorded (bench record 2026-09-22, not in the repo; reboot PASS) | Filled in |
| --- | --- | --- | --- |
| whole userspace (`deploy.sh` set: services, shell, CLI, init scripts, release file) | **`b41be37`**, one build | `5bb51aa`, deployed with `deploy.sh`, meshcored started by `S65meshcored` | step 1: `5bb51aa` · step 3: **`b41be37`** in the shell, `mesh.info` and `/etc/doors-release`; meshcored sha256 `5d6833b5…` = the build's |
| `/etc/default/meshcored` | untouched: `MESHCORED_ENABLE=1`, `MESHCORED_NAME=Mstr_k230`, `MESHCORED_TX_POWER_DBM=2` | as stated | unchanged; the service runs `--tx-power-dbm 2 --name Mstr_k230` |
| Rotation mode | as found; record it | not recorded | `automatic`, landscape (keyboard present), unchanged |

**The build under test is `feat/mesh-node-capacity-256` at `b41be37`**, on
master `2d0914b`. VERSION stays `0.0.10`. Not merged. Commits after
`b41be37` on this branch change this sheet only, so **build from `b41be37`
itself**. A build of the branch tip would stamp a different build id.

## What this gate is for

| | |
| --- | --- |
| **HOST VERIFIED** (`b41be37`, clean clones) | 255 nodes held with nothing turned away, the 256th kept, the 257th turned away and counted; a full table of 256 persisted, reloaded whole and still full; `state.v1` refuses 257; `mesh.nodes` newest heard first, then by the stored last-updated time after a restart; RIFT keeps exactly the newest 64 of a 256-node snapshot. Five mutants of the fix, each killed by a named check. `make test`, `make meshcored-test` (store 161, runtime 363, plain and ASan/UBSan), `meshcore-core-test`, `rift_shell_test.sh` (app 338), the RIFT host suites under ASan/UBSan, riscv64 `make all` + meshcored `-Werror` + DRM shell, 0 first-party warnings. |
| **THIS GATE — VERIFIED on unit A 2026-09-22** | The same build on unit A's real radio and real mesh: it starts, keeps its identity, channels and 32 stored contacts, grows past 32 from real adverts (32 → 37), lists newest first, and RIFT on the panel follows. See Results. |
| **NOT ASKED OF THIS GATE** | Filling 256, or even 64, by hand. A real table above 32 is enough; the rest is the host evidence above. |

## Read before starting — the traps

1. **Never start `meshcored` by hand, and never with `--name`.** Only
   `/etc/init.d/S65meshcored` starts it. It reads `/etc/default/meshcored`,
   which pins the name `Mstr_k230`.
2. **Never touch `/var/lib/pocketos/meshcored/identity.id`.** Only its sha256
   is read here.
3. **Rollback must restore `state.v1`.** Once this build has kept more than
   32 nodes, an older meshcored refuses the file ("longer than this format
   allows"). It then moves it aside as `state.v1.corrupt.N` and starts with
   no contacts. The rollback below puts the pre-gate file back.
4. **Before the reboot, the unit's own log line counts are the offsets**
   (step 2). The logs hold every earlier boot, and boot-time lines are dated
   1970 until NTP syncs.
5. Not this branch's, if seen: **NEVER HEARD** for every node right after a
   restart (RIFT follow-up F4; `last_heard` is runtime-only), meshcored going
   `online → degraded → online` around its own transmits, the shell's
   `radio.status poll failed` WARN, and the `RX` chip clipping in landscape.

## Step 0 — build (build host, WSL)

```sh
C=~/work/nodecap256-gate
git clone -q --no-checkout /mnt/c/K230 $C && cd $C && git checkout -q b41be37
mkdir vendor && cp -a ~/work/mcd-boot-5bb51aa/vendor/{Crypto,RIFT,RadioLib,ggwave} vendor/
find vendor/RadioLib \( -name '*.o' -o -name '*.d' \) | wc -l      # must be 0
bash platforms/k230/scripts/apply_to_sdk.sh ~/work/t-display-k230
POCKETOS_OUT_DIR=$C/out/k230 bash platforms/k230/scripts/build_image.sh ~/work/t-display-k230 \
    2>&1 | tee $C.build.log | tail -5
T=~/work/t-display-k230/k230_linux_sdk/output/k230_pocketos_defconfig/target
sha256sum $T/usr/sbin/meshcored $T/usr/bin/doors-shell | tee $C.sha256
```

Pass: the log ends `IMAGE GATE: PASS` and `BUILD_ID=b41be37`. Record both
sha256 values. The SDK tree now carries `b41be37`: re-apply master before
building anything else from it.

## Step 1 — baseline on the old build, and the rollback copy (unit)

In a root shell on the unit (`ssh root@$UNIT`). First define `snap` in that
shell; every later step calls it.

```sh
D=/root/gate-256; mkdir -p $D; chmod 700 $D
snap() {  # snap <label>: what this gate compares, in $D/<label>/
  o=$D/$1; mkdir -p $o; M=/var/lib/pocketos/meshcored; L=/var/lib/pocketos/log
  for m in info identity channels status nodes; do doors call meshcored mesh.$m > $o/$m.json 2>&1; done
  tr -d ' \t\n' < $o/nodes.json | grep -oE '"public_key":"[0-9a-f]+"' | sort > $o/keys.txt
  sha256sum $M/identity.id $M/channels.v1 /usr/sbin/meshcored /usr/bin/doors-shell > $o/sha.txt
  wc -c < $M/state.v1 > $o/state_bytes.txt
  for s in doors-shell sysd netd radiod meshcored; do
    echo "$s $(grep -E '^(running|crashloop|restarts)=' /run/pocketos/$s.state 2>/dev/null | tr '\n' ' ')"
  done > $o/services.txt
  ls /run/pocketos/*crashloop* $L/crash-* 2>/dev/null > $o/crash.txt
  for f in shell radiod meshcored; do echo "$f $(wc -l < $L/$f.log)"; done > $o/loglines.txt
  for p in meshcored doors-shell; do echo "$p $(grep VmRSS /proc/$(pidof $p)/status)"; done > $o/rss.txt
  echo "$(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"') release=$(cat /etc/doors-release | tr '\n' ' ')" > $o/builds.txt
  tr -d ' \t\n' < $o/status.json | grep -oE '"(state|radio_state)":"[^"]*"|"lease_held":[a-z]*|"(nodes|channels|nodes_unretained|contacts_full)":[0-9]+' | tr '\n' ' ' > $o/status.txt
}
snap pre; cat $D/pre/builds.txt $D/pre/status.txt; echo; wc -l < $D/pre/keys.txt
sleep 600; snap pre10; cat $D/pre10/status.txt     # 10 minutes on the old build
```

Record: the build (expect `5bb51aa`), rotation mode, `nodes` (expect 32),
and how much `nodes_unretained` grew in the 10 minutes. Growth means the mesh
has more nodes than the old table could keep. It is the reason this gate can
show growth past 32 at all.

Then the rollback copy, from the files `deploy.sh` replaces plus the two
state files. **identity.id is not copied.**

```sh
R=/root/rollback-pre256; mkdir -p $R; chmod 700 $R; cd /
tar -cf $R/userspace-before.tar usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer \
  usr/bin/pos-wave usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored \
  usr/bin/doors-shell etc/doors-release etc/pocketos-release usr/share/doors/THIRD_PARTY_NOTICES.txt \
  usr/share/pocketos/THIRD_PARTY_NOTICES.txt etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod \
  etc/init.d/S65meshcored etc/init.d/S90doors-shell
cp -p /var/lib/pocketos/meshcored/state.v1 /var/lib/pocketos/meshcored/channels.v1 $R/
(cd $R && sha256sum * > SHA256SUMS && cat SHA256SUMS)
```

## Step 2 — deploy, then reboot

From the build host: `bash platforms/k230/scripts/deploy.sh $UNIT ~/work/t-display-k230`
(in `$C`). Pass: rc 0, the closing `doors version` says `b41be37`, and the
deploy does not report that S65meshcored failed.

On the unit, redefine `snap` if the shell is new. Then `snap predeploy-reboot`
(its `loglines.txt` holds the offsets for step 10) and `reboot`.

## Step 3 — radiod and meshcored start (unit, after boot)

`snap boot; cat $D/boot/builds.txt $D/boot/services.txt $D/boot/crash.txt $D/boot/status.txt`

Pass:
- the shell, `mesh.info` and `/etc/doors-release` all say `b41be37`;
- every service is `running=1 crashloop=0 restarts=0`;
- no crashloop marker, and no crash report newer than the ones in step 1;
- `mesh.status` is `state online`, `lease_held true`, `radio_state rx`.

## Step 4 — name, identity, channels unchanged

```sh
diff $D/pre/identity.json $D/boot/identity.json && echo identity-same
diff $D/pre/channels.json $D/boot/channels.json && echo channels-same
grep -E 'identity.id|channels.v1' $D/pre/sha.txt $D/boot/sha.txt
```

Pass: both `-same` lines printed. The name is `Mstr_k230`, the key
`19f7b327…`, and the channels are slot 0 `#doorsbench` `9a`, 1 `Public`
`11`, 2 `test` `7b`. The identity.id and channels.v1 sha256 are identical
before and after.

## Step 5 — the 256 build is the one running

`grep meshcored $D/boot/sha.txt` must equal step 0's meshcored sha256, and
`mesh.info` build must be `b41be37`. The behaviour that proves the capacity
comes in step 7: `contacts_full` stays 0 past 32.

## Step 6 — the stored contacts reloaded

```sh
grep -c . $D/pre/keys.txt                                        # expect 32
grep -F -x -f $D/pre/keys.txt $D/boot/keys.txt | grep -c .       # must equal the line above
```

Pass: every pre-gate key is present after the reboot. The table may already
hold more than 32, because new adverts are now kept.

## Step 7 — the table grows past 32

Let the unit listen (panel on RIFT is fine). Every few minutes, up to
**60 minutes**:

`snap grow; cat $D/grow/status.txt; echo; cat $D/grow/state_bytes.txt`

Pass: `nodes` above 32, `contacts_full 0`, and `nodes_unretained 0` (below
256 nothing is turned away). `state.v1` bytes equal `44 + 148 × nodes`,
within one persist interval (10 s). If `nodes` is still 32 after 60 minutes
and step 1 showed no `nodes_unretained` growth, record step 7 as
**INCONCLUSIVE (quiet mesh)**, not FAIL.

Then one restart through the init script, to reload a table larger than 32
on hardware:

```sh
/etc/init.d/S65meshcored restart; sleep 5; snap reload
tr -d ' \t\n' < $D/reload/status.json | grep -oE '"nodes":[0-9]+'
grep -F -x -f $D/grow/keys.txt $D/reload/keys.txt | grep -c . ; grep -c . $D/grow/keys.txt   # equal
```

## Step 8 — `mesh.nodes` is newest heard first

After some adverts have been heard since the reload:

```sh
doors call meshcored mesh.nodes > $D/order.json
awk '/"public_key"/ {n++; h[n] = "-"}
     /"last_heard_mono_ms"/ {v = $0; gsub(/[^0-9]/, "", v); h[n] = v}
     END {bad = 0; u = 0; heard = 0; prev = -1
          for (i = 1; i <= n; i++) {
              if (h[i] == "-") { u = 1; continue }
              heard++; if (u) bad++
              if (prev >= 0 && h[i] + 0 > prev + 0) bad++
              prev = h[i] }
          print "nodes " n ", heard " heard ", order violations " bad }' $D/order.json
grep '"name"' $D/order.json | head -3
wc -c < $D/order.json
```

Pass: `order violations 0`, which means heard nodes come first with
`last_heard_mono_ms` never increasing down the list. Record the first three
names for step 9 and the reply size in bytes (1 MiB is the IPC limit;
256 nodes is about 124 KB at worst).

## Step 9 — RIFT keeps and shows the newest 64

Open RIFT → NODES. Record the group labels (`HEARD < 12 H · a`,
`NOT HEARD > 12 H · b`, `NEVER HEARD · c`).

Pass:
- **a + b + c = min(`nodes`, 64)**;
- the first rows of the HEARD group are step 8's first names, in that order;
- the footer "…adverts the service had no room to keep" is absent.

If `nodes` is above 64, also check that the HEARD count is
`min(heard, 64)`, and that the last name in `$D/order.json`
(`grep '"name"' $D/order.json | tail -1`) is not in the list. If `nodes` stays
at 64 or below, record that the >64 case rests on the host evidence
(`tests/rift_model_test.c`).

## Step 10 — health over 10 minutes on the new build

With RIFT on NODES for most of it, run `snap post; sleep 600; snap post10`. Then:

```sh
cat $D/post10/services.txt $D/post10/crash.txt $D/post10/rss.txt $D/pre/rss.txt
L=/var/lib/pocketos/log
for f in shell radiod meshcored; do
  off=$(awk -v f=$f '$1 == f {print $2}' $D/predeploy-reboot/loglines.txt)
  echo "== $f since the reboot: $(tail -n +$((off + 1)) $L/$f.log | grep -cE ' (WARN |ERROR) ') WARN/ERROR"
  tail -n +$((off + 1)) $L/$f.log | grep -E ' (WARN |ERROR) ' | cut -d' ' -f3- | sort | uniq -c | sort -rn | head -5
  tail -n +$((off + 1)) $L/$f.log | grep -ciE 'too long|too large|EMSGSIZE|frame'
done
top -b -n 1 | grep -E 'doors-shell|meshcored' | head -3
```

Pass:
- no crashloop and no new crash report; restarts 0, apart from step 7's
  deliberate one;
- WARN/ERROR per 10 minutes no higher than the step 1 baseline, with only
  the known kinds (trap 5);
- 0 frame/size errors;
- meshcored RSS higher than step 1 by no more than the table and the buffers
  that follow it (about 260 KB), and **not still growing**: `VmHWM` equal to
  `VmRSS` across the window. The first draft said "within about 100 KB",
  which counted only the contact table and telemetry — see Findings;
- by eye, NODES scrolls and switches sections without a stall of a second or
  more while adverts arrive. Record the shell's CPU %.

## Step 11 — finish

Decide PASS or FAIL. Leave the unit on `b41be37` if it passed. Otherwise roll
back (below), and in either case state at the top of this sheet what the
unit carries.

**As left, 18:16 UTC:** `b41be37` everywhere, 37 nodes and rising, nothing
turned away, `online` with the lease held, identity and channels as they
were, rotation automatic, RIFT open on NODES. The rollback copy stays at
`/root/rollback-pre256/` and the snapshots at `/root/gate-256/` (collected
to `out/nodecap256-gate/unitA-gate-256.tar` on the build host). The SDK tree
`~/work/t-display-k230` now carries the `b41be37` apply: re-apply master
before building anything else from it.

## Rollback (unit)

```sh
R=/root/rollback-pre256; (cd $R && sha256sum -c SHA256SUMS)
for s in S90doors-shell S65meshcored S60radiod S55netd S50sysd; do /etc/init.d/$s stop; done
cd / && tar -xf $R/userspace-before.tar
cp -p $R/state.v1 /var/lib/pocketos/meshcored/state.v1        # REQUIRED - trap 3
cmp -s $R/channels.v1 /var/lib/pocketos/meshcored/channels.v1 || echo "channels.v1 changed: investigate, do not overwrite blindly"
reboot
```

After the reboot, `mesh.info` shows the old build, `nodes` is 32, and
identity and channels match step 1. Nodes learned during the gate are gone,
by design. If `state.v1` was not restored and the old build moved it aside,
recover it:

```sh
/etc/init.d/S65meshcored stop
cp -p /root/rollback-pre256/state.v1 /var/lib/pocketos/meshcored/state.v1
/etc/init.d/S65meshcored start
```

## Not covered on hardware

**After the run**, these are what the PASS does not cover.

- **More than 64 real nodes — not reached.** The table went from 32 to 37 in
  the 26 minutes after the reboot, so RIFT was never asked for more than it
  holds, and the head-of-list behaviour that the `mesh.nodes` order exists
  for is still only host evidence (`tests/rift_model_test.c`). The same
  mesh should get there on its own; a later look at this unit with the node
  count above 64 would close it with no new build.
- **A full table of 256** on the device: memory, CPU and `mesh.nodes` at
  that size. The run measured 37 nodes and an 8,087 B reply; 256 would be
  about seven times that, still far inside the 1 MiB frame limit, but it is
  host evidence.
- **Flash writes over time.** `state.v1` is up to 38 KB, rewritten at most
  every 10 s while nodes change. There is no soak here.
- **More `mesh.node` events.** Adverts from nodes the old table turned away
  now reach RIFT as events and re-sort NODES. Step 10 observes it for 10
  minutes only.
- **One-byte hash collisions.** Inbound direct packets are matched by a
  one-byte hash, and at most 8 contacts sharing it are tried. This is
  negligible at 256 in simulation, and not observed on air.

## Results

Run by Claude over SSH from the build host, 2026-09-22. The panel was read
with the earlier gates' `kmsgrab` capture (`out/nodecap256-gate/caps/`, not
in the repository); the NODES list was tapped and scrolled with the RIFT
gate's `rift_tap.py`. Nothing was typed on the unit by hand.

| Step | Result | Evidence |
| --- | --- | --- |
| 0 build | **PASS** | `IMAGE GATE: PASS`, `BUILD_ID=b41be37`, `DOORS_BUILD_ID=b41be37` in the binary; target meshcored `5d6833b5…`, doors-shell `5ebc738b…` |
| 1 baseline + rollback copy | **PASS** | 17:38 UTC on `5bb51aa`: 32 nodes (full), `nodes_unretained` 18 → 19 over ten minutes, so the mesh had more nodes than the table could keep; 0 WARN/ERROR and **no new log lines at all** in those ten minutes; `/root/rollback-pre256/` holds the 20 deployed paths (1,789,952 B), `state.v1` (4,780 B) and `channels.v1`, with `SHA256SUMS` |
| 2 deploy + reboot | **PASS** | `deploy.sh` rc 0, every stamp `b41be37`; reboot at 17:49:50, meshcored answering 53 s later (uptime 43 s) |
| 3 services start | **PASS** | shell, `mesh.info`, `/etc/doors-release` all `b41be37`; all five services `running=1 crashloop=0 restarts=0`; 0 crash files; `online`, `lease_held`, `radio_state rx`; meshcored started by `S65meshcored` at boot uptime 17.0 s, online at 17.1 s |
| 4 identity / channels | **PASS** | `mesh.identity` and `mesh.channels` byte-identical before and after (`diff` clean); `identity.id` `41e8a50a…` and `channels.v1` `e8247d67…` unchanged; still `Mstr_k230` / `19f7b327…`, channels `9a`/`11`/`7b` |
| 5 256 build running | **PASS** | on-unit meshcored sha256 `5d6833b5…` = the build's; `mesh.info` build `b41be37`; and step 7's growth is the capacity itself |
| 6 contacts reloaded | **PASS** | 32 of 32 pre-gate public keys present after the reboot |
| 7 growth past 32 + reload | **PASS** | 33 nodes at 17:52 (`state.v1` 4,928 B = 44 + 148 × 33), 35 by 17:58 (5,224 B), `contacts_full` and `nodes_unretained` **0** throughout — where the old build had turned 19 adverts away. `S65meshcored restart`: 35 of 35 keys back, online again in under 1 s, same argv |
| 8 newest first | **PASS** | 35 nodes, 3 heard since the restart, **0 order violations**: `Mstr_m5` 956936, `Varden RP` 950923, `T-Deck-RIFT` 858826, then the 32 not heard this run. Reply 8,087 B |
| 9 RIFT newest 64 | **PASS (table ≤ 64)** | `35 KNOWN · 3 FRESH`; groups `HEARD < 12 H · 3` + `NEVER HEARD · 32` = 35 = min(35, 64), so RIFT held every node; its HEARD rows are the service's order exactly; the "adverts the service had no room to keep" footer, present on the old build, is gone. Above 64 was not reachable — see "Not covered on hardware" |
| 10 health | **PASS** | ten minutes with NODES open: **0 WARN/ERROR** in shell, radiod and meshcored since the reboot (18 / 7 / 18 new lines), 0 frame or size errors, 0 crash files, 0 restarts; CPU 0–10 % for the shell and 0–3 % for meshcored and radiod; RSS 3,456 kB with `VmHWM` equal to it. The list scrolled and kept its place while a 36th node arrived |

## Findings

1. **The RSS criterion in the first draft was too tight, not the build.**
   meshcored measured 3,200 kB on `5bb51aa`, 3,328 kB at first boot on
   `b41be37` and 3,456 kB once the table had grown — **+256 kB**, against a
   criterion of "about 100 KB". The 100 KB counted only the contact table
   (+42 KB) and telemetry (+18 KB); it left out the two `state.v1` buffers
   (2 × 38 KB), the `NodeState` the load and save paths put on the stack
   (47 KB) and the node array `mesh.nodes` allocates (47 KB), which together
   account for the rest. It is not a leak: `VmHWM` equals `VmRSS`, and the
   figure was flat from 18:05 to the end of the run. The criterion above is
   corrected; the build is unchanged.
2. **`NEVER HEARD · 32` right after the reboot** is the pre-existing
   follow-up F4 of the RIFT improvements gate: last-heard is runtime-only,
   so a restarted service reports no node as heard until it hears one. Not
   this branch's, and it resolved itself as adverts arrived (3 FRESH by
   18:06, 4 by 18:07).
3. The status chip still reads `DX` in landscape (chrome stage 1,
   `a89b456`), as predicted by that gate.
