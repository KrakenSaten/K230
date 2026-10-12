#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Drive the real shell/helper with synthetic data, pointer events and keys."""
import json
import os
from pathlib import Path
import struct
import socket
import subprocess
import time
import tempfile
import sys

root = Path.cwd()
output = root/'work/ninjaone-ui'
output.mkdir(parents=True, exist_ok=True)
helper = root/'tools/ninjaone/pos-ninjaone'
shell = Path(os.environ.get('SHELL_BIN', str(root/'out/cloud-shell/pocketos-shell')))
checks = 0


def run(orientation, size):
    global checks
    case = Path(tempfile.mkdtemp(prefix=f'{orientation}-{size}-', dir=output))
    for sub in ('runtime', 'config', 'state', 'log'): (case/sub).mkdir(exist_ok=True)
    scenario = case/'scenario'; scenario.write_text('demo')
    calls = case/'calls'; calls.write_text('')
    wrapper = case/'helper.sh'
    wrapper.write_text(f'#!/bin/sh\nprintf x >> "{calls}"\nexec "{helper}" "$1" --fake "$(cat "{scenario}")"\n')
    wrapper.chmod(0o755)
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'POCKETOS_NINJAONE_HELPER': str(wrapper),
           'POCKETOS_NINJAONE_BACKEND': 'fake', **{f'POCKETOS_{k.upper()}_DIR': str(case/k)
                                                  for k in ('runtime', 'config', 'state', 'log')}}
    log = open(case/'run.log', 'w')
    process = subprocess.Popen([str(shell), '--rotation', orientation, '--text-size', size,
                                '--open', 'ninjaone', '--no-lock'], env=env, stdout=log, stderr=log)

    def call(method, **params):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3); sock.connect(str(case/'runtime/shell.sock'))
            payload = json.dumps({'id': 1, 'method': method, 'params': params}).encode()
            sock.sendall(struct.pack('!I', len(payload)) + payload)
            def read(n):
                data = b''
                while len(data) < n:
                    chunk = sock.recv(n-len(data))
                    if not chunk: raise RuntimeError('closed IPC')
                    data += chunk
                return data
            return json.loads(read(struct.unpack('!I', read(4))[0]))

    def tap(text):
        answer = call('shell.tap', text=text)
        assert 'error' not in answer, (text, answer)
        time.sleep(.25)

    def contains(text):
        return 'error' not in call('shell.tap', text=text)

    def key(code):
        answer = call('shell.key', code=code)
        assert 'error' not in answer
        time.sleep(.08)

    def audit(name):
        answer = call('shell.audit')['result']
        (case/f'{name}.json').write_text(json.dumps(answer, indent=2))
        # Scrollable content outside its viewport is legitimate; drawn labels
        # must not overlap or be truncated at any supported text size.
        assert not answer['count']['overlap'] and not answer['count']['truncated'], answer
        call('shell.screenshot', path=str(case/f'{name}.png'))
        time.sleep(.1)

    try:
        deadline = time.monotonic()+8
        while not (case/'runtime/shell.sock').exists():
            assert process.poll() is None and time.monotonic() < deadline
            time.sleep(.05)
        time.sleep(.6)
        assert call('shell.info')['result']['current'] == 'ninjaone'
        if '--focused' in sys.argv:
            tap('Organizations'); tap('Demo North'); tap('Save local favorite')
            settings = case/'config/settings.conf'
            assert 'n1_d_000000000000000000000001=1' in settings.read_text()
            scenario.write_text('tenant2'); tap('Refresh data'); time.sleep(.5)
            tap('Organizations'); tap('Demo North')
            assert contains('Save local favorite')  # Same numeric ID, independent tenant.
            time.sleep(.25)
            assert 'n1_d_000000000000000000000002=1' in settings.read_text()
            scenario.write_text('renewing'); tap('Refresh data'); time.sleep(.7)
            tap('Overview'); audit('renewed')
            scenario.write_text('expired'); tap('Refresh data'); time.sleep(.5)
            audit('sign-in-again'); assert contains('Demo server / Demo North'); time.sleep(.25)
            tap('Back to list'); tap('Disconnect / Forget credentials'); time.sleep(.5)
            audit('disconnected'); assert contains('Demo server / Demo North'); time.sleep(.25)
            calls_before = calls.read_text(); time.sleep(.4); assert calls.read_text() == calls_before
            call('shell.home'); time.sleep(.2)
            checks += 1
            return
        audit('overview')
        tap('Organizations'); audit('organizations')
        # From the tapped Organizations control, Tab visits Alerts, Refresh data, Disconnect,
        # then the search textarea through the shared keyboard stream.
        for _ in range(4): key(6)
        for code in (24, 43, 37, 47, 35): key(code)  # north (pos_keymap.c)
        assert not contains('Demo South')
        tap('Demo North'); audit('organization')
        tap('Save local favorite')
        assert 'n1_d_000000000000000000000001=1' in (case/'config/settings.conf').read_text()
        tap('Demo server'); audit('detail')
        # API values are checked in ninjaone_test; detail/stale text is reviewed
        # from screenshots, avoiding taps on noninteractive labels.
        tap('Back to list'); tap('All organizations')
        tap('Alerts')
        for _ in range(3): key(6)
        for code in (45, 24, 33, 24, 43, 39, 24): key(code)  # unknown
        assert not contains('Demo server / MAJOR / Demo: agent disconnected')
        tap('Demo laptop / Unknown / Demo: disk space condition triggered')
        audit('unknown-detail')
        tap('Back to list')
        tap('Overview')
        scenario.write_text('network'); tap('Refresh data'); time.sleep(.5)
        audit('stale')
        tap('Demo server / Demo North')
        audit('stale-detail')
        call('shell.home'); time.sleep(.3)
        before = calls.read_text(); time.sleep(.3); assert calls.read_text() == before
        # Actual launcher membership, not just an incremented count.
        call('shell.folder', id='apps'); time.sleep(.2)
        info = call('shell.info')['result']
        assert info['launcher']['apps'] == 28
        assert 'ninjaone' in [cell['id'] for cell in info['launcher']['cells']]
        checks += 1
    finally:
        process.terminate(); process.wait(timeout=5); log.close()


for orientation in (('portrait',) if '--focused' in sys.argv else ('portrait', 'landscape')):
    for size in (('medium',) if '--focused' in sys.argv else ('small', 'medium', 'large')):
        run(orientation, size)
print(f'ninjaone_shell_test: {checks} focused auth/tenant/disconnect cases passed' if '--focused' in sys.argv else f'ninjaone_shell_test: {checks} orientation/text-size cases passed')
