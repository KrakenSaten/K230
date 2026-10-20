#!/usr/bin/env python3
"""Unit B, test session only: drive the running shell over its own IPC socket
(the client of tools/design/neon_text_captures.py). Sent over SSH stdin; not
installed.
  neon-drive.py state                      print theme/mode/current/rotation
  neon-drive.py prepare THEME MODE         set theme+mode, back to the launcher root
  neon-drive.py sequence                   the fixed interaction sequence (~20 s)
  neon-drive.py shot PATH                  screenshot (needs LVGL snapshot)
  neon-drive.py call METHOD [JSON]         any shell method
"""
import json
import socket
import struct
import sys
import time

SOCK = '/run/pocketos/shell.sock'


def call(method, params=None):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(8)
        conn.connect(SOCK)
        data = json.dumps(dict(id=1, method=method, params=params or {})).encode()
        conn.sendall(struct.pack('!I', len(data)) + data)

        def read(n):
            out = b''
            while len(out) < n:
                part = conn.recv(n - len(out))
                if not part:
                    raise RuntimeError('shell closed the connection')
                out += part
            return out

        reply = json.loads(read(struct.unpack('!I', read(4))[0]))
        if 'error' in reply:
            raise RuntimeError(reply['error'])
        return reply['result']


def state():
    i = call('shell.info')
    d = i.get('display', {})
    print(f"theme={i.get('theme')} mode={i.get('mode')} current={i.get('current')} "
          f"rotation={d.get('rotation')} build={i.get('build')}")


cmd = sys.argv[1]
if cmd == 'state':
    state()
elif cmd == 'prepare':
    call('shell.theme', dict(theme=sys.argv[2], mode=sys.argv[3]))
    call('shell.home')
    call('shell.folder', dict(id=''))
    time.sleep(1)
    state()
elif cmd == 'sequence':
    steps = [('shell.folder', dict(id='utilities'), 2), ('shell.folder', dict(id=''), 2),
             ('shell.folder', dict(id='utilities'), 2), ('shell.folder', dict(id=''), 2),
             ('shell.open', dict(id='settings'), 3), ('shell.home', {}, 2),
             ('shell.open', dict(id='rift'), 3), ('shell.home', {}, 2),
             ('shell.folder', dict(id=''), 1)]
    for method, params, wait in steps:
        call(method, params)
        time.sleep(wait)
    print('sequence done')
elif cmd == 'call':
    print(json.dumps(call(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {})))
elif cmd == 'shot':
    call('shell.screenshot', dict(path=sys.argv[2]))
