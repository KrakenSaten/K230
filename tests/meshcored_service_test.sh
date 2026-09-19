#!/bin/bash
# meshcored against the real radiod.
#
# This suite exists to check the relationship, not the protocol. Everything
# here goes through the radiod that ships - its lease, its region guard, its
# asynchronous transmit, its events - so that what meshcored is held to is
# radiod's actual behaviour rather than this project's reading of its
# documentation. The protocol itself is covered where it belongs: two real
# MeshCore runtimes talking to each other in tests/meshcored_runtime_test.cpp,
# and two whole meshcored processes over a mock air in
# tests/meshcored_harness_test.sh.
#
# One frame here is a genuine MeshCore advert, built by tools/meshcore-frame -
# the same tool whose frames an independent RIFT peer accepted in the accepted
# P0 on-air gate (docs/hardware/MESHCORE_INTEROP_GATE.md). Injecting it into
# radiod's mock backend is as close to that gate as a host test can get: the
# bytes are real, the signature is real, and meshcored has to verify it with
# the real Ed25519 before it will call the sender a node.
#
# No hardware is touched: radiod runs on its mock backend throughout, and
# nothing here transmits on a radio.
#
# Run from the repository root after `make ENABLE_MESHCORED=1 meshcored` and
# `make meshcore-frame`.
set -u

RADIOD=${RADIOD:-services/radiod/radiod}
MESHCORED=${MESHCORED:-services/meshcored/meshcored}
FRAME=${FRAME:-tools/meshcore-frame/meshcore-frame}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_STATE_DIR
TMP=$(mktemp -d)
POCKETOS_RUNTIME_DIR="$TMP/run"
POCKETOS_LOG_DIR="$TMP/log"
POCKETOS_STATE_DIR="$TMP/state"
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_STATE_DIR"
RSOCK="$POCKETOS_RUNTIME_DIR/radiod.sock"
MSOCK="$POCKETOS_RUNTIME_DIR/meshcored.sock"
failed=0
RADIOD_PID=""
MCD_PID=""

cleanup() {
    [ -n "$MCD_PID" ] && kill "$MCD_PID" 2>/dev/null
    [ -n "$RADIOD_PID" ] && kill "$RADIOD_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() { # <label> <0|1>
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}

for bin in "$RADIOD" "$MESHCORED" "$FRAME"; do
    if [ ! -x "$bin" ]; then
        echo "FAIL $bin is not built; run 'make ENABLE_MESHCORED=1 meshcored meshcore-frame'"
        exit 1
    fi
done

start_radiod() {
    "$RADIOD" --backend mock --region EU868 > "$TMP/radiod.log" 2>&1 &
    RADIOD_PID=$!
    local i
    for i in $(seq 1 50); do [ -S "$RSOCK" ] && return 0; sleep 0.1; done
    echo "FAIL radiod did not start"; cat "$TMP/radiod.log"; exit 1
}

stop_radiod() {
    [ -n "$RADIOD_PID" ] || return 0
    kill "$RADIOD_PID" 2>/dev/null
    wait "$RADIOD_PID" 2>/dev/null
    RADIOD_PID=""
    rm -f "$RSOCK"
}

start_meshcored() {
    "$MESHCORED" --verbose "$@" >> "$TMP/meshcored.log" 2>&1 &
    MCD_PID=$!
    local i
    for i in $(seq 1 50); do [ -S "$MSOCK" ] && return 0; sleep 0.1; done
    echo "FAIL meshcored did not start"; cat "$TMP/meshcored.log"; exit 1
}

stop_meshcored() {
    [ -n "$MCD_PID" ] || return 0
    kill "$MCD_PID" 2>/dev/null
    wait "$MCD_PID" 2>/dev/null
    MCD_PID=""
    rm -f "$MSOCK"
}

# ---- a genuine MeshCore advert, from the tool the on-air gate used --------
#
# A disposable identity in a scratch directory, exactly as that gate did. The
# key is spent the moment this test ends.
"$FRAME" identity new "$TMP/peer.id" > "$TMP/peer.txt" 2>&1
check "a disposable peer identity is created" "$([ -s "$TMP/peer.id" ] && echo 1 || echo 0)"
PEER_KEY=$(awk '/^public_key:/ {print $2}' "$TMP/peer.txt")
check "its public key is printed" "$([ ${#PEER_KEY} -eq 64 ] && echo 1 || echo 0)"
"$FRAME" advert --key "$TMP/peer.id" --name TEST-PEER --type chat > "$TMP/advert.txt" 2>&1
ADVERT_HEX=$(awk '/^frame_hex:/ {print $2}' "$TMP/advert.txt")

# One more advert, from a second identity, with a name no well-behaved node
# would choose: a terminal escape sequence. It is a real signed MeshCore
# advert - the name is remote input that has already passed every check
# MeshCore itself makes - so what happens to it at the mesh.* boundary is the
# only thing standing between a hostile node and a client's screen.
"$FRAME" identity new "$TMP/nasty.id" > "$TMP/nasty.txt" 2>&1
NASTY_KEY=$(awk '/^public_key:/ {print $2}' "$TMP/nasty.txt")
"$FRAME" advert --key "$TMP/nasty.id" --name "$(printf 'A\033[2JB')" --type chat \
    > "$TMP/esc.txt" 2>&1
ESC_HEX=$(awk '/^frame_hex:/ {print $2}' "$TMP/esc.txt")
check "an advert with an escape sequence in its name is built" \
    "$([ -n "$ESC_HEX" ] && echo 1 || echo 0)"
# A name that is not UTF-8 at all cannot be built here: meshcore-frame checks
# its --name and refuses, which is correct for a tool that will not produce a
# frame a MeshCore node would truncate. The byte-level case is covered where
# it is reachable - a node that does not use this tool, in
# tests/meshcored_runtime_test.cpp - and the sanitiser itself is covered
# exhaustively in tests/meshcored_util_test.c. What this section proves is the
# wiring: that the daemon really does put remote names through it.
check "a name that is not UTF-8 is refused by the frame tool, as it should be" \
    "$("$FRAME" advert --key "$TMP/nasty.id" --name "$(printf 'C\377D')" --type chat 2>&1 |
        grep -q 'not valid UTF-8' && echo 1 || echo 0)"
check "a signed MeshCore advert is built" "$([ -n "$ADVERT_HEX" ] && echo 1 || echo 0)"

# ---- the driver -----------------------------------------------------------
#
# Same shape as tests/radiod_async_test.sh: the python driver prints its own
# ok/FAIL lines and a sentinel, and this harness refuses to call a driver that
# died half way a pass.
run_driver() { # run_driver <script> [args...]
    local script="$1"; shift
    local out="$TMP/driver.out"
    local rc

    python3 "$script" "$@" > "$out" 2>"$TMP/driver.err"
    rc=$?
    cat "$out"
    local printed
    printed=$(grep -c '^FAIL' "$out" || true)
    failed=$((failed + printed))
    if ! grep -q '^driver: [0-9]* failure' "$out"; then
        echo "FAIL the driver exited $rc without reaching its last line; the checks after that point did not run"
        sed 's/^/     /' "$TMP/driver.err" | head -20
        failed=$((failed + 1))
    elif [ "$rc" -ne 0 ] && [ "$printed" -eq 0 ]; then
        echo "FAIL the driver ran to the end but exited $rc without printing a failure"
        failed=$((failed + 1))
    fi
}

cat > "$TMP/lib.py" <<'PYEOF'
import json, socket, struct, sys, time

fails = 0

def ok(label, cond, detail=""):
    global fails
    if cond:
        print("ok   " + label)
    else:
        fails += 1
        print("FAIL " + label + ((": " + str(detail)) if detail else ""))

class Conn:
    """One connection. Events that arrive while waiting for a reply are kept,
    not discarded: half of what these services do is send events."""
    def __init__(self, path, timeout=15):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""
        self.events = []
        self.next_id = 1

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _frame(self, timeout=None):
        if timeout is not None:
            self.s.settimeout(max(0.01, timeout))
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    msg = json.loads(self.buf[4:4 + n])
                    self.buf = self.buf[4 + n:]
                    return msg
            chunk = self.s.recv(65536)
            if not chunk:
                return None
            self.buf += chunk

    def call(self, method, params=None, timeout=15):
        rid = self.next_id
        self.next_id += 1
        body = {"id": rid, "method": method}
        if params is not None:
            body["params"] = params
        raw = json.dumps(body).encode()
        self.s.sendall(struct.pack(">I", len(raw)) + raw)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self._frame(timeout=deadline - time.monotonic())
            if msg is None:
                raise IOError("connection closed waiting for " + method)
            if "event" in msg:
                self.events.append(msg)
                continue
            if msg.get("id") == rid:
                return msg
        raise IOError("timed out waiting for " + method)

    def result(self, method, params=None, timeout=15):
        r = self.call(method, params, timeout)
        if "result" not in r:
            raise IOError(method + " failed: " + json.dumps(r.get("error")))
        return r["result"]

    def error(self, method, params=None):
        r = self.call(method, params)
        return r.get("error")

    def drain(self, seconds=0.5):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                msg = self._frame(timeout=max(0.01, end - time.monotonic()))
            except socket.timeout:
                break
            if msg is None:
                break
            if "event" in msg:
                self.events.append(msg)

    def wait_event(self, name, seconds=10, match=None):
        end = time.monotonic() + seconds
        while True:
            for e in list(self.events):
                if e.get("event") == name and (match is None or match(e.get("data", {}))):
                    self.events.remove(e)
                    return e
            if time.monotonic() >= end:
                return None
            try:
                msg = self._frame(timeout=max(0.01, end - time.monotonic()))
            except socket.timeout:
                continue
            if msg is None:
                return None
            if "event" in msg:
                self.events.append(msg)

def wait_state(conn, wanted, seconds=20):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            last = conn.result("mesh.status")["state"]
        except (IOError, OSError):
            time.sleep(0.1)
            continue
        if last == wanted:
            return True, last
        time.sleep(0.1)
    return False, last

def done():
    print("driver: %d failure(s)" % fails)
    sys.exit(1 if fails else 0)
PYEOF

# ---------------------------------------------------------------------------
# 1. meshcored started before radiod exists
# ---------------------------------------------------------------------------
echo "--- meshcored with no radiod"
start_meshcored --radiod-socket radiod

cat > "$TMP/t1.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

m = Conn(sys.argv[2])
st = m.result("mesh.status")
ok("a service started before radiod is waiting for it",
   st["state"] == "waiting_for_radiod", st["state"])
ok("it is not connected", st["radio"]["connected"] is False)
ok("it holds no lease", st["radio"]["lease_held"] is False)
ok("and it says nothing about a radio state it has not been told",
   "radio_state" not in st["radio"])
ok("and no profile it has not applied", "profile" not in st["radio"])

ident = m.result("mesh.identity")
ok("it has an identity even with no radio", len(ident["public_key"]) == 64)
ok("whose node hash is the key's first byte",
   ident["node_hash"] == ident["public_key"][:2], ident["node_hash"])
ok("the private key is not in the identity result",
   "private" not in json.dumps(ident).lower() and "prv" not in json.dumps(ident).lower())

ok("mesh.info names the service", m.result("mesh.info")["service"] == "meshcored")
ok("and the protocol revision it was built from",
   len(m.result("mesh.info")["source"]["rift_commit"]) == 40)

e = m.error("mesh.send", {"to": "aabb", "text": "hello"})
ok("a send with no radio is refused as busy", e["code"] == 5, e)
e = m.error("mesh.advert")
ok("and so is an advert", e["code"] == 5, e)

ok("no client is required for any of this", True)
done()
PYEOF
run_driver "$TMP/t1.py" "$TMP" "$MSOCK"

# ---------------------------------------------------------------------------
# 2. radiod appears later: acquire, configure, subscribe, online
# ---------------------------------------------------------------------------
echo "--- radiod appears"
start_radiod

cat > "$TMP/t2.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

m = Conn(sys.argv[2])
r = Conn(sys.argv[3])
m.result("mesh.subscribe")

reached, last = wait_state(m, "online")
ok("meshcored reaches online once radiod exists", reached, last)

st = m.result("mesh.status")
ok("it is connected", st["radio"]["connected"] is True)
ok("it holds the lease", st["radio"]["lease_held"] is True)
ok("the runtime is online", st["radio"]["online"] is True)
ok("radiod reports the radio receiving", st["radio"]["radio_state"] == "rx",
   st["radio"].get("radio_state"))
ok("one connect is counted", st["counters"]["radiod_connects"] == 1)
ok("one lease acquisition is counted", st["counters"]["lease_acquired"] == 1)

p = st["radio"]["profile"]
ok("the applied profile is the MeshCore one: frequency", p["frequency_mhz"] == 869.618, p)
ok("...bandwidth", p["bandwidth_khz"] == 62.5, p)
ok("...spreading factor", p["spreading_factor"] == 8, p)
ok("...coding rate", p["coding_rate"] == 5, p)
ok("...sync word", p["sync_word"] == 0x12, p)
ok("...preamble", p["preamble_length"] == 32, p)
ok("...CRC", p["crc"] is True, p)
ok("...and the tested transmit power, not the region maximum",
   p["tx_power_dbm"] == 2, p)

# radiod's own view of the same lease, from a second connection.
lease = r.result("radio.lease")
ok("radiod says the radio is held", lease["held"] is True, lease)
ok("by meshcored, named", lease.get("owner") == "meshcored", lease)
ok("and not by this connection", lease["mine"] is False)

e = r.error("radio.configure", {"frequency_mhz": 868.0})
ok("another client cannot reconfigure the radio underneath it",
   e["code"] == 3, e)

rs = r.result("radio.status")
ok("radiod is in receive", rs["state"] == "rx", rs)
ok("and carries the profile meshcored asked for",
   rs["profile"]["frequency_mhz"] == 869.618, rs["profile"])
done()
PYEOF
run_driver "$TMP/t2.py" "$TMP" "$MSOCK" "$RSOCK"

# ---------------------------------------------------------------------------
# 3. a real MeshCore advert in, and a real one out
# ---------------------------------------------------------------------------
echo "--- receive and transmit through radiod"

cat > "$TMP/t3.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

msock, rsock, advert_hex, peer_key = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
m = Conn(msock)
r = Conn(rsock)
m.result("mesh.subscribe")
r.result("radio.subscribe")

before = m.result("mesh.status")["counters"]

# A frame that is not a MeshCore packet at all. It must be received, counted
# and thrown away by MeshCore's own bounded parse - not crash anything.
r.result("mock.inject_rx", {"payload_hex": "ff" * 40})
time.sleep(0.5)
mid = m.result("mesh.status")["counters"]
ok("a frame radiod delivered was counted", mid["rx_events"] == before["rx_events"] + 1)
ok("and was handed to the protocol core",
   mid["rx_delivered"] == before["rx_delivered"] + 1)
ok("nothing was rejected as malformed", mid["rx_rejected"] == before["rx_rejected"])
ok("and it did not become a node", m.result("mesh.status")["nodes"] == 0)

# Now the real thing: a signed MeshCore advert from the tool whose frames an
# independent RIFT peer accepted on air.
r.result("mock.inject_rx", {"payload_hex": advert_hex, "rssi_dbm": -66.5, "snr_db": 9.25})
ev = m.wait_event("mesh.node", seconds=10)
ok("a signed advert raises a node event", ev is not None)
if ev:
    node = ev["data"]["node"]
    ok("naming the sender", node["name"] == "TEST-PEER", node)
    ok("with its real public key", node["public_key"] == peer_key, node)
    ok("and the node hash radiod would route on",
       node["node_hash"] == peer_key[:2], node)
    ok("heard at the signal radiod reported", node.get("last_rssi_dbm") == -66.5, node)
    ok("and the SNR", node.get("last_snr_db") == 9.25, node)
    ok("with no path known yet, said rather than implied",
       node["path_known"] is False and "hops" not in node, node)

nodes = m.result("mesh.nodes")
ok("and it is in the node list", nodes["count"] == 1, nodes)
one = m.result("mesh.node", {"node": peer_key[:4]})
ok("a prefix finds it", one["public_key"] == peer_key)
e = m.error("mesh.node", {"node": "zz"})
ok("a prefix that is not hex is refused", e["code"] == 2, e)
e = m.error("mesh.node", {"node": "0000"})
ok("a prefix nobody matches is refused", e["code"] == 2, e)

# A duplicate of the same frame. MeshCore's duplicate table must swallow it.
events_before = len([e for e in m.events if e.get("event") == "mesh.node"])
r.result("mock.inject_rx", {"payload_hex": advert_hex})
time.sleep(0.6)
m.drain(0.3)
ok("a repeated advert does not raise a second node event",
   len([e for e in m.events if e.get("event") == "mesh.node"]) == events_before)

# Transmit. This is the whole asynchronous path: mesh.advert -> the protocol
# core -> radio.send_async -> tx_id -> radio.tx_done -> the outcome.
#
# radiod announces state tx for the airtime of the packet, so this is also
# where the service is held to not reporting its own voice as a radio it
# cannot use (docs/hardware/MESHCORED_HARDWARE_GATE.md, finding 1). The
# harness proves it against a stand-in that can be made slow; here it is the
# real radiod, with a real radio.state event, and the check is the one a
# client sees: the events that were announced.
m.drain(0.3)
m.events = [e for e in m.events if e.get("event") != "mesh.state"]
tx_before = r.result("radio.stats")["tx_packets"]
res = m.result("mesh.advert")
ok("an advert is accepted", res["accepted"] is True)
done_ev = r.wait_event("radio.tx_done", seconds=15)
ok("radiod reports a completion", done_ev is not None)
if done_ev:
    d = done_ev["data"]
    ok("the packet went out", d["transmitted"] is True, d)
    ok("the radio is receiving again", d["rx_resumed"] is True, d)
    ok("and the completion is a success", d["ok"] is True, d)
act = m.wait_event("mesh.activity", seconds=10,
                   match=lambda d: d.get("kind") == "tx")
ok("meshcored reports the transmit outcome to its own clients", act is not None)
if act:
    ok("as the result radiod gave", act["data"]["result"] == "ok", act["data"])
ok("radiod counted exactly one transmit",
   r.result("radio.stats")["tx_packets"] == tx_before + 1)

st = m.result("mesh.status")["counters"]
ok("meshcored counted the submission", st["tx_submitted"] >= 1)
ok("radiod accepted it", st["tx_accepted"] >= 1)
ok("it completed", st["tx_ok"] >= 1)
ok("nothing failed", st["tx_failed"] == 0 and st["tx_unknown"] == 0)
ok("and no completion was left unmatched", st["tx_done_unmatched"] == 0)

# radiod was in state tx and said so: that is the event this service used to
# answer with two of its own.
state_ev = r.wait_event("radio.state", seconds=5, match=lambda d: d.get("state") == "tx")
ok("radiod announced the radio transmitting", state_ev is not None)
m.drain(0.5)
announced = [e["data"] for e in m.events if e.get("event") == "mesh.state"]
ok("and meshcored announced no state change for its own transmit",
   announced == [], announced)
ok("it is online, with radiod receiving again",
   m.result("mesh.status")["state"] == "online" and
   m.result("mesh.status")["radio"]["radio_state"] == "rx")

# Input the API must refuse.
e = m.error("mesh.send", {"to": peer_key[:4], "text": ""})
ok("an empty message is refused", e["code"] == 2, e)
e = m.error("mesh.send", {"to": peer_key[:4], "text": "x" * 500})
ok("an over-long message is refused", e["code"] == 2, e)
e = m.error("mesh.send", {"to": peer_key[:4], "text": "bad\x1b[2J"})
ok("a message with a control sequence is refused", e["code"] == 2, e)
e = m.error("mesh.send", {"to": peer_key[:4], "text": 42})
ok("a message that is not a string is refused", e["code"] == 2, e)
e = m.error("mesh.send", {"text": "no recipient"})
ok("a message with no recipient is refused", e["code"] == 2, e)
e = m.error("mesh.send", "not-an-object")
ok("params that are not an object are refused", e["code"] == 2, e)
e = m.error("mesh.nonsense")
ok("an unknown method is refused", e["code"] == 1, e)

# A message to a known node. It cannot be acknowledged here - the mock
# backend transmits into nothing - so the point is that it is accepted,
# transmitted, and left in a state that says it has not been answered.
res = m.result("mesh.send", {"to": peer_key[:4], "text": "hello over the mock air"})
ok("a message to a known node is accepted", res["accepted"] is True)
ok("its route is flood, no path being known", res["route"] == "flood", res)
ok("it has an ACK deadline", res["ack_timeout_ms"] > 0)
msg_id = res["message_id"]
msgs = m.result("mesh.messages")
ok("it is in the message list", any(x["id"] == msg_id for x in msgs["messages"]))
ok("the list says plainly that it is not persistent", msgs["persistent"] is False)
one = [x for x in msgs["messages"] if x["id"] == msg_id][0]
ok("recorded as outgoing", one["direction"] == "out")
ok("with the text", one["text"] == "hello over the mock air")
ok("and a state that is not 'acknowledged'", one["state"] != "acked", one["state"])
done()
PYEOF
run_driver "$TMP/t3.py" "$TMP" "$MSOCK" "$RSOCK" "$ADVERT_HEX" "$PEER_KEY"

# ---------------------------------------------------------------------------
# 3b. a node whose name is hostile
# ---------------------------------------------------------------------------
echo "--- remote text that should not reach a client raw"

cat > "$TMP/t3b.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

msock, rsock = sys.argv[2], sys.argv[3]
esc_hex, esc_key = sys.argv[4], sys.argv[5]

m = Conn(msock)
r = Conn(rsock)
m.result("mesh.subscribe")
r.result("radio.subscribe")

# The driver reads frames with json.loads on bytes, which in Python 3 refuses
# anything that is not valid UTF-8. So every call below is itself the test:
# if a raw byte reached the wire, this connection would stop working here
# rather than returning a wrong answer.
r.result("mock.inject_rx", {"payload_hex": esc_hex})
ev = m.wait_event("mesh.node", seconds=10)
ok("a node with an escape sequence in its name is still learned", ev is not None)
if ev:
    name = ev["data"]["node"]["name"]
    ok("and the node is the one that adverted",
       ev["data"]["node"]["public_key"] == esc_key)
    ok("the escape character does not reach the client", "\x1b" not in name, repr(name))
    ok("nor does any other control character",
       all(ord(c) >= 0x20 or c in "\n\t" for c in name), repr(name))
    ok("it was replaced rather than dropped", "�" in name, repr(name))
    ok("and the printable part survived", "A" in name and "B" in name, repr(name))

# And the same through a method rather than an event, since a client may
# never subscribe at all.
nodes = m.result("mesh.nodes")
found = [n for n in nodes["nodes"] if n["public_key"] == esc_key]
ok("it is in the node list", len(found) == 1, len(found))
for n in found:
    ok("with no control character in the name",
       all(ord(c) >= 0x20 or c in "\n\t" for c in n["name"]), repr(n["name"]))
one = m.result("mesh.node", {"node": esc_key[:8]})
ok("and mesh.node answers for it too", one["public_key"] == esc_key)
ok("with the same safe name", "\x1b" not in one["name"])

# Every call in this driver decoded a frame as UTF-8 to get here: had a raw
# byte reached the wire, json.loads would have refused it and this section
# would have died rather than returned a wrong answer.
ok("every frame this section read was valid UTF-8", True)
ok("the service is still online", m.result("mesh.status")["state"] == "online")
ok("and still counts it as a real node", m.result("mesh.status")["nodes"] >= 2)
done()
PYEOF
run_driver "$TMP/t3b.py" "$TMP" "$MSOCK" "$RSOCK" "$ESC_HEX" "$NASTY_KEY"

# ---------------------------------------------------------------------------
# 4. radiod goes away and comes back
# ---------------------------------------------------------------------------
echo "--- radiod restarts"

cat > "$TMP/t4a.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *
m = Conn(sys.argv[2])
m.result("mesh.subscribe")
ident = m.result("mesh.identity")
nodes = m.result("mesh.nodes")["count"]
print("IDENT " + ident["public_key"])
print("NODES %d" % nodes)
ok("a snapshot was taken before radiod is stopped", True)
done()
PYEOF
python3 "$TMP/t4a.py" "$TMP" "$MSOCK" > "$TMP/before.txt" 2>&1
BEFORE_KEY=$(awk '/^IDENT/ {print $2}' "$TMP/before.txt")
BEFORE_NODES=$(awk '/^NODES/ {print $2}' "$TMP/before.txt")

stop_radiod

cat > "$TMP/t4b.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

m = Conn(sys.argv[2])
before_key, before_nodes = sys.argv[3], int(sys.argv[4])
m.result("mesh.subscribe")

reached, last = wait_state(m, "waiting_for_radiod", seconds=15)
ok("meshcored notices radiod is gone", reached, last)
ok("and did not crash: it is still answering", m.result("mesh.info")["service"] == "meshcored")

st = m.result("mesh.status")
ok("it reports the connection gone", st["radio"]["connected"] is False)
ok("it drops the lease it can no longer hold", st["radio"]["lease_held"] is False)
ok("the runtime is offline", st["radio"]["online"] is False)
ok("one disconnect is counted", st["counters"]["radiod_disconnects"] == 1)
ok("and the profile it can no longer claim is gone", "profile" not in st["radio"])

ok("the identity is unchanged by the radio going away",
   m.result("mesh.identity")["public_key"] == before_key)
ok("and so are the nodes it had learned",
   m.result("mesh.nodes")["count"] == before_nodes)

e = m.error("mesh.send", {"to": "aabb", "text": "hello"})
ok("a send is refused while the radio is away", e["code"] == 5, e)
done()
PYEOF
run_driver "$TMP/t4b.py" "$TMP" "$MSOCK" "$BEFORE_KEY" "$BEFORE_NODES"

start_radiod

cat > "$TMP/t4c.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

m = Conn(sys.argv[2])
r = Conn(sys.argv[3])
before_key, before_nodes = sys.argv[4], int(sys.argv[5])
m.result("mesh.subscribe")

reached, last = wait_state(m, "online", seconds=25)
ok("meshcored comes back online when radiod returns", reached, last)

st = m.result("mesh.status")
ok("it reacquired the lease", st["radio"]["lease_held"] is True)
ok("two connects are counted", st["counters"]["radiod_connects"] == 2)
ok("one disconnect is counted", st["counters"]["radiod_disconnects"] == 1)
ok("two lease acquisitions", st["counters"]["lease_acquired"] == 2)
ok("and the profile was applied again",
   st["radio"]["profile"]["frequency_mhz"] == 869.618, st["radio"].get("profile"))
ok("radiod agrees it is held",
   r.result("radio.lease")["held"] is True)
ok("the identity survived the reconnection",
   m.result("mesh.identity")["public_key"] == before_key)
ok("and the nodes did too", m.result("mesh.nodes")["count"] == before_nodes)

res = m.result("mesh.advert")
ok("and it can transmit again", res["accepted"] is True)
act = m.wait_event("mesh.activity", seconds=15, match=lambda d: d.get("kind") == "tx")
ok("with a completion after the reconnection", act is not None)
if act:
    ok("which succeeded", act["data"]["result"] == "ok", act["data"])
done()
PYEOF
run_driver "$TMP/t4c.py" "$TMP" "$MSOCK" "$RSOCK" "$BEFORE_KEY" "$BEFORE_NODES"

# ---------------------------------------------------------------------------
# 5. the lease is held by somebody else
# ---------------------------------------------------------------------------
echo "--- the radio is busy"
stop_meshcored

cat > "$TMP/t5.py" <<'PYEOF'
import subprocess, sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

rsock, msock, meshcored, state = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]

# Somebody else takes the radio first.
hog = Conn(rsock)
lease = hog.result("radio.acquire", {"owner": "a-test-that-got-there-first"})
ok("another client holds the radio", lease["held"] is True and lease["mine"] is True)

proc = subprocess.Popen([meshcored, "--verbose"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try:
            m = Conn(msock)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        ok("meshcored started", False)
        done()

    reached, last = wait_state(m, "waiting_for_lease", seconds=15)
    ok("meshcored waits for a lease it cannot have", reached, last)
    st = m.result("mesh.status")
    ok("it is connected to radiod all the same", st["radio"]["connected"] is True)
    ok("it holds no lease", st["radio"]["lease_held"] is False)
    ok("and a refusal is counted", st["counters"]["lease_refused"] >= 1)

    # The one thing it must not do.
    ok("the radio is still the other client's",
       hog.result("radio.lease")["mine"] is True)
    ok("and meshcored never took it", hog.result("radio.lease")["owner"] !=
       "meshcored")

    e = m.error("mesh.advert")
    ok("it will not transmit on a radio it does not own", e["code"] == 5, e)

    # And when the radio is given back, it takes it without being asked.
    hog.result("radio.release")
    reached, last = wait_state(m, "online", seconds=40)
    ok("it acquires the radio once it is free", reached, last)
    ok("and says so", m.result("mesh.status")["radio"]["lease_held"] is True)
finally:
    proc.terminate()
    proc.wait(timeout=10)
done()
PYEOF
run_driver "$TMP/t5.py" "$TMP" "$RSOCK" "$MSOCK" "$MESHCORED" "$POCKETOS_STATE_DIR"

# ---------------------------------------------------------------------------
# 5b. a profile radiod will not accept
# ---------------------------------------------------------------------------
#
# 902 MHz is outside radiod's EU868 region guard (863 to 870), so radiod
# refuses the configure with error 3 - after meshcored has already taken the
# lease. The question this asks is not whether meshcored notices, but what it
# does with the radio it is holding when it gives up: a service that enters an
# error state and keeps the lease leaves radiod owned by something that has
# stopped trying, and nothing short of killing the process gets the radio
# back.
echo "--- a profile radiod will not accept"
start_meshcored --frequency-mhz 902.0

cat > "$TMP/t5b.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

m = Conn(sys.argv[2])
r = Conn(sys.argv[3])

reached, last = wait_state(m, "error", seconds=20)
ok("a profile radiod refuses puts meshcored in the error state", reached, last)
st = m.result("mesh.status")
ok("and it says why", "869" not in st["reason"] and st["reason"] != "", st["reason"])

# The point of the whole section.
ok("it is not holding the radio lease", st["radio"]["lease_held"] is False, st["radio"])
ok("it has closed the connection", st["radio"]["connected"] is False, st["radio"])
ok("it claims no applied profile", "profile" not in st["radio"], st["radio"])
ok("and the runtime knows it has no radio", st["radio"]["online"] is False)

lease = r.result("radio.lease")
ok("radiod agrees the radio is free", lease["held"] is False, lease)

# Somebody else can have it, which is the thing that was impossible before.
got = r.result("radio.acquire", {"owner": "the-next-client"})
ok("another client can acquire radiod afterwards", got["held"] is True and got["mine"] is True)
ok("and configure it", r.result("radio.configure", {"frequency_mhz": 868.1})["frequency_mhz"] == 868.1)
r.result("radio.release")

# Terminal, and quietly so: no reconnect loop, no retry of a profile that
# cannot be accepted, and the service still answers.
time.sleep(3.0)
st = m.result("mesh.status")
ok("it stays in error rather than retrying", st["state"] == "error", st["state"])
ok("and has not reconnected", st["radio"]["connected"] is False)
ok("and has not taken the lease back", st["radio"]["lease_held"] is False)
ok("it still answers its own clients", m.result("mesh.info")["service"] == "meshcored")
ok("and still has its identity", len(m.result("mesh.identity")["public_key"]) == 64)
e = m.error("mesh.advert")
ok("but will not transmit", e["code"] == 5, e)
ok("radiod is still free at the end", r.result("radio.lease")["held"] is False)
done()
PYEOF
run_driver "$TMP/t5b.py" "$TMP" "$MSOCK" "$RSOCK"
stop_meshcored

# ---------------------------------------------------------------------------
# 6. clients come and go
# ---------------------------------------------------------------------------
echo "--- clients come and go"
start_meshcored

cat > "$TMP/t6.py" <<'PYEOF'
import sys, time
sys.path.insert(0, sys.argv[1])
from lib import *

msock, rsock = sys.argv[2], sys.argv[3]
m = Conn(msock)
r = Conn(rsock)
reached, last = wait_state(m, "online", seconds=25)
ok("the service is online again", reached, last)

# A frame that MeshCore's parser accepts, so it reaches the activity feed: a
# flood-routed ACK with no hops. Bytes of 0xAA would not - their top two bits
# make the header claim a payload version this build does not speak, and
# tryParsePacket refuses it before anything is logged.
PARSEABLE = "0d00" + "aabbccdd11223344"

# Many clients connecting and disconnecting quickly. The service must not
# leak them, wedge, or lose its own state.
for i in range(60):
    c = Conn(msock)
    c.result("mesh.subscribe")
    c.close()
ok("sixty rapid connect/disconnect cycles leave it answering",
   m.result("mesh.info")["service"] == "meshcored")
ok("and online", m.result("mesh.status")["state"] == "online")

# A subscriber that goes away in the middle of a broadcast must not take the
# service with it. Half of these never read a byte.
deaf = [Conn(msock) for _ in range(6)]
for c in deaf:
    c.result("mesh.subscribe")
for i in range(12):
    r.result("mock.inject_rx", {"payload_hex": PARSEABLE})
for c in deaf[:3]:
    c.close()
time.sleep(1.0)
ok("a client disappearing during a broadcast does not stop the service",
   m.result("mesh.status")["state"] == "online")
for c in deaf[3:]:
    c.close()

# A subscriber sees events; one that unsubscribed does not.
sub = Conn(msock)
sub.result("mesh.subscribe")
quiet = Conn(msock)
quiet.result("mesh.subscribe")
quiet.result("mesh.unsubscribe")
r.result("mock.inject_rx", {"payload_hex": PARSEABLE})
ev = sub.wait_event("mesh.activity", seconds=5)
ok("a subscriber is told about activity", ev is not None)
quiet.drain(0.5)
ok("and one that unsubscribed is not", len(quiet.events) == 0)
sub.close()
quiet.close()

# A burst. The queue is bounded, the counters must account for everything,
# and nothing may be counted as delivered that was not.
before = m.result("mesh.status")["counters"]
for i in range(150):
    r.result("mock.inject_rx", {"payload_hex": PARSEABLE})
time.sleep(2.0)
after = m.result("mesh.status")["counters"]
seen = after["rx_events"] - before["rx_events"]
ok("every injected frame reached meshcored", seen == 150, seen)
ok("and each one was delivered or dropped, none unaccounted for",
   (after["rx_delivered"] - before["rx_delivered"]) +
   (after["rx_dropped"] - before["rx_dropped"]) +
   (after["rx_rejected"] - before["rx_rejected"]) == seen)
ok("the service is still online after the burst",
   m.result("mesh.status")["state"] == "online")
done()
PYEOF
run_driver "$TMP/t6.py" "$TMP" "$MSOCK" "$RSOCK"

# ---------------------------------------------------------------------------
# 7. shutdown
# ---------------------------------------------------------------------------
echo "--- shutdown"
kill -TERM "$MCD_PID" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$MCD_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$MCD_PID" 2>/dev/null; then
    check "meshcored stops on SIGTERM" 0
    kill -KILL "$MCD_PID" 2>/dev/null
else
    check "meshcored stops on SIGTERM" 1
fi
wait "$MCD_PID" 2>/dev/null
MCD_PID=""
check "and removes its socket" "$([ ! -S "$MSOCK" ] && echo 1 || echo 0)"
check "the identity file is still 0600" \
    "$([ "$(stat -c %a "$POCKETOS_STATE_DIR/meshcored/identity.id")" = "600" ] && echo 1 || echo 0)"
check "and the node state is 0600" \
    "$([ "$(stat -c %a "$POCKETOS_STATE_DIR/meshcored/state.v1")" = "600" ] && echo 1 || echo 0)"
check "the state directory is 0700" \
    "$([ "$(stat -c %a "$POCKETOS_STATE_DIR/meshcored")" = "700" ] && echo 1 || echo 0)"

# radiod must be unharmed by all of it, and its lease released.
cat > "$TMP/t7.py" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1])
from lib import *
r = Conn(sys.argv[2])
ok("radiod is still answering after meshcored stopped",
   r.result("radio.info")["chip"] == "mock")
lease = r.result("radio.lease")
ok("and the radio lease was given back", lease["held"] is False, lease)
ok("radiod is still receiving", r.result("radio.status")["state"] == "rx")
done()
PYEOF
run_driver "$TMP/t7.py" "$TMP" "$RSOCK"

# The logs must carry no error the service did not explain. Exactly one is
# explained: section 5b hands it a profile radiod will not accept, on purpose,
# and an ERROR line is the correct answer to that. It is named here rather
# than allowed by a blanket exemption, and its presence is asserted too - a
# daemon that gave up silently would be worse than one that complained.
EXPECTED_ERROR='radiod refused the radio profile permanently'
unexplained=$(grep " ERROR " "$TMP/meshcored.log" | grep -v "$EXPECTED_ERROR" || true)
if [ -n "$unexplained" ]; then
    echo "FAIL meshcored logged an ERROR nothing in this suite asked for:"
    printf '%s\n' "$unexplained" | head -5
    failed=$((failed + 1))
else
    check "meshcored logged no ERROR it was not given" 1
fi
check "and it did log the refusal it was given" \
    "$(grep -q "$EXPECTED_ERROR" "$TMP/meshcored.log" && echo 1 || echo 0)"
if grep -qi "sanitizer\|AddressSanitizer\|runtime error" "$TMP/meshcored.log"; then
    echo "FAIL meshcored logged a sanitizer report"; failed=$((failed + 1))
fi

echo "meshcored_service_test: $failed failure(s)"
exit $((failed > 0))
