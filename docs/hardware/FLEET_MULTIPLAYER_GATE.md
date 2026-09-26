# The Fleet multiplayer hardware gate (P7): unit A + unit B

**Status: PLAN - NOT RUN.** Nothing in this document has been executed on
either unit. It is the procedure for the product owner to run, prepared at the
end of P0-P6 (ADR-008, docs/apps/FLEET_MULTIPLAYER.md). Every result field
below is empty on purpose; a claim becomes ON-AIR only when it is filled in
from the units.

Two T-Display K230 units, each running `radiod` on its SX1262 and `meshcored`
over it, each with the new `doors-shell`, play PocketFleet against each other
over LoRa:

```
unit A: Fleet ── fleet_link_mesh ──[mesh.app_*]──▶ meshcored ──[radio.*]──▶ radiod ──▶ SX1262
                                                                                          │  869.618 MHz
unit B: Fleet ── fleet_link_mesh ──[mesh.app_*]──▶ meshcored ──[radio.*]──▶ radiod ──▶ SX1262
```

What this gate is for: to turn the host-only evidence of P1-P6 into evidence
about two radios. The protocol, the state machine, the recovery and the UI are
already VERIFIED host (see "What P1-P6 already proved"); what no host test can
show is the real channel - airtime, latency, loss at range, a real MeshCore
dispatcher's timing, a real reboot - and a person playing on real glass.

Scope limits, the same as every bench gate before it:

- VERSION stays 0.0.13. No image is built, no card is flashed, no release or
  package path is used, `deploy.sh` is not used.
- Two files per unit, installed by hand over SSH: `/usr/bin/doors-shell` and
  `/usr/sbin/meshcored`. `radiod` is **not** replaced unless the survey says
  the unit's one predates `radio.send_async` (step 1, stop condition).
- No init script is added and nothing under `/etc` is written. `meshcored` is
  started by hand, as in MESHCORED_HARDWARE_GATE.md and RIFT_IMPROVEMENTS_GATE.md.
- `POCKETFLEET_MP_FAKE` and `POCKETFLEET_SCREEN` must be unset on both units.
  They are development aids; either one set would make the gate prove nothing.

## Traps (read before step 1)

1. **`doors call` turns an all-digit value into a number.** A `payload_hex`
   or key made only of digits reaches the service as a number and is refused
   as "not hex" (`tools/pos/pos_radio.c`, `pos_params_from_kv`). Every hex
   value typed below contains a letter.
2. **Fleet reads the mesh only while Multiplayer is open** (or RESUME MATCH
   was pressed). A packet that arrives while Fleet is closed waits in
   `meshcored`'s inbox - 32 datagrams, this run of the service only - and is
   read when the player opens Multiplayer. That is by design (ADR-008: opening
   Fleet transmits nothing and answers nothing), not a lost packet.
3. **A `meshcored` restart empties its inbox and starts new ids** (`run_id`
   changes). Fleet recovers by its own retries; expect a pause of up to one
   retry interval (7 s, doubling) after one.
4. **The airtime governor paces shots.** After six quick shots a player gets
   one more every 5 s, and at most 90 s of airtime in any hour. A shot held by
   it says "Holding back to spare the airtime. It will go shortly." That is
   correct behaviour, not a stall.
5. **`--name` renames a node.** Unit A is `Mstr_k230` (hash `19`) since the
   RIFT gates; never start it with another `--name`. Unit B's name is chosen
   once, in step 2, only if it has no identity yet.
6. **Adverts are stamped with the wall clock and the board has no RTC.** Set
   the clock (NTP) on both units before step 5, or a peer that holds a newer
   advert from before a reboot will ignore the new one (docs/KNOWN_ISSUES.md).
7. A rotation change restarts the shell. Playing across it is fine (the match
   is on disk), but do not rotate in the middle of a step that measures time.

## What P1-P6 already proved (host)

| Claim | Evidence | Class |
| --- | --- | --- |
| The codec takes exactly the right bytes for every type and refuses everything else | `tests/fleet_proto_test` (a million random packets) | VERIFIED host |
| The state machine's rules, one check each | `tests/fleet_match_test` | VERIFIED host |
| Whole matches over a modelled LoRa channel with loss, duplication, reordering, collisions, outages, crashes, app close, service restarts and reboots; both sides always agree; eight cheating peers caught | `tests/fleet_mp_sim_test`, `make fleet-mp-soak` (12 000 matches) | VERIFIED host |
| The app under a finger, both shapes; every multiplayer screen rendered | `tests/fleet_app_test`, `tests/fleet_shell_test.sh` | VERIFIED host |
| The save before every packet; resume from it | `tests/fleet_session_test`, `tests/fleet_save_test` | VERIFIED host |
| meshcored carries app datagrams: flood, receipt, direct; refuses REQs that are not app datagrams | `tests/meshcored_runtime_test.cpp`, `tests/meshcored_service_test.sh` | VERIFIED host |
| The mesh link: states, players, exactly-once delivery, run changes | `tests/fleet_link_test` | VERIFIED host |
| Whole matches between two real `meshcored` processes over a mock air: real pace, 15 % loss, an app crash at ply 30, a `meshcored` restart at ply 40 | `make ENABLE_MESHCORED=1 fleet-mp-e2e` | VERIFIED host |
| Airtime per device per match: about 34 s in the simulator (about 104 plies), 42 s over two real meshcored processes (133 plies) | computed from frame sizes, not measured on air | computed |

## The payload

Built on the WSL build host from the PR head at the time of the gate, the way
RIFT_IMPROVEMENTS_GATE.md's Provenance describes:

```bash
C=<the commit under test>                  # the PR head; write it here
git -C ~/work/K230 fetch origin claude/fleet-multiplayer-protocol-95evn7
rm -rf ~/work/fleet-gate-$C && mkdir -p ~/work/fleet-gate-$C/src
git -C ~/work/K230 archive $C | tar -C ~/work/fleet-gate-$C/src -xf -
echo ${C:0:7} > ~/work/fleet-gate-$C/src/BUILD_ID
# meshcored: make ENABLE_MESHCORED=1 meshcored, with the Buildroot toolchain,
#            -O2 -Wall -Wextra -Werror, 0 objects before the build.
# doors-shell: the shell's CMake with the Buildroot toolchain file,
#            -DCMAKE_BUILD_TYPE=Release -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot
# Strip both (--remove-section=.comment --remove-section=.note) after the checks below.
OUT=~/work/fleet-gate-out-${C:0:7}; mkdir -p $OUT
# copy doors-shell and meshcored into $OUT; (cd $OUT && sha256sum doors-shell meshcored > SHA256SUMS)
```

Checked on the build host before either file leaves it:

| Check | How | Result |
| --- | --- | --- |
| both are `ELF 64-bit LSB pie, UCB RISC-V` | `file` | |
| `meshcored` carries the app datagram surface | `strings` has `mesh.app_send`, `mesh.app_inbox`, `mesh.app`, `run_id`, `app_receipts` | |
| `doors-shell` carries Fleet multiplayer | `strings` has `DOORS-FLEET-COMMIT-1`, `mesh.app_send`, `MAKE VISIBLE`, `RESUME MATCH` | |
| `doors-shell` transmits only what the lint allows | `mesh.send` appears only from RIFT; `tests/fleet_lint.sh` passes on `$C` | |
| no test hook in either | `fake`, `test_hook`, `mock_` absent before stripping | |
| `meshcored` reproducible | a second build from the same archive is byte-identical | |
| libraries | `readelf -d`: as in RIFT_IMPROVEMENTS_GATE.md (Provenance); step 1 checks them on each unit | |
| the host suites pass on `$C` | `make test`, `make fleet-mp-san-test`, `make ENABLE_MESHCORED=1 meshcored-test fleet-mp-e2e` | |

| Path on each unit | sha256 | Size |
| --- | --- | --- |
| `/usr/bin/doors-shell` | | |
| `/usr/sbin/meshcored` | | |

Optional, for step 11 only: `tests/fleet_mp_player` cross-built the same way
(`make CC=riscv64-unknown-linux-gnu-gcc tests/fleet_mp_player` against the
Buildroot sysroot). It is a test tool; it goes to `/tmp` and nowhere else.

---

# The procedure

Twelve steps. 1-4 need neither player; 5-10 need both units side by side (1-3 m
apart to start) and a person at each, or one person and both panels; 11 is
optional; 12 puts everything back. From the build host:

```bash
A=root@192.168.10.157          # unit A on the bench, as of 2026-09-21 - confirm
B=root@<unit B address>        # unit B - fill in
SSHO="-i $HOME/.ssh/pocketos_bench -o UserKnownHostsFile=$HOME/work/chrome-gate-ssh/known_hosts -o StrictHostKeyChecking=yes -o BatchMode=yes -o ConnectTimeout=8"
OUT=$HOME/work/fleet-gate-out-<short commit>
RB=/root/rollback-fleet-mp
ua() { ssh $SSHO $A sh -s -- "$@"; }
ub() { ssh $SSHO $B sh -s -- "$@"; }
both() { f=$(mktemp); cat > $f; echo "=== unit A"; ua "$@" < $f; echo "=== unit B"; ub "$@" < $f; rm -f $f; }
```

Unit B's host key has to be in the known-hosts file first (confirm its
fingerprint on the unit's console, once).

### 1. Survey both units - the "before" reading and the stop conditions

```bash
both <<'EOF'
echo "== release: $(tr '\n' ' ' < /etc/doors-release)"
echo "== shell: $(doors shell info | tr -d ' \t\n' | grep -oE '"(build|orientation)":"[^"]*"' | tr '\n' ' ')"
P=$(pidof meshcored)
echo "== meshcored pid ${P:-NOT RUNNING}: $(tr '\0' ' ' 2>/dev/null < /proc/${P:-0}/cmdline)"
echo "== mesh.identity: $(doors call meshcored mesh.identity 2>&1 | tr -d ' \t\n')"
echo "== mesh.status: $(doors call meshcored mesh.status 2>&1 | tr -d ' \t\n' | grep -oE '"(state|radio_state)":"[^"]*"|"lease_held":[a-z]*|"nodes":[0-9]+' | tr '\n' ' ')"
echo "== radio.lease: $(doors call radiod radio.lease 2>&1 | tr -d ' \t\n')"
echo "== radio.stats: $(doors call radiod radio.stats 2>&1 | tr -d ' \t\n')"
echo "== radiod: $(cat /etc/default/radiod 2>/dev/null || echo 'NO /etc/default/radiod (mock backend)')"
echo "== shell env: $(grep -v '^#' /etc/default/doors-shell 2>/dev/null | tr '\n' ' ')"
echo "== fake aids: $(tr '\0' '\n' < /proc/$(pidof doors-shell | cut -d' ' -f1)/environ 2>/dev/null | grep -E 'POCKETFLEET_(MP_FAKE|SCREEN)' || echo none)"
ls -l /var/lib/pocketos/meshcored/ /var/lib/pocketos/fleet/ 2>&1
sha256sum /usr/bin/doors-shell /usr/sbin/meshcored /usr/sbin/radiod
for l in liblvgl.so.9 libcjson.so.1 libgpiod.so.3 libdrm.so.2 libevdev.so.2 libatomic.so.1 libstdc++.so.6 libgcc_s.so.1; do
    f=$(ls /usr/lib/$l /lib/$l 2>/dev/null | head -1); echo "lib $l: ${f:-MISSING}"
done
date -u; ls /run/pocketos/*crashloop* 2>/dev/null || echo "no crashloop markers"
EOF
```

Keep the output. **Stop** on either unit if:

- `/etc/default/radiod` does not say `RADIOD_BACKEND=sx1262`;
- the owner has not confirmed an 868 MHz antenna on MMCX1 (BRINGUP_CHECKLIST.md section 5);
- `radio.lease` answers `unknown method` - that radiod predates the
  asynchronous API, and needs a `radiod` of at least `e241805` deployed first
  (MESHCORED_HARDWARE_GATE.md, "Why radiod was deployed as well"); write it
  down, and add it to the payload before going on;
- any library is `MISSING`;
- `fake aids` shows anything but `none`;
- the date is 1970 (trap 6): set the clock first.

Unit B specifically: record whether `/var/lib/pocketos/meshcored/identity.id`
exists. If it does, B keeps that identity and its name. If it does not, B gets
one in step 2, named `K230-B`.

| Result | Unit A | Unit B |
| --- | --- | --- |
| release, shell build, radiod build | | |
| identity (name, hash, first 8 bytes) | `Mstr_k230`, `19`, `19f7b327` expected | |
| `radiod` backend, antenna confirmed | | |
| libraries | | |
| `tx_packets`, `tx_airtime_ms` before | | |

### 2. Rollback copies and helpers, on both

```bash
both <<'EOF'
RB=/root/rollback-fleet-mp; D=/var/lib/pocketos/meshcored
mkdir -p $RB
[ -f $RB/doors-shell ] || cp -p /usr/bin/doors-shell $RB/doors-shell
[ -f $RB/meshcored ] || cp -p /usr/sbin/meshcored $RB/meshcored
for f in state.v1 channels.v1; do [ -f $RB/$f ] || [ ! -f $D/$f ] || cp -p $D/$f $RB/$f; done
[ -f $RB/match.v1 ] || [ ! -f /var/lib/pocketos/fleet/match.v1 ] || cp -p /var/lib/pocketos/fleet/match.v1 $RB/match.v1
if [ ! -f $RB/meshcored.argv ]; then
    P=$(pidof meshcored)
    if [ -n "$P" ]; then tr '\0' ' ' < /proc/$P/cmdline > $RB/meshcored.argv
    else
        N=; [ -f $D/identity.id ] || N="--name K230-B"
        echo "meshcored $N --frequency-mhz 869.618 --bandwidth-khz 62.5 --spreading-factor 8 --coding-rate 5 --sync-word 0x12 --preamble 32 --tx-power-dbm 2 --verbose" > $RB/meshcored.argv
    fi
fi
cat > $RB/mcd-restart.sh <<'X'
#!/bin/sh
# Restart meshcored with the argv this unit ran with before the gate.
RB=/root/rollback-fleet-mp
ARGV=$(cat $RB/meshcored.argv)
killall meshcored 2>/dev/null
n=0; while pidof meshcored >/dev/null && [ $n -lt 10 ]; do sleep 1; n=$((n + 1)); done
nohup $ARGV >>/tmp/meshcored-bench.log 2>&1 </dev/null &
n=0; S=
while [ $n -lt 15 ]; do
    S=$(doors call meshcored mesh.status 2>/dev/null | tr -d ' \t\n')
    case "$S" in *'"state":"online"'*) break ;; esac
    sleep 1; n=$((n + 1))
done
echo "meshcored pid $(pidof meshcored): $(echo "$S" | grep -oE '"state":"[^"]*"|"lease_held":[a-z]*|"run_id":"[^"]*"|"nodes":[0-9]+' | tr '\n' ' ')"
echo "build: $(doors call meshcored mesh.info 2>/dev/null | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
exit 0
X
cat > $RB/RESTORE.sh <<'X'
#!/bin/sh
# Put back what this gate replaced. match.v1 is restored only with
# --with-match: a finished gate match is harmless, and restoring the file
# would bring back whatever match was there before.
RB=/root/rollback-fleet-mp
/etc/init.d/S90doors-shell stop; sleep 1
killall meshcored 2>/dev/null
n=0; while pidof meshcored >/dev/null && [ $n -lt 10 ]; do sleep 1; n=$((n + 1)); done
cp -p $RB/doors-shell /usr/bin/doors-shell
cp -p $RB/meshcored /usr/sbin/meshcored
if [ "$1" = --with-match ]; then
    if [ -f $RB/match.v1 ]; then cp -p $RB/match.v1 /var/lib/pocketos/fleet/match.v1
    else rm -f /var/lib/pocketos/fleet/match.v1; fi
fi
sync
sh $RB/mcd-restart.sh
/etc/init.d/S90doors-shell start; sleep 3
sha256sum /usr/bin/doors-shell /usr/sbin/meshcored
X
cat $RB/meshcored.argv; echo; sha256sum $RB/*
EOF
```

Unit A's argv must not carry a `--name` other than `Mstr_k230`. The binary
hashes must match step 1's.

### 3. Install, and the build identity, on both

```bash
(cd "$OUT" && sha256sum -c SHA256SUMS) || exit 1
for U in $A $B; do
  ssh $SSHO $U '/etc/init.d/S90doors-shell stop; killall meshcored; sleep 2' &&
  tar -C "$OUT" --owner=0 --group=0 --numeric-owner -cf - doors-shell meshcored |
    ssh $SSHO $U 'tar -C /tmp -xf - && install -m 0755 -o 0 -g 0 /tmp/doors-shell /usr/bin/doors-shell && install -m 0755 -o 0 -g 0 /tmp/meshcored /usr/sbin/meshcored && rm -f /tmp/doors-shell /tmp/meshcored && sync && sha256sum /usr/bin/doors-shell /usr/sbin/meshcored' &&
  ssh $SSHO $U 'sh /root/rollback-fleet-mp/mcd-restart.sh; /etc/init.d/S90doors-shell start'
done
both <<'EOF'
sleep 3
echo "shell:     $(doors shell info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
echo "meshcored: $(doors call meshcored mesh.info | tr -d ' \t\n' | grep -oE '"build":"[^"]*"')"
echo "identity:  $(doors call meshcored mesh.identity | tr -d ' \t\n' | grep -oE '"(public_key|name|node_hash)":"[^"]*"' | tr '\n' ' ')"
echo "status:    $(doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"(state|run_id)":"[^"]*"|"lease_held":[a-z]*|"app_(rx|tx|receipts)":[0-9]+' | tr '\n' ' ')"
echo "shells:    $(pidof doors-shell | wc -w)"
EOF
```

| Must be true | Unit A | Unit B |
| --- | --- | --- |
| installed hashes match the payload table | | |
| shell and meshcored `build` = the short commit | | |
| `mesh.status` `online`, lease held, a 16-hex `run_id`, `app_rx`/`app_tx`/`app_receipts` present and 0 | | |
| identity: A unchanged (`19f7b327...`); B as recorded in step 1, or new `K230-B` | | |
| exactly one `doors-shell` | | |

Write both full public keys here; later steps name them `$KA` and `$KB`:

```bash
KA=<unit A public key>   # 64 hex
KB=<unit B public key>   # 64 hex
```

### 4. Nothing is transmitted until a player asks - on both

For each unit, read `radio.stats` `tx_packets`, then on the panel: open Fleet,
play two single-player shots, go back to Command, open **MULTIPLAYER** and
leave the lobby open for 60 s, then close Fleet. Read `tx_packets` again.

```bash
both <<'EOF'
doors call radiod radio.stats | tr -d ' \t\n' | grep -oE '"tx_packets":[0-9]+'
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"app_(rx|tx)":[0-9]+' | tr '\n' ' '
EOF
```

| Must be true | Unit A | Unit B |
| --- | --- | --- |
| `tx_packets` unchanged across all of it | | |
| `app_tx` unchanged (0) | | |
| the lobby lists whom the unit has heard, or says it has heard nobody | | |

This is ADR-008 decision 5 on hardware. If either unit transmitted, **stop**:
that is a defect, and nothing after this step is evidence.

### 5. Finding each other

Both open **MULTIPLAYER**. If a unit does not list the other, press **MAKE
VISIBLE** on the one that is missing (a zero-hop advert: heard in range,
repeated by nobody). Wait up to 30 s.

| Must be true | Unit A | Unit B |
| --- | --- | --- |
| the other unit is listed, by name, with `DIRECT` or its hops, and "heard N min ago" | | |
| `tx_packets` rose by exactly the adverts pressed, and nothing else | | |
| no chat node other than a player was offered (a repeater never is) | | |

### 6. Invite, accept, deploy

On A: select B, press **INVITE**. On B: the invitation appears; press
**ACCEPT**. Both go to Deploy; each places a fleet by hand (or AUTO) and
confirms.

```bash
ua <<EOF
doors call meshcored mesh.node node=$KB | tr -d ' \t\n' | grep -oE '"(path_known|direct)":[a-z]*|"hops":[0-9]+'
EOF
both <<'EOF'
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"app_(rx|tx|receipts)":[0-9]+' | tr '\n' ' '
EOF
```

(The first heredoc is unquoted on purpose, so `$KB` is expanded on the host.)

| Record | Value |
| --- | --- |
| INVITE pressed to "invites you" on B, seconds | |
| ACCEPT pressed to Deploy on A, seconds | |
| B answered the first flood with a receipt: B's `app_receipts` >= 1 | |
| A learned a direct route to B: `path_known true`, `hops 0` | |
| both reached Battle; B (the guest) fires first | |
| "Waiting for ... to deploy" shown on whichever confirmed first | |

### 7. Battle - play the match

Play to the end, by hand. Each player fires on their own turn. Watch for, and
note the ply of, each of these if it happens:

| Observation | Seen? | Ply / notes |
| --- | --- | --- |
| "Shot at XY sent. Waiting for the report.", then the result on the board | | |
| "Link problem: no report on XY yet. Retrying (n of 6)." | | |
| "Holding back to spare the airtime" (trap 4) | | |
| "... is out of reach. The match is paused" | | |
| the last shot each way shown in the header line | | |
| any screen that scrolls during a turn (it must not) | | |

Measure five shots on each side with a stopwatch: FIRE pressed to the result
shown on the shooter's panel.

| Shot | A fires: seconds | B fires: seconds |
| --- | --- | --- |
| 1-5 | | |
| median | | |

### 8. Interruptions, in the middle of the match

Do these during step 7, in this order, each at a different ply. After each,
the match must carry on to its end with no action but the ones named.

**8a. Fleet closed and reopened.** On B, during A's turn: close Fleet (home),
wait 30 s, reopen Fleet, press **RESUME MATCH**.

**8b. Service restart.** On A, during B's turn:

```bash
ua <<'EOF'
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"run_id":"[^"]*"'
sh /root/rollback-fleet-mp/mcd-restart.sh
EOF
```

The `run_id` printed by the restart must differ from the one before. B's next
shot must arrive (up to one retry interval, trap 3).

**8c. Out of range.** Power B down (or take it out of range) during A's turn
for 3 minutes. A must say B is out of reach and that the match is paused - not
lost. Bring B back, open Fleet, **RESUME MATCH**. (On power-up, start
`meshcored` by hand with `sh /root/rollback-fleet-mp/mcd-restart.sh`: no init
script starts it, by the gate's own scope.)

**8d. Reboot.** Reboot A in the middle of its own turn, having pressed FIRE
(the shot is saved before it is sent). After boot: `mcd-restart.sh`, open
Fleet, **RESUME MATCH**. The shot must not be lost and must not be fired twice.

| Must be true | 8a | 8b | 8c | 8d |
| --- | --- | --- | --- | --- |
| the match continued to its end | | | | |
| no ply fired twice, none lost (the board after matches the board before) | | | | |
| seconds from resuming to the next result | | | | |

### 9. The result, and the fleets verified

At the end both panels show Result.

| Must be true | Unit A | Unit B |
| --- | --- | --- |
| one "Enemy fleet destroyed", the other "Fleet lost" | | |
| "Verified" (the other's fleet matched its commitment and every answer) | | |
| "Their fleet" shows the opponent's real placement | | |
| the same number of shots each way on both | | |

```bash
both <<'EOF'
doors call meshcored mesh.status | tr -d ' \t\n' | grep -oE '"app_(rx|tx|receipts)":[0-9]+|"(tx_failed|tx_unknown|tx_refused|rx_rejected|rx_dropped)":[0-9]+' | tr '\n' ' '; echo
doors call radiod radio.stats | tr -d ' \t\n' | grep -oE '"(tx_packets|tx_airtime_ms|tx_airtime_last_hour_ms|duty_cycle_last_hour_percent)":[0-9.]+' | tr '\n' ' '; echo
doors call meshcored mesh.messages | tr -d ' \t\n' | grep -oE '"count":[0-9]+'
ls -l /var/lib/pocketos/fleet/
EOF
```

| Record | Unit A | Unit B |
| --- | --- | --- |
| plies | | |
| `tx_packets` during the match (minus step 4-5) | | |
| `tx_airtime_ms` during the match; compare with the computed 34-42 s | | |
| duty cycle, last hour | | |
| `app_tx` / `app_rx` / `app_receipts` | | |
| `tx_failed`, `tx_unknown`, `tx_refused`, `rx_rejected`, `rx_dropped` | | |
| `mesh.messages` count unchanged: Fleet sent no chat message | | |
| `match.v1` present, 655 bytes | | |

### 10. A MeshCore node that is not Doors (optional, needs the T-Deck)

ADR-008 carries one ASSUMED claim: a MeshCore node that is not Doors ignores a
Doors app datagram. With the T-Deck (RIFT firmware) in range and known to A
(advert it from the T-Deck if needed):

```bash
TDECK=<the T-Deck's public key>
ua <<EOF
doors call meshcored mesh.app_send to=$TDECK port=1 payload_hex=4a0b0c0d0e
sleep 10
doors call meshcored mesh.node node=$TDECK | tr -d ' \t\n' | grep -oE '"(name|path_known)":[^,]*'
EOF
```

| Must be true | Result |
| --- | --- |
| `accepted: true`, and the frame went out (`tx_packets` +1 on A) | |
| the T-Deck shows nothing, does not crash or reboot, and still exchanges a direct message with A afterwards | |
| A receives nothing back from it for this (`app_rx` unchanged) | |

That moves "T-Deck ignores an unknown REQ type" from ASSUMED to ON-AIR.

### 11. An unattended match at real pace (optional)

Only with `fleet_mp_player` built for the target. Stop the shell on both
units first, so only one Fleet reads port 1 on each:

```bash
for U in $A $B; do ssh $SSHO $U '/etc/init.d/S90doors-shell stop'; done
# copy fleet_mp_player to /tmp on both, then:
ub <<'EOF'
/tmp/fleet_mp_player --service meshcored --role guest --save /tmp/e2e-b.v1 --seed 22 > /tmp/e2e-b.log 2>&1 &
EOF
ua <<'EOF'
/tmp/fleet_mp_player --service meshcored --role host --peer <B's mesh name> --save /tmp/e2e-a.v1 --seed 11 > /tmp/e2e-a.log 2>&1 &
EOF
# wait for a DONE line in both logs (about ten minutes on the host model), then:
both <<'EOF'
grep -E '^(DONE|TIMEOUT)' /tmp/e2e-*.log; killall fleet_mp_player; rm -f /tmp/fleet_mp_player /tmp/e2e-*
EOF
for U in $A $B; do ssh $SSHO $U '/etc/init.d/S90doors-shell start'; done
```

Without `--fast`: the product's own pacing. Its saves go to `/tmp`, not to
`/var/lib/pocketos/fleet`, so the player's own `match.v1` is untouched.

| Must be true | Result |
| --- | --- |
| both `DONE`, one `outcome=win` and one `outcome=loss`, `verify=1` on both | |
| the same `plies` and `digest` on both | |
| `tx_airtime_ms` per device, and the match's duration | |

### 12. Put it back

Either keep the gate build on both units (the owner's call) or:

```bash
for U in $A $B; do ssh $SSHO $U 'sh /root/rollback-fleet-mp/RESTORE.sh'; done
```

`--with-match` also restores the `match.v1` that was there before the gate (or
removes the gate's). Then on both: the hashes are the step 1 ones, `mesh.status`
online, one `doors-shell`, no crashloop marker.

```bash
both <<'EOF'
for s in doors-shell sysd netd radiod; do echo "$s: $(grep -E '^(running|crashloop|restarts)=' /run/pocketos/$s.state 2>/dev/null | tr '\n' ' ')"; done
grep -cE 'WARN|ERROR' /var/lib/pocketos/log/meshcored.log /tmp/meshcored-bench.log 2>/dev/null
dmesg | grep -iE 'segfault|oom' | tail -3
EOF
```

## What the gate turns into evidence

| Claim | Now | After a PASS |
| --- | --- | --- |
| Two Doors units play a whole Fleet match over LoRa | VERIFIED host (mock air) | **ON-AIR** |
| Opening Fleet or the lobby transmits nothing | VERIFIED host | **DEVICE** (step 4) |
| The first datagram floods; the receipt teaches a direct route | VERIFIED host | **ON-AIR** (step 6) |
| Fleet closed, service restarted, out of range, rebooted: the match resumes, nothing fired twice | VERIFIED host | **ON-AIR** (step 8) |
| Both fleets verified against their commitments on real units | VERIFIED host | **DEVICE** (step 9) |
| Airtime per device per match | computed (34 s simulated, 42 s end to end on a mock air) | **measured** (step 9, 11) |
| Shot-to-result latency | not known | **measured** (step 7) |
| A non-Doors MeshCore node ignores a Doors app datagram | ASSUMED | **ON-AIR** (step 10) |
| Behaviour at range, through a repeater, on a busy channel | UNRESOLVED | still UNRESOLVED unless a repeater run is added |

## Results

Not run. To be filled in from the units, with the date, the commit, both
units' builds and identities, and every table above.
