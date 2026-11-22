#!/usr/bin/env python3
"""What the gatewayd end-to-end suites share: framing, fake pocketipc
services, the local control socket, starting and stopping the real service
(the test-hooks build), client identities and the TLS client.

Imported by tests/gatewayd_e2e_test.py (the M2 core) and
tests/gatewayd_mesh_e2e_test.py (the mesh group). The service binary is
argv[1] of the suite that imports this.

The client pins the unit's key the way the app must: it compares the SHA-256
of the SubjectPublicKeyInfo with the fingerprint of the pairing URI. That the
test client uses CERT_NONE underneath is the test's own business; the pin
check is what stands in for it (ssl has no hook for a custom verifier).

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import base64
import hashlib
import json
import os
import shutil
import signal
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

BIN = os.path.abspath(sys.argv[1])
HOST = "127.0.0.1"
ROOT = tempfile.mkdtemp(prefix="gatewayd-e2e-")
RUN = os.path.join(ROOT, "run")
STATE = os.path.join(ROOT, "state")
LOGS = os.path.join(ROOT, "log")
KEYS = os.path.join(ROOT, "keys")
SHADOW = os.path.join(ROOT, "shadow")
PASSWD = os.path.join(ROOT, "passwd")
GWDIR = os.path.join(STATE, "gatewayd")
for d in (RUN, STATE, LOGS, KEYS):
    os.makedirs(d)

checks = 0
failed = 0


def check(what, ok):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("  FAIL", what)


def free_port():
    s = socket.socket()
    s.bind((HOST, 0))
    p = s.getsockname()[1]
    s.close()
    return p


PORT = free_port()

# ---- framing -----------------------------------------------------------------


def send_frame(sock, obj):
    data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
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


# ---- fake pocketipc services ---------------------------------------------------


class FakeService(threading.Thread):
    """Answers every method from a table. mode: ok, stall (never answers),
    error (code 5)."""

    def __init__(self, name, results):
        super().__init__(daemon=True)
        self.path = os.path.join(RUN, name + ".sock")
        self.results = results
        self.mode = "ok"
        self.calls = []
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(self.path)
        self.sock.listen(8)
        self.alive = True
        self.conns = []
        self.start()

    def run(self):
        while self.alive:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.conns.append(c)
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            while True:
                req = recv_frame(c)
                self.calls.append((req.get("method"), req.get("params")))
                if self.mode == "stall":
                    continue
                if self.mode == "error":
                    send_frame(c, {"id": req["id"], "error": {"code": 5, "message": "SECRET upstream words"}})
                elif req.get("method") in self.results:
                    send_frame(c, {"id": req["id"], "result": self.results[req["method"]]})
                else:
                    send_frame(c, {"id": req["id"], "error": {"code": 1, "message": "unknown method"}})
        except (EOFError, OSError, ValueError):
            c.close()

    def stop(self):
        self.alive = False
        self.sock.close()
        for c in self.conns:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if os.path.exists(self.path):
            os.unlink(self.path)


SYSD = {
    "system.info": {"version": "0.4.0", "build": "test", "model": "T", "hostname": "doors", "release_file": "/etc/x", "api_version": 0},
    "system.status": {
        "uptime_s": 42,
        "memory": {"total_kb": 1},
        "network": [{"name": "wlan0", "mac": "02:11:22:33:44:55", "ipv4": "10.0.0.2", "operstate": "up"}],
        "services": [{"name": "sysd", "pid": 77, "running": True, "restarts": 0}],
        "power": {"status": "charging", "session_token": "LEAK"},
        "bluetooth": {"present": False},
    },
}
NETD = {"wifi.status": {"state": "connected", "ssid": "home", "bssid": "aa:bb:cc:dd:ee:ff", "passphrase": "LEAK", "ipv4": "10.0.0.2"}}
RADIOD = {"radio.status": {"state": "idle", "debug_key": "LEAK", "profile": {"sf": 8}}}
# The gateway's own connection to meshcored (M4.1) asks for these three and
# nothing else; the mesh group has its own suite and its own fake.
MESHD = {"mesh.status": {"state": "ready", "run_id": "abcd", "private_key": "LEAK"},
         "mesh.subscribe": {"subscribed": True}, "mesh.messages": {"messages": []}}

# ---- the local control socket --------------------------------------------------


class Ctl:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        self.s.connect(os.path.join(RUN, "gatewayd.sock"))
        self.next = 1
        self.events = []

    def call(self, method, params=None):
        rid = self.next
        self.next += 1
        req = {"id": rid, "method": method}
        if params is not None:
            req["params"] = params
        send_frame(self.s, req)
        while True:
            msg = recv_frame(self.s)
            if "event" in msg:
                self.events.append(msg)
                continue
            if msg.get("id") == rid:
                return msg

    def wait_event(self, name, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            for i, e in enumerate(self.events):
                if e["event"] == name:
                    return self.events.pop(i)
            self.s.settimeout(max(0.05, end - time.time()))
            try:
                msg = recv_frame(self.s)
            except (socket.timeout, TimeoutError):
                break
            finally:
                self.s.settimeout(5)
            if "event" in msg:
                self.events.append(msg)
        return None

    def close(self):
        self.s.close()


# ---- the service ----------------------------------------------------------------

proc = None


def start():
    global proc
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS,
               POCKETOS_LOG_STDERR="0")
    proc = subprocess.Popen([BIN, "--test-listen", HOST, "--test-port", str(PORT), "--test-shadow", SHADOW,
                             "--test-passwd", PASSWD],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    end = time.time() + 10
    while time.time() < end:
        if proc.poll() is not None:
            raise RuntimeError("gatewayd exited: " + proc.stderr.read().decode(errors="replace"))
        try:
            c = Ctl()
            c.call("gateway.info")
            return c
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("gatewayd did not come up")


def stop():
    global proc
    if proc is None:
        return ""
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        check("gatewayd stopped on SIGTERM", False)
    err = proc.stderr.read().decode(errors="replace")
    code = proc.returncode
    proc = None
    check("gatewayd exited 0 on SIGTERM (stderr: %r)" % err[-300:], code == 0)
    # A sanitizer build reports here; the plain build says nothing.
    check("no sanitizer report", "Sanitizer" not in err and "runtime error" not in err)
    return err


def port_open():
    s = socket.socket()
    s.settimeout(1)
    try:
        s.connect((HOST, PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


def set_password(on, passwd_field="x"):
    with open(PASSWD, "w") as f:
        f.write("root:%s:0:0:root:/root:/bin/sh\n" % passwd_field)
    with open(SHADOW, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\ndaemon:*:19000::::::\n" % ("$6$salt$notarealhash" if on else ""))


# ---- client identities -----------------------------------------------------------


def openssl(*args, data=None):
    return subprocess.run(["openssl"] + list(args), input=data, capture_output=True, check=True).stdout


def spki_fp_of_cert(pem_or_der, der=False):
    pub = openssl("x509", "-pubkey", "-noout", *(["-inform", "DER"] if der else []), data=pem_or_der)
    return hashlib.sha256(openssl("pkey", "-pubin", "-outform", "DER", data=pub)).digest()


class Identity:
    def __init__(self, name, keyopts=("ec", "ec_paramgen_curve:prime256v1")):
        self.key = os.path.join(KEYS, name + ".key")
        self.cert = os.path.join(KEYS, name + ".crt")
        cmd = ["req", "-x509", "-nodes", "-newkey", keyopts[0], "-keyout", self.key, "-out", self.cert,
               "-subj", "/CN=doors-companion", "-days", "36500"]
        if len(keyopts) > 1:
            cmd[5:5] = ["-pkeyopt", keyopts[1]]
        openssl(*cmd)
        with open(self.cert, "rb") as f:
            self.fp = spki_fp_of_cert(f.read())
        self.fp_hex = self.fp.hex()


_last_connect = [0.0]


def pace():
    """The unit admits four handshakes a second. The suite stays under that,
    so a refusal it sees is the refusal under test and not the budget."""
    wait = _last_connect[0] + 0.3 - time.time()
    if wait > 0:
        time.sleep(wait)
    _last_connect[0] = time.time()


class Client:
    """One TLS connection. pin: the unit's fingerprint to insist on."""

    def __init__(self, ident, pin=None, max_tls=None, session=None, chain=None, ctx=None, rcvbuf=None):
        if ctx is None:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            if max_tls is not None:
                ctx.maximum_version = max_tls
            if ident is not None:
                ctx.load_cert_chain(chain or ident.cert, ident.key)
        self.ctx = ctx
        pace()
        raw = socket.socket()
        if rcvbuf is not None:
            # A client that will not keep up: a small window, set before the
            # connection so that it is the one advertised.
            raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        raw.settimeout(5)
        raw.connect((HOST, PORT))
        self.s = ctx.wrap_socket(raw, session=session)
        self.s.settimeout(5)
        self.next = 1
        self.events = []
        self.server_fp = spki_fp_of_cert(self.s.getpeercert(True), der=True)
        if pin is not None and self.server_fp != pin:
            self.s.close()
            raise RuntimeError("the unit's key is not the pinned one")

    def send(self, method, params=None):
        rid = self.next
        self.next += 1
        req = {"id": rid, "method": method}
        if params is not None:
            req["params"] = params
        send_frame(self.s, req)
        return rid

    def read(self, rid):
        while True:
            msg = recv_frame(self.s)
            if "event" in msg:
                self.events.append(msg)
                continue
            if msg.get("id") == rid:
                return msg

    def call(self, method, params=None):
        return self.read(self.send(method, params))

    def hello(self):
        return self.call("gateway.hello", {"proto_min": 1, "proto_max": 1, "app": "e2e", "app_version": "0"})

    def closed(self, timeout=5):
        """True when the unit ends the connection without another frame."""
        self.s.settimeout(timeout)
        try:
            while True:
                msg = recv_frame(self.s)
                if "event" in msg:
                    self.events.append(msg)
                    continue
                return False
        except (EOFError, OSError, ssl.SSLError):
            return True

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def refused(ident, **kw):
    """A handshake the unit must not complete: with TLS 1.3 the client may
    think it is through, so the proof is that no request is ever answered."""
    try:
        c = Client(ident, **kw)
    except (OSError, ssl.SSLError, EOFError):
        return True
    try:
        c.send("gateway.hello", {"proto_min": 1, "proto_max": 1, "app": "e2e", "app_version": "0"})
        return c.closed()
    except (OSError, ssl.SSLError, EOFError):
        return True
    finally:
        c.close()


def err_code(msg):
    return msg.get("error", {}).get("code")


def parse_uri(uri):
    assert uri.startswith("doors-pair:1?"), uri
    q = dict(kv.split("=", 1) for kv in uri[len("doors-pair:1?"):].split("&"))
    pad = lambda s: s + "=" * (-len(s) % 4)
    return q["h"], int(q["p"]), base64.urlsafe_b64decode(pad(q["fp"])), q["t"]


def pair(ctl, ident, name, scopes=None, allow=True):
    """The whole pairing of one client. Returns the unit's fingerprint."""
    r = ctl.call("gateway.pair_begin")
    assert "result" in r, r
    _, _, fp, token = parse_uri(r["result"]["uri"])
    c = Client(ident, pin=fp)
    h = c.hello()
    assert h["result"]["paired"] is False, h
    rid = c.send("gateway.pair", {"token": token, "name": name})
    ev = ctl.wait_event("gateway.candidate")
    assert ev is not None and ev["data"]["fingerprint"] == ident.fp_hex, ev
    params = {"allow": allow}
    if scopes is not None:
        params["scopes"] = scopes
    d = ctl.call("gateway.pair_decide", params)
    reply = c.read(rid)
    c.close()
    return fp, d, reply
