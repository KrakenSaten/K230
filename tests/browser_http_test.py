#!/usr/bin/env python3
"""pos-browser over real sockets: the libcurl fetcher against local HTTP and
HTTPS servers (docs/apps/BROWSER.md "Validation").

usage: browser_http_test.py HELPER WORKDIR

Every check drives the real helper, as `dump` or as a `session` over pipes,
the way the shell drives it. The servers run in this process; nothing
leaves the machine (proxies are switched off for the helper).
"""
import gzip
import http.server
import os
import select
import socket
import ssl
import subprocess
import sys
import threading
import time
import zlib
import struct

HELPER, WORK = sys.argv[1], sys.argv[2]
failed = 0
checks = 0


def check(name, ok):
    global failed, checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + name, flush=True)
    if not ok:
        failed += 1


def png(w, h):
    def chunk(k, b):
        return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xFFFFFFFF)
    row = b"\x00" + b"".join(bytes([(x * 255) // max(w - 1, 1), 80, 160]) for x in range(w))
    raw = row * h
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PAGES = {}
PORTS = {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="text/html; charset=utf-8", extra=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        http_port, https_port = PORTS["http"], PORTS["https"]
        if p == "/":
            self.send(200, "<title>Local</title><h1>Hello</h1><p>From <a href=/two>two</a>.</p>")
        elif p.startswith("/q"):
            self.send(200, "<title>Query</title><p>q</p>")
        elif p == "/two":
            self.send(200, "<title>Two</title><p>second</p>")
        elif p == "/moved":
            self.send(301, "", extra=[("Location", "/two")])
        elif p == "/to-https":
            self.send(302, "", extra=[("Location", "https://localhost:%d/" % https_port)])
        elif p == "/to-http":
            self.send(302, "", extra=[("Location", "http://127.0.0.1:%d/two" % http_port)])
        elif p == "/to-file":
            self.send(302, "", extra=[("Location", "file:///etc/passwd")])
        elif p == "/to-ftp":
            self.send(302, "", extra=[("Location", "ftp://127.0.0.1/x")])
        elif p == "/loop":
            self.send(302, "", extra=[("Location", "/loop")])
        elif p == "/big":
            self.send(200, "<title>Big</title>" + "<p>" + "x" * 100 + "</p>" * 1 + ("<p>filler</p>" * 300000))
        elif p == "/gzip":
            body = gzip.compress(b"<title>Zipped</title><p>" + b"compressed " * 1000 + b"</p>")
            self.send(200, body, extra=[("Content-Encoding", "gzip")])
        elif p == "/bomb":
            body = gzip.compress(b"<p>" + b"a" * (64 * 1024 * 1024))
            self.send(200, body, extra=[("Content-Encoding", "gzip")])
        elif p == "/setcookie":
            self.send(302, "", extra=[("Set-Cookie", "sid=abc123; Path=/"), ("Location", "/showcookie")])
        elif p == "/showcookie":
            self.send(200, "<title>Cookie</title><p>cookie=%s</p>" % self.headers.get("Cookie", "none"))
        elif p == "/pics":
            self.send(200, "<title>Pics</title><p>a</p><img src=/pic.png alt=pic width=300>"
                           "<img src=/nopic.png alt=missing width=300><img src=/html.png alt=notpng width=300>")
        elif p == "/pic.png":
            self.send(200, PAGES["png"], "image/png")
        elif p == "/nopic.png":
            self.send(404, "no", "text/plain")
        elif p == "/html.png":
            self.send(200, "<p>not a png</p>", "image/png")
        elif p == "/bin":
            self.send(200, b"\x00\x01\x02" * 1000, "application/octet-stream")
        elif p == "/latin1":
            self.send(200, b"<title>L</title><p>Caf\xe9</p>", "text/html; charset=iso-8859-1")
        elif p == "/slow":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "100000")
            self.end_headers()
            try:
                for _ in range(100):
                    self.wfile.write(b"<p>slow</p>")
                    self.wfile.flush()
                    time.sleep(0.1)
            except OSError:
                pass
        elif p == "/short":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "5000")
            self.end_headers()
            self.wfile.write(b"<title>Short</title><p>only this")
            self.wfile.flush()
            self.close_connection = True
        else:
            self.send(404, "<title>Not Found</title><p>no</p>")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # a client that refused our certificate hangs up: expected


def serve(tls_ctx=None):
    s = Server(("127.0.0.1", 0), Handler)
    if tls_ctx:
        s.socket = tls_ctx.wrap_socket(s.socket, server_side=True)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s, s.server_address[1]


def raw_server(reply):
    """Answers every connection with reply, whatever was asked."""
    ls = socket.socket()
    ls.bind(("127.0.0.1", 0))
    ls.listen(8)

    def run():
        while True:
            c, _ = ls.accept()
            try:
                c.recv(4096)
                c.sendall(reply)
            except OSError:
                pass
            c.close()
    threading.Thread(target=run, daemon=True).start()
    return ls.getsockname()[1]


def dump(url, *args, env=None):
    e = dict(ENV)
    if env:
        e.update(env)
    p = subprocess.run([HELPER, "dump"] + list(args) + [url], capture_output=True, text=True, env=e, timeout=60)
    return p.returncode, p.stdout, p.stderr


class Session:
    def __init__(self, images=True, extra=()):
        self.dir = os.path.join(WORK, "img%d" % time.monotonic_ns())
        os.mkdir(self.dir, 0o700)
        args = [HELPER, "session"] + (["--images", self.dir] if images else []) + list(extra)
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV)
        self.buf = b""

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def lines(self, until, timeout):
        """Read lines until until(line) is true or timeout; returns them all."""
        out = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                s = l.decode("utf-8", "replace")
                out.append(s)
                if until(s):
                    return out
            r, _, _ = select.select([self.p.stdout], [], [], max(0.0, end - time.monotonic()))
            if not r:
                break
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                break
            self.buf += chunk
        return out

    def close(self):
        try:
            self.send("quit")
        except OSError:
            pass
        try:
            return self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            return None


# ---- setup ---------------------------------------------------------------------------------------
ENV = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
ENV["POCKETOS_LOG_DIR"] = os.path.join(WORK, "log")
ENV["POCKETOS_LOG_STDERR"] = "0"
PAGES["png"] = png(640, 360)

feat = subprocess.run([HELPER, "features"], capture_output=True, text=True).stdout.strip()
if not feat.startswith("net"):
    print("SKIP the real fetcher: this pos-browser has no libcurl (%s); make BROWSER_CURL=1" % feat)
    print("browser_http_test: 0 check(s), 0 failure(s) (skipped)")
    sys.exit(0)

cert, key, cert_other, key_other = (os.path.join(WORK, n) for n in ("cert.pem", "key.pem", "o.pem", "ok.pem"))
for c, k, cn, san in ((cert, key, "localhost", "DNS:localhost,IP:127.0.0.1"), (cert_other, key_other, "other.test",
                                                                               "DNS:other.test")):
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", k, "-out", c, "-days",
                    "2", "-subj", "/CN=" + cn, "-addext", "subjectAltName=" + san], check=True,
                   capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
ctx_other = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx_other.load_cert_chain(cert_other, key_other)
_, PORTS["http"] = serve()
_, PORTS["https"] = serve(ctx)
_, other_port = serve(ctx_other)
H = "http://127.0.0.1:%d" % PORTS["http"]
S = "https://localhost:%d" % PORTS["https"]
garbage_port = raw_server(b"HELLO THERE\r\n\r\nnot http at all\x00\xff")
empty_port = raw_server(b"")

# ---- pages -------------------------------------------------------------------------------------------
rc, out, err = dump(H + "/")
check("a page over HTTP", rc == 0 and "Local" in out and "HTTP 200" in out)
rc, out, err = dump(H + "/moved")
check("a 301 is followed to where it points", rc == 0 and "Two" in out and "/two" in out)
rc, out, err = dump(H + "/loop")
check("a redirect loop ends with a failure", rc == 1 and "redirects" in err)
rc, out, err = dump(H + "/to-file")
check("a redirect to file:// is not followed", rc == 1 and "bad-redirect" in err)
rc, out, err = dump(H + "/to-ftp")
check("nor to ftp://", rc == 1 and "bad-redirect" in err)
rc, out, err = dump(H + "/missing")
check("a 404 with a page is shown", rc == 0 and "HTTP 404" in out and "Not Found" in out)
rc, out, err = dump(H + "/big")
check("a page over 2 MB is cut at 2 MB and shown", rc == 0 and "(cut)" in out and "2097152 bytes" in out)
rc, out, err = dump(H + "/gzip")
check("gzip is decoded", rc == 0 and "Zipped" in out and "compressed compressed" in out)
t0 = time.monotonic()
rc, out, err = dump(H + "/bomb")
check("a gzip bomb (64 MB inflated) is cut at 2 MB, not inflated whole",
      rc == 0 and "2097152 bytes" in out and time.monotonic() - t0 < 20)
rc, out, err = dump(H + "/latin1")
check("an ISO-8859-1 page is shown as UTF-8", rc == 0 and "Café" in out)
rc, out, err = dump(H + "/bin")
check("a download is refused with the words", rc == 1 and "downloads are not supported" in err)
rc, out, err = dump(H + "/setcookie")
check("cookies live for the helper's life (in memory)", rc == 0 and "cookie=sid=abc123" in out)
rc, out, err = dump(H + "/showcookie")
check("and are gone with it: never written", rc == 0 and "cookie=none" in out)
rc, out, err = dump(H + "/short")
check("a connection cut before the end fails as an unreadable answer", rc == 1 and "response" in err)
rc, out, err = dump("http://127.0.0.1:%d/" % garbage_port)
check("an answer that is not HTTP fails cleanly", rc == 1 and err.strip() != "")
rc, out, err = dump("http://127.0.0.1:%d/" % empty_port)
check("a server that says nothing fails cleanly", rc == 1 and "response" in err)

# ---- HTTPS -------------------------------------------------------------------------------------------
rc, out, err = dump(S + "/")
check("a self-signed certificate is refused", rc == 1 and "tls:" in err)
rc, out, err = dump(S + "/", "--ca-file", cert)
check("and accepted from a CA file", rc == 0 and "Local" in out)
rc, out, err = dump("https://127.0.0.1:%d/" % other_port, "--ca-file", cert_other)
check("a certificate for another name is refused, CA or not", rc == 1 and "tls:" in err)
rc, out, err = dump(H + "/to-https", "--ca-file", cert)
check("http to https is followed", rc == 0 and "Local" in out and out.split("\n")[2].startswith("https://"))
rc, out, err = dump(S + "/to-http", "--ca-file", cert)
check("https to http is not: insecure-redirect", rc == 1 and "insecure-redirect" in err)

# ---- where the network is not ----------------------------------------------------------------------------
rc, out, err = dump("http://127.0.0.1:1/")
check("connection refused", rc == 1 and "connect:" in err)
rc, out, err = dump("http://doors-browser-test.invalid/")
check("an unknown host", rc == 1 and ("dns:" in err or "offline:" in err))
empty = os.path.join(WORK, "noroute")
os.makedirs(empty, exist_ok=True)
rc, out, err = dump("http://127.0.0.1:1/", env={"POCKETOS_BROWSER_PROC_NET": empty})
check("refused with no default route at all: no network", rc == 1 and "offline:" in err)
rc, out, err = dump("file:///etc/passwd")
check("file:// is refused before anything (exit 2)", rc == 2 and "root:" not in out)

# ---- the session, as the shell drives it ------------------------------------------------------------------
s = Session()
hello = s.lines(lambda l: l.startswith("hello"), 3)
check("session: hello with the network and the decoders", hello and hello[-1].startswith("hello\t1\tnet"))
s.send("open\t1\t528\ti\t%s/pics" % H)
got = s.lines(lambda l: l == "idle\t1", 10)
pix = [l for l in got if l.startswith("pixels\t1\t0\t")]
check("a page with pictures: the page, then pixels for the PNG", any(l.startswith("page\t1\t") for l in got) and
      len(pix) == 1)
if pix:
    f = pix[0].split("\t")
    path = os.path.join(s.dir, f[5])
    size = os.path.getsize(path) if os.path.exists(path) else -1
    check("scaled to the page's 300 wide, exactly w*h*2 bytes, 0600",
          f[3] == "300" and f[4] == "168" and size == 300 * 168 * 2 and
          (os.stat(path).st_mode & 0o777) == 0o600)
check("a picture that is not there: nopixels with its HTTP status", any(l == "nopixels\t1\t1\tHTTP 404" for l in got))
check("a .png that is HTML: nopixels, format", any(l.startswith("nopixels\t1\t2\t") and "format" in l for l in got))
s.send("open\t2\t528\ti\t%s/slow" % H)
s.lines(lambda l: l.startswith("progress\t2\treceive"), 5)
t0 = time.monotonic()
s.send("stop\t2")
got = s.lines(lambda l: l == "idle\t2", 5)
check("STOP ends a page that is arriving, within half a second",
      got and got[-1] == "idle\t2" and time.monotonic() - t0 < 0.5 and not any(l.startswith("page\t2") for l in got))
s.send("open\t3\t528\t-\t%s/slow" % H)
s.lines(lambda l: l.startswith("progress\t3\treceive"), 5)
t0 = time.monotonic()
s.send("open\t4\t528\t-\t%s/two" % H)
got = s.lines(lambda l: l == "idle\t4", 5)
check("a new page replaces one arriving, at once",
      any(l.startswith("page\t4") for l in got) and time.monotonic() - t0 < 1.0 and
      not any(l.startswith("page\t3") for l in got))
s.send("open\t5\t528\t-\t%s/q?token=SECRET-TOKEN-123" % H)
s.lines(lambda l: l == "idle\t5", 5)
check("session: quit leaves at once", s.close() == 0)
logs = ""
for root, _, files in os.walk(ENV["POCKETOS_LOG_DIR"]):
    for n in files:
        logs += open(os.path.join(root, n), errors="replace").read()
check("the log names hosts, never a path or a query", "127.0.0.1" in logs and "SECRET-TOKEN" not in logs and
      "/pics" not in logs)
s = Session(images=False)
s.lines(lambda l: l.startswith("hello"), 3)
s.send("open\t1\t528\ti\t%s/pics" % H)
got = s.lines(lambda l: l == "idle\t1", 10)
check("without a picture directory no picture is fetched", not any(l.startswith("pixels") for l in got))
s.p.stdin.close()
try:
    rc = s.p.wait(timeout=3)
except subprocess.TimeoutExpired:
    rc = None
check("session: the shell closing its end ends the helper", rc == 0)

print("browser_http_test: %d check(s), %d failure(s)" % (checks, failed))
sys.exit(1 if failed else 0)
