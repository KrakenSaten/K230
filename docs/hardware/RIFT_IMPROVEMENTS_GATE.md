# RIFT improvements on unit A: the gate

**Status: NOT RUN. Prepared 2026-09-22.** Nothing on this branch has been on
unit A, and preparing this sheet did not touch the unit.

## The build the unit must carry — check this before anything else

A physical check is only evidence about the build on the glass. Before any
observation is written down, `doors shell info` must say **`3e89c9c`**, and
this table's right-hand column must be filled in from step 3.

| On the unit | The gate needs | Last recorded on the unit (2026-09-21 20:18 UTC) | Filled in at step 3 |
| --- | --- | --- | --- |
| `/usr/bin/doors-shell` | **`3e89c9c`** — this branch. It carries master's chrome policy stage 1 (`a89b456`) as well, because the branch is built on it | `a89b456` | |
| `/usr/sbin/meshcored` | **`3e89c9c`** | `765a3a3` | |
| `/usr/sbin/radiod` | `765a3a3`, **not replaced**: `services/radiod` has no change since `765a3a3` (`git diff --stat 765a3a3..3e89c9c -- services/radiod` is empty) | `765a3a3` | |
| `sysd`, `netd` | not replaced | `646dcbb` | |
| `doors` CLI, `/etc/doors-release` | not replaced; neither says anything about RIFT | `a89b456` | |
| Rotation mode | record it in step 1, restore it at step 12 | not recorded | |

"Last recorded" is the chrome stage 1 bench record,
`out/chrome-a89b456/hwgate-unitA/STATE.md` on the build host. The unit may
have been reflashed or rebooted since: step 1 is what counts. If it was
rebooted, `meshcored` is not running (it is started by hand and has no init
script); step 2 covers that.

**The build under test is `feat/rift-improvements` at `3e89c9c`,** from
origin/master `a89b456`. VERSION stays `0.0.10`. Not merged, and not to be
merged unless this gate passes. Commits after `3e89c9c` on the branch change
this sheet and nothing else.

## What this gate is for

| | |
| --- | --- |
| **HOST VERIFIED** | rift_format 92, rift_model 193, rift_comms 239, rift_ipc 186, rift_app 338, meshcored_runtime 368 named checks, 0 failures; `make test` 4239 ok / 0 FAIL; `meshcored_service_test` against the real `radiod` on its mock backend; the RIFT suites and the meshcored suites again under ASan + UBSan with leak detection; four lints. A mutation pass broke each fix on purpose, 34 times, and a named check failed every time. |
| **HOST VERIFIED** | The target build: `doors-shell` and `meshcored` for riscv64, from a `git archive` of `3e89c9c`, 0 objects before the build, 0 warnings. See Provenance. |
| **NOT VERIFIED — this gate** | Everything below: the new service methods on a real radio, the adverts on the air, forgetting and re-routing against a real peer, two direct messages in flight, the screens on the panel, and that channels and direct messages are unchanged. |

What the branch changed, in one line each (details in `docs/apps/RIFT.md`,
`docs/services/MESHCORED.md` and `docs/api/mesh.md`):

- **meshcored** — each direct message waits for its own ACK; before, an ACK for
  one message cancelled another's timeout and left it `sent_*` for ever.
  `mesh.node_remove` and `mesh.node_reset_path` are new; `mesh.advert` takes
  `zero_hop`.
- **RIFT** — ADVERT NEAR / MESH, FORGET (after a confirmation) and RE-ROUTE;
  panel captions no longer clipped; lists keep their place; the command line
  only where it holds something; `NOT DONE` kept apart from `NO ANSWER`; the
  table-full warning counts only since the last forget.

## On the panel, and not this branch's

Written down first, so they are not held against the branch
(`docs/hardware/FLEET_LANDSCAPE_GATE.md` is the reason this section exists).

| What you may see | Whose | |
| --- | --- | --- |
| The landscape status bar's radio chip cuts the tops off `RX` | chrome stage 1, `a89b456` | COMPACT chip is 24 px with 5 px padding for a 22 px line; fix recorded, not made (the chrome record's follow-up 1). The shell under test carries it because it is built on `a89b456`. |
| Landscape top corners inset 50 px | this unit's `/etc/default/doors-shell` (`POCKETOS_SAFE_CORNERS=50,30,30,50`) | a per-unit bench setting, not in the repo. **Leave it as it is.** |
| A shell `WARN radio.status poll failed: timed out after 200 ms` | the status bar's own poll of `radiod` while the radio is busy | pre-existing, recorded by the phase 2 re-gate and the `meshcored` hardware gate |
| Emoji in a remote name or message drawn as a box | font coverage | pre-existing |
| `sysd` reporting `646dcbb` | not redeployed on purpose | |

## The peers

| | Unit A | The T-Deck | A second answering node (step 7 only) |
| --- | --- | --- | --- |
| What | LILYGO T-Display-K230 | LILYGO T-Deck, RIFT v0.9.5 | any MeshCore node that acknowledges direct messages and that the owner is content to message |
| Name | `Mstr_k230` | `T-Deck-RIFT` | e.g. `Mstr_m5`, which ACKed in about 1 s on 2026-09-20 |
| Public key | `19f7b327a254fe8a2565578a29a5d40b1202536f2cca3243f869972b59043715` | `e34a0352bb535d71a602e30b147108f7453e0c34cd5c3aefefb5a4596f686d38` as of 2026-09-21. It was re-keyed once before, so have it advert and read the key back from `mesh.nodes` | take the whole key from `mesh.nodes`. **Two nodes share the prefix `c0`**, so a short prefix is ambiguous. |
| Channels | slot 0 `#doorsbench` (hash `9a`), 1 `Public` (`11`), 2 `test` (`7b`) | needs `#doorsbench` for step 9: COMMS → add channel → `doorsbench`, no `#`, "Hashtag - open topic" | |

The T-Deck needs no new firmware. The T-Deck survey
(`vendor/RIFT/examples/companion_radio/ui-rift/UITask.cpp`) puts ADVERT NEAR
and ADVERT MESH on its home screen.

## Read before starting — the traps

1. **Never start `meshcored` with `--name`.** The node is `Mstr_k230`, stored in
   `state.v1`; the older sheets' `--name K230-A` would rename it. Every restart
   here goes through `mcd-restart.sh`, which reuses the argv the service was
   running with and refuses a `--name` other than `Mstr_k230`.
2. **Never touch `/var/lib/pocketos/meshcored/identity.id`.** It is this node's
   private key; every peer knows the unit by it and it cannot be recovered.
3. **A full table and FORGET.** If `mesh.status` says `nodes: 32`, forgetting
   the T-Deck opens a slot that any stranger's advert can take before the
   T-Deck adverts again — and then the T-Deck cannot come back, and a direct
   message to it is refused. So step 8a forgets a **stranger** first, and at
   8c the T-Deck adverts **at once** after it is forgotten.
4. **A rotation change restarts the shell.** `shell.rotation` re-executes the
   shell in place and it comes back on the launcher, so RIFT is closed and
   reopened. It is not a way to test anything that lives inside one RIFT
   session (see "Host only").
5. **Messages do not survive a `meshcored` restart** (`persistent: false`), and
   RIFT empties its own copy when it sees the new run. An empty thread after a
   restart is correct.
6. `doors call` takes `key=value`; `true` and `false` become JSON booleans,
   numbers become numbers, and everything else is a string.
7. If taps are injected rather than made by hand, hold them 150 ms; 60 ms is
   marginal on this panel.

## The payload

Two files. They are on the build host in **`~/work/rift-gate-out-3e89c9c/`**
(`$OUT` below), with `SHA256SUMS` beside them. `deploy.sh` is not used, no init
script is added, nothing under `/etc` is written, and no image is built.

| Path on the unit | sha256 | Size |
| --- | --- | --- |
| `/usr/bin/doors-shell` | `dddc541fd6ff843f6920071ada50e63742f8026960b526ddc3b6885796da4a81` | 1 042 944 |
| `/usr/sbin/meshcored` | `7e538a07e7ae64c28c3e81c345f3a25c1621a6bcb2d6ce9b3f30614a7d356fce` | 231 952 |

---

# The procedure

Twelve steps. 1–4 need no peer; 5–9 need the T-Deck and a pair of eyes;
10 is the screens; 11 and 12 put it back. Run from the build host (WSL):

```bash
A=root@192.168.10.157      # unit A on the bench, as of 2026-09-21
SSHO="-i $HOME/.ssh/pocketos_bench -o UserKnownHostsFile=$HOME/work/chrome-gate-ssh/known_hosts -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=8"
OUT=$HOME/work/rift-gate-out-3e89c9c
RB=/root/rollback-rift-improvements
SELF=19f7b327a254fe8a2565578a29a5d40b1202536f2cca3243f869972b59043715
TDECK=e34a0352bb535d71a602e30b147108f7453e0c34cd5c3aefefb5a4596f686d38   # confirm at step 5
MCF=$HOME/work/rift-improve/tools/meshcore-frame/meshcore-frame           # any build of tools/meshcore-frame
u() { ssh $SSHO $A sh -s -- "$@"; }   # runs the script on stdin on the unit
```

Each unit-side script below is a quoted heredoc fed to `u`, so nothing in it
is expanded on the build host.

### 1. Survey — the rollback record and the "before" reading

```bash
u <<'EOF'
echo "== release: $(tr '\n' ' ' < /etc/doors-release)"
echo "== shell: $(doors shell info | tr -d ' \t\n' | grep -oE '"(build|current|orientation)":"[^"]*"|"chrome":\{[^}]*\}' | tr '\n' ' ')"
echo "== rotation: $(doors call shell shell.rotation | tr -d ' \t\n' | grep -oE '"(rotation_mode|orientation|keyboard)":"[^"]*"|"bench_override":[a-z]*' | tr '\n' ' ')"
P=$(pidof meshcored)
echo "== meshcored pid ${P:-NOT RUNNING}: $(tr '\0' ' ' 2>/dev/null < /proc/${P:-0}/cmdline)"
echo "== mesh.info: $(doors call meshcored mesh.info 2>&1 | tr -d ' \t\n' | grep -oE '"(version|build)":"[^"]*"' | tr '\n' ' ')"
echo "== mesh.identity: $(doors call meshcored mesh.identity 2>&1 | tr -d ' \t\n')"
echo "== mesh.status: $(doors call meshcored mesh.status 2>&1 | tr -d ' \t\n' | grep -oE '"(state|radio_state)":"[^"]*"|"lease_held":[a-z]*|"(nodes|channels)":[0-9]+|"(nodes_unretained|contacts_full|tx_submitted|tx_ok|tx_failed)":[0-9]+' | tr '\n' ' ')"
sha256sum /usr/bin/doors-shell /usr/sbin/meshcored /usr/sbin/radiod
echo "== radiod: $(cat /etc/default/radiod 2>/dev/null || echo 'NO /etc/default/radiod (mock backend)')"
echo "== doors-shell defaults: $(grep -v '^#' /etc/default/doors-shell 2>/dev/null | tr '\n' ' ')"
ls -l /var/lib/pocketos/meshcored/
for l in liblvgl.so.9 libcjson.so.1 libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1 libstdc++.so.6 libgcc_s.so.1; do
    f=$(ls /usr/lib/$l /lib/$l 2>/dev/null | head -1); echo "lib $l: ${f:-MISSING}"
done
ls /run/pocketos/*crashloop* 2>/dev/null || echo "no crashloop markers"
EOF
```

Keep the output. **Stop** if `/etc/default/radiod` does not say
`RADIOD_BACKEND=sx1262` (nothing below can transmit), if any library is
`MISSING` (the payload would not start), or if `identity.id` is not there.

### 2. Rollback copies and the two helpers

```bash
u <<'EOF'
RB=/root/rollback-rift-improvements; D=/var/lib/pocketos/meshcored
mkdir -p $RB
[ -f $RB/doors-shell ] || cp -p /usr/bin/doors-shell $RB/doors-shell
[ -f $RB/meshcored ] || cp -p /usr/sbin/meshcored $RB/meshcored
[ -f $RB/state.v1 ] || cp -p $D/state.v1 $RB/state.v1
[ -f $RB/channels.v1 ] || cp -p $D/channels.v1 $RB/channels.v1
if [ ! -f $RB/meshcored.argv ]; then
    P=$(pidof meshcored)
    if [ -n "$P" ]; then tr '\0' ' ' < /proc/$P/cmdline > $RB/meshcored.argv
    else echo "meshcored --frequency-mhz 869.618 --bandwidth-khz 62.5 --spreading-factor 8 --coding-rate 5 --sync-word 0x12 --preamble 32 --tx-power-dbm 2 --verbose" > $RB/meshcored.argv
    fi
fi
cat > $RB/mcd-restart.sh <<'X'
#!/bin/sh
# Restart meshcored with the argv it ran with before this gate - never with a
# --name that would rename the node.
RB=/root/rollback-rift-improvements
ARGV=$(cat $RB/meshcored.argv)
case "$ARGV" in
*"--name Mstr_k230"*) ;;
*--name*) echo "REFUSED: the saved argv renames the node: $ARGV"; exit 1 ;;
esac
killall meshcored 2>/dev/null
n=0; while pidof meshcored >/dev/null && [ $n -lt 10 ]; do sleep 1; n=$((n + 1)); done
nohup $ARGV >>/tmp/meshcored-bench.log 2>&1 </dev/null &
n=0; S=
while [ $n -lt 15 ]; do
    S=$(doors call meshcored mesh.status 2>/dev/null | tr -d ' \t\n')
    case "$S" in *'"state":"online"'*) break ;; esac
    sleep 1; n=$((n + 1))
done
echo "meshcored pid $(pidof meshcored): $(echo "$S" | grep -oE '"state":"[^"]*"|"lease_held":[a-z]*|"(nodes|channels)":[0-9]+' | tr '\n' ' ')"
echo "build: $(doors call meshcored mesh.info 2>/dev/null | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
grep -h 'known node' /tmp/meshcored-bench.log /var/lib/pocketos/log/meshcored.log 2>/dev/null | tail -1
exit 0
X
cat > $RB/RESTORE.sh <<'X'
#!/bin/sh
# Put back the doors-shell and meshcored this gate replaced. The node table is
# restored only with --with-state: a forgotten node comes back by its own
# advert, and restoring state.v1 also discards every node learned since.
RB=/root/rollback-rift-improvements
/etc/init.d/S90doors-shell stop; sleep 1
killall meshcored 2>/dev/null
n=0; while pidof meshcored >/dev/null && [ $n -lt 10 ]; do sleep 1; n=$((n + 1)); done
cp -p $RB/doors-shell /usr/bin/doors-shell
cp -p $RB/meshcored /usr/sbin/meshcored
[ "$1" = --with-state ] && cp -p $RB/state.v1 /var/lib/pocketos/meshcored/state.v1
sync
sh $RB/mcd-restart.sh
/etc/init.d/S90doors-shell start; sleep 3
sha256sum /usr/bin/doors-shell /usr/sbin/meshcored
echo "shell $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
X
cat $RB/meshcored.argv; echo; sha256sum $RB/doors-shell $RB/meshcored $RB/state.v1 $RB/channels.v1
EOF
```

The argv printed last must not carry `--name K230-A`. The two binary hashes
must match step 1's. The copies are taken once: a second run of this step keeps
the first ones.

### 3. Install, and the build identity

```bash
(cd "$OUT" && sha256sum -c SHA256SUMS) &&
ssh $SSHO $A '/etc/init.d/S90doors-shell stop; killall meshcored; sleep 2' &&
tar -C "$OUT" --owner=0 --group=0 --numeric-owner -cf - doors-shell meshcored |
  ssh $SSHO $A 'tar -C /tmp -xf - && install -m 0755 -o 0 -g 0 /tmp/doors-shell /usr/bin/doors-shell && install -m 0755 -o 0 -g 0 /tmp/meshcored /usr/sbin/meshcored && rm -f /tmp/doors-shell /tmp/meshcored && sync && sha256sum /usr/bin/doors-shell /usr/sbin/meshcored' &&
ssh $SSHO $A 'sh /root/rollback-rift-improvements/mcd-restart.sh; /etc/init.d/S90doors-shell start'
u <<'EOF'
sleep 3
echo "shell:     $(doors shell info | tr -d ' \t\n' | grep -oE '"(build|orientation)":"[^"]*"' | tr '\n' ' ')"
echo "meshcored: $(doors call meshcored mesh.info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
echo "radiod:    $(doors radio info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
echo "identity:  $(doors call meshcored mesh.identity | tr -d ' \t\n' | grep -oE '"(name|node_hash)":"[^"]*"' | tr '\n' ' ')"
echo "shells:    $(pidof doors-shell | wc -w)"
EOF
```

| Must be true | |
| --- | --- |
| both installed hashes match the payload table | |
| shell `"build":"3e89c9c"`, meshcored `"build":"3e89c9c"`, radiod `"build":"765a3a3"` | **write these into the table at the top now** |
| `mesh.status` `online`, lease held | printed by `mcd-restart.sh`. If it says `degraded`, or the lease is elsewhere, stop. |
| `name` is `Mstr_k230`, `node_hash` `19` | the identity survived |
| exactly one `doors-shell` | |

### 4. The new methods, with nothing transmitted

```bash
u <<'EOF'
tx() { doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"tx_submitted":[0-9]+'; }
Z=0000000000000000000000000000000000000000000000000000000000000000
echo "before: $(tx)"
echo "-- a prefix is refused:";           doors call meshcored mesh.node_remove node=19f7b327
echo "-- a key nobody holds is refused:"; doors call meshcored mesh.node_reset_path node=$Z
echo "-- zero_hop must be a boolean:";    doors call meshcored mesh.advert zero_hop=yes
echo "after:  $(tx)"
EOF
```

| Must be true | |
| --- | --- |
| the prefix: error 2, `node must be a whole public key, 64 hex characters` | a guess is never acted on |
| the unknown key: error 2, `no node with that public key is held` | |
| `zero_hop=yes`: error 2, `zero_hop must be true or false` | a string is not read as "true" |
| `tx_submitted` unchanged | none of the three reached the radio |

Then open RIFT (`ssh $SSHO $A 'doors call shell shell.open id=rift'`, or from
the launcher) and look at ACTIVITY: the RADIO SERVICE panel has a
`TRAFFIC · RX … · TX … OK` line, and THIS DEVICE has **ADVERT NEAR** and
**ADVERT MESH**, both enabled. **DEVICE VERIFIED** for this step.

### 5. ADVERT MESH, then ADVERT NEAR — part 1

The flooded one goes first because it is the control: a repeater in range
relays it, and unit A hears its own advert come back with a path. The zero-hop
one must **not** come back that way. The absence only means something if the
flood's echo was seen.

Paste this once; it listens for 45 s while you press the button, then reports
every copy of unit A's own advert it heard:

```bash
adv() {
  s() { ssh $SSHO $A 'doors call meshcored mesh.status' | tr -d ' \t\n' | grep -oE '"(tx_submitted|tx_ok|tx_failed)":[0-9]+' | tr '\n' ' '; }
  echo "before: $(s)"
  ssh $SSHO $A 'doors radio listen 45' > /tmp/adv-$1.jsonl &
  echo ">>> press $1 on unit A now (listening 45 s)"; wait
  echo "after:  $(s)"
  grep -oE '"payload_hex":"[0-9a-fA-F]+"' /tmp/adv-$1.jsonl | cut -d'"' -f4 | while read -r h; do
    "$MCF" parse "$h" > /tmp/adv-frame.txt 2>&1
    grep -qi "^advert.public_key: $SELF" /tmp/adv-frame.txt &&
      grep -E '^(route_type|path_hash_count|advert.name):' /tmp/adv-frame.txt | tr '\n' ' ' && echo
  done
  echo "frames heard: $(grep -c payload_hex /tmp/adv-$1.jsonl)"
}
adv "ADVERT MESH"
sleep 2
adv "ADVERT NEAR"
```

| Must be true | Evidence class |
| --- | --- |
| each press: `tx_submitted` and `tx_ok` up by exactly one, `tx_failed` unchanged | LOCAL TX PATH |
| under the buttons: `FLOOD ADVERT · ACCEPTED …s AGO`, then `ZERO-HOP ADVERT · ACCEPTED …s AGO` — **accepted**, never "sent" | DEVICE |
| MESH ACTIVITY shows the transmit as `TX` with the service's `ok` | DEVICE |
| after ADVERT NEAR, the T-Deck's node list shows `Mstr_k230` heard just now | **ON-AIR** — a peer in direct range learned it from the zero-hop advert |
| after ADVERT MESH, at least one copy of our advert with `path_hash_count` ≥ 1 | **ON-AIR** — relayed by someone. If none, there is no repeater in range: record that, and the next row is **NOT RUN** rather than passed |
| after ADVERT NEAR, **no** copy of ours with `path_hash_count` ≥ 1 | **ON-AIR** — nobody repeated a zero-hop advert |
| the buttons are disabled while one advert is in flight, and whenever the service says the radio cannot send | DEVICE |

Read the T-Deck's key off its node entry, or from
`ssh $SSHO $A 'doors call meshcored mesh.nodes' | grep -B2 -A2 T-Deck`, and
update `TDECK` if it has changed.

### 6. RE-ROUTE — part 3

It needs a learned route to the T-Deck. Check:

```bash
ssh $SSHO $A "doors call meshcored mesh.node node=$TDECK" | grep -E '"(name|path_known|hops|direct)"'
```

If `path_known` is `false`, send one direct message to the T-Deck from RIFT
(NODES → the T-Deck → MESSAGE → type → SEND), wait for `DELIVERED`, and check
again.

Then on unit A: NODES → select `T-Deck-RIFT` → (portrait: its DETAIL; landscape:
the pane beside the list) → **RE-ROUTE**.

| Must be true | |
| --- | --- |
| the caption reads `ROUTE FORGOTTEN …s AGO · NEXT MESSAGE FLOODS` | DEVICE |
| `mesh.node` now says `"path_known": false` | DEVICE |
| RE-ROUTE is disabled: there is no route left to forget | DEVICE |
| `tx_submitted` unchanged: re-routing transmits nothing | DEVICE |

Now send a direct message to the T-Deck **from RIFT's composer**:

| Must be true | |
| --- | --- |
| the message is received on the T-Deck | **ON-AIR** |
| unit A's message caption ends `DELIVERED · ACK n s` | ON-AIR — the ACK came back |
| `mesh.messages` shows it went `sent_flood` (the route was forgotten), then `acked` | DEVICE |
| afterwards `mesh.node` says `path_known: true` again, and RE-ROUTE is enabled | ON-AIR — the reply taught a new route |

### 7. Two direct messages in flight — part 4, the bug the service used to have

Before this branch, an ACK for the second message cancelled the first
message's timeout, and the first stayed `sent_*` **for ever**. What must happen
now: the answered one is `acked`, and the unanswered one becomes `no_ack` at
**its own** deadline — not earlier, and not never.

You need one node that will not answer and one that will:

- **A (preferred):** the silent one is the T-Deck, **switched off**. It keeps its
  learned route in unit A's table, so the message goes direct and its deadline
  is short (about 7 s). The answering one is a second node, e.g. `Mstr_m5`:
  `ANSWER=<its whole key from mesh.nodes>`.
- **B, with one peer only:** the answering one is the T-Deck (switched on) and
  the silent one is a node in the table that has not been heard for over 12 h.
  If that node is in fact listening, its owner receives the text
  `inflight one <n>`. The owner decides whether B is acceptable.

With no answering node at all this part cannot be run. Record it **NOT RUN**:
one message on its own times out correctly on the old service too, so it would
prove nothing.

```bash
SILENT=$TDECK
ANSWER=          # the answering node's whole key, from mesh.nodes
u "$SILENT" "$ANSWER" <<'EOF'
[ ${#1} -eq 64 ] && [ ${#2} -eq 64 ] || { echo "SILENT and ANSWER must both be whole keys (64 hex)"; exit 1; }
N=$(( $(date +%s) % 10000 )); up() { cut -d' ' -f1 /proc/uptime; }
echo "t=$(up) one: $(doors call meshcored mesh.send to=$1 "text=inflight one $N" | tr -d ' \t\n')"
echo "t=$(up) two: $(doors call meshcored mesh.send to=$2 "text=inflight two $N" | tr -d ' \t\n')"
i=0
while [ $i -lt 40 ]; do
    echo "t=$(up) $(doors call meshcored mesh.messages limit=2 | grep -E '"(text|state)"' | tr -d '\t' | tr '\n' ' ')"
    sleep 1; i=$((i + 1))
done
EOF
```

`limit=2` is the two newest, so a third message arriving during the run shows
in the table; read it by its text.

| Must be true | |
| --- | --- |
| both answers `accepted`, each with its own `ack_timeout_ms` | |
| `inflight two` reaches `acked` | ON-AIR |
| `inflight one` stays `sent_direct` (or `sent_flood`) **until** its send time + its `ack_timeout_ms`, then becomes `no_ack` within about 2 s of it | DEVICE — the per-message deadline on real hardware |
| it does **not** become `no_ack` when `inflight two` is acknowledged | this is the discriminating observation |
| RIFT's thread for the silent peer says `NO ACK`; the other's says `DELIVERED · ACK n s` | DEVICE |

Switch the T-Deck back on before step 8.

### 8. FORGET — part 2, and the table-full warning

#### 8a. The table-full warning (only if the table is full)

If `mesh.status` says `nodes: 32` and `nodes_unretained` is above zero, the
warning must be on screen: in ACTIVITY `The node table is full: N adverts could
not be kept …`, and in NODES' footer `N adverts the service had no room to keep
· forget a node to make room`. If the table is not full, write **NOT
APPLICABLE** and go to 8b. The host tests cover the arithmetic.

To clear it, forget a **stranger** — a node not heard for over 12 h, not the
T-Deck (trap 3). Use the same FORGET steps as 8c.

| Must be true | |
| --- | --- |
| both warnings go as soon as the forget is answered, while `nodes_unretained` stays where it was | DEVICE — the count is since the last forget, not over the run |
| a warning returns only if a **new** advert is turned away afterwards (`nodes_unretained` goes up) | DEVICE, if it happens during the gate |

#### 8b. The confirmation, and every way out of it

NODES → `T-Deck-RIFT` → **FORGET**, then:

| Do | Must be true |
| --- | --- |
| look | `Forget T-Deck-RIFT?`, what it costs ("comes back when it next adverts"), two buttons, **CANCEL first and accented** |
| CANCEL | the actions are back (four in the portrait DETAIL, three in the landscape pane); `mesh.node node=$TDECK` still answers |
| FORGET again, then tap the **ACTIVITY** tab and come back to NODES | no question is waiting on the T-Deck |
| (portrait) FORGET again, then **‹ NODES** | closing the detail cancelled it |
| (landscape) FORGET again, then tap another row | a question about one node does not stay on another |

After all four, `mesh.node node=$TDECK` still answers and `tx_submitted` has not
moved. **DEVICE VERIFIED.**

#### 8c. Forget it

FORGET → **FORGET** (the confirmation). **Then have the T-Deck ready to advert
at once** if the table is full.

```bash
u "$TDECK" <<'EOF'
doors call meshcored mesh.node node=$1
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"nodes":[0-9]+'
EOF
```

| Must be true | |
| --- | --- |
| NODES' footer says `T-Deck-RIFT FORGOTTEN …s AGO · BACK WHEN IT ADVERTS`, and the row is gone | DEVICE |
| `mesh.node`: error 2, `no node with that public key prefix` | DEVICE |
| `nodes` one fewer than before | DEVICE |

#### 8d. It stays forgotten across a restart

```bash
ssh $SSHO $A 'sh /root/rollback-rift-improvements/mcd-restart.sh'
u "$TDECK" <<'EOF'
doors call meshcored mesh.node node=$1
echo "-- a message to it is refused, and nothing transmits:"
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"tx_submitted":[0-9]+'
doors call meshcored mesh.send to=$1 text=forgotten
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"tx_submitted":[0-9]+'
EOF
```

| Must be true | |
| --- | --- |
| the `nodes` count `mcd-restart.sh` prints (and its `known node(s)` log line, when the log has one) is one fewer than before the forget, unless a stranger's advert has filled the slot since — the `mesh.node` line below settles which | DEVICE — `state.v1` was written before the answer |
| `mesh.node`: still error 2 | DEVICE |
| `mesh.send`: error 2, `no single node matches …`, and `tx_submitted` unchanged | DEVICE |
| RIFT reconnected on its own, with no stale row for the T-Deck | DEVICE |

#### 8e. It comes back by its own advert

On the T-Deck: home screen → **ADVERT NEAR**.

| Must be true | |
| --- | --- |
| `mesh.node node=$TDECK` answers again, `path_known: false` | ON-AIR |
| the row is back in NODES | DEVICE |
| a direct message from RIFT's composer reaches the T-Deck and comes back `DELIVERED · ACK n s`; it went `sent_flood` | ON-AIR — the T-Deck still held unit A's contact, so it could decrypt and ACK |

### 9. Nothing else moved — channels, incoming messages, a restart

| Do | Must be true |
| --- | --- |
| T-Deck → unit A on `#doorsbench` | one message, in the `#doorsbench` thread, sender shown as a claim (`T-Deck-RIFT?`), header `CHANNEL · HASH 9a · FLOOD`, unread 0 → 1 with COMMS closed, cleared on opening. **ON-AIR** |
| unit A → T-Deck on `#doorsbench`, from the composer | arrives on the T-Deck; unit A shows `SENT · FLOOD · NO ACK ON CHANNELS` and **never** `DELIVERED`. ON-AIR |
| T-Deck → unit A, a direct message | arrives once, in the T-Deck's conversation, with `RECEIVED · … dBm · SNR …`. ON-AIR |
| `sh /root/rollback-rift-improvements/mcd-restart.sh` | the three channels back in slots 0/1/2 with hashes `9a`/`11`/`7b`; RIFT reconnects; threads empty (trap 5); one message each way still works, both kinds |

### 10. The screens — part 5

Captured off unit A's own DRM plane, the way the earlier gates did:
`ffmpeg -f kmsgrab -i - -frames:v 1 -vf hwdownload,format=rgb565le -f rawvideo /tmp/cap.raw`
on the unit, converted on the host — `out/chrome-a89b456/hwgate-unitA/tools/cap.sh`
does both (copy its `c_capture.sh` to `/tmp/chrome-tools/` on the unit first if
the unit has rebooted). Switch orientation with
`ssh $SSHO $A 'doors call shell shell.rotation mode=portrait'` (or `landscape`),
then reopen RIFT (trap 4).

**Portrait**

| Screen | Must be true |
| --- | --- |
| ACTIVITY | the four captions drawn whole — `RADIO SERVICE`, `THIS DEVICE`, `RECENTLY HEARD`, `MESH ACTIVITY` — not cut by their top rule; both ADVERT words inside their buttons; no command line unless the service is down |
| NODES | no command line; no footer unless it says something (cached, forgotten, full); `DIR` for a direct node and `?` where nothing was measured |
| NODES, a list longer than the screen | scroll half way, leave it for two minutes of live traffic: it keeps its place while the mesh re-orders it (two captures). Arrow keys, if the keyboard is attached, keep the selected row in view. |
| a node's DETAIL | the four actions first — `‹ NODES`, `MESSAGE`, `RE-ROUTE`, `FORGET` — each word inside its button; then `LINK STATE`, `PATH`, `PATH CHANGES SEEN BY RIFT`, `IDENTITY`, every caption whole |
| the FORGET confirmation | as in 8b |
| COMMS, a thread | one caption line per message (`4m · DELIVERED · ACK 41 s`), no `you` line above the bodies |

**Landscape**

| Screen | Must be true |
| --- | --- |
| NODES | list beside the pane; the pane's actions under its title without scrolling; the strip's right caption reads `↑↓ SELECT · ENTER MESSAGE · n KNOWN · n FRESH · MAX n HOPS`; no command line |
| NODES, Enter (keyboard attached) | opens the selected node's conversation in COMMS and sends nothing; skip if no keyboard |
| ACTIVITY | two columns; THIS DEVICE heads the right one with both ADVERT buttons in view |
| COMMS with a conversation open | three panes; the command line is the composer; the newest message is above it, not under it |

The product owner confirms the physical rendering — no clipping, no wrong
glyph, no wrong state — and the table above is filled in from the captures,
not from the report. Ignore the status bar's radio chip (see "On the panel,
and not this branch's").

### 11. Health — after each part and at the end

```bash
u <<'EOF'
echo "shell $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"'), $(pidof doors-shell | wc -w) process(es)"
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"(state|radio_state)":"[^"]*"|"lease_held":[a-z]*|"(tx_ok|tx_failed|tx_refused|tx_unknown|tx_done_unmatched|rx_rejected|rx_dropped|packets_free|packets_total|lease_lost|radiod_disconnects)":[0-9]+' | tr '\n' ' '; echo
for s in doors-shell sysd netd radiod; do echo "$s: $(grep -E '^(running|crashloop|restarts)=' /run/pocketos/$s.state 2>/dev/null | tr '\n' ' ')"; done
ls /run/pocketos/*crashloop* 2>/dev/null || echo "no crashloop markers"
echo "crash reports: $(ls /var/lib/pocketos/log/crash-* 2>/dev/null | wc -l); dmesg segfault/oops: $(dmesg | grep -ciE 'segfault|oops|panic')"
grep -hE ' (ERROR|WARN) ' /tmp/meshcored-bench.log /var/lib/pocketos/log/meshcored.log /var/lib/pocketos/log/shell.log 2>/dev/null | tail -8
EOF
```

| Must be true | |
| --- | --- |
| `online`, lease held, `radio_state` `rx` | |
| `tx_failed`, `tx_refused`, `tx_unknown`, `rx_rejected`, `rx_dropped`, `lease_lost` at 0, or each one explained | |
| `packets_free` equals `packets_total` when idle | nothing leaked from the packet pool |
| one `doors-shell`, no crashloop, no crash report, no segfault | |
| no `ERROR` or `WARN` that is not in "On the panel, and not this branch's" | |

### 12. As left, or rolled back

**Leave the unit on `3e89c9c`** unless something failed or the owner wants
otherwise: it is the branch whose check is outstanding. Put the rotation mode
back to what step 1 recorded, remove nothing from
`/var/lib/pocketos/meshcored/`, and fill in "As left" below.

Full rollback, if anything went wrong:

```bash
ssh $SSHO $A 'sh /root/rollback-rift-improvements/RESTORE.sh'
```

The two hashes it prints must match step 1's. Add `--with-state` only to undo
a forget the node's own advert cannot undo; it also discards every node
learned since step 2. The new service writes `state.v1` in the format the old
one reads — no storage format changed.

---

## What counts as PASS

| Part | Needs |
| --- | --- |
| 1 — adverts | one transmit per press; the T-Deck learns unit A from the zero-hop advert; the flood's echo seen and the zero-hop's not (or that row NOT RUN, with the reason) |
| 2 — FORGET | every way out of the confirmation cancels it; forgotten → gone from the list, from `mesh.node` and across a restart; a message to it refused with nothing transmitted; back by its own advert; a direct message ACKed again |
| 3 — RE-ROUTE | route forgotten with nothing transmitted; the next message floods, is ACKed, and the route is learned again |
| 4 — two in flight | the answered message `acked`, the other `no_ack` at its own deadline and not before — or NOT RUN with the reason |
| 5 — screens | the tables in step 10, off the DRM plane, confirmed by the owner |
| 6 — unchanged | step 9: channels and direct messages both ways, and a restart |
| always | step 11 healthy, and the build identity at the top filled in |

A PASS also needs that nothing on the panel claimed more than is known: an
advert is `ACCEPTED`, never sent; a channel message is never `DELIVERED`; a
message is `NO ACK` only after its deadline; and a refusal says `NOT DONE` in
the service's words.

Record which of HOST / DEVICE / LOCAL TX PATH / ON-AIR each observation is.
Only what crossed between two radios is ON-AIR.

## Host only — not reproducible on the bench, and not owed by this gate

| Behaviour | Why not on the bench | Where it is proven |
| --- | --- | --- |
| a ninth message refused (`mesh.send` error 5) while eight wait for ACKs | needs eight unanswered at once | `tests/meshcored_runtime_test.cpp`, `test_ack_deadlines` |
| a send after every deadline has passed is not refused as busy | same | same, in real time |
| an ACK queued behind another frame is matched before its deadline is judged | the order frames arrive in cannot be arranged on air | same, "an ACK that came in time …" |
| `NO ANSWER` when the service goes away mid-request | the window is milliseconds | `tests/rift_model_test.c`, `tests/rift_app_test.c` |
| a FORGET confirmation cancelled by turning the panel | on the device a rotation restarts the shell (trap 4) | `tests/rift_app_test.c` |
| a zero-hop advert not learned by a node two hops away | needs a peer two hops away | not proven anywhere; a later bench with a relay could |

## Results

Not run.

## As left

Not run.

## Provenance

Built **2026-09-22** on the WSL build host. Unit A was not touched.

| | |
| --- | --- |
| Commit | `3e89c9cddb6b894639e9618e1ba59fe8b1d6ac90` (`feat/rift-improvements`) |
| Source | `git archive` of that commit, so nothing uncommitted could reach a binary that goes on a radio; `BUILD_ID` `3e89c9c` written beside `VERSION`, the way `apply_to_sdk.sh` does for a tree with no git history |
| Vendor | `RIFT` `3ca7e3f0`, `Crypto` `37a76b8f` (the pins in `protocols/meshcore`), `lvgl` `59dc7e4` |
| Toolchain | the Buildroot one, `riscv64-unknown-linux-gnu-`, against its own staging sysroot |
| Configuration | `meshcored`: `make ENABLE_MESHCORED=1 meshcored`, `-O2 -Wall -Wextra -Werror`. `doors-shell`: the shell's CMake with the Buildroot toolchain file, `-DCMAKE_BUILD_TYPE=Release -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot` — the flags `platforms/k230/package/pocketos/pocketos.mk` uses |
| Objects before the build | **0** |
| Warnings | **0**, both |
| Stripped | `--remove-section=.comment --remove-section=.note`, after the symbol check below |

| Artefact | sha256 | md5 | Says of itself |
| --- | --- | --- | --- |
| `doors-shell` | `dddc541fd6ff843f6920071ada50e63742f8026960b526ddc3b6885796da4a81` | `6f072f63e2d4a43c03092461073b1946` | `0.0.10`, `3e89c9c` |
| `meshcored` | `7e538a07e7ae64c28c3e81c345f3a25c1621a6bcb2d6ce9b3f30614a7d356fce` | `9b71828fceb54769e8cff1efd87ece23` | `0.0.10`, `3e89c9c` |

Both are `ELF 64-bit LSB pie, UCB RISC-V, RVC, double-float ABI`.

What was checked on them before they left the build host:

| | |
| --- | --- |
| `meshcored` is reproducible | a second build from the same archive is **byte-identical** (stripped and unstripped) |
| `meshcored` carries the new methods | `mesh.node_remove`, `mesh.node_reset_path`, `zero_hop`, `persisted` |
| `meshcored` carries no fake radio and no test hook | 0 matching symbols (`fake`, `test_hook`, `mock_`), checked before stripping |
| `doors-shell` has the new RIFT | `ADVERT NEAR`, `ADVERT MESH`, `ZERO-HOP ADVERT`, `RE-ROUTE`, `NO ANSWER`, `mesh.node_remove`, `mesh.node_reset_path` |
| `doors-shell` transmits from the places the lint allows | `mesh.send` and `mesh.advert` each once in the binary's strings; `tests/rift_lint.sh` holds the one handler that may advert and the one that may forget |
| `doors-shell` has no simulator hook | `POCKETOS_TEST_CHROME` absent |
| `doors-shell` carries chrome stage 1 | `status_bar_height` present, as expected from `a89b456` |
| Libraries they need | `doors-shell`: `libcjson.so.1 libm.so.6 liblvgl.so.9 libpthread.so.0 libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1 libc.so.6`. `meshcored`: `libcjson.so.1 libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6`. Step 1 checks they are on the unit. |

The `doors-shell` build was not repeated to compare hashes; the `radiod` on the
unit is not rebuilt by this gate, so its non-reproducible RadioLib build string
(`RIFT_CHANNELS_GATE.md`, Provenance) does not arise.
