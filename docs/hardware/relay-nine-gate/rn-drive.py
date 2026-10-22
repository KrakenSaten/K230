#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Unit B, Relay Nine test session only: drive the running test shell over
its IPC socket. Keys go through shell.key, the physical keyboard's own path
(key map, game opt-in, release), as raw matrix codes. Not installed.
  rn-drive.py call METHOD [JSON]     any shell method
  rn-drive.py shot NAME              screenshot -> <kit>/shots/NAME.png
  rn-drive.py play STEP...           steps, in order:
      open                 shell.open the game
      home | back          shell.home / Back action
      KEY[+KEY...]:MS      hold the keys together for MS, then release all
      tap:KEY              press and release
      down:KEY / up:KEY    press only / release only
      wait:MS              sleep
      shot:NAME            screenshot
      vol:PCT / mute:0|1   shell.volume
Keys: w a s d left right up down space space2 q r f p esc enter h
"""
import json
import os
import socket
import struct
import sys
import time

SOCK = '/run/pocketos/shell.sock'
KIT = os.path.dirname(os.path.abspath(__file__))
SHOTS = '/var/lib/pocketos/screenshots'
CODES = dict(w=39, a=29, s=28, d=27, left=2, right=1, up=22, down=12, space=5, space2=14,
             q=20, r=37, f=26, p=42, esc=40, enter=21, h=35)


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


def keys(names, pressed):
    raw = [(0x80 if pressed else 0) | CODES[n] for n in names]
    return call('shell.key', {'raw': raw})


def shot(name):
    before = set(os.listdir(SHOTS)) if os.path.isdir(SHOTS) else set()
    call('shell.action', {'action': 'screenshot'})
    for _ in range(100):
        time.sleep(0.1)
        new = sorted(set(os.listdir(SHOTS)) - before) if os.path.isdir(SHOTS) else []
        # ffmpeg writes the file in place; wait until its size settles.
        if new:
            path = os.path.join(SHOTS, new[0])
            size = os.path.getsize(path)
            time.sleep(0.4)
            if size and size == os.path.getsize(path):
                os.makedirs(os.path.join(KIT, 'shots'), exist_ok=True)
                dest = os.path.join(KIT, 'shots', name + '.png')
                os.rename(path, dest)
                return dest
    raise RuntimeError('no screenshot appeared')


def log(msg):
    print('%.3f %s' % (time.monotonic(), msg), flush=True)


def step(s):
    if s == 'open':
        log('open %s' % call('shell.open', {'id': 'neon-outpost'}))
    elif s in ('home', 'back'):
        log('%s %s' % (s, call('shell.home') if s == 'home' else call('shell.action', {'action': 'back'})))
    elif s.startswith('wait:'):
        time.sleep(int(s[5:]) / 1000)
    elif s.startswith('shot:'):
        log('shot %s' % shot(s[5:]))
    elif s.startswith('vol:'):
        log('volume %s' % call('shell.volume', {'percent': int(s[4:])}))
    elif s.startswith('mute:'):
        log('volume %s' % call('shell.volume', {'muted': s[5:] == '1'}))
    elif s.startswith('tap:'):
        keys([s[4:]], True)
        time.sleep(0.08)
        keys([s[4:]], False)
        log(s)
    elif s.startswith('down:') or s.startswith('up:'):
        name = s.split(':', 1)[1]
        keys([name], s.startswith('down:'))
        log(s)
    else:
        names, ms = s.rsplit(':', 1)
        names = names.split('+')
        keys(names, True)
        log('hold %s %s ms' % ('+'.join(names), ms))
        time.sleep(int(ms) / 1000)
        keys(names, False)


def main(argv):
    if len(argv) >= 2 and argv[1] == 'call':
        print(json.dumps(call(argv[2], json.loads(argv[3]) if len(argv) > 3 else None), indent=1))
    elif len(argv) == 3 and argv[1] == 'shot':
        print(shot(argv[2]))
    elif len(argv) >= 3 and argv[1] == 'play':
        for s in argv[2:]:
            step(s)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
