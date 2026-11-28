#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""N1.2 controlled bench: real protocol exchanges in an isolated set of
network namespaces, captured by the real pos-capture helper.

Nothing leaves the host: every endpoint is a namespace on this machine,
joined by veth links to a bridge in its own namespace. No interface of the
host's own network namespace is touched.

  b<k>dev   the device under test: dev0 on the LAN (captured), dev1 a second
            link straight to the resolver (for an asymmetric return path)
  b<k>lan   the bridge
  b<k>dns   a DNS resolver (normal, slow, dropping, SERVFAIL)
  b<k>dhcp  a DHCP server (offers, or stops offering)
  b<k>p1-4  peers: ARP requests (quiet, periodic bursts, storms), DHCP
            clients with retry back-off, bulk traffic to the device

Ground truth comes from the endpoints' own logs (what the resolver did with
each question, every ARP request and DISCOVER sent), never from the capture.

Usage (as root):
  bench.py session OUTDIR INSTANCE SPEC_JSON HELPER   one comparison session
  bench.py agent-* ...                                 (internal)
"""
import json
import os
import random
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time


def wall_ms():
    return time.time_ns() // 1_000_000


def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError("%s: %s" % (cmd, r.stderr.strip()))
    return r.stdout


# ---- topology -----------------------------------------------------------------------------

def names(k):
    return dict(dev="b%ddev" % k, lan="b%dlan" % k, dns="b%ddns" % k, dhcp="b%ddhcp" % k,
                peers=["b%dp%d" % (k, i) for i in range(1, 5)])


def net(k):
    return "10.77.%d" % k


def topo_up(k):
    n = names(k)
    topo_down(k)
    for ns in [n["dev"], n["lan"], n["dns"], n["dhcp"]] + n["peers"]:
        sh("ip netns add %s" % ns)
        sh("ip -n %s link set lo up" % ns)
    sh("ip -n %s link add br0 type bridge" % n["lan"])
    sh("ip -n %s link set br0 up" % n["lan"])
    hosts = [(n["dev"], "dev0", 10), (n["dns"], "dns0", 53), (n["dhcp"], "dhcp0", 2)]
    hosts += [(p, "eth0", 21 + i) for i, p in enumerate(n["peers"])]
    for idx, (ns, ifn, host) in enumerate(hosts):
        peer = "v%d_%d" % (k, idx)
        sh("ip link add %s netns %s type veth peer name %s netns %s" % (ifn, ns, peer, n["lan"]))
        sh("ip -n %s link set %s master br0 up" % (n["lan"], peer))
        sh("ip -n %s addr add %s.%d/24 dev %s" % (ns, net(k), host, ifn))
        sh("ip -n %s link set %s up" % (ns, ifn))
        sh("ip -n %s route add default dev %s" % (ns, ifn))  # broadcasts (DHCP) need a route
    # the second link, device <-> resolver, for an asymmetric return path
    sh("ip link add dev1 netns %s type veth peer name dns1 netns %s" % (n["dev"], n["dns"]))
    sh("ip -n %s addr add 10.78.%d.10/24 dev dev1" % (n["dev"], k))
    sh("ip -n %s addr add 10.78.%d.53/24 dev dns1" % (n["dns"], k))
    sh("ip -n %s link set dev1 up" % n["dev"])
    sh("ip -n %s link set dns1 up" % n["dns"])
    # accept replies on dev1 although they are addressed to dev0's address
    sh("ip netns exec %s sysctl -qw net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.dev1.rp_filter=0 "
       "net.ipv4.conf.dev0.rp_filter=0" % n["dev"])
    return n


def topo_down(k):
    n = names(k)
    for ns in [n["dev"], n["lan"], n["dns"], n["dhcp"]] + n["peers"]:
        sh("ip netns del %s" % ns, check=False)


def mac_of(ns, ifn):
    return sh("ip netns exec %s cat /sys/class/net/%s/address" % (ns, ifn)).strip()


# ---- the agents (run inside a namespace) ---------------------------------------------------

def read_mode(path, last):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return last


def dns_name(q):
    return b"".join(bytes([len(p)]) + p for p in q.split(b".")) + b"\x00"


def agent_dns(modefile, logfile):
    """The resolver: answers A questions, as the mode file says."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 53))
    log = open(logfile, "a", buffering=1)
    mode = {}
    pending = []  # (due_ms, reply, addr)
    rnd = random.Random(53)
    while True:
        mode = read_mode(modefile, mode)
        now = wall_ms()
        due = [p for p in pending if p[0] <= now]
        pending = [p for p in pending if p[0] > now]
        for _, reply, addr in due:
            s.sendto(reply, addr)
        r, _, _ = select.select([s], [], [], 0.005)
        if not r:
            continue
        data, addr = s.recvfrom(2048)
        if len(data) < 12:
            continue
        qid = data[:2]
        qend = data.find(b"\x00", 12) + 5
        question = data[12:qend]
        lo, hi = mode.get("delay_ms", [10, 30])
        delay = rnd.uniform(lo, hi)
        if rnd.random() < mode.get("drop", 0.0):
            log.write("%d %d drop\n" % (now, struct.unpack("!H", qid)[0]))
            continue
        fail = rnd.random() < mode.get("servfail", 0.0)
        flags = 0x8182 if fail else 0x8180
        reply = qid + struct.pack("!HHHHH", flags, 1, 0 if fail else 1, 0, 0) + question
        if not fail:
            reply += b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + bytes([192, 0, 2, 7])
        pending.append((now + int(delay), reply, addr))
        log.write("%d %d %s %d\n" % (now, struct.unpack("!H", qid)[0], "servfail" if fail else "answer", int(delay)))


def agent_client(modefile, logfile, server):
    """The device's own DNS questions, at the mode's rate (Poisson)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    log = open(logfile, "a", buffering=1)
    mode = {}
    rnd = random.Random(7)
    qid = rnd.randrange(65536)
    nxt = time.time()
    while True:
        mode = read_mode(modefile, mode)
        rate = mode.get("dns_rate", 1.0)
        now = time.time()
        if now >= nxt:
            qid = (qid + 1) & 0xffff
            q = struct.pack("!HHHHHH", qid, 0x0100, 1, 0, 0, 0) + dns_name(b"h%d.bench.test" % rnd.randrange(1000))
            q += struct.pack("!HH", 1, 1)
            s.sendto(q, (server, 53))
            log.write("%d %d ask\n" % (wall_ms(), qid))
            nxt = now + rnd.expovariate(rate) if rate > 0 else now + 3600
        r, _, _ = select.select([s], [], [], 0.01)
        if r:
            data, _ = s.recvfrom(2048)
            log.write("%d %d got %d\n" % (wall_ms(), struct.unpack("!H", data[:2])[0], data[3] & 0x0f))


def agent_arp(modefile, logfile, ifn, subnet, seed):
    """ARP requests from this peer: periodic cycles with short bursts, as
    Unit A's own traffic showed (§8.1), scaled by the mode's factor."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((ifn, 0))
    mac = bytes.fromhex(open("/sys/class/net/%s/address" % ifn).read().strip().replace(":", ""))
    ip = socket.inet_aton(sh("ip -4 -o addr show %s" % ifn).split()[3].split("/")[0])
    log = open(logfile, "a", buffering=1)
    rnd = random.Random(seed)
    mode = {}
    periods = [rnd.choice([6.0, 12.0, 15.0, 31.0]) for _ in range(3)]
    nexts = [time.time() + rnd.uniform(0, p) for p in periods]
    storm_next = time.time()
    while True:
        mode = read_mode(modefile, mode)
        now = time.time()
        burst = []
        for i, p in enumerate(periods):
            if now >= nexts[i]:
                burst += [1] * rnd.choice([1, 1, 2, 3]) if mode.get("arp_bursty", 1) else [1]
                nexts[i] = now + p * rnd.uniform(0.9, 1.1)
        extra = mode.get("arp_storm", 0.0)  # extra requests a second from this peer
        if extra > 0 and now >= storm_next:
            burst.append(1)
            storm_next = now + rnd.expovariate(extra)
        for _ in burst:
            tgt = socket.inet_aton("%s.%d" % (subnet, rnd.randrange(100, 250)))
            frame = b"\xff" * 6 + mac + b"\x08\x06" + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1) + mac + ip
            frame += b"\x00" * 6 + tgt
            s.send(frame)
            log.write("%d arp\n" % wall_ms())
        time.sleep(0.01)


def dhcp_packet(op, xid, chaddr, msgtype, yiaddr=b"\x00" * 4, server=None):
    p = struct.pack("!BBBBIHH", op, 1, 6, 0, xid, 0, 0x8000) + b"\x00" * 4 + yiaddr + b"\x00" * 8
    p += chaddr + b"\x00" * 10 + b"\x00" * 192 + b"\x63\x82\x53\x63" + bytes([53, 1, msgtype])
    if server:
        p += bytes([54, 4]) + server
    return p + b"\xff"


def agent_dhcp_server(modefile, logfile, myip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", 67))
    log = open(logfile, "a", buffering=1)
    mode = {}
    n = 0
    while True:
        mode = read_mode(modefile, mode)
        r, _, _ = select.select([s], [], [], 0.05)
        if not r:
            continue
        data, _ = s.recvfrom(2048)
        if len(data) < 240 or data[242:243] != b"\x01" and b"\x35\x01\x01" not in data[236:]:
            continue
        xid = struct.unpack("!I", data[4:8])[0]
        if mode.get("dhcp_answer", 1):
            n += 1
            offer = dhcp_packet(2, xid, data[28:34], 2, socket.inet_aton(myip.rsplit(".", 1)[0] + ".%d" % (150 + n % 50)),
                                socket.inet_aton(myip))
            s.sendto(offer, ("255.255.255.255", 68))
            log.write("%d offer %08x\n" % (wall_ms(), xid))
        else:
            log.write("%d silent %08x\n" % (wall_ms(), xid))


def agent_dhcp_clients(modefile, logfile, nclients, seed):
    """Clients that DISCOVER now and then; without an OFFER they retry with
    back-off (4, 8, 8 ... s), as real clients do."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", 68))
    log = open(logfile, "a", buffering=1)
    rnd = random.Random(seed)
    cl = []
    for i in range(nclients):
        cl.append(dict(mac=bytes([2, 0x42, seed & 0xff, i, rnd.randrange(256), rnd.randrange(256)]),
                       xid=rnd.getrandbits(32), next=time.time() + rnd.uniform(0, 40), wait=None, backoff=4.0))
    while True:
        now = time.time()
        for c in cl:
            if now >= c["next"]:
                c["xid"] = (c["xid"] + 1) & 0xffffffff
                s.sendto(dhcp_packet(1, c["xid"], c["mac"], 1), ("255.255.255.255", 67))
                log.write("%d discover %08x\n" % (wall_ms(), c["xid"]))
                c["wait"] = c["xid"]
                c["next"] = now + c["backoff"] * rnd.uniform(0.9, 1.1)
                c["backoff"] = min(c["backoff"] * 2, 8.0)
        r, _, _ = select.select([s], [], [], 0.05)
        if r:
            data, _ = s.recvfrom(2048)
            xid = struct.unpack("!I", data[4:8])[0]
            for c in cl:
                if c["wait"] == xid:
                    c["wait"] = None
                    c["backoff"] = 4.0
                    c["next"] = time.time() + rnd.uniform(30, 60)  # bound for a while


def agent_bulk(modefile, logfile, target):
    """UDP to the device: bursts in 'busy', a blast in 'blast'."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    log = open(logfile, "a", buffering=1)
    mode = {}
    pay = b"\x00" * 1400
    rnd = random.Random(11)
    nxt_burst = time.time()
    while True:
        mode = read_mode(modefile, mode)
        if mode.get("blast", 0):
            for _ in range(200):
                s.sendto(pay, (target, 9999))
            time.sleep(0.02)
            continue
        if mode.get("busy", 0) and time.time() >= nxt_burst:
            for _ in range(rnd.randrange(50, 400)):
                s.sendto(pay, (target, 9999))
            log.write("%d burst\n" % wall_ms())
            nxt_burst = time.time() + rnd.uniform(3, 20)
        time.sleep(0.02)


# ---- one session --------------------------------------------------------------------------

def nsrun(ns, args, log):
    return subprocess.Popen(["ip", "netns", "exec", ns] + args, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)


def session(out, k, spec, helper):
    """One comparison: base 0-60 s, window 60-120 s, stop at 127 s, as the
    G0 interface frames it. spec: {"id", "dns_rate", "base": {...}, "window":
    {...}, "events": [[t_s, action, arg], ...]} where actions are mode
    updates ("mode", {...}), "hold" 1/0, "pause" seconds (the helper stopped:
    its ring overflows) and "asym" 1/0 (the resolver answers over dev1)."""
    os.makedirs(out, exist_ok=True)
    n = topo_up(k)
    modefile = os.path.join(out, "mode.json")
    mode = dict(dns_rate=spec.get("dns_rate", 1.0), delay_ms=[10, 30], drop=0.0, servfail=0.0,
                arp_bursty=spec.get("arp_bursty", 1), arp_storm=0.0, dhcp_answer=1, busy=spec.get("busy", 0), blast=0)
    mode.update(spec.get("base", {}))

    def set_mode(upd):
        mode.update(upd)
        tmp = modefile + ".tmp"
        json.dump(mode, open(tmp, "w"))
        os.replace(tmp, modefile)

    set_mode({})
    logs = {}
    procs = []
    me = os.path.abspath(__file__)
    sub = net(k)

    def agent(ns, name, *args):
        lf = open(os.path.join(out, "agent-%s.out" % name), "w")
        logs[name] = os.path.join(out, "truth-%s.log" % name)
        procs.append(nsrun(ns, [sys.executable, me] + list(args) + [logs[name]], lf))

    agent(n["dns"], "dns", "agent-dns", modefile)
    agent(n["dhcp"], "dhcpd", "agent-dhcpd", modefile, "%s.2" % sub)
    for i, p in enumerate(n["peers"]):
        agent(p, "arp%d" % i, "agent-arp", modefile, "eth0", sub, str(100 + i))
    agent(n["peers"][2], "dhcpc", "agent-dhcpc", modefile, "3", "7")
    agent(n["peers"][3], "dhcpc2", "agent-dhcpc", modefile, "3", "8")
    agent(n["peers"][1], "bulk", "agent-bulk", modefile, "%s.10" % sub)
    # the device's own traffic: DNS questions, and a ping a second (a way back)
    agent(n["dev"], "client", "agent-client", modefile, "%s.53" % sub)
    ping = nsrun(n["dev"], ["ping", "-q", "-i", "1", "%s.21" % sub], subprocess.DEVNULL)
    time.sleep(3)  # agents up, ARP caches warm

    # the helper, as the shell drives it: one socketpair for stdin and stdout
    rt = os.path.join(out, "run")
    os.makedirs(rt, exist_ok=True)
    cap = os.path.join("/root/n12cap", "b%d" % k, "Captures")
    os.makedirs(os.path.dirname(cap), mode=0o700, exist_ok=True)
    sh("rm -rf %s" % cap)
    env = dict(os.environ, POCKETOS_RUNTIME_DIR=rt, POCKETOS_LOG_DIR=rt, POCKETOS_LOG_STDERR="0",
               POCKETOS_CAPTURES_DIR=cap)
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    hp = subprocess.Popen(["ip", "netns", "exec", n["dev"], helper, "session"], stdin=b.fileno(), stdout=b.fileno(),
                          stderr=subprocess.DEVNULL, env=env, close_fds=True)
    b.close()
    side = open(os.path.join(out, "sidecar.txt"), "w", buffering=1)
    side.write("own %s\n" % mac_of(n["dev"], "dev0"))
    lines = []
    stop_reader = threading.Event()

    def reader():
        buf = b""
        a.settimeout(0.2)
        while not stop_reader.is_set():
            try:
                d = a.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                t = wall_ms()
                s = ln.decode(errors="replace")
                lines.append((t, s))
                f = s.split("\t")
                if f[0] == "stats" and len(f) > 6:
                    side.write("read %d 1 %d\n" % (t, int(f[5]) + int(f[6])))
                elif f[0] == "state":
                    side.write("state %d %s\n" % (t, f[1]))
                elif f[0] == "note" and len(f) > 2 and f[1] == "shed":
                    side.write("shed %d %s\n" % (t, f[2]))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(0.5)
    t0 = wall_ms()
    side.write("start %d\n" % t0)
    a.sendall(b"start dev0 headers 0 all 3600\n")
    events = sorted(spec.get("events", []), key=lambda e: e[0])
    log_ev = open(os.path.join(out, "truth-events.log"), "w", buffering=1)
    for t_s, act, arg in events + [[127.0, "end", None]]:
        while wall_ms() < t0 + int(t_s * 1000):
            time.sleep(0.01)
        log_ev.write("%d %s %s\n" % (wall_ms(), act, json.dumps(arg)))
        if act == "mode":
            set_mode(arg)
        elif act == "hold":
            a.sendall(b"hold %d\n" % arg)
        elif act == "pause":
            hpid = hp.pid  # ip netns exec runs the helper in its own process (exec)
            set_mode({"blast": 1})
            time.sleep(0.3)
            os.kill(hpid, signal.SIGSTOP)
            time.sleep(arg)
            os.kill(hpid, signal.SIGCONT)
            time.sleep(0.3)
            set_mode({"blast": 0})
            log_ev.write("%d paused %s s\n" % (wall_ms(), arg))
        elif act == "asym":
            sh("ip -n %s route replace %s.10/32 %s" % (n["dns"], sub, "via 10.78.%d.10 dev dns1" % k if arg else "dev dns0"))
    side.write("stop %d\n" % wall_ms())
    a.sendall(b"stop\n")
    time.sleep(0.8)
    a.sendall(b"save\n")
    t_end = time.time() + 30
    while time.time() < t_end and not any(s.startswith("saved\tdone") or s.startswith("saved\tfail") for _, s in lines):
        time.sleep(0.1)
    a.sendall(b"quit\n")
    time.sleep(0.5)
    stop_reader.set()
    for p in procs + [ping]:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except Exception:
            pass
    try:
        hp.wait(5)
    except Exception:
        hp.kill()
    with open(os.path.join(out, "helper.out"), "w") as f:
        for t, s in lines:
            if not s.startswith(("row\t", "pkt", "insp")):
                f.write("%d %s\n" % (t, s))
    saved = [s.split("\t") for _, s in lines if s.startswith("saved\tdone")]
    if saved:
        sh("cp %s/%s %s/capture.pcap" % (cap, saved[-1][5], out))
    sh("rm -rf %s" % cap)
    topo_down(k)
    return bool(saved)


def main():
    cmd = sys.argv[1]
    if cmd == "agent-dns":
        agent_dns(sys.argv[2], sys.argv[3])
    elif cmd == "agent-client":
        agent_client(sys.argv[2], sys.argv[4], sys.argv[3])
    elif cmd == "agent-arp":
        agent_arp(sys.argv[2], sys.argv[6], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    elif cmd == "agent-dhcpd":
        agent_dhcp_server(sys.argv[2], sys.argv[4], sys.argv[3])
    elif cmd == "agent-dhcpc":
        agent_dhcp_clients(sys.argv[2], sys.argv[5], int(sys.argv[3]), int(sys.argv[4]))
    elif cmd == "agent-bulk":
        agent_bulk(sys.argv[2], sys.argv[4], sys.argv[3])
    elif cmd == "session":
        ok = session(sys.argv[2], int(sys.argv[3]), json.loads(open(sys.argv[4]).read()), sys.argv[5])
        print("session", sys.argv[2], "saved" if ok else "NOT SAVED")
    elif cmd == "down":
        topo_down(int(sys.argv[2]))


if __name__ == "__main__":
    main()
