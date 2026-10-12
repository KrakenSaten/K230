#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Real libcurl against loopback TLS; exclusively synthetic responses."""
import email.utils
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import threading
import time

out = Path('work/ninjaone-tests')
subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                '-keyout', str(out/'key.pem'), '-out', str(out/'cert.pem')],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
seen = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args): pass

    def do_GET(self):
        seen.append(self.path)
        assert self.headers['Authorization'] == 'Bearer synthetic-secret-token'
        status = int(self.path[1:]) if self.path[1:].isdigit() else 200
        payload = b'x' * (2 * 1024 * 1024 + 1) if self.path == '/large' else (b'synthetic-secret-token' if status != 200 else b'[]')
        self.send_response(status)
        self.send_header('Content-Length', str(len(payload)))
        if status == 302: self.send_header('Location', '/redirected')
        if status == 429: self.send_header('Retry-After', '75')
        if self.path == '/date':
            self.send_header('Retry-After', email.utils.formatdate(time.time()+180, usegmt=True))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError): pass


class Server(http.server.ThreadingHTTPServer):
    def handle_error(self, *_args):
        pass  # Expected connection cuts in certificate/cap tests; no captures.


server = Server(('127.0.0.1', 0), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(out/'cert.pem', out/'key.pem')
server.socket = context.wrap_socket(server.socket, server_side=True)
thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
base = f'https://localhost:{server.server_port}'


def get(path, ca=str(out/'cert.pem'), url=base):
    result = subprocess.run([str(out/'http-test'), url, ca, path], capture_output=True, text=True, check=True,
                            env={**os.environ, "NO_PROXY": "localhost,127.0.0.1", "no_proxy": "localhost,127.0.0.1"})
    assert 'synthetic-secret-token' not in result.stdout + result.stderr
    parsed = tuple(map(int, result.stdout.split()))
    return parsed


try:
    assert get('/200')[0] == 0
    assert get('/401') == (2, 0, 0)
    assert get('/403') == (3, 0, 0)
    assert get('/429') == (4, 75, 0)
    assert 178 <= get('/date')[1] <= 180
    assert get('/302')[0] == 5 and '/redirected' not in seen
    assert get('/large')[0] == 7
    assert get('/200', '-') == (5, 0, 0)
    assert get('/200', url=base.replace('localhost', '127.0.0.1')) == (5, 0, 0)
    server.shutdown(); server.server_close()
    assert get('/200')[0] == 5
    print('HTTP/TLS: GET, 401, 403, 429, Retry-After seconds/date, redirect refusal, response cap, CA/hostname verification, network loss and redaction passed')
finally:
    server.server_close()
