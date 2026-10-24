#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real DOORS shell and a scripted, non-radio meshcored.
SHELL_BIN=/path/to/pocketos-shell python3 tests/meshscope_shell_test.py
Optional SHOTS_DIR retains screenshots for visual review.
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

def call(run, method, **params):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(4)
        s.connect(str(run / 'shell.sock'))
        b = json.dumps(dict(id=1, method=method, params=params)).encode()
        s.sendall(struct.pack('!I', len(b)) + b)
        def read(n):
            b = b''
            while len(b) < n:
                chunk = s.recv(n-len(b))
                if not chunk:
                    raise RuntimeError('shell disconnected')
                b += chunk
            return b
        reply = json.loads(read(struct.unpack('!I', read(4))[0]))
        assert 'error' not in reply, reply
        return reply['result']

def tap(run, text):
    call(run, 'shell.tap', text=text)
    time.sleep(.22)

def wait_socket(path):
    for _ in range(100):
        if path.exists():
            return
        time.sleep(.05)
    raise RuntimeError(f'No socket: {path}')

def main():
    with tempfile.TemporaryDirectory(prefix='meshscope-') as tmp:
        base = Path(tmp)
        shots = Path(os.environ.get('SHOTS_DIR', str(base/'shots'))).resolve()
        shots.mkdir(parents=True, exist_ok=True)
        for orientation in ('portrait', 'landscape'):
            run = base / orientation
            run.mkdir()
            nodes = []
            for i, (name, lat, lon) in enumerate((('OSLO',59.91,10.75),('NORD',59.96,10.79),('VEST',59.92,10.68))):
                nodes.append(dict(public_key=f'{i+0xa1:02x}'+'0'*62,node_hash=f'{i+0xa1:02x}', name=name,
                                  type=2,lat=lat,lon=lon,last_heard_mono_ms=-1000))
            event = dict(v=1,seq=1,mono_ms=-1000,bytes=42,verdict='new',type_code=5,type='group_text',
                         route='flood',path_kind='hops',path_hash_size=1,path_hops=3,path_hex='a1a2a3',
                         hash='1234567890abcdef',dup=1,rssi_dbm=-82,snr_db=7.5)
            events = run/'events'
            events.write_text('mesh.rx|'+json.dumps(event)+'\n')
            env = dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=str(run),
                       POCKETOS_LOG_DIR=str(run),POCKETOS_CONFIG_DIR=str(run),POCKETOS_STATE_DIR=str(run),
                       FAKE_MESHCORED_STATE='online', FAKE_MESHCORED_NODES=json.dumps(nodes),
                       FAKE_MESHCORED_EVENTS=str(events),FAKE_MESHCORED_RX_LOG='1',
                       FAKE_MESHCORED_EVENTS_AFTER_SNAPSHOT='1',FAKE_MESHCORED_LIFE_MS='60000',
                       FAKE_MESHCORED_METHODS=str(run/'methods'))
            log = (run/'shell.log').open('w')
            fake = subprocess.Popen([str(FAKE)],env=env,stdout=log,stderr=log)
            shell = None
            try:
                wait_socket(run/'meshcored.sock')
                shell = subprocess.Popen([SHELL,'--open','meshscope','--rotation',orientation,
                                          '--text-size','large','--theme','carbon'],env=env,stdout=log,stderr=log)
                wait_socket(run/'shell.sock')
                time.sleep(1)
                assert call(run,'shell.info')['current']=='meshscope'
                def shot(name):
                    p = shots / f'{orientation}-{name}.png'
                    call(run,'shell.screenshot',path=str(p))
                    assert p.read_bytes().startswith(b'\x89PNG')
                shot('map')
                tap(run,'PACKETS'); shot('packets')
                tap(run,'NODES'); shot('nodes')
                tap(run,'OSLO  [a10000]\nPosition advertised - tap for details'); shot('node')
                tap(run,'BACK TO LIST')
                tap(run,'MAP')
                tap(run,'+'); tap(run,'-'); tap(run,'FIT')
                call(run,'shell.theme',theme='brass',mode='outdoor'); shot('outdoor')
                # A real loss must leave the UI usable and cached data labelled.
                fake.terminate(); fake.wait(timeout=3)
                time.sleep(1.3); shot('disconnected')
                assert call(run,'shell.info')['current']=='meshscope'
                # Close and reopen: no hidden capture session is retained.
                call(run,'shell.home'); time.sleep(.2)
                call(run,'shell.open',id='meshscope'); time.sleep(.3)
                shot('reopened')
            finally:
                for proc in (shell,fake):
                    if proc and proc.poll() is None:
                        proc.terminate()
                        try: proc.wait(timeout=4)
                        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                log.close()
            methods=(run/'methods').read_text().splitlines()
            assert 'mesh.subscribe' in methods and 'mesh.nodes' in methods
            allowed={'mesh.subscribe','mesh.unsubscribe','mesh.info','mesh.status','mesh.identity',
                     'mesh.nodes','mesh.channels','mesh.messages','mesh.path_hash','mesh.remote_session','mesh.discovered'}
            assert set(methods)<=allowed, set(methods)-allowed
            print(f'{orientation}: tabs, node detail, map controls, theme, disconnect, reopen, read-only IPC passed')
        print(f'Screenshots: {shots}')

if __name__=='__main__':
    main()
