#!/usr/bin/env python3
"""The Companion's capture path, whole, on the host: the real gatewayd (the
test-hooks build) in front of the real shell with PacketShark (the SDL
simulator, on synthetic traffic, with the real pos-capture helper), and real
TLS clients. No fake stands between them.

  python3 tests/companion_capture_integration_test.py <gatewayd-testhooks> <pocketos-shell> <pos-capture>

What is driven, from a paired client's side and checked on the unit's side:
capture.status with PacketShark closed and open; that the gateway asks the
shell nothing until a client reads; a start request that opens PacketShark
and starts nothing, ended by REFUSE, by the client leaving, by
capture_control being taken away, and by START tapped on the panel; HOLD,
RESUME and STOP; the events and their figures; that figures leave the shell
only while a client reads; the last reading client revoked while its request
waits; the shell restarted under the gateway; and that no helper is left.

A "tap" is the simulator's own finger (shell.tap, a test hook of the
simulator build): the one thing in this suite that a person would do.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403
from gatewayd_capture_lib import STATE as STATE_KEYS  # noqa: E402  (not the state directory of gatewayd_e2e_lib)
from gatewayd_capture_lib import TOP, CONTROL, VIEW, clean, connect, events, scopes, wait_event, wait_for, word  # noqa: E402

SHELL_BIN = os.path.abspath(sys.argv[2])
HELPER = os.path.abspath(sys.argv[3])
SOCK = os.path.join(RUN, "shell.sock")
CONF = os.path.join(ROOT, "conf")
os.makedirs(CONF)
ASK = {"iface": "synth0", "limit_s": 300}
shell_proc = None


def shell_start():
    global shell_proc
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS,
               POCKETOS_CONFIG_DIR=CONF, SDL_VIDEODRIVER="dummy", POCKETOS_CAPTURE_SOURCE="synth:200:1",
               POCKETOS_CAPTURE_HELPER=HELPER)
    shell_proc = subprocess.Popen([SHELL_BIN], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 12
    while time.time() < end:
        if os.path.exists(SOCK):
            try:
                unit("shell.info")
                return
            except OSError:
                pass
        time.sleep(0.1)
    raise RuntimeError("the shell did not come up")


def shell_stop():
    global shell_proc
    if shell_proc is not None:
        shell_proc.send_signal(signal.SIGTERM)
        try:
            shell_proc.wait(5)
        except subprocess.TimeoutExpired:
            shell_proc.kill()
            shell_proc.wait()
        shell_proc = None


def unit(method, params=None):
    """One request at the unit itself, on the shell's socket: what a person
    or the bench does there, never what a phone can."""
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(SOCK)
    try:
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


def tap(text):
    return "result" in unit("shell.tap", {"text": text})


def on_screen(text):
    """Whether a label a finger can reach reads exactly this (the tap lands)."""
    return tap(text)


def info():
    return unit("shell.info")["result"]


def helpers():
    out = subprocess.run(["pgrep", "-f", HELPER + " session"], capture_output=True, text=True).stdout.split()
    return len(out)


class Listener(threading.Thread):
    """A local listener of the shell's events, as any process on the unit
    could be: it asks for nothing and only counts what it is sent."""

    def __init__(self):
        super().__init__(daemon=True)
        self.names = []
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(1)
        self.s.connect(SOCK)
        send_frame(self.s, {"id": 1, "method": "shell.subscribe"})
        self.alive = True
        self.start()

    def run(self):
        while self.alive:
            try:
                msg = recv_frame(self.s)
            except (socket.timeout, TimeoutError):
                continue
            except (EOFError, OSError, ValueError):
                return
            if "event" in msg:
                self.names.append((time.time(), msg["event"]))

    def count(self, name, since):
        return len([1 for t, n in self.names if n == name and t >= since])

    def stop(self):
        self.alive = False
        self.s.close()


def ask(c, params=None):
    return c.call("capture.request_start", params or ASK)


ids = {n: Identity(n) for n in "abcde"}
meshd = None
ear = None

try:
    print("companion capture integration (real gatewayd, real shell)")
    set_password(True)
    meshd = FakeService("meshcored", MESHD)
    shell_start()
    unit("shell.unlock")
    ctl = start()
    ctl.call("gateway.subscribe")

    # ---- 1. paired, connected, and nothing asked of the shell yet ------------------------------
    A, B, C, D, E = (ids[n] for n in "abcde")
    fp, d, _ = pair(ctl, A, "phone A", CONTROL)
    check("A is paired with capture_view and capture_control", d["result"]["paired"] is True)
    time.sleep(2.5)
    i0 = info()
    check("the gateway connected to the shell and asked it nothing: no app method was called",
          i0["app_ipc"]["calls"] == 0 and i0["app_ipc"]["closed_status"] == 0 and i0["app_ipc"]["closed_refused"] == 0)
    a = connect(A, fp)
    check("the capability is offered to A", "capture" in a.caps)
    r = a.call("capture.status").get("result")
    check("PacketShark is closed: capture.status says not available, and nothing else", r == {"available": False})
    check("capture.hold and capture.stop are the unit's refusal, as a code alone (3)",
          err_code(a.call("capture.hold", {"on": True})) == 3 and err_code(a.call("capture.stop")) == 3)
    check("none of that opened PacketShark", info()["current"] == "home")
    a.call("gateway.subscribe", {"topics": ["capture"]})

    # ---- 2. a start request: opens PacketShark, shows a sheet, starts nothing ---------------------
    r = ask(a)
    check("a start request: {shown: true}", r.get("result") == {"shown": True})
    time.sleep(0.8)
    check("PacketShark was opened by it", info()["current"] == "packetshark")
    st = unit("capture.status")["result"]
    check("the unit says a request is shown, and is still on setup with nothing captured",
          st["request"] == "shown" and st["state"] == "idle" and st["seen"] == 0)
    check("no helper was started", helpers() == 0)
    r = a.call("capture.status").get("result")
    check("the client reads setup: the contract's fields, no word of the request, nothing more",
          r is not None and list(r) == TOP and r["state"] == "idle" and clean(r))
    d = wait_event(a, "capture.state", lambda d: True)
    check("and is told so as an event", d is not None and list(d) == STATE_KEYS and word(d) == "idle")
    check("REFUSE, tapped on the panel", tap("REFUSE"))
    d = wait_event(a, "capture.state", lambda d: d["reason"] == "refused")
    check("the client is told: refused", d is not None and word(d) == "idle/refused")
    check("the sheet is gone and nothing started", not on_screen("REFUSE") and helpers() == 0 and
          unit("capture.status")["result"]["request"] == "refused")
    check("A may not ask again at once (code 10)", err_code(ask(a)) == 10)

    # ---- 3. the asking client leaves: the real sheet comes down -------------------------------------
    pair(ctl, B, "phone B", CONTROL)
    b = connect(B, fp)
    check("B asks: shown", ask(b).get("result") == {"shown": True})
    time.sleep(0.6)
    check("its sheet is up on the unit", unit("capture.status")["result"]["request"] == "shown")
    events(a)
    b.close()
    wait_for("B left: the sheet is taken down on the unit", lambda: unit("capture.status")["result"]["request"] == "refused", 5)
    d = wait_event(a, "capture.state", lambda d: d["reason"] == "refused")
    check("and A, who listens, is told it was refused", d is not None)
    check("nothing started", helpers() == 0 and unit("capture.status")["result"]["state"] == "idle")

    # ---- 4. capture_control taken away with the sheet up ------------------------------------------------
    pair(ctl, C, "phone C", CONTROL)
    a.close()
    c = connect(C, fp, ["capture"])
    check("C asks: shown", ask(c).get("result") == {"shown": True})
    time.sleep(0.6)
    scopes(ctl, C, VIEW)
    wait_for("capture_control taken from C: its sheet is taken down on the unit",
             lambda: unit("capture.status")["result"]["request"] == "refused", 5)
    check("C may still read, and not ask (code 7)", "result" in c.call("capture.status") and err_code(ask(c)) == 7)
    c.close()

    # ---- 5. START, tapped on the panel: the capture, its events, HOLD, RESUME, STOP -------------------------
    pair(ctl, D, "phone D", CONTROL)
    dc = connect(D, fp, ["capture"])
    ear = Listener()
    events(dc)
    check("D asks: shown", ask(dc).get("result") == {"shown": True})
    time.sleep(0.5)
    check("START tapped at once is not taken: the sheet is in its first moment", tap("START") and
          unit("capture.status")["result"]["request"] == "shown" and helpers() == 0)
    time.sleep(1.6)
    check("START, tapped on the panel by a finger", tap("START"))
    d = wait_event(dc, "capture.state", lambda d: d["state"] == "capturing", 10)
    check("the client is told it is capturing, and of no refusal on the way", d is not None and
          not [x for x in events(dc, "capture.state") if x["data"]["reason"] == "refused"])
    check("on the interface and with the limit it asked for, HEADERS", d is not None and d["iface"] == "synth0" and
          d["limit_s"] == 300 and d["keep"] == "headers")
    check("one helper, the app's own", helpers() == 1)
    t0 = time.time()
    time.sleep(3.3)
    stats = [x["data"] for x in events(dc, "capture.stats")]
    check("figures arrive about once a second (%d in 3.3 s)" % len(stats), 2 <= len(stats) <= 6)
    check("each is the contract's fields and nothing more, and marked as asked for",
          all(list(s) == TOP and clean(s) and s["requested_by_companion"] is True for s in stats))
    check("they are the real capture's: packets counted, rising, on synth0, all of them classed",
          len(stats) >= 2 and stats[-1]["seen"] > stats[0]["seen"] > 0 and stats[-1]["iface"] == "synth0" and
          sum(stats[-1]["classes"].values()) == stats[-1]["seen"] and stats[-1]["bytes"] > stats[-1]["seen"])
    check("while a client reads, the shell raises its figures (%d)" % ear.count("capture.stats", t0), ear.count("capture.stats", t0) >= 2)
    r = dc.call("capture.hold", {"on": True})
    check("capture.hold: taken", r.get("result") == {"state": "capturing", "changing": True})
    d = wait_event(dc, "capture.state", lambda d: d["state"] == "hold")
    check("the client is told: hold", d is not None)
    check("and the panel shows what a tap on HOLD gives: RESUME is there to tap", on_screen("RESUME"))
    d = wait_event(dc, "capture.state", lambda d: d["state"] == "capturing")
    check("that tap on RESUME is told to the client as capturing again", d is not None)
    r = dc.call("capture.hold", {"on": True})
    wait_event(dc, "capture.state", lambda d: d["state"] == "hold")
    check("held again by request; a second hold changes nothing", dc.call("capture.hold", {"on": True}).get("result") ==
          {"state": "hold", "changing": False})
    r = dc.call("capture.hold", {"on": False})
    d = wait_event(dc, "capture.state", lambda d: d["state"] == "capturing")
    check("capture.hold off: capturing again", r.get("result") == {"state": "hold", "changing": True} and d is not None)
    check("a second request over the running capture is the unit's refusal (code 3 or 10)", err_code(ask(dc)) in (3, 10))
    r = dc.call("capture.stop")
    d = wait_event(dc, "capture.state", lambda d: d["state"] == "stopped")
    check("capture.stop: stopped, by the user", r.get("result") == {"state": "capturing", "changing": True} and
          d is not None and word(d) == "stopped/user")
    last = wait_event(dc, "capture.stats", lambda s: s["state"] == "stopped")
    check("its totals follow, with no rate", last is not None and last["pps"] == 0 and last["seen"] > 0)
    check("the panel shows what a tap on STOP gives: DISCARD is offered, and the capture is kept", on_screen("DISCARD") and helpers() == 1)
    tap("STAY")
    check("capture.stop on the stopped capture is the unit's refusal (code 3)", err_code(dc.call("capture.stop")) == 3)

    # ---- 6. figures leave the shell only while a client reads -----------------------------------------------
    dc.call("gateway.subscribe", {"topics": []})
    time.sleep(6.5)  # the reader's mark has gone by now
    calls0 = info()["app_ipc"]["calls"]
    t1 = time.time()
    time.sleep(3.0)
    check("nobody reads: the gateway asks the shell nothing", info()["app_ipc"]["calls"] == calls0)
    check("and the shell raises no figures to anybody", ear.count("capture.stats", t1) == 0)
    dc.close()

    # ---- 7. the shell restarts under the gateway ------------------------------------------------------------------
    e = None
    pair(ctl, E, "phone E", CONTROL)
    e = connect(E, fp, ["capture"])
    wait_event(e, "capture.state", lambda d: d["state"] == "stopped")
    ear.stop()
    ear = None
    shell_stop()
    d = wait_event(e, "capture.state", lambda d: d["state"] == "ended", 8)
    check("the shell is gone: the capture's last word to the client", d is not None and word(d) == "ended/left")
    check("and its methods are unavailable (code 9)", err_code(e.call("capture.status")) == 9)
    wait_for("its helper went with it", lambda: helpers() == 0, 6)
    shell_start()
    unit("shell.unlock")
    wait_for("the shell is back: the gateway finds it again and reads", lambda: e.call("capture.status").get("result") == {"available": False}, 15)

    # ---- 8. the last client that may read is revoked with its request waiting -----------------------------------------
    for other in (A, B, C, D):
        ctl.call("gateway.revoke", {"client": other.fp_hex})
    check("E, now the only client with a capture scope, asks: shown", ask(e).get("result") == {"shown": True})
    time.sleep(0.6)
    check("PacketShark is open with the sheet up", info()["current"] == "packetshark" and
          unit("capture.status")["result"]["request"] == "shown")
    r = ctl.call("gateway.revoke", {"client": E.fp_hex})
    check("E is revoked", "result" in r and e.closed())
    wait_for("its sheet is taken down on the unit although nobody may read a capture any more",
             lambda: unit("capture.status")["result"]["request"] == "refused", 5)
    check("and nothing started", helpers() == 0 and unit("capture.status")["result"]["state"] == "idle")
    calls0 = info()["app_ipc"]["calls"]
    time.sleep(2.5)
    check("after that the gateway asks the shell nothing more", info()["app_ipc"]["calls"] <= calls0 + 1)

    # ---- 9. what is left -------------------------------------------------------------------------------------------------
    stats_info = info()["app_ipc"]
    print("     the shell's handlers: %d calls, %d slow, worst %s us (%s)" %
          (stats_info["calls"], stats_info["slow"], stats_info.get("worst_us"), stats_info.get("worst_method")))
    check("no answer of the app was over the shell's size limit", stats_info["too_large"] == 0)
    err = stop()
    shell_stop()
    time.sleep(0.5)
    check("no helper is left when both have stopped", helpers() == 0)
    log = err
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as fh:
            log += fh.read()
    check("the logs of both hold no client name and no figure of a capture",
          all(w not in log for w in ("phone A", "phone D", "\"seen\"", "requested_by_companion")))
except Exception as ex:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % ex, False)
finally:
    if ear is not None:
        ear.stop()
    if lib.proc is not None:
        lib.proc.kill()
        print(lib.proc.stderr.read().decode(errors="replace")[-2000:])
    shell_stop()
    if meshd is not None:
        meshd.stop()
    subprocess.run(["pkill", "-f", HELPER + " session"], capture_output=True)
    if lib.failed and os.environ.get("GATEWAYD_E2E_KEEP"):
        print("  kept:", ROOT)
    else:
        shutil.rmtree(ROOT, ignore_errors=True)

print("companion capture integration: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
