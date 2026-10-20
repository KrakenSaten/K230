#!/usr/bin/env python3
"""Matched SDL simulator captures for the Neon text-colour refinement.

Usage: neon_text_captures.py SHELL_BIN OUTPUT_DIR [theme ...]

For each theme (default: neon doors), rotation, text size and mode, starts
the simulator with a fresh state directory and captures the screens the owner
reported as too white: the launcher clock, an opened folder, a Favorites
picker, Settings, System, Controls and the lock clock. Files are named
<theme>-<rotation>-<size>-<mode>-<screen>.png. Run it under the fixed wall
clock fixture (tools/design/neon_capture_clock.c) so the clock matches.
Each frame gets its shell.audit layout findings beside it (.audit.json).
Uses only local shell IPC; no radio or network helper is started.
"""
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def call(run, method, params=None):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(8)
        conn.connect(str(run / 'shell.sock'))
        data = json.dumps(dict(id=1, method=method, params=params or {})).encode()
        conn.sendall(struct.pack('!I', len(data)) + data)

        def read(n):
            result = b''
            while len(result) < n:
                part = conn.recv(n - len(result))
                if not part:
                    raise RuntimeError('shell closed the connection')
                result += part
            return result

        reply = json.loads(read(struct.unpack('!I', read(4))[0]))
        assert 'error' not in reply, reply
        return reply['result']


def main():
    binary = str(Path(sys.argv[1]).resolve())
    output = Path(sys.argv[2]).resolve()
    themes = sys.argv[3:] or ['neon', 'doors']
    output.mkdir(parents=True, exist_ok=True)
    count = 0
    for theme in themes:
        for rotation in ('portrait', 'landscape'):
            for size in ('small', 'medium', 'large'):
                with tempfile.TemporaryDirectory(prefix='neon-text-') as directory:
                    run = Path(directory)
                    env = os.environ | dict(SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=directory,
                                            POCKETOS_CONFIG_DIR=directory, POCKETOS_STATE_DIR=directory,
                                            POCKETOS_LOG_DIR=directory,
                                            POCKETOS_TEST_SYSFS_ROOT=str(run / 'nosys'),
                                            POCKETOS_EXTERNAL_DISPLAY='0')
                    args = [binary, '--no-lock', '--rotation', rotation, '--text-size', size, '--theme', theme]
                    process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL)
                    try:
                        for _ in range(100):
                            if (run / 'shell.sock').exists():
                                break
                            assert process.poll() is None, 'shell exited at startup'
                            time.sleep(.05)
                        time.sleep(.3)
                        for mode in ('normal', 'outdoor', 'night'):
                            call(run, 'shell.theme', dict(theme=theme, mode=mode))
                            info = call(run, 'shell.info')
                            assert info['theme'] == theme, info['theme']
                            stem = f'{theme}-{rotation}-{size}-{mode}'

                            def shot(screen):
                                time.sleep(.2)
                                call(run, 'shell.screenshot', dict(path=str(output / f'{stem}-{screen}.png')))
                                # The layout audit beside each frame: a colour
                                # change must not add a clipped or cut label.
                                (output / f'{stem}-{screen}.audit.json').write_text(
                                    json.dumps(call(run, 'shell.audit'), sort_keys=True))

                            call(run, 'shell.home')
                            call(run, 'shell.folder', dict(id=''))
                            shot('launcher')
                            call(run, 'shell.folder', dict(id='utilities'))
                            shot('folder')
                            call(run, 'shell.folder', dict(id=''))
                            call(run, 'shell.favorite', dict(slot=1, pick=True))
                            shot('favorites')
                            call(run, 'shell.folder', dict(id=''))
                            call(run, 'shell.home')
                            call(run, 'shell.open', dict(id='settings'))
                            shot('settings')
                            call(run, 'shell.open', dict(id='system'))
                            shot('system')
                            call(run, 'shell.home')
                            call(run, 'shell.controls', dict(show=True))
                            shot('controls')
                            call(run, 'shell.controls', dict(show=False))
                            call(run, 'shell.lock')
                            shot('lock')
                            call(run, 'shell.unlock')
                            count += 7
                    finally:
                        process.terminate()
                        process.wait(timeout=10)
                print(f'ok {theme} {rotation} {size}', flush=True)
    print(f'neon_text_captures: {count} captures')


if __name__ == '__main__':
    main()
