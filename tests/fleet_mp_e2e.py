"""PocketFleet multiplayer end to end: two whole meshcored processes, a
stand-in radiod under each, one mock air between them that can lose and
duplicate frames, and two Fleet players (tests/fleet_mp_player.c) - the real
session, mesh link and match - playing a whole match through them.

    python3 tests/fleet_mp_e2e.py MESHCORED PLAYER ROOT SCENARIO

Scenarios:
  real     a clean air at the product's own pacing (the airtime governor on)
  fast     a clean air, the governor's token bucket refilled every turn
  lossy    15 % of frames lost and 5 % duplicated on each path, fast
  crash    the guest's app dies at ply 30 and is started again on its save
  restart  the guest's meshcored is restarted at ply 40: a new run, an empty
           inbox, the node table read back from disk

The stand-in radiod is a smaller copy of tests/meshcored_harness_test.sh's:
the same radio.* shapes and the same order of radio.state, reply and
radio.tx_done, without its fault knobs, and with an air that can drop and
repeat. Nothing here touches a radio.

Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
"""
import json
import os
import random
import select
import socket
import struct
import subprocess
import sys
import threading
import time

MESHCORED, PLAYER, ROOT, SCENARIO = sys.argv[1:5]
RUN = os.path.join(ROOT, "run")
fails = 0


def ok(label, cond, detail=""):
    global fails
    tag = "[%s] " % SCENARIO
    if cond:
        print("ok   " + tag + label, flush=True)
    else:
        fails += 1
        print("FAIL " + tag + label + ((": " + str(detail)) if detail else ""), flush=True)


def frame(obj):
    raw = json.dumps(obj).encode()
    return struct.pack(">I", len(raw)) + raw


# ---------------------------------------------------------------------------
# The stand-in radiod and the air
# ---------------------------------------------------------------------------

class Client(object):
    def __init__(self, sock, cid):
        self.sock = sock
        self.cid = cid
        self.buf = b""
        self.subscribed = False


class MockRadiod(threading.Thread):
    def __init__(self, path, air):
        threading.Thread.__init__(self)
        self.daemon = True
        self.path = path
        self.air = air
        self.lock = threading.RLock()
        self.clients = {}
        self.next_cid = 1
        self.next_tx_id = 1
        self.lease_owner = None
        self.state = "rx"
        self.profile = None
        self.pending = []
        self.tx_count = 0
        try:
            os.unlink(path)
        except OSError:
            pass
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen(8)
        self.srv.setblocking(False)

    def run(self):
        while True:
            with self.lock:
                socks = [self.srv] + [c.sock for c in self.clients.values()]
            try:
                ready, _, _ = select.select(socks, [], [], 0.01)
            except (OSError, ValueError):
                with self.lock:
                    for fd, c in list(self.clients.items()):
                        if c.sock.fileno() < 0:
                            self.clients.pop(fd, None)
                continue
            for s in ready:
                with self.lock:
                    if s is self.srv:
                        conn, _ = self.srv.accept()
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
                    while len(c.buf) >= 4:
                        n = struct.unpack(">I", c.buf[:4])[0]
                        if len(c.buf) < 4 + n:
                            break
                        msg = json.loads(c.buf[4:4 + n])
                        c.buf = c.buf[4 + n:]
                        self.handle(c, msg)
            self.complete_pending()

    def drop(self, c):
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
            c.sock.setblocking(True)
            c.sock.sendall(frame(obj))
            c.sock.setblocking(False)
        except OSError:
            self.drop(c)

    def broadcast(self, name, data):
        for c in list(self.clients.values()):
            if c.subscribed:
                self.send(c, {"event": name, "data": data})

    def reply(self, c, msg, result):
        self.send(c, {"id": msg.get("id"), "result": result})

    def fail(self, c, msg, code, text):
        self.send(c, {"id": msg.get("id"), "error": {"code": code, "message": text}})

    def handle(self, c, msg):
        method = msg.get("method")
        params = msg.get("params") or {}
        if method == "radio.acquire":
            if self.lease_owner not in (None, c.cid):
                self.fail(c, msg, 5, "held by another client")
                return
            self.lease_owner = c.cid
            self.reply(c, msg, {"held": True, "mine": True, "owner": params.get("owner", ""),
                                "owner_id": c.cid,
                                "since_mono_ms": int(time.monotonic() * 1000)})
        elif method == "radio.release":
            self.lease_owner = None
            self.reply(c, msg, {"held": False, "mine": False})
        elif method == "radio.lease":
            self.reply(c, msg, {"held": self.lease_owner is not None,
                                "mine": self.lease_owner == c.cid})
        elif method == "radio.configure":
            self.profile = dict(params)
            self.reply(c, msg, dict(self.profile))
        elif method == "radio.subscribe":
            c.subscribed = True
            self.reply(c, msg, {"subscribed": True})
        elif method == "radio.unsubscribe":
            c.subscribed = False
            self.reply(c, msg, {"subscribed": False})
        elif method == "radio.status":
            self.reply(c, msg, {"state": self.state, "profile": self.profile or {},
                                "uptime_s": 1})
        elif method == "radio.info":
            self.reply(c, msg, {"chip": "mock-e2e", "backend": "e2e", "api_version": 0})
        elif method == "radio.stats":
            self.reply(c, msg, {"tx_packets": self.tx_count, "rx_packets": 0})
        elif method == "radio.send_async":
            payload = params.get("payload_hex")
            if not isinstance(payload, str) or not payload or len(payload) % 2:
                self.fail(c, msg, 2, "payload_hex must be hex")
                return
            tx_id = self.next_tx_id
            self.next_tx_id += 1
            self.tx_count += 1
            # radiod's order: the state, then the reply, then the completion.
            self.state = "tx"
            self.broadcast("radio.state", {"state": "tx"})
            self.reply(c, msg, {"accepted": True, "tx_id": tx_id, "bytes": len(payload) // 2,
                                "airtime_ms": 100.0})
            self.pending.append((time.monotonic() + 0.05, tx_id, len(payload) // 2, payload))
        else:
            self.fail(c, msg, 1, "unknown method")

    def complete_pending(self):
        now = time.monotonic()
        carries = []
        with self.lock:
            due = [p for p in self.pending if p[0] <= now]
            self.pending = [p for p in self.pending if p[0] > now]
            for _, tx_id, nbytes, payload in due:
                carries.append(payload)
                self.state = "rx"
                self.broadcast("radio.state", {"state": "rx"})
                self.broadcast("radio.tx_done", {
                    "tx_id": tx_id, "ok": True, "result": "ok", "transmitted": True,
                    "rx_resumed": True, "state": "rx", "bytes": nbytes, "airtime_ms": 100.0,
                    "mono_ms": int(now * 1000)})
        for payload in carries:
            self.air.carry(self, payload)

    def inject_rx(self, payload_hex):
        with self.lock:
            self.broadcast("radio.rx", {
                "payload_hex": payload_hex, "bytes": len(payload_hex) // 2,
                "rssi_dbm": -70.5, "snr_db": 8.0, "frequency_error_hz": -200.0,
                "timestamp_ms": int(time.time() * 1000),
                "mono_ms": int(time.monotonic() * 1000), "airtime_ms": 100.0})


class Air(object):
    """Every frame one radio sends reaches the others, unless the air loses
    it; some arrive twice."""

    def __init__(self, loss, dup, seed):
        self.radios = []
        self.loss = loss
        self.dup = dup
        self.rng = random.Random(seed)
        self.lock = threading.Lock()
        self.carried = 0
        self.lost = 0
        self.duplicated = 0

    def carry(self, sender, payload_hex):
        for r in self.radios:
            if r is sender:
                continue
            with self.lock:
                lose = self.rng.random() < self.loss
                twice = self.rng.random() < self.dup
                self.carried += 1
                self.lost += lose
                self.duplicated += (twice and not lose)
            if lose:
                continue
            r.inject_rx(payload_hex)
            if twice:
                r.inject_rx(payload_hex)


# ---------------------------------------------------------------------------
# meshcored, and a client of it
# ---------------------------------------------------------------------------

class Conn(object):
    def __init__(self, path, timeout=20):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""
        self.next_id = 1

    def close(self):
        self.s.close()

    def result(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        body = {"id": rid, "method": method}
        if params is not None:
            body["params"] = params
        self.s.sendall(frame(body))
        while True:
            while len(self.buf) < 4 or len(self.buf) < 4 + struct.unpack(">I", self.buf[:4])[0]:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise IOError("closed")
                self.buf += chunk
            n = struct.unpack(">I", self.buf[:4])[0]
            msg = json.loads(self.buf[4:4 + n])
            self.buf = self.buf[4 + n:]
            if msg.get("id") == rid:
                if "result" not in msg:
                    raise IOError(method + ": " + json.dumps(msg.get("error")))
                return msg["result"]


def call(sock, method, params=None):
    c = Conn(sock)
    try:
        return c.result(method, params)
    finally:
        c.close()


class Service(object):
    def __init__(self, name, radiod_name):
        self.name = name
        self.radiod_name = radiod_name
        self.state_dir = os.path.join(ROOT, "state", name)
        self.sock = os.path.join(RUN, name + ".sock")
        self.log = os.path.join(ROOT, name + ".log")
        self.proc = None

    def start(self):
        env = dict(os.environ)
        env["POCKETOS_RUNTIME_DIR"] = RUN
        env["POCKETOS_LOG_DIR"] = os.path.join(ROOT, "log")
        env["POCKETOS_STATE_DIR"] = os.path.join(ROOT, "state")
        env["POCKETOS_LOG_STDERR"] = "1"
        out = open(self.log, "ab")
        self.proc = subprocess.Popen(
            [MESHCORED, "--socket-name", self.name, "--radiod-socket", self.radiod_name,
             "--state-dir", self.state_dir, "--name", self.name.upper()],
            stdout=out, stderr=out, env=env)
        return self.wait_online()

    def wait_online(self, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                if call(self.sock, "mesh.status")["state"] == "online":
                    return True
            except (IOError, OSError):
                pass
            time.sleep(0.1)
        return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# ---------------------------------------------------------------------------
# A player
# ---------------------------------------------------------------------------

class Player(object):
    def __init__(self, service, role, peer, save, seed, fast, crash_at=0):
        self.lines = []
        self.lock = threading.Lock()
        args = [PLAYER, "--service", service, "--role", role, "--save", save,
                "--seed", str(seed), "--timeout", "1500"]
        if peer:
            args += ["--peer", peer]
        if fast:
            args.append("--fast")
        if crash_at:
            args += ["--crash-at-ply", str(crash_at)]
        env = dict(os.environ)
        env["POCKETOS_RUNTIME_DIR"] = RUN
        self.log = open(os.path.join(ROOT, "%s-%s.log" % (role, int(time.time() * 1000))), "w")
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=self.log, env=env,
                                     universal_newlines=True)
        self.t = threading.Thread(target=self.read)
        self.t.daemon = True
        self.t.start()

    def read(self):
        for line in self.proc.stdout:
            line = line.strip()
            self.log.write(line + "\n")
            self.log.flush()
            with self.lock:
                self.lines.append(line)

    def find(self, prefix):
        with self.lock:
            for line in self.lines:
                if line.startswith(prefix):
                    return line
        return None

    def ply(self):
        with self.lock:
            plies = [int(l.split()[1]) for l in self.lines if l.startswith("PLY ")]
        return max(plies) if plies else 0

    def done(self):
        line = self.find("DONE ")
        if not line:
            return None
        return dict(kv.split("=", 1) for kv in line.split()[1:])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ---------------------------------------------------------------------------
# The scenario
# ---------------------------------------------------------------------------

def main():
    os.makedirs(RUN, exist_ok=True)
    fast = SCENARIO != "real"
    loss, dup = (0.15, 0.05) if SCENARIO == "lossy" else (0.0, 0.0)
    air = Air(loss, dup, seed=sum(map(ord, SCENARIO)))
    ra = MockRadiod(os.path.join(RUN, "radiod-a.sock"), air)
    rb = MockRadiod(os.path.join(RUN, "radiod-b.sock"), air)
    air.radios = [ra, rb]
    ra.start()
    rb.start()
    a = Service("fleet-a", "radiod-a")
    b = Service("fleet-b", "radiod-b")
    players = []
    try:
        ok("meshcored A comes online over its radiod", a.start())
        ok("meshcored B comes online over its radiod", b.start())

        # Each learns the other from an advert; on a lossy air, until it has.
        for _ in range(20):
            call(a.sock, "mesh.advert")
            call(b.sock, "mesh.advert")
            time.sleep(1.2)
            na = [n["name"] for n in call(a.sock, "mesh.nodes")["nodes"]]
            nb = [n["name"] for n in call(b.sock, "mesh.nodes")["nodes"]]
            if "FLEET-B" in na and "FLEET-A" in nb:
                break
        ok("the two nodes know each other", "FLEET-B" in na and "FLEET-A" in nb, (na, nb))
        run_b = call(b.sock, "mesh.status")["run_id"]

        save_a = os.path.join(ROOT, "match-a.v1")
        save_b = os.path.join(ROOT, "match-b.v1")
        host = Player("fleet-a", "host", "FLEET-B", save_a, 11, fast)
        guest = Player("fleet-b", "guest", None, save_b, 22, fast,
                       crash_at=30 if SCENARIO == "crash" else 0)
        players = [host, guest]
        started = time.monotonic()
        limit = 1200 if SCENARIO == "real" else 900
        crashed = restarted = False
        while time.monotonic() - started < limit:
            if host.done() and guest.done():
                break
            if SCENARIO == "crash" and not crashed and guest.proc.poll() is not None:
                crashed = True
                ok("the guest's app died mid-match, as told",
                   guest.proc.returncode == 3 and guest.ply() >= 30, guest.proc.returncode)
                ok("its save is on disk", os.path.getsize(save_b) > 0)
                guest = Player("fleet-b", "guest", None, save_b, 22, fast)
                players.append(guest)
            if SCENARIO == "restart" and not restarted and guest.ply() >= 40:
                restarted = True
                b.stop()
                time.sleep(2)
                ok("the guest's meshcored comes back", b.start())
                ok("as a new run", call(b.sock, "mesh.status")["run_id"] != run_b)
            time.sleep(0.2)
        elapsed = time.monotonic() - started
        dh, dg = host.done(), guest.done()
        ok("the host finished the match", dh is not None,
           host.find("TIMEOUT") or "ply %d" % host.ply())
        ok("the guest finished the match", dg is not None,
           guest.find("TIMEOUT") or "ply %d" % guest.ply())
        if SCENARIO == "crash":
            ok("the restarted guest resumed its match rather than starting another",
               guest.find("RESUMED") is not None and guest.find("ACCEPTED") is None)
        if dh and dg:
            ok("one won and the other lost", {dh["outcome"], dg["outcome"]} == {"win", "loss"},
               (dh["outcome"], dg["outcome"]))
            # A match won by sinking every ship ends on the final RESULT and the
            # reveals; no END is sent, so no end reason is recorded.
            ok("by sinking every ship, not by an END", dh["end"] == "0" and dg["end"] == "0",
               (dh["end"], dg["end"]))
            ok("each verified the other's fleet against its commitment",
               dh["verify"] == "1" and dg["verify"] == "1", (dh["verify"], dg["verify"]))
            ok("both hold the same record of every shot",
               dh["plies"] == dg["plies"] and dh["digest"] == dg["digest"],
               (dh["plies"], dh["digest"], dg["plies"], dg["digest"]))
            ok("and neither saw a violation", dh["violations"] == "0" and dg["violations"] == "0")
            print("     [%s] %s plies in %.0f s; airtime host %.1f s, guest %.1f s; "
                  "frames host %s, guest %s; air carried %d, lost %d, duplicated %d"
                  % (SCENARIO, dh["plies"], elapsed, int(dh["tx_airtime_ms"]) / 1000.0,
                     int(dg["tx_airtime_ms"]) / 1000.0, dh["sent"], dg["sent"], air.carried,
                     air.lost, air.duplicated), flush=True)
        sa = call(a.sock, "mesh.status")["counters"]
        sb = call(b.sock, "mesh.status")["counters"]
        ok("the match went as app datagrams both ways",
           sa["app_tx"] > 0 and sa["app_rx"] > 0 and sb["app_tx"] > 0 and sb["app_rx"] > 0,
           (sa["app_tx"], sa["app_rx"], sb["app_tx"], sb["app_rx"]))
        ok("a flood was answered with a receipt, so a route was learned",
           sa["app_receipts"] + sb["app_receipts"] >= 1)
        ok("and no chat message was sent by either",
           call(a.sock, "mesh.messages")["count"] == 0 and
           call(b.sock, "mesh.messages")["count"] == 0)
    finally:
        for p in players:
            p.stop()
        a.stop()
        b.stop()
    print("fleet_mp_e2e [%s]: %d failure(s)" % (SCENARIO, fails), flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
