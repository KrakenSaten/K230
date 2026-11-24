#!/usr/bin/env python3
"""Vision for a paired companion, whole, on the host: the real gatewayd (the
test-hooks build) in front of the real shell (the SDL simulator) with the real
Vision app, the real Settings app and the real pos-vision on the fake camera,
and real TLS clients. No fake stands between them.

  python3 tests/companion_vision_integration_test.py <gatewayd-testhooks> <pocketos-shell> <pos-vision with libjpeg>

What is driven, by a finger on the unit's screen and from a paired client,
and checked on both sides:

  - a phone is given `vision` by the switch on Settings > Companion and by
    nothing a phone can do; the switch takes it away again;
  - the picture comes by itself once Vision is in front (real JPEGs);
  - STOP on the Vision screen ends it at once and holds against the phone,
    also when the shell creates the Vision app again by itself (a text size
    changed over IPC); only ALLOW, tapped there, lets the phone back;
  - FACE, RECOGNIZE and READ, tapped on the unit, end it at once, and
    cannot be shared or chosen from the phone, which is told the mode's
    name and nothing of its picture;
  - the viewer disconnecting, losing the scope and being revoked each end it
    at once on the unit;
  - leaving Vision ends it, and no helper is left.

A "tap" is the simulator's own finger (shell.tap, a test hook of the
simulator build). The private modes are offered here because the helper is
pointed at stand-in model files; nothing of a face or a text is made.

Needs a pos-vision built with libjpeg (POCKETCAM_JPEG=1): without one no
picture can be shared, and the suite says so and fails.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import base64
import json
import os
import select
import signal
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403
from gatewayd_capture_lib import connect, events, wait_for  # noqa: E402

SHELL_BIN = os.path.abspath(sys.argv[2])
HELPER = os.path.abspath(sys.argv[3])
SOCK = os.path.join(RUN, "shell.sock")
CONF = os.path.join(ROOT, "conf")
os.makedirs(CONF)
# A stand-in for the models of the private modes: the helper offers a mode
# when its files can be read, and the fake detector runs none of them.
STANDIN = os.path.join(ROOT, "standin.model")
with open(STANDIN, "wb") as fh:
    fh.write(b"not a model\n")
shell_proc = None


def shell_start():
    global shell_proc
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=RUN, POCKETOS_STATE_DIR=STATE, POCKETOS_LOG_DIR=LOGS,
               POCKETOS_CONFIG_DIR=CONF, SDL_VIDEODRIVER="dummy", POCKETOS_VISION_HELPER=HELPER,
               POCKETOS_CAMERA_BACKEND="fake", POCKETOS_CAMERA_FAKE="period=20",
               POCKETOS_VISION_KPU_SCRIPT="box=2:800:100:120:120:80:3:0,box=0:700:400:40:60:160",
               POCKETOS_VISION_FACE_DET=STANDIN, POCKETOS_VISION_FACE_EMBED=STANDIN,
               POCKETOS_VISION_TEXT_DET=STANDIN, POCKETOS_VISION_TEXT_REC=STANDIN, POCKETOS_VISION_TEXT_DICT=STANDIN)
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


def tap(text, lift=True):
    """A finger on the label that reads exactly this. False when no such
    label is on the screen (nothing is touched then). The simulator takes
    one tap at a time, so the finger is lifted before this returns."""
    ok = "result" in unit("shell.tap", {"text": text})
    if lift:
        time.sleep(0.2)
    return ok


def info():
    return unit("shell.info")["result"]


def shot(name):
    """A picture of the unit's screen, kept when VISION_INT_SHOTS names a directory."""
    d = os.environ.get("VISION_INT_SHOTS")
    if d:
        unit("shell.screenshot", {"path": os.path.join(d, name + ".png")})


def refused3(what, c):
    """The unit's refusal of a share, as a code alone."""
    r = share(c)
    if err_code(r) != 3:
        print("     %s: answered %s" % (what, json.dumps(r)[:200]))
    check(what, err_code(r) == 3)


def helpers():
    out = subprocess.run(["pgrep", "-f", HELPER + " session"], capture_output=True, text=True).stdout.split()
    return len(out)


def ustatus():
    """Vision's state as the unit itself holds it."""
    return unit("vision.status").get("result") or {}


def pstatus(c):
    """And as a phone is told it."""
    return c.call("vision.status").get("result") or {}


def link(ident, fp, topics=None):
    c = connect(ident, fp, topics)
    c.ident = ident
    return c


asked = {}


def paced(c):
    """The gateway takes 20 of vision.share, vision.mode, vision.set and
    vision.reset a minute from one client (ADR-019 amendment 1, change 7).
    This suite asks more often than a phone would, so it waits its turn
    rather than meet that limit, which has its own test."""
    times = asked.setdefault(c.ident.fp_hex, [])
    now = time.time()
    times[:] = [t for t in times if now - t < 61]
    if len(times) >= 18:
        time.sleep(max(0.0, 61 - (now - times[0])))
    times.append(time.time())


def share(c, on=True):
    if on:
        paced(c)
    return c.call("vision.share", {"on": on})


def take(c, seconds=0.0, want=0):
    """The pictures that arrive within `seconds` (or until `want` have),
    each acknowledged as the app does, so that the next one is pulled. It
    listens and asks nothing but the acknowledgements: a client that asked
    for news thirty times a second would meet the gateway's general limit."""
    got = []
    end = time.time() + seconds
    while True:
        for e in [e for e in c.events if e["event"] == "vision.frame"]:
            c.events.remove(e)
            got.append(e["data"])
            c.call("vision.ack", {"seq": e["data"]["seq"]})
        left = end - time.time()
        if (want and len(got) >= want) or left <= 0:
            return got
        if c.s.pending() or select.select([c.s], [], [], min(left, 0.1))[0]:
            msg = recv_frame(c.s)
            if "event" in msg:
                c.events.append(msg)


def is_jpeg(d):
    raw = base64.b64decode(d["jpeg"], validate=True)
    return raw[:2] == b"\xff\xd8" and raw[-2:] == b"\xff\xd9" and len(raw) <= 65536 and 1 <= d["w"] <= 640 and 1 <= d["h"] <= 640


def ended(what, c, t0):
    """After something that must end the preview at `t0`: how long the unit
    took to stop sharing, and that no picture reaches the client afterwards
    (one that was already on its way when it happened may still land)."""
    ok = wait_for("%s: the unit stops sharing" % what, lambda: ustatus().get("share") != "live", 3)
    ms = int((time.time() - t0) * 1000)
    if c is not None:
        early = take(c, 0.3)
        late = take(c, 1.5)
        check("%s: no picture reaches the phone after it (%d already on the way)" % (what, len(early)),
              ok and len(early) <= 1 and not late)
    print("     %s: the unit had stopped sharing %d ms after it" % (what, ms))
    return ms


def live(what, c):
    r = share(c)
    if (r.get("result") or {}).get("state") != "live":
        print("     %s: answered %s; the unit says %s" % (what, json.dumps(r)[:200], json.dumps(ustatus())[-160:]))
    check("%s: vision.share is live" % what, (r.get("result") or {}).get("state") == "live")
    got = take(c, 6, want=2)
    check("%s: pictures arrive, real JPEGs within the bounds" % what, len(got) >= 2 and all(is_jpeg(d) for d in got))
    return got


def key_line(ident, connected):
    k = ident.fp_hex[:16].upper()
    return "Key %s %s %s %s%s" % (k[0:4], k[4:8], k[8:12], k[12:16], " · connected" if connected else "")


def scopes_of(ctl, ident):
    for c in ctl.call("gateway.clients")["result"]["clients"]:
        if c["fingerprint"] == ident.fp_hex:
            return c["scopes"]
    return None


ids = {n: Identity(n) for n in "ab"}
meshd = None
a = b = None

try:
    print("companion vision integration (real gatewayd, real shell, real helper)")
    set_password(True)
    meshd = FakeService("meshcored", MESHD)
    shell_start()
    unit("shell.unlock")
    ctl = start()
    ctl.call("gateway.subscribe")
    A, B = ids["a"], ids["b"]

    # ---- 1. a phone as a pairing leaves it: no vision ---------------------------------------------------
    fp, d, _ = pair(ctl, A, "phone A")
    check("A is paired, with what a pairing gives", d["result"]["paired"] is True and "vision" not in scopes_of(ctl, A))
    a = link(A, fp)
    check("the capability is not offered to it", "vision" not in a.caps)
    check("vision.status and vision.share are refused", err_code(a.call("vision.status")) is not None and
          err_code(share(a)) is not None)
    check("it cannot give itself the scope: the local methods are not a phone's (code 1)",
          err_code(a.call("gateway.scope", {"client": A.fp_hex, "scope": "vision", "on": True})) == 1 and
          err_code(a.call("gateway.set_scopes", {"client": A.fp_hex, "scopes": ["status", "vision"]})) == 1 and
          "vision" not in scopes_of(ctl, A))

    # ---- 2. the switch on the unit: Settings > Companion ---------------------------------------------------
    unit("shell.open", {"id": "settings"})
    time.sleep(1.2)
    check("Settings lists Companion, with the phones and that none is shown Vision",
          tap("1 phone · Vision not shown to any"))
    time.sleep(1.3)
    shot("settings-companion-off")
    check("its page names the phone and the start of its key, as the phone shows it",
          tap("phone A") and tap(key_line(A, True)))
    check("its switch reads OFF", not tap("ON"))
    events(a)
    check("a tap on the switch", tap("OFF"))
    time.sleep(0.4)
    check("gatewayd now holds vision for that phone, and its other scopes as they were",
          sorted(scopes_of(ctl, A)) == sorted(["status", "mesh_read", "mesh_send", "vision"]))
    check("the switch reads ON", not tap("OFF"))
    shot("settings-companion-on")
    ev = [e for e in events(a, "gateway.scopes", 0.3)]
    check("the connected phone is told its scopes", ev and "vision" in ev[-1]["data"]["scopes"])
    a.close()
    a = link(A, fp, ["vision"])
    check("on its next connection the capability is offered", "vision" in a.caps)
    check("Vision is not open on the unit: not available, and nothing else", pstatus(a) == {"available": False})

    # ---- 3. the picture comes when Vision is in front ---------------------------------------------------------
    unit("shell.open", {"id": "vision"})
    wait_for("Vision goes live on the unit", lambda: ustatus().get("owner") == "vision", 12)
    if (share(a).get("result") or {}).get("state") != "live":
        check("this pos-vision can make a JPEG (build it with POCKETCAM_JPEG=1)", False)
        raise RuntimeError("no picture can be shared with this helper")
    got = take(a, 6, want=3)
    check("the picture arrives by itself: real JPEGs within the bounds, numbered upwards",
          len(got) >= 3 and all(is_jpeg(d) for d in got) and [d["seq"] for d in got] == sorted({d["seq"] for d in got}))
    check("each with the boxes of its own frame", all(len(d["boxes"]) <= 24 and all(len(x) == 9 for x in d["boxes"]) for d in got))
    st = ustatus()
    check("the unit says to whom it is shown", st.get("share") == "live" and st.get("client") == A.fp_hex[:16])
    check("the mark is on the Vision screen with the phone's name", tap("SHOWN ON phone A"))
    shot("vision-shown")
    check("the phone is told live, and not who the viewer is", pstatus(a).get("share") == "live" and "client" not in pstatus(a))

    # ---- 4. STOP, by a finger; it holds; the shell creating the app again does not undo it ----------------------
    take(a, 0.2)
    t0 = time.time()
    check("STOP, tapped on the unit", tap("STOP", lift=False))
    ended("STOP", a, t0)
    check("the unit says stopped", ustatus().get("share") == "stopped")
    wait_for("the phone is told stopped", lambda: pstatus(a).get("share") == "stopped", 4)
    refused3("the phone asking again is refused (code 3)", a)
    check("the screen says so and offers ALLOW", tap("SHARING STOPPED"))
    shot("vision-stopped")
    r = unit("shell.text_size", {"size": "medium"})
    check("a text size changed over IPC: the shell creates the Vision app again", "result" in r)
    wait_for("Vision is live again after it", lambda: ustatus().get("owner") == "vision", 12)
    time.sleep(0.5)
    check("and STOP still stands on the unit", ustatus().get("share") == "stopped")
    refused3("the phone is still refused (code 3)", a)
    check("and no picture went out", not take(a, 1.2))
    check("the mark still says so", tap("SHARING STOPPED"))
    unit("shell.text_size", {"size": "small"})
    wait_for("Vision is live again after the size is put back", lambda: ustatus().get("owner") == "vision", 12)
    time.sleep(0.5)
    check("once more created again, once more stopped", ustatus().get("share") == "stopped")
    unit("shell.open", {"id": "vision"})
    wait_for("Vision is live again after an open request that names the app already open",
             lambda: ustatus().get("owner") == "vision", 12)
    time.sleep(0.5)
    check("that is not Vision opened anew either: still stopped", ustatus().get("share") == "stopped")
    refused3("and the phone refused (code 3)", a)
    check("ALLOW, tapped on the unit", tap("ALLOW"))
    check("the unit says off: nothing is shared until the phone asks", ustatus().get("share") == "off")
    live("after ALLOW", a)

    # ---- 5. the private modes, chosen by a finger ----------------------------------------------------------------
    for word, label in (("face", "FACE"), ("read", "READ"), ("recognize", "RECOGNIZE")):
        if ustatus().get("share") != "live":
            live("before %s" % label, a)
        take(a, 0.2)
        check("%s: the mode sheet opens, with STOP still there" % label, tap("MODE") and tap("SHOWN ON phone A"))
        # Where the mark is: a finger on its words lands above the sheet's
        # first caption with room between (the mark is 52 px tall there and
        # the caption 24), so no choice of the sheet is under it.
        mark = unit("shell.tap", {"text": "SHOWN ON phone A"}).get("result") or {}
        time.sleep(0.2)
        first = unit("shell.tap", {"text": "GENERAL"}).get("result") or {}
        time.sleep(0.2)
        check("%s: the mark lies above the sheet's first row, not over it (mark y %s, first caption y %s)" %
              (label, mark.get("y"), first.get("y")), "y" in mark and "y" in first and first["y"] - mark["y"] >= 38)
        shot("vision-sheet-%s" % word)
        time.sleep(0.4)
        t0 = time.time()
        check("%s, tapped on the unit" % label, tap(label, lift=False))
        ended(label, a, t0)
        check("%s is the mode on the unit, and nothing is shared" % label,
              ustatus().get("mode") == word and ustatus().get("share") == "off")
        p = pstatus(a)
        check("%s: the phone is told the mode's name and that nothing is shared, as the contract says" % label,
              p.get("mode") == word and p.get("share") == "off")
        refused3("%s: a share is refused (code 3)" % label, a)
        check("%s: no picture leaves while it is the mode" % label, not take(a, 1.0))
        paced(a)
        check("%s: and it cannot be chosen from the phone" % label, err_code(a.call("vision.mode", {"mode": word})) is not None)
        tap("MODE")
        time.sleep(0.4)
        check("back to DETECT on the unit", tap("DETECT"))
        wait_for("DETECT is the mode again", lambda: ustatus().get("mode") == "detect", 5)
    live("after the private modes", a)

    # ---- 6. the viewer goes: disconnect, scope, revocation ------------------------------------------------------------
    t0 = time.time()
    a.close()
    ended("the viewer disconnects", None, t0)
    check("the unit names no viewer", "client" not in ustatus())
    a = link(A, fp, ["vision"])
    live("reconnected", a)
    take(a, 0.2)
    t0 = time.time()
    r = ctl.call("gateway.scope", {"client": A.fp_hex, "scope": "vision", "on": False})
    ended("vision taken from the viewer", a, t0)
    check("the scope is gone, and Vision with it for that phone", "vision" not in r["result"]["scopes"] and
          err_code(a.call("vision.status")) is not None and err_code(share(a)) is not None)
    ctl.call("gateway.scope", {"client": A.fp_hex, "scope": "vision", "on": True})
    a.close()
    a = link(A, fp, ["vision"])
    live("granted again", a)
    t0 = time.time()
    check("the viewer is revoked on the unit", "result" in ctl.call("gateway.revoke", {"client": A.fp_hex}))
    ended("the viewer is revoked", None, t0)
    check("its connection is closed", a.closed(3))
    a = None

    # ---- 7. taking it away with the switch -------------------------------------------------------------------------------
    pair(ctl, B, "phone B", ["status", "vision"])
    b = link(B, fp, ["vision"])
    # The last phone with vision was revoked, so the gateway let go of the
    # shell's socket; a phone with vision is paired again, and it takes it up.
    t0 = time.time()
    wait_for("the gateway reaches Vision again for the new phone", lambda: pstatus(b).get("available") is True, 15)
    print("     the gateway had the shell's socket again %d ms after the new phone connected" % int((time.time() - t0) * 1000))
    live("a second phone, paired with vision", b)
    take(b, 0.2)
    t0 = time.time()
    unit("shell.open", {"id": "settings"})
    ended("Vision left for Settings", b, t0)
    check("Companion is opened there", tap("Companion"))
    time.sleep(1.3)  # the page asks gatewayd when it opens and once a second
    check("the phone is told Vision is not available", pstatus(b) == {"available": False})
    shot("settings-companion-b")
    check("the page lists the phone that is left, switched ON", tap("phone B") and tap(key_line(B, True)) and not tap("OFF"))
    events(b)
    check("a tap on the switch", tap("ON"))
    time.sleep(0.4)
    check("gatewayd no longer holds vision for it", scopes_of(ctl, B) == ["status"])
    check("the switch reads OFF", not tap("ON"))
    ev = events(b, "gateway.scopes", 0.3)
    check("the phone is told", ev and "vision" not in ev[-1]["data"]["scopes"])
    unit("shell.open", {"id": "vision"})
    wait_for("Vision is live again", lambda: ustatus().get("owner") == "vision", 12)
    check("and with Vision in front again that phone gets nothing", err_code(share(b)) is not None and
          ustatus().get("share") == "off" and not take(b, 1.0))

    # ---- 8. what is left ---------------------------------------------------------------------------------------------------
    unit("shell.home")
    time.sleep(1.0)
    check("Vision left: no helper remains", helpers() == 0)
    st = info()["app_ipc"]
    print("     the shell's handlers: %d calls, %d over the budget, worst %s us (%s)" %
          (st["calls"], st["slow"], st.get("worst_us"), st.get("worst_method")))
    check("no answer of the app was over its size limit", st["too_large"] == 0)
    b.close()
    b = None
    err = stop()
    shell_stop()
    log = err
    for name in os.listdir(LOGS):
        with open(os.path.join(LOGS, name), errors="replace") as fh:
            log += fh.read()
    check("the logs of both hold no picture and no phone's name", "/9j/" not in log and "phone A" not in log and "phone B" not in log)
except Exception as ex:  # a broken step is a failure, not a traceback alone
    import traceback

    traceback.print_exc()
    check("the suite ran to its end (%s)" % ex, False)
finally:
    for c in (a, b):
        if c is not None:
            c.close()
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

print("companion vision integration: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
