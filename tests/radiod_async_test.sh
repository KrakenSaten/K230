#!/bin/bash
# radiod's asynchronous transmit, radio lease, monotonic receive timestamps
# and generic channel status, over the real socket.
#
# tests/radiod_mock_test.sh covers the v0 contract and must keep passing
# unchanged; this file covers what was added on top of it, and in particular
# the two properties that cannot be checked from a single request-and-reply:
# that radio.send_async answers before the packet has gone out, and that the
# daemon still answers everyone else while it is going.
#
# Run from the repository root after `make all`.
set -u

RADIOD=${RADIOD:-services/radiod/radiod}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR
POCKETOS_RUNTIME_DIR=$(mktemp -d)
SOCK="$POCKETOS_RUNTIME_DIR/radiod.sock"
failed=0

cleanup() {
    [ -n "${RADIOD_PID:-}" ] && kill "$RADIOD_PID" 2>/dev/null
    wait "${RADIOD_PID:-}" 2>/dev/null
    rm -rf "$POCKETOS_RUNTIME_DIR"
}
trap cleanup EXIT

start_radiod() { # start_radiod <logname> [args...]
    local log="$1"; shift
    "$RADIOD" --backend mock --region EU868 "$@" \
        > "$POCKETOS_RUNTIME_DIR/$log" 2>&1 &
    RADIOD_PID=$!
    local i
    for i in $(seq 1 50); do [ -S "$SOCK" ] && return 0; sleep 0.1; done
    echo "FAIL radiod did not start"; cat "$POCKETOS_RUNTIME_DIR/$log"; exit 1
}

stop_radiod() {
    kill "$RADIOD_PID" 2>/dev/null
    wait "$RADIOD_PID" 2>/dev/null
    RADIOD_PID=""
}

# The python driver prints its own "ok"/"FAIL" lines; count the failures.
run_driver() { # run_driver <script-name>
    local out
    out=$(python3 - "$SOCK" < "$1")
    printf '%s\n' "$out"
    failed=$((failed + $(printf '%s\n' "$out" | grep -c '^FAIL' || true)))
}

check() { # check <name> <expected-substring> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -10
        failed=$((failed + 1))
    fi
}

DRIVER="$POCKETOS_RUNTIME_DIR/driver.py"
cat > "$DRIVER" <<'PYEOF'
import json, socket, struct, sys, time

SOCK = sys.argv[1]
fails = 0

def ok(label, cond, detail=""):
    global fails
    if cond:
        print("ok   " + label)
    else:
        fails += 1
        print("FAIL " + label + ((": " + str(detail)) if detail else ""))

class Conn:
    """One connection to radiod. Requests are matched by id; events that
    arrive while waiting for a reply are kept, not thrown away - half of
    what is being tested here is which of the two arrives first."""
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(SOCK)
        self.s.settimeout(10)
        self.buf = b""
        self.events = []
        self.next_id = 1

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _send(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        body = {"id": rid, "method": method}
        if params is not None:
            body["params"] = params
        raw = json.dumps(body).encode()
        self.s.sendall(struct.pack(">I", len(raw)) + raw)
        return rid

    def _frame(self, timeout=None):
        if timeout is not None:
            self.s.settimeout(timeout)
        try:
            while True:
                if len(self.buf) >= 4:
                    n = struct.unpack(">I", self.buf[:4])[0]
                    if len(self.buf) >= 4 + n:
                        msg = json.loads(self.buf[4:4 + n])
                        self.buf = self.buf[4 + n:]
                        return msg
                chunk = self.s.recv(65536)
                if not chunk:
                    return None
                self.buf += chunk
        finally:
            self.s.settimeout(10)

    def call(self, method, params=None, timeout=10):
        rid = self._send(method, params)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self._frame(timeout=max(0.01, deadline - time.monotonic()))
            if msg is None:
                raise IOError("connection closed waiting for " + method)
            if "event" in msg:
                msg["_t"] = time.monotonic()
                self.events.append(msg)
                continue
            if msg.get("id") == rid:
                return msg
        raise IOError("timed out waiting for " + method)

    def result(self, method, params=None, timeout=10):
        r = self.call(method, params, timeout)
        return r.get("result")

    def error(self, method, params=None):
        r = self.call(method, params)
        return r.get("error", {})

    def pump(self, seconds):
        """Collect events for a while."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                msg = self._frame(timeout=max(0.01, end - time.monotonic()))
            except socket.timeout:
                break
            if msg is None:
                break
            if "event" in msg:
                msg["_t"] = time.monotonic()
                self.events.append(msg)

    def wait_event(self, name, timeout=10):
        for e in self.events:
            if e.get("event") == name:
                return e
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                msg = self._frame(timeout=max(0.01, deadline - time.monotonic()))
            except socket.timeout:
                return None
            if msg is None:
                return None
            if "event" in msg:
                msg["_t"] = time.monotonic()
                self.events.append(msg)
                if msg.get("event") == name:
                    return msg
        return None

    def drop_events(self):
        self.events = []


def knob(c, key, value):
    c.result("mock.set", {"key": key, "value": value})


# ---------------------------------------------------------------------------
# 1. The asynchronous transmit answers before the packet has gone out.
#
# This is the whole point of the addition and the one thing a test of the
# reply alone cannot show. A backend that transmitted inside the request
# handler would produce exactly the same reply - the difference is only
# visible in when it arrives relative to the completion.
# ---------------------------------------------------------------------------
obs = Conn()
obs.result("radio.subscribe")
tx = Conn()
# SF7/BW125/CR4/5, where a 10-byte packet is 41.216 ms - the value
# tests/airtime_test.c pins against published calculators. radiod starts on
# the EU868 default of SF9, which would be 144.384 ms; pinning it here means
# the airtimes below are checked against a known number rather than against
# whatever the daemon happened to compute.
tx.result("radio.configure", {"spreading_factor": 7, "bandwidth_khz": 125,
                              "coding_rate": 5, "preamble_length": 8, "crc": True})
knob(tx, "tx_delay_ms", 1200)

t0 = time.monotonic()
r = tx.result("radio.send_async", {"payload_hex": "00112233445566778899"})
t_reply = time.monotonic() - t0

ok("radio.send_async is accepted", r is not None and r.get("accepted") is True, r)
ok("it returns a tx_id", isinstance(r.get("tx_id"), (int, float)) and r["tx_id"] >= 1, r)
ok("it reports the bytes it took", r.get("bytes") == 10, r)
ok("and the airtime it expects", abs(r.get("airtime_ms", 0) - 41.216) < 0.01, r)
ok("the reply comes back promptly, long before the airtime",
   t_reply < 0.3, "%.3f s" % t_reply)

tx_id = r["tx_id"]

# And the daemon is still serving everyone else while the packet is on air.
# Before this change it was inside a blocking send and answered nothing.
served = 0
states = set()
t1 = time.monotonic()
while time.monotonic() - t1 < 0.6:
    st = obs.result("radio.status", timeout=2)
    states.add(st.get("state"))
    served += 1
ok("the daemon keeps answering while the packet is on the air", served >= 3, served)
ok("and says it is transmitting", states == {"tx"}, states)

done = obs.wait_event("radio.tx_done", timeout=5)
t_done = done["_t"] - t0 if done else -1
ok("the completion arrives", done is not None)
if done:
    d = done["data"]
    ok("it names the transmission that was accepted", d.get("tx_id") == tx_id, d)
    ok("the completion came after the reply, not with it",
       t_done > 1.0, "%.3f s" % t_done)
    ok("it reports success", d.get("ok") is True and d.get("result") == "ok", d)
    ok("the bytes went out", d.get("transmitted") is True, d)
    ok("and the radio is receiving again", d.get("rx_resumed") is True, d)
    ok("it carries the state the radio ended in", d.get("state") == "rx", d)
    ok("with the airtime", abs(d.get("airtime_ms", 0) - 41.216) < 0.01, d)
    ok("a monotonic completion time", isinstance(d.get("mono_ms"), (int, float))
       and d["mono_ms"] > 0, d)
    ok("and a wall-clock one alongside it",
       isinstance(d.get("timestamp_ms"), (int, float)), d)
    ok("the v0 fields are still there", "bytes" in d and "airtime_ms" in d
       and "timestamp_ms" in d, d)
    ok("nothing failed, so no error is reported", "error" not in d, d)

# Exactly one completion per accepted transmission.
obs.pump(0.4)
dones = [e for e in obs.events if e.get("event") == "radio.tx_done"]
ok("exactly one completion for one transmission", len(dones) == 1, len(dones))

st = obs.result("radio.status")
ok("the radio is receiving again afterwards", st.get("state") == "rx", st)
stats = obs.result("radio.stats")
ok("the transmission was counted once", stats.get("tx_packets") == 1, stats)

# ---------------------------------------------------------------------------
# 2. One at a time. A second request is refused, never queued behind the
#    first or allowed to overwrite it.
# ---------------------------------------------------------------------------
obs.drop_events()
r = tx.result("radio.send_async", {"payload_hex": "aabb"})
busy_id = r["tx_id"]
ok("a second transmission gets its own id", busy_id != tx_id, busy_id)

e = tx.error("radio.send_async", {"payload_hex": "ccdd"})
ok("another asynchronous send while one is on the air is refused", e.get("code") == 5, e)
ok("and the refusal names what is holding the radio",
   str(busy_id) in e.get("message", ""), e)
e = tx.error("radio.send", {"payload_hex": "ccdd"})
ok("a synchronous send is refused the same way", e.get("code") == 5, e)

# Things that would disturb a packet already going out. Before the
# asynchronous path existed these could not arrive during a transmit at all,
# so nothing guarded them; a configure landing here would change the
# frequency of a packet that is already on the air.
e = tx.error("radio.configure", {"spreading_factor": 8})
ok("radio.configure is refused while a packet is on the air", e.get("code") == 5, e)
e = tx.error("radio.cad")
ok("radio.cad is refused too", e.get("code") == 5, e)

# Reads stay available. A diagnostic that cannot run while the radio is busy
# is a diagnostic nobody can use when they need it.
ok("radio.status still answers", obs.result("radio.status").get("state") == "tx")
ok("radio.stats still answers", obs.result("radio.stats") is not None)
ok("radio.info still answers", obs.result("radio.info") is not None)
ch = obs.result("radio.channel")
ok("radio.channel still answers", ch is not None)
ok("and says the radio is transmitting, so it knows nothing about the channel",
   ch.get("transmitting") is True and ch.get("rssi_known") is False
   and ch.get("activity_known") is False, ch)

done = obs.wait_event("radio.tx_done", timeout=5)
ok("the refused requests did not disturb the one in flight",
   done is not None and done["data"]["tx_id"] == busy_id, done)
ok("and it still succeeded", done["data"]["ok"] is True, done)

# ---------------------------------------------------------------------------
# 3. The failure paths, each producing exactly one completion.
# ---------------------------------------------------------------------------
knob(tx, "tx_delay_ms", 0)
for knob_name, label, expect in [
        ("tx_fail",           "a transmit that fails",              "tx_failed"),
        ("tx_fail_begin",     "a transmit refused before going out","tx_failed"),
        ("tx_rx_fails_after", "receive lost after transmitting",    "rx_resume_failed")]:
    obs.drop_events()
    knob(tx, knob_name, 1)
    r = tx.result("radio.send_async", {"payload_hex": "0102"})
    ok(label + " is still accepted", r.get("accepted") is True, r)
    d = obs.wait_event("radio.tx_done", timeout=5)
    ok(label + " completes", d is not None)
    if d:
        data = d["data"]
        ok(label + " is not reported as ok", data.get("ok") is False, data)
        ok(label + " says which case it was", data.get("result") == expect, data)
        ok(label + " explains itself", bool(data.get("error")), data)
        ok(label + " names its tx_id", data.get("tx_id") == r["tx_id"], data)
        if expect == "rx_resume_failed":
            # The packet went out. A daemon reading only `ok` would send it
            # again; this is why the two are reported separately.
            ok("a lost receive still reports the packet as transmitted",
               data.get("transmitted") is True and data.get("rx_resumed") is False, data)
            ok("and the state it left the radio in", data.get("state") == "error", data)
        else:
            ok(label + " reports nothing transmitted",
               data.get("transmitted") is False, data)
    obs.pump(0.3)
    dones = [e for e in obs.events if e.get("event") == "radio.tx_done"]
    ok(label + " produced exactly one completion", len(dones) == 1, len(dones))
    knob(tx, knob_name, 0)

knob(tx, "rx_failing", 0)
time.sleep(1.5)
ok("the radio recovers afterwards", obs.result("radio.status").get("state") == "rx")

# ---------------------------------------------------------------------------
# 4. The blocking fallback: a backend with no asynchronous transmit, which is
#    what the SX1262 is. The contract must hold there too - the client is
#    answered first and the completion follows - even though the daemon is
#    unresponsive for the airtime.
# ---------------------------------------------------------------------------
obs.drop_events()
knob(tx, "tx_async", 0)
r = tx.result("radio.send_async", {"payload_hex": "0a0b0c"})
ok("a backend without an asynchronous transmit still accepts one",
   r.get("accepted") is True, r)
ok("and still returns a tx_id before the packet goes out",
   isinstance(r.get("tx_id"), (int, float)), r)
d = obs.wait_event("radio.tx_done", timeout=5)
ok("it completes through the same event", d is not None and d["data"]["ok"] is True, d)
ok("and reports the same shape", d["data"].get("result") == "ok", d)
knob(tx, "tx_async", 1)

# ---------------------------------------------------------------------------
# 5. The synchronous radio.send, unchanged apart from the id it now carries.
# ---------------------------------------------------------------------------
obs.drop_events()
r = tx.result("radio.send", {"payload_hex": "00112233445566778899"})
ok("radio.send still answers with the airtime",
   abs(r.get("airtime_ms", 0) - 41.216) < 0.01, r)
ok("and the byte count", r.get("bytes") == 10, r)
ok("and now also the id its completion will carry",
   isinstance(r.get("tx_id"), (int, float)), r)
d = obs.wait_event("radio.tx_done", timeout=5)
ok("the synchronous send raises the same completion event",
   d is not None and d["data"]["tx_id"] == r["tx_id"], d)
ok("through the same state machine", d["data"]["ok"] is True, d)

e = tx.error("radio.send", {"payload_hex": "0102", "timeout_ms": 500})
ok("timeout_ms is still refused", e.get("code") == 2, e)
ok("and now points at the asynchronous path",
   "send_async" in e.get("message", ""), e)

# ---------------------------------------------------------------------------
# 6. The lease. An exclusivity boundary, opt-in: while nobody holds one,
#    everything works as it always did.
# ---------------------------------------------------------------------------
lease = obs.result("radio.lease")
ok("nothing holds the lease to begin with", lease.get("held") is False, lease)
ok("and a client that has not asked does not hold it", lease.get("mine") is False, lease)
ok("so an unleased radio still transmits for anybody",
   tx.result("radio.send", {"payload_hex": "01"}).get("bytes") == 1)

owner = Conn()
owner.result("radio.subscribe")
r = owner.result("radio.acquire", {"owner": "meshcored"})
ok("a client takes the lease", r.get("held") is True and r.get("mine") is True, r)
ok("it is labelled", r.get("owner") == "meshcored", r)
ok("and identified", isinstance(r.get("owner_id"), (int, float)) and r["owner_id"] >= 1, r)
owner_id = r["owner_id"]

ev = owner.wait_event("radio.lease", timeout=2)
ok("taking it is announced", ev is not None and ev["data"].get("reason") == "acquired", ev)

r = owner.result("radio.acquire", {"owner": "meshcored"})
ok("the holder may ask again after a reconnect it is unsure about",
   r.get("mine") is True and r.get("owner_id") == owner_id, r)

e = tx.error("radio.acquire", {"owner": "meshtasticd"})
ok("another client cannot take it", e.get("code") == 5, e)
ok("and is told who has it", "meshcored" in e.get("message", ""), e)

# The boundary itself: the operations that use the radio are the owner's.
for method, params in [("radio.send", {"payload_hex": "01"}),
                       ("radio.send_async", {"payload_hex": "01"}),
                       ("radio.configure", {"spreading_factor": 8}),
                       ("radio.cad", None)]:
    e = tx.error(method, params)
    ok(method + " is refused to a client that does not hold the lease",
       e.get("code") == 3, e)
    ok(method + "'s refusal says why", "lease" in e.get("message", ""), e)

# Reads are not. A daemon owning the radio must not make it undiagnosable.
for method in ["radio.info", "radio.status", "radio.stats", "radio.rssi",
               "radio.channel", "radio.lease"]:
    ok(method + " is still available to everybody",
       tx.result(method) is not None)

ok("the owner may still transmit",
   owner.result("radio.send", {"payload_hex": "01"}).get("bytes") == 1)
ok("and configure", owner.result("radio.configure", {"spreading_factor": 7}) is not None)

e = tx.error("radio.release")
ok("a client that does not hold the lease cannot release it", e.get("code") == 3, e)
ok("and is told so", "not by you" in e.get("message", ""), e)
ok("the lease is still held", obs.result("radio.lease").get("held") is True)

r = owner.result("radio.release")
ok("the holder releases it", r.get("held") is False, r)
e = owner.error("radio.release")
ok("releasing again is refused rather than silently ignored", e.get("code") == 3, e)
ok("and everybody may transmit again",
   tx.result("radio.send", {"payload_hex": "01"}).get("bytes") == 1)

# A daemon that dies without releasing must not lock the radio for good.
owner.result("radio.acquire", {"owner": "meshcored"})
ok("it is held again", obs.result("radio.lease").get("held") is True)
obs.drop_events()
owner.close()
time.sleep(0.4)
ev = obs.wait_event("radio.lease", timeout=3)
ok("the owner disconnecting is announced",
   ev is not None and ev["data"].get("reason") == "client_gone", ev)
ok("and the lease is free", obs.result("radio.lease").get("held") is False)
ok("so another client can take it",
   tx.result("radio.acquire", {"owner": "meshtasticd"}).get("held") is True)
new_id = obs.result("radio.lease").get("owner_id")
ok("with a new identity, not the one that went away", new_id != owner_id, new_id)
tx.result("radio.release")

ok("an over-long owner label is refused",
   tx.error("radio.acquire", {"owner": "x" * 200}).get("code") == 2)

# ---------------------------------------------------------------------------
# 7. The submitter disappears while its packet is on the air. The transmit
#    finishes - the radio is mid-packet - and the daemon does not lose track
#    of it.
# ---------------------------------------------------------------------------
obs.drop_events()
before = obs.result("radio.stats")["tx_packets"]
gone = Conn()
knob(obs, "tx_delay_ms", 700)
r = gone.result("radio.send_async", {"payload_hex": "deadbeef"})
ok("a transmission is accepted", r.get("accepted") is True, r)
gone.close()

d = obs.wait_event("radio.tx_done", timeout=5)
ok("it still completed after its submitter vanished",
   d is not None and d["data"]["tx_id"] == r["tx_id"], d)
ok("and it still went out", d["data"]["ok"] is True, d)
after = obs.result("radio.stats")["tx_packets"]
ok("and was counted", after == before + 1, (before, after))
ok("the radio is free again", obs.result("radio.status").get("state") == "rx")
knob(obs, "tx_delay_ms", 0)

# The same, with the client closing the instant the reply is written.
before = obs.result("radio.stats")["tx_packets"]
for i in range(10):
    c = Conn()
    c.result("radio.send_async", {"payload_hex": "%02x%02x" % (i, i)})
    c.close()
    time.sleep(0.05)
time.sleep(0.5)
after = obs.result("radio.stats")["tx_packets"]
ok("ten submit-and-vanish transmissions all went out", after == before + 10,
   (before, after))
ok("and the daemon is still answering", obs.result("radio.info") is not None)

# ---------------------------------------------------------------------------
# 8. Monotonic receive timestamps.
# ---------------------------------------------------------------------------
obs.drop_events()
obs.result("mock.inject_rx", {"payload_hex": "48656c6c6f"})
e1 = obs.wait_event("radio.rx", timeout=3)
ok("a received packet carries a monotonic timestamp",
   e1 is not None and isinstance(e1["data"].get("mono_ms"), (int, float)), e1)
ok("and still carries the wall-clock one beside it",
   "timestamp_ms" in e1["data"], e1)
m1 = e1["data"]["mono_ms"]
w1 = e1["data"]["timestamp_ms"]
ok("they are not the same clock relabelled", w1 - m1 > 1500000000000, (w1, m1))

time.sleep(0.25)
obs.drop_events()
obs.result("mock.inject_rx", {"payload_hex": "5445"})
e2 = obs.wait_event("radio.rx", timeout=3)
m2 = e2["data"]["mono_ms"]
ok("the next packet's monotonic time is later", m2 > m1, (m1, m2))
ok("by about the time that passed", 150 < m2 - m1 < 5000, m2 - m1)

# Past 2^32, which a board reaches after 49.7 days of uptime. Anything that
# narrowed the value to 32 bits on the way out would show here.
obs.drop_events()
big = 4294967296 + 123456
obs.result("mock.inject_rx", {"payload_hex": "ff", "mono_ms": big})
e3 = obs.wait_event("radio.rx", timeout=3)
ok("a monotonic time past 2^32 survives to the client",
   e3 is not None and e3["data"].get("mono_ms") == big, e3)
obs.drop_events()
bigger = 1099511627776
obs.result("mock.inject_rx", {"payload_hex": "fe", "mono_ms": bigger})
e4 = obs.wait_event("radio.rx", timeout=3)
ok("and so does one past 2^40", e4["data"].get("mono_ms") == bigger, e4)

# ---------------------------------------------------------------------------
# 9. Generic channel status. What matters is that the unknown stays unknown.
# ---------------------------------------------------------------------------
ch = obs.result("radio.channel")
ok("the channel report is timestamped monotonically",
   isinstance(ch.get("mono_ms"), (int, float)) and ch["mono_ms"] > 0, ch)
ok("it says whether the radio is transmitting", ch.get("transmitting") is False, ch)
ok("the mock knows its own channel",
   ch.get("rssi_known") is True and ch.get("noise_known") is True
   and ch.get("activity_known") is True, ch)
ok("so it reports a busy flag", isinstance(ch.get("busy"), bool), ch)
ok("and the values it claims to know", "rssi_dbm" in ch and "noise_dbm" in ch, ch)
ok("it reports whether CAD can be asked for separately",
   ch.get("cad_supported") is True, ch)

knob(obs, "channel_unknown", 1)
ch = obs.result("radio.channel")
ok("a backend that measures nothing says so",
   ch.get("rssi_known") is False and ch.get("noise_known") is False
   and ch.get("activity_known") is False, ch)
ok("and offers no value it cannot stand behind",
   "rssi_dbm" not in ch and "noise_dbm" not in ch and "busy" not in ch, ch)
ok("but still says whether CAD exists", ch.get("cad_supported") is True, ch)
knob(obs, "channel_unknown", 0)

# ---------------------------------------------------------------------------
# 10. Malformed and extreme requests.
# ---------------------------------------------------------------------------
for params, why in [({"payload_hex": ""},        "an empty payload"),
                    ({"payload_hex": "0"},       "an odd number of hex digits"),
                    ({"payload_hex": "zz"},      "hex that is not hex"),
                    ({"payload_hex": "00" * 256},"a payload past the maximum"),
                    ({},                         "no payload at all"),
                    ({"payload_hex": 7},         "a payload that is not a string")]:
    e = tx.error("radio.send_async", params)
    ok("radio.send_async refuses " + why, e.get("code") == 2, e)
ok("nothing was left in flight by the refusals",
   tx.result("radio.status").get("state") == "rx")

r = tx.result("radio.send_async", {"payload_hex": "ab" * 255})
ok("the largest legal payload is accepted", r.get("bytes") == 255, r)
obs.wait_event("radio.tx_done", timeout=10)

e = tx.error("radio.acquire", {"owner": 7})
ok("a non-string owner label is ignored rather than accepted as one",
   e == {} or e.get("code") == 2, e)
tx.result("radio.release") if obs.result("radio.lease").get("held") else None

# ---------------------------------------------------------------------------
# 11. A lease owner that stops reading its events.
#
# The new path this exercises is a nested one. radiod broadcasts an event;
# the write to a client that has stopped reading eventually fails; that
# closes the client from inside the broadcast; closing it raises the
# disconnect callback, which releases the lease and broadcasts again. Before
# the disconnect callback existed, closing a client did nothing but close it.
#
# What must come out of it: the lease is freed, the other subscriber's event
# stream is still well-formed, and radiod is still there.
# ---------------------------------------------------------------------------
obs.drop_events()
deaf = Conn()
deaf.result("radio.subscribe")
deaf.result("radio.acquire", {"owner": "deaf-daemon"})
ok("the deaf client holds the lease", obs.result("radio.lease").get("held") is True)

# Fill its socket until radiod's bounded write to it gives up. The mock's
# receive queue is 16 deep, so this goes in batches and lets the daemon
# drain between them.
freed = False
for batch in range(80):
    for _ in range(16):
        try:
            obs.result("mock.inject_rx", {"payload_hex": "ab" * 255}, timeout=5)
        except Exception:
            pass
    if obs.result("radio.lease", timeout=5).get("held") is False:
        freed = True
        break
ok("a lease owner that stops reading is eventually dropped and the lease freed",
   freed, "after %d batches" % (batch + 1))
ok("and the other subscriber's stream is still readable",
   obs.result("radio.status").get("state") in ("rx", "error"))
ok("radiod is still serving", obs.result("radio.info").get("chip") == "mock")
ok("another client can take the lease afterwards",
   tx.result("radio.acquire", {"owner": "after"}).get("held") is True)
tx.result("radio.release")
deaf.close()

print("driver: %d failure(s)" % fails)
PYEOF

start_radiod radiod.log
run_driver "$DRIVER"

# A garbage frame from a client must not take the daemon, or anyone else's
# lease, with it.
python3 - "$SOCK" <<'PY' || true
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.sendall(b"\x00\x00\x00\x03abc")
s.settimeout(1)
try: s.recv(64)
except Exception: pass
s.close()
PY
out=$("$POS" radio info 2>&1)
check "radiod survives an invalid frame from a lease-aware client" '"chip"' "$out"

stop_radiod

# A restart is the clean slate it claims to be: no lease survives it, and the
# transmission ids start again - which is safe only because a restart closes
# every connection there was, so no client can still be holding an old one.
start_radiod radiod2.log
out=$(python3 - "$SOCK" <<'PY'
import json, socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
def call(m, p=None):
    body = {"id": 1, "method": m}
    if p: body["params"] = p
    raw = json.dumps(body).encode()
    s.sendall(struct.pack(">I", len(raw)) + raw)
    n = struct.unpack(">I", s.recv(4))[0]
    return json.loads(s.recv(n, socket.MSG_WAITALL))
print(json.dumps(call("radio.lease")))
print(json.dumps(call("radio.send", {"payload_hex": "0102"})))
PY
)
check "no lease survives a restart" '"held":[[:space:]]*false' "$(printf '%s' "$out" | head -1)"
check "and transmission ids start again at 1" '"tx_id":[[:space:]]*1' "$(printf '%s' "$out" | tail -1)"
stop_radiod

echo "radiod_async_test: $failed failure(s)"
exit $((failed > 0))
