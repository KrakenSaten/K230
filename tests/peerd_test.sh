#!/bin/bash
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
# Two real peerd processes on one machine, on two loopback addresses, driven
# over pocketipc exactly as a client drives them (docs/api/peer.md).
#
# The peerds under test are out/peerd/peerd-testhooks: the shipped service
# with only peer_net.c able to bind one address (PEERD_TEST_BIND) instead of
# all. The shipped services/peerd/peerd does not contain that name, which is
# checked first.
#
# Covers: silence until asked (no UDP socket bound), the pocketipc surface
# and its refusals, a probe by address, sealed datagrams both ways and as an
# event, kept peers across a restart with nobody visible, the socket closing
# again, garbage from the network, and the two faults that leave peerd
# closed with the state untouched.
#
# Run by `make ENABLE_PEERD=1 SODIUM_DIR=... peerd-service-test`.
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
PEERD=${PEERD:-$REPO/out/peerd/peerd-testhooks}
SHIPPED=${SHIPPED:-$REPO/services/peerd/peerd}
[ -x "$PEERD" ] && [ -x "$SHIPPED" ] || { echo "FAIL peerd is not built (make peerd-service-test)"; exit 1; }
if strings "$SHIPPED" | grep -q "PEERD_TEST"; then
    echo "FAIL the shipped peerd contains a test hook"; exit 1
fi
echo "ok   the shipped peerd contains no test hook"
ROOT=$(mktemp -d /tmp/peerd_test.XXXXXX)
export PEERD ROOT
python3 - <<'PY'
import json, os, signal, socket, struct, subprocess, sys, time

PEERD, ROOT = os.environ["PEERD"], os.environ["ROOT"]
PORT = 47931
failed = 0

def check(name, ok):
    global failed
    print(("ok   " if ok else "FAIL ") + name, flush=True)
    failed += 0 if ok else 1

class Peer:
    def __init__(self, tag, addr):
        self.tag, self.addr = tag, addr
        self.dirs = {k: os.path.join(ROOT, tag, k) for k in ("run", "state", "log", "config")}
        for d in self.dirs.values():
            os.makedirs(d, exist_ok=True)
        self.proc = None
    def env(self):
        e = dict(os.environ)
        e.update(POCKETOS_RUNTIME_DIR=self.dirs["run"], POCKETOS_STATE_DIR=self.dirs["state"],
                 POCKETOS_LOG_DIR=self.dirs["log"], POCKETOS_CONFIG_DIR=self.dirs["config"],
                 PEERD_TEST_BIND=self.addr, POCKETOS_LOG_STDERR="0")
        return e
    def start(self):
        self.proc = subprocess.Popen([PEERD, "--port", str(PORT), "--name", self.tag], env=self.env(),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        path = os.path.join(self.dirs["run"], "peerd.sock")
        for _ in range(100):
            if os.path.exists(path):
                try:
                    self.call("peer.info"); return True
                except OSError:
                    pass
            time.sleep(0.05)
        return False
    def stop(self):
        if self.proc:
            self.proc.send_signal(signal.SIGTERM)
            try:
                rc = self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill(); rc = -9
            self.proc = None
            return rc
        return None
    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(os.path.join(self.dirs["run"], "peerd.sock"))
        return s
    @staticmethod
    def send(s, obj):
        body = json.dumps(obj).encode()
        s.sendall(struct.pack(">I", len(body)) + body)
    @staticmethod
    def recv(s):
        def read(n):
            b = b""
            while len(b) < n:
                c = s.recv(n - len(b))
                if not c:
                    raise OSError("closed")
                b += c
            return b
        return json.loads(read(struct.unpack(">I", read(4))[0]))
    def call(self, method, params=None):
        s = self.connect()
        req = {"id": 1, "method": method}
        if params is not None:
            req["params"] = params
        self.send(s, req)
        r = self.recv(s)
        s.close()
        return r
    def result(self, method, params=None):
        return self.call(method, params).get("result", {})
    def code(self, method, params=None):
        return self.call(method, params).get("error", {}).get("code", 0)
    def state(self, name):
        return os.path.join(self.dirs["state"], "peerd", name)

def udp_port_free(addr):
    """True when nothing is bound to addr:PORT (peerd is closed)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind((addr, PORT)); return True
    except OSError:
        return False
    finally:
        s.close()

def wait(pred, seconds=4.0):
    end = time.time() + seconds
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False

A, B = Peer("alpha", "127.0.0.2"), Peer("bravo", "127.0.0.3")
try:
    check("start: both come up and answer", A.start() and B.start())
    info = A.result("peer.info")
    check("info: the service, its API and its wire version",
          info.get("service") == "peerd" and info.get("api") == 0 and info.get("wire") == 1)
    st = A.result("peer.status")
    check("silence: ready, not visible, and no socket", st.get("state") == "ready" and
          st.get("visible") is False and st.get("socket") is False and st.get("kept_fault") is False)
    check("silence: nothing is bound to its UDP port", udp_port_free(A.addr) and udp_port_free(B.addr))
    check("silence: this run's epoch is 2 on a new device", st.get("epoch") == 2)
    ida, idb = A.result("peer.identity"), B.result("peer.identity")
    ka, kb = ida.get("key", ""), idb.get("key", "")
    check("identity: a whole key, 64 hex digits, and the two differ",
          len(ka) == 64 and len(kb) == 64 and ka != kb and all(c in "0123456789abcdef" for c in ka))
    fp = ida.get("fingerprint", "")
    check("identity: the fingerprint is sixteen digits in fours, the key's first",
          fp.replace(" ", "") == ka[:16] and len(fp.split(" ")) == 4)
    check("identity: the name it was given", ida.get("name") == "alpha")
    for f, size in (("identity.v1", 74), ("epoch.a", 16), ("epoch.b", 16)):
        stt = os.stat(A.state(f))
        check("state: %s is %d bytes, 0600" % (f, size), stt.st_size == size and (stt.st_mode & 0o777) == 0o600)
    check("state: the directory is 0700", (os.stat(A.state("")).st_mode & 0o777) == 0o700)
    seed = open(A.state("identity.v1"), "rb").read()[6:38].hex()
    logs = b"".join(open(os.path.join(A.dirs["log"], f), "rb").read() for f in os.listdir(A.dirs["log"]))
    check("identity: the secret is not in the log", seed.encode() not in logs and len(logs) > 0)
    check("identity: nor does any method return it",
          seed not in json.dumps(A.result("peer.identity")) + json.dumps(A.result("peer.status")))

    # --- refusals -----------------------------------------------------------------------
    check("refused: an unknown method", A.code("peer.nonsense") == 1)
    check("refused: an unknown parameter", A.code("peer.set_visible", {"visible": True, "extra": 1}) == 2)
    check("refused: visible that is not true or false", A.code("peer.set_visible", {"visible": 1}) == 2)
    check("refused: a key that is not whole", A.code("peer.keep", {"key": ka[:62]}) == 2 and
          A.code("peer.keep", {"key": "zz" * 32}) == 2)
    check("refused: keeping this device's own key", A.code("peer.keep", {"key": ka}) == 2)
    check("refused: keeping the identity point (a small-order key)",
          A.code("peer.keep", {"key": "01" + "00" * 31}) == 2)
    check("refused: a port outside 1..15", A.code("peer.app_send", {"to": kb, "port": 0, "payload_hex": "00"}) == 2 and
          A.code("peer.app_send", {"to": kb, "port": 16, "payload_hex": "00"}) == 2)
    check("refused: 1025 bytes, none, and hex that is not hex",
          A.code("peer.app_send", {"to": kb, "port": 1, "payload_hex": "00" * 1025}) == 2 and
          A.code("peer.app_send", {"to": kb, "port": 1, "payload_hex": ""}) == 2 and
          A.code("peer.app_send", {"to": kb, "port": 1, "payload_hex": "0g"}) == 2)
    check("refused: sending to a key that is not a node", A.code("peer.app_send", {"to": kb, "port": 1, "payload_hex": "aa"}) == 2)
    check("refused: a probe while not visible", A.code("peer.probe", {"address": B.addr}) == 3)
    check("refused: an address that is not dotted IPv4",
          A.code("peer.probe", {"address": "bravo.local"}) == 2 and A.code("peer.probe", {"address": "1.2.3"}) == 2 and
          A.code("peer.probe", {"address": "1.2.3.4.5"}) == 2 and A.code("peer.probe", {"address": "256.1.1.1"}) == 2)
    check("refused: a name with a control character", A.code("peer.set_name", {"name": "a\tb"}) == 2)
    check("refused: an announcement with no seats", A.code("peer.announce", {"port": 1, "proto_min": 1, "proto_max": 1, "seats": 0}) == 2)
    s = A.connect(); s.sendall(struct.pack(">I", 7) + b"not js}")
    time.sleep(0.2); s.close()
    check("refused: a frame that is not JSON closes that connection; peerd stays up",
          A.result("peer.status").get("state") == "ready")

    # --- visible: the socket opens --------------------------------------------------------
    check("visible: asked for, it is", A.result("peer.set_visible", {"visible": True}).get("visible") is True and
          B.result("peer.set_visible", {"visible": True}).get("visible") is True)
    check("visible: and now the UDP port is bound", wait(lambda: not udp_port_free(A.addr) and not udp_port_free(B.addr)))
    check("visible: status says so", A.result("peer.status").get("socket") is True)

    # --- a probe by address (broadcast does not exist on loopback: "blocked") -------------
    check("probe: neither has heard the other", A.result("peer.nodes").get("count") == 0)
    pid = A.result("peer.probe", {"address": B.addr}).get("probe_id", 0)
    def answered():
        return any(p["probe_id"] == pid and p["state"] == "answered" for p in A.result("peer.probes").get("probes", []))
    check("probe: answered", pid > 0 and wait(answered))
    probes = A.result("peer.probes").get("probes", [])
    check("probe: by the key at that address", any(p.get("key") == kb for p in probes))
    nodes = A.result("peer.nodes").get("nodes", [])
    check("probe: the node is listed, proved live, with its name, address and fingerprint",
          len(nodes) == 1 and nodes[0]["key"] == kb and nodes[0]["proved"] is True and
          nodes[0]["name"] == "bravo" and nodes[0]["address"] == B.addr and
          nodes[0]["fingerprint"].replace(" ", "") == kb[:16])
    bn = B.result("peer.nodes").get("nodes", [])
    check("probe: the one asked lists the asker, as a claim not yet proved",
          len(bn) == 1 and bn[0]["key"] == ka and bn[0]["proved"] is False)
    pid2 = A.result("peer.probe", {"address": "127.0.0.9"}).get("probe_id", 0)
    check("probe: nobody at an address: no answer, said so within a few seconds",
          wait(lambda: any(p["probe_id"] == pid2 and p["state"] == "no_answer"
                           for p in A.result("peer.probes").get("probes", [])), 6))

    # --- datagrams --------------------------------------------------------------------------
    sub = B.connect()
    Peer.send(sub, {"id": 5, "method": "peer.subscribe"}); Peer.recv(sub)
    sent = A.result("peer.app_send", {"to": kb, "port": 1, "payload_hex": "c0ffee"})
    check("datagram: accepted, on the LAN route, with a timeout estimate",
          sent.get("accepted") is True and sent.get("route") == "lan" and sent.get("bytes") == 3 and
          sent.get("est_timeout_ms", 0) > 0)
    ev = Peer.recv(sub)
    check("datagram: it arrives as a peer.app event, from the sender's whole key",
          ev.get("event") == "peer.app" and ev["data"]["from"] == ka and ev["data"]["payload_hex"] == "c0ffee" and
          ev["data"]["port"] == 1 and ev["data"]["id"] == 1 and ev["data"]["route"] == "lan")
    sub.close()
    box = B.result("peer.app_inbox", {"port": 1})
    check("datagram: and waits in the inbox for a client that was not listening",
          box.get("count") == 1 and box["datagrams"][0]["payload_hex"] == "c0ffee")
    check("datagram: another port's inbox is empty, and after_id skips it",
          B.result("peer.app_inbox", {"port": 2}).get("count") == 0 and
          B.result("peer.app_inbox", {"port": 1, "after_id": 1}).get("count") == 0)
    B.result("peer.app_send", {"to": ka, "port": 3, "payload_hex": "aa" * 1024})
    check("datagram: 1024 bytes come back the other way, whole",
          wait(lambda: A.result("peer.app_inbox", {"port": 3}).get("count") == 1) and
          A.result("peer.app_inbox", {"port": 3})["datagrams"][0]["payload_hex"] == "aa" * 1024)

    # --- garbage from the network -----------------------------------------------------------
    before = A.result("peer.status")["counters"]
    g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    g.bind(("127.0.0.4", 0))
    for blob in (b"", b"\x01", b"\x01\x03" + b"\xff" * 300, os.urandom(210), os.urandom(1123), os.urandom(1400),
                 b"\x01\x01" + os.urandom(208)):
        g.sendto(blob, (A.addr, PORT))
    g.close()
    def counted():
        c = A.result("peer.status")["counters"]
        return c["rx"] - before["rx"] >= 6
    check("garbage: seven datagrams of rubbish are counted and dropped", wait(counted))
    after = A.result("peer.status")["counters"]
    check("garbage: none of it was accepted, and peerd is still ready",
          after["accepted_data"] == before["accepted_data"] and after["accepted_signed"] == before["accepted_signed"] and
          A.result("peer.status").get("state") == "ready" and A.result("peer.nodes").get("count") == 1)

    # --- kept peers; a restart with nobody visible ---------------------------------------------
    check("keep: each keeps the other", A.code("peer.keep", {"key": kb}) == 0 and B.code("peer.keep", {"key": ka}) == 0)
    kept = B.result("peer.kept")
    check("keep: listed, with its address", kept.get("count") == 1 and kept["kept"][0]["key"] == ka and
          kept["kept"][0].get("address") == A.addr)
    check("keep: kept.v1 is on disk, 0600", os.path.exists(B.state("kept.v1")) and
          (os.stat(B.state("kept.v1")).st_mode & 0o777) == 0o600)
    A.result("peer.app_send", {"to": kb, "port": 1, "payload_hex": "01"})
    check("keep: a kept peer's datagram arrives", wait(lambda: B.result("peer.app_inbox", {"port": 1}).get("count") == 2))
    A.result("peer.set_visible", {"visible": False}); B.result("peer.set_visible", {"visible": False})
    time.sleep(0.6)
    check("keep: not visible, but with a kept peer the socket stays", not udp_port_free(A.addr) and not udp_port_free(B.addr))
    run_before = B.result("peer.status")["run_id"]
    epoch_before = B.result("peer.status")["epoch"]
    check("restart: it stops cleanly when asked", B.stop() == 0)
    check("restart: and comes back", B.start())
    stb = B.result("peer.status")
    check("restart: a new run, the next epoch, the same identity",
          stb["run_id"] != run_before and stb["epoch"] == epoch_before + 1 and B.result("peer.identity")["key"] == kb)
    check("restart: the kept peer is back from disk, and the socket is bound though nobody is visible",
          B.result("peer.kept").get("count") == 1 and stb.get("visible") is False and
          wait(lambda: not udp_port_free(B.addr)))
    check("restart: its inbox started empty", B.result("peer.app_inbox", {"port": 1}).get("count") == 0)
    check("restart: the opponent's next datagram is accepted with no beacon and no round trip",
          A.code("peer.app_send", {"to": kb, "port": 1, "payload_hex": "02"}) == 0 and
          wait(lambda: B.result("peer.app_inbox", {"port": 1}).get("count") == 1) and
          B.result("peer.app_inbox", {"port": 1})["datagrams"][0]["payload_hex"] == "02")
    check("restart: and it can answer, to the address it remembered",
          B.code("peer.app_send", {"to": ka, "port": 1, "payload_hex": "03"}) == 0 and
          wait(lambda: any(d["payload_hex"] == "03" for d in A.result("peer.app_inbox", {"port": 1}).get("datagrams", []))))
    check("release: released on both sides", A.code("peer.release", {"key": kb}) == 0 and B.code("peer.release", {"key": ka}) == 0)
    check("release: with nobody kept and nobody visible, the socket is closed again",
          wait(lambda: udp_port_free(A.addr) and udp_port_free(B.addr)))
    check("release: releasing what is not kept is refused", A.code("peer.release", {"key": kb}) == 2)

    # --- the name is kept ----------------------------------------------------------------------
    check("name: set over IPC", B.result("peer.set_name", {"name": "Bjørn"}).get("name") == "Bjørn")

    # --- faults: closed, and nothing regenerated ------------------------------------------------
    A.stop()
    path = A.state("identity.v1")
    raw = bytearray(open(path, "rb").read()); raw[20] ^= 1
    open(path, "wb").write(raw)
    check("fault: it starts even with a damaged identity", A.start())
    st = A.result("peer.status")
    check("fault: and says identity_fault, with no socket", st.get("state") == "identity_fault" and st.get("socket") is False)
    check("fault: everything but status is refused, naming the fault",
          A.code("peer.identity") == 4 and A.code("peer.set_visible", {"visible": True}) == 4 and
          "identity_fault" in A.call("peer.nodes")["error"]["message"])
    check("fault: the damaged file is exactly as it was: no new identity was made",
          open(path, "rb").read() == bytes(raw) and udp_port_free(A.addr))
    A.stop()
    B.stop()
    os.remove(B.state("epoch.a")); os.remove(B.state("epoch.b"))
    check("fault: an identity with no epoch file starts closed", B.start() and
          B.result("peer.status").get("state") == "epoch_fault")
    check("fault: and the epoch was not begun again from 1", not os.path.exists(B.state("epoch.a")) and
          not os.path.exists(B.state("epoch.b")))
finally:
    for p in (A, B):
        p.stop()
print("peerd_test: %d failure(s)" % failed)
sys.exit(1 if failed else 0)
PY
rc=$?
if [ $rc -eq 0 ]; then rm -rf "$ROOT"; else echo "evidence kept in $ROOT"; fi
exit $rc
