#!/usr/bin/env python3
"""Sending through the gateway (M4.2; docs/api/gateway.md §8), end to end:
the real gatewayd (the test-hooks build, argv[1]) between real TLS clients
and a fake meshcored that can accept a message, refuse it, take it and lose
the answer, never read it, hold the answer, and start again as another run.

What is counted throughout is what the fake meshcored was asked to
transmit: one client_msg_id must never make it transmit twice.

Covers: the mesh_send scope on its own, the request checks, direct, routed
and channel messages, delivery states through the journal and the events,
repeats answered from the record, an answer that never came (taken, and not
taken), a connection lost while a send waits, a second connection taking
the place of the first, meshcored starting again, resend, the rate limit,
a scope taken away while the gateway is finding out, and what the log holds.

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
SECRET_TEXT = "the words of a message 4711"

# A client may make 40 requests at once and 20 a second after that; the suite
# stays under it, so a code 10 it sees is the send limit under test.
_send = Client.send


def _paced_send(self, method, params=None):
    time.sleep(0.055)
    return _send(self, method, params)


Client.send = _paced_send


class Mesh(FakeService):
    """A meshcored that sends. send_mode says what becomes of a mesh.send:
    ok       taken and answered
    refuse   answered with its own error 5, nothing taken
    swallow  taken (the message exists, its event is raised), never answered
    ignore   read and not taken, no answer; the service goes on talking, so
             the gateway keeps its connection
    drop     never read as far as anyone can tell: nothing taken, no answer,
             and not a byte: the gateway gives the connection up
    hold     taken and answered only when release() is called
    odd      answered `accepted` with no message id
    """

    def __init__(self, run_id, messages=()):
        self.run_id = run_id
        self.messages = [dict(m) for m in messages]
        self.next_id = max([m["id"] for m in self.messages] + [0]) + 1
        self.subs = []
        self.lock = threading.RLock()
        self.send_mode = "ok"
        self.sent = []          # every message it was made to transmit, in order
        self.held = []
        self.hold_lists = False  # mesh.messages is answered only by release_lists()
        self.held_lists = []
        super().__init__("meshcored", {})

    def take(self, p):
        """Accepts one message the way meshcored does: it exists from here on."""
        with self.lock:
            self.sent.append(p)
            if "resend" in p:
                m = next(x for x in self.messages if x["id"] == p["resend"])
                m.update(state="sent_direct", attempts=m.get("attempts", 1) + 1)
                res = {"accepted": True, "message_id": m["id"], "route": "direct", "ack_timeout_ms": 8000,
                       "ack_expected": True, "resent": True}
            else:
                m = {"id": self.next_id, "direction": "out", "text": p["text"], "timestamp": 1700000000,
                     "mono_ms": 1000 + self.next_id, "state": "sent_flood", "session_key": "LEAK"}
                self.next_id += 1
                if "channel" in p:
                    m.update(kind="channel", channel=p["channel"], channel_name="Public", channel_hash="11",
                             sender_name="unit", text="unit: " + p["text"], ack_expected=False)
                    res = {"accepted": True, "message_id": m["id"], "route": "flood", "channel": p["channel"],
                           "ack_expected": False}
                else:
                    m.update(kind="direct", peer_public_key=p["to"], peer_name="Ola Nordmann", ack_expected=True,
                             attempts=1)
                    res = {"accepted": True, "message_id": m["id"], "route": "explicit" if "via" in p else "flood",
                           "ack_timeout_ms": 9000, "ack_expected": True, "expected_ack": "LEAK"}
                    if "via" in p:
                        m.update(state="sent_direct", route="explicit", path_hex="b2", path_hops=len(p["via"]))
                self.messages.append(m)
            self.emit("mesh.message", {"message": dict(m)})
            return res

    def update(self, mid, **changes):
        with self.lock:
            m = next(x for x in self.messages if x["id"] == mid)
            m.update(changes)
            self.emit("mesh.message", {"message": dict(m)})

    def answer(self, m, p):
        if m == "mesh.status":
            return {"state": "online", "run_id": self.run_id, "uptime_s": 5}
        if m == "mesh.subscribe":
            return {"subscribed": True}
        if m == "mesh.messages":
            with self.lock:
                return {"messages": [dict(x) for x in self.messages], "count": len(self.messages), "persistent": False}
        if m == "mesh.send":
            if self.send_mode == "refuse":
                return (5, "8 messages are already waiting; LEAK the service's own words")
            if self.send_mode == "odd":
                return {"accepted": True}
            if "resend" in p:
                with self.lock:
                    old = [x for x in self.messages if x["id"] == p["resend"]]
                if not old or old[0].get("state") != "no_ack":
                    return (2, "not an unacknowledged direct message of this run")
            return self.take(p)
        return (1, "unknown method")

    def reply(self, c, req, res):
        with self.lock:
            try:
                if isinstance(res, tuple):
                    send_frame(c, {"id": req["id"], "error": {"code": res[0], "message": res[1]}})
                else:
                    send_frame(c, {"id": req["id"], "result": res})
            except OSError:
                pass

    def serve(self, c):
        try:
            while True:
                req = recv_frame(c)
                m, p = req.get("method"), req.get("params")
                self.calls.append((m, p))
                if m == "mesh.send" and self.send_mode == "drop":
                    continue
                if m == "mesh.send" and self.send_mode == "ignore":
                    self.emit("mesh.state", {"state": "online", "reason": "none", "mono_ms": 1})
                    continue
                if m == "mesh.send" and self.send_mode == "swallow":
                    self.take(p)
                    continue
                if m == "mesh.send" and self.send_mode == "hold":
                    self.held.append((c, req))
                    continue
                if m == "mesh.messages" and self.hold_lists:
                    self.held_lists.append((c, req))
                    continue
                self.reply(c, req, self.answer(m, p))
                if m == "mesh.subscribe":
                    with self.lock:
                        self.subs.append(c)
        except (EOFError, OSError, ValueError):
            c.close()

    def release(self):
        held, self.held = self.held, []
        for c, req in held:
            self.reply(c, req, self.take(req["params"]))

    def release_lists(self):
        held, self.held_lists = self.held_lists, []
        self.hold_lists = False
        for c, req in held:
            self.reply(c, req, self.answer("mesh.messages", None))

    def emit(self, event, data):
        with self.lock:
            for c in list(self.subs):
                try:
                    send_frame(c, {"event": event, "data": data})
                except OSError:
                    self.subs.remove(c)

    def sends(self):
        """The mesh.send requests that reached it, taken or not."""
        return [p for m, p in self.calls if m == "mesh.send"]

    def count(self, method):
        return len([1 for m, _ in self.calls if m == method])


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


def connect(ident, fp, topics=None):
    c = Client(ident, pin=fp)
    h = c.hello()
    assert h["result"]["paired"] is True, h
    if topics is not None:
        c.call("gateway.subscribe", {"topics": topics})
    return c


def events(c, name="mesh.message"):
    """What the unit has sent so far under this name; a ping's answer comes after it."""
    c.call("gateway.ping")
    got = [e["data"] for e in c.events if e["event"] == name]
    c.events = [e for e in c.events if e["event"] != name]
    return got


def wait_reconnect(ctl, before):
    """The gateway gave its connection to meshcored up and made it again."""
    wait_for("the gateway connects to meshcored again",
             lambda: mesh.count("mesh.subscribe") > before and mesh_status(ctl)["connected"])


_n = [0]


def cmid():
    _n[0] += 1
    return "e2e-msg-%010d" % _n[0]


def direct(text, cm=None, **more):
    p = {"to": K1, "text": text, "client_msg_id": cm or cmid()}
    p.update(more)
    return p


A, B, C, D, E = (Identity(n) for n in "abcde")
mesh = None

try:
    print("gatewayd send e2e")
    set_password(True)
    mesh = Mesh("run1", [{"id": 1, "direction": "in", "kind": "direct", "peer_public_key": K1, "text": "hi",
                          "mono_ms": 1001, "state": "received", "ack_expected": False},
                         {"id": 2, "direction": "out", "kind": "direct", "peer_public_key": K1, "text": "are you there",
                          "mono_ms": 1002, "state": "no_ack", "ack_expected": True, "attempts": 1}])
    ctl = start()
    ctl.call("gateway.subscribe")

    # ---- 1. the scope: mesh_read does not send ---------------------------------
    fp, d, _ = pair(ctl, B, "reader", ["status", "mesh_read"])
    check("B is paired to read and not to send", d["result"]["paired"] is True)
    wait_for("the gateway connects to meshcored", lambda: mesh_status(ctl)["connected"])
    b = connect(B, fp)
    r = b.call("mesh.send", direct("no"))
    check("mesh.send without mesh_send is not permitted (code 7)", err_code(r) == 7)
    r = b.call("mesh.resend", {"message_id": 2, "run_id": "run1", "client_msg_id": cmid()})
    check("nor is mesh.resend", err_code(r) == 7)
    check("a request that is not even well-formed is told the same: the scope comes first",
          err_code(b.call("mesh.send", {"x": 1})) == 7)
    check("B still reads", b.call("mesh.sync").get("result", {}).get("seq") == 2)
    check("nothing of that reached meshcored", mesh.sends() == [])
    b.close()

    _, d, _ = pair(ctl, A, "sender")
    check("A is paired with the default scopes", d["result"]["paired"] is True)
    a = Client(A, pin=fp)
    h = a.hello()["result"]
    check("hello names the capability, the scope and the limit",
          "mesh.send" in h["capabilities"] and "mesh_send" in h["scopes"] and h["limits"]["send_per_min"] == 20)
    a.call("gateway.subscribe", {"topics": ["mesh"]})

    # ---- 2. what may be asked ---------------------------------------------------
    bad = [None, {}, {"to": K1, "text": "x"}, {"to": K1, "text": "x", "client_msg_id": "short"},
           {"to": K1, "channel": 0, "text": "x", "client_msg_id": cmid()},
           {"text": "x", "client_msg_id": cmid()}, {"to": "a1", "text": "x", "client_msg_id": cmid()},
           {"to": K1, "text": "", "client_msg_id": cmid()}, {"to": K1, "text": "x" * 161, "client_msg_id": cmid()},
           {"to": K1, "text": "bell\x07", "client_msg_id": cmid()}, {"to": K1, "text": "esc\x1b[2J", "client_msg_id": cmid()},
           {"to": K1, "text": 5, "client_msg_id": cmid()}, {"channel": "0", "text": "x", "client_msg_id": cmid()},
           {"channel": 0, "via": [], "text": "x", "client_msg_id": cmid()},
           {"to": K1, "via": None, "text": "x", "client_msg_id": cmid()},
           {"to": K1, "via": ["b2"], "text": "x", "client_msg_id": cmid()},
           {"to": K1, "text": "x", "client_msg_id": cmid(), "resend": 2},
           {"to": K1, "text": "x", "client_msg_id": cmid(), "zero_hop": True}]
    check("a request that is not exactly a message is invalid (code 2), each of them",
          all(err_code(a.call("mesh.send", p)) == 2 for p in bad))
    bad = [None, {"message_id": 2, "run_id": "run1"}, {"message_id": 0, "run_id": "run1", "client_msg_id": cmid()},
           {"message_id": 2, "client_msg_id": cmid()}, {"message_id": 2, "run_id": "run 1", "client_msg_id": cmid()},
           {"message_id": 2, "run_id": "run1", "client_msg_id": cmid(), "text": "x"}]
    check("and so is a resend that names more or less than a message",
          all(err_code(a.call("mesh.resend", p)) == 2 for p in bad))
    check("none of that reached meshcored", mesh.sends() == [])
    check("what a phone may never do stays unknown (code 1)",
          all(err_code(a.call(m, {})) == 1 for m in ("mesh.app_send", "mesh.advert", "radio.send", "mesh.messages")))

    # ---- 3. a direct message, and its delivery states ---------------------------
    cm1 = cmid()
    r = a.call("mesh.send", direct(SECRET_TEXT, cm1))
    res = r.get("result", {})
    check("mesh.send answers accepted, with the message and the run",
          res == {"accepted": True, "message_id": 3, "run_id": "run1", "route": "flood", "ack_expected": True,
                  "ack_timeout_ms": 9000, "duplicate": False} and "mono_ms" in r)
    check("meshcored was sent the destination and the text, written again, and nothing else",
          mesh.sends() == [{"to": K1, "text": SECRET_TEXT}] and len(mesh.sent) == 1)
    ev = events(a)
    check("the message arrives as mesh.message under its id, in the journal",
          len(ev) == 1 and ev[0]["message"]["id"] == 3 and ev[0]["message"]["state"] == "sent_flood" and
          ev[0]["run_id"] == "run1" and ev[0]["seq"] == 3 and "session_key" not in ev[0]["message"])
    mesh.update(3, state="sent_direct")
    mesh.update(3, state="acked", ack_mono_ms=5000)
    wait_for("the state changes reach the journal", lambda: mesh_status(ctl)["seq"] == 5)
    ev = events(a)
    check("each delivery state is an event with a new seq and the same id",
          [(e["seq"], e["message"]["id"], e["message"]["state"]) for e in ev] == [(4, 3, "sent_direct"), (5, 3, "acked")])
    s = a.call("mesh.sync", {"after_seq": 2})["result"]
    check("and mesh.sync gives them to a client that was away",
          [(m["seq"], m["message"]["state"]) for m in s["messages"]] == [(3, "sent_flood"), (4, "sent_direct"), (5, "acked")])

    # ---- 4. a repeat is answered from the record ---------------------------------
    r = a.call("mesh.send", direct(SECRET_TEXT, cm1))
    check("the same client_msg_id again: the same answer, duplicate, nothing transmitted",
          r.get("result") == dict(res, duplicate=True) and len(mesh.sends()) == 1)
    r = a.call("mesh.send", direct("other words", cm1))
    check("the id under another message is a mistake (code 2), nothing transmitted",
          err_code(r) == 2 and len(mesh.sends()) == 1)
    r = a.call("mesh.send", {"channel": 0, "text": SECRET_TEXT, "client_msg_id": cm1})
    check("also under another destination", err_code(r) == 2 and len(mesh.sends()) == 1)
    cm = cmid()
    mesh.send_mode = "hold"
    rid = a.send("mesh.send", direct("pipelined", cm))
    wait_for("the send reached meshcored", lambda: len(mesh.held) == 1)
    r = a.call("mesh.send", direct("pipelined", cm))
    mesh.send_mode = "ok"
    mesh.release()
    got = a.read(rid)
    check("the same id again while the first still waits: told busy (code 5), one transmission",
          got.get("result", {}).get("duplicate") is False and err_code(r) == 5 and len(mesh.sent) == 2)
    r = a.call("mesh.send", direct("pipelined", cm))
    check("and afterwards it is a duplicate", r["result"]["duplicate"] is True and r["result"]["message_id"] == 4 and
          len(mesh.sent) == 2)

    # ---- 5. a route, and a channel -------------------------------------------------
    r = a.call("mesh.send", direct("routed", via=[K2]))
    check("via is passed on and the route is explicit",
          r["result"]["route"] == "explicit" and mesh.sends()[-1] == {"to": K1, "text": "routed", "via": [K2]})
    r = a.call("mesh.send", {"channel": 0, "text": "to everyone", "client_msg_id": cmid()})
    check("a channel message: no ACK is expected and no timeout is named",
          r.get("result") == {"accepted": True, "message_id": 6, "run_id": "run1", "route": "flood", "channel": 0,
                              "ack_expected": False, "duplicate": False} and
          mesh.sends()[-1] == {"channel": 0, "text": "to everyone"})
    events(a)
    seq0 = mesh_status(ctl)["seq"]
    mesh.update(6, transmitted=True)
    mesh.update(6, heard_back=2, heard_back_hops=1, heard_back_mono_ms=7000)
    wait_for("the channel message's updates reach the journal", lambda: mesh_status(ctl)["seq"] == seq0 + 2)
    ev = events(a)
    check("what a channel send can show arrives the same way: transmitted, heard back",
          [e["message"].get("transmitted") for e in ev] == [True, True] and ev[1]["message"]["heard_back"] == 2 and
          all(e["message"]["id"] == 6 and e["message"]["state"] == "sent_flood" for e in ev))

    # ---- 6. meshcored refuses: nothing was sent, the id is free -------------------
    cm = cmid()
    mesh.send_mode = "refuse"
    r = a.call("mesh.send", direct("when there is room", cm))
    check("its refusal keeps its code and loses its words",
          err_code(r) == 5 and r["error"]["message"] == "upstream error" and len(mesh.sent) == 4)
    mesh.send_mode = "ok"
    r = a.call("mesh.send", direct("when there is room", cm))
    check("the same id is then sent, once", r["result"]["duplicate"] is False and len(mesh.sent) == 5)
    mesh.send_mode = "odd"
    cm = cmid()
    r = a.call("mesh.send", direct("odd", cm))
    check("an answer that says accepted and not what: outcome unknown (code 3)", err_code(r) == 3)
    mesh.send_mode = "ok"
    n = len(mesh.sends())
    check("and that id is never passed on again", err_code(a.call("mesh.send", direct("odd", cm))) == 3 and
          len(mesh.sends()) == n)
    a.close()

    # ---- 7. the answer never came, and meshcored had taken it ---------------------
    _, d, _ = pair(ctl, D, "uncertain")
    dd = connect(D, fp, ["mesh"])
    sent0 = len(mesh.sent)
    cm = cmid()
    mesh.send_mode = "swallow"
    t0 = time.time()
    r = dd.call("mesh.send", direct("taken, answer lost", cm))
    check("no answer from meshcored in 2 s: upstream unavailable (code 9)",
          err_code(r) == 9 and 1.5 < time.time() - t0 < 4 and len(mesh.sent) == sent0 + 1)
    mesh.send_mode = "ok"
    n_send, n_list = len(mesh.sends()), mesh.count("mesh.messages")
    r = dd.call("mesh.send", direct("taken, answer lost", cm))
    taken_id = mesh.sent and mesh.messages[-1]["id"]
    check("the repeat asks meshcored for its messages, finds it, and transmits nothing",
          r.get("result", {}).get("duplicate") is True and r["result"]["message_id"] == taken_id and
          r["result"]["run_id"] == "run1" and r["result"]["accepted"] is True and
          mesh.count("mesh.messages") == n_list + 1 and len(mesh.sends()) == n_send and len(mesh.sent) == sent0 + 1)
    r = dd.call("mesh.send", direct("taken, answer lost", cm))
    check("and from then on it is answered from the record",
          r["result"]["duplicate"] is True and mesh.count("mesh.messages") == n_list + 1)

    # ---- 8. the answer never came, and meshcored had not taken it ----------------
    cm = cmid()
    mesh.send_mode = "ignore"
    r = dd.call("mesh.send", direct("never read", cm))
    check("no answer and nothing taken: code 9 again", err_code(r) == 9 and len(mesh.sent) == sent0 + 1)
    mesh.send_mode = "ok"
    n_list = mesh.count("mesh.messages")
    r = dd.call("mesh.send", direct("never read", cm))
    check("the repeat finds it is not there and transmits it, once",
          r.get("result", {}).get("duplicate") is False and mesh.count("mesh.messages") == n_list + 1 and
          len(mesh.sent) == sent0 + 2 and mesh.sent[-1] == {"to": K1, "text": "never read"})
    r = dd.call("mesh.send", direct("never read", cm))
    check("and again is a duplicate", r["result"]["duplicate"] is True and len(mesh.sent) == sent0 + 2)
    # The connection to meshcored was given up meanwhile: a list from the new
    # one proves nothing about a request that may lie unread in the old one.
    cm = cmid()
    subs = mesh.count("mesh.subscribe")
    mesh.send_mode = "drop"
    r = dd.call("mesh.send", direct("the line went quiet", cm))
    check("no answer and not a byte from meshcored: code 9, and its connection is made again", err_code(r) == 9)
    mesh.send_mode = "ok"
    wait_reconnect(ctl, subs)
    n_send, n_sent = len(mesh.sends()), len(mesh.sent)
    r = dd.call("mesh.send", direct("the line went quiet", cm))
    check("the repeat is not sent although the list does not hold it: outcome unknown (code 3)",
          err_code(r) == 3 and r["error"]["message"] == "outcome unknown" and len(mesh.sends()) == n_send and
          len(mesh.sent) == n_sent)
    check("however often", err_code(dd.call("mesh.send", direct("the line went quiet", cm))) == 3 and
          len(mesh.sends()) == n_send)
    # The same words a second time are a second message, not the first found again.
    cm_a, cm_b = cmid(), cmid()
    r = dd.call("mesh.send", direct("same words", cm_a))
    first_id = r["result"]["message_id"]
    mesh.send_mode = "swallow"
    r = dd.call("mesh.send", direct("same words", cm_b))
    check("a second message of the same words, its answer lost", err_code(r) == 9 and len(mesh.sent) == sent0 + 4)
    mesh.send_mode = "ok"
    r = dd.call("mesh.send", direct("same words", cm_b))
    check("is found as itself and not as the first",
          r["result"]["duplicate"] is True and r["result"]["message_id"] == first_id + 1 and len(mesh.sent) == sent0 + 4)

    # ---- 9. the connection is lost while a send waits ------------------------------
    cm = cmid()
    mesh.send_mode = "hold"
    dd.send("mesh.send", direct("phone went away", cm))
    wait_for("the send reached meshcored", lambda: len(mesh.held) == 1)
    dd.close()
    time.sleep(0.3)
    mesh.send_mode = "ok"
    mesh.release()
    time.sleep(0.3)
    dd = connect(D, fp, ["mesh"])
    r = dd.call("mesh.send", direct("phone went away", cm))
    check("the answer nobody waited for was recorded: the repeat is a duplicate, one transmission",
          r.get("result", {}).get("duplicate") is True and len(mesh.sent) == sent0 + 5)
    # A second connection of the same client takes the place of the first.
    cm = cmid()
    mesh.send_mode = "hold"
    dd.send("mesh.send", direct("changed networks", cm))
    wait_for("the send reached meshcored", lambda: len(mesh.held) == 1)
    d2 = connect(D, fp)
    rid = d2.send("mesh.send", direct("changed networks", cm))
    time.sleep(0.3)
    check("the old connection was ended", dd.closed(2))
    mesh.send_mode = "ok"
    mesh.release()
    r = d2.read(rid)
    check("the repeat on the new connection gets the answer of the send still waiting, one transmission",
          r.get("result", {}).get("duplicate") is True and len(mesh.sent) == sent0 + 6 and
          len([p for p in mesh.sends() if p.get("text") == "changed networks"]) == 1)
    dd = d2

    # ---- 10. a scope taken away while the gateway is finding out -----------------
    cm = cmid()
    mesh.send_mode = "ignore"
    check("a send that is never read", err_code(dd.call("mesh.send", direct("after the scope went", cm))) == 9)
    mesh.send_mode = "ok"
    mesh.hold_lists = True
    rid = dd.send("mesh.send", direct("after the scope went", cm))
    wait_for("the gateway asks for the messages", lambda: len(mesh.held_lists) == 1)
    r = ctl.call("gateway.set_scopes", {"client": D.fp_hex, "scopes": ["status", "mesh_read"]})
    check("the unit takes mesh_send away meanwhile", "result" in r)
    mesh.release_lists()
    r = dd.read(rid)
    time.sleep(0.3)
    check("the list proves it absent, and without the scope it is still not sent: not permitted (code 7)",
          err_code(r) == 7 and len(mesh.sent) == sent0 + 6)
    check("and a new one is not permitted either", err_code(dd.call("mesh.send", direct("no"))) == 7)
    ctl.call("gateway.set_scopes", {"client": D.fp_hex, "scopes": ["status", "mesh_read", "mesh_send"]})
    r = dd.call("mesh.send", direct("after the scope went", cm))
    check("with the scope back the same id is found out again and sent once",
          r.get("result", {}).get("duplicate") is False and len(mesh.sent) == sent0 + 7)

    # ---- 11. meshcored is away, and starts again as another run --------------------
    cm_lost = cmid()
    mesh.send_mode = "swallow"
    check("a send taken by a meshcored that then dies", err_code(dd.call("mesh.send", direct("last words", cm_lost))) == 9)
    total = len(mesh.sent)
    mesh.stop()
    wait_for("the gateway notices", lambda: not mesh_status(ctl)["connected"])
    cm_new = cmid()
    r = dd.call("mesh.send", direct("while it is away", cm_new))
    check("while meshcored is away a new send is unavailable (code 9), at once", err_code(r) == 9)
    check("the unanswered one too: nothing can be found out", err_code(dd.call("mesh.send", direct("last words", cm_lost))) == 9)
    r = dd.call("mesh.send", direct("same words", cm_a))
    check("a recorded send is still answered from the record", r.get("result", {}).get("duplicate") is True)
    mesh = Mesh("run2")
    wait_for("the gateway connects to the new run", lambda: mesh_status(ctl)["connected"])
    r = dd.call("mesh.send", direct("last words", cm_lost))
    check("after the restart the unanswered send cannot be found out: outcome unknown (code 3), not sent",
          err_code(r) == 3 and r["error"]["message"] == "outcome unknown" and mesh.sends() == [])
    check("however often it is repeated", err_code(dd.call("mesh.send", direct("last words", cm_lost))) == 3 and
          mesh.sends() == [])
    r = dd.call("mesh.send", direct("while it is away", cm_new))
    check("the one that never left is sent in the new run, once",
          r.get("result", {}).get("run_id") == "run2" and r["result"]["message_id"] == 1 and
          r["result"]["duplicate"] is False and len(mesh.sent) == 1)
    r = dd.call("mesh.resend", {"message_id": 2, "run_id": "run1", "client_msg_id": cmid()})
    check("a resend of a message of the earlier run is refused (code 3)", err_code(r) == 3 and len(mesh.sent) == 1)
    dd.close()

    # ---- 12. resend ------------------------------------------------------------------
    mesh.stop()
    mesh = Mesh("run3", [{"id": 1, "direction": "out", "kind": "direct", "peer_public_key": K1, "text": "are you there",
                          "mono_ms": 1001, "state": "no_ack", "ack_expected": True, "attempts": 1},
                         {"id": 2, "direction": "out", "kind": "direct", "peer_public_key": K1, "text": "delivered",
                          "mono_ms": 1002, "state": "acked", "ack_expected": True, "attempts": 1}])
    wait_for("the gateway connects to run3", lambda: mesh_status(ctl)["connected"] and mesh_status(ctl)["journal"] == 2)
    _, d, _ = pair(ctl, E, "resender")
    e = connect(E, fp, ["mesh"])
    cm = cmid()
    r = e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})
    check("mesh.resend is meshcored's resend of that message and nothing else",
          r.get("result") == {"accepted": True, "message_id": 1, "run_id": "run3", "route": "direct",
                              "ack_expected": True, "ack_timeout_ms": 8000, "resent": True, "duplicate": False} and
          mesh.sends() == [{"resend": 1}])
    ev = events(e)
    check("the message goes back to sent with one more attempt, as an event",
          len(ev) == 1 and ev[0]["message"]["id"] == 1 and ev[0]["message"]["attempts"] == 2 and
          ev[0]["message"]["state"] == "sent_direct")
    r = e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})
    check("the same resend again is a duplicate", r["result"]["duplicate"] is True and len(mesh.sent) == 1)
    r = e.call("mesh.resend", {"message_id": 2, "run_id": "run3", "client_msg_id": cmid()})
    check("a delivered message is not resent: meshcored's refusal (code 2), its words kept back",
          err_code(r) == 2 and r["error"]["message"] == "upstream error" and len(mesh.sent) == 1)
    mesh.update(1, state="no_ack")
    wait_for("the gateway sees the message fail again", lambda: mesh_status(ctl)["seq"] >= 4)
    cm = cmid()
    mesh.send_mode = "swallow"
    r = e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})
    check("a resend taken, its answer lost", err_code(r) == 9 and len(mesh.sent) == 2)
    mesh.send_mode = "ok"
    r = e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})
    check("the repeat sees one more attempt and transmits nothing",
          r.get("result", {}).get("duplicate") is True and r["result"]["resent"] is True and len(mesh.sent) == 2)
    mesh.update(1, state="no_ack")
    wait_for("the gateway sees it fail a third time", lambda: mesh_status(ctl)["seq"] >= 6)
    cm = cmid()
    mesh.send_mode = "ignore"
    check("a resend never read", err_code(e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})) == 9)
    mesh.send_mode = "ok"
    r = e.call("mesh.resend", {"message_id": 1, "run_id": "run3", "client_msg_id": cm})
    check("the repeat sees the attempts unchanged and resends, once",
          r.get("result", {}).get("duplicate") is False and len(mesh.sent) == 3 and mesh.sent[-1] == {"resend": 1} and
          mesh.messages[0]["attempts"] == 4)
    e.close()

    # ---- 13. the rate limit --------------------------------------------------------
    _, d, _ = pair(ctl, C, "talkative")
    c = connect(C, fp)
    base = len(mesh.sent)
    ids = [cmid() for _ in range(21)]
    codes = []
    for i in range(20):
        r = c.call("mesh.send", {"channel": 0, "text": "burst %d" % i, "client_msg_id": ids[i]})
        codes.append("result" in r)
        time.sleep(0.03)
    check("20 sends in a minute are passed on", all(codes) and len(mesh.sent) == base + 20)
    r = c.call("mesh.send", {"channel": 0, "text": "burst 20", "client_msg_id": ids[20]})
    check("the 21st is rate limited (code 10) and not passed on", err_code(r) == 10 and len(mesh.sent) == base + 20)
    r = c.call("mesh.send", {"channel": 0, "text": "burst 0", "client_msg_id": ids[0]})
    check("a repeat is still answered from the record", r.get("result", {}).get("duplicate") is True)
    c.close()
    c = connect(C, fp)
    r = c.call("mesh.send", {"channel": 0, "text": "burst 20", "client_msg_id": ids[20]})
    check("the limit is the client's, not the connection's", err_code(r) == 10 and len(mesh.sent) == base + 20)
    c.close()
    a = connect(A, fp)
    r = a.call("mesh.send", direct("another client"))
    check("another client has its own budget", "result" in r and len(mesh.sent) == base + 21)
    a.close()

    # ---- 14. the end -----------------------------------------------------------------
    ctl.close()
    err = stop()
    log = ""
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as f:
            log += f.read()
    check("the log says when a send's outcome was not known", "its outcome is not known" in log)
    check("the log holds no message text, no id of a message, no key and no run",
          all(w not in log for w in (SECRET_TEXT, "4711", "e2e-msg", K1, "never read", "last words", "run1", "run3", "LEAK")))
except Exception as ex:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % ex, False)
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

print("gatewayd send e2e: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
