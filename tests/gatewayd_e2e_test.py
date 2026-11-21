#!/usr/bin/env python3
"""gatewayd end to end: the real service (the test-hooks build) between fake
pocketipc services and real TLS clients, on loopback.

  python3 tests/gatewayd_e2e_test.py <path to gatewayd-testhooks>

What is driven: the device-password gate, "closed until asked", pairing
(token, three failures, approval and refusal on the local socket, a reused
token), authenticated sessions, every way a handshake must fail (no
certificate, TLS 1.2, RSA, P-384, an unknown key, two certificates), scopes,
revocation, malformed and oversized frames, limits, upstream failures,
restart with persisted, damaged and missing state, and what the log holds.

The client pins the unit's key the way the app must: it compares the SHA-256
of the SubjectPublicKeyInfo with the fingerprint of the pairing URI. That the
test client uses CERT_NONE underneath is the test's own business; the pin
check below is what stands in for it (ssl has no hook for a custom verifier).

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
MESHD = {"mesh.status": {"state": "ready", "run_id": "abcd", "private_key": "LEAK"}}

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

    def __init__(self, ident, pin=None, max_tls=None, session=None, chain=None, ctx=None):
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
        raw = socket.create_connection((HOST, PORT), timeout=5)
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


# ==================================================================================

sysd = FakeService("sysd", SYSD)
netd = FakeService("netd", NETD)
radiod = FakeService("radiod", RADIOD)
meshd = FakeService("meshcored", MESHD)

A = Identity("a")
B = Identity("b")
C = Identity("c")
STRANGER = Identity("stranger")
RSA = Identity("rsa", ("rsa:2048",))
P384 = Identity("p384", ("ec", "ec_paramgen_curve:secp384r1"))

try:
    # ---- 1. no device password: closed, and pairing cannot begin ---------------
    print("gatewayd e2e")
    set_password(False)
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("no password: not listening", st["listening"] is False and st["closed"] == "password_required")
    check("no password: the port is closed", not port_open())
    r = ctl.call("gateway.pair_begin")
    check("no password: pair_begin is refused by policy", err_code(r) == 3 and r["error"]["message"] == "password_required")
    check("no password: still closed after the attempt", not port_open())
    info = ctl.call("gateway.info")["result"]
    check("gateway.info names the service and protocol 1", info["service"] == "gatewayd" and info["proto"] == 1)
    check("an unknown local method is code 1", err_code(ctl.call("gateway.nope")) == 1)
    ctl.close()
    stop()

    # ---- 2. a password, nobody paired: closed until asked ----------------------
    set_password(True)
    ctl = start()
    ctl.call("gateway.subscribe")
    st = ctl.call("gateway.status")["result"]
    check("nobody paired: closed with no_clients", st["listening"] is False and st["closed"] == "no_clients")
    check("nobody paired: the port is closed", not port_open())
    device_id = st["device_id"]
    check("state directory is 0700", stat.S_IMODE(os.stat(GWDIR).st_mode) == 0o700)
    check("identity file is 0600", stat.S_IMODE(os.stat(os.path.join(GWDIR, "identity.v1")).st_mode) == 0o600)

    # ---- 3. the pairing window: token, failures, refusal ------------------------
    r = ctl.call("gateway.pair_begin")
    host, port, unit_fp, token = parse_uri(r["result"]["uri"])
    check("the URI names the listening address and port", host == HOST and port == PORT)
    check("the URI's fingerprint is the device id", unit_fp.hex() == device_id)
    check("a second window cannot be opened over the first", err_code(ctl.call("gateway.pair_begin")) == 5)
    check("the port opens with the window", port_open())
    # The unit's key as OpenSSL's own tools see it is the fingerprint the
    # service computed: two implementations, one value.
    c = Client(STRANGER)
    check("the unit's certificate carries the pinned key", c.server_fp == unit_fp)
    check("TLS 1.3 was negotiated", c.s.version() == "TLSv1.3")
    h = c.hello()
    check("a candidate's hello says unpaired and gives no scopes",
          h["result"]["paired"] is False and "scopes" not in h["result"] and "boot_id" not in h["result"])
    check("a candidate cannot call a status method (code 1)", err_code(c.call("system.status")) == 1)
    check("a candidate cannot ping (code 1)", err_code(c.call("gateway.ping")) == 1)
    # A second unknown key while one is at the door.
    check("a second candidate is refused while one is connected", refused(B))
    r = c.call("gateway.pair", {"token": "AAAAAAAAAAAAAAAAAAAAAA", "name": "x"})
    check("a wrong token is refused (code 3)", err_code(r) == 3)
    check("and the candidate is dropped", c.closed())
    c.close()
    for bad in ({"token": token[:-1], "name": "x"}, {"name": "x"}):
        c = Client(STRANGER, pin=unit_fp)
        c.hello()
        check("a malformed or missing token is refused", err_code(c.call("gateway.pair", bad)) == 3)
        c.close()
    ev = ctl.wait_event("gateway.pairing")
    check("three wrong tokens close the window", ev is not None and ev["data"]["state"] == "closed")
    time.sleep(0.3)
    check("and the port with it", not port_open())
    check("nobody was paired", ctl.call("gateway.clients")["result"]["clients"] == [])
    old_token = token

    # Candidates that connect and leave without a token do not use the window
    # up: a stranger on the network cannot close it for the owner.
    r = ctl.call("gateway.pair_begin")
    for _ in range(5):
        c = Client(STRANGER, pin=unit_fp)
        c.hello()
        c.close()
        time.sleep(0.2)
    check("five candidates that left did not close the window", ctl.call("gateway.status")["result"]["pairing"] is True)
    ctl.call("gateway.pair_cancel")
    ctl.events.clear()

    # Refusal on the unit.
    fp, d, reply = pair(ctl, A, "Phone A", allow=False)
    check("a refused candidate gets code 3", err_code(reply) == 3 and d["result"]["paired"] is False)
    check("refusal pairs nobody", ctl.call("gateway.clients")["result"]["clients"] == [])
    time.sleep(0.3)
    check("the port closes after a refusal", not port_open())
    check("a decision with nobody waiting is refused", err_code(ctl.call("gateway.pair_decide", {"allow": True})) == 3)

    # A token from an earlier window opens nothing in a new one.
    r = ctl.call("gateway.pair_begin")
    _, _, _, token = parse_uri(r["result"]["uri"])
    check("each window has its own token", token != old_token)
    c = Client(A, pin=unit_fp)
    c.hello()
    check("a reused token is refused", err_code(c.call("gateway.pair", {"token": old_token, "name": "x"})) == 3)
    c.close()
    # The right token with a name that is not one.
    c = Client(A, pin=unit_fp)
    c.hello()
    check("a control character in the name is refused", err_code(c.call("gateway.pair", {"token": token, "name": "a\nb"})) == 3)
    c.close()
    # Cancel: the window and its candidate go.
    c = Client(A, pin=unit_fp)
    c.hello()
    ctl.call("gateway.pair_cancel")
    check("cancel drops the candidate", c.closed())
    c.close()
    time.sleep(0.3)
    check("cancel closes the port", not port_open())
    # A right token, then the candidate leaves before the decision.
    r = ctl.call("gateway.pair_begin")
    _, _, _, token = parse_uri(r["result"]["uri"])
    c = Client(A, pin=unit_fp)
    c.hello()
    c.send("gateway.pair", {"token": token, "name": "Phone A"})
    check("the unit is told a candidate waits", ctl.wait_event("gateway.candidate") is not None)
    c.close()
    time.sleep(0.5)
    check("approval after the candidate left pairs nobody", err_code(ctl.call("gateway.pair_decide", {"allow": True})) == 3)
    check("still nobody paired", ctl.call("gateway.clients")["result"]["clients"] == [])
    ctl.events.clear()

    # ---- 4. pairing, and an authenticated session --------------------------------
    r = ctl.call("gateway.pair_begin")
    _, _, _, token = parse_uri(r["result"]["uri"])
    c = Client(A, pin=unit_fp)
    c.hello()
    rid = c.send("gateway.pair", {"token": token, "name": "Phone A"})
    ev = ctl.wait_event("gateway.candidate")
    check("the candidate event carries the phone's own fingerprint and name",
          ev is not None and ev["data"] == {"fingerprint": A.fp_hex, "name": "Phone A"})
    check("an invalid scope set is refused locally and decides nothing",
          err_code(ctl.call("gateway.pair_decide", {"allow": True, "scopes": ["mesh_send"]})) == 2)
    d = ctl.call("gateway.pair_decide", {"allow": True, "scopes": ["status"]})
    reply = c.read(rid)
    check("approval pairs the candidate", d["result"]["paired"] is True and reply["result"] == {"paired": True, "scopes": ["status"]})
    check("the pairing connection ends", c.closed())
    c.close()
    lst = ctl.call("gateway.clients")["result"]["clients"]
    check("the client is listed with its scopes", len(lst) == 1 and lst[0]["fingerprint"] == A.fp_hex and lst[0]["scopes"] == ["status"])
    check("paired list is 0600", stat.S_IMODE(os.stat(os.path.join(GWDIR, "paired.v1")).st_mode) == 0o600)
    check("the port stays open with a paired client", port_open())

    a = Client(A, pin=unit_fp)
    check("before gateway.hello nothing else is answered (code 3)", err_code(a.call("gateway.ping")) == 3)
    check("and the connection ends", a.closed())
    a.close()

    a = Client(A, pin=unit_fp)
    check("the session is not a resumed one", a.s.session_reused is False)
    h = a.hello()["result"]
    check("hello: protocol 1, paired, this unit", h["proto"] == 1 and h["paired"] is True and h["device_id"] == device_id)
    check("hello: the scopes granted on the unit", h["scopes"] == ["status"])
    check("hello: capabilities of this build", h["capabilities"] == ["status"])
    check("hello: clock facts and limits", "boot_id" in h and "clock_set" in h and h["limits"]["frame_in"] == 65536)
    check("a second hello is refused", err_code(a.call("gateway.hello", {"proto_min": 1, "proto_max": 1, "app": "e", "app_version": "0"})) == 3)
    a.close()
    a = Client(A, pin=unit_fp)
    r = a.call("gateway.hello", {"proto_min": 2, "proto_max": 3, "app": "e", "app_version": "0"})
    check("no common protocol version is code 8", err_code(r) == 8 and a.closed())
    a.close()
    a = Client(A, pin=unit_fp)
    check("hello with bad params is code 2", err_code(a.call("gateway.hello", {"proto_min": "1"})) == 2)
    a.close()

    a = Client(A, pin=unit_fp)
    a.hello()
    r = a.call("gateway.ping")
    check("ping answers, with the unit's monotonic clock", r.get("result") == {} and isinstance(r.get("mono_ms"), (int, float)))
    r = a.call("system.status")["result"]
    check("system.status passes the listed fields", r["uptime_s"] == 42 and r["memory"] == {"total_kb": 1})
    check("system.status drops the MAC", r["network"] == [{"name": "wlan0", "ipv4": "10.0.0.2", "operstate": "up"}])
    check("system.status drops the pid", r["services"] == [{"name": "sysd", "running": True, "restarts": 0}])
    check("system.status drops unlisted and secret-named fields", "bluetooth" not in r and r["power"] == {"status": "charging"})
    r = a.call("system.info")["result"]
    check("system.info drops unlisted fields", "release_file" not in r and r["model"] == "T")
    r = a.call("wifi.status")["result"]
    check("wifi.status drops the BSSID and anything secret", r == {"state": "connected", "ssid": "home", "ipv4": "10.0.0.2"})
    check("radio.status loses a secret-named field", a.call("radio.status")["result"] == {"state": "idle", "profile": {"sf": 8}})
    check("mesh.status loses a secret-named field", a.call("mesh.status")["result"] == {"state": "ready", "run_id": "abcd"})
    check("no LEAK anywhere", all("LEAK" not in json.dumps(a.call(m)) for m in ("system.status", "wifi.status", "radio.status", "mesh.status")))
    check("params on a status method are code 2", err_code(a.call("system.status", {"x": 1})) == 2)
    calls_before = len(sysd.calls)
    for m in ("system.reboot", "system.poweroff", "shell.key", "shell.open", "mesh.send", "mesh.channel_add", "wifi.connect",
              "radio.send", "storage.expand", "gateway.pair", "gateway.pair_begin", "gateway.revoke", "nonsense", "system.logs"):
        check("%s does not exist for a client (code 1)" % m, err_code(a.call(m, {})) == 1)
    check("none of them reached a service", len(sysd.calls) == calls_before)
    check("no client parameter is ever passed upstream", all(p is None for _, p in sysd.calls + netd.calls + radiod.calls + meshd.calls))
    check("only allowlisted methods were called upstream",
          {m for m, _ in sysd.calls + netd.calls + radiod.calls + meshd.calls} <=
          {"system.info", "system.status", "wifi.status", "radio.status", "mesh.status"})

    # ---- 5. upstream failures ------------------------------------------------------
    sysd.mode = "error"
    r = a.call("system.status")
    check("an upstream error keeps its code and loses its words", err_code(r) == 5 and "SECRET" not in json.dumps(r))
    sysd.mode = "stall"
    t0 = time.time()
    r = a.call("system.status")
    check("a silent upstream is code 9 within its deadline", err_code(r) == 9 and time.time() - t0 < 2)
    sysd.mode = "ok"
    t0 = time.time()
    check("a service that just failed is not asked again at once", err_code(a.call("system.status")) == 9 and time.time() - t0 < 0.2)
    time.sleep(1.1)
    check("and answers again once it does", "result" in a.call("system.status"))
    netd.stop()
    check("an absent upstream is code 9", err_code(a.call("wifi.status")) == 9)
    check("the session survives upstream failures", "result" in a.call("gateway.ping"))

    # ---- 6. scopes ------------------------------------------------------------------
    check("scopes of an unknown client cannot be set", err_code(ctl.call("gateway.set_scopes", {"client": B.fp_hex, "scopes": []})) == 2)
    check("an invalid set is refused", err_code(ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["capture_control"]})) == 2)
    check("an unknown scope is refused", err_code(ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["admin"]})) == 2)
    check("the refusals changed nothing", "result" in a.call("system.status"))
    ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["mesh_read", "capture_view"]})
    r = a.call("system.status")
    check("without the status scope a status method is code 7, at once", err_code(r) == 7)
    check("the client was told its scopes", any(e["event"] == "gateway.scopes" and e["data"]["scopes"] == ["mesh_read", "capture_view"] for e in a.events))
    for m in ("system.info", "wifi.status", "radio.status", "mesh.status"):
        check("%s needs the status scope too" % m, err_code(a.call(m)) == 7)
    check("ping needs no scope", "result" in a.call("gateway.ping"))
    check("a method that does not exist is still code 1, not 7", err_code(a.call("system.reboot")) == 1)
    ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": []})
    check("no scopes at all: code 7", err_code(a.call("system.status")) == 7)
    ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["status", "mesh_read", "mesh_send", "capture_view", "capture_control"]})
    check("all five scopes can be granted, and status works again", "result" in a.call("system.status"))
    a.close()

    # ---- 7. handshakes that must fail -----------------------------------------------
    check("no client certificate", refused(None))
    check("TLS 1.2 at most", refused(A, max_tls=ssl.TLSVersion.TLSv1_2))
    check("an RSA key", refused(RSA))
    check("a P-384 key", refused(P384))
    check("an unknown P-256 key outside a window", refused(STRANGER))
    chain = os.path.join(KEYS, "chain.pem")
    with open(chain, "wb") as out:
        for part in (A.cert, STRANGER.cert):
            with open(part, "rb") as f:
                out.write(f.read())
    check("the paired key with a second certificate behind it", refused(A, chain=chain))
    pace()
    raw = socket.create_connection((HOST, PORT), timeout=5)
    raw.sendall(b'\x00\x00\x00\x2a{"id":1,"method":"gateway.ping","params":{}}')
    raw.settimeout(5)
    try:
        got = raw.recv(64)
    except OSError:
        got = b""
    check("plaintext gets no protocol answer", b"result" not in got and b"mono_ms" not in got)
    raw.close()
    time.sleep(1.2)  # the handshake budget refills
    a = Client(A, pin=unit_fp)
    check("the paired client still gets in after all of that", "result" in a.hello())
    first_session, first_ctx = a.s.session, a.ctx
    a.close()
    a = Client(A, pin=unit_fp, session=first_session, ctx=first_ctx)
    check("offering the earlier session resumes nothing", a.s.session_reused is False and "result" in a.hello())

    # ---- 8. malformed and oversized frames -------------------------------------------
    def frame_closes(what, payload=None, rawbytes=None):
        c2 = Client(B, pin=unit_fp)
        c2.hello()
        if rawbytes is not None:
            c2.s.sendall(rawbytes)
        else:
            send_frame(c2.s, payload)
        check(what, c2.closed())
        c2.close()
        time.sleep(0.3)

    # B is paired for this and for the limits below.
    _, d, _ = pair(ctl, B, "Phone B")
    check("a second client pairs with the default scopes", d["result"]["paired"] is True and
          [x["scopes"] for x in ctl.call("gateway.clients")["result"]["clients"] if x["fingerprint"] == B.fp_hex] == [["status", "mesh_read", "mesh_send"]])
    frame_closes("invalid JSON closes the connection", b"{not json")
    frame_closes("a JSON array closes the connection", b"[1,2,3]")
    frame_closes("an id that is not an integer closes it", b'{"id":"1","method":"gateway.ping"}')
    frame_closes("params that are not an object close it", b'{"id":1,"method":"gateway.ping","params":[1]}')
    frame_closes("invalid UTF-8 closes it", b'{"id":1,"method":"gateway.ping\xff"}')
    frame_closes("a NUL in the frame closes it", b'{"id":1,"method":"gateway.ping"}\x00')
    frame_closes("a zero-length frame closes it", rawbytes=b"\x00\x00\x00\x00")
    frame_closes("a frame one byte over the limit closes it", rawbytes=struct.pack(">I", 65537) + b"x" * 100)
    frame_closes("the largest announced length closes it", rawbytes=b"\xff\xff\xff\xff")
    c2 = Client(B, pin=unit_fp)
    c2.hello()
    big = {"id": 5, "method": "gateway.ping", "params": {}, "pad": "x" * 65000}
    check("a frame just under the limit is read and answered", "result" in c2.read(send_frame(c2.s, big) or 5))
    deep = b'{"id":6,"method":"gateway.ping","params":' + b"[" * 3000 + b"]" * 3000 + b"}"
    send_frame(c2.s, deep)
    check("deep nesting does not crash the service", c2.closed() or True)
    c2.close()
    check("the service is still there", "result" in ctl.call("gateway.status"))
    check("and the other session never noticed", "result" in a.call("gateway.ping"))

    # ---- 9. limits ---------------------------------------------------------------------
    time.sleep(1.2)
    a2 = Client(A, pin=unit_fp)
    check("the same client connecting again is admitted", "result" in a2.hello())
    check("and replaces its earlier connection", a.closed())
    a.close()
    b = Client(B, pin=unit_fp)
    b.hello()
    _, d, _ = pair(ctl, C, "Phone C", scopes=["status"])
    check("a third client pairs", d["result"]["paired"] is True)
    time.sleep(1.2)
    check("a third session at once is refused", refused(C))
    check("the two sessions are untouched", "result" in a2.call("gateway.ping") and "result" in b.call("gateway.ping"))
    st = ctl.call("gateway.status")["result"]
    check("status counts clients and sessions", st["clients"] == 3 and st["sessions"] == 2)
    connected = {x["name"]: x["connected"] for x in ctl.call("gateway.clients")["result"]["clients"]}
    check("the client list says who is connected", connected == {"Phone A": True, "Phone B": True, "Phone C": False})
    # Requests faster than the budget are answered with code 10, not dropped.
    ids = [b.send("gateway.ping") for _ in range(120)]
    codes = [err_code(b.read(i)) for i in ids]
    check("a burst is rate limited (code 10) and every request is answered", codes.count(10) >= 60 and codes.count(None) >= 30)
    time.sleep(1.5)
    check("the budget refills", "result" in b.call("gateway.ping"))
    # Connections that never speak TLS do not take the service down.
    idle = []
    for _ in range(20):
        try:
            idle.append(socket.create_connection((HOST, PORT), timeout=2))
        except OSError:
            pass
    check("silent connections do not disturb a session", "result" in a2.call("gateway.ping"))
    check("nor the local socket", "result" in ctl.call("gateway.status"))
    for s_ in idle:
        s_.close()
    time.sleep(1.5)
    b.close()

    # ---- 10. revocation -------------------------------------------------------------------
    check("revoking an unknown client is refused", err_code(ctl.call("gateway.revoke", {"client": STRANGER.fp_hex})) == 2)
    check("revoking with a prefix is refused", err_code(ctl.call("gateway.revoke", {"client": A.fp_hex[:16]})) == 2)
    check("revoke succeeds", "result" in ctl.call("gateway.revoke", {"client": A.fp_hex}))
    check("the live session is told and closed", a2.closed() and any(e["event"] == "gateway.revoked" for e in a2.events))
    check("at once: no session of it is left when revoke returns", ctl.call("gateway.status")["result"]["sessions"] == 0)
    a2.close()
    time.sleep(1.2)
    check("the revoked key no longer gets through a handshake", refused(A))
    with open(os.path.join(GWDIR, "paired.v1")) as f:
        check("the revoked key is gone from storage", A.fp_hex not in f.read())
    check("the others are untouched", {x["name"] for x in ctl.call("gateway.clients")["result"]["clients"]} == {"Phone B", "Phone C"})
    time.sleep(1.2)
    b = Client(B, pin=unit_fp)
    check("another client still works", "result" in b.hello())
    check("a client can remove itself", b.call("gateway.unpair").get("result") == {} and b.closed())
    b.close()
    time.sleep(1.2)
    check("and is then a stranger", refused(B))
    check("one client is left", [x["name"] for x in ctl.call("gateway.clients")["result"]["clients"]] == ["Phone C"])

    # ---- 11. restart: what persists ------------------------------------------------------------
    ctl.close()
    stop()
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("after a restart: the same identity", st["device_id"] == device_id)
    check("after a restart: the paired client, and listening", st["clients"] == 1 and st["listening"] is True)
    c3 = Client(C, pin=unit_fp)
    check("the paired client gets in without pairing again", c3.hello()["result"]["scopes"] == ["status"])
    check("the revoked one still does not", refused(A))
    c3.close()

    # ---- 12. the password is removed while running ----------------------------------------------
    time.sleep(1.2)
    c3 = Client(C, pin=unit_fp)
    c3.hello()
    set_password(False)
    check("removing the device password drops the session", c3.closed(timeout=9))
    c3.close()
    st = ctl.call("gateway.status")["result"]
    check("and closes the listener", st["listening"] is False and st["closed"] == "password_required" and not port_open())
    set_password(True, passwd_field="")
    time.sleep(6)
    check("an empty field in passwd is no password, whatever shadow holds", not port_open())
    set_password(True)
    end = time.time() + 9
    while time.time() < end and not port_open():
        time.sleep(0.3)
    check("setting it again reopens the listener", port_open())
    ctl.close()
    log = stop()

    # ---- 13. damaged and missing state: closed, and nothing regenerated --------------------------
    paired = os.path.join(GWDIR, "paired.v1")
    ident = os.path.join(GWDIR, "identity.v1")
    shutil.copy(paired, paired + ".keep")
    shutil.copy(ident, ident + ".keep")
    with open(paired, "a") as f:
        f.write("client zz\n")
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("a damaged paired list: state_fault, closed", st["state_fault"] is True and st["listening"] is False and st["closed"] == "state_fault")
    check("and nobody gets in", not port_open())
    check("and no pairing can begin", err_code(ctl.call("gateway.pair_begin")) == 4)
    with open(paired) as f:
        check("the damaged file was not rewritten", f.read().endswith("client zz\n"))
    ctl.close()
    stop()
    shutil.copy(paired + ".keep", paired)

    os.unlink(ident)
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("a lost identity beside a paired list: identity_fault, closed",
          st["identity_fault"] is True and st["listening"] is False and "device_id" not in st)
    check("no new identity was made", not os.path.exists(ident))
    check("and no pairing can begin", err_code(ctl.call("gateway.pair_begin")) == 4)
    ctl.close()
    stop()

    shutil.copy(ident + ".keep", ident)
    os.chmod(ident, 0o644)
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("an identity file others can read: identity_fault", st["identity_fault"] is True and st["listening"] is False)
    ctl.close()
    stop()
    os.chmod(ident, 0o600)
    ctl = start()
    st = ctl.call("gateway.status")["result"]
    check("with the state restored the unit is itself again", st["device_id"] == device_id and st["listening"] is True)

    # A second gatewayd beside the first is refused.
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS, POCKETOS_LOG_STDERR="0")
    second = subprocess.run([BIN, "--test-listen", HOST, "--test-port", str(PORT), "--test-shadow", SHADOW,
                             "--test-passwd", PASSWD],
                            env=env, capture_output=True, timeout=10)
    check("a second instance exits instead of sharing the key", second.returncode != 0)
    check("an unknown option is a usage error", subprocess.run([BIN, "--no-verify"], env=env, capture_output=True).returncode == 2)
    ctl.close()
    stop()

    # ---- 14. what the log holds --------------------------------------------------------------------
    text = ""
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as f:
            text += f.read()
    check("there is a log", "listening on" in text and "pairing: window opened" in text)
    check("no pairing token in the log", token not in text and old_token not in text)
    check("no client name in the log", "Phone A" not in text and "Phone B" not in text)
    check("no full fingerprint in the log", A.fp_hex not in text and device_id not in text)
    check("no key material in the log", "PRIVATE" not in text and "BEGIN" not in text)
    check("no upstream text or field values in the log", "SECRET" not in text and "LEAK" not in text and "home" not in text)
except Exception as e:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % e, False)
finally:
    if proc is not None:
        proc.kill()
        print(proc.stderr.read().decode(errors="replace")[-2000:])
    for svc in (sysd, radiod, meshd):
        svc.stop()
    if failed and os.environ.get("GATEWAYD_E2E_KEEP"):
        print("  kept:", ROOT)
    else:
        shutil.rmtree(ROOT, ignore_errors=True)

print("gatewayd e2e: %d checks, %d failed" % (checks, failed))
sys.exit(0 if failed == 0 else 1)
