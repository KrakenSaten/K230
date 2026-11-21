"""PocketFleet over the local network, end to end: two whole peerd processes
- the shipped binary, not a test build of it - each in a network namespace
of its own with a real interface, and two Fleet players
(tests/fleet_lan_player.c: the real session, LAN link and match) playing a
whole match through them. M2 package 6 (docs/design/games-multiplayer/
M2-PLAN.md).

    python3 tests/fleet_lan_e2e.py PEERD PLAYER ROOT SCENARIO

It must be started inside a user, network and mount namespace of its own
(tests/fleet_lan_e2e_test.sh does: unshare -Urnm), where it is root over
nothing but what it makes. It then builds:

  flat    two namespaces joined by one veth pair, one subnet: announcements
          reach the other side, as on a home network
  routed  two namespaces on two subnets with a third between them that
          forwards: unicast gets through, broadcast does not, as on a
          network that keeps announcements from crossing

Scenarios:
  clean    flat; the host finds the guest by its announcement and invites it
  lossy    flat, with 15 % of packets lost, 5 % duplicated and 20 ms of delay
           each way (netem)
  crash    the guest's app dies at ply 30 and is started again on its save
  restart  the guest's peerd is killed at ply 40 and started again: a new
           run, an empty inbox, its kept peer read back from disk
  address  the guest's address changes at ply 30
  blocked  routed: nobody is listed, the host asks the guest's address
  chat     "clean", with both players talking all through the battle
  forfeit  "lossy", and the host forfeits at ply 20: the match ends with an
           END that has to be acknowledged across a network that loses packets

Each must end with one win and one loss, both fleets verified, the same
record on both sides and no violation ("forfeit": the forfeiter lost, by its
own END, acknowledged, and no violation). From Deploy on the players are hidden
(as the app is outside its lobby), so every match here is played with the
kept opponent alone; that is checked too, along with peerd keeping exactly
the opponent during the match and nobody after it.

No hardware, and nothing here leaves the namespaces it makes.

Copyright (c) 2026 Doors authors.
SPDX-License-Identifier: Apache-2.0
"""
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

PEERD, PLAYER, ROOT, SCENARIO = sys.argv[1:5]
PEERD = os.path.abspath(PEERD)
PLAYER = os.path.abspath(PLAYER)
fails = 0


def ok(label, cond, detail=""):
    global fails
    tag = "[%s] " % SCENARIO
    if cond:
        print("ok   " + tag + label, flush=True)
    else:
        fails += 1
        print("FAIL " + tag + label + ((": " + str(detail)) if detail else ""), flush=True)


def sh(*args):
    r = subprocess.run(list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    if r.returncode != 0:
        raise RuntimeError("%s: %s" % (" ".join(args), r.stdout.strip()))
    return r.stdout


def frame(obj):
    raw = json.dumps(obj).encode()
    return struct.pack(">I", len(raw)) + raw


def call(sock, method, params=None, timeout=5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sock)
        body = {"id": 1, "method": method}
        if params is not None:
            body["params"] = params
        s.sendall(frame(body))
        buf = b""
        while len(buf) < 4 or len(buf) < 4 + struct.unpack(">I", buf[:4])[0]:
            chunk = s.recv(65536)
            if not chunk:
                raise IOError("closed")
            buf += chunk
        msg = json.loads(buf[4:4 + struct.unpack(">I", buf[:4])[0]])
        if "result" not in msg:
            raise IOError(method + ": " + json.dumps(msg.get("error")))
        return msg["result"]
    finally:
        s.close()


# ---------------------------------------------------------------------------
# The network
# ---------------------------------------------------------------------------

ADDR_A = "10.99.1.1"
ADDR_B_FLAT = "10.99.1.2"
ADDR_B_MOVED = "10.99.1.7"
ADDR_B_ROUTED = "10.99.2.2"


def build_network(routed):
    """Namespaces a and b (and r between them when routed). Returns b's address."""
    sh("mount", "-t", "tmpfs", "none", "/run")
    os.makedirs("/run/netns", exist_ok=True)
    for ns in ("a", "b") + (("r",) if routed else ()):
        sh("ip", "netns", "add", ns)
        sh("ip", "-n", ns, "link", "set", "lo", "up")
    if not routed:
        sh("ip", "link", "add", "va", "type", "veth", "peer", "name", "vb")
        sh("ip", "link", "set", "va", "netns", "a")
        sh("ip", "link", "set", "vb", "netns", "b")
        sh("ip", "-n", "a", "addr", "add", ADDR_A + "/24", "brd", "+", "dev", "va")
        sh("ip", "-n", "b", "addr", "add", ADDR_B_FLAT + "/24", "brd", "+", "dev", "vb")
        sh("ip", "-n", "a", "link", "set", "va", "up")
        sh("ip", "-n", "b", "link", "set", "vb", "up")
        return ADDR_B_FLAT
    sh("ip", "link", "add", "va", "type", "veth", "peer", "name", "ra")
    sh("ip", "link", "add", "vb", "type", "veth", "peer", "name", "rb")
    sh("ip", "link", "set", "va", "netns", "a")
    sh("ip", "link", "set", "vb", "netns", "b")
    sh("ip", "link", "set", "ra", "netns", "r")
    sh("ip", "link", "set", "rb", "netns", "r")
    sh("ip", "-n", "a", "addr", "add", ADDR_A + "/24", "brd", "+", "dev", "va")
    sh("ip", "-n", "b", "addr", "add", ADDR_B_ROUTED + "/24", "brd", "+", "dev", "vb")
    sh("ip", "-n", "r", "addr", "add", "10.99.1.254/24", "brd", "+", "dev", "ra")
    sh("ip", "-n", "r", "addr", "add", "10.99.2.254/24", "brd", "+", "dev", "rb")
    for ns, dev in (("a", "va"), ("b", "vb"), ("r", "ra"), ("r", "rb")):
        sh("ip", "-n", ns, "link", "set", dev, "up")
    sh("ip", "-n", "a", "route", "add", "default", "via", "10.99.1.254")
    sh("ip", "-n", "b", "route", "add", "default", "via", "10.99.2.254")
    sh("ip", "netns", "exec", "r", "sysctl", "-q", "-w", "net.ipv4.ip_forward=1")
    return ADDR_B_ROUTED


def reachable(ns, addr):
    r = subprocess.run(["ip", "netns", "exec", ns, "ping", "-c1", "-W1", addr],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode == 0


# ---------------------------------------------------------------------------
# peerd, the shipped one, in its namespace
# ---------------------------------------------------------------------------

class Peerd(object):
    def __init__(self, tag, ns, name):
        self.tag, self.ns, self.name = tag, ns, name
        self.dirs = dict((k, os.path.join(ROOT, tag, k)) for k in ("run", "state", "log", "config"))
        for d in self.dirs.values():
            os.makedirs(d, exist_ok=True)
        self.sock = os.path.join(self.dirs["run"], "peerd.sock")
        self.proc = None

    def env(self):
        e = dict(os.environ)
        e.update(POCKETOS_RUNTIME_DIR=self.dirs["run"], POCKETOS_STATE_DIR=self.dirs["state"],
                 POCKETOS_LOG_DIR=self.dirs["log"], POCKETOS_CONFIG_DIR=self.dirs["config"],
                 POCKETOS_LOG_STDERR="1")
        return e

    def start(self):
        out = open(os.path.join(ROOT, self.tag + "-peerd.log"), "ab")
        self.proc = subprocess.Popen(["ip", "netns", "exec", self.ns, PEERD, "--name", self.name],
                                     stdout=out, stderr=out, env=self.env())
        end = time.monotonic() + 15
        while time.monotonic() < end:
            try:
                if self.status()["state"] == "ready":
                    return True
            except (IOError, OSError):
                pass
            time.sleep(0.1)
        return False

    def status(self):
        return call(self.sock, "peer.status")

    def kept(self):
        return call(self.sock, "peer.kept")["count"]

    def nodes(self):
        return call(self.sock, "peer.nodes")["nodes"]

    def kill(self):
        """As a crash: no chance to put anything away."""
        if self.proc and self.proc.poll() is None:
            # The process we hold is "ip netns exec", which has become peerd.
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        try:
            os.unlink(self.sock)
        except OSError:
            pass

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# ---------------------------------------------------------------------------
# A player
# ---------------------------------------------------------------------------

class Player(object):
    def __init__(self, peerd, role, save, seed, peer=None, address=None, crash_at=0,
                 forfeit_at=0):
        self.lines = []
        self.lock = threading.Lock()
        args = [PLAYER, "--role", role, "--save", save, "--seed", str(seed), "--timeout", "420"]
        if peer:
            args += ["--peer", peer]
        if address:
            args += ["--address", address]
        if SCENARIO == "chat":
            args.append("--chat")
        if crash_at:
            args += ["--crash-at-ply", str(crash_at)]
        if forfeit_at:
            args += ["--forfeit-at-ply", str(forfeit_at)]
        env = dict(os.environ)
        env["POCKETOS_RUNTIME_DIR"] = peerd.dirs["run"]
        self.log = open(os.path.join(ROOT, "%s-%s.log" % (role, int(time.time() * 1000))), "w")
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=self.log, env=env,
                                     universal_newlines=True)
        self.t = threading.Thread(target=self.read)
        self.t.daemon = True
        self.t.start()

    def read(self):
        for line in self.proc.stdout:
            line = line.strip()
            self.log.write(line + "\n")
            self.log.flush()
            with self.lock:
                self.lines.append(line)

    def find(self, prefix):
        with self.lock:
            for line in self.lines:
                if line.startswith(prefix):
                    return line
        return None

    def ply(self):
        with self.lock:
            plies = [int(l.split()[1]) for l in self.lines if l.startswith("PLY ")]
        return max(plies) if plies else 0

    def done(self):
        line = self.find("DONE ")
        if not line:
            return None
        return dict(kv.split("=", 1) for kv in line.split()[1:])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def wait_for(cond, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if cond():
                return True
        except (IOError, OSError, KeyError):
            pass
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------
# The scenario
# ---------------------------------------------------------------------------

def main():
    routed = SCENARIO == "blocked"
    addr_b = build_network(routed)
    ok("the two devices reach each other by address", wait_for(lambda: reachable("a", addr_b), 10))
    if SCENARIO in ("lossy", "forfeit"):
        for ns, dev in (("a", "va"), ("b", "vb")):
            sh("ip", "netns", "exec", ns, "tc", "qdisc", "add", "dev", dev, "root", "netem",
               "loss", "15%", "duplicate", "5%", "delay", "20ms")
    a = Peerd("a", "a", "ALPHA")
    b = Peerd("b", "b", "BRAVO")
    players = []
    try:
        ok("peerd A is ready", a.start())
        ok("peerd B is ready", b.start())
        sa, sb = a.status(), b.status()
        ok("neither has opened a socket or is visible before anybody asked",
           not sa["socket"] and not sb["socket"] and not sa["visible"] and not sb["visible"],
           (sa["socket"], sb["socket"], sa["visible"], sb["visible"]))
        run_b = sb["run_id"]

        save_a = os.path.join(ROOT, "match-lan-a.v1")
        save_b = os.path.join(ROOT, "match-lan-b.v1")
        guest = Player(b, "guest", save_b, 22, crash_at=30 if SCENARIO == "crash" else 0)
        host = Player(a, "host", save_a, 11, peer=None if routed else "BRAVO",
                      address=addr_b if routed else None,
                      forfeit_at=20 if SCENARIO == "forfeit" else 0)
        players = [host, guest]
        started = time.monotonic()
        crashed = restarted = moved = seen_hidden = False
        limit = 400
        while time.monotonic() - started < limit:
            if host.done() and guest.done():
                break
            if not seen_hidden and host.ply() >= 6 and guest.ply() >= 6 and \
                    not (SCENARIO == "crash" and crashed) and not restarted:
                # Mid-battle, before any fault: hidden, bound, and keeping
                # exactly the opponent.
                seen_hidden = True
                t0 = time.monotonic()
                # The link ends the lease once the opponent is kept and peerd's
                # list has been read back right: a few seconds into the match.
                hidden = wait_for(lambda: not a.status()["visible"] and
                                  not b.status()["visible"], 12)
                ha, hb = a.status(), b.status()
                ok("in the battle neither device is visible",
                   hidden, (ha["visible"], hb["visible"]))
                print("     [%s] hidden %.1f s after ply 6 (host at ply %d)"
                      % (SCENARIO, time.monotonic() - t0, host.ply()), flush=True)
                ok("each peerd is bound for its kept opponent and keeps exactly one",
                   ha["socket"] and hb["socket"] and a.kept() == 1 and b.kept() == 1,
                   (ha["socket"], hb["socket"], a.kept(), b.kept()))
                ok("both players stopped asking to be seen when the match began",
                   host.find("HIDDEN") is not None and guest.find("HIDDEN") is not None)
            if SCENARIO == "crash" and not crashed and guest.proc.poll() is not None:
                crashed = True
                ok("the guest's app died mid-match, as told",
                   guest.proc.returncode == 3 and guest.ply() >= 30, guest.proc.returncode)
                ok("its save is on disk", os.path.getsize(save_b) > 0)
                ok("its peerd still keeps the opponent, with no client attached",
                   b.kept() == 1 and b.status()["socket"])
                time.sleep(2)       # the host's shots meanwhile wait in peerd's inbox
                guest = Player(b, "guest", save_b, 22)
                players.append(guest)
            if SCENARIO == "restart" and not restarted and guest.ply() >= 40:
                restarted = True
                b.kill()
                time.sleep(2)
                ok("the guest's peerd comes back", b.start())
                ok("as a new run", b.status()["run_id"] != run_b)
                ok("with its kept opponent read back from disk", b.kept() == 1, b.kept())
            if SCENARIO == "address" and not moved and guest.ply() >= 30:
                moved = True
                sh("ip", "-n", "b", "addr", "del", ADDR_B_FLAT + "/24", "dev", "vb")
                sh("ip", "-n", "b", "addr", "add", ADDR_B_MOVED + "/24", "brd", "+", "dev", "vb")
                ok("the guest has a new address", wait_for(lambda: reachable("a", ADDR_B_MOVED), 10)
                   and not reachable("a", ADDR_B_FLAT))
            time.sleep(0.2)
        elapsed = time.monotonic() - started
        dh, dg = host.done(), guest.done()
        ok("the host finished the match", dh is not None,
           host.find("TIMEOUT") or "ply %d" % host.ply())
        ok("the guest finished the match", dg is not None,
           guest.find("TIMEOUT") or "ply %d" % guest.ply())

        if routed:
            ok("no announcement crossed: nobody was listed",
               host.find("LISTED 0") is not None, host.find("LISTED"))
            ok("the host asked the guest's address, and it answered",
               host.find("ASKED " + addr_b) is not None and host.find("ANSWERED") is not None)
        else:
            ok("the host found the guest by its announcement, asking no address",
               host.find("LISTED") is not None and host.find("ASKED") is None,
               host.find("LISTED"))
        if SCENARIO == "crash":
            ok("the restarted guest resumed its match rather than starting another",
               guest.find("RESUMED") is not None and guest.find("ACCEPTED") is None)
        if SCENARIO == "address":
            ok("the address was changed mid-match", moved)
            nodes = dict((n["name"], n.get("address")) for n in a.nodes())
            ok("and the host's peerd followed the guest to it",
               nodes.get("BRAVO") == ADDR_B_MOVED, nodes)
        if SCENARIO == "restart":
            ok("the peerd was restarted mid-match", restarted)
        if dh and dg and SCENARIO == "forfeit":
            ok("the host forfeited, as told", host.find("FORFEITED") is not None)
            ok("the forfeiter lost and the other won",
               dh["outcome"] == "loss" and dg["outcome"] == "win", (dh["outcome"], dg["outcome"]))
            ok("by a forfeit, the host's own, on both records",
               dh["end"] == "1" and dg["end"] == "1" and dh["by_me"] == "1" and dg["by_me"] == "0",
               (dh["end"], dg["end"], dh["by_me"], dg["by_me"]))
            # The guest's record of it is the proof that the END crossed the
            # loss; the host says DONE only once it no longer owes one.
            ok("the END reached the guest, and the host no longer owes it",
               dg["end"] == "1" and host.find("DONE ") is not None)
            ok("and neither saw a violation", dh["violations"] == "0" and dg["violations"] == "0")
        elif dh and dg:
            ok("one won and the other lost", {dh["outcome"], dg["outcome"]} == {"win", "loss"},
               (dh["outcome"], dg["outcome"]))
            ok("by sinking every ship, not by an END", dh["end"] == "0" and dg["end"] == "0",
               (dh["end"], dg["end"]))
            ok("each verified the other's fleet against its commitment",
               dh["verify"] == "1" and dg["verify"] == "1", (dh["verify"], dg["verify"]))
            ok("both hold the same record of every shot",
               dh["plies"] == dg["plies"] and dh["digest"] == dg["digest"],
               (dh["plies"], dh["digest"], dg["plies"], dg["digest"]))
            ok("and neither saw a violation", dh["violations"] == "0" and dg["violations"] == "0")
            ok("both played on the LAN profile, nothing rationed",
               dh["profile"] == "lan" and dg["profile"] == "lan" and
               dh["gov_waits"] == "0" and dg["gov_waits"] == "0",
               (dh["profile"], dg["profile"], dh["gov_waits"], dg["gov_waits"]))
            if SCENARIO == "chat":
                ok("both players were heard, many times",
                   int(dh["chat_shown"]) >= 5 and int(dg["chat_shown"]) >= 5,
                   (dh["chat_shown"], dg["chat_shown"]))
                ok("every line held is the opponent's, whole, once and in order",
                   dh["chat_ok"] == "1" and dg["chat_ok"] == "1" and
                   dh["chat_order"] == "1" and dg["chat_order"] == "1",
                   (dh["chat_ok"], dg["chat_ok"], dh["chat_order"], dg["chat_order"]))
                ok("on a clean network no line was given up",
                   dh["chat_failed"] == "0" and dg["chat_failed"] == "0",
                   (dh["chat_failed"], dg["chat_failed"]))
        if dh and dg:
            ok("the match over, each peerd lets its opponent go",
               wait_for(lambda: a.kept() == 0 and b.kept() == 0, 25), (a.kept(), b.kept()))
            ok("and, hidden and keeping nobody, closes its socket",
               wait_for(lambda: not a.status()["socket"] and not b.status()["socket"], 15),
               (a.status()["socket"], b.status()["socket"]))
            ca, cb = a.status()["counters"], b.status()["counters"]
            print("     [%s] %s plies in %.0f s; packets host %s, guest %s; resyncs %s/%s; "
                  "duplicate replies %s/%s"
                  % (SCENARIO, dh["plies"], elapsed, dh["sent"], dg["sent"], dh["resyncs"],
                     dg["resyncs"], dh["dup_replies"], dg["dup_replies"]), flush=True)
            print("     [%s] peerd A %s" % (SCENARIO, json.dumps(ca, sort_keys=True)), flush=True)
            print("     [%s] peerd B %s" % (SCENARIO, json.dumps(cb, sort_keys=True)), flush=True)
    finally:
        for p in players:
            p.stop()
        a.stop()
        b.stop()
    print("fleet_lan_e2e [%s]: %d failure(s)" % (SCENARIO, fails), flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
