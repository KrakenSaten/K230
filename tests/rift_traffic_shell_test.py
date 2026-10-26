#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""MAP's TRAFFIC in RIFT, in the real DOORS shell, against a scripted
meshcored on Unix sockets (no radio).

SHELL_BIN=/path/to/pocketos-shell python3 tests/rift_traffic_shell_test.py
Optional SHOTS_DIR keeps the screenshots for review.

Both orientations at Large text: RIFT opened, MAP, TRAFFIC on, a tab away and
back, the map controls, LIVE, a theme change, TRAFFIC off, the service going
away and coming back, RIFT left and reopened. Every method the service was
asked for while TRAFFIC was in use is read back from its own log: only reads.
"""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SHELL = os.environ['SHELL_BIN']
FAKE = ROOT / 'tests/fake-meshcored'
READS = {'mesh.subscribe', 'mesh.unsubscribe', 'mesh.info', 'mesh.status', 'mesh.identity',
         'mesh.nodes', 'mesh.node', 'mesh.channels', 'mesh.messages', 'mesh.path_hash',
         'mesh.remote_session', 'mesh.discovered'}
NEVER = {'mesh.send', 'mesh.advert', 'mesh.discover', 'mesh.remote_login', 'mesh.remote_cli',
         'mesh.remote_request', 'mesh.node_remove', 'mesh.node_reset_path', 'mesh.channel_add',
         'mesh.channel_remove', 'mesh.set_name', 'mesh.set_path_hash', 'mesh.path_preview'}


def call(run, method, **params):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(4)
        s.connect(str(run / 'shell.sock'))
        b = json.dumps(dict(id=1, method=method, params=params)).encode()
        s.sendall(struct.pack('!I', len(b)) + b)

        def read(n):
            b = b''
            while len(b) < n:
                chunk = s.recv(n - len(b))
                if not chunk:
                    raise RuntimeError('shell disconnected')
                b += chunk
            return b
        reply = json.loads(read(struct.unpack('!I', read(4))[0]))
        assert 'error' not in reply, (method, params, reply)
        return reply['result']


def tap(run, text):
    call(run, 'shell.tap', text=text)
    time.sleep(.25)


def wait_socket(path):
    for _ in range(100):
        if path.exists():
            return
        time.sleep(.05)
    raise RuntimeError(f'No socket: {path}')


def methods(run):
    p = run / 'methods'
    return p.read_text().splitlines() if p.exists() else []


def fake_env(run, nodes, events):
    return dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=str(run),
                POCKETOS_LOG_DIR=str(run), POCKETOS_CONFIG_DIR=str(run),
                POCKETOS_STATE_DIR=str(run),
                FAKE_MESHCORED_STATE='online', FAKE_MESHCORED_NODES=json.dumps(nodes),
                FAKE_MESHCORED_EVENTS=str(events), FAKE_MESHCORED_RX_LOG='1',
                FAKE_MESHCORED_EVENTS_AFTER_SNAPSHOT='1', FAKE_MESHCORED_LIFE_MS='90000',
                FAKE_MESHCORED_METHODS=str(run / 'methods'))


def main():
    with tempfile.TemporaryDirectory(prefix='rift-traffic-') as tmp:
        base = Path(tmp)
        shots = Path(os.environ.get('SHOTS_DIR', str(base / 'shots'))).resolve()
        shots.mkdir(parents=True, exist_ok=True)
        for orientation in ('portrait', 'landscape'):
            run = base / orientation
            run.mkdir()
            nodes = []
            for i, (name, lat, lon) in enumerate((('OSLO', 59.91, 10.75), ('NORD', 59.96, 10.79),
                                                  ('VEST', 59.92, 10.68))):
                nodes.append(dict(public_key=f'{i + 0xa1:02x}' + '0' * 62, node_hash=f'{i + 0xa1:02x}',
                                  name=name, type=2, lat=lat, lon=lon, last_heard_mono_ms=-1000))
            events = run / 'events'
            lines = []
            for seq in range(1, 4):
                lines.append('mesh.rx|' + json.dumps(dict(
                    v=1, seq=seq, mono_ms=-300 * seq, bytes=42, verdict='new' if seq == 1 else 'duplicate',
                    type_code=5, type='group_text', route='flood', path_kind='hops', path_hash_size=1,
                    path_hops=3, path_hex='a1a2a3', hash='1234567890abcdef', dup=seq,
                    rssi_dbm=-82, snr_db=7.5)))
            events.write_text('\n'.join(lines) + '\n')
            env = fake_env(run, nodes, events)
            log = (run / 'shell.log').open('w')
            fake = subprocess.Popen([str(FAKE)], env=env, stdout=log, stderr=log)
            shell = None
            try:
                wait_socket(run / 'meshcored.sock')
                shell = subprocess.Popen([SHELL, '--open', 'rift', '--rotation', orientation,
                                          '--text-size', 'large', '--theme', 'carbon'],
                                         env=env, stdout=log, stderr=log)
                wait_socket(run / 'shell.sock')
                time.sleep(1.5)
                assert call(run, 'shell.info')['current'] == 'rift'

                def shot(name):
                    p = shots / f'{orientation}-{name}.png'
                    call(run, 'shell.screenshot', path=str(p))
                    assert p.read_bytes().startswith(b'\x89PNG')

                tap(run, 'MAP')
                shot('map')
                before = len(methods(run))
                tap(run, 'TRAFFIC')
                time.sleep(.3)
                shot('traffic')
                tap(run, 'NODES')
                tap(run, 'MAP')
                shot('traffic-after-tab')
                tap(run, '+')
                tap(run, '−')
                tap(run, 'FIT')
                tap(run, 'LIVE')
                call(run, 'shell.theme', theme='brass', mode='outdoor')
                time.sleep(.3)
                shot('traffic-outdoor')
                call(run, 'shell.theme', theme='carbon', mode='normal')
                tap(run, 'TRAFFIC')
                shot('map-again')
                used = methods(run)[before:]
                assert set(used) <= READS, set(used) - READS
                tap(run, 'TRAFFIC')
                # A real loss: the held packets stay, labelled; no write on the way back.
                fake.terminate()
                fake.wait(timeout=3)
                time.sleep(1.5)
                shot('traffic-service-lost')
                assert call(run, 'shell.info')['current'] == 'rift'
                fake = subprocess.Popen([str(FAKE)], env=env, stdout=log, stderr=log)
                time.sleep(3)
                shot('traffic-reconnected')
                # RIFT left and reopened: its session (and ring) stays in the background.
                call(run, 'shell.home')
                time.sleep(.3)
                call(run, 'shell.open', id='rift')
                time.sleep(.6)
                assert call(run, 'shell.info')['current'] == 'rift'
                shot('reopened')
            finally:
                for proc in (shell, fake):
                    if proc and proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=4)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait()
                log.close()
            every = methods(run)
            assert every.count('mesh.subscribe') >= 2, 'one subscription per connection'
            assert not (set(every) & NEVER), set(every) & NEVER
            print(f'{orientation}: MAP, TRAFFIC, tabs, controls, theme, service loss and return, '
                  f'reopen; TRAFFIC asked only for reads ({len(every)} methods in all, none sends)')
        print(f'Screenshots: {shots}')


if __name__ == '__main__':
    main()
