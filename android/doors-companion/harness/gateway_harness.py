#!/usr/bin/env python3
"""The host gateway test environment for the app (CONTRACTS.md section 5).

Runs the real gatewayd (its test-hooks build, compiled by run-gateway.sh from
the reference checkout, which is only read) between fake pocketipc services,
and offers the unit's local control socket to the test on a loopback TCP
port, since a JVM on Windows cannot open a Unix socket inside WSL.

  python3 gateway_harness.py <gatewayd-testhooks> <tls port> <control port>

Control: one JSON object per line, {"method": ..., "params": ...}.
  gateway.*        passed to gatewayd.sock as the panel would (pair_begin,
                   pair_decide, clients, set_scopes, revoke, status)
  harness.stop     stop gatewayd (the unit goes away)
  harness.start    start it again on the same state directory
  harness.upstream {"service": "sysd", "mode": "ok" | "error"}
  harness.unit     {"method": ..., "params": ...}: one request on the real
                   shell's own socket, as a person or the bench at the unit
                   (shell.tap is the finger). Only with DOORS_SHELL_BIN.
  harness.shell    {"op": "start" | "stop" | "helpers"}: the real shell
  harness.mesh     {"op": ...}: the fake meshcored (FakeMesh below): message,
                   state, bulk, node, channel, activity, down, up, restart,
                   seed, demo; and for sending: send_mode, release, sends, auto

This is a bench tool. It is not part of the app and ships in no build.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import os
import random
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

BIN, PORT, CTL_PORT = os.path.abspath(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
# The real shell with PacketShark (the SDL simulator build) and its capture
# helper, when both are named: gatewayd then talks to it on its own socket,
# as on a unit. Without them the capture group is simply not there.
SHELL_BIN = os.environ.get("DOORS_SHELL_BIN", "")
CAPTURE_HELPER = os.environ.get("DOORS_CAPTURE_HELPER", "")
HOST = "127.0.0.1"
ROOT = tempfile.mkdtemp(prefix="doors-companion-gw-")
RUN, STATE, LOGS = (os.path.join(ROOT, d) for d in ("run", "state", "log"))
SHADOW, PASSWD = os.path.join(ROOT, "shadow"), os.path.join(ROOT, "passwd")
for d in (RUN, STATE, LOGS):
    os.makedirs(d)
with open(PASSWD, "w") as f:
    f.write("root:x:0:0:root:/root:/bin/sh\n")
with open(SHADOW, "w") as f:
    f.write("root:$6$salt$notarealhash:19000:0:99999:7:::\n")


def send_frame(sock, obj):
    data = json.dumps(obj).encode()
    sock.sendall(struct.pack(">I", len(data)) + data)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise EOFError("closed")
        buf += part
    return buf


def recv_frame(sock):
    (n,) = struct.unpack(">I", recv_exact(sock, 4))
    return json.loads(recv_exact(sock, n))


class FakeService(threading.Thread):
    def __init__(self, name, results):
        super().__init__(daemon=True)
        self.results, self.mode = results, "ok"
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(os.path.join(RUN, name + ".sock"))
        self.sock.listen(8)
        self.start()

    def run(self):
        while True:
            c, _ = self.sock.accept()
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            while True:
                req = recv_frame(c)
                result = self.results.get(req.get("method"))
                if self.mode == "error" or result is None:
                    send_frame(c, {"id": req["id"], "error": {"code": 5, "message": "busy"}})
                else:
                    result = result() if callable(result) else result
                    send_frame(c, {"id": req["id"], "result": result})
        except (EOFError, OSError, ValueError):
            c.close()


T0 = time.time()


def system_status():
    up = int(time.time() - T0) + 3600
    return {
        "uptime_s": up, "load": [0.42, 0.31, 0.25], "cpu_percent": 12.0 + (up % 7) * 3, "temperature_c": 49.5,
        "clock_set": True, "memory": {"total_kb": 1048576, "available_kb": 612000, "free_kb": 401000},
        "storage": [{"mount": "/", "total_bytes": 574000000, "avail_bytes": 211000000},
                    {"mount": "/data", "total_bytes": 29000000000, "avail_bytes": 27100000000}],
        "network": [{"name": "eth0", "operstate": "down", "carrier": None, "mac": "02:11:22:33:44:55", "ipv4": None},
                    {"name": "wlan0", "operstate": "up", "carrier": True, "mac": "02:11:22:33:44:56", "ipv4": "192.168.1.20"}]
                   # The simulator's PacketShark captures on its stand-in, and offers nothing else.
                   + ([{"name": "synth0", "operstate": "up", "carrier": True, "mac": None, "ipv4": None}] if SHELL_BIN else []),
        "power": {"source": "external", "external_online": True, "battery": None, "supplies": []},
        "services": [{"name": n, "pid": 100 + i, "running": True, "crashloop": False, "last_exit_code": None, "restarts": 0}
                     for i, n in enumerate(["gatewayd", "meshcored", "netd", "radiod", "sysd"])],
    }


SERVICES = {
    "sysd": FakeService("sysd", {
        "system.info": {"version": "0.4.0", "build": "harness", "model": "Canaan CanMV-K230 (host harness)", "kernel": "6.6.36",
                        "hostname": "doors", "cpus": 1, "os": "Buildroot 2025.02.1", "api_version": 0},
        "system.status": system_status}),
    "netd": FakeService("netd", {"wifi.status": {"state": "connected", "reason": None, "ssid": "workshop", "bssid": "aa:bb:cc:dd:ee:ff",
                                                 "signal_dbm": -58, "bars": 3, "ipv4": "192.168.1.20", "security": "wpa2"}}),
    "radiod": FakeService("radiod", {"radio.status": {"state": "idle", "frequency_hz": 869525000, "profile": {"sf": 11, "bw_hz": 250000}}}),
}


def mono():
    return int(time.monotonic() * 1000)  # CLOCK_MONOTONIC, the clock gatewayd stamps with


def key(i):
    return "%02x" % (0xa0 + i) + "%062x" % (0x5eed0000 + i * 7919)


NODES = [("Ola Nordmann", 1), ("Kari Fjell", 1), ("Ridge repeater", 2), ("Harbour room", 3), ("Weather mast", 4),
         ("Saga", 1), ("North relay", 2)]
CHANNELS = [{"channel": 0, "name": "Public", "channel_hash": "11", "key_bits": 128, "well_known": "public",
             "text_limit": 140, "ack_expected": False},
            {"channel": 1, "name": "#workshop", "channel_hash": "a4", "key_bits": 128, "text_limit": 140,
             "ack_expected": False}]
LINES = ["Anyone on the ridge tonight?", "Copy that, signal is good here.", "Repeater battery at 81 percent.",
         "Fog rolling in over the harbour.", "Moving to the north side, back in ten.", "Heard you through two relays.",
         "Antenna is up again.", "Coffee is on at the workshop."]


class FakeMesh:
    """A meshcored written from docs/api/mesh.md, as far as the gateway reads
    it: status with a run_id, a subscription, the two message rings (64
    direct, 64 channel), nodes, channels, identity, and the events. It holds
    no key and transmits nothing; a test, or the demo thread, tells it what
    "happened" on the air.

    It takes mesh.send the way the gateway's own suite models it
    (tests/gatewayd_send_e2e_test.py). send_mode says what becomes of one:
      ok       taken and answered
      refuse   answered with its own error 5, nothing taken
      swallow  taken (the message exists, its event is raised), never answered
      ignore   read and not taken, no answer; the service goes on talking, so
               the gateway keeps its connection
      drop     nothing taken, no answer, and not a byte: the gateway gives the
               connection up
      hold     taken and answered only on the op "release"
    `taken` counts what it was made to "transmit"; that number is what a test
    of at-most-once looks at. With auto on, what it took then fares as on a
    quiet evening: a direct message is acknowledged (not by Kari Fjell, who
    never answers), a channel message goes on the air and is heard back."""

    def __init__(self):
        self.lock = threading.RLock()
        self.path = os.path.join(RUN, "meshcored.sock")
        self.sock = None
        self.conns = []
        self.subs = []
        self.demo = False
        self.send_mode = "ok"
        self.auto = False
        self.held = []
        self.asked = 0   # mesh.send requests that reached it, taken or not
        self.taken = []  # what it was made to transmit, in order
        self.fresh()
        self.up()
        threading.Thread(target=self.demo_loop, daemon=True).start()

    def fresh(self):
        self.run_id = "%016x" % random.getrandbits(64)
        self.t0 = time.time()
        self.next_id = 1
        self.direct, self.chan = [], []
        self.nodes = [self.node(i) for i in range(len(NODES))]

    def node(self, i):
        name, kind = NODES[i]
        n = {"public_key": key(i), "node_hash": key(i)[:2], "name": name, "type": kind, "path_known": i % 3 != 2,
             "last_advert_timestamp": int(time.time()) - i * 40, "last_heard_mono_ms": mono() - i * 45000,
             "last_snr_db": 9.5 - i * 2.25, "last_rssi_dbm": -71 - i * 6, "advert_hops": i % 3,
             "advert_mono_ms": mono() - i * 45000}
        if n["path_known"]:
            n.update({"hops": i % 3, "direct": i % 3 == 0, "path_hex": "a2" * (i % 3)})
        if kind in (2, 4):
            n.update({"lat": 59.91 + i / 100.0, "lon": 10.75 + i / 100.0})
        return n

    # ---- the socket ----
    def up(self):
        with self.lock:
            if self.sock is not None:
                return
            if os.path.exists(self.path):
                os.unlink(self.path)
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.bind(self.path)
            self.sock.listen(8)
            threading.Thread(target=self.accept, args=(self.sock,), daemon=True).start()

    def down(self):
        with self.lock:
            if self.sock is None:
                return
            sock, self.sock = self.sock, None
            # A listener blocked in accept() only leaves it when it is woken.
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
            os.unlink(self.path)
            for c in self.conns:
                try:
                    c.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            self.conns, self.subs = [], []

    def accept(self, sock):
        while True:
            try:
                c, _ = sock.accept()
            except OSError:
                return
            with self.lock:
                self.conns.append(c)
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            while True:
                req = recv_frame(c)
                if req.get("method") == "mesh.send":
                    with self.lock:
                        self.asked += 1
                        mode = self.send_mode
                    if mode == "drop":
                        continue
                    if mode == "ignore":
                        self.emit("mesh.state", {"state": "ready", "reason": "none", "mono_ms": mono()})
                        continue
                    if mode == "swallow":
                        self.send(req.get("params") or {})
                        continue
                    if mode == "hold":
                        with self.lock:
                            self.held.append((c, req))
                        continue
                res = self.answer(req.get("method"), req.get("params") or {})
                with self.lock:
                    if isinstance(res, tuple):
                        send_frame(c, {"id": req["id"], "error": {"code": res[0], "message": res[1]}})
                    else:
                        send_frame(c, {"id": req["id"], "result": res})
                    if req.get("method") == "mesh.subscribe":
                        self.subs.append(c)
        except (EOFError, OSError, ValueError):
            c.close()

    def answer(self, m, p):
        with self.lock:
            if m == "mesh.status":
                return {"state": "ready", "reason": "none", "run_id": self.run_id, "uptime_s": int(time.time() - self.t0),
                        "nodes": len(self.nodes), "messages": len(self.direct) + len(self.chan)}
            if m == "mesh.subscribe":
                return {"subscribed": True}
            if m == "mesh.messages":
                all_ = sorted(self.direct + self.chan, key=lambda x: x["id"])
                return {"messages": all_, "count": len(all_), "total": len(all_), "persistent": False}
            if m == "mesh.identity":
                return {"public_key": key(90), "node_hash": key(90)[:2], "name": "Doors bench", "name_source": "stored",
                        "name_max": 31}
            if m == "mesh.nodes":
                return {"nodes": self.nodes, "count": len(self.nodes)}
            if m == "mesh.node":
                hit = [n for n in self.nodes if n["public_key"].startswith(str(p.get("node", "?")))]
                return hit[0] if len(hit) == 1 else (2, "no such node")
            if m == "mesh.channels":
                return {"channels": CHANNELS, "count": len(CHANNELS), "max": 8, "persistent": True}
            if m == "mesh.send":
                if self.send_mode == "refuse":
                    return (5, "8 messages are already waiting for an ACK")
                return self.send(p)
            return (1, "unknown method")

    def send(self, p):
        """Accepts one message the way meshcored does, or refuses it with a
        code of its own: from here on it exists, and its event is raised."""
        with self.lock:
            if "resend" in p:
                old = [x for x in self.direct if x["id"] == p["resend"]]
                if not old or old[0].get("direction") != "out" or old[0].get("state") != "no_ack":
                    return (2, "not an unacknowledged direct message of this run")
                m = old[0]
                m.update({"state": "sent_direct", "attempts": m.get("attempts", 1) + 1})
                self.taken.append(dict(p))
                self.emit("mesh.message", {"message": m})
                self.fare(m)
                return {"accepted": True, "message_id": m["id"], "route": "direct", "ack_timeout_ms": 8000,
                        "ack_expected": True, "resent": True}
            text = p.get("text", "")
            if "channel" in p:
                ch = [c for c in CHANNELS if c["channel"] == p["channel"]]
                if not ch:
                    return (2, "no such channel")
                if len(text.encode()) > ch[0]["text_limit"]:
                    return (2, "text too long for this channel")
                m = {"kind": "channel", "channel": ch[0]["channel"], "channel_name": ch[0]["name"],
                     "channel_hash": ch[0]["channel_hash"], "sender_name": "Doors bench", "text": "Doors bench: " + text,
                     "state": "sent_flood", "ack_expected": False}
                ring = self.chan
                res = {"accepted": True, "route": "flood", "channel": ch[0]["channel"], "ack_expected": False}
            else:
                peer = [n for n in self.nodes if n["public_key"] == p.get("to")]
                if not peer:
                    return (2, "no such node")
                if len(text.encode()) > 160:
                    return (2, "text too long")
                m = {"kind": "direct", "peer_public_key": peer[0]["public_key"], "peer_name": peer[0]["name"], "text": text,
                     "state": "sent_flood", "ack_expected": True, "attempts": 1}
                ring = self.direct
                res = {"accepted": True, "route": "flood", "ack_timeout_ms": 9000, "ack_expected": True}
            m.update({"id": self.next_id, "direction": "out", "timestamp": int(time.time()), "mono_ms": mono()})
            self.next_id += 1
            res["message_id"] = m["id"]
            ring.append(m)
            del ring[:-64]
            self.taken.append(dict(p))
            self.emit("mesh.message", {"message": m})
            self.fare(m)
            return res

    def fare(self, m):
        if not self.auto:
            return
        mid, run = m["id"], self.run_id

        def later(delay, **change):
            def go():
                time.sleep(delay)
                with self.lock:
                    if self.run_id == run:
                        self.state(change | {"id": mid})
            threading.Thread(target=go, daemon=True).start()

        if m["kind"] == "channel":
            later(0.6, more={"transmitted": True})
            later(2.2, more={"heard_back": 1, "heard_back_hops": 1})
        elif m.get("peer_public_key") == key(1):
            later(3.0, state="no_ack")
        else:
            later(1.4, state="acked", more={"ack_mono_ms": mono()})

    def release(self):
        with self.lock:
            held, self.held = self.held, []
        for c, req in held:
            res = self.send(req.get("params") or {})
            with self.lock:
                try:
                    if isinstance(res, tuple):
                        send_frame(c, {"id": req["id"], "error": {"code": res[0], "message": res[1]}})
                    else:
                        send_frame(c, {"id": req["id"], "result": res})
                except OSError:
                    pass

    def emit(self, event, data):
        with self.lock:
            for c in list(self.subs):
                try:
                    send_frame(c, {"event": event, "data": data})
                except OSError:
                    self.subs.remove(c)

    # ---- what "happens" ----
    def message(self, p):
        with self.lock:
            m = {"id": self.next_id, "direction": p.get("direction", "in"), "kind": p.get("kind", "direct"),
                 "text": p.get("text", "hello"), "timestamp": int(time.time()), "mono_ms": mono(),
                 "ack_expected": False}
            self.next_id += 1
            out = m["direction"] == "out"
            if m["kind"] == "channel":
                ch = CHANNELS[int(p.get("channel", 0))]
                m.update({"channel": ch["channel"], "channel_name": ch["name"], "channel_hash": ch["channel_hash"],
                          "state": "sent_flood" if out else "received"})
                if not out:
                    who = p.get("sender_name", NODES[int(p.get("peer", 0))][0])
                    m.update({"sender_name": who, "text": who + ": " + m["text"]})
                ring = self.chan
            else:
                i = int(p.get("peer", 0))
                m.update({"peer_public_key": key(i), "peer_name": NODES[i][0], "state": "sent_direct" if out else "received"})
                if out:
                    m.update({"ack_expected": True, "attempts": 1})
                ring = self.direct
            if not out:
                m.update({"snr_db": 7.25, "rssi_dbm": -88})
            m.update(p.get("more", {}))
            ring.append(m)
            del ring[:-64]
            self.emit("mesh.message", {"message": m})
            return {"id": m["id"], "run_id": self.run_id}

    def state(self, p):
        with self.lock:
            for m in self.direct + self.chan:
                if m["id"] == p["id"]:
                    m.update(p.get("more", {}))
                    if "state" in p:
                        m["state"] = p["state"]
                    self.emit("mesh.message", {"message": m})
                    return {"id": m["id"]}
            return {}

    def heard(self, i, reason="discovered"):
        with self.lock:
            n = self.nodes[i]
            n.update({"last_heard_mono_ms": mono(), "advert_mono_ms": mono(), "last_rssi_dbm": random.randint(-112, -64),
                      "last_snr_db": random.randint(-20, 44) / 4.0})
            self.emit("mesh.node", {"reason": reason, "node": n})

    def activity(self, rx=0, tx=0, kind="advert", result="ok"):
        for _ in range(rx):
            self.emit("mesh.activity", {"kind": "rx", "payload_type": kind, "bytes": random.randint(20, 180), "mono_ms": mono(),
                                        "rssi_dbm": -90, "snr_db": 6.0})
        for _ in range(tx):
            self.emit("mesh.activity", {"kind": "tx", "submit_id": 1, "result": result, "bytes": 64, "mono_ms": mono()})

    def seed(self):
        for i, line in enumerate(LINES):
            self.message({"kind": "channel", "channel": i % 2, "peer": i % 6, "text": line})
        self.message({"peer": 0, "text": "Are you at the cabin this weekend?"})
        a = self.message({"peer": 0, "direction": "out", "text": "Yes, from Friday evening."})
        self.state({"id": a["id"], "state": "acked", "more": {"ack_mono_ms": mono()}})
        self.message({"peer": 0, "text": "Good. Bring the spare antenna."})
        self.message({"peer": 1, "text": "Test from the fjell, how copy?"})
        b = self.message({"peer": 1, "direction": "out", "text": "Loud and clear."})
        self.state({"id": b["id"], "state": "no_ack"})
        c = self.message({"kind": "channel", "channel": 0, "direction": "out", "text": "Doors bench: Evening all."})
        self.state({"id": c["id"], "more": {"transmitted": True, "heard_back": 2, "heard_back_hops": 1}})

    def demo_loop(self):
        n = 0
        while True:
            time.sleep(1.0)
            if not self.demo or self.sock is None:
                continue
            n += 1
            self.activity(rx=random.choice([0, 1, 1, 2, 3, 6]), kind=random.choice(["advert", "group_text", "text", "ack", "path"]))
            if n % 5 == 0:
                self.activity(tx=1, result=random.choice(["ok", "ok", "ok", "tx_failed"]))
            if n % 7 == 0:
                self.heard(random.randrange(len(NODES)))
            if n % 11 == 0:
                self.activity(rx=1, kind="group_text")
                self.message({"kind": "channel", "channel": random.randrange(2), "peer": random.randrange(6), "text": random.choice(LINES)})
            if n % 23 == 0:
                self.activity(rx=1, kind="text")
                self.message({"peer": random.randrange(2), "text": random.choice(LINES)})

    def control(self, p):
        op = p.get("op")
        if op == "message":
            return self.message(p)
        if op == "state":
            return self.state(p)
        if op == "bulk":
            for i in range(int(p["count"])):
                self.message({"kind": p.get("kind", "channel"), "peer": i % 6, "text": "%s %d" % (p.get("text", "bulk"), i)})
        elif op == "node":
            self.heard(int(p.get("peer", 0)), p.get("reason", "discovered"))
        elif op == "channel":
            self.emit("mesh.channel", {"reason": p.get("reason", "added"), "channel": p["channel"]})
        elif op == "activity":
            self.activity(int(p.get("rx", 0)), int(p.get("tx", 0)), p.get("kind", "advert"), p.get("result", "ok"))
        elif op == "down":
            self.down()
        elif op == "up":
            self.up()
        elif op == "restart":
            self.down()
            with self.lock:
                self.fresh()
            self.up()
        elif op == "seed":
            self.seed()
        elif op == "demo":
            self.demo = bool(p.get("on", True))
        elif op == "send_mode":
            with self.lock:
                self.send_mode = p.get("mode", "ok")
        elif op == "release":
            self.release()
        elif op == "auto":
            self.auto = bool(p.get("on", True))
        elif op == "sends":
            with self.lock:
                if p.get("reset"):
                    self.asked, self.taken = 0, []
                return {"asked": self.asked, "taken": self.taken, "run_id": self.run_id}
        else:
            raise ValueError("unknown mesh op %r" % (op,))
        return {"run_id": self.run_id}


MESH = FakeMesh()

proc = None
lock = threading.Lock()


def ctl(method, params=None):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    try:
        s.connect(os.path.join(RUN, "gatewayd.sock"))
        req = {"id": 1, "method": method}
        if params is not None:
            req["params"] = params
        send_frame(s, req)
        while True:
            msg = recv_frame(s)
            if msg.get("id") == 1:
                return msg
    finally:
        s.close()


shell_proc = None


def unit(method, params=None):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    try:
        s.connect(os.path.join(RUN, "shell.sock"))
        req = {"id": 1, "method": method}
        if params is not None:
            req["params"] = params
        send_frame(s, req)
        while True:
            msg = recv_frame(s)
            if msg.get("id") == 1:
                return msg
    finally:
        s.close()


def shell_start():
    global shell_proc
    if not SHELL_BIN or (shell_proc is not None and shell_proc.poll() is None):
        return
    path = os.path.join(RUN, "shell.sock")
    if os.path.exists(path):
        os.unlink(path)
    conf = os.path.join(ROOT, "conf")
    os.makedirs(conf, exist_ok=True)
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS,
               POCKETOS_CONFIG_DIR=conf, SDL_VIDEODRIVER="dummy", POCKETOS_CAPTURE_SOURCE="synth:200:1",
               POCKETOS_CAPTURE_HELPER=CAPTURE_HELPER)
    shell_proc = subprocess.Popen([SHELL_BIN], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 15
    while time.time() < end:
        try:
            unit("shell.info")
            unit("shell.unlock")  # a person at the unit: the simulator starts behind its lock
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("the shell did not come up")


def shell_stop():
    global shell_proc
    if shell_proc is not None:
        shell_proc.terminate()
        try:
            shell_proc.wait(5)
        except subprocess.TimeoutExpired:
            shell_proc.kill()
            shell_proc.wait()
        shell_proc = None


def start():
    global proc
    shell_start()
    if proc is not None and proc.poll() is None:
        return  # already running: a second one would leave the first without anyone to stop it
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS, POCKETOS_LOG_STDERR="0")
    proc = subprocess.Popen([BIN, "--test-listen", HOST, "--test-port", str(PORT), "--test-shadow", SHADOW, "--test-passwd", PASSWD], env=env)
    end = time.time() + 10
    while time.time() < end:
        try:
            ctl("gateway.info")
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("gatewayd did not come up")


def stop():
    global proc
    if proc is not None:
        proc.send_signal(signal.SIGTERM)
        proc.wait(5)
        proc = None


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            try:
                req = json.loads(line)
                m, p = req.get("method", ""), req.get("params")
                with lock:
                    if m == "harness.stop":
                        stop()
                        out = {"result": {}}
                    elif m == "harness.start":
                        start()
                        out = {"result": {}}
                    elif m == "harness.upstream":
                        SERVICES[p["service"]].mode = p["mode"]
                        out = {"result": {}}
                    elif m == "harness.mesh":
                        out = {"result": MESH.control(p)}
                    elif m == "harness.unit" and SHELL_BIN:
                        out = unit(p["method"], p.get("params"))
                        out = {"result": out.get("result", {})} if "result" in out else {"result": {"error": out.get("error")}}
                    elif m == "harness.shell" and SHELL_BIN:
                        if p["op"] == "stop":
                            shell_stop()
                        elif p["op"] == "start":
                            shell_start()
                        n = len(subprocess.run(["pgrep", "-f", CAPTURE_HELPER + " session"], capture_output=True, text=True).stdout.split())
                        out = {"result": {"running": shell_proc is not None, "helpers": n}}
                    elif m.startswith("gateway."):
                        out = ctl(m, p)
                    else:
                        out = {"error": {"code": 1, "message": "unknown"}}
            except Exception as e:  # the bench's own failure, told to the test
                out = {"error": {"code": -1, "message": repr(e)}}
            self.wfile.write((json.dumps(out) + "\n").encode())
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


start()
srv = Server((HOST, CTL_PORT), Handler)
signal.signal(signal.SIGTERM, lambda *_: (stop(), shell_stop(), os._exit(0)))
print("harness ready: gateway %s:%d, control %s:%d, state %s" % (HOST, PORT, HOST, CTL_PORT, ROOT), flush=True)
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass
finally:
    stop()
    shell_stop()
