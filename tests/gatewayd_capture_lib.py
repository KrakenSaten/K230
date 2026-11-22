#!/usr/bin/env python3
"""What the gatewayd capture suites share: a fake shell that serves
PacketShark's capture.* methods as the app answers them (with more in every
answer than any client may see), and the helpers of a client that listens.

Imported by tests/gatewayd_capture_e2e_test.py and
tests/gatewayd_capture_regress_e2e_test.py, after gatewayd_e2e_lib.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import re
import threading
import time

from gatewayd_e2e_lib import *  # noqa: F401,F403
from gatewayd_e2e_lib import Client, FakeService, check, recv_frame, send_frame

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
        self.swallow = False        # capture.request_start is taken and never answered, and raises nothing
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
                if m == "capture.request_start" and self.swallow:
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
