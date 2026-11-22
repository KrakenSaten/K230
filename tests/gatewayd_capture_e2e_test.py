#!/usr/bin/env python3
"""gatewayd, the capture group (G1), end to end: the real service (the
test-hooks build) between a fake shell that serves PacketShark's capture.*
methods on a real pocketipc socket and real TLS clients, on loopback.

  python3 tests/gatewayd_capture_e2e_test.py <path to gatewayd-testhooks>

What is driven: that the shell's socket is not opened while no client may
read a capture, and what is asked on it (five method names, never a shell.*
one, never a subscription); capture_view and capture_control on every method
and on the topic; the filters on answers and events; parameters; that a
client can name neither who asks nor what its request is called; one start
request at a time and one per client in 30 s; the gateway's own cancel when
the asking client disconnects, is revoked, loses capture_control, or its
request expires, always naming that request and never sent for a request
that is over or for a capture a tap has started; a cancel that overtakes a
slow answer; a shell that does not answer, that goes away and comes back,
and one without PacketShark; and what the log holds.

The 62 seconds of a request's expiry are waited for in full, once.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403

TOP = ["available", "state", "reason", "iface", "keep", "elapsed_ms", "limit_s", "seen", "stored", "held",
       "kernel_drops", "iface_drops", "shed", "bytes", "pps", "bps", "fill_permille", "classes", "findings", "diag",
       "requested_by_companion"]
STATE = ["state", "reason", "iface", "keep", "limit_s"]
FIVE = {"capture.status", "capture.hold", "capture.stop", "capture.request_start", "capture.request_cancel"}


class FakeShell(FakeService):
    """PacketShark's capture.* on the shell's socket, as the app answers them,
    with more in every answer than any client may see."""

    def __init__(self, present=True):
        self.present = present      # PacketShark is in the image
        self.front = True           # and in front
        self.state = "idle"
        self.reason = "none"
        self.by_companion = False
        self.seen = 0
        self.request = "none"       # none, shown, refused, timeout
        self.token = None
        self.delay = 0.0            # before the answer to capture.request_start
        self.live = 0               # connections open now
        self.lock = threading.Lock()
        super().__init__("shell", {})

    def status(self):
        if not self.front:
            return {"available": False}
        self.seen += 100 if self.state == "capturing" else 0
        return {"available": True, "state": self.state, "reason": self.reason, "iface": "eth0", "keep": "headers",
                "elapsed_ms": 61250, "limit_s": 900, "seen": self.seen, "stored": self.seen, "held": 0,
                "kernel_drops": 0, "iface_drops": 0, "shed": 0, "bytes": self.seen * 600, "pps": 100, "bps": 480000,
                "fill_permille": 310,
                "classes": {"tcp": self.seen, "udp": 0, "dns": 0, "dhcp": 0, "arp": 0, "icmp": 0, "ipv6": 0,
                            "other": 0, "hosts": ["192.0.2.7"]},
                "findings": [0, 3, 0, 1, 0, 0], "diag": {"dns_queries": 815, "dns_rtt_max_seq": 4242},
                "requested_by_companion": self.by_companion, "request": self.request,
                # What no PacketShark sends, and what must not get through if one ever did.
                "rows": [{"src": "192.0.2.7", "dst": "192.0.2.9", "info": "LEAK"}], "save_name": "capture-LEAK.pcap",
                "client": "LEAK", "helper_version": "LEAK", "session_key": "LEAK"}

    def answer(self, m, p):
        if not self.present or m not in FIVE:
            return (1, "unknown method")
        if m == "capture.status":
            return self.status()
        if not self.front:
            return (3, "PacketShark is not open LEAK")
        if m in ("capture.hold", "capture.stop"):
            if self.state not in ("capturing", "hold"):
                return (3, "nothing is capturing LEAK")
            was = self.state
            self.state = "stopped" if m == "capture.stop" else ("hold" if p["on"] else "capturing")
            self.reason = "user" if m == "capture.stop" else "none"
            return {"state": was, "changing": True, "helper_pid": 77, "note": "LEAK"}
        if m == "capture.request_start":
            if self.request == "shown":
                return (3, "a request is already shown")
            if p.get("iface") != "eth0":
                return (3, "that interface is not offered LEAK")
            self.request = "shown"
            self.token = p.get("request")
            return {"shown": True, "request": p.get("request"), "client": "LEAK"}
        # capture.request_cancel: only the request it names.
        if self.request != "shown":
            return (3, "no request is shown")
        if (p or {}).get("request") != self.token:
            return (3, "that is not the request shown")
        self.request = "refused"
        return {"shown": False}

    def serve(self, c):
        self.live += 1
        try:
            while True:
                req = recv_frame(c)
                m, p = req.get("method"), req.get("params")
                self.calls.append((m, p))
                if self.mode == "stall":
                    continue
                if m == "capture.request_start" and self.delay:
                    time.sleep(self.delay)
                with self.lock:
                    res = self.answer(m, p)
                if isinstance(res, tuple):
                    send_frame(c, {"id": req["id"], "error": {"code": res[0], "message": res[1]}})
                else:
                    send_frame(c, {"id": req["id"], "result": res})
        except (EOFError, OSError, ValueError):
            c.close()
        finally:
            self.live -= 1

    def of(self, name):
        return [p for m, p in self.calls if m == name]

    def tap_start(self, starting=0.0):
        """A finger on START on the unit's sheet. The sheet goes at once; the
        capture is there when the helper has answered, `starting` later:
        until then the unit is on setup and says nothing of the request."""
        with self.lock:
            self.request = "none"

        def begun():
            with self.lock:
                self.state = "capturing"
                self.by_companion = True

        if starting:
            threading.Timer(starting, begun).start()
        else:
            begun()

    def tap_refuse(self):
        with self.lock:
            self.request = "refused"


def wait_for(what, cond, timeout=12):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    check(what, False)
    return False


def connect(ident, fp, topics=None):
    c = Client(ident, pin=fp)
    h = c.hello()
    assert h["result"]["paired"] is True, h
    c.caps = h["result"]["capabilities"]
    if topics is not None:
        c.sub = c.call("gateway.subscribe", {"topics": topics}).get("result")
    return c


def events(c, name=None, wait=0.0):
    """What the unit has sent so far (a ping's answer comes after it)."""
    if wait:
        time.sleep(wait)
    c.call("gateway.ping")
    got = [e for e in c.events if name is None or e["event"] == name]
    c.events = [e for e in c.events if not (name is None or e["event"] == name)]
    return got


def wait_event(c, name, pred, timeout=8):
    end = time.time() + timeout
    seen = []
    while time.time() < end:
        for e in events(c, name):
            seen.append(e)
            if pred(e["data"]):
                return e["data"]
        time.sleep(0.1)
    return None


def word(d):
    return d["state"] + ("" if d["reason"] == "none" else "/" + d["reason"])


def clean(obj):
    text = json.dumps(obj)
    return not re.search(r"LEAK|192\.0\.2|enx|4242|pcap|rows|helper|session_key|\"request\"|\"client\"", text)


def scopes(ctl, ident, names):
    r = ctl.call("gateway.set_scopes", {"client": ident.fp_hex, "scopes": names})
    assert "result" in r, r


VIEW = ["status", "capture_view"]
CONTROL = ["status", "capture_view", "capture_control"]
ASK = {"iface": "eth0", "limit_s": 900}
ids = {n: Identity(n) for n in "abcdefghij"}
shell = None
meshd = None

try:
    print("gatewayd capture e2e")
    set_password(True)
    meshd = FakeService("meshcored", MESHD)
    shell = FakeShell()
    ctl = start()
    ctl.call("gateway.subscribe")

    # ---- 1. nobody may read a capture: the shell's socket is not opened ---------------------------
    A, B = ids["a"], ids["b"]
    fp, d, _ = pair(ctl, A, "phone A", ["status"])
    check("A is paired with status alone", d["result"]["paired"] is True)
    pair(ctl, B, "phone B")  # the default scopes: status and the mesh
    time.sleep(1.2)
    check("no client holds capture_view: no connection to the shell, nothing asked", shell.calls == [] and shell.live == 0)
    a = connect(A, fp)
    check("the capability is not offered", "capture" not in a.caps)
    check("each capture method is not permitted without its scope (code 7)",
          all(err_code(a.call(m, p)) == 7 for m, p in
              [("capture.status", None), ("capture.hold", {"on": True}), ("capture.stop", None),
               ("capture.request_start", ASK)]))
    check("the gateway's own cancel is no method of a client (code 1), nor is anything else of a capture or the shell",
          all(err_code(a.call(m, p)) == 1 for m, p in
              [("capture.request_cancel", None), ("capture.request_cancel", {"request": "00"}), ("capture.start", ASK),
               ("capture.discard", None), ("capture.save", None), ("shell.subscribe", None), ("shell.open", {"id": "x"}),
               ("shell.tap", {"text": "START"})]))
    r = a.call("gateway.subscribe", {"topics": ["capture"]})
    check("the topic is not held without capture_view", r.get("result") == {"topics": []})
    check("and none of that reached the shell", shell.calls == [])
    a.close()

    # ---- 2. capture_view: the socket is opened, one question, and no more until somebody listens ----
    scopes(ctl, A, VIEW)
    wait_for("the gateway asks the shell once whether PacketShark is there", lambda: len(shell.calls) == 1)
    time.sleep(2.6)
    check("and asks nothing more while no client listens (the unit's screen shows no reader)",
          shell.calls == [("capture.status", None)] and shell.live == 1)
    a = connect(A, fp)
    check("the capability is offered now", "capture" in a.caps)
    r = a.call("capture.status")
    st = r.get("result")
    check("capture.status: the contract's fields and no other", st is not None and list(st) == TOP)
    check("nothing of what the unit said beyond them: no row, address, file, word of a request", st is not None and clean(st))
    check("its figures are whole numbers and its classes the eight",
          st is not None and all(type(st[k]) is int for k in TOP[5:17]) and len(st["classes"]) == 8 and
          len(st["findings"]) == 6 and len(st["diag"]) == 18 and "dns_rtt_max_seq" not in st["diag"])
    shell.front = False
    check("PacketShark not in front: not available, and nothing else", a.call("capture.status").get("result") == {"available": False})
    shell.front = True
    n0 = len(shell.calls)
    check("reading alone does not hold, stop or ask (code 7)",
          all(err_code(a.call(m, p)) == 7 for m, p in
              [("capture.hold", {"on": True}), ("capture.stop", None), ("capture.request_start", ASK)]))
    check("and none of the three reached the shell", len(shell.calls) == n0)
    check("capture.status takes no parameters", err_code(a.call("capture.status", {"rows": True})) == 2 and len(shell.calls) == n0)

    # ---- 3. the topic: events made from the gateway's own questions --------------------------------
    r = a.call("gateway.subscribe", {"topics": ["capture"]}).get("result")
    check("the topic is held, and the answer says the shell is there", r is not None and r["topics"] == ["capture"] and
          r["upstream"].get("shell") is True)
    d = wait_event(a, "capture.state", lambda d: True)
    check("a new listener is told the state: setup", d is not None and list(d) == STATE and word(d) == "idle")
    shell.state = "capturing"
    d = wait_event(a, "capture.state", lambda d: d["state"] == "capturing")
    check("a change of state is an event", d is not None and list(d) == STATE and d["iface"] == "eth0")
    n0 = len(shell.of("capture.status"))
    time.sleep(3.2)
    polls = len(shell.of("capture.status")) - n0
    check("while a client listens the gateway asks about once a second (%d in 3.2 s)" % polls, 2 <= polls <= 5)
    ev = events(a)
    stats = [e["data"] for e in ev if e["event"] == "capture.stats"]
    check("the figures come with it, about once a second (%d)" % len(stats), 2 <= len(stats) <= 6)
    check("each is the contract's fields and no other, of the state that was said",
          all(list(s) == TOP and s["state"] == "capturing" for s in stats))
    check("no event holds anything more of what the unit said", clean(ev))
    check("the gateway told its clients of no other kind of event",
          {e["event"] for e in ev} <= {"capture.state", "capture.stats"})
    b = connect(B, fp, ["capture", "mesh"])
    check("a client without capture_view does not hold the topic", b.sub["topics"] == ["mesh"] and "shell" not in b.sub["upstream"])
    check("and is offered no capability, whatever another client holds", "capture" not in b.caps)
    time.sleep(2.2)
    check("and is sent no event of a capture", not [e for e in events(b) if e["event"].startswith("capture.")])
    b.close()
    a.call("gateway.subscribe", {"topics": []})
    time.sleep(1.2)
    events(a)  # what was sent while it still listened
    n0 = len(shell.of("capture.status"))
    time.sleep(2.4)
    check("nobody listens any more: the gateway stops asking", len(shell.of("capture.status")) == n0)
    check("and sends no figure", not events(a, "capture.stats"))
    a.call("gateway.subscribe", {"topics": ["capture"]})
    d = wait_event(a, "capture.state", lambda d: True)
    check("listening again: the state is said again", d is not None and word(d) == "capturing")

    # ---- 4. capture_view taken away while listening --------------------------------------------------
    scopes(ctl, A, ["status"])
    time.sleep(0.4)
    events(a)
    time.sleep(2.4)
    check("the scope taken away: no further event, though the capture runs on",
          not [e for e in events(a) if e["event"].startswith("capture.")])
    check("and capture.status is not permitted", err_code(a.call("capture.status")) == 7)
    wait_for("nobody may read a capture: the connection to the shell is given up", lambda: shell.live == 0)
    scopes(ctl, A, CONTROL)
    wait_for("capture_control granted: the connection is back", lambda: shell.live == 1)
    a.close()
    a = connect(A, fp, ["capture"])
    check("and the topic is held again", a.sub["topics"] == ["capture"])

    # ---- 5. hold and stop ---------------------------------------------------------------------------------
    n0 = len(shell.calls)
    check("capture.hold with anything but one boolean is refused before the shell (code 2)",
          all(err_code(a.call("capture.hold", p)) == 2 for p in
              [None, {}, {"on": 1}, {"on": "true"}, {"on": True, "request": "aa"}, {"on": True, "client": "x"}]) and
          not shell.of("capture.hold"))
    r = a.call("capture.hold", {"on": True})
    check("capture.hold: {state, changing} and nothing else of the answer", r.get("result") == {"state": "capturing", "changing": True})
    check("the shell was sent {on} and nothing else", shell.of("capture.hold") == [{"on": True}])
    d = wait_event(a, "capture.state", lambda d: d["state"] == "hold")
    check("the hold arrives as a state", d is not None)
    check("capture.stop takes no parameters", err_code(a.call("capture.stop", {"discard": True})) == 2 and not shell.of("capture.stop"))
    r = a.call("capture.stop")
    check("capture.stop: {state, changing}", r.get("result") == {"state": "hold", "changing": True} and shell.of("capture.stop") == [None])
    d = wait_event(a, "capture.state", lambda d: d["state"] == "stopped")
    check("stopped, by the user", d is not None and word(d) == "stopped/user")
    r = a.call("capture.stop")
    check("an error of the unit is passed on as its code alone: the unit's words stay on the unit",
          err_code(r) == 3 and "LEAK" not in json.dumps(r) and "capturing" not in json.dumps(r))
    shell.state, shell.reason = "idle", "none"
    wait_event(a, "capture.state", lambda d: d["state"] == "idle")

    # ---- 6. asking for a start ------------------------------------------------------------------------------
    check("a request with anything but an interface and a limit of the list is refused before the shell (code 2)",
          all(err_code(a.call("capture.request_start", p)) == 2 for p in
              [None, {}, {"iface": "eth0"}, {"limit_s": 900}, dict(ASK, client="evil"), dict(ASK, request="00112233"),
               dict(ASK, keep="full"), dict(ASK, promisc=True), {"iface": "eth0", "limit_s": 0},
               {"iface": "eth0", "limit_s": 120}, {"iface": "eth0; reboot", "limit_s": 900},
               {"iface": "ETH0", "limit_s": 900}]) and not shell.of("capture.request_start"))
    r = a.call("capture.request_start", ASK)
    asked = shell.of("capture.request_start")
    check("a request: {shown: true} and nothing else of the answer", r.get("result") == {"shown": True})
    t1 = asked[0]["request"] if asked else ""
    check("the shell is told the interface, the limit, the name this unit gave the client, and a name for the request",
          len(asked) == 1 and sorted(asked[0]) == ["client", "iface", "limit_s", "request"] and
          asked[0]["iface"] == "eth0" and asked[0]["limit_s"] == 900 and asked[0]["client"] == "phone A" and
          re.fullmatch(r"[0-9a-f]{16}", t1) is not None)
    check("asking started nothing, and the gateway asked for nothing else", shell.state == "idle" and
          {m for m, _ in shell.calls} <= FIVE and not shell.of("capture.request_cancel"))
    check("a second request while one waits is refused before the shell (code 5)",
          err_code(a.call("capture.request_start", ASK)) == 5 and len(shell.of("capture.request_start")) == 1)
    C = ids["c"]
    pair(ctl, C, "phone C", CONTROL)
    c = connect(C, fp, ["capture"])
    check("and so is another client's", err_code(c.call("capture.request_start", ASK)) == 5 and
          len(shell.of("capture.request_start")) == 1)

    # ---- 7. the asking client disconnects: its request is taken back, by its name ---------------------------
    a.close()
    wait_for("the gateway cancels the request of a client that went away", lambda: len(shell.of("capture.request_cancel")) == 1, 4)
    check("naming that request and nothing else", shell.of("capture.request_cancel") == [{"request": t1}])
    d = wait_event(c, "capture.state", lambda d: d["reason"] == "refused")
    check("those who listen are told it was refused", d is not None and word(d) == "idle/refused")
    check("on the unit the sheet is down", shell.request == "refused")
    a = connect(A, fp, ["capture"])
    check("the same client may not ask again within 30 s (code 10), and the shell is not asked",
          err_code(a.call("capture.request_start", ASK)) == 10 and len(shell.of("capture.request_start")) == 1)
    a.close()

    # ---- 8. the asking client is revoked --------------------------------------------------------------------------
    r = c.call("capture.request_start", ASK)
    t2 = shell.of("capture.request_start")[-1]["request"]
    check("another client asks: shown, under a name of its own", r.get("result") == {"shown": True} and t2 != t1 and
          shell.of("capture.request_start")[-1]["client"] == "phone C")
    r = ctl.call("gateway.revoke", {"client": C.fp_hex})
    check("C is revoked with its request waiting", "result" in r and c.closed())
    wait_for("its request is taken back", lambda: len(shell.of("capture.request_cancel")) == 2, 4)
    check("by its own name", shell.of("capture.request_cancel")[-1] == {"request": t2} and shell.request == "refused")

    # ---- 9. the asking client loses capture_control -------------------------------------------------------------
    D = ids["d"]
    pair(ctl, D, "phone D", CONTROL)
    dc = connect(D, fp, ["capture"])
    dc.call("capture.request_start", ASK)
    t3 = shell.of("capture.request_start")[-1]["request"]
    scopes(ctl, D, VIEW)
    wait_for("capture_control taken away: its request is taken back", lambda: len(shell.of("capture.request_cancel")) == 3, 4)
    check("by its own name, though the client is still connected", shell.of("capture.request_cancel")[-1] == {"request": t3})
    check("and it may still read but not ask", "result" in dc.call("capture.status") and
          err_code(dc.call("capture.request_start", ASK)) == 7)
    dc.close()
    ctl.call("gateway.revoke", {"client": D.fp_hex})

    # ---- 10. refused on the unit: nothing to take back, and nothing taken back later ------------------------------
    E = ids["e"]
    pair(ctl, E, "phone E", CONTROL)
    e = connect(E, fp, ["capture"])
    events(e)
    e.call("capture.request_start", ASK)
    t4 = shell.of("capture.request_start")[-1]["request"]
    time.sleep(1.5)
    shell.tap_refuse()
    d = wait_event(e, "capture.state", lambda d: d["reason"] == "refused")
    check("REFUSE tapped on the unit: the client is told", d is not None and word(d) == "idle/refused")
    time.sleep(1.2)
    check("said once", not [x for x in events(e, "capture.state") if x["data"]["reason"] == "refused"])
    e.close()
    time.sleep(1.5)
    check("its client going away afterwards cancels nothing: the request was over",
          len(shell.of("capture.request_cancel")) == 3 and t4 not in json.dumps(shell.of("capture.request_cancel")))
    ctl.call("gateway.revoke", {"client": E.fp_hex})

    # ---- 11. START tapped: a capture is out of a cancel's reach ----------------------------------------------------
    F = ids["f"]
    pair(ctl, F, "phone F", CONTROL)
    f = connect(F, fp, ["capture"])
    events(f)
    f.call("capture.request_start", ASK)
    t5 = shell.of("capture.request_start")[-1]["request"]
    n_stop = len(shell.of("capture.stop"))
    time.sleep(1.5)
    shell.tap_start(starting=2.4)  # the sheet is down, the helper is not capturing yet
    seen = []
    d = wait_event(f, "capture.state", lambda d: seen.append(word(d)) or d["state"] == "capturing", 10)
    check("START tapped on the unit: the client is told it is capturing", d is not None)
    check("and was told of no refusal while the capture was starting (%s)" % seen, not [w for w in seen if "/" in w])
    s = wait_event(f, "capture.stats", lambda d: True)
    check("and that the capture is the one it asked for", s is not None and s["requested_by_companion"] is True and clean(s))
    f.close()
    time.sleep(1.8)
    check("its client going away cancels nothing and stops nothing: a started capture is the person's",
          len(shell.of("capture.request_cancel")) == 3 and len(shell.of("capture.stop")) == n_stop and
          shell.state == "capturing" and t5 not in json.dumps(shell.of("capture.request_cancel")))
    shell.state, shell.by_companion = "idle", False

    # ---- 11b. START tapped, and the client gone before the gateway has looked again -------------------------------
    I = ids["i"]
    pair(ctl, I, "phone I", CONTROL)
    a = connect(A, fp, ["capture"])  # another client that listens
    ic = connect(I, fp)
    wait_event(a, "capture.state", lambda d: d["state"] == "idle")
    events(a)
    n_can = len(shell.of("capture.request_cancel"))
    ic.call("capture.request_start", ASK)
    t8 = shell.of("capture.request_start")[-1]["request"]
    time.sleep(1.3)
    shell.tap_start()
    ic.close()  # at once: the gateway still believes the request is waiting
    seen = []
    d = wait_event(a, "capture.state", lambda d: seen.append(word(d)) or d["state"] == "capturing", 8)
    time.sleep(1.5)
    seen += [word(x["data"]) for x in events(a, "capture.state")]
    check("the gateway takes back what it believes is waiting, by its name",
          shell.of("capture.request_cancel")[n_can:] == [{"request": t8}])
    check("the unit had no request of that name any more, and its capture runs on", shell.state == "capturing")
    check("and nobody is told of a refusal that did not happen (%s)" % seen, d is not None and not [w for w in seen if "/" in w])
    a.close()
    shell.state, shell.by_companion = "idle", False

    # ---- 12. a cancel that overtakes a slow answer ----------------------------------------------------------------
    Gc = ids["g"]
    pair(ctl, Gc, "phone G", CONTROL)
    g = connect(Gc, fp)
    shell.delay = 0.7
    n_req, n_can = len(shell.of("capture.request_start")), len(shell.of("capture.request_cancel"))
    g.send("capture.request_start", ASK)
    time.sleep(0.15)
    g.close()  # gone before the unit has answered
    wait_for("the request of a client that left before the answer is taken back",
             lambda: len(shell.of("capture.request_cancel")) == n_can + 1, 5)
    shell.delay = 0.0
    t6 = shell.of("capture.request_start")[-1]["request"]
    order = [m for m, _ in shell.calls if m in ("capture.request_start", "capture.request_cancel")][-2:]
    check("the cancel follows the request it names, on the same connection, and names it",
          len(shell.of("capture.request_start")) == n_req + 1 and order == ["capture.request_start", "capture.request_cancel"] and
          shell.of("capture.request_cancel")[-1] == {"request": t6})
    wait_for("so no sheet is left up on the unit", lambda: shell.request == "refused", 4)

    # ---- 12b. an answer that does not come in time: the request is taken back, not forgotten -------------------------
    J = ids["j"]
    pair(ctl, J, "phone J", CONTROL)
    j = connect(J, fp)
    shell.delay = 3.0  # longer than the gateway waits for the shell
    n_can = len(shell.of("capture.request_cancel"))
    t0 = time.time()
    r = j.call("capture.request_start", ASK)
    check("a request the shell does not answer in time is upstream unavailable (code 9, %.1f s)" % (time.time() - t0),
          err_code(r) == 9 and 1.5 <= time.time() - t0 <= 4.0)
    t9 = shell.of("capture.request_start")[-1]["request"]
    wait_for("the gateway takes it back all the same, by its name, once the shell can be told",
             lambda: {"request": t9} in shell.of("capture.request_cancel")[n_can:], 15)
    shell.delay = 0.0
    wait_for("so the sheet the slow shell raised after all does not stay up for a client that was told no",
             lambda: shell.request == "refused", 6)
    check("and nothing started", shell.state == "idle")
    j.close()
    wait_for("the shell is reached again", lambda: shell.live == 1 and "result" in ctl.call("gateway.status"), 8)
    time.sleep(1.0)

    # ---- 13. a request nobody answers: the gateway's own time ---------------------------------------------------------
    H = ids["h"]
    pair(ctl, H, "phone H", CONTROL)
    h = connect(H, fp, ["capture"])
    events(h)
    n_can = len(shell.of("capture.request_cancel"))
    t_ask = time.time()
    h.call("capture.request_start", ASK)
    t7 = shell.of("capture.request_start")[-1]["request"]
    d = None
    while time.time() - t_ask < 75 and d is None:  # the client stays, and keeps its connection alive
        d = wait_event(h, "capture.state", lambda d: d["reason"] in ("timeout", "refused"), 5)
    took = time.time() - t_ask
    check("after its time the gateway takes the request back (%.0f s)" % took,
          58 <= took <= 70 and len(shell.of("capture.request_cancel")) == n_can + 1 and
          shell.of("capture.request_cancel")[-1] == {"request": t7})
    check("and tells those who listen: timeout", d is not None and word(d) == "idle/timeout")
    check("no capture came of any request that was not tapped", shell.state == "idle")

    # ---- 14. a shell that does not answer --------------------------------------------------------------------------------
    conns0 = len(shell.conns)
    shell.mode = "stall"
    t0 = time.time()
    rid = h.send("capture.status")
    p0 = time.time()
    pong = h.call("gateway.ping")
    check("the gateway answers others at once while the shell is silent (%.2f s)" % (time.time() - p0),
          "result" in pong and time.time() - p0 < 0.6)
    r = h.read(rid)
    check("the request is given up after its time, as upstream unavailable (code 9, %.1f s)" % (time.time() - t0),
          err_code(r) == 9 and 1.5 <= time.time() - t0 <= 4.0)
    wait_for("a shell that does not answer in time loses the connection, which is made again", lambda: len(shell.conns) > conns0, 15)
    shell.mode = "ok"
    wait_for("and when the shell answers again so does the gateway", lambda: "result" in h.call("capture.status"), 10)

    # ---- 15. the shell goes away and comes back ---------------------------------------------------------------------------
    shell.state = "capturing"
    d = wait_event(h, "capture.state", lambda d: d["state"] == "capturing")
    check("a capture the client knows of", d is not None)
    shell.stop()
    d = wait_event(h, "capture.state", lambda d: d["state"] == "ended")
    check("the shell is gone: the capture's last word to those who listened", d is not None and word(d) == "ended/left")
    up = [x["data"] for x in events(h, "gateway.upstream")]
    check("and they are told the shell is out of reach", {"service": "shell", "connected": False} in up)
    check("and its methods are unavailable (code 9), at once", err_code(h.call("capture.status")) == 9)
    events(h)  # what was sent before it went
    time.sleep(1.6)
    check("no figure follows", not events(h, "capture.stats"))
    shell = FakeShell()
    wait_for("the shell is back: the gateway finds it again by itself", lambda: "result" in h.call("capture.status"), 12)
    d = wait_event(h, "capture.state", lambda d: True)
    check("and says how the capture is now", d is not None and word(d) == "idle")
    up = [x["data"] for x in events(h, "gateway.upstream")]
    check("and that the shell is in reach again", {"service": "shell", "connected": True} in up)
    h.close()

    # ---- 16. a unit without PacketShark -----------------------------------------------------------------------------------------
    shell.stop()
    shell = FakeShell(present=False)
    wait_for("a shell without PacketShark is asked once", lambda: len(shell.calls) >= 1, 12)
    time.sleep(2.5)
    h = connect(H, fp, ["capture"])
    check("the subscription says the shell does not answer, and its methods are unavailable (code 9)",
          h.sub["upstream"].get("shell") is False and err_code(h.call("capture.status")) == 9 and
          err_code(h.call("capture.request_start", ASK)) == 9)
    time.sleep(2.2)
    check("it is not asked again and again", len(shell.calls) == 1 and shell.calls[0][0] == "capture.status")
    h.close()

    # ---- 17. what was asked of the shell, ever; and the log ------------------------------------------------------------------------
    err = stop()
    log = err
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as fh:
            log += fh.read()
    check("the log has the lifecycle of the connection", "PacketShark answers on the shell's socket" in log and
          "a start request was withdrawn" in log)
    check("the log holds no figure, no name of a request or a client, nothing of the unit's answers",
          all(w not in log for w in ("LEAK", "192.0.2", "phone A", "phone C", t1, t2, t3, t7, t8, t9, "eth0")))
except Exception as ex:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % ex, False)
finally:
    if lib.proc is not None:
        lib.proc.kill()
        print(lib.proc.stderr.read().decode(errors="replace")[-2000:])
    for svc in (shell, meshd):
        if svc is not None:
            svc.stop()
    if lib.failed and os.environ.get("GATEWAYD_E2E_KEEP"):
        print("  kept:", ROOT)
    else:
        shutil.rmtree(ROOT, ignore_errors=True)

print("gatewayd capture e2e: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
