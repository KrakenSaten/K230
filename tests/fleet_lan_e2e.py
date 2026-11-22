"""PocketFleet over the local network, end to end: two whole peerd processes
- the shipped binary, not a test build of it - each in a network namespace
of its own with a real interface, and two Fleet players
(tests/fleet_lan_player.c: the real session, LAN link and match) playing a
whole match through them. M2 package 6 (docs/design/games-multiplayer/
M2-PLAN.md).

    python3 tests/fleet_lan_e2e.py PEERD PLAYER ROOT SCENARIO

It must be started as the first process of a user, network, mount and
process namespace of its own (tests/fleet_lan_e2e_test.sh does: unshare
-Urnm --pid --fork --kill-child), where it is root over nothing but what it
makes, and where everything it starts ends with it. It then builds:

  flat    two namespaces joined by one veth pair, one subnet: announcements
          reach the other side, as on a home network
  routed  two namespaces on two subnets with a third between them that
          forwards: unicast gets through, broadcast does not, as on a
          network that keeps announcements from crossing

Scenarios:
  clean    flat; the host finds the guest by its announcement and invites it;
           after the match both keep the opponent, the host's app is closed
           and its peerd lets go at once, the guest's after the quiet half
           minute
  lossy    flat, with 15 % of packets lost, 5 % duplicated and 20 ms of delay
           each way (netem)
  crash    the guest's app dies at ply 30 (or the first ply after it that
           leaves the host to shoot) and is started again on its save
  restart  the guest's peerd is killed at ply 40 and started again: a new
           run, an empty inbox, its kept peer read back from disk
  address  the guest's address changes at ply 30; the match goes on (the
           host's peerd follows the guest's next packet, or asks: whichever
           comes first)
  lookup   the guest's app is stopped, then its address changed: nothing it
           sends can say where it went, and the host's peerd has to ask
           ("who holds this key") and be answered before the app is let go on
  outage   the guest's interface is down for eight seconds at ply 30: an
           outage shorter than the match's retry budget (six tries, 12.5 s
           at the least), so nothing is given up; the match pauses and goes on
  blocked  routed: after five seconds visible neither peerd has heard the
           other; the host listens seven, then asks the guest's address
  chat     "clean", with both players talking all through the battle
  forfeit  5 % loss, and the host forfeits at ply 20: the match ends with an
           END that is answered across a network that loses packets
  lastpacket  400 ms of delay each way; the instant the first player is done,
           its interface goes down for half a second, which throws away the
           packet it has just sent - the one the other needs to be done too.
           The other must still finish, by asking again and being answered.
           When the two reveals cross, both are done at once and there is no
           such packet; the scenario then says so and exits 5, and the
           wrapper plays it again (three times at most)
  endlost  400 ms of delay; the host forfeits at ply 20 and its interface is
           down for eight seconds from that moment, so the END and all its
           repeats are lost and given up. The guest must still learn of the
           forfeit, from a host that goes on answering
  closemid the guest's app is closed (SIGTERM: the player closes its link as
           the app does) at ply 30 and opened again on its save

Each must end with one win and one loss, both fleets verified, the same
record on both sides and no violation ("forfeit": the forfeiter lost, by its
own END, which was answered, and no violation; "endlost": by its END, given
up unanswered and learnt all the same). From Deploy on the players are hidden
(as the app is outside its lobby), so every match here is played with the
kept opponent alone; that is checked when the match begins and then sampled
to its end, through every fault, along with peerd keeping exactly the
opponent during the match and nobody after it.

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
T0 = time.monotonic()
NOT_STAGED = [False]
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
                 forfeit_at=0, chat=False):
        self.lines = []
        self.done_t = None
        self.lock = threading.Lock()
        args = [PLAYER, "--role", role, "--save", save, "--seed", str(seed), "--timeout", "420"]
        if peer:
            args += ["--peer", peer]
        if address:
            args += ["--address", address]
        if chat:
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
                if line.startswith("DONE ") and self.done_t is None:
                    self.done_t = time.monotonic() - T0
                self.lines.append(line)

    def find(self, prefix):
        with self.lock:
            for line in self.lines:
                if line.startswith(prefix):
                    return line
        return None

    def count(self, prefix):
        with self.lock:
            return len([l for l in self.lines if l.startswith(prefix)])

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
        except (IOError, OSError, KeyError, ValueError):
            pass
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------------------
# The scenario
# ---------------------------------------------------------------------------

def netem_delay(ms):
    for ns, dev in (("a", "va"), ("b", "vb")):
        sh("ip", "netns", "exec", ns, "tc", "qdisc", "add", "dev", dev, "root", "netem",
           "delay", ms)


def netem(loss):
    for ns, dev in (("a", "va"), ("b", "vb")):
        sh("ip", "netns", "exec", ns, "tc", "qdisc", "add", "dev", dev, "root", "netem",
           "loss", loss, "duplicate", "5%", "delay", "20ms")


def netem_dropped():
    """Packets netem has dropped so far, on both devices together."""
    total = 0
    for ns, dev in (("a", "va"), ("b", "vb")):
        out = sh("ip", "netns", "exec", ns, "tc", "-s", "qdisc", "show", "dev", dev)
        for part in out.replace(",", " ").split("dropped")[1:]:
            total += int(part.split()[0])
    return total


def move_guest():
    sh("ip", "-n", "b", "addr", "del", ADDR_B_FLAT + "/24", "dev", "vb")
    sh("ip", "-n", "b", "addr", "add", ADDR_B_MOVED + "/24", "brd", "+", "dev", "vb")
    return wait_for(lambda: reachable("a", ADDR_B_MOVED), 10) and not reachable("a", ADDR_B_FLAT)


def address_of(peerd, name):
    return dict((n["name"], n.get("address")) for n in peerd.nodes()).get(name)


def main():
    global T0
    routed = SCENARIO == "blocked"
    addr_b = build_network(routed)
    ok("the two devices reach each other by address", wait_for(lambda: reachable("a", addr_b), 10))
    if SCENARIO == "lossy":
        netem("15%")
    elif SCENARIO == "forfeit":
        # Less loss than "lossy": the END is tried three times and then given
        # up, and this scenario is about its being answered.
        netem("5%")
    elif SCENARIO in ("lastpacket", "endlost"):
        # Long enough that a packet just sent is still in the interface's
        # queue when the interface is taken down, and is thrown away with it.
        netem_delay("400ms")
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
        guest = Player(b, "guest", save_b, 22, crash_at=30 if SCENARIO == "crash" else 0,
                       chat=SCENARIO == "chat")
        host = Player(a, "host", save_a, 11, peer=None if routed else "BRAVO",
                      address=addr_b if routed else None,
                      forfeit_at=20 if SCENARIO in ("forfeit", "endlost") else 0,
                      # "lookup": the host talks, so its peerd mostly has
                      # something to send to an opponent gone silent. Not
                      # always: a line waits while a shot of the host's is
                      # unanswered, and then it is that shot's own repeats
                      # that make peerd ask.
                      chat=SCENARIO in ("chat", "lookup"))
        players = [host, guest]
        started = T0 = time.monotonic()
        crashed = restarted = moved = down = silent_checked = cut = closed = False
        seen_hidden = False
        seen_visible_again = []
        fault_at_ply = 30
        limit = 400
        cutter = {}

        def cut_last_packet():
            # The first player to be done has, in the same instant, sent the
            # packet the other needs to be done too. It is still waiting out
            # its 400 ms in the interface's queue: taking the interface down
            # now throws it away. Watched every 10 ms, well inside that.
            while not cutter.get("stop"):
                h, g = host.done(), guest.done()
                if h and g:
                    cutter["late"] = True       # both at once: nothing to cut
                    return
                if h or g:
                    ns, dev = ("a", "va") if h else ("b", "vb")
                    at = time.monotonic() - started
                    sh("ip", "-n", ns, "link", "set", dev, "down")
                    time.sleep(0.5)
                    sh("ip", "-n", ns, "link", "set", dev, "up")
                    cutter.update(first="host" if h else "guest", at=at)
                    return
                time.sleep(0.01)

        if SCENARIO == "lastpacket":
            threading.Thread(target=cut_last_packet, daemon=True).start()
        while time.monotonic() - started < limit:
            if host.done() and guest.done():
                break
            now = time.monotonic() - started

            if routed and not silent_checked and now >= 5:
                # Both lobbies have been open and announcing for five seconds,
                # and the host has not yet asked anything (it waits seven).
                silent_checked = True
                va, vb = a.status(), b.status()
                ok("both devices are visible and announcing", va["visible"] and vb["visible"] and
                   va["socket"] and vb["socket"], (va["visible"], vb["visible"]))
                # Announcing, not merely meaning to: each has an interface to
                # announce on and has sent (a beacon every two seconds), so
                # hearing nothing below is the router's doing.
                ok("each has an interface to announce on and has sent on it",
                   len(va.get("interfaces", [])) >= 1 and len(vb.get("interfaces", [])) >= 1 and
                   va["counters"]["tx"] >= 2 and vb["counters"]["tx"] >= 2,
                   (va.get("interfaces"), vb.get("interfaces"), va["counters"]["tx"],
                    vb["counters"]["tx"]))
                ok("and neither peerd has heard the other: no announcement crossed the router",
                   a.nodes() == [] and b.nodes() == [], (a.nodes(), b.nodes()))
                ok("nothing has been asked yet", host.find("ASKED") is None)

            if not seen_hidden and host.find("HIDDEN") and guest.find("HIDDEN") and \
                    not crashed and not restarted and not moved and not down:
                # The match has begun and no fault has been injected: each
                # link ends its lease once its opponent is kept.
                hidden = wait_for(lambda: not a.status()["visible"] and
                                  not b.status()["visible"], 12)
                seen_hidden = True
                ha, hb = a.status(), b.status()
                ok("in the battle neither device is visible",
                   hidden, (ha["visible"], hb["visible"]))
                ok("each peerd is bound for its kept opponent and keeps exactly one",
                   ha["socket"] and hb["socket"] and a.kept() == 1 and b.kept() == 1,
                   (ha["socket"], hb["socket"], a.kept(), b.kept()))
                ok("both players stopped asking to be seen when the match began",
                   host.find("HIDDEN") is not None and guest.find("HIDDEN") is not None)
            elif seen_hidden:
                # From then to the end, through every fault: never visible again.
                for tag, p in (("A", a), ("B", b)):
                    try:
                        if p.status().get("visible"):
                            seen_visible_again.append((tag, round(now, 1)))
                    except (IOError, OSError, ValueError):
                        pass        # a peerd that is being restarted

            if SCENARIO == "crash" and not crashed and guest.proc.poll() is not None:
                crashed = True
                guest.t.join(3)     # its last lines, before they are counted
                ok("the guest's app died mid-match, as told",
                   guest.proc.returncode == 3 and guest.ply() >= 30, guest.proc.returncode)
                ok("its save is on disk", os.path.getsize(save_b) > 0)
                ok("its peerd still keeps the opponent, with no client attached",
                   b.kept() == 1 and b.status()["socket"])
                # Newer than anything the app could have read before it died:
                # only what the host sends after the crash counts.
                held = call(b.sock, "peer.app_inbox", {"port": 1, "after_id": 0})["datagrams"]
                last = max([d["id"] for d in held] or [0])
                waiting = wait_for(lambda: call(b.sock, "peer.app_inbox",
                                                {"port": 1, "after_id": last})["count"] > 0, 12)
                ok("and what the host sends after the crash waits in its inbox", waiting, last)
                guest = Player(b, "guest", save_b, 22)
                players.append(guest)

            if SCENARIO == "restart" and not restarted and guest.ply() >= 40:
                restarted = True
                b.kill()
                time.sleep(2)
                ok("the guest's peerd comes back", b.start())
                ok("as a new run", b.status()["run_id"] != run_b)
                ok("with its kept opponent in its list", b.kept() == 1, b.kept())
                # Its own word for where that came from: what it read at
                # start, before any client could have asserted anything.
                log = open(os.path.join(ROOT, "b-peerd.log"), errors="replace").read()
                starts = [l for l in log.splitlines() if "ready: epoch" in l]
                ok("read back from disk at start, by peerd's own account",
                   len(starts) == 2 and "0 kept peer(s)" in starts[0] and
                   "1 kept peer(s)" in starts[1], starts)

            if SCENARIO == "address" and not moved and guest.ply() >= fault_at_ply:
                moved = True
                ok("the guest has a new address, and the old one is gone", move_guest())

            if SCENARIO == "lookup" and not moved and guest.ply() >= fault_at_ply:
                # The guest's app stops answering (its peerd does not), and
                # its address changes while it is stopped: nothing the guest
                # sends can tell the host where it went. Only the host's
                # peerd asking can.
                moved = True
                signed_before = a.status()["counters"]["accepted_signed"]
                os.kill(guest.proc.pid, signal.SIGSTOP)
                time.sleep(0.3)     # a line already in the pipe is read first
                ply_stopped = guest.ply()
                ok("the guest has a new address, and the old one is gone", move_guest())
                found = wait_for(lambda: address_of(a, "BRAVO") == ADDR_B_MOVED, 40)
                ok("the host's peerd found the guest at its new address",
                   found, address_of(a, "BRAVO"))
                ok("while the guest's app was stopped: it sent nothing that could have told it",
                   guest.ply() == ply_stopped and guest.proc.poll() is None)
                ok("by asking - a signed answer came back from a device that announces nothing",
                   a.status()["counters"]["accepted_signed"] > signed_before,
                   (signed_before, a.status()["counters"]["accepted_signed"]))
                os.kill(guest.proc.pid, signal.SIGCONT)

            if SCENARIO == "outage" and not down and guest.ply() >= fault_at_ply:
                down = True
                sh("ip", "-n", "b", "link", "set", "vb", "down")
                ok("the network is gone", not reachable("a", ADDR_B_FLAT))
                ply_then = host.ply()
                time.sleep(8)
                ok("and the match stands still without it", host.ply() - ply_then <= 1,
                   (ply_then, host.ply()))
                sh("ip", "-n", "b", "link", "set", "vb", "up")
                ok("the network is back", wait_for(lambda: reachable("a", ADDR_B_FLAT), 10))

            if SCENARIO == "endlost" and not cut and host.find("FORFEITED"):
                # The END has just gone out; the host's interface goes down
                # for longer than it is tried, so it is given up unanswered.
                cut = True
                sh("ip", "-n", "a", "link", "set", "va", "down")
                time.sleep(8)
                sh("ip", "-n", "a", "link", "set", "va", "up")

            if SCENARIO == "closemid" and not closed and guest.ply() >= fault_at_ply:
                # The app closed by its player, not killed: the player closes
                # its link the way the app does when it is closed.
                closed = True
                guest.proc.terminate()
                rc = guest.proc.wait(10)
                guest.t.join(3)
                ok("the guest's app was closed mid-match, and closed its link", rc == 4, rc)
                time.sleep(1)
                ok("its peerd still keeps the opponent of the unfinished match, and stays bound",
                   b.kept() == 1 and b.status()["socket"], (b.kept(), b.status()["socket"]))
                guest = Player(b, "guest", save_b, 22)
                players.append(guest)
            time.sleep(0.2)
        elapsed = time.monotonic() - started
        cutter["stop"] = True
        for p in (host, guest):
            p.t.join(0.5)
        done_at = {}
        if SCENARIO == "lastpacket":
            # When each was done, from the player's own account of it.
            for tag, p in (("host", host), ("guest", guest)):
                line = p.find("DONE ")
                if line is not None:
                    with p.lock:
                        done_at[tag] = p.done_t
        dh, dg = host.done(), guest.done()
        ok("the host finished the match", dh is not None,
           host.find("TIMEOUT") or "ply %d" % host.ply())
        ok("the guest finished the match", dg is not None,
           guest.find("TIMEOUT") or "ply %d" % guest.ply())
        ok("the mid-match checks ran", seen_hidden)
        ok("and from then to the end neither device was visible again",
           not seen_visible_again, seen_visible_again[:6])

        listed = host.find("LISTED")
        if routed:
            ok("after seven seconds of listening the host had nobody listed",
               listed is not None and listed.startswith("LISTED 0 "), listed)
            ok("the host asked the guest's address, and it answered",
               host.find("ASKED " + addr_b) is not None and host.find("ANSWERED") is not None)
        elif SCENARIO in ("lossy", "forfeit"):
            # A beacon every two seconds, and some of them lost: found later.
            ms = int(listed.split("after_ms=")[1]) if listed else -1
            ok("the host found the guest by its announcement, through the loss",
               0 <= ms < 12000, listed)
        else:
            ms = int(listed.split("after_ms=")[1]) if listed else -1
            ok("the host found the guest by its announcement, well inside the time the "
               "blocked network is given", 0 <= ms < 6000, listed)
        if SCENARIO not in ("lossy", "forfeit"):
            ok("one invitation was enough", host.count("INVITED") == 1, host.count("INVITED"))
        if SCENARIO == "crash":
            ok("the guest's app was killed mid-match", crashed)
            ok("the restarted guest resumed its match rather than starting another",
               guest.find("RESUMED") is not None and guest.find("ACCEPTED") is None)
        if SCENARIO in ("address", "lookup"):
            ok("the address was changed mid-match", moved)
            ok("and the host's peerd holds the guest at the new one",
               address_of(a, "BRAVO") == ADDR_B_MOVED, address_of(a, "BRAVO"))
        if SCENARIO == "restart":
            ok("the peerd was restarted mid-match", restarted)
        if SCENARIO == "outage":
            ok("the network was taken away mid-match", down)
        if SCENARIO == "closemid":
            ok("the app was closed mid-match", closed)
            ok("opened again, it resumed its match rather than starting another",
               guest.find("RESUMED") is not None and guest.find("ACCEPTED") is None)
        if SCENARIO == "lastpacket":
            ok("the first player's last packet was thrown away", "first" in cutter, cutter)
            if "first" in cutter and len(done_at) == 2:
                gap = max(done_at.values()) - cutter["at"]
                if gap < 0.4 and not fails:
                    # Both reveals were on their way before either side was
                    # done: the second was done with what it already had,
                    # and nothing it needed was left in the queue to lose.
                    print("     [lastpacket] not staged: the reveals crossed (the other was done "
                          "%.2f s later); to be played again" % gap, flush=True)
                    NOT_STAGED[0] = True
                else:
                    ok("the other finished all the same, later, by asking again and being answered",
                       gap > 1.0, (cutter, done_at))
                    print("     [lastpacket] %s was done first, at %.1f s; the other %.1f s later"
                          % (cutter["first"], cutter["at"], gap), flush=True)
        if SCENARIO in ("lossy", "forfeit"):
            dropped = netem_dropped()
            ok("packets really were lost on the way", dropped > 0, dropped)
        if SCENARIO == "endlost":
            ok("the host forfeited and its END was cut off", cut)
            if dh and dg:
                ok("the forfeiter lost and the other won",
                   dh["outcome"] == "loss" and dg["outcome"] == "win",
                   (dh["outcome"], dg["outcome"]))
                ok("the host gave its END up unanswered",
                   dh["end"] == "1" and dh["by_me"] == "1" and dh["end_acked"] == "0",
                   (dh["end"], dh["by_me"], dh["end_acked"]))
                ok("the guest learnt of the forfeit all the same, from a host still answering",
                   dg["end"] == "1" and dg["by_me"] == "0", (dg["end"], dg["by_me"]))
                ok("and neither saw a violation", dh["violations"] == "0" and dg["violations"] == "0")
        elif dh and dg and SCENARIO == "forfeit":
            ok("the host forfeited, as told", host.find("FORFEITED") is not None)
            ok("the forfeiter lost and the other won",
               dh["outcome"] == "loss" and dg["outcome"] == "win", (dh["outcome"], dg["outcome"]))
            ok("by a forfeit, the host's own, on both records",
               dh["end"] == "1" and dg["end"] == "1" and dh["by_me"] == "1" and dg["by_me"] == "0",
               (dh["end"], dg["end"], dh["by_me"], dg["by_me"]))
            ok("the END was answered, not given up", dh["end_acked"] == "1", dh["end_acked"])
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
        if dh and dg and SCENARIO == "clean":
            # Over on both sides. Each still keeps the other for as long as
            # the other may need an answer; closing the app lets go at once.
            time.sleep(3)
            ok("three seconds after the match both still keep the opponent",
               a.kept() == 1 and b.kept() == 1, (a.kept(), b.kept()))
            t0 = time.monotonic()
            host.stop()
            ok("the host's app closed: its peerd lets the opponent go at once",
               wait_for(lambda: a.kept() == 0, 3), a.kept())
            ok("and closes its socket", wait_for(lambda: not a.status()["socket"], 5))
            ok("the guest's app is still open, and still keeps the host", b.kept() == 1, b.kept())
            ok("until the host has been quiet half a minute; then it lets go by itself",
               wait_for(lambda: b.kept() == 0, 45) and time.monotonic() - t0 > 15,
               (b.kept(), round(time.monotonic() - t0, 1)))
        if dh and dg:
            # Thirty quiet seconds after the match, the opponent is let go.
            ok("the match over, each peerd lets its opponent go",
               wait_for(lambda: a.kept() == 0 and b.kept() == 0, 60), (a.kept(), b.kept()))
            ok("and, hidden and keeping nobody, closes its socket",
               wait_for(lambda: not a.status()["socket"] and not b.status()["socket"], 15),
               (a.status()["socket"], b.status()["socket"]))
            ca, cb = a.status()["counters"], b.status()["counters"]
            print("     [%s] %s plies in %.0f s; packets host %s, guest %s; resyncs %s/%s; "
                  "duplicate replies %s/%s; peerd datagrams accepted %s/%s, kept-record "
                  "writes %s/%s"
                  % (SCENARIO, dh["plies"], elapsed, dh["sent"], dg["sent"], dh["resyncs"],
                     dg["resyncs"], dh["dup_replies"], dg["dup_replies"], ca["accepted_data"],
                     cb["accepted_data"], ca["kept_writes"], cb["kept_writes"]), flush=True)
    finally:
        for p in players:
            p.stop()
        a.stop()
        b.stop()
    if NOT_STAGED[0] and not fails:
        print("fleet_lan_e2e [%s]: not staged" % SCENARIO, flush=True)
        return 5
    print("fleet_lan_e2e [%s]: %d failure(s)" % (SCENARIO, fails), flush=True)
    return 1 if fails else 0


def on_term(signum, frame):
    # Killed with the test: leave by the door, so players and peerds are
    # stopped (the process namespace would take them anyway).
    raise SystemExit(130)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, on_term)
    sys.exit(main())
