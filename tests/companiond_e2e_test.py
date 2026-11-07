#!/usr/bin/env python3
"""companiond end to end, on the host: no radio, no MeshCore sources.

    scripted meshcored (tests/fake-meshcored)
        |  pocketipc, the real socket          \\
    services/companiond/companiond               a second subscriber standing
        |  TCP on 127.0.0.1                      in for RIFT, on the same
    a companion-protocol client                  meshcored socket
    written here from upstream's framing

The client speaks MeshCore's companion protocol exactly as the pinned upstream
(meshcore-dev/MeshCore 81dbae59, SerialWifiInterface.cpp and
examples/companion_radio/MyMesh.cpp) and meshcore.js (26193c05,
src/connection/tcp_connection.js) frame it: '<' + LE16 length + payload out,
'>' + LE16 length + payload in. It is not the official app. What the app
does that this does not is the hardware acceptance procedure's to find
(docs/services/COMPANIOND.md).

What is checked, against what meshcored was asked (the fake's logs) and what
the RIFT stand-in received, not against the bridge's own say-so:

  - the startup sequence: DEVICE_QUERY, APP_START, SET_DEVICE_TIME refused,
    GET_CONTACTS, GET_CHANNEL until NOT_FOUND, SYNC_NEXT_MESSAGE until empty;
  - direct and channel sends: one mesh.send each, an ACK reported under the
    tag the app was given, a repeated frame answered and not sent again,
    the app's own retry sent as meshcored's resend;
  - refusals: every setting, clock, identity, key and radio command, and
    key export, never answered OK and never reaching meshcored;
  - framing: fragmented, combined, wrong-direction, oversized and empty
    frames;
  - reconnects: messages queued while no app was connected, and across a
    reconnect, delivered once; a second connection replacing the first;
  - coexistence: the RIFT stand-in receives every event the bridge does;
  - meshcored going away and coming back under a live app;
  - the access rules: the wildcard address refused, a peer outside the
    allowed networks refused.
"""

import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPANIOND = os.environ.get("COMPANIOND_BIN",
                            os.path.join(REPO, "services/companiond/companiond"))
FAKE = os.path.join(REPO, "tests/fake-meshcored")

SELF_KEY = "5f000000000000000000000000000000000000000000000000000000000000ff"
ALICE = "aa11" + "22" * 30
BOB = "bb33" + "44" * 30
PUBLIC_KEY = bytes.fromhex("8b3387e9c5cdea6ac9e5edbaa115cd72")

NODES = [
    {"public_key": ALICE, "node_hash": "aa", "name": "Alice", "type": 1, "path_known": True,
     "hops": 2, "direct": False, "path_hex": "1a2b", "last_advert_timestamp": 1762500000,
     "last_heard_mono_ms": -5000, "lat": 59.912345, "lon": 10.754321},
    {"public_key": BOB, "node_hash": "bb", "name": "Bob \U0001F680", "type": 2,
     "path_known": False},
]
CHANNELS = [
    {"channel": 0, "name": "Public", "channel_hash": "11", "key_bits": 128,
     "well_known": "public", "text_limit": 147, "ack_expected": False},
    {"channel": 1, "name": "Crew", "channel_hash": "c7", "key_bits": 128,
     "text_limit": 20, "ack_expected": False},
]
EVENTS = [
    ("mesh.message", {"message": {"id": 7, "direction": "in", "kind": "direct",
                                  "peer_public_key": ALICE, "peer_name": "Alice",
                                  "text": "hello from alice", "timestamp": 1762500100,
                                  "mono_ms": -100, "state": "received", "ack_expected": False,
                                  "snr_db": 6.25}}),
    ("mesh.message", {"message": {"id": 8, "direction": "in", "kind": "channel",
                                  "channel": 1, "channel_name": "Crew", "channel_hash": "c7",
                                  "sender_name": "Bob", "text": "Bob: on my way",
                                  "timestamp": 1762500200, "mono_ms": -50,
                                  "state": "received", "ack_expected": False}}),
    # The same message raised again (a state change, a repeat): queued once.
    ("mesh.message", {"message": {"id": 8, "direction": "in", "kind": "channel",
                                  "channel": 1, "channel_name": "Crew", "channel_hash": "c7",
                                  "text": "Bob: on my way", "timestamp": 1762500200,
                                  "mono_ms": -50, "state": "received", "ack_expected": False}}),
    # Somebody else's outgoing message (RIFT's): nothing for the app.
    ("mesh.message", {"message": {"id": 9, "direction": "out", "kind": "direct",
                                  "peer_public_key": BOB, "text": "sent by RIFT",
                                  "timestamp": 1762500300, "mono_ms": -10,
                                  "state": "sent_flood", "ack_expected": True}}),
]

failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ---- the scripted meshcored, and a pocketipc subscriber standing in for RIFT --

class Fake:
    def __init__(self, tmp, ack_sends=1, subscribers=2, events=EVENTS, life_ms=60000):
        self.tmp = tmp
        self.sends = os.path.join(tmp, "sends.log")
        self.methods = os.path.join(tmp, "methods.log")
        self.manage = os.path.join(tmp, "manage.log")
        self.adverts = os.path.join(tmp, "adverts.log")
        evfile = os.path.join(tmp, "events.txt")
        with open(evfile, "w") as f:
            for name, data in events:
                f.write("%s|%s\n" % (name, json.dumps(data)))
        env = dict(os.environ)
        env.update({
            "POCKETOS_RUNTIME_DIR": tmp,
            "FAKE_MESHCORED_NODES": json.dumps(NODES),
            "FAKE_MESHCORED_CHANNELS": json.dumps(CHANNELS),
            "FAKE_MESHCORED_EVENTS": evfile,
            "FAKE_MESHCORED_SENDS": self.sends,
            "FAKE_MESHCORED_METHODS": self.methods,
            "FAKE_MESHCORED_MANAGE": self.manage,
            "FAKE_MESHCORED_SUBSCRIBERS": str(subscribers),
            "FAKE_MESHCORED_ACK_SENDS": str(ack_sends),
            "FAKE_MESHCORED_LIFE_MS": str(life_ms),
        })
        for p in (self.sends, self.methods, self.manage):
            if os.path.exists(p):
                os.unlink(p)
        self.proc = subprocess.Popen([FAKE], env=env)
        sock = os.path.join(tmp, "meshcored.sock")
        for _ in range(400):
            if os.path.exists(sock):
                break
            time.sleep(0.005)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait()
        sock = os.path.join(self.tmp, "meshcored.sock")
        if os.path.exists(sock):
            os.unlink(sock)

    def lines(self, path):
        try:
            with open(path) as f:
                return [l.rstrip("\n") for l in f]
        except FileNotFoundError:
            return []


class RiftStandIn:
    """A plain pocketipc subscriber on meshcored's socket, as RIFT is one."""

    def __init__(self, tmp):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(os.path.join(tmp, "meshcored.sock"))
        self.events = []
        self.lock = threading.Lock()
        body = json.dumps({"id": 1, "method": "mesh.subscribe"}).encode()
        self.s.sendall(struct.pack(">I", len(body)) + body)
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        buf = b""
        while True:
            try:
                d = self.s.recv(65536)
            except OSError:
                return
            if not d:
                return
            buf += d
            while len(buf) >= 4:
                n = struct.unpack(">I", buf[:4])[0]
                if len(buf) < 4 + n:
                    break
                msg = json.loads(buf[4:4 + n])
                buf = buf[4 + n:]
                if "event" in msg:
                    with self.lock:
                        self.events.append(msg)

    def messages(self):
        with self.lock:
            return [e["data"]["message"] for e in self.events if e["event"] == "mesh.message"]

    def close(self):
        self.s.close()


# ---- the companion-protocol client ---------------------------------------

PUSHES = (0x80, 0x81, 0x82, 0x83, 0x8F)


class App:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.pushes = []

    def raw(self, data):
        self.s.sendall(data)

    @staticmethod
    def frame(payload):
        return b"<" + struct.pack("<H", len(payload)) + payload

    def send(self, payload):
        self.raw(self.frame(payload))

    def read_frame(self, timeout=3.0):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 3:
                typ = self.buf[0]
                n = struct.unpack("<H", self.buf[1:3])[0]
                if typ != 0x3E:
                    raise AssertionError("bad frame type %r" % typ)
                if len(self.buf) >= 3 + n:
                    f = self.buf[3:3 + n]
                    self.buf = self.buf[3 + n:]
                    return f
            d = self.s.recv(65536)
            if not d:
                raise EOFError("closed")
            self.buf += d

    def reply(self, timeout=3.0):
        """The next frame that is not a push; pushes are kept aside."""
        while True:
            f = self.read_frame(timeout)
            if f[0] in PUSHES:
                self.pushes.append(f)
                continue
            return f

    def cmd(self, payload, timeout=3.0):
        self.send(payload)
        return self.reply(timeout)

    def wait_push(self, code, timeout=3.0):
        end = time.time() + timeout
        while True:
            for p in self.pushes:
                if p[0] == code:
                    self.pushes.remove(p)
                    return p
            left = end - time.time()
            if left <= 0:
                return None
            try:
                f = self.read_frame(left)
            except (socket.timeout, EOFError):
                return None
            if f[0] in PUSHES:
                self.pushes.append(f)
            else:
                raise AssertionError("unexpected reply %r" % f[:4])

    def closed(self, timeout=3.0):
        self.s.settimeout(timeout)
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    return True
                self.buf += d
        except (ConnectionResetError, BrokenPipeError):
            return True
        except socket.timeout:
            return False

    def close(self):
        self.s.close()


def device_query(ver=3):
    return bytes([22, ver])


def app_start():
    return bytes([1, 0, 0, 0, 0, 0, 0, 0]) + b"e2e"


def send_dm(key_hex, text, ts, attempt=0):
    return bytes([2, 0, attempt]) + struct.pack("<I", ts) + bytes.fromhex(key_hex)[:6] + \
        text.encode()


def send_chan(slot, text, ts):
    return bytes([3, 0, slot]) + struct.pack("<I", ts) + text.encode()


class Bridge:
    def __init__(self, tmp, port, allow="127.0.0.1/32", listen=None):
        env = dict(os.environ)
        env["POCKETOS_RUNTIME_DIR"] = tmp
        env["POCKETOS_LOG_DIR"] = tmp
        # A file, not a pipe: a pipe nobody drains would stall the service.
        self.err_path = os.path.join(tmp, "companiond.stderr")
        self.err = open(self.err_path, "ab")
        self.proc = subprocess.Popen(
            [COMPANIOND, "--listen", listen or ("127.0.0.1:%d" % port), "--allow", allow,
             "--verbose"], env=env, stderr=self.err)
        self.port = port

    def log(self):
        with open(self.err_path, errors="replace") as f:
            return f.read()

    def wait_listening(self):
        for _ in range(200):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.02)
        return False

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        rc = self.proc.wait(timeout=5)
        self.err.close()
        return rc

    def stop_clean(self, what):
        """Stopped by SIGTERM, it leaves with 0 and nothing from a sanitiser
        (the second run of this suite is the service built with ASan/UBSan)."""
        rc = self.stop()
        log = self.log()
        check(rc == 0, "%s: companiond exited %r" % (what, rc))
        check("Sanitizer" not in log and "runtime error" not in log,
              "%s: no sanitiser report:\n%s" % (what, log[-2000:]))


def wait_until(fn, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.02)
    return fn()


# ---- the suites -----------------------------------------------------------

def suite_main(tmp):
    fake = Fake(tmp, ack_sends=1, subscribers=2)
    port = free_port()
    bridge = Bridge(tmp, port)
    check(bridge.wait_listening(), "companiond listens")
    # The bridge connected (the listening check proves the process is up; give
    # it its meshcored subscription), then RIFT subscribes: events flow.
    time.sleep(0.3)
    rift = RiftStandIn(tmp)
    check(wait_until(lambda: len(rift.messages()) >= 4), "RIFT stand-in receives the events")

    # -- startup, with messages already waiting from before any app connected
    app = App(port)
    f = app.cmd(device_query())
    check(f[0] == 13 and f[1] == 14 and f[2] == 255 and f[3] == 8,
          "DEVICE_INFO: version 14, contacts 255, 8 channel slots: %r" % f[:4])
    check(len(f) == 82 and f[80] == 0 and f[81] == 0, "DEVICE_INFO: no repeat, 1-byte hashes")
    check(f[20:60].rstrip(b"\0") == b"LILYGO T-Display K230 (Doors)", "DEVICE_INFO: model")
    check(struct.unpack("<I", f[4:8])[0] == 0, "DEVICE_INFO: no BLE PIN")

    f = app.cmd(app_start())
    check(f[0] == 5 and f[4:36] == bytes.fromhex(SELF_KEY), "SELF_INFO: meshcored's identity")
    freq, bw = struct.unpack("<II", f[48:56])
    check((freq, bw, f[56], f[57]) == (869618, 62500, 8, 8),
          "SELF_INFO: the configured profile %r" % ((freq, bw, f[56], f[57]),))
    check(f[2] == 2 and f[3] == 22, "SELF_INFO: tx power 2 of 22 dBm")
    check(f[58:] == b"K230-A", "SELF_INFO: the name %r" % f[58:])

    f = app.cmd(bytes([6]) + struct.pack("<I", int(time.time())))
    check(f == bytes([1, 1]), "SET_DEVICE_TIME refused as unsupported, not OK: %r" % f)
    f = app.cmd(bytes([5]))
    check(f[0] == 9 and abs(struct.unpack("<I", f[1:5])[0] - time.time()) < 5,
          "GET_DEVICE_TIME: the system clock")

    # contacts
    f = app.cmd(bytes([4]))
    check(f[0] == 2 and struct.unpack("<I", f[1:5])[0] == 2, "CONTACTS_START: 2")
    contacts = []
    while True:
        f = app.reply()
        if f[0] == 4:
            break
        contacts.append(f)
    check(len(contacts) == 2 and all(len(c) == 148 for c in contacts), "two 148-byte contacts")
    a = contacts[0]
    check(a[1:33] == bytes.fromhex(ALICE) and a[33] == 1, "Alice: key and type")
    check(a[35] == 2 and a[36:38] == bytes.fromhex("1a2b") and a[38:100] == bytes(62),
          "Alice: route of 2 one-byte hops")
    check(a[100:132].rstrip(b"\0") == b"Alice", "Alice: name")
    lat, lon = struct.unpack("<ii", a[136:144])
    check((lat, lon) == (59912345, 10754321), "Alice: location %r" % ((lat, lon),))
    lastmod = struct.unpack("<I", a[144:148])[0]
    # Coarse on purpose: lastmod puts a monotonic age on the wall clock, and a
    # host (WSL among them) may step its wall clock while the test runs.
    check(abs(lastmod - (time.time() - 5)) < 30,
          "Alice: lastmod from when she was heard (%d, now %d)" % (lastmod, time.time()))
    b = contacts[1]
    check(b[35] == 0xFF, "Bob: no route known")
    check(b[100:132].rstrip(b"\0").decode() == "Bob \U0001F680", "Bob: UTF-8 name intact")
    check(struct.unpack("<I", f[1:5])[0] == lastmod, "END_OF_CONTACTS: newest lastmod")

    f = app.cmd(bytes([30]) + bytes.fromhex(ALICE))
    check(f[0] == 3 and f[1:33] == bytes.fromhex(ALICE), "GET_CONTACT_BY_KEY")
    f = app.cmd(bytes([30]) + bytes(32))
    check(f == bytes([1, 2]), "GET_CONTACT_BY_KEY unknown: NOT_FOUND")

    # channels, the way meshcore.js lists them: until the first error
    chans = []
    for i in range(20):
        f = app.cmd(bytes([31, i]))
        if f[0] == 1:
            check(f[1] == 2, "end of the channel table is NOT_FOUND")
            break
        chans.append(f)
    check(len(chans) == 8, "8 channel slots listed (%d)" % len(chans))
    check(chans[0][2:34].rstrip(b"\0") == b"Public" and chans[0][34:50] == PUBLIC_KEY,
          "slot 0: Public with its public key")
    check(chans[1][2:34].rstrip(b"\0") == b"Crew" and chans[1][34:50] == bytes(16),
          "slot 1: a private channel, its key withheld")
    check(chans[2][2:50] == bytes(48), "slot 2: empty, as upstream answers an empty slot")

    # queued messages, delivered once each
    f = app.cmd(bytes([10]))
    check(f[0] == 16 and f[1] == 25 and f[4:10] == bytes.fromhex(ALICE)[:6] and f[10] == 0xFF and
          f[11] == 0 and struct.unpack("<I", f[12:16])[0] == 1762500100 and
          f[16:] == b"hello from alice", "SYNC: Alice's direct message (V3, SNR 6.25)")
    f = app.cmd(bytes([10]))
    check(f[0] == 17 and f[4] == 1 and struct.unpack("<I", f[7:11])[0] == 1762500200 and
          f[11:] == b"Bob: on my way", "SYNC: the channel message, sender prefix kept")
    f = app.cmd(bytes([10]))
    check(f == bytes([10]), "SYNC: no more; the repeat and RIFT's own message not queued")

    # -- sending
    ts = int(time.time())
    f = app.cmd(send_dm(ALICE, "hi alice", ts))
    check(f[0] == 6 and f[1] == 1 and struct.unpack("<I", f[6:10])[0] == 30000,
          "SENT: flood, meshcored's ACK timeout")
    tag = f[2:6]
    p = app.wait_push(0x82)
    check(p is not None and p[1:5] == tag, "SEND_CONFIRMED under the tag the app was given")
    check(fake.lines(fake.sends) == ["aa1122222222|hi alice"],
          "one mesh.send, to the 6-byte prefix: %r" % fake.lines(fake.sends))
    f = app.cmd(send_dm(ALICE, "hi alice", ts))
    check(f[0] == 6 and f[2:6] == tag, "the same frame again: the same answer")
    check(len(fake.lines(fake.sends)) == 1, "...and nothing sent again")

    f = app.cmd(send_chan(0, "hello public", ts))
    check(f == bytes([0]), "channel send: OK")
    f = app.cmd(send_chan(0, "hello public", ts))
    check(f == bytes([0]), "channel send repeated: OK")
    check(fake.lines(fake.sends)[1:] == ["#0|hello public"], "one channel mesh.send")
    f = app.cmd(send_chan(1, "x" * 21, ts))
    check(f == bytes([1, 6]), "over the channel's text limit: ILLEGAL_ARG")
    f = app.cmd(send_chan(3, "nobody here", ts))
    check(f == bytes([1, 2]), "an empty slot: NOT_FOUND")
    f = app.cmd(send_dm(ALICE, "bad \x1b[31m", ts))
    check(f == bytes([1, 6]), "a control character: ILLEGAL_ARG, never sent")
    f = app.cmd(bytes([2, 1, 0]) + struct.pack("<I", ts) + bytes.fromhex(ALICE)[:6] + b"reboot")
    check(f == bytes([1, 1]), "CLI data to a node: unsupported")
    check(len(fake.lines(fake.sends)) == 2, "refused sends reached meshcored: %r" %
          fake.lines(fake.sends))

    # -- everything that would change the node: refused, never OK
    refused = {
        7: bytes([7, 1]),                                       # SEND_SELF_ADVERT
        8: bytes([8]) + b"Mallory",                             # SET_ADVERT_NAME
        9: bytes([9]) + bytes.fromhex(BOB) + bytes(3 + 64 + 32 + 4),  # ADD_UPDATE_CONTACT
        11: bytes([11]) + struct.pack("<II", 869525, 250000) + bytes([11, 5]),  # RADIO_PARAMS
        12: bytes([12, 22]),                                    # SET_RADIO_TX_POWER
        13: bytes([13]) + bytes.fromhex(ALICE),                 # RESET_PATH
        14: bytes([14]) + bytes(8),                             # SET_ADVERT_LATLON
        15: bytes([15]) + bytes.fromhex(ALICE),                 # REMOVE_CONTACT
        19: bytes([19]) + b"reboot",                            # REBOOT
        21: bytes([21]) + bytes(8),                             # SET_TUNING_PARAMS
        32: bytes([32, 2]) + b"Evil".ljust(32, b"\0") + bytes(range(16)),  # SET_CHANNEL
        33: bytes([33]),                                        # SIGN_START
        37: bytes([37]) + bytes(4),                             # SET_DEVICE_PIN
        38: bytes([38, 1]),                                     # SET_OTHER_PARAMS
        51: bytes([51]) + b"reset",                             # FACTORY_RESET
        61: bytes([61, 0, 2]),                                  # SET_PATH_HASH_MODE
        63: bytes([63]),                                        # SET_DEFAULT_FLOOD_SCOPE
        66: bytes([66]) + b"set name x",                        # RUN_CLI_COMMAND
        200: bytes([200]),                                      # nothing at all
    }
    for code, payload in refused.items():
        f = app.cmd(payload)
        check(f == bytes([1, 1]), "command %d refused as unsupported: %r" % (code, f))
    f = app.cmd(bytes([23]))
    check(f == bytes([15]), "EXPORT_PRIVATE_KEY: DISABLED")
    f = app.cmd(bytes([24]) + bytes(64))
    check(f == bytes([15]), "IMPORT_PRIVATE_KEY: DISABLED")
    f = app.cmd(bytes([64]))
    check(f == bytes([28]), "GET_DEFAULT_FLOOD_SCOPE: none")
    methods = set(fake.lines(fake.methods))
    for m in ("mesh.set_name", "mesh.channel_add", "mesh.channel_remove", "mesh.set_path_hash",
              "mesh.advert", "mesh.node_remove", "mesh.node_reset_path", "mesh.discover"):
        check(m not in methods, "%s never asked of meshcored" % m)
    check(fake.lines(fake.manage) == [], "nothing managed")

    # -- framing
    app.raw(b"\x3e\x02\x00ab")                      # the wrong direction: read past
    app.raw(b"<" + struct.pack("<H", 400) + b"<" * 400)   # oversized: read past
    app.raw(b"<\x00\x00")                           # empty: dropped
    f = app.cmd(bytes([10]))
    check(f == bytes([10]), "in step after junk, oversized and empty frames")
    app.raw(App.frame(bytes([5])) + App.frame(bytes([10])) + App.frame(bytes([64])))
    r = [app.reply(), app.reply(), app.reply()]
    check([x[0] for x in r] == [9, 10, 28], "three combined frames, answered in order")
    whole = App.frame(device_query())
    for i in range(len(whole)):
        app.raw(whole[i:i + 1])
        time.sleep(0.01)
    f = app.reply()
    check(f[0] == 13, "a frame one byte at a time")

    # -- the queue across a reconnect, and a send repeated after one
    app.close()
    time.sleep(0.2)
    app = App(port)
    f = app.cmd(send_dm(ALICE, "hi alice", ts))
    check(f[0] == 6 and f[2:6] == tag, "after a reconnect, the repeated frame is answered")
    check(len(fake.lines(fake.sends)) == 2, "...and still not sent again")

    # -- a second connection replaces the first
    other = App(port)
    check(app.closed(), "the first connection is closed when a second arrives")
    f = other.cmd(device_query())
    check(f[0] == 13, "the second one is served")
    app.close()

    # -- meshcored goes away and comes back under a live app
    rift.close()
    fake.stop()
    time.sleep(0.3)
    f = other.cmd(app_start())
    check(f == bytes([1, 4]), "with meshcored gone, APP_START is BAD_STATE: %r" % f)
    f = other.cmd(bytes([10]))
    check(f == bytes([10]), "...while the local queue still answers")
    fake = Fake(tmp, ack_sends=1, subscribers=1, events=[
        ("mesh.node", {"reason": "discovered", "node": NODES[1]}),
        ("mesh.node", {"reason": "path", "node": NODES[0]}),
        ("mesh.node", {"reason": "removed", "node": NODES[1]}),
        ("mesh.message", {"message": {"id": 1, "direction": "in", "kind": "direct",
                                      "peer_public_key": BOB, "text": "back again",
                                      "timestamp": 1762500400, "mono_ms": -5,
                                      "state": "received", "ack_expected": False}})])
    check(other.wait_push(0x83, timeout=5) is not None,
          "after meshcored returns, a new message is announced (MSG_WAITING)")
    p = other.wait_push(0x80)
    check(p is not None and p[1:] == bytes.fromhex(BOB), "ADVERT push for a discovered node")
    p = other.wait_push(0x81)
    check(p is not None and p[1:] == bytes.fromhex(ALICE), "PATH_UPDATED push")
    p = other.wait_push(0x8F)
    check(p is not None and p[1:] == bytes.fromhex(BOB), "CONTACT_DELETED push")
    f = other.cmd(bytes([10]))
    check(f[0] == 16 and f[16:] == b"back again",
          "a new run's message id 1 is not mistaken for one already seen")
    f = other.cmd(app_start())
    check(f[0] == 5, "APP_START answered again")
    other.close()

    bridge.stop_clean("main suite")
    fake.stop()


def suite_resend(tmp):
    """The app's own retry (attempt one higher) is meshcored's resend, and the
    ACK is reported under the newest tag; an ACK that arrives while no app is
    connected is reported when one has started."""
    fake = Fake(tmp, ack_sends=2, subscribers=1, events=[])
    port = free_port()
    bridge = Bridge(tmp, port)
    check(bridge.wait_listening(), "companiond listens (resend)")
    time.sleep(0.3)
    app = App(port)
    app.cmd(device_query())
    app.cmd(app_start())
    ts = int(time.time())
    f = app.cmd(send_dm(ALICE, "are you there", ts))
    tag1 = f[2:6]
    check(f[0] == 6, "first attempt SENT")
    check(app.wait_push(0x82, timeout=0.5) is None, "first attempt: no ACK, nothing confirmed")
    f = app.cmd(send_dm(ALICE, "are you there", ts, attempt=1))
    tag2 = f[2:6]
    check(f[0] == 6 and tag2 != tag1, "retry SENT under a new tag")
    p = app.wait_push(0x82)
    check(p is not None and p[1:5] == tag2, "the retry's ACK confirmed under the new tag")
    check(fake.lines(fake.sends) == ["aa1122222222|are you there", "resend|1001"],
          "one send and one meshcored resend: %r" % fake.lines(fake.sends))
    f = app.cmd(send_dm(ALICE, "are you there", ts, attempt=1))
    check(f[0] == 6 and f[2:6] == tag2 and len(fake.lines(fake.sends)) == 2,
          "the retry repeated: answered, not resent")
    app.close()
    bridge.stop_clean("resend suite")
    fake.stop()


def suite_access(tmp):
    port = free_port()
    b = Bridge(tmp, port, listen="0.0.0.0:%d" % port)
    check(b.proc.wait(timeout=5) == 1, "the wildcard address is refused")
    b = Bridge(tmp, port, listen="8.8.8.8:%d" % port)
    check(b.proc.wait(timeout=5) == 1, "a public address is refused")
    b = Bridge(tmp, port, allow="10.0.0.0/8")
    time.sleep(0.3)
    s = socket.create_connection(("127.0.0.1", port), timeout=2)
    s.settimeout(2)
    try:
        d = s.recv(10)
    except ConnectionResetError:
        d = b""
    check(d == b"", "a peer outside --allow is closed at once")
    s.close()
    b.stop_clean("access suite")
    check("not in the allowed addresses" in b.log(), "the refusal is logged")


def main():
    for p in (COMPANIOND, FAKE):
        if not os.access(p, os.X_OK):
            print("missing %s (make companiond-test builds it)" % p)
            return 2
    tmp = tempfile.mkdtemp(prefix="companiond-e2e-")
    try:
        suite_main(tmp)
        suite_resend(tmp)
        suite_access(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("companiond_e2e_test: %d checks, %d failed" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
