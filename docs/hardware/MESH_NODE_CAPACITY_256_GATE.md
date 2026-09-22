# 256-node interim capacity on unit A: the gate

**Status: NOT RUN.** Prepared 2026-09-22. Nothing has been deployed to
unit A for this branch yet.

## The build the unit must carry — check this before anything else

Every observation below is about **`b41be37`** only if the unit is running
it. After step 3, `doors shell info`, `mesh.info` and `/etc/doors-release`
must all say `b41be37`. Fill in the right-hand column as you go.

| On the unit | The gate needs | Last recorded (bench record 2026-09-22, not in the repo; reboot PASS) | Filled in |
| --- | --- | --- | --- |
| whole userspace (`deploy.sh` set: services, shell, CLI, init scripts, release file) | **`b41be37`**, one build | `5bb51aa`, deployed with `deploy.sh`, meshcored started by `S65meshcored` | step 1: ______ step 3: ______ |
| `/etc/default/meshcored` | untouched: `MESHCORED_ENABLE=1`, `MESHCORED_NAME=Mstr_k230`, `MESHCORED_TX_POWER_DBM=2` | as stated | |
| Rotation mode | as found; record it | not recorded | |

**The build under test is `feat/mesh-node-capacity-256` at `b41be37`**, on
master `2d0914b`. VERSION stays `0.0.10`. Not merged. Commits after
`b41be37` on this branch change this sheet only, so **build from `b41be37`
itself**. A build of the branch tip would stamp a different build id.

## What this gate is for

| | |
| --- | --- |
| **HOST VERIFIED** (`b41be37`, clean clones) | 255 nodes held with nothing turned away, the 256th kept, the 257th turned away and counted; a full table of 256 persisted, reloaded whole and still full; `state.v1` refuses 257; `mesh.nodes` newest heard first, then by the stored last-updated time after a restart; RIFT keeps exactly the newest 64 of a 256-node snapshot. Five mutants of the fix, each killed by a named check. `make test`, `make meshcored-test` (store 161, runtime 363, plain and ASan/UBSan), `meshcore-core-test`, `rift_shell_test.sh` (app 338), the RIFT host suites under ASan/UBSan, riscv64 `make all` + meshcored `-Werror` + DRM shell, 0 first-party warnings. |
| **THIS GATE** | The same build on unit A's real radio and real mesh: it starts, keeps its identity, channels and 32 stored contacts, grows past 32 from real adverts, lists newest first, and RIFT on the panel follows. |
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
- meshcored RSS within about 100 KB of step 1;
- by eye, NODES scrolls and switches sections without a stall of a second or
  more while adverts arrive. Record the shell's CPU %.

## Step 11 — finish

Decide PASS or FAIL. Leave the unit on `b41be37` if it passed. Otherwise roll
back (below), and in either case state at the top of this sheet what the
unit carries.

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

- **More than 64 real nodes.** Only if the mesh supplies them in the step 7
  window. RIFT keeping the head of a larger list is otherwise host evidence.
- **A full table of 256** on the device: memory, CPU and `mesh.nodes`
  (about 124 KB) at that size. Host-measured only.
- **Flash writes over time.** `state.v1` is up to 38 KB, rewritten at most
  every 10 s while nodes change. There is no soak here.
- **More `mesh.node` events.** Adverts from nodes the old table turned away
  now reach RIFT as events and re-sort NODES. Step 10 observes it for 10
  minutes only.
- **One-byte hash collisions.** Inbound direct packets are matched by a
  one-byte hash, and at most 8 contacts sharing it are tried. This is
  negligible at 256 in simulation, and not observed on air.

## Results

| Step | Result | Evidence |
| --- | --- | --- |
| 0 build | | |
| 1 baseline + rollback copy | | |
| 2 deploy + reboot | | |
| 3 services start | | |
| 4 identity / channels | | |
| 5 256 build running | | |
| 6 contacts reloaded | | |
| 7 growth past 32 + reload | | |
| 8 newest first | | |
| 9 RIFT newest 64 | | |
| 10 health | | |
