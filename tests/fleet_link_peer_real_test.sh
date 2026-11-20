#!/bin/bash
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
# PocketFleet's LAN link against the real peerd: two peerd processes on two
# loopback addresses, and beside each one a tests/fleet_link_peer_real, which
# is the link driven as a session drives it.
#
# tests/fleet_link_peer_test holds the link to a scripted peerd; this holds
# the script to the real one. It shows that the two links find each other by
# address, that datagrams go both ways through peerd's sealing, that each
# peerd ends up keeping exactly the other as the opponent, and that closing
# the links ends visibility while the kept peer stays.
#
# The peerds are out/peerd/peerd-testhooks (bound to one address by
# PEERD_TEST_BIND; see tests/peerd_test.sh). Not part of make test: it needs
# libsodium. Run by `make ENABLE_PEERD=1 SODIUM_DIR=... fleet-link-peer-real`.
set -u
cd "$(dirname "$0")/.." || exit 1
PEERD=${PEERD:-out/peerd/peerd-testhooks}
PLAYER=tests/fleet_link_peer_real
[ -x "$PEERD" ] && [ -x "$PLAYER" ] || { echo "FAIL not built (make fleet-link-peer-real)"; exit 1; }
ROOT=$(mktemp -d /tmp/fleet_link_peer_real.XXXXXX)
PORT=47933
failed=0
check() { if [ "$2" = 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

envs() { # <tag> <address>
    for k in run state log config; do mkdir -p "$ROOT/$1/$k"; done
    echo "POCKETOS_RUNTIME_DIR=$ROOT/$1/run POCKETOS_STATE_DIR=$ROOT/$1/state \
POCKETOS_LOG_DIR=$ROOT/$1/log POCKETOS_CONFIG_DIR=$ROOT/$1/config POCKETOS_LOG_STDERR=0 PEERD_TEST_BIND=$2"
}
EA=$(envs a 127.0.0.1)
EB=$(envs b 127.0.0.2)
env $EA "$PEERD" --port $PORT --name ALPHA >/dev/null 2>&1 &
PA=$!
env $EB "$PEERD" --port $PORT --name BRAVO >/dev/null 2>&1 &
PB=$!
trap 'kill $PA $PB 2>/dev/null; rm -rf "$ROOT"' EXIT
for i in $(seq 100); do
    [ -S "$ROOT/a/run/peerd.sock" ] && [ -S "$ROOT/b/run/peerd.sock" ] && break
    sleep 0.05
done
check "two peerd processes are up" "$([ -S "$ROOT/a/run/peerd.sock" ] && [ -S "$ROOT/b/run/peerd.sock" ] && echo 1)"

# B waits to be asked; A asks B's address.
env $EB "$PLAYER" bb > "$ROOT/b.txt" 2>&1 &
LB=$!
env $EA "$PLAYER" aa 127.0.0.2 > "$ROOT/a.txt" 2>&1
RA=$?
wait $LB
RB=$?
cat "$ROOT/a.txt" "$ROOT/b.txt"
check "the link that asked an address found, kept and heard the other" "$([ $RA = 0 ] && echo 1)"
check "the link that was asked found, kept and heard the first" "$([ $RB = 0 ] && echo 1)"
check "both played on the LAN profile with peerd's estimate as the retry base" \
      "$([ "$(grep -c 'base=500 profile=lan' "$ROOT/a.txt" "$ROOT/b.txt" | grep -c ':1')" = 2 ] && echo 1)"

ask() { # <tag> <method>
    python3 - "$ROOT/$1/run/peerd.sock" "$2" <<'PY'
import json, socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5); s.connect(sys.argv[1])
b = json.dumps({"id": 1, "method": sys.argv[2]}).encode(); s.sendall(struct.pack(">I", len(b)) + b)
def read(n):
    d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: raise OSError("closed")
        d += c
    return d
r = json.loads(read(struct.unpack(">I", read(4))[0])).get("result", {})
print("visible=%s kept=%s socket=%s" % (r.get("visible"), r.get("count"), r.get("socket")))
PY
}
SA=$(ask a peer.status); KA=$(ask a peer.kept); SB=$(ask b peer.status); KB=$(ask b peer.kept)
echo "a: $SA / $KA"; echo "b: $SB / $KB"
check "the links closed, neither peerd is visible any more" \
      "$(case "$SA$SB" in *visible=True*) ;; *visible=False*visible=False*) echo 1 ;; esac)"
check "each still keeps exactly its opponent" \
      "$(case "$KA|$KB" in *kept=1*"|"*kept=1*) echo 1 ;; esac)"
check "and stays bound for them" \
      "$(case "$SA|$SB" in *socket=True*"|"*socket=True*) echo 1 ;; esac)"
echo "fleet_link_peer_real_test: $failed failure(s)"
exit $((failed > 0))
