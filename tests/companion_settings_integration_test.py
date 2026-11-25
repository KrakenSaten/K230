#!/usr/bin/env python3
"""Companion Settings 2.0 end to end on the host (ADR-019 amendment 2).

    companion_settings_integration_test.py <gatewayd-testhooks> <sysd-testhooks>

The real sysd (its test-hooks build, every Companion path under a root of this
test's own: SYSD_TEST_COMPANION_ROOT) and the real gatewayd (its test-hooks
build: loopback, a port of its own, this root's passwd and shadow). Between
them an init script that keeps S70gatewayd's contract - GATEWAYD_ENABLE in
/etc/default/gatewayd, start and stop, the daemon's pid file - and starts
gatewayd directly rather than under pos-supervise (tests/initscript_test.sh
runs the real S70gatewayd under the real pos-supervise). sshd and S50sshd are
stand-ins that record what they are asked. The phone is the Python reference
client of the gateway suites, with a P-256 key of its own.

What is driven, over the two local sockets exactly as Settings drives them:
the gateway refused without a password; the device password set (SSH hardened
first, checked, reloaded; the shadow line a SHA-512 crypt of exactly that
password, verified with OpenSSL; nothing else in the file changed); the
gateway switched on through sysd and answering; a pairing window, the
candidate's key read from gateway.status before the decision, the decision
with scopes; scopes changed and a phone revoked; then a restart of sysd and a
"reboot" with an upgraded gatewayd binary, after which the switch, the
password, the unit's key and the pairing are all as they were and the phone
connects without pairing again; and the gateway switched off, written before
the stop. Nothing on the unit is touched: everything is under a temporary
directory.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import hashlib
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

GATEWAYD = os.path.abspath(sys.argv[1])
SYSD = os.path.abspath(sys.argv[2])
sys.argv = [sys.argv[0], GATEWAYD]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatewayd_e2e_lib as lib  # noqa: E402
from gatewayd_e2e_lib import (Client, Ctl, Identity, check, err_code, parse_uri, recv_frame,  # noqa: E402
                              send_frame)

CR = os.path.join(lib.ROOT, "unit")  # the unit's root, as sysd sees it
PW = "tangerine harbor 42"
VENDOR_SSHD = ("#PermitRootLogin prohibit-password\nPermitRootLogin yes\nPasswordAuthentication yes\n"
               "PermitEmptyPasswords no\nSubsystem\tsftp\t/usr/libexec/sftp-server\n")
SHADOW_EMPTY = "root::::::::\ndaemon:*:::::::\n"


def path(p):
    return os.path.join(CR, p.lstrip("/"))


def write(p, text, mode=0o644):
    os.makedirs(os.path.dirname(path(p)), exist_ok=True)
    with open(path(p), "w") as f:
        f.write(text)
    os.chmod(path(p), mode)


def read(p):
    try:
        with open(path(p)) as f:
            return f.read()
    except OSError:
        return None


def digest(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# ---- the unit's root ----------------------------------------------------------------

write("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\ndaemon:x:1:1:daemon:/usr/sbin:/bin/false\n")
write("/etc/shadow", SHADOW_EMPTY, 0o600)
write("/etc/ssh/sshd_config", VENDOR_SSHD)
os.makedirs(path("/run/pocketos"))
os.makedirs(path("/var/run"))
os.makedirs(path("/usr/sbin"))
shutil.copy(GATEWAYD, path("/usr/sbin/gatewayd"))  # installed, as the image installs it
# sshd: -t -f <file> accepts what lets root in by key only, and says so.
write("/usr/sbin/sshd", """#!/bin/sh
echo "sshd $*" >> "%s/sshd.log"
[ "$1" = -t ] || exit 1
grep -q '^PermitRootLogin prohibit-password$' "$3"
""" % CR, 0o755)
write("/etc/init.d/S50sshd", '#!/bin/sh\necho "S50sshd $1" >> "%s/sshd.log"\nexit 0\n' % CR, 0o755)
sshd = subprocess.Popen(["sleep", "600"])
write("/var/run/sshd.pid", "%d\n" % sshd.pid)
# S70gatewayd's contract, starting the test-hooks gatewayd.
write("/etc/init.d/S70gatewayd", """#!/bin/sh
CR="%(cr)s"
PIDF="$CR/run/pocketos/gatewayd.pid"
GATEWAYD_ENABLE=0
[ -r "$CR/etc/default/gatewayd" ] && . "$CR/etc/default/gatewayd"
running() { [ -s "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }
case "$1" in
start)
    [ "$GATEWAYD_ENABLE" = 1 ] || { echo "disabled"; exit 0; }
    running && { echo "already running"; exit 0; }
    POCKETOS_RUNTIME_DIR="%(run)s" POCKETOS_STATE_DIR="%(state)s" POCKETOS_LOG_DIR="%(logs)s" POCKETOS_LOG_STDERR=0 \\
        "$CR/usr/sbin/gatewayd" --test-listen 127.0.0.1 --test-port %(port)d \\
        --test-shadow "$CR/etc/shadow" --test-passwd "$CR/etc/passwd" </dev/null >/dev/null 2>&1 &
    echo $! > "$PIDF"
    echo "$1" >> "$CR/S70.log"
    echo OK ;;
stop)
    echo "$1" >> "$CR/S70.log"
    if running; then
        p=$(cat "$PIDF"); kill "$p"
        n=0; while kill -0 "$p" 2>/dev/null && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
        kill -0 "$p" 2>/dev/null && exit 1
    fi
    rm -f "$PIDF"; echo OK ;;
esac
""" % {"cr": CR, "run": lib.RUN, "state": lib.STATE, "logs": lib.LOGS, "port": lib.PORT}, 0o755)


# ---- sysd -------------------------------------------------------------------------------

class Sys:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        self.s.connect(os.path.join(lib.RUN, "sysd.sock"))
        self.next = 1

    def call(self, method, params=None):
        rid = self.next
        self.next += 1
        req = {"id": rid, "method": method}
        if params is not None:
            req["params"] = params
        send_frame(self.s, req)
        while True:
            msg = recv_frame(self.s)
            if msg.get("id") == rid:
                return msg

    def status(self):
        return self.call("companion.status")["result"]

    def wait_job(self, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            st = self.status()
            if st["state"] != "running":
                return st
            time.sleep(0.1)
        return self.status()


sysd_proc = None


def sysd_start():
    global sysd_proc
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=lib.RUN, POCKETOS_STATE_DIR=lib.STATE, POCKETOS_LOG_DIR=lib.LOGS,
               POCKETOS_LOG_STDERR="0", SYSD_TEST_COMPANION_ROOT=CR)
    sysd_proc = subprocess.Popen([SYSD], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 10
    while time.time() < end:
        try:
            s = Sys()
            s.call("system.info")
            return s
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("sysd did not come up")


def sysd_stop():
    global sysd_proc
    sysd_proc.send_signal(signal.SIGTERM)
    sysd_proc.wait(10)
    sysd_proc = None


def ctl_up(timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            c = Ctl()
            c.call("gateway.info")
            return c
        except OSError:
            time.sleep(0.05)
    return None


def gateway_running():
    p = read("/run/pocketos/gatewayd.pid")
    if not p:
        return False
    try:
        os.kill(int(p), 0)
        return True
    except OSError:
        return False


def verify_crypt(hashed, password):
    salt = hashed.split("$")[2]
    out = subprocess.run(["openssl", "passwd", "-6", "-salt", salt, "-stdin"], input=password.encode(),
                         capture_output=True, check=True).stdout.decode().strip()
    return out == hashed


def root_hash():
    line = [l for l in read("/etc/shadow").splitlines() if l.startswith("root:")][0]
    return line.split(":")[1]


def main():
    sysd = sysd_start()

    # ---- 1. a unit as it comes ---------------------------------------------------------
    st = sysd.status()
    check("as it comes: installed, off, not running, no password, SSH open, idle",
          st["installed"] is True and st["enabled"] is False and st["running"] is False and
          st["password_set"] is False and st["ssh"] == "open" and st["state"] == "idle")
    r = sysd.call("companion.set_enabled", {"enabled": True})
    check("the gateway cannot be switched on without a password (rule 9b), in sysd's words",
          err_code(r) == 3 and r["error"]["message"].startswith("password_required: "))
    check("and nothing ran", not gateway_running() and read("/etc/default/gatewayd") is None and read("/S70.log") is None)
    r = sysd.call("companion.set_enabled", {"enabled": "yes"})
    check("a switch that is not a boolean is refused", err_code(r) == 2)
    r = sysd.call("companion.set_enabled", {"enabled": True, "service": "sshd"})
    check("and so is any other member: the request names no service", err_code(r) == 2)
    r = sysd.call("companion.set_password", {"password": "short"})
    check("a password that is too short is refused before anything runs",
          err_code(r) == 2 and r["error"]["message"].startswith("invalid_password: "))

    # ---- 2. the device password --------------------------------------------------------
    r = sysd.call("companion.set_password", {"password": PW})
    check("the password is accepted and set in a job", r.get("result", {}).get("state") == "running")
    st = sysd.wait_job()
    check("set: the job is done, root has a password, SSH lets root in by key only",
          st["state"] == "done" and st["action"] == "password" and st["password_set"] is True and st["ssh"] == "hardened")
    h = root_hash()
    check("root's shadow field is a SHA-512 crypt of exactly that password (OpenSSL agrees)",
          h.startswith("$6$") and verify_crypt(h, PW) and not verify_crypt(h, PW + "x"))
    check("every other part of the shadow file is as it was",
          read("/etc/shadow") == "root:%s:::::::\ndaemon:*:::::::\n" % h)
    check("shadow stays 0600", stat.S_IMODE(os.stat(path("/etc/shadow")).st_mode) == 0o600)
    cfg = read("/etc/ssh/sshd_config")
    check("sshd_config: root by key only, every other line kept",
          cfg == VENDOR_SSHD.replace("PermitRootLogin yes\n", "PermitRootLogin prohibit-password\n"))
    check("the original kept beside it", read("/etc/ssh/sshd_config.doors-orig") == VENDOR_SSHD)
    log = read("/sshd.log") or ""
    check("sshd checked the hardened file before it was installed, then reloaded",
          "sshd -t -f %s.doors-check" % path("/etc/ssh/sshd_config") in log and "S50sshd reload" in log and
          log.index("sshd -t") < log.index("S50sshd reload"))

    # ---- 3. the gateway on -------------------------------------------------------------
    write("/etc/default/gatewayd", "# this unit's own\nGATEWAYD_INTERFACE=wlan0\n")
    r = sysd.call("companion.set_enabled", {"enabled": True})
    check("on: accepted", r.get("result", {}).get("state") == "running")
    st = sysd.wait_job()
    check("on: done, and the gateway runs", st["state"] == "done" and st["enabled"] is True and st["running"] is True)
    check("the switch written, the unit's own line kept",
          read("/etc/default/gatewayd") == "# this unit's own\nGATEWAYD_INTERFACE=wlan0\nGATEWAYD_ENABLE=1\n")
    ctl = ctl_up()
    check("gatewayd answers on its local socket", ctl is not None)
    gs = ctl.call("gateway.status")["result"]
    check("its own gate agrees: a password, and closed only because nobody is paired",
          gs["password_set"] is True and gs["listening"] is False and gs["closed"] == "no_clients")
    device_id = gs["device_id"]
    gwdir = os.path.join(lib.STATE, "gatewayd")

    # ---- 4. pairing, as Settings does it ------------------------------------------------
    phone = Identity("phone")
    r = ctl.call("gateway.pair_begin")
    _, port, unit_fp, token = parse_uri(r["result"]["uri"])
    check("a window: the code names this unit's key", unit_fp.hex() == device_id and port == lib.PORT)
    gs = ctl.call("gateway.status")["result"]
    check("the page sees the window and its time", gs["pair"]["state"] == "open" and gs["pair"]["left_ms"] > 100000)
    c = Client(phone, pin=unit_fp)
    c.hello()
    rid = c.send("gateway.pair", {"token": token, "name": "Pixel 8"})
    end = time.time() + 5
    while time.time() < end:
        gs = ctl.call("gateway.status")["result"]
        if gs.get("pair", {}).get("state") == "waiting":
            break
        time.sleep(0.05)
    check("before any decision, the page reads the candidate's name and key from the status (issue #172)",
          gs["pair"]["candidate"] == {"name": "Pixel 8", "fingerprint": phone.fp_hex})
    d = ctl.call("gateway.pair_decide", {"allow": True, "scopes": ["status", "vision"]})
    reply = c.read(rid)
    check("ALLOW with the chosen scopes pairs it with exactly those",
          d["result"]["paired"] is True and reply["result"] == {"paired": True, "scopes": ["status", "vision"]})
    c.close()
    gs = ctl.call("gateway.status")["result"]
    check("the window is closed and says why", "pair" not in gs and gs["pair_closed"] == "paired")
    c = Client(phone, pin=unit_fp)
    h = c.hello()
    check("the phone connects as itself", h["result"]["paired"] is True and h["result"]["scopes"] == ["status", "vision"])
    c.close()
    r = ctl.call("gateway.set_scopes", {"client": phone.fp_hex, "scopes": ["status", "mesh_read", "mesh_send"]})
    check("its permissions changed as the phone page does it", r["result"]["scopes"] == ["status", "mesh_read", "mesh_send"])
    other = Identity("other")
    r = ctl.call("gateway.pair_begin")
    _, _, _, token = parse_uri(r["result"]["uri"])
    c2 = Client(other, pin=unit_fp)
    c2.hello()
    rid = c2.send("gateway.pair", {"token": token, "name": "Old phone"})
    while ctl.call("gateway.status")["result"].get("pair", {}).get("state") != "waiting":
        time.sleep(0.05)
    ctl.call("gateway.pair_decide", {"allow": True})
    c2.read(rid)
    c2.close()
    r = ctl.call("gateway.revoke", {"client": other.fp_hex})
    lst = ctl.call("gateway.clients")["result"]["clients"]
    check("a second phone paired and revoked: only the first is left",
          "result" in r and [x["fingerprint"] for x in lst] == [phone.fp_hex])
    ctl.close()
    identity_before = digest(os.path.join(gwdir, "identity.v1"))
    paired_before = digest(os.path.join(gwdir, "paired.v1"))
    shadow_before = read("/etc/shadow")
    default_before = read("/etc/default/gatewayd")

    # ---- 5. sysd restarts; then a reboot with an upgraded gatewayd -----------------------
    sysd_stop()
    check("the gateway does not depend on sysd running", gateway_running())
    sysd = sysd_start()
    st = sysd.status()
    check("after a restart of sysd: on, running, password set, SSH hardened, nothing pending",
          st["enabled"] is True and st["running"] is True and st["password_set"] is True and st["ssh"] == "hardened" and
          st["state"] == "idle")
    sysd_stop()
    subprocess.run([path("/etc/init.d/S70gatewayd"), "stop"], check=True, stdout=subprocess.DEVNULL)
    # The upgrade: a deploy replaces the binary and init script and nothing a unit holds.
    os.unlink(path("/usr/sbin/gatewayd"))
    shutil.copy(GATEWAYD, path("/usr/sbin/gatewayd"))
    # The boot: rcS runs S70gatewayd start, which reads the unit's own switch.
    subprocess.run([path("/etc/init.d/S70gatewayd"), "start"], check=True, stdout=subprocess.DEVNULL)
    sysd = sysd_start()
    ctl = ctl_up()
    gs = ctl.call("gateway.status")["result"] if ctl else {}
    check("after the reboot the gateway is on again by itself, with the same key",
          gateway_running() and gs.get("device_id") == device_id)
    check("its key, its paired list, the switch and the password are byte for byte as they were",
          digest(os.path.join(gwdir, "identity.v1")) == identity_before and
          digest(os.path.join(gwdir, "paired.v1")) == paired_before and
          read("/etc/shadow") == shadow_before and read("/etc/default/gatewayd") == default_before)
    time.sleep(0.3)
    c = Client(phone, pin=unit_fp)
    h = c.hello()
    check("the phone connects without pairing again, with its permissions",
          h["result"]["paired"] is True and h["result"]["scopes"] == ["status", "mesh_read", "mesh_send"])
    c.close()
    ctl.close()

    # ---- 6. the gateway off ---------------------------------------------------------------
    open(path("/S70.log"), "w").close()
    r = sysd.call("companion.set_enabled", {"enabled": False})
    st = sysd.wait_job()
    check("off: done, not running", st["state"] == "done" and st["enabled"] is False and st["running"] is False)
    check("the switch says off, written before the stop",
          read("/etc/default/gatewayd").endswith("GATEWAYD_ENABLE=0\n") and read("/S70.log") == "stop\n")
    check("nothing listens: gatewayd's socket is gone", ctl_up(1) is None)
    check("its state is kept for when it is on again",
          digest(os.path.join(gwdir, "identity.v1")) == identity_before and
          digest(os.path.join(gwdir, "paired.v1")) == paired_before)
    subprocess.run([path("/etc/init.d/S70gatewayd"), "start"], check=True, stdout=subprocess.DEVNULL)
    check("and a reboot leaves it off", not gateway_running())

    # ---- 7. not installed; and no secret anywhere --------------------------------------------
    os.unlink(path("/usr/sbin/gatewayd"))
    r = sysd.call("companion.set_enabled", {"enabled": True})
    check("an image without gatewayd: refused, said", err_code(r) == 3 and r["error"]["message"].startswith("not_installed: "))
    shutil.copy(GATEWAYD, path("/usr/sbin/gatewayd"))
    leaked = []
    for d in (lib.LOGS, CR):
        for base, _, files in os.walk(d):
            for f in files:
                p = os.path.join(base, f)
                if os.path.islink(p) or os.path.getsize(p) > 4 << 20:
                    continue
                with open(p, "rb") as fh:
                    if PW.encode() in fh.read():
                        leaked.append(p)
    check("the password is in no log and no file (shadow holds only its hash)", leaked == [])
    sysd_stop()


try:
    main()
finally:
    try:
        subprocess.run([path("/etc/init.d/S70gatewayd"), "stop"], stdout=subprocess.DEVNULL, timeout=10)
    except Exception:
        pass
    if sysd_proc is not None:
        sysd_proc.kill()
    sshd.kill()
    shutil.rmtree(lib.ROOT, ignore_errors=True)

print("companion settings integration: %d checks, %d failed" % (lib.checks, lib.failed))
sys.exit(1 if lib.failed else 0)
