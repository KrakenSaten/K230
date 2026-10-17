#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Only synthetic credentials; exercise the real helper store and curl renewal."""
import ctypes as C
import importlib.util
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

root = Path.cwd()
binary = root/'work/ninjaone-tests/auth-test'
helper = root/'tools/ninjaone/pos-ninjaone'
class Org(C.Structure):
    _fields_ = [('id', C.c_int32), ('name', C.c_char*160)]
class Device(C.Structure):
    _fields_ = [('id', C.c_int32), ('org', C.c_int32), ('offline', C.c_int32), ('contact', C.c_double), ('name', C.c_char*160)]
class Alert(C.Structure):
    _fields_ = [('device', C.c_int32), ('uid', C.c_char*80), ('message', C.c_char*160), ('severity', C.c_char*64)]
class Frame(C.Structure):
    _fields_ = [('magic', C.c_uint32), ('error', C.c_int32), ('retry', C.c_int32), ('demo', C.c_int32), ('partial', C.c_int32),
               ('updated', C.c_int64), ('expires', C.c_int64), ('profile', C.c_char*25),
               ('norgs', C.c_uint32), ('ndevices', C.c_uint32), ('nalerts', C.c_uint32),
               ('orgs', Org*250), ('devices', Device*500), ('alerts', Alert*500)]

def frames(raw):
    assert len(raw) % C.sizeof(Frame) == 0, len(raw)
    return [Frame.from_buffer_copy(raw[i:i+C.sizeof(Frame)]) for i in range(0, len(raw), C.sizeof(Frame))]

with tempfile.TemporaryDirectory(dir=root/'work/ninjaone-tests') as tmp:
    p = Path(tmp); (p/'runtime').mkdir(); (p/'config').mkdir()
    (p/'config/ninjaone.conf').write_text('base=https://eu.ninjarmm.com\n')
    env = {**os.environ, 'POCKETOS_RUNTIME_DIR': str(p/'runtime'), 'POCKETOS_CONFIG_DIR': str(p/'config'), 'NO_PROXY': 'localhost,127.0.0.1'}
    subprocess.run([binary, 'store'], env=env, check=True)
    key, cert = p/'key.pem', p/'cert.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
                    '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True)
    mode, calls, entered, release = 'rotate', [], threading.Event(), threading.Event()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            auth = self.headers.get('Authorization')
            status = 401 if auth == 'Bearer synthetic-access-A' or mode == 'still401' else 200
            raw = b'[]'
            self.send_response(status); self.send_header('Content-Length', str(len(raw))); self.end_headers()
            self.wfile.write(raw)
        def do_POST(self):
            fields = parse_qs(self.rfile.read(int(self.headers['Content-Length'])).decode())
            assert self.path == '/ws/oauth/token' and set(fields) == {'grant_type', 'client_id', 'refresh_token'}
            assert fields['grant_type'] == ['refresh_token'] and fields['client_id'] == ['native-client']
            calls.append(fields)
            if mode == 'wait': entered.set(); release.wait(3)
            if mode == 'drop': self.connection.shutdown(2); self.connection.close(); return
            status = 429 if mode == 'rate' else 400 if mode == 'revoked' else 200
            data = {'access_token': 'synthetic-access-B', 'refresh_token': 'synthetic-refresh-B',
                    'token_type': 'Bearer', 'scope': 'monitoring', 'expires_in': 3600}
            if mode == 'missing': del data['refresh_token']
            if mode == 'revoked': data = {'error': 'invalid_grant'}
            raw = json.dumps(data).encode()
            try:
                self.send_response(status)
                if mode == 'rate': self.send_header('Retry-After', '90')
                self.send_header('Content-Length', str(len(raw))); self.end_headers()
                self.wfile.write(raw)
            except (BrokenPipeError, ssl.SSLError): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.load_cert_chain(cert, key)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    url = f'https://localhost:{server.server_port}'
    def run(exe, *args, input=None):
        result = subprocess.run([exe, *args], env=env, input=input, capture_output=True, check=True)
        assert b'synthetic-' not in result.stdout + result.stderr
        return result.stdout
    def provision(expiry=None, profile='000000000000000000000001'):
        bundle = dict(access_token='synthetic-access-A', refresh_token='synthetic-refresh-A', client_id='native-client',
                      base='https://eu.ninjarmm.com', profile=profile, expires_at=expiry or int(time.time())-1)
        run(helper, 'set-credentials', input=json.dumps(bundle).encode())
    def renew(): return frames(run(binary, url, str(cert)))
    provision(int(time.time())+3600); before = len(calls)
    assert [f.error for f in renew()] == [0] and len(calls) == before
    provision(); reply = renew(); assert [f.error for f in reply] == [9, 0], ([f.error for f in reply], len(calls))
    assert reply[-1].norgs == reply[-1].ndevices == 1 and reply[-1].expires > time.time()
    before = len(calls); stored = renew()[-1]
    assert stored.error == 0 and stored.norgs == stored.ndevices == 1 and len(calls) == before
    # Valid recorded TTL, but expiry/401 during data collection: one renewal.
    provision(int(time.time())+3600); before = len(calls)
    assert [f.error for f in frames(run(binary, url, str(cert), 'snapshot'))] == [9, 0]
    assert len(calls) == before + 1
    mode = 'still401'; provision(int(time.time())+3600); before = len(calls)
    assert frames(run(binary, url, str(cert), 'snapshot'))[-1].error == 2
    assert len(calls) == before + 1  # Replacement rejected: no renewal loop.
    for mode in ('revoked', 'missing', 'drop'):
        provision(); assert renew()[-1].error == 2
        before = len(calls); assert renew()[-1].error == 2 and len(calls) == before
    mode = 'rate'; provision(); reply = renew()[-1]; assert reply.error == 11 and reply.retry == 90 and reply.nalerts == 1
    before = len(calls); assert renew()[-1].error == 11 and len(calls) == before
    # Advance stored cooldown only in this synthetic fixture. Three requests max.
    creds = p/'runtime/ninjaone/credentials'
    # Binary layout: five strings followed by two int64 values and int attempts.
    class Cred(C.Structure):
        _fields_ = [('access', C.c_char*4096), ('refresh', C.c_char*4096), ('base', C.c_char*128),
                    ('client', C.c_char*160), ('profile', C.c_char*25), ('expires', C.c_int64), ('retry', C.c_int64), ('attempts', C.c_int)]
    for attempt in (2, 3):
        c = Cred.from_buffer_copy(creds.read_bytes()); c.retry = 0; creds.write_bytes(bytes(c))
        assert renew()[-1].nalerts == attempt
    before = len(calls); assert renew()[-1].error == 2 and len(calls) == before
    mode = 'wait'; provision()
    process = subprocess.Popen([binary, url, str(cert)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Drain the progress frame to let the child reach its synthetic server.
    progress = process.stdout.read(C.sizeof(Frame)); assert frames(progress)[0].error == 9
    assert entered.wait(2)
    blocked = subprocess.run([helper, 'forget'], env=env, capture_output=True, check=True)
    assert frames(blocked.stdout)[-1].error == 8  # No overlapping write or deletion.
    process.kill(); process.communicate(); release.set(); time.sleep(.1)
    before = len(calls); assert renew()[-1].error == 2 and len(calls) == before  # No interrupted replay.
    assert frames(run(helper, 'forget'))[-1].error == 10
    assert not creds.exists() and not (p/'runtime/ninjaone/renewing').exists()
    assert frames(run(helper, 'snapshot'))[-1].error == 10
    provision(profile='000000000000000000000002')
    assert renew()[-1].profile == b'000000000000000000000002'
    server.shutdown(); server.server_close(); thread.join()

spec = importlib.util.spec_from_file_location('provision', root/'tools/ninjaone/provision.py')
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
assert p.redirect_address('http://localhost:8765/fixed')[0] == ('127.0.0.1', 8765)
assert p.callback_code('/fixed?code=synthetic&state=s', 's', '/fixed') == 'synthetic'
for uri in ('http://evil.example/callback', 'http://localhost/cb?x=y', 'http://user@localhost/cb', 'https://localhost/cb'):
    try: p.redirect_address(uri)
    except ValueError: pass
    else: raise AssertionError('unsupported redirect accepted')
data = dict(access_token='synthetic-A', refresh_token='synthetic-R', token_type='Bearer', scope='monitoring offline_access', expires_in=3600)
a = p.credential_bundle(data, 'https://eu.ninjarmm.com', 'native', 'tenant-A', 100)
b = p.credential_bundle(data, 'https://eu.ninjarmm.com', 'native', 'tenant-B', 100)
assert a['profile'] != b['profile'] and a['expires_at'] == 3700
assert a['profile'] == p.credential_bundle(data, 'https://eu.ninjarmm.com', 'new-native', 'tenant-A', 100)['profile']
print('OAuth: expiry, mid-snapshot 401, rotation, revoked/missing/ambiguous replies, bounded retries, cancellation, forget and tenant namespaces passed')
