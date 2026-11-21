#!/usr/bin/env python3
"""gatewayd, the mesh group (M4.1), end to end: the real service (the
test-hooks build) between a fake meshcored on a real pocketipc socket and
real TLS clients, on loopback.

  python3 tests/gatewayd_mesh_e2e_test.py <path to gatewayd-testhooks>

What is driven: when the gateway reads the mesh at all, what it asks
meshcored for (and that it never asks for the receive log, a send or a
change), the mesh_read scope on every method and topic, the result filters,
parameters, mesh.nodes in pages, gateway.subscribe and each event, the
journal and its cursor, a repeated event, a lost and regained meshcored in
the same run and in a new one, the ring wrapping under a client that was
away, a client that does not read, a meshcored that does not answer, the
limit of waiting requests, a meshcored that answers wrongly, a scope taken
away while connected, and what the log holds.

Pairing, TLS and the session rules themselves are tests/gatewayd_e2e_test.py.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403

K1 = "a1" + "00" * 30 + "01"
K2 = "b2" + "00" * 30 + "02"
K3 = "c3" + "00" * 30 + "03"
TEXT = "hello from the mesh"
OWN = ("mesh.status", "mesh.subscribe", "mesh.messages")  # the gateway's own calls
READ = ("mesh.identity", "mesh.nodes", "mesh.node", "mesh.channels", "mesh.path_preview")


def message(i, state="received", text=TEXT, **more):
    m = {"id": i, "direction": "in", "kind": "direct", "peer_public_key": K1, "peer_name": "Ola Nordmann",
         "text": text, "timestamp": 1700000000 + i, "mono_ms": 1000 + i, "state": state, "ack_expected": False,
         # What no meshcored reports today, and what must not get through if one ever did.
         "ack_hash": "LEAK", "session_key": "LEAK", "raw_hex": "LEAK"}
    m.update(more)
    return m


def node(i, **more):
    n = {"public_key": "%064x" % (0x1000 + i), "node_hash": "10", "name": "node-%d" % i, "type": 1,
         "path_known": False, "last_heard_mono_ms": 5000 - i, "shared_secret": "LEAK", "slot": i}
    n.update(more)
    return n


class FakeMesh(FakeService):
    """A meshcored: answers the reading methods, takes a subscription, raises
    the events a test tells it to. stall: answers nothing but the gateway's
    own three calls."""

    ever = []  # every request any instance was sent

    def __init__(self, run_id, messages=(), nodes=(), status=None):
        self.run_id = run_id
        self.messages = list(messages)
        self.nodes = list(nodes)
        self.status = status
        self.subs = []
        self.lock = threading.Lock()
        super().__init__("meshcored", {})

    def answer(self, m, p):
        if m == "mesh.status":
            return self.status if self.status is not None else {"state": "online", "run_id": self.run_id, "uptime_s": 5}
        if m == "mesh.subscribe":
            return {"subscribed": True}
        if m == "mesh.messages":
            return {"messages": self.messages, "count": len(self.messages), "persistent": False}
        if m == "mesh.identity":
            return {"public_key": K3, "node_hash": "c3", "name": "unit", "name_source": "stored", "name_max": 31,
                    "private_key": "LEAK", "prv_hex": "LEAK", "state_file": "LEAK"}
        if m == "mesh.nodes":
            return {"nodes": self.nodes, "count": len(self.nodes), "debug": "LEAK"}
        if m == "mesh.node":
            if p == {"node": "ffff"}:
                return (2, "LEAK the service's own words")
            return node(1)
        if m == "mesh.channels":
            return {"channels": [{"channel": 0, "name": "Public", "channel_hash": "11", "key_bits": 128,
                                  "well_known": "public", "text_limit": 140, "ack_expected": False,
                                  "key": "LEAK", "secret_b64": "LEAK"}],
                    "count": 1, "max": 8, "persistent": True, "store_path": "LEAK"}
        if m == "mesh.path_preview":
            return {"valid": True, "route": "explicit", "relays": len(p["via"]), "hash_bytes": 1, "path_bytes": 1,
                    "path_hex": "b2", "transmitted": False, "link_evidence_verified": False, "internal": "LEAK"}
        return (1, "unknown method")

    def serve(self, c):
        try:
            while True:
                req = recv_frame(c)
                m, p = req.get("method"), req.get("params")
                self.calls.append((m, p))
                FakeMesh.ever.append((m, p))
                if self.mode == "stall" and m not in OWN:
                    continue
                res = self.answer(m, p)
                with self.lock:
                    if isinstance(res, tuple):
                        send_frame(c, {"id": req["id"], "error": {"code": res[0], "message": res[1]}})
                    else:
                        send_frame(c, {"id": req["id"], "result": res})
                    if m == "mesh.subscribe":
                        self.subs.append(c)
        except (EOFError, OSError, ValueError):
            c.close()

    def emit(self, event, data):
        with self.lock:
            for c in list(self.subs):
                try:
                    send_frame(c, {"event": event, "data": data})
                except OSError:
                    self.subs.remove(c)

    def raw(self, data):
        with self.lock:
            for c in list(self.subs):
                try:
                    c.sendall(data)
                except OSError:
                    pass

    def methods(self):
        return [m for m, _ in self.calls]


def wait_for(what, cond, timeout=12):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    check(what, False)
    return False


def mesh_status(ctl):
    return ctl.call("gateway.status")["result"]["mesh"]


def connect(ident, fp, topics=None, **kw):
    c = Client(ident, pin=fp, **kw)
    h = c.hello()
    assert h["result"]["paired"] is True, h
    if topics is not None:
        c.call("gateway.subscribe", {"topics": topics})
    return c


def drain(c):
    """Takes what the unit has sent so far: a ping's answer comes after it."""
    c.call("gateway.ping")
    out, c.events = c.events, []
    return out


def wait_events(c, name, n=1, timeout=6):
    """n events of this name, or as many as came in time."""
    got = []
    end = time.time() + timeout
    while True:
        c.call("gateway.ping")
        got += [e for e in c.events if e["event"] == name]
        c.events = [e for e in c.events if e["event"] != name]
        if len(got) >= n or time.time() >= end:
            return got
        time.sleep(0.1)


def read_all(c, rids, timeout=12):
    """The answers to these requests, in the order and at the time they came."""
    out = []
    want = set(rids)
    t0 = time.time()
    c.s.settimeout(timeout)
    while want:
        msg = recv_frame(c.s)
        if "event" in msg:
            c.events.append(msg)
        elif msg.get("id") in want:
            want.discard(msg["id"])
            out.append((msg["id"], time.time() - t0, msg))
    c.s.settimeout(5)
    return out


A = Identity("a")
B = Identity("b")
mesh = None

try:
    print("gatewayd mesh e2e")
    set_password(True)
    mesh = FakeMesh("run1", [message(1), message(2, "sent_direct", direction="out"), message(3)],
                    [node(i) for i in range(250)])
    ctl = start()
    ctl.call("gateway.subscribe")  # the local events: a candidate is announced there

    # ---- 1. nobody may read the mesh: it is not read ----------------------------
    fp, d, _ = pair(ctl, B, "status only", ["status"])
    check("B is paired with status alone", d["result"]["paired"] is True)
    time.sleep(1.0)
    st = mesh_status(ctl)
    check("no client holds mesh_read: no connection to meshcored, nothing asked",
          st == {"wanted": False, "connected": False, "journal": 0, "seq": 0} and mesh.calls == [])
    b = connect(B, fp)
    check("a method of the mesh group is not permitted without mesh_read (code 7), each of them",
          all(err_code(b.call(m, p)) == 7 for m, p in
              [("mesh.identity", None), ("mesh.nodes", None), ("mesh.node", {"node": "ab"}), ("mesh.channels", None),
               ("mesh.path_preview", {"to": K1, "via": []}), ("mesh.sync", None)]))
    r = b.call("gateway.subscribe", {"topics": ["mesh", "mesh.activity", "device"]})
    check("its subscription holds no topic and says nothing of the mesh", r.get("result") == {"topics": []})
    check("and none of that reached meshcored", mesh.calls == [])

    # ---- 2. a client with mesh_read: connect, identify the run, subscribe, fill ----
    _, d, _ = pair(ctl, A, "reader")
    check("A is paired with the default scopes", d["result"]["paired"] is True)
    wait_for("the gateway connects to meshcored once a client may read", lambda: mesh_status(ctl)["connected"])
    check("it asks who it is, subscribes, then takes the messages - in that order, nothing else",
          mesh.methods() == list(OWN))
    check("the subscription asks for nothing: no receive log", mesh.calls[1][1] is None)
    st = mesh_status(ctl)
    check("the journal holds what meshcored held", st["journal"] == 3 and st["seq"] == 3 and st["wanted"] is True)
    a = Client(A, pin=fp)
    h = a.hello()["result"]
    check("hello names the mesh capabilities", {"status", "mesh", "mesh.activity"} <= set(h["capabilities"]) and
          "mesh_read" in h["scopes"])

    # ---- 3. the reading methods ---------------------------------------------------
    r = a.call("mesh.identity")
    check("mesh.identity: the public half and the name, nothing else",
          r.get("result") == {"public_key": K3, "node_hash": "c3", "name": "unit", "name_source": "stored", "name_max": 31}
          and "mono_ms" in r)
    r = a.call("mesh.channels")["result"]
    check("mesh.channels: no key under any name",
          r == {"channels": [{"channel": 0, "name": "Public", "channel_hash": "11", "key_bits": 128,
                              "well_known": "public", "text_limit": 140, "ack_expected": False}],
                "count": 1, "max": 8, "persistent": True})
    n0 = len(mesh.calls)
    r = a.call("mesh.node", {"node": "1001"})["result"]
    check("mesh.node: the documented fields only", r["name"] == "node-1" and "shared_secret" not in r and "slot" not in r)
    check("what reached meshcored is the checked parameter and only that", mesh.calls[n0] == ("mesh.node", {"node": "1001"}))
    r = a.call("mesh.node", {"node": "ffff"})
    check("an upstream refusal keeps its code and loses its words",
          err_code(r) == 2 and r["error"]["message"] == "upstream error")
    r = a.call("mesh.path_preview", {"to": K1, "via": [K2]})
    check("mesh.path_preview is passed on and says it transmitted nothing",
          r["result"] == {"valid": True, "route": "explicit", "relays": 1, "hash_bytes": 1, "path_bytes": 1,
                          "path_hex": "b2", "transmitted": False, "link_evidence_verified": False} and
          mesh.calls[-1] == ("mesh.path_preview", {"to": K1, "via": [K2]}))
    n0 = len(mesh.calls)
    bad = [("mesh.node", None), ("mesh.node", {}), ("mesh.node", {"node": "abc"}), ("mesh.node", {"node": "zz"}),
           ("mesh.node", {"node": "ab", "remove": True}), ("mesh.identity", {"verbose": True}),
           ("mesh.channels", {"keys": True}), ("mesh.path_preview", {"to": "ab", "via": []}),
           ("mesh.path_preview", {"to": K1}), ("mesh.path_preview", {"to": K1, "via": [K2], "text": "x"}),
           ("mesh.nodes", {"limit": 101}), ("mesh.nodes", {"offset": -1}), ("mesh.nodes", {"all": True}),
           ("mesh.sync", {"limit": 0}), ("mesh.sync", {"after_seq": "1"}), ("mesh.sync", {"after_seq": 1.5}),
           ("gateway.subscribe", None), ("gateway.subscribe", {"topics": "mesh"}),
           ("gateway.subscribe", {"topics": ["mesh"], "rx_log": True})]
    check("invalid parameters are code 2, each of them", all(err_code(a.call(m, p)) == 2 for m, p in bad))
    check("and none reached meshcored", len(mesh.calls) == n0)
    time.sleep(1.5)  # the request budget (40, then 20 a second) is not what is under test
    gone = ["mesh.send", "mesh.resend", "mesh.messages", "mesh.subscribe", "mesh.unsubscribe", "mesh.info",
            "mesh.channel", "mesh.channel_add", "mesh.channel_remove", "mesh.set_name", "mesh.node_remove",
            "mesh.node_reset_path", "mesh.advert", "mesh.discover", "mesh.discovered", "mesh.remote_login",
            "mesh.remote_session", "mesh.app_send", "mesh.app_inbox", "mesh.path_hash", "mesh.set_path_hash",
            "capture.status"]
    check("sending, changing and everything unapproved does not exist (code 1)",
          all(err_code(a.call(m, {"to": K1, "text": "x", "client_msg_id": "0123456789abcdef"})) == 1 for m in gone))
    check("and none reached meshcored", len(mesh.calls) == n0)

    # mesh.nodes in pages, from one upstream list.
    time.sleep(1.5)
    n0 = len(mesh.calls)
    r1 = a.send("mesh.nodes")
    r2 = a.send("mesh.nodes", {"offset": 100, "limit": 100})
    r3 = a.send("mesh.nodes", {"offset": 200})
    got = {rid: m for rid, _, m in read_all(a, [r1, r2, r3])}
    p1, p2, p3 = got[r1]["result"], got[r2]["result"], got[r3]["result"]
    check("mesh.nodes: 100 a page, the total, the run",
          (p1["count"], p2["count"], p3["count"]) == (100, 100, 50) and p1["total"] == 250 and p1["run_id"] == "run1")
    names = [n["name"] for n in p1["nodes"] + p2["nodes"] + p3["nodes"]]
    check("the pages are the list, in order, each node once", names == ["node-%d" % i for i in range(250)])
    check("a node carries only documented fields", all("shared_secret" not in n and "slot" not in n for n in p1["nodes"]))
    r = a.call("mesh.nodes", {"offset": 5000})["result"]
    check("a page past the end is empty, not an error", r["count"] == 0 and r["total"] == 250)
    check("three pages asked at once and one more cost one upstream call", mesh.methods()[n0:] == ["mesh.nodes"])

    # ---- 4. the journal and its cursor --------------------------------------------
    s = a.call("mesh.sync")["result"]
    jid = s["journal_id"]
    check("mesh.sync without a cursor: what is held, oldest first",
          [e["seq"] for e in s["messages"]] == [1, 2, 3] and [e["message"]["id"] for e in s["messages"]] == [1, 2, 3]
          and s["seq"] == 3 and s["first_seq"] == 1 and s["more"] is False and "gap" not in s and s["run_id"] == "run1")
    check("a message keeps its text and its peer and loses what is not documented",
          s["messages"][0]["message"]["text"] == TEXT and s["messages"][0]["message"]["peer_public_key"] == K1
          and "LEAK" not in json.dumps(s))
    s = a.call("mesh.sync", {"after_seq": 1, "limit": 1})["result"]
    check("with a cursor and a limit: the next one, and more to come",
          [e["seq"] for e in s["messages"]] == [2] and s["more"] is True)
    s = a.call("mesh.sync", {"after_seq": 3})["result"]
    check("up to date: nothing, no gap", s["messages"] == [] and "gap" not in s and s["more"] is False)
    s = a.call("mesh.sync", {"after_seq": 99})["result"]
    check("a cursor this journal never gave is a gap", s.get("gap") is True and len(s["messages"]) == 3)
    check("mesh.sync is answered from memory", mesh.methods().count("mesh.messages") == 1)

    # ---- 5. events ------------------------------------------------------------------
    r = a.call("gateway.subscribe", {"topics": ["mesh", "mesh.activity", "device", "capture", "nonsense"]})["result"]
    check("gateway.subscribe: the topics served and allowed, and how meshcored is now",
          r == {"topics": ["mesh", "mesh.activity"], "upstream": {"meshcored": True}})
    drain(b)
    mesh.emit("mesh.message", {"message": message(4)})
    ev = wait_events(a, "mesh.message")
    check("a message arrives as an event with its seq, run and journal",
          len(ev) == 1 and ev[0]["data"]["seq"] == 4 and ev[0]["data"]["message"]["id"] == 4 and
          ev[0]["data"]["run_id"] == "run1" and ev[0]["data"]["journal_id"] == jid and "mono_ms" in ev[0] and
          "LEAK" not in json.dumps(ev))
    mesh.emit("mesh.message", {"message": message(4)})
    mesh.emit("mesh.message", {"message": message(4)})
    mesh.emit("mesh.message", {"message": message(4, "acked", ack_mono_ms=7)})
    ev = wait_events(a, "mesh.message", 2, timeout=2)
    check("the same event again is not a second message; a change of state is, under a new seq",
          [(e["data"]["seq"], e["data"]["message"]["state"]) for e in ev] == [(5, "acked")])
    s = a.call("mesh.sync", {"after_seq": 3})["result"]
    check("and the journal says the same", [(e["seq"], e["message"]["id"]) for e in s["messages"]] == [(4, 4), (5, 4)])

    for i in range(6):
        mesh.emit("mesh.node", {"reason": "path", "node": node(7, hops=i)})
    mesh.emit("mesh.node", {"reason": "discovered", "node": node(8)})
    time.sleep(0.5)
    ev = wait_events(a, "mesh.node", 3, timeout=1.5)
    check("mesh.node: one per node at once, the rest of the burst held back",
          [(e["data"]["node"]["name"], e["data"]["node"].get("hops")) for e in ev] == [("node-7", 0), ("node-8", None)]
          and "LEAK" not in json.dumps(ev))
    mesh.emit("mesh.node", {"reason": "removed", "node": node(7)})
    ev = wait_events(a, "mesh.node", 1, timeout=2)
    check("a removal is never held back", len(ev) == 1 and ev[0]["data"]["reason"] == "removed")
    mesh.emit("mesh.channel", {"reason": "added", "channel": {"channel": 1, "name": "ops", "channel_hash": "22",
                                                               "key_bits": 128, "key": "LEAK", "secret": "LEAK"}})
    mesh.emit("mesh.state", {"state": "degraded", "reason": "radio", "mono_ms": 9, "lease_owner_id": 4})
    ev = wait_events(a, "mesh.channel") + wait_events(a, "mesh.state")
    check("mesh.channel and mesh.state pass, reduced",
          [e["data"] for e in ev] == [{"reason": "added", "channel": {"channel": 1, "name": "ops", "channel_hash": "22", "key_bits": 128}},
                                      {"state": "degraded", "reason": "radio", "mono_ms": 9}])

    for i in range(5):
        mesh.emit("mesh.activity", {"kind": "rx", "payload_type": "advert", "bytes": 100, "rssi_dbm": -91, "snr_db": 2.5})
    mesh.emit("mesh.activity", {"kind": "tx", "submit_id": 3, "result": "ok", "bytes": 40})
    for name in ("mesh.rx", "mesh.app", "mesh.remote", "mesh.discover", "mesh.secret", "gateway.revoked"):
        mesh.emit(name, {"v": 1, "from": K1, "payload_hex": "LEAK", "text": "LEAK", "hash": "LEAK"})
    ev = wait_events(a, "mesh.activity_bin", 1, timeout=4)
    check("activity arrives as one bin of counts",
          len(ev) == 1 and ev[0]["data"]["rx_frames"] == 5 and ev[0]["data"]["rx_bytes"] == 500 and
          ev[0]["data"]["tx_ok"] == 1 and ev[0]["data"]["by_type"] == {"advert": 5} and "rssi_dbm" not in ev[0]["data"])
    time.sleep(1.2)
    rest = drain(a)
    check("no frame, no receive log, no datagram, no repeater event, nothing unknown is passed on",
          rest == [] and "LEAK" not in json.dumps(rest))
    check("a client without mesh_read received none of it", drain(b) == [])
    r = a.call("gateway.subscribe", {"topics": ["mesh.activity"]})["result"]
    mesh.emit("mesh.message", {"message": message(5)})
    time.sleep(0.4)
    check("a new subscription replaces the old: no mesh topic, no message event",
          r["topics"] == ["mesh.activity"] and drain(a) == [])
    check("the journal took it all the same", mesh_status(ctl)["seq"] == 6)

    # ---- 6. a scope taken away while connected ---------------------------------------
    a.call("gateway.subscribe", {"topics": ["mesh", "mesh.activity"]})
    r = ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["status"]})
    check("the unit reduces A to status", "result" in r)
    mesh.emit("mesh.message", {"message": message(6)})
    time.sleep(0.4)
    ev = drain(a)
    check("A is told, and receives no mesh event from then on", [e["event"] for e in ev] == ["gateway.scopes"])
    check("nor an answer", err_code(a.call("mesh.sync")) == 7 and err_code(a.call("mesh.identity")) == 7)
    wait_for("with nobody left to read it, the mesh is let go and the journal emptied",
             lambda: {k: v for k, v in mesh_status(ctl).items() if k != "seq"} ==
             {"wanted": False, "connected": False, "journal": 0})
    base = mesh_status(ctl)["seq"]  # one is left out; and one more when the last event still came in
    check("the sequence goes on past what was held: no seq is used twice, every old cursor is a gap", base in (7, 8))
    r = ctl.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["status", "mesh_read"]})
    wait_for("given back, the gateway connects again", lambda: mesh_status(ctl)["connected"])
    drain(a)
    mesh.emit("mesh.message", {"message": message(8)})
    time.sleep(0.4)
    check("the subscription did not come back with the scope", drain(a) == [])
    check("asked for again, it is held", a.call("gateway.subscribe", {"topics": ["mesh"]})["result"]["topics"] == ["mesh"])
    s = a.call("mesh.sync", {"after_seq": 5})["result"]
    check("the journal was filled again under new numbers; the old cursor is a gap",
          s.get("gap") is True and s["journal_id"] == jid and s["first_seq"] == base + 1 and
          [e["message"]["id"] for e in s["messages"]] == [1, 2, 3, 8])

    # ---- 7. meshcored away and back, in the same run -----------------------------------
    cur = a.call("mesh.sync")["result"]["seq"]
    drain(a)
    kept = mesh.messages
    mesh.stop()
    ev = wait_events(a, "gateway.upstream", 1)
    check("meshcored gone: the client is told", len(ev) == 1 and ev[0]["data"] == {"service": "meshcored", "connected": False})
    t0 = time.time()
    r = a.call("mesh.identity")
    check("a method that needs it answers code 9 at once", err_code(r) == 9 and time.time() - t0 < 0.3 and
          r["error"]["message"] == "upstream unavailable")
    check("mesh.nodes too", err_code(a.call("mesh.nodes")) == 9)
    s = a.call("mesh.sync", {"after_seq": cur})["result"]
    check("mesh.sync still answers from the journal", s["messages"] == [] and "gap" not in s and s["run_id"] == "run1")
    # It comes back as the same run, having received one message and seen one acknowledged meanwhile.
    mesh = FakeMesh("run1", kept[:1] + [message(2, "acked", direction="out", ack_mono_ms=9)] + kept[2:] + [message(7)],
                    [node(i) for i in range(3)])
    ev = wait_events(a, "gateway.upstream", 1, timeout=12)
    check("back: the client is told, with the run", len(ev) == 1 and ev[0]["data"] ==
          {"service": "meshcored", "connected": True, "run_id": "run1", "journal_id": jid})
    ev = wait_events(a, "mesh.message", 2, timeout=2)
    check("what was missed arrives - the changed one and the new one - and nothing is repeated",
          [(e["data"]["message"]["id"], e["data"]["message"]["state"]) for e in ev] == [(2, "acked"), (7, "received")])
    s = a.call("mesh.sync", {"after_seq": cur})["result"]
    check("the cursor from before the loss continues without a gap",
          "gap" not in s and [e["seq"] for e in s["messages"]] == [cur + 1, cur + 2])
    check("it subscribed again, without the receive log", mesh.calls[:3] == [(m, None) for m in OWN])

    # ---- 8. meshcored starts again: a new run ---------------------------------------------
    cur = s["seq"]
    drain(a)
    mesh.stop()
    wait_events(a, "gateway.upstream", 1)
    mesh = FakeMesh("run2", [message(1, text="first of the new run")], [node(i) for i in range(3)])
    ev = wait_events(a, "gateway.upstream", 1, timeout=12)
    check("a new run is announced as one", len(ev) == 1 and ev[0]["data"].get("run_id") == "run2")
    ev = wait_events(a, "mesh.message", 1, timeout=2)
    check("its message id 1 is a new message under a new seq, not the old id 1",
          len(ev) == 1 and ev[0]["data"]["seq"] == cur + 1 and ev[0]["data"]["run_id"] == "run2" and
          ev[0]["data"]["message"]["text"] == "first of the new run")
    s = a.call("mesh.sync", {"after_seq": cur - 1})["result"]
    check("a cursor of the old run is a gap, and the answer names the new run",
          s.get("gap") is True and s["run_id"] == "run2" and s["journal_id"] == jid and s["first_seq"] == cur + 1
          and [e["message"]["id"] for e in s["messages"]] == [1])
    s = a.call("mesh.sync")["result"]
    check("nothing of the old run is left in the journal", len(s["messages"]) == 1 and mesh_status(ctl)["journal"] == 1)
    check("mesh.nodes is asked afresh after a restart", a.call("mesh.nodes")["result"]["total"] == 3)

    # ---- 9. a client that was away while the ring wrapped ------------------------------------
    cur = s["seq"]
    a.call("gateway.subscribe", {"topics": []})
    for i in range(600):
        mesh.emit("mesh.message", {"message": message(100 + i)})
    wait_for("600 messages are journaled", lambda: mesh_status(ctl)["seq"] == cur + 600)
    check("512 are held", mesh_status(ctl)["journal"] == 512)
    s = a.call("mesh.sync", {"after_seq": cur})["result"]
    check("the cursor is older than what is held: a gap, and the answer starts at the oldest held",
          s.get("gap") is True and s["first_seq"] == cur + 600 - 511 and s["messages"][0]["seq"] == s["first_seq"]
          and len(s["messages"]) == 100 and s["more"] is True)
    seqs, at = [], s["first_seq"] - 1
    while True:
        s = a.call("mesh.sync", {"after_seq": at})["result"]
        seqs += [e["seq"] for e in s["messages"]]
        if not s["more"]:
            break
        at = seqs[-1]
    check("paging by cursor reads all 512 once, in order, with no gap",
          seqs == list(range(cur + 89, cur + 601)) and "gap" not in s)

    # ---- 10. a meshcored that does not answer; the limit of waiting requests ------------------
    a.call("gateway.subscribe", {"topics": ["mesh"]})
    mesh.mode = "stall"
    rids = [a.send("mesh.identity") for _ in range(9)]
    ping = a.send("gateway.ping")
    t0 = time.time()
    check("meanwhile the loop serves everyone else at once", "result" in b.call("gateway.ping") and time.time() - t0 < 0.5)
    check("and the local socket", "result" in ctl.call("gateway.status") and time.time() - t0 < 0.5)
    got = read_all(a, rids + [ping])
    order = [rid for rid, _, _ in got]
    when = {rid: t for rid, t, _ in got}
    check("eight wait at a time: each is answered code 9 when its two seconds are over",
          all(err_code(m) == 9 for rid, _, m in got if rid != ping) and 1.5 < when[rids[0]] < 3.5)
    check("nothing further was read until one completed: the ping behind them waited",
          when[ping] > 1.5 and order.index(ping) > order.index(rids[7]))
    ev = wait_events(a, "gateway.upstream", 2, timeout=12)
    check("a meshcored that sent nothing while asked is given up, the clients are told, and it is taken up again",
          [e["data"]["connected"] for e in ev][:2] == [False, True] and mesh.methods().count("mesh.status") >= 2)
    mesh.mode = "ok"
    time.sleep(2.5)  # the ninth request, asked of the new connection, may have ended that one too
    wait_for("it is connected again", lambda: mesh_status(ctl)["connected"])
    drain(a)
    check("it answers again: so does the gateway", "result" in a.call("mesh.identity"))

    # ---- 11. a client that does not read -------------------------------------------------------
    slow = connect(A, fp, ["mesh"], rcvbuf=4096)  # replaces A's first connection
    check("a second connection of the same client replaced the first", a.closed())
    subs = mesh.methods().count("mesh.subscribe")
    big = "x" * 7000
    for i in range(700):
        mesh.emit("mesh.message", {"message": message(1000 + i, text=big)})
    wait_for("the flood is journaled", lambda: mesh_status(ctl)["seq"] >= cur + 600 + 700, timeout=30)
    time.sleep(0.5)
    st = ctl.call("gateway.status")["result"]
    check("the client that fell behind was dropped; the gateway and its connection to meshcored were not",
          st["sessions"] == 1 and st["mesh"]["connected"] is True and st["mesh"]["journal"] == 512)
    check("meshcored was never held up by it: the same connection throughout",
          mesh.methods().count("mesh.subscribe") == subs)
    check("the other client is served", "result" in b.call("gateway.ping"))
    slow.close()
    a = connect(A, fp, ["mesh"])
    s = a.call("mesh.sync")["result"]
    check("it returns and syncs: an answer under the frame limit, the newest messages",
          0 < len(s["messages"]) < 100 and s["messages"][-1]["seq"] == s["seq"] and len(json.dumps(s)) < 256 * 1024)

    rids = [a.send("mesh.sync") for _ in range(5)]
    got = read_all(a, rids, timeout=20)
    check("five such answers asked for at once (near 1 MiB) are each delivered: requests wait for the output, the client is not dropped",
          len(got) == 5 and all(len(m["result"]["messages"]) > 0 for _, _, m in got) and "result" in a.call("gateway.ping"))

    # ---- 12. a meshcored that answers wrongly ----------------------------------------------------
    drain(a)
    mesh.raw(b"\x00\x00\x00\x05nope!")  # a frame that is not JSON
    ev = wait_events(a, "gateway.upstream", 2, timeout=12)
    check("a frame that is not JSON ends that connection, and it is made again",
          [e["data"]["connected"] for e in ev] == [False, True])
    mesh.stop()
    wait_events(a, "gateway.upstream", 1)
    mesh = FakeMesh("run3", [], [], status={"state": "online", "run_id": "bad run id\"}"})
    time.sleep(3.0)
    check("a run_id that is not an identifier: the mesh group stays unavailable, the gateway up",
          mesh_status(ctl)["connected"] is False and err_code(a.call("mesh.identity")) == 9 and
          "result" in a.call("gateway.ping") and "mesh.subscribe" not in mesh.methods())
    mesh.stop()
    mesh = FakeMesh("run3", [], [], status={"state": "online"})
    time.sleep(3.0)
    check("no run_id at all: the same", mesh_status(ctl)["connected"] is False and "mesh.subscribe" not in mesh.methods())
    mesh.stop()
    mesh = FakeMesh("run3", [message(1, text="third run")], [node(1)])
    wait_for("a proper meshcored is taken up again", lambda: mesh_status(ctl)["connected"], timeout=15)
    check("and served", a.call("mesh.sync")["result"]["run_id"] == "run3")

    # ---- 13. revoked with a request waiting; the last reader gone ----------------------------------
    mesh.mode = "stall"
    a.send("mesh.identity")
    time.sleep(0.2)
    r = ctl.call("gateway.revoke", {"client": A.fp_hex})
    check("A is revoked while its request waits", "result" in r and a.closed())
    mesh.mode = "ok"
    wait_for("nobody may read the mesh any more: let go, emptied",
             lambda: mesh_status(ctl)["wanted"] is False and mesh_status(ctl)["journal"] == 0)
    time.sleep(2.5)  # the waiting request's deadline passes with nobody to answer
    check("the gateway is unharmed", "result" in b.call("gateway.ping"))
    b.close()

    # ---- 14. what was asked of meshcored, ever; what the log holds ---------------------------------
    check("meshcored was only ever asked for its status, a subscription, its messages and the reading methods",
          {m for m, _ in FakeMesh.ever} <= set(OWN) | set(READ) and len(FakeMesh.ever) > 30)
    check("never with a parameter of the gateway's own, never for the receive log",
          all(p is None for m, p in FakeMesh.ever if m in OWN or m == "mesh.nodes") and
          "rx_log" not in json.dumps(FakeMesh.ever))
    ctl.close()
    err = stop()
    log = ""
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as f:
            log += f.read()
    check("the log has the lifecycle of the mesh connection",
          "meshcored connected" in log and "meshcored connection lost" in log and "journal was emptied" in log)
    check("the log holds no message text, no name, no key, no run",
          all(w not in log for w in (TEXT, "Ola", K1, K3, "node-", "run1", "run2", "first of the new run", "LEAK", "xxxxxxxx")))
except Exception as e:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % e, False)
finally:
    if lib.proc is not None:
        lib.proc.kill()
        print(lib.proc.stderr.read().decode(errors="replace")[-2000:])
    if mesh is not None:
        mesh.stop()
    if lib.failed and os.environ.get("GATEWAYD_E2E_KEEP"):
        print("  kept:", ROOT)
    else:
        shutil.rmtree(ROOT, ignore_errors=True)

print("gatewayd mesh e2e: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
