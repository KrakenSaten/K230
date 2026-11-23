#!/usr/bin/env python3
"""gatewayd, the Vision group (ADR-019 amendment 1), end to end: the real
service (the test-hooks build) between a fake shell that serves the Vision
app's vision.* methods on a real pocketipc socket and real TLS clients, on
loopback.

  python3 tests/gatewayd_vision_e2e_test.py <path to gatewayd-testhooks>

What is driven: that nothing of Vision exists for a client without the scope
(no capability, code 7, no topic) and that vision.pull is no method of a
client; gateway.scope on the local socket, and that it is no method of a
client; the status filter and who reads live and who busy; the preview: one
viewer, a picture only after the acknowledgement of the last and never more
than five a second, the unit's empty answers, a 64 KiB picture and what is
dropped; how a preview ends (the viewer disconnects, gives the topic back,
is revoked, loses the scope, stops acknowledging; STOP on the unit; Vision
left; a mode that is never shared; the gateway stopping); the reconciliation
with a unit that shares for nobody; the limit of 20 requests a minute; an
image whose shell knows no vision.status; and what the log holds.

Nothing here is a Vision app or a camera: the fake shell stands for both.

The 5 seconds without an acknowledgement are waited for in full, once.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import base64
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403

from gatewayd_capture_lib import *  # noqa: E402,F401,F403
from gatewayd_capture_lib import FakeShell  # noqa: E402

TOP = ["available", "owner", "mode", "modes", "settings", "model", "labels", "pic", "stats", "share"]
STATS = ["fps_x10", "infer_ms", "pre_ms", "post_ms", "cpu_pct", "rss_kb", "dropped", "count_ab", "count_ba"]
FRAME = ["seq", "w", "h", "age_ms", "jpeg", "boxes"]
SIX = {"vision.status", "vision.share", "vision.pull", "vision.mode", "vision.set", "vision.reset"}
PRIVATE = ("face", "recognize", "read")


class VisionShell(FakeShell):
    """The shell's socket with the Vision app in front: vision.* as the app
    answers them, with more in every answer than any client may see.
    PacketShark's capture.* stay as FakeShell serves them."""

    def __init__(self):
        self.vpresent = True        # this image's Vision serves the methods
        self.vfront = True          # and is the app in front
        self.vmode = "detect"
        self.share = "off"          # off, live, stopped
        self.viewer = None          # who it shares with: the gateway's words
        self.viewer_name = None
        self.seq = 0                # the newest picture
        self.produce = True         # a new picture is ready at every pull
        self.jpeg_bytes = 3000
        self.nboxes = 2
        self.mangle = None          # a function over the answer to vision.pull
        self.lenient = False        # a unit that would share in any mode (it must not matter)
        self.settings = {"track.line": "across", "traffic.range": "normal", "traffic.line": "off",
                         "traffic.speed": "off", "color.tol": "med", "edge.hard": "0", "trace.dark": "0",
                         "seg.view": "overlay"}
        self.counts = [3, 1]
        super().__init__()

    def vstatus(self):
        if not self.vfront:
            return {"available": False}
        st = {"available": True, "owner": "vision", "mode": self.vmode,
              "modes": ["detect", "track", "traffic", "color", "edge", "trace", "face", "recognize", "read"],
              "settings": dict(self.settings, **{"face.owner": "LEAK"}), "model": "yolov8n 320",
              "labels": ["person", "bicycle", "car"], "pic": {"w": 264, "h": 469},
              "stats": {"fps_x10": 172, "infer_ms": 34, "pre_ms": 5, "post_ms": 2, "cpu_pct": 44, "rss_kb": 7300,
                        "dropped": 0, "count_ab": self.counts[0], "count_ba": self.counts[1], "faces_seen": 4242},
              "share": self.share,
              # What no client may see, and what must not get through if a Vision ever sent it.
              "viewer_name": self.viewer_name or "LEAK", "owner_name": "LEAK", "embedding": [4242, 4242],
              "enrolled": ["LEAK"], "helper_pid": 4242, "session_key": "LEAK"}
        if self.share == "live":
            st["client"] = self.viewer
        return st

    def picture(self):
        raw = b"\xff\xd8\xff\xe0" + bytes(max(0, self.jpeg_bytes - 4))
        return {"seq": self.seq, "w": 264, "h": 469, "age_ms": 41, "jpeg": base64.b64encode(raw).decode(),
                "boxes": [[i + 1, 0, 870, 10 * i, 20, 64, 128, 1, 312] for i in range(self.nboxes)],
                "path": "/tmp/LEAK.jpg", "face": {"owner": "LEAK"}}

    def vision(self, m, p):
        if not self.vpresent or m not in SIX:
            return (1, "unknown method")
        if m == "vision.status":
            return self.vstatus()
        if not self.vfront:
            return (3, "Vision is not open LEAK")
        if m == "vision.share":
            if not p["on"]:
                if self.share == "live":
                    self.share, self.viewer, self.viewer_name = "off", None, None
                return {"state": self.share}
            if self.share == "stopped" or (self.vmode in PRIVATE and not self.lenient):
                return (3, "not shared LEAK")
            self.share, self.viewer, self.viewer_name = "live", p.get("client"), p.get("name")
            return {"state": "live", "client": "LEAK", "name": "LEAK"}
        if m == "vision.pull":
            if self.share != "live":
                return (3, "nobody is shown the picture LEAK")
            if self.produce:
                self.seq += 1
            if self.seq <= p["after_seq"]:
                return {"seq": self.seq}
            pic = self.picture()
            return self.mangle(pic) if self.mangle else pic
        if m == "vision.mode":
            self.vmode = p["mode"]
        elif m == "vision.set":
            self.settings[p["key"]] = p["value"]
        else:
            self.counts = [0, 0]
        return {"applied": True, "note": "LEAK"}

    def answer(self, m, p):
        if m is not None and m.startswith("vision."):
            return self.vision(m, p)
        return super().answer(m, p)

    def tap_stop(self):
        """A finger on STOP on the unit: nobody is shown the picture until a
        person there allows it again."""
        with self.lock:
            self.share, self.viewer, self.viewer_name = "stopped", None, None

    def tap_resume(self):
        with self.lock:
            self.share = "off"


def vclean(obj):
    return not re.search(r"LEAK|4242|client|viewer|owner_name|embedding|enrolled|helper|session_key|faces|path",
                         json.dumps(obj))


def scope(ctl, client, name, on):
    return ctl.call("gateway.scope", {"client": client, "scope": name, "on": on})


def frame(c, timeout=4):
    return wait_event(c, "vision.frame", lambda d: True, timeout)


def frames(c, wait=0.0):
    return [e["data"] for e in events(c, "vision.frame", wait)]


def ack(c, seq):
    return c.call("vision.ack", {"seq": seq})


def share(c, on=True):
    return c.call("vision.share", {"on": on})


def offs():
    return len([p for p in shell.of("vision.share") if p == {"on": False}])


def ons():
    return [p for p in shell.of("vision.share") if p.get("on")]


def pulls():
    return len(shell.of("vision.pull"))


def view(c):
    return (c.call("vision.status").get("result") or {}).get("share")


ids = {n: Identity(n) for n in "abcdr"}
shell = None
meshd = None

try:
    print("gatewayd vision e2e")
    set_password(True)
    meshd = FakeService("meshcored", MESHD)
    shell = VisionShell()
    ctl = start()
    ctl.call("gateway.subscribe")
    A, B, C, D, R = (ids[n] for n in "abcdr")

    # ---- 1. no scope: nothing of Vision exists, and the shell is not reached -------------------------
    fp, d, _ = pair(ctl, A, "phone A", ["status"])
    check("A is paired with status alone", d["result"]["paired"] is True)
    a = connect(A, fp)
    check("the capability is not offered", "vision" not in a.caps)
    check("each vision method is not permitted without the scope (code 7)",
          all(err_code(a.call(m, p)) == 7 for m, p in
              [("vision.status", None), ("vision.share", {"on": True}), ("vision.share", {"on": False}),
               ("vision.ack", {"seq": 1}), ("vision.mode", {"mode": "detect"}),
               ("vision.set", {"key": "edge.hard", "value": "1"}), ("vision.reset", None)]))
    check("the gateway's own vision.pull is no method of a client (code 1), nor is an event's name or anything else of Vision",
          all(err_code(a.call(m, p)) == 1 for m, p in
              [("vision.pull", None), ("vision.pull", {"after_seq": 0}), ("vision.frame", None), ("vision.state", None),
               ("vision.stats", None), ("vision.enrol", {"name": "x"}), ("vision.forget", None), ("vision.open", None),
               ("vision.save", None)]))
    check("gateway.scope is no method of a client (code 1): a client cannot give itself a scope",
          err_code(a.call("gateway.scope", {"client": A.fp_hex, "scope": "vision", "on": True})) == 1 and
          err_code(a.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["status", "vision"]})) == 1)
    r = a.call("gateway.subscribe", {"topics": ["vision"]})
    check("the topic is not held without the scope", r.get("result") == {"topics": []})
    time.sleep(1.2)
    check("no client holds vision or capture_view: no connection to the shell, nothing asked",
          shell.calls == [] and shell.live == 0)

    # ---- 2. gateway.scope on the unit's own socket ------------------------------------------------------
    check("gateway.scope takes a paired client, a scope's name and a boolean, and nothing else (code 2)",
          all(err_code(ctl.call("gateway.scope", p)) == 2 for p in
              [None, {}, {"client": A.fp_hex, "scope": "vision"}, {"client": A.fp_hex, "on": True},
               {"scope": "vision", "on": True}, {"client": A.fp_hex, "scope": "vision", "on": 1},
               {"client": A.fp_hex, "scope": "admin", "on": True}, {"client": A.fp_hex, "scope": ["vision"], "on": True},
               {"client": A.fp_hex[:7], "scope": "vision", "on": True}, {"client": 12345678, "scope": "vision", "on": True}, {"client": "zz" + A.fp_hex[2:], "scope": "vision", "on": True},
               {"client": "00" * 32, "scope": "vision", "on": True}, {"client": A.fp_hex + "00", "scope": "vision", "on": True},
               {"client": A.fp_hex, "scope": "vision", "on": True, "scopes": ["status"]}]))
    check("a scope that needs another is refused without it (code 2), and nothing changed",
          err_code(scope(ctl, A.fp_hex, "mesh_send", True)) == 2 and
          err_code(scope(ctl, A.fp_hex, "capture_control", True)) == 2 and
          ctl.call("gateway.clients")["result"]["clients"][0]["scopes"] == ["status"])
    r = scope(ctl, A.fp_hex[:8].upper(), "vision", True)
    check("the start of the fingerprint names the client, and the answer is its scopes now",
          r.get("result") == {"scopes": ["status", "vision"]})
    check("the connected client is told its scopes", any(e["event"] == "gateway.scopes" and
          e["data"] == {"scopes": ["status", "vision"]} for e in events(a)))
    check("giving it again changes nothing", scope(ctl, A.fp_hex, "vision", True).get("result") == {"scopes": ["status", "vision"]})
    wait_for("vision granted: the gateway connects to the shell", lambda: shell.live == 1)
    stop_err = stop()
    ctl = start()
    ctl.call("gateway.subscribe")
    check("the grant is on storage: it is there after the gateway was started again",
          ctl.call("gateway.clients")["result"]["clients"][0]["scopes"] == ["status", "vision"])
    wait_for("and the gateway connects to the shell again", lambda: shell.live == 1)
    time.sleep(2.4)
    check("and asks it nothing while no client asks: no question of its own", shell.calls == [])
    a = connect(A, fp)
    check("the capability is offered now, and capture is not", "vision" in a.caps and "capture" not in a.caps)

    # ---- 3. an image whose shell knows no vision.status ---------------------------------------------------
    shell.vpresent = False
    check("the shell knows no vision.status: unavailable (code 9), not unknown", err_code(a.call("vision.status")) == 9)
    r = a.call("gateway.subscribe", {"topics": ["vision"]}).get("result")
    check("a client that then listens is told that nothing there answers", r is not None and r["topics"] == ["vision"] and
          r["upstream"].get("shell") is False)
    check("and every method is unavailable", err_code(a.call("vision.reset")) == 9 and err_code(share(a)) == 9)
    time.sleep(2.2)
    check("the shell was asked once, and not again and again", shell.calls == [("vision.status", None)])
    scope(ctl, A.fp_hex, "vision", False)
    wait_for("the scope taken away: the connection to the shell is given up", lambda: shell.live == 0)
    check("and the client is told, and Vision is gone for it",
          any(e["event"] == "gateway.scopes" and e["data"] == {"scopes": ["status"]} for e in events(a)) and
          err_code(a.call("vision.status")) == 7)
    shell.vpresent = True
    shell.calls.clear()
    scope(ctl, A.fp_hex, "vision", True)
    wait_for("granted again: the connection is back", lambda: shell.live == 1)
    a.close()
    a = connect(A, fp)

    # ---- 4. the status ----------------------------------------------------------------------------------------
    st = a.call("vision.status").get("result")
    check("vision.status: the contract's fields and no other", st is not None and list(st) == TOP)
    check("nothing of what the unit said beyond them", st is not None and vclean(st))
    check("its words and figures (%s)" % json.dumps(st), st is not None and st["owner"] == "vision" and st["mode"] == "detect" and
          st["share"] == "off" and list(st["stats"]) == STATS and st["pic"] == {"w": 264, "h": 469} and
          st["labels"] == ["person", "bicycle", "car"] and "face.owner" not in st["settings"] and
          st["settings"]["track.line"] == "across" and len(st["settings"]) == 8)
    check("vision.status takes no parameters", err_code(a.call("vision.status", {"labels": True})) == 2)
    shell.vfront = False
    check("Vision not in front: not available, and nothing else", a.call("vision.status").get("result") == {"available": False})
    check("and everything else is refused by the unit (code 3), its words left there",
          err_code(a.call("vision.reset")) == 3 and "LEAK" not in json.dumps(a.call("vision.reset")))
    shell.vfront = True
    n0 = len(ons())
    check("a preview without the topic is refused before the unit (code 3): there is nobody to send a picture to",
          err_code(share(a)) == 3 and len(ons()) == n0)

    # ---- 5. the topic: events made from the gateway's own questions -------------------------------------------
    events(a)
    r = a.call("gateway.subscribe", {"topics": ["vision"]}).get("result")
    check("the topic is held, and the answer says the shell is there", r is not None and r["topics"] == ["vision"] and
          r["upstream"].get("shell") is True)
    d = wait_event(a, "vision.state", lambda d: True, 2)
    check("a new listener is told once to ask how Vision is: vision.state, empty", d == {})
    n0 = len(shell.of("vision.status"))
    time.sleep(3.2)
    polls = len(shell.of("vision.status")) - n0
    check("while a client listens the gateway asks about once a second (%d in 3.2 s)" % polls, 2 <= polls <= 5)
    ev = events(a)
    stats = [e["data"] for e in ev if e["event"] == "vision.stats"]
    check("the figures come about once a second (%d)" % len(stats), 2 <= len(stats) <= 6)
    check("each is the nine figures and nothing else", all(list(s) == STATS for s in stats) and vclean(ev))
    check("the state is said once per change, not with every answer",
          len([e for e in ev if e["event"] == "vision.state"]) <= 1)
    check("and no picture is sent to a client that did not ask for the preview",
          not [e for e in ev if e["event"] == "vision.frame"] and pulls() == 0)

    # ---- 6. the preview: at once, one picture, the next only after the acknowledgement -------------------------
    r = share(a)
    check("vision.share {on: true}: live at once, {state} and nothing else", r.get("result") == {"state": "live"})
    check("the unit is told who views: the start of the client's key and the name this unit gave it",
          ons() == [{"on": True, "client": A.fp_hex[:16], "name": "phone A"}])
    check("a client can name neither (code 2), nor anything but one boolean",
          all(err_code(a.call("vision.share", p)) == 2 for p in
              [None, {}, {"on": 1}, {"on": True, "client": B.fp_hex[:16]}, {"on": True, "name": "evil"},
               {"on": True, "seconds": 600}]) and len(ons()) == 1)
    f1 = frame(a)
    check("the first picture arrives without an acknowledgement: vision.frame with the contract's fields",
          f1 is not None and list(f1) == FRAME and f1["seq"] == 1 and f1["w"] == 264 and f1["h"] == 469)
    raw = base64.b64decode(f1["jpeg"]) if f1 else b""
    check("its picture is the unit's, as base64 of a JPEG, and its boxes are nine numbers each",
          len(raw) == 3000 and raw[:3] == b"\xff\xd8\xff" and f1 is not None and len(f1["boxes"]) == 2 and
          all(len(b) == 9 and all(type(x) is int for x in b) for b in f1["boxes"]) and vclean(f1["boxes"]))
    time.sleep(0.9)
    check("no second picture before the acknowledgement, and the unit is not asked for one",
          frames(a) == [] and pulls() == 1 and shell.of("vision.pull") == [{"after_seq": 0}])
    check("the viewer reads live", view(a) == "live")
    check("an acknowledgement of another picture is answered {} and changes nothing",
          ack(a, 7).get("result") == {} and ack(a, 0).get("result") == {} and frames(a, 0.6) == [] and pulls() == 1)
    check("vision.ack takes one whole number (code 2)",
          all(err_code(a.call("vision.ack", p)) == 2 for p in [None, {}, {"seq": "1"}, {"seq": 1.5}, {"seq": -1},
                                                              {"seq": 1, "all": True}]))
    check("vision.ack reaches nothing on the unit", not [m for m, _ in shell.calls if m == "vision.ack"])
    t0 = time.time()
    check("the acknowledgement of the picture sent is answered {}", ack(a, 1).get("result") == {})
    seqs = [1]
    for _ in range(6):
        f = frame(a)
        if f is None:
            break
        seqs.append(f["seq"])
        ack(a, f["seq"])
    took = time.time() - t0
    check("then the next comes, and so on: seq rising by one (%s)" % seqs, seqs == [1, 2, 3, 4, 5, 6, 7])
    check("never more than five a second: six more pictures took %.2f s" % took, 0.98 <= took <= 6)
    asked = [p["after_seq"] for p in shell.of("vision.pull")]
    check("each asked for with the number of the last", asked[:7] == [0, 1, 2, 3, 4, 5, 6] and
          all(set(p) == {"after_seq"} for p in shell.of("vision.pull")))
    f = frame(a)
    last = f["seq"] if f else -1

    # ---- 7. a second client: busy, and nothing of the picture ---------------------------------------------------
    pair(ctl, B, "phone B", ["status", "vision"])
    b = connect(B, fp, ["vision"])
    check("B holds the topic and is offered the capability", b.sub["topics"] == ["vision"] and "vision" in b.caps)
    n0 = len(shell.of("vision.share"))
    check("a second client is refused the preview (code 5) before the unit", err_code(share(b)) == 5 and
          len(shell.of("vision.share")) == n0)
    stb = b.call("vision.status").get("result")
    check("and reads busy, with nothing of who views", stb is not None and stb["share"] == "busy" and vclean(stb))
    check("while the viewer still reads live", view(a) == "live")
    check("its acknowledgement is answered {} and counts for nothing", ack(b, last).get("result") == {} and
          frames(a, 0.6) == [])
    check("its vision.share {on: false} is answered off and does not touch the unit",
          share(b, False).get("result") == {"state": "off"} and len(shell.of("vision.share")) == n0 and view(a) == "live")
    check("no picture went to it", not [e for e in events(b) if e["event"] == "vision.frame"])
    ack(a, last)
    f = frame(a)
    check("the viewer's own acknowledgement brings the next", f is not None and f["seq"] == last + 1)
    last = f["seq"] if f else -1
    check("and still none went to the other client", not [e for e in events(b, None, 0.3) if e["event"] == "vision.frame"])

    # ---- 8. mode, settings, counters ----------------------------------------------------------------------------
    events(a)
    events(b)
    check("vision.mode: {} whatever the unit said", a.call("vision.mode", {"mode": "track"}).get("result") == {} and
          shell.of("vision.mode") == [{"mode": "track"}])
    check("the modes of a face and of text cannot be chosen, nor anything else (code 2), and the unit is not asked",
          all(err_code(a.call("vision.mode", p)) == 2 for p in
              [{"mode": "face"}, {"mode": "recognize"}, {"mode": "read"}, {"mode": "FACE"}, {"mode": "camera"}, {},
               None, {"mode": "track", "force": True}]) and len(shell.of("vision.mode")) == 1)
    check("vision.set: one of the eight settings with one of its values",
          a.call("vision.set", {"key": "traffic.range", "value": "far"}).get("result") == {} and
          shell.of("vision.set") == [{"key": "traffic.range", "value": "far"}])
    check("anything else is refused before the unit (code 2)",
          all(err_code(a.call("vision.set", p)) == 2 for p in
              [{"key": "traffic.range", "value": "across"}, {"key": "face.owner", "value": "x"},
               {"key": "edge.hard", "value": 1}, {"key": "detector", "value": "big"}, {"key": "edge.hard"},
               {"key": "edge.hard", "value": "1", "persist": False}, None]) and len(shell.of("vision.set")) == 1)
    n_reset = len(shell.of("vision.reset"))
    check("vision.reset: {} and no parameters", a.call("vision.reset").get("result") == {} and
          err_code(a.call("vision.reset", {"all": True})) == 2 and shell.of("vision.reset")[-1] is None and
          len(shell.of("vision.reset")) == n_reset + 1)
    d = wait_event(b, "vision.state", lambda d: True, 3)
    check("a change of mode or setting is told to every listener as vision.state, empty", d == {})
    st = a.call("vision.status").get("result")
    check("and the status has it", st is not None and st["mode"] == "track" and st["settings"]["traffic.range"] == "far" and
          st["stats"]["count_ab"] == 0)

    # ---- 9. the unit has no new picture yet -------------------------------------------------------------------------
    shell.produce = False
    n0 = pulls()
    ack(a, last)
    time.sleep(0.8)
    more = pulls() - n0
    check("an answer without a picture is no picture, and the unit is asked again soon (%d times in 0.8 s)" % more,
          frames(a) == [] and 4 <= more <= 22)
    shell.produce = True
    f = frame(a)
    check("until there is one", f is not None and f["seq"] == last + 1)
    last = f["seq"] if f else -1

    # ---- 10. the largest picture passes; what is not as the contract says is dropped ----------------------------------
    shell.jpeg_bytes, shell.nboxes = 65536, 24
    ack(a, last)
    f = frame(a)
    check("a picture of 64 KiB with 24 boxes passes whole", f is not None and len(base64.b64decode(f["jpeg"])) == 65536 and
          len(f["jpeg"]) == 87384 and len(f["boxes"]) == 24 and f["seq"] == last + 1)
    last = f["seq"] if f else -1
    shell.jpeg_bytes = 65537
    n0 = pulls()
    ack(a, last)
    check("one byte more is dropped whole: no picture", frames(a, 1.0) == [])
    check("and the unit is asked on, at the interval, with the number of the last picture sent",
          2 <= pulls() - n0 <= 7 and shell.of("vision.pull")[-1] == {"after_seq": last})
    shell.nboxes = 25
    shell.jpeg_bytes = 3000
    check("a picture with more than 24 boxes is dropped", frames(a, 0.7) == [])
    first = True
    for what, fn in [("that is not base64", lambda p: dict(p, jpeg="/9j/" + "*" * 400)),
                     ("that is no JPEG", lambda p: dict(p, jpeg=base64.b64encode(b"<svg onload=LEAK>" * 9).decode())),
                     ("larger than 640", lambda p: dict(p, w=1280)),
                     ("with a box of eight numbers", lambda p: dict(p, boxes=[[1, 2, 3, 4, 5, 6, 7, 8]])),
                     ("with a box that holds text", lambda p: dict(p, boxes=[[1, 2, 3, 4, 5, 6, 7, 8, "LEAK"]])),
                     ("with a number that goes back", lambda p: dict(p, seq=1))]:
        shell.mangle = fn
        if first:
            shell.nboxes, first = 2, False
        check("a picture %s is dropped" % what, frames(a, 0.5) == [])
    shell.mangle = None
    f = frame(a)
    check("and the next good one is sent, with a number above all of them", f is not None and f["seq"] > last + 8 and
          len(base64.b64decode(f["jpeg"])) == 3000)
    last = f["seq"] if f else -1
    check("the viewer was not ended by any of that", view(a) == "live" and offs() == 0)

    # ---- 11. the viewer disconnects: the unit is told, the next client may view -------------------------------------
    events(b)
    a.close()
    wait_for("the viewer disconnected: the gateway tells the unit that the preview is over",
             lambda: offs() == 1 and shell.share == "off", 3)
    check("with {on: false} and nothing else, and the unit shares with nobody",
          shell.of("vision.share")[-1] == {"on": False} and shell.share == "off")
    d = wait_event(b, "vision.state", lambda d: True, 3)
    check("the other listener is told", d == {})
    check("and reads off, not busy", view(b) == "off")
    n0 = pulls()
    time.sleep(0.6)
    check("no picture is asked for while nobody views", pulls() == n0)
    n_p = pulls()
    r = share(b)
    check("now it may view", r.get("result") == {"state": "live"} and ons()[-1] == {"on": True, "client": B.fp_hex[:16],
                                                                                  "name": "phone B"})
    f = frame(b)
    check("and gets the pictures", f is not None and list(f) == FRAME)
    check("a new preview starts at the beginning: its first question is for anything after 0",
          shell.of("vision.pull")[n_p] == {"after_seq": 0})
    check("vision.share {on: true} again from the viewer is answered live without asking the unit",
          share(b).get("result") == {"state": "live"} and len(ons()) == 2)

    # ---- 12. the scope is taken away in the middle of the preview ------------------------------------------------------
    ack(b, f["seq"] if f else 0)
    f = frame(b)
    n_off = offs()
    t0 = time.time()
    r = scope(ctl, B.fp_hex[:12], "vision", False)
    check("gateway.scope takes vision away", r.get("result") == {"scopes": ["status"]})
    wait_for("and the unit is told at once that the preview is over", lambda: offs() == n_off + 1, 2)
    check("within a moment (%.2f s), without waiting for the next picture" % (time.time() - t0), time.time() - t0 < 1.0)
    ev = events(b)
    check("the client is told its scopes", any(e["event"] == "gateway.scopes" and e["data"] == {"scopes": ["status"]} for e in ev))
    n0 = pulls()
    check("its acknowledgement is not permitted any more (code 7)", err_code(ack(b, f["seq"] if f else 0)) == 7)
    check("no picture follows, and none is asked for", frames(b, 0.9) == [] and pulls() == n0)
    check("nor any event of Vision: the topic went with the scope",
          not [e for e in events(b, None, 1.3) if e["event"].startswith("vision.")])
    check("and nothing of Vision is permitted", err_code(b.call("vision.status")) == 7 and err_code(share(b)) == 7)

    # ---- 13. the viewer gives the topic back; the viewer is revoked ------------------------------------------------------
    scope(ctl, B.fp_hex, "vision", True)
    b.close()
    b = connect(B, fp, ["vision"])
    share(b)
    f = frame(b)
    n_off = offs()
    b.call("gateway.subscribe", {"topics": []})
    wait_for("the viewer gave the topic back: the unit is told", lambda: offs() == n_off + 1, 2)
    check("and no picture follows", frames(b, 0.6) == [])
    b.call("gateway.subscribe", {"topics": ["vision"]})
    check("it may view again when it listens again", share(b).get("result") == {"state": "live"} and frame(b) is not None)
    n_off = offs()
    check("the viewer is revoked on the unit", "result" in ctl.call("gateway.revoke", {"client": B.fp_hex}))
    wait_for("and the unit is told that its preview is over", lambda: offs() == n_off + 1 and shell.share == "off", 2)
    check("and shares with nobody", shell.share == "off")
    b.close()

    # ---- 14. STOP on the unit ------------------------------------------------------------------------------------------------
    a = connect(A, fp, ["vision"])
    check("A views again", share(a).get("result") == {"state": "live"})
    f = frame(a)
    events(a)
    n_off, n0 = offs(), pulls()
    shell.tap_stop()
    d = wait_event(a, "vision.state", lambda d: True, 3)
    check("STOP on the unit, while the viewer has not acknowledged: the gateway learns it from its own question and says vision.state",
          d == {})
    check("the client reads stopped", view(a) == "stopped")
    check("its acknowledgement brings no picture and no question to the unit",
          ack(a, f["seq"] if f else 0).get("result") == {} and frames(a, 0.7) == [] and pulls() == n0)
    check("the gateway did not tell the unit what the unit told it", offs() == n_off)
    check("a preview is refused by the unit until a person there allows it (code 3), its words left there",
          err_code(share(a)) == 3 and "LEAK" not in json.dumps(share(a)))
    shell.tap_resume()
    wait_event(a, "vision.state", lambda d: True, 3)
    check("allowed again on the unit: the client reads off, and may view", view(a) == "off" and
          share(a).get("result") == {"state": "live"})
    f = frame(a)
    check("and the pictures come", f is not None)
    # Vision is left while the viewer waits for the next picture: the pull is what is refused.
    events(a)
    n_off = offs()
    shell.produce = False
    ack(a, f["seq"] if f else 0)
    events(a, None, 0.3)
    with shell.lock:
        shell.share, shell.viewer = "off", None
    d = wait_event(a, "vision.state", lambda d: True, 2)
    check("the unit ends the sharing between two pictures: the refused pull ends the preview, and vision.state says so", d == {})
    time.sleep(0.3)
    n0 = pulls()
    shell.produce = True
    check("no picture follows and none is asked for", frames(a, 0.7) == [] and pulls() == n0 and view(a) == "off")
    check("and again the unit was not told what it said itself", offs() == n_off)

    # ---- 15. a mode that is never shared ----------------------------------------------------------------------------------------
    check("A views again", share(a).get("result") == {"state": "live"})
    f = frame(a)
    events(a)
    n_off = offs()
    shell.lenient = True
    with shell.lock:
        shell.vmode = "face"  # a unit that goes on sharing in FACE: it must not matter
    wait_for("the unit is in FACE and says it still shares: the gateway ends the preview itself and tells the unit",
             lambda: offs() == n_off + 1, 3)
    n0 = pulls()
    ack(a, f["seq"] if f else 0)
    check("no picture of that mode was sent, and none is asked for", frames(a, 0.8) == [] and pulls() == n0)
    st = a.call("vision.status").get("result")
    check("the client is told the mode the unit is in, and nothing else of it", st is not None and st["mode"] == "face" and
          st["share"] == "off" and vclean(st))
    r = share(a)
    n0 = pulls()
    got = frames(a, 1.5)
    check("a unit that says live in FACE all the same: not one picture reaches the client (answer %s)" % json.dumps(r),
          got == [] and view(a) == "off")
    wait_for("and it is told to stop again", lambda: shell.share == "off", 3)
    shell.lenient = False
    with shell.lock:
        shell.vmode = "detect"
    time.sleep(1.3)

    # ---- 16. a unit that shares for nobody -----------------------------------------------------------------------------------------
    n_off = offs()
    with shell.lock:
        shell.share, shell.viewer = "live", "00" * 8  # as after the gateway was started again in the middle of a preview
    wait_for("the unit says it shares and the gateway has no viewer: it is told to stop", lambda: offs() >= n_off + 1 and shell.share == "off", 3)
    check("and stops", shell.share == "off" and view(a) == "off")

    # ---- 17. no acknowledgement for five seconds ---------------------------------------------------------------------------------------
    check("A views again", share(a).get("result") == {"state": "live"})
    f = frame(a)
    t0 = time.time()
    n_off = offs()
    events(a)
    wait_for("a viewer that does not acknowledge: after five seconds the gateway ends the preview and tells the unit",
             lambda: offs() == n_off + 1, 9)
    took = time.time() - t0
    check("after about five seconds (%.1f s)" % took, 4.3 <= took <= 7.5)
    d = wait_event(a, "vision.state", lambda d: True, 2)
    check("the client is told, and reads off", d == {} and view(a) == "off")
    check("a late acknowledgement brings nothing", ack(a, f["seq"] if f else 0).get("result") == {} and frames(a, 0.6) == [])

    # ---- 18. how often ---------------------------------------------------------------------------------------------------------------------
    pair(ctl, R, "phone R", ["vision"])
    rc = connect(R, fp, ["vision"])
    n0 = len(shell.of("vision.reset"))
    codes = [err_code(rc.call("vision.reset")) for _ in range(20)]
    check("twenty requests in a minute are passed on", codes == [None] * 20 and len(shell.of("vision.reset")) == n0 + 20)
    n1 = len(shell.calls)
    check("the next is refused before the unit (code 10), whichever of the four it is",
          err_code(rc.call("vision.reset")) == 10 and err_code(rc.call("vision.mode", {"mode": "edge"})) == 10 and
          err_code(rc.call("vision.set", {"key": "edge.hard", "value": "1"})) == 10 and err_code(share(rc)) == 10 and
          not [m for m, _ in shell.calls[n1:] if m in ("vision.reset", "vision.mode", "vision.set", "vision.share")])
    check("reading the state, an acknowledgement and turning the preview off are not counted",
          "result" in rc.call("vision.status") and ack(rc, 1).get("result") == {} and
          share(rc, False).get("result") == {"state": "off"})
    check("and another client's own count is its own", a.call("vision.reset").get("result") == {})
    rc.close()

    # ---- 19. both apps on the one socket -----------------------------------------------------------------------------------------------------
    pair(ctl, C, "phone C", ["status", "capture_view", "vision"])
    c = connect(C, fp, ["capture", "vision"])
    check("a client with both scopes is offered both capabilities and holds both topics",
          "capture" in c.caps and "vision" in c.caps and c.sub["topics"] == ["capture", "vision"])
    check("and reads both through the one connection to the shell",
          (c.call("capture.status").get("result") or {}).get("available") is True and
          (c.call("vision.status").get("result") or {}).get("available") is True and shell.live == 1)
    check("C views", share(c).get("result") == {"state": "live"})
    f = frame(c)
    ev = events(c, None, 1.3)
    check("pictures, the capture's state and Vision's figures reach it side by side",
          f is not None and {"capture.state", "vision.stats"} <= {e["event"] for e in ev} | {"capture.state"})
    check("everything the gateway ever asked on the shell's socket is one of the six vision names or the capture's own",
          {m for m, _ in shell.calls} <= SIX | {"capture.status"})
    check("and it asked for no shell.* method and no subscription", not [m for m, _ in shell.calls if m.startswith("shell.")])

    # ---- 20. the gateway stops in the middle of a preview ----------------------------------------------------------------------------------------
    n_off = offs()
    err = stop()
    check("the gateway is stopped while C views: the unit was told that the preview is over", offs() == n_off + 1 and
          shell.share == "off")
    log = stop_err + err
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as fh:
            log += fh.read()
    pa, pb = A.fp_hex[:16], B.fp_hex[:16]
    check("the log has the lifecycle of the previews, by the start of the client's key",
          "Vision answers on the shell's socket" in log and ("the preview of %s began" % pa) in log and
          ("the preview of %s ended (its client went away)" % pa) in log and
          ("the preview of %s ended (its client may no longer view)" % pb) in log and
          ("the preview of %s ended (its client no longer listens)" % pb) in log and
          ("the preview of %s ended (the unit stopped sharing)" % pa) in log and
          ("the preview of %s ended (a mode that is never shared)" % pa) in log and
          ("the preview of %s ended (no acknowledgement)" % pa) in log)
    check("a dropped answer is said once, not every time", log.count("was dropped (said once)") == 1)
    check("the log holds no picture, no name of a client, nothing of the unit's answers",
          all(w not in log for w in ("LEAK", "/9j/", "phone A", "phone B", "4242", "yolov8n", A.fp_hex, B.fp_hex)))
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

print("gatewayd vision e2e: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
