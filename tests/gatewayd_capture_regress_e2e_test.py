#!/usr/bin/env python3
"""gatewayd, the capture group: regressions of the request and its
cancellation, end to end (the real service, a fake shell, real TLS clients).

  python3 tests/gatewayd_capture_regress_e2e_test.py <path to gatewayd-testhooks>

Each of these went wrong, or could have, in a way the first suite
(tests/gatewayd_capture_e2e_test.py) does not show:

  1. the last client that may read a capture is revoked while its request
     waits: the cancel must leave before the gateway gives up its connection
     to the shell;
  2. capture_control is taken away while the request is still on its way to
     the unit: the client is refused the answer, and the request is taken
     back;
  3. a client leaves and comes back: its old request is taken back once, and
     its new connection is the same client to the rate limit;
  4. a request the unit never answered is taken back again and again, by its
     own name, and those late cancels leave another client's newer request
     alone;
  5. the shell goes away with a request waiting and comes back: the request
     is taken back on the new connection;
  6. the gateway stops with a request waiting: the cancel leaves first.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import *  # noqa: E402,F401,F403
from gatewayd_capture_lib import *  # noqa: E402,F401,F403

ids = {n: Identity(n) for n in "abcdefg"}
shell = None
meshd = None


def cancels(sh):
    return sh.of("capture.request_cancel")


def token_of(sh):
    return sh.of("capture.request_start")[-1]["request"]


try:
    print("gatewayd capture regressions e2e")
    set_password(True)
    meshd = FakeService("meshcored", MESHD)
    shell = FakeShell()
    ctl = start()
    ctl.call("gateway.subscribe")

    # ---- 1. the last client that may read is revoked while its request waits -----------------------
    A = ids["a"]
    fp, d, _ = pair(ctl, A, "phone A", CONTROL)
    a = connect(A, fp, ["capture"])
    check("A, the only client with a capture scope, asks: shown", a.call("capture.request_start", ASK).get("result") == {"shown": True})
    ta = token_of(shell)
    r = ctl.call("gateway.revoke", {"client": A.fp_hex})
    check("A is revoked", "result" in r and a.closed())
    wait_for("the request is taken back by its name, though nobody may read a capture any more",
             lambda: cancels(shell) == [{"request": ta}], 4)
    wait_for("and only then is the connection to the shell given up", lambda: shell.live == 0, 6)
    check("no sheet is left up on the unit", shell.request == "refused")
    check("the cancel was sent once", cancels(shell) == [{"request": ta}])

    # ---- 2. capture_control taken away while the request is on its way -------------------------------
    B = ids["b"]
    pair(ctl, B, "phone B", CONTROL)
    wait_for("a client with a capture scope again: the connection is back", lambda: shell.live == 1, 8)
    b = connect(B, fp, ["capture"])
    shell.delay = 1.0
    rid = b.send("capture.request_start", ASK)
    time.sleep(0.25)
    scopes(ctl, B, VIEW)  # while the unit has not answered yet
    r = b.read(rid)
    shell.delay = 0.0
    check("the answer that comes after the scope went is not given to the client (code 7)", err_code(r) == 7)
    tb = token_of(shell)
    wait_for("and the request it made is taken back by its name", lambda: {"request": tb} in cancels(shell), 5)
    wait_for("so no sheet stays up for a client that may no longer ask", lambda: shell.request == "refused", 4)
    check("it still reads", "result" in b.call("capture.status"))
    b.close()

    # ---- 3. gone and back again ---------------------------------------------------------------------------
    C = ids["c"]
    pair(ctl, C, "phone C", CONTROL)
    c = connect(C, fp)
    c.call("capture.request_start", ASK)
    tc = token_of(shell)
    n = len(cancels(shell))
    c.close()
    wait_for("a client that leaves: its request is taken back", lambda: cancels(shell)[n:] == [{"request": tc}], 4)
    c = connect(C, fp, ["capture"])
    check("it comes back as the same client: not within 30 s (code 10)", err_code(c.call("capture.request_start", ASK)) == 10)
    time.sleep(2.2)
    check("and its old request is not taken back a second time", cancels(shell)[n:] == [{"request": tc}])
    check("the unit was asked for one start by it, not two", [p["client"] for p in shell.of("capture.request_start")].count("phone C") == 1)

    # ---- 4. late cancels of an unanswered request leave a newer request alone ------------------------------
    D, E = ids["d"], ids["e"]
    pair(ctl, D, "phone D", CONTROL)
    c.close()
    dc = connect(D, fp)
    shell.swallow = True  # the unit takes the request and never answers, and raises nothing
    t0 = time.time()
    r = dc.call("capture.request_start", ASK)
    shell.swallow = False
    td = token_of(shell)
    check("a request the unit does not answer: upstream unavailable (code 9, %.1f s)" % (time.time() - t0), err_code(r) == 9)
    dc.close()
    wait_for("the shell is reached again", lambda: shell.live == 1, 8)
    n = len(cancels(shell))
    wait_for("the unanswered request is taken back by its name, again and again while its sheet could still come up",
             lambda: cancels(shell)[n:].count({"request": td}) >= 2, 8)
    pair(ctl, E, "phone E", CONTROL)
    e = connect(E, fp, ["capture"])
    check("meanwhile another client asks: shown", e.call("capture.request_start", ASK).get("result") == {"shown": True})
    te = token_of(shell)
    n = len(cancels(shell))
    time.sleep(3.3)
    late = cancels(shell)[n:]
    check("the late cancels keep coming, each for the old request (%d)" % len(late), len(late) >= 2 and all(x == {"request": td} for x in late))
    check("and the newer request is still up, under its own name", shell.request == "shown" and shell.token == te and te != td)
    check("nobody was told that it was refused", not [x for x in events(e, "capture.state") if x["data"]["reason"] == "refused"])

    # ---- 5. the shell goes away with a request waiting, and comes back ----------------------------------------
    shell.stop()
    up = None
    end = time.time() + 6
    while up is None and time.time() < end:
        up = next((x["data"] for x in events(e, "gateway.upstream") if x["data"] == {"service": "shell", "connected": False}), None)
        time.sleep(0.2)
    check("the shell is gone: the client that asked is told it is out of reach", up is not None)
    check("and its methods are unavailable (code 9)", err_code(e.call("capture.status")) == 9)
    shell = FakeShell()
    shell.request, shell.token = "shown", te  # the unit kept its sheet through the gateway's reconnection
    wait_for("the shell is back: the request that waited is taken back on the new connection, by its name",
             lambda: {"request": te} in cancels(shell), 12)
    wait_for("and its sheet goes down", lambda: shell.request == "refused", 4)
    check("the new connection was asked for no start", not shell.of("capture.request_start"))
    e.close()

    # ---- 6. the gateway stops with a request waiting ---------------------------------------------------------------
    F = ids["f"]
    pair(ctl, F, "phone F", CONTROL)
    f = connect(F, fp)
    time.sleep(0.5)
    check("a request waits", f.call("capture.request_start", ASK).get("result") == {"shown": True})
    tf = token_of(shell)
    stop()
    wait_for("the gateway stopped: its cancel left first, by the request's name", lambda: {"request": tf} in cancels(shell), 3)
    check("and no sheet is left asking for a gateway that is gone", shell.request == "refused")
    check("nothing but the five capture methods was ever asked of the shell", {m for m, _ in shell.calls} <= FIVE)
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

print("gatewayd capture regressions e2e: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(0 if lib.failed == 0 else 1)
