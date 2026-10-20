#!/usr/bin/env python3
"""Focused Neon selection, persistence and live-screen proof in the SDL shell.

Usage: neon_shell_test.py SHELL_BIN [capture-directory]
Uses only local shell IPC and absent services; does not start radio/network helpers.

SPDX-License-Identifier: Apache-2.0
"""
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
    output = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    if output:
        output.mkdir(parents=True, exist_ok=True)
    checks = 0
    cases = [(rotation, size, None) for rotation in ('portrait', 'landscape')
             for size in ('small', 'medium', 'large')]
    cases.append(('landscape', 'large', '1920x1080'))
    for rotation, size, hdmi in cases:
        name = f'{"hdmi" if hdmi else rotation}-{size}'
        with tempfile.TemporaryDirectory(prefix='neon-shell-') as directory:
            run = Path(directory)
            env = os.environ | dict(SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=directory,
                                    POCKETOS_CONFIG_DIR=directory, POCKETOS_STATE_DIR=directory,
                                    POCKETOS_LOG_DIR=directory, POCKETOS_TEST_SYSFS_ROOT=str(run / 'nosys'),
                                    POCKETOS_EXTERNAL_DISPLAY=hdmi or '0')
            args = [binary, '--no-lock', '--rotation', rotation, '--text-size', size]

            def start():
                process = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for _ in range(100):
                    if (run / 'shell.sock').exists():
                        break
                    assert process.poll() is None, 'shell exited at startup'
                    time.sleep(.05)
                time.sleep(.3)
                return process

            def tap(text):
                call(run, 'shell.tap', dict(text=text))
                time.sleep(.25)

            def shot(screen):
                if output:
                    call(run, 'shell.screenshot', dict(path=str(output / f'{name}-{screen}.png')))

            process = start()
            try:
                assert call(run, 'shell.info')['theme'] == 'doors'
                call(run, 'shell.open', dict(id='settings'))
                tap('Appearance')
                tap('Neon')
                assert call(run, 'shell.info')['theme'] == 'neon'
                assert 'theme=neon\n' in (run / 'settings.conf').read_text()
                shot('appearance')
                call(run, 'shell.open', dict(id='settings'))
                time.sleep(.15)
                shot('settings')
                tap('Display')
                shot('display')
                call(run, 'shell.open', dict(id='settings'))
                time.sleep(.15)
                tap('Network')
                shot('network-disabled-warning')
                call(run, 'shell.home')
                time.sleep(.15)
                shot('launcher')
                call(run, 'shell.key', dict(code=1))
                time.sleep(.15)
                assert call(run, 'shell.info')['launcher']['focus_shown']
                shot('launcher-focus')
                call(run, 'shell.controls', dict(show=True))
                time.sleep(.15)
                shot('controls')
                call(run, 'shell.controls', dict(show=False))
                call(run, 'shell.lock')
                time.sleep(.15)
                shot('lock')
                # The same shared styles repaint even while the lock is up.
                call(run, 'shell.theme', dict(theme='doors'))
                call(run, 'shell.theme', dict(theme='neon'))
                call(run, 'shell.unlock')
                checks += 4
            finally:
                process.terminate()
                process.wait(timeout=10)
            process = start()
            try:
                assert call(run, 'shell.info')['theme'] == 'neon'
                call(run, 'shell.open', dict(id='settings'))
                tap('Appearance')
                tap('Doors')
                assert call(run, 'shell.info')['theme'] == 'doors'
                assert 'theme=doors\n' in (run / 'settings.conf').read_text()
                # Destroyed app callbacks must be gone before a new selection.
                call(run, 'shell.home')
                call(run, 'shell.theme', dict(theme='neon'))
                call(run, 'shell.open', dict(id='settings'))
                tap('Appearance')
                assert call(run, 'shell.info')['theme'] == 'neon'
                checks += 4
            finally:
                process.terminate()
                process.wait(timeout=10)
            print(f'ok {name}: UI selection, persistence, restart, live changes, reopen and switch back', flush=True)
    print(f'neon_shell_test: {checks} checks, 0 failures')


if __name__ == '__main__':
    main()
