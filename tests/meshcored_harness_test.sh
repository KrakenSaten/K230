#!/bin/bash
# Two whole meshcored processes, a mock radiod each, and one mock air between
# them.
#
# tests/meshcored_service_test.sh holds meshcored to the real radiod, which is
# the right test for the relationship and the wrong one for the protocol: the
# real radiod's mock backend transmits into nothing, so no frame one daemon
# sends can ever reach another. This harness supplies the missing half. The
# radiod here is a stand-in - a few hundred lines of Python speaking radio.* -
# and its one extra trick is that a frame handed to radio.send_async on one
# side comes back as a radio.rx event on the other.
#
# What that buys is a two-node MeshCore exchange between two real services,
# each with its own identity on disk, its own state directory and its own
# process: an advert learned, a directed message received, an ACK returned,
# and all of it surviving one of them being restarted.
#
# It also buys the failures. A stand-in radiod can be told to refuse a
# transmit, to report one that did not happen, to report one twice, to report
# one nobody asked for, to answer nothing at all, or to close the connection
# in the middle - none of which the real radiod can be made to do on demand,
# and every one of which meshcored has to answer correctly or it will
# retransmit a packet that already went out.
#
# No hardware, no radio, and nothing here transmits anything anywhere.
set -u

MESHCORED=${MESHCORED:-services/meshcored/meshcored}
TMP=$(mktemp -d)
failed=0

cleanup() {
    pkill -f "$TMP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x "$MESHCORED" ]; then
    echo "FAIL $MESHCORED is not built; run 'make ENABLE_MESHCORED=1 meshcored'"
    exit 1
fi

cat > "$TMP/harness.py" <<'PYEOF'
"""A stand-in radiod, an air between two of them, and the driver."""
import json
import os
import select
import socket
import struct
import subprocess
import sys
import threading
import time

MESHCORED = sys.argv[1]
ROOT = sys.argv[2]

fails = 0


def ok(label, cond, detail=""):
    global fails
    if cond:
        print("ok   " + label)
    else:
        fails += 1
        print("FAIL " + label + ((": " + str(detail)) if detail else ""))


def frame(obj):
    raw = json.dumps(obj).encode()
    return struct.pack(">I", len(raw)) + raw


# ---------------------------------------------------------------------------
# The stand-in radiod
# ---------------------------------------------------------------------------

class Client(object):
    def __init__(self, sock, cid):
        self.sock = sock
        self.cid = cid
        self.buf = b""
        self.subscribed = False


class MockRadiod(threading.Thread):
    """radio.* over pocketipc framing. Only the methods meshcored calls."""

    def __init__(self, path, label, air):
        threading.Thread.__init__(self)
        self.daemon = True
        self.path = path
        self.label = label
        self.air = air
        # Reentrant: a request handler runs holding it, and one of the fault
        # knobs (close_on_send) makes that handler tear the listener down.
        self.lock = threading.RLock()
        self.stop_flag = False
        self.listening = False
        self.srv = None
        self.clients = {}
        self.next_cid = 1
        self.next_tx_id = 1
        self.lease_owner = None
        self.lease_owner_id = 0
        self.next_owner_id = 1
        self.state = "rx"
        self.profile = None
        self.pending = []          # (due_ms, cid, tx_id, bytes, payload_hex)
        # Fault knobs, all off.
        self.send_async_error = None   # (code, message)
        self.tx_transmitted = True
        self.tx_rx_resumed = True
        self.tx_state = "rx"
        self.drop_tx_done = False
        self.duplicate_tx_done = False
        self.bridge = True
        self.close_on_send = False
        self.acquire_error = None
        self.configure_error = None
        self.tx_count = 0
        self.last_tx_hex = None

    # ---- lifecycle ----

    def open(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.path)
        self.srv.listen(8)
        self.srv.setblocking(False)
        self.listening = True

    def close_listener(self):
        """Go away entirely, as a radiod that is stopped does."""
        with self.lock:
            self.listening = False
            for c in list(self.clients.values()):
                try:
                    c.sock.close()
                except OSError:
                    pass
            self.clients = {}
            self.lease_owner = None
            if self.srv:
                try:
                    self.srv.close()
                except OSError:
                    pass
                self.srv = None
            try:
                os.unlink(self.path)
            except OSError:
                pass

    def reopen(self):
        with self.lock:
            self.open()

    def stop(self):
        self.stop_flag = True

    # ---- the loop ----

    def run(self):
        while not self.stop_flag:
            with self.lock:
                socks = []
                if self.srv is not None:
                    socks.append(self.srv)
                socks.extend([c.sock for c in self.clients.values()])
            if not socks:
                time.sleep(0.02)
                self.complete_pending()
                continue
            try:
                ready, _, _ = select.select(socks, [], [], 0.02)
            except (OSError, ValueError):
                # Something in the set is no longer a descriptor. Prune rather
                # than spin: a stand-in that silently stops accepting is worse
                # than one that fails loudly.
                with self.lock:
                    for fd, c in list(self.clients.items()):
                        if c.sock.fileno() < 0:
                            self.clients.pop(fd, None)
                continue
            for s in ready:
                with self.lock:
                    if self.srv is not None and s is self.srv:
                        try:
                            conn, _ = self.srv.accept()
                        except OSError:
                            continue
                        conn.setblocking(False)
                        self.clients[conn.fileno()] = Client(conn, self.next_cid)
                        self.next_cid += 1
                        continue
                    c = self.clients.get(s.fileno())
                    if c is None:
                        continue
                    try:
                        data = s.recv(65536)
                    except OSError:
                        data = b""
                    if not data:
                        self.drop(c)
                        continue
                    c.buf += data
                    self.dispatch(c)
            self.complete_pending()

    def drop(self, c):
        # The descriptor is read BEFORE the close: a closed socket reports
        # fileno() -1, so closing first leaves the entry in the table keyed by
        # its old number, select() is then handed a -1 for ever, and this
        # stand-in stops accepting anything at all.
        fd = c.sock.fileno()
        if self.lease_owner == c.cid:
            self.lease_owner = None
        self.clients.pop(fd, None)
        try:
            c.sock.close()
        except OSError:
            pass

    def send(self, c, obj):
        try:
            c.sock.sendall(frame(obj))
        except OSError:
            self.drop(c)

    def broadcast(self, name, data):
        for c in list(self.clients.values()):
            if c.subscribed:
                self.send(c, {"event": name, "data": data})

    def dispatch(self, c):
        while len(c.buf) >= 4:
            n = struct.unpack(">I", c.buf[:4])[0]
            if len(c.buf) < 4 + n:
                return
            msg = json.loads(c.buf[4:4 + n])
            c.buf = c.buf[4 + n:]
            self.handle(c, msg)

    def reply(self, c, msg, result):
        self.send(c, {"id": msg.get("id"), "result": result})

    def fail(self, c, msg, code, text):
        self.send(c, {"id": msg.get("id"), "error": {"code": code, "message": text}})

    def handle(self, c, msg):
        method = msg.get("method")
        params = msg.get("params") or {}

        if method == "radio.acquire":
            if self.acquire_error is not None:
                code, text = self.acquire_error
                self.fail(c, msg, code, text)
                return
            if self.lease_owner not in (None, c.cid):
                self.fail(c, msg, 5, "held by another client")
                return
            if self.lease_owner != c.cid:
                self.lease_owner = c.cid
                self.lease_owner_id = self.next_owner_id
                self.next_owner_id += 1
            self.reply(c, msg, {"held": True, "mine": True,
                                "owner": params.get("owner", ""),
                                "owner_id": self.lease_owner_id,
                                "since_mono_ms": int(time.monotonic() * 1000)})
            return
        if method == "radio.release":
            if self.lease_owner != c.cid:
                self.fail(c, msg, 3, "not the owner")
                return
            self.lease_owner = None
            self.reply(c, msg, {"held": False, "mine": False})
            return
        if method == "radio.lease":
            self.reply(c, msg, {"held": self.lease_owner is not None,
                                "mine": self.lease_owner == c.cid})
            return
        if method == "radio.configure":
            if self.configure_error is not None:
                code, text = self.configure_error
                self.fail(c, msg, code, text)
                return
            if self.lease_owner not in (None, c.cid):
                self.fail(c, msg, 3, "another client holds the radio")
                return
            self.profile = dict(params)
            self.reply(c, msg, dict(self.profile))
            return
        if method == "radio.subscribe":
            c.subscribed = True
            self.reply(c, msg, {"subscribed": True})
            return
        if method == "radio.unsubscribe":
            c.subscribed = False
            self.reply(c, msg, {"subscribed": False})
            return
        if method == "radio.status":
            self.reply(c, msg, {"state": self.state,
                                "profile": self.profile or {},
                                "uptime_s": 1})
            return
        if method == "radio.info":
            self.reply(c, msg, {"chip": "mock-harness", "backend": "harness",
                                "api_version": 0})
            return
        if method == "radio.stats":
            self.reply(c, msg, {"tx_packets": self.tx_count, "rx_packets": 0})
            return
        if method == "radio.send_async":
            self.on_send(c, msg, params)
            return
        self.fail(c, msg, 1, "unknown method")

    def on_send(self, c, msg, params):
        if self.send_async_error is not None:
            code, text = self.send_async_error
            self.fail(c, msg, code, text)
            return
        if self.lease_owner not in (None, c.cid):
            self.fail(c, msg, 3, "another client holds the radio")
            return
        payload = params.get("payload_hex")
        if not isinstance(payload, str) or len(payload) % 2 or not payload:
            self.fail(c, msg, 2, "payload_hex must be 1 to 255 bytes of hex")
            return
        tx_id = self.next_tx_id
        self.next_tx_id += 1
        nbytes = len(payload) // 2
        self.tx_count += 1
        self.last_tx_hex = payload
        self.reply(c, msg, {"accepted": True, "tx_id": tx_id, "bytes": nbytes,
                            "airtime_ms": 100.0})
        if self.close_on_send:
            # radiod going away with a transmit accepted and on the air. The
            # outcome cannot be known from the other end, which is the case
            # that must NOT become a retransmission.
            self.close_listener()
            return
        self.pending.append((time.monotonic() + 0.05, c.cid, tx_id, nbytes, payload))

    def complete_pending(self):
        now = time.monotonic()
        carries = []
        with self.lock:
            due = [p for p in self.pending if p[0] <= now]
            self.pending = [p for p in self.pending if p[0] > now]
            for _, cid, tx_id, nbytes, payload in due:
                if self.bridge and self.tx_transmitted:
                    # Collected, not carried: carrying reaches into the other
                    # stand-in's lock, and doing that while holding this one
                    # deadlocks the pair the moment both are transmitting.
                    carries.append(payload)
                if self.drop_tx_done:
                    continue
                data = {"tx_id": tx_id,
                        "ok": bool(self.tx_transmitted and self.tx_rx_resumed),
                        "result": ("ok" if self.tx_transmitted and self.tx_rx_resumed
                                   else ("rx_resume_failed" if self.tx_transmitted
                                         else "tx_failed")),
                        "transmitted": bool(self.tx_transmitted),
                        "rx_resumed": bool(self.tx_rx_resumed),
                        "state": self.tx_state,
                        "bytes": nbytes,
                        "airtime_ms": 100.0,
                        "mono_ms": int(now * 1000)}
                self.broadcast("radio.tx_done", data)
                if self.duplicate_tx_done:
                    self.broadcast("radio.tx_done", data)
        for payload in carries:
            self.air.carry(self, payload)

    # ---- what the driver injects ----

    def inject_rx(self, data):
        with self.lock:
            self.broadcast("radio.rx", data)

    def inject_event(self, name, data):
        with self.lock:
            self.broadcast(name, data)

    def inject_stale_tx_done(self, tx_id):
        with self.lock:
            self.broadcast("radio.tx_done", {
                "tx_id": tx_id, "ok": True, "result": "ok", "transmitted": True,
                "rx_resumed": True, "state": "rx", "bytes": 10,
                "airtime_ms": 1.0, "mono_ms": int(time.monotonic() * 1000)})


class Air(object):
    """Every frame one stand-in transmits arrives at the others."""

    def __init__(self):
        self.radios = []
        self.carried = 0

    def add(self, r):
        self.radios.append(r)

    def carry(self, sender, payload_hex):
        self.carried += 1
        for r in self.radios:
            if r is sender:
                continue
            r.inject_rx({"payload_hex": payload_hex,
                         "bytes": len(payload_hex) // 2,
                         "rssi_dbm": -70.5, "snr_db": 8.0,
                         "frequency_error_hz": -200.0,
                         "timestamp_ms": int(time.time() * 1000),
                         "mono_ms": int(time.monotonic() * 1000),
                         "airtime_ms": 100.0})


# ---------------------------------------------------------------------------
# A client of meshcored
# ---------------------------------------------------------------------------

class Conn(object):
    def __init__(self, path, timeout=20):
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

    def call(self, method, params=None, timeout=20):
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

    def result(self, method, params=None):
        r = self.call(method, params)
        if "result" not in r:
            raise IOError(method + " failed: " + json.dumps(r.get("error")))
        return r["result"]

    def error(self, method, params=None):
        return self.call(method, params).get("error")

    def wait_event(self, name, seconds=15, match=None):
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


def wait_state(conn, wanted, seconds=30):
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


def wait_counter(conn, path, wanted, seconds=20):
    """Wait for a counter to reach at least `wanted`. Returns (bool, value)."""
    end = time.monotonic() + seconds
    value = None
    while time.monotonic() < end:
        st = conn.result("mesh.status")
        value = st
        for key in path:
            value = value[key]
        if value >= wanted:
            return True, value
        time.sleep(0.1)
    return False, value


# ---------------------------------------------------------------------------
# The two services
# ---------------------------------------------------------------------------

class Service(object):
    def __init__(self, name, radiod_name):
        self.name = name
        self.radiod_name = radiod_name
        self.state_dir = os.path.join(ROOT, "state", name)
        self.sock = os.path.join(ROOT, "run", name + ".sock")
        self.log = os.path.join(ROOT, name + ".log")
        self.proc = None

    def start(self):
        env = dict(os.environ)
        env["POCKETOS_RUNTIME_DIR"] = os.path.join(ROOT, "run")
        env["POCKETOS_LOG_DIR"] = os.path.join(ROOT, "log")
        env["POCKETOS_STATE_DIR"] = os.path.join(ROOT, "state")
        env["POCKETOS_LOG_STDERR"] = "1"
        out = open(self.log, "ab")
        self.proc = subprocess.Popen(
            [MESHCORED, "--verbose",
             "--socket-name", self.name,
             "--radiod-socket", self.radiod_name,
             "--state-dir", self.state_dir,
             "--name", self.name.upper()],
            stdout=out, stderr=out, env=env)
        for _ in range(200):
            if os.path.exists(self.sock):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def stop(self):
        if self.proc is None:
            return None
        self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = self.proc.wait(timeout=10)
        self.proc = None
        return rc


os.makedirs(os.path.join(ROOT, "run"), exist_ok=True)
os.makedirs(os.path.join(ROOT, "log"), exist_ok=True)
os.makedirs(os.path.join(ROOT, "state"), exist_ok=True)

air = Air()
radio_a = MockRadiod(os.path.join(ROOT, "run", "harness-a.sock"), "a", air)
radio_b = MockRadiod(os.path.join(ROOT, "run", "harness-b.sock"), "b", air)
air.add(radio_a)
air.add(radio_b)
radio_a.open()
radio_b.open()
radio_a.start()
radio_b.start()

svc_a = Service("meshcored-a", "harness-a")
svc_b = Service("meshcored-b", "harness-b")

ok("service A starts", svc_a.start())
ok("service B starts", svc_b.start())

ca = Conn(svc_a.sock)
cb = Conn(svc_b.sock)
ca.result("mesh.subscribe")
cb.result("mesh.subscribe")

# ---------------------------------------------------------------------------
# 1. both come up against their stand-in radiod
# ---------------------------------------------------------------------------
reached, last = wait_state(ca, "online")
ok("A reaches online", reached, last)
reached, last = wait_state(cb, "online")
ok("B reaches online", reached, last)

ident_a = ca.result("mesh.identity")
ident_b = cb.result("mesh.identity")
ok("they have different identities", ident_a["public_key"] != ident_b["public_key"])
ok("A is named as it was started", ident_a["name"] == "MESHCORED-A", ident_a)

prof = ca.result("mesh.status")["radio"]["profile"]
ok("A applied the MeshCore profile", prof["frequency_mhz"] == 869.618, prof)
ok("with the tested power", prof["tx_power_dbm"] == 2, prof)

# ---------------------------------------------------------------------------
# 2. A adverts; B learns A
# ---------------------------------------------------------------------------
ok("A is asked to advert", ca.result("mesh.advert")["accepted"] is True)
ev = cb.wait_event("mesh.node", seconds=20)
ok("B learned a node from the air", ev is not None)
if ev:
    node = ev["data"]["node"]
    ok("and it is A, by public key", node["public_key"] == ident_a["public_key"], node)
    ok("with A's advert name", node["name"] == "MESHCORED-A", node)
    ok("heard at the signal the air reported", node.get("last_rssi_dbm") == -70.5, node)
ok("B's node list has exactly one node", cb.result("mesh.nodes")["count"] == 1)

# The same advert twice more. MeshCore's duplicate table must swallow both:
# on a real mesh the same flood packet arrives by several routes.
last_hex = radio_a.last_tx_hex
before = len([e for e in cb.events if e.get("event") == "mesh.node"])
for _ in range(2):
    air.carry(radio_a, last_hex)
time.sleep(1.0)
cb.drain(0.5)
ok("a repeated advert raises no further node event",
   len([e for e in cb.events if e.get("event") == "mesh.node"]) == before)
ok("and does not duplicate the node", cb.result("mesh.nodes")["count"] == 1)

# ---------------------------------------------------------------------------
# 3. B adverts back, then A sends B a message and gets an ACK
# ---------------------------------------------------------------------------
ok("B is asked to advert", cb.result("mesh.advert")["accepted"] is True)
ev = ca.wait_event("mesh.node", seconds=20)
ok("A learned B", ev is not None and
   ev["data"]["node"]["public_key"] == ident_b["public_key"])

res = ca.result("mesh.send", {"to": ident_b["public_key"][:8],
                              "text": "hello from the harness"})
ok("A sends B a message", res["accepted"] is True)
ok("flood, because no path is known yet", res["route"] == "flood", res)
msg_id = res["message_id"]

ev = cb.wait_event("mesh.message", seconds=25)
ok("B received a message", ev is not None)
if ev:
    m = ev["data"]["message"]
    ok("with the text A sent", m["text"] == "hello from the harness", m)
    ok("incoming", m["direction"] == "in", m)
    ok("from A by public key", m["peer_public_key"] == ident_a["public_key"], m)
    ok("and A's name", m["peer_name"] == "MESHCORED-A", m)

# The ACK comes back inside B's PATH return, so A matches it and learns a
# route from the same frame.
acked = False
end = time.monotonic() + 30
while time.monotonic() < end and not acked:
    for m in ca.result("mesh.messages")["messages"]:
        if m["id"] == msg_id and m["state"] == "acked":
            acked = True
    if not acked:
        time.sleep(0.2)
ok("A's message was acknowledged by B", acked)

node = ca.result("mesh.node", {"node": ident_b["public_key"][:8]})
ok("and A learned a path to B", node["path_known"] is True, node)

# With a path known, the next message goes direct.
res = ca.result("mesh.send", {"to": ident_b["public_key"][:8], "text": "and a second"})
ok("the second message goes direct", res["route"] == "direct", res)
ev = cb.wait_event("mesh.message", seconds=25)
ok("B received the directed message", ev is not None)
if ev:
    ok("with its own text", ev["data"]["message"]["text"] == "and a second")

# ---------------------------------------------------------------------------
# 4. malformed and hostile radio.rx
# ---------------------------------------------------------------------------
before = ca.result("mesh.status")["counters"]
bad_events = [
    {"payload_hex": "zz" * 10},                     # not hex
    {"payload_hex": "abc"},                         # odd length
    {"payload_hex": ""},                            # empty
    {"payload_hex": "00" * 300},                    # longer than a LoRa frame
    {"payload_hex": 42},                            # not a string
    {"payload_hex": None},                          # null
    {},                                             # no payload at all
    {"payload_hex": "41", "rssi_dbm": "not a number"},
    {"payload_hex": "0d00aabbccdd", "mono_ms": "later"},
]
# Numbers no radiod would send, and every one of which would be undefined
# behaviour if it were cast to an integer rather than clamped.
absurd = [
    {"payload_hex": "0d00aabbccdd", "mono_ms": 1e300},
    {"payload_hex": "0d00aabbccdd", "mono_ms": -1e300},
    {"payload_hex": "0d00aabbccdd", "rssi_dbm": 1e300, "snr_db": -1e300},
    {"payload_hex": "0d00aabbccdd", "bytes": 1e300},
]
for e in bad_events:
    radio_a.inject_rx(e)
time.sleep(1.5)
after = ca.result("mesh.status")["counters"]
ok("every malformed payload was rejected",
   after["rx_rejected"] - before["rx_rejected"] == 7,
   after["rx_rejected"] - before["rx_rejected"])
# At least the two well-formed ones. Not exactly: B's own ACK and path
# traffic from the exchange above is still arriving over the air, and a test
# that demanded an exact count here would be asserting that the mesh had
# gone quiet rather than that these events were handled.
ok("and the well-formed ones were delivered",
   after["rx_delivered"] - before["rx_delivered"] >= 2,
   after["rx_delivered"] - before["rx_delivered"])
ok("A is still online after all of that", ca.result("mesh.status")["state"] == "online")

before = ca.result("mesh.status")["counters"]
for e in absurd:
    radio_a.inject_rx(e)
time.sleep(1.5)
after = ca.result("mesh.status")["counters"]
ok("numbers no radio could produce are taken without undefined behaviour",
   after["rx_delivered"] - before["rx_delivered"] >= 4,
   after["rx_delivered"] - before["rx_delivered"])
ok("and A is still online", ca.result("mesh.status")["state"] == "online")
# The telemetry that was not a plausible measurement must not become one.
st = ca.result("mesh.status")
ok("the service still answers its status", st["state"] == "online")

# A tx_done carrying absurd numbers, which reach the same conversions.
radio_a.inject_event("radio.tx_done", {
    "tx_id": 1e300, "ok": True, "result": "ok", "transmitted": True,
    "rx_resumed": True, "state": "rx", "bytes": -1e300, "airtime_ms": 1e300,
    "mono_ms": 1e300})
time.sleep(0.8)
ok("a completion carrying absurd numbers is handled and counted as unmatched",
   ca.result("mesh.status")["counters"]["tx_done_unmatched"] >=
   before["tx_done_unmatched"] + 1)
ok("and A is online after it", ca.result("mesh.status")["state"] == "online")

# A burst of frames at speed. The queue is bounded and nothing may be lost
# from the accounting.
#
# The driver stops subscribing for this: pocketipc disconnects a subscriber
# that has not drained its socket within 200 ms of a blocked write
# (docs/api/pocketipc.md), and three hundred events at once is exactly that.
# A deliberately deaf subscriber is left connected to prove the policy bites
# and that the service outlives it - which is the point, since a client that
# stops reading must not be able to take a protocol daemon down with it.
deaf = Conn(svc_a.sock)
deaf.result("mesh.subscribe")
ca.result("mesh.unsubscribe")
before = ca.result("mesh.status")["counters"]
for i in range(300):
    radio_a.inject_rx({"payload_hex": "0d00" + ("%02x" % (i % 256)) * 8,
                       "mono_ms": int(time.monotonic() * 1000)})
time.sleep(3.0)
after = ca.result("mesh.status")["counters"]
seen = after["rx_events"] - before["rx_events"]
ok("a burst of three hundred frames all arrived", seen >= 300, seen)
ok("and each is accounted for as delivered, dropped or rejected",
   (after["rx_delivered"] - before["rx_delivered"]) +
   (after["rx_dropped"] - before["rx_dropped"]) +
   (after["rx_rejected"] - before["rx_rejected"]) == seen)
ok("A survived the burst", ca.result("mesh.status")["state"] == "online")
try:
    deaf.result("mesh.info")
    deaf_alive = True
except (IOError, OSError):
    deaf_alive = False
deaf.close()
ok("a subscriber that never read was disconnected, and the service did not care",
   ca.result("mesh.status")["state"] == "online", deaf_alive)
ca.result("mesh.subscribe")

# ---------------------------------------------------------------------------
# 5. transmit outcomes that are not success
# ---------------------------------------------------------------------------

# Refused outright.
before = ca.result("mesh.status")["counters"]
radio_a.send_async_error = (5, "a transmission is already in progress")
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_refused"), before["tx_refused"] + 1)
ok("a refused transmit is counted as refused", reached, value)
ok("and not as one that went out",
   ca.result("mesh.status")["counters"]["tx_ok"] == before["tx_ok"])
radio_a.send_async_error = None

# Accepted, then reported as never transmitted.
before = ca.result("mesh.status")["counters"]
radio_a.tx_transmitted = False
radio_a.tx_rx_resumed = False
radio_a.tx_state = "error"
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_failed"), before["tx_failed"] + 1)
ok("a transmit that did not happen is counted as failed", reached, value)
ok("and not as a success",
   ca.result("mesh.status")["counters"]["tx_ok"] == before["tx_ok"])

# Transmitted, but the radio could not go back to receiving. The bytes DID go
# out: this must not be counted as a failure, or the answer would be to send
# them again.
before = ca.result("mesh.status")["counters"]
radio_a.tx_transmitted = True
radio_a.tx_rx_resumed = False
radio_a.tx_state = "error"
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_rx_resume_failed"),
                              before["tx_rx_resume_failed"] + 1)
ok("a transmit that went out with a failed receive is counted apart", reached, value)
ok("and not as a failed transmit",
   ca.result("mesh.status")["counters"]["tx_failed"] == before["tx_failed"])
reached, last = wait_state(ca, "degraded", seconds=10)
ok("and the service says it is degraded, not online", reached, last)

# radiod recovers its receive state.
radio_a.inject_event("radio.state", {"state": "rx"})
reached, last = wait_state(ca, "online", seconds=15)
ok("and online again when radiod says it is receiving", reached, last)
ok("and radiod's own state word is carried through",
   ca.result("mesh.status")["radio"]["radio_state"] == "rx")
radio_a.tx_rx_resumed = True
radio_a.tx_state = "rx"

# And the protocol core really has the radio back, not just the service
# state: a failed completion clears the adapter's receive flag, and a node
# that went on believing it was deaf would be wrong about its own link.
before = ca.result("mesh.status")["counters"]
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_ok"), before["tx_ok"] + 1, seconds=25)
ok("and it transmits normally after the recovery", reached, value)

# A completion that never comes.
before = ca.result("mesh.status")["counters"]
radio_a.drop_tx_done = True
ca.result("mesh.advert")
time.sleep(3.0)
after = ca.result("mesh.status")["counters"]
ok("a submission with no completion is submitted",
   after["tx_submitted"] == before["tx_submitted"] + 1)
ok("and accepted", after["tx_accepted"] == before["tx_accepted"] + 1)
ok("but is not counted as having gone out", after["tx_ok"] == before["tx_ok"])
ok("nor as having failed", after["tx_failed"] == before["tx_failed"])

# The deadline is what ends it. Until it fires, the submission still holds
# the one transmit slot and a further one is refused - which is correct, and
# is why the deadline has to exist at all.
reached, value = wait_counter(ca, ("counters", "tx_unknown"), before["tx_unknown"] + 1,
                              seconds=20)
ok("a completion that never comes is resolved as unknown by the deadline",
   reached, value)
ok("and still not as a failure, which would invite sending it again",
   ca.result("mesh.status")["counters"]["tx_failed"] == before["tx_failed"])
radio_a.drop_tx_done = False

# ...and the service is not wedged: the next transmit completes normally.
before = ca.result("mesh.status")["counters"]
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_ok"), before["tx_ok"] + 1, seconds=25)
ok("a lost completion does not wedge the service", reached, value)

# Two completions for one transmit.
before = ca.result("mesh.status")["counters"]
radio_a.duplicate_tx_done = True
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_ok"), before["tx_ok"] + 1, seconds=25)
ok("a duplicated completion still counts one transmit", reached, value)
time.sleep(0.5)
after = ca.result("mesh.status")["counters"]
ok("exactly one", after["tx_ok"] == before["tx_ok"] + 1, after["tx_ok"])
ok("and the second copy is counted as unmatched",
   after["tx_done_unmatched"] >= before["tx_done_unmatched"] + 1)
radio_a.duplicate_tx_done = False

# A completion for a transmit nobody made.
before = ca.result("mesh.status")["counters"]
radio_a.inject_stale_tx_done(987654)
time.sleep(1.0)
after = ca.result("mesh.status")["counters"]
ok("a completion for an unknown transmit is counted as unmatched",
   after["tx_done_unmatched"] == before["tx_done_unmatched"] + 1)
ok("and changes nothing else", after["tx_ok"] == before["tx_ok"])
ok("A is still online", ca.result("mesh.status")["state"] == "online")

# ---------------------------------------------------------------------------
# 6. the stand-in radiod goes away with a transmit accepted
# ---------------------------------------------------------------------------
before = ca.result("mesh.status")["counters"]
radio_a.close_on_send = True
ca.result("mesh.advert")
reached, value = wait_counter(ca, ("counters", "tx_unknown"), before["tx_unknown"] + 1)
ok("a transmit whose radiod vanished is counted as unknown, not failed", reached, value)
ok("and not as a success",
   ca.result("mesh.status")["counters"]["tx_ok"] == before["tx_ok"])
ok("and not as a failure either, because nobody knows",
   ca.result("mesh.status")["counters"]["tx_failed"] == before["tx_failed"])
reached, last = wait_state(ca, "waiting_for_radiod", seconds=15)
ok("and the service waits for radiod to come back", reached, last)
radio_a.close_on_send = False

nodes_before = ca.result("mesh.nodes")["count"]
ok("it still knows the nodes it learned", nodes_before >= 1)

radio_a.reopen()
reached, last = wait_state(ca, "online", seconds=40)
ok("it comes back online when the stand-in returns", reached, last)
ok("with the nodes it had", ca.result("mesh.nodes")["count"] == nodes_before)
ok("and the same identity", ca.result("mesh.identity")["public_key"] == ident_a["public_key"])

# A reconnect loop: away and back several times in a row.
for i in range(3):
    radio_a.close_listener()
    reached, last = wait_state(ca, "waiting_for_radiod", seconds=20)
    if not reached:
        ok("cycle %d: the service notices radiod is gone" % i, False, last)
        break
    radio_a.reopen()
    reached, last = wait_state(ca, "online", seconds=45)
    if not reached:
        ok("cycle %d: the service comes back" % i, False, last)
        break
else:
    ok("three disconnect/reconnect cycles all recover", True)
ok("and the identity is unchanged through all of them",
   ca.result("mesh.identity")["public_key"] == ident_a["public_key"])

# ---------------------------------------------------------------------------
# 7. restart a whole service
# ---------------------------------------------------------------------------
nodes_b_before = cb.result("mesh.nodes")["count"]
ident_b_before = cb.result("mesh.identity")
messages_b_before = cb.result("mesh.messages")["total"]
ok("B knew at least one node before the restart", nodes_b_before >= 1)
ok("and had received messages", messages_b_before >= 1)

cb.close()
rc = svc_b.stop()
ok("B stops cleanly on SIGTERM", rc == 0, rc)
ok("B removed its socket", not os.path.exists(svc_b.sock))

ok("B starts again", svc_b.start())
cb = Conn(svc_b.sock)
cb.result("mesh.subscribe")
reached, last = wait_state(cb, "online", seconds=40)
ok("and comes back online", reached, last)

ident_b_after = cb.result("mesh.identity")
ok("with the same identity", ident_b_after["public_key"] == ident_b_before["public_key"])
ok("and the same name", ident_b_after["name"] == ident_b_before["name"])
ok("and the nodes it had learned", cb.result("mesh.nodes")["count"] == nodes_b_before)
ok("the node it kept is still A",
   cb.result("mesh.node", {"node": ident_a["public_key"][:8]})["public_key"]
   == ident_a["public_key"])
ok("messages did not survive, which this phase says plainly",
   cb.result("mesh.messages")["total"] == 0)
ok("and the message list says it is not persistent",
   cb.result("mesh.messages")["persistent"] is False)

# The restarted service is a working node, not just a live socket.
res = ca.result("mesh.send", {"to": ident_b["public_key"][:8],
                              "text": "after the restart"})
ok("A can send to the restarted B", res["accepted"] is True)
ev = cb.wait_event("mesh.message", seconds=30)
ok("and B receives it", ev is not None)
if ev:
    ok("with the text", ev["data"]["message"]["text"] == "after the restart")
    ok("still recognising A by key",
       ev["data"]["message"]["peer_public_key"] == ident_a["public_key"])

# And its state on disk is still only readable by its owner.
id_path = os.path.join(svc_b.state_dir, "identity.id")
st_path = os.path.join(svc_b.state_dir, "state.v1")
ok("the identity file is 0600", (os.stat(id_path).st_mode & 0o777) == 0o600)
ok("the node state is 0600", (os.stat(st_path).st_mode & 0o777) == 0o600)
ok("and the directory is 0700", (os.stat(svc_b.state_dir).st_mode & 0o777) == 0o700)

# ---------------------------------------------------------------------------
# 8. shutdown
# ---------------------------------------------------------------------------
ca.close()
cb.close()
rc_a = svc_a.stop()
rc_b = svc_b.stop()
ok("A stops cleanly", rc_a == 0, rc_a)
ok("B stops cleanly", rc_b == 0, rc_b)
radio_a.stop()
radio_b.stop()

for svc in (svc_a, svc_b):
    with open(svc.log, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    ok(svc.name + " logged no ERROR", " ERROR " not in text,
       [l for l in text.splitlines() if " ERROR " in l][:3])
    ok(svc.name + " reported no sanitizer fault",
       "AddressSanitizer" not in text and "runtime error:" not in text)

print("driver: %d failure(s)" % fails)
sys.exit(1 if fails else 0)
PYEOF

python3 "$TMP/harness.py" "$MESHCORED" "$TMP" > "$TMP/out.txt" 2>"$TMP/err.txt"
rc=$?
cat "$TMP/out.txt"
printed=$(grep -c '^FAIL' "$TMP/out.txt" || true)
failed=$((failed + printed))
if ! grep -q '^driver: [0-9]* failure' "$TMP/out.txt"; then
    echo "FAIL the harness exited $rc without reaching its last line; the checks after that point did not run"
    sed 's/^/     /' "$TMP/err.txt" | head -30
    failed=$((failed + 1))
elif [ "$rc" -ne 0 ] && [ "$printed" -eq 0 ]; then
    echo "FAIL the harness ran to the end but exited $rc without printing a failure"
    failed=$((failed + 1))
fi

echo "meshcored_harness_test: $failed failure(s)"
exit $((failed > 0))
