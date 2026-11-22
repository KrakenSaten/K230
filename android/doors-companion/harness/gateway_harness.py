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

This is a bench tool. It is not part of the app and ships in no build.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import os
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
                    {"name": "wlan0", "operstate": "up", "carrier": True, "mac": "02:11:22:33:44:56", "ipv4": "192.168.1.20"}],
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
    "meshcored": FakeService("meshcored", {"mesh.status": {"state": "ready", "run_id": "harness", "nodes": 4}}),
}

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


def start():
    global proc
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
signal.signal(signal.SIGTERM, lambda *_: (stop(), os._exit(0)))
print("harness ready: gateway %s:%d, control %s:%d, state %s" % (HOST, PORT, HOST, CTL_PORT, ROOT), flush=True)
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass
finally:
    stop()
