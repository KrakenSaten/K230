#!/usr/bin/env python3
"""Capture the two screens of the approved Neon Arcade reference.

Usage: neon_arcade_capture.py SHELL_BIN OUTPUT_DIR NAME

Writes launcher-NAME.png (portrait, Normal, Large text, launcher root with
keyboard focus on the first cell) and settings-NAME.png (landscape, Outdoor,
Large text, Settings > Display), each with its shell.audit findings beside
it. These are the content and state of docs/design/neon/arcade-selected/
reference/. Run under the fixed wall clock fixture
(tools/design/neon_capture_clock.c). Uses only local shell IPC.
"""
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from neon_text_captures import call  # noqa: E402


def session(binary, out, name, rotation, mode, steps, shot):
    with tempfile.TemporaryDirectory(prefix='neon-arcade-') as d:
        run = Path(d)
        env = os.environ | dict(SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=d, POCKETOS_CONFIG_DIR=d,
                                POCKETOS_STATE_DIR=d, POCKETOS_LOG_DIR=d,
                                POCKETOS_TEST_SYSFS_ROOT=str(run / 'nosys'), POCKETOS_EXTERNAL_DISPLAY='0')
        p = subprocess.Popen([binary, '--no-lock', '--rotation', rotation, '--text-size', 'large',
                              '--theme', 'neon', '--mode', mode], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if (run / 'shell.sock').exists():
                    break
                assert p.poll() is None, 'shell exited at startup'
                time.sleep(.05)
            time.sleep(.3)
            assert call(run, 'shell.info')['theme'] == 'neon'
            for method, params in steps:
                call(run, method, params)
                time.sleep(.25)
            call(run, 'shell.screenshot', dict(path=str(out / f'{shot}-{name}.png')))
            (out / f'{shot}-{name}.audit.json').write_text(json.dumps(call(run, 'shell.audit'), sort_keys=True))
        finally:
            p.terminate()
            p.wait(timeout=10)


def main():
    binary, out, name = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    session(binary, out, name, 'portrait', 'normal',
            [('shell.home', {}), ('shell.folder', dict(id='')), ('shell.key', dict(code=1))], 'launcher')
    session(binary, out, name, 'landscape', 'outdoor',
            [('shell.open', dict(id='settings')), ('shell.tap', dict(text='Display'))], 'settings')
    print('captured', name)


if __name__ == '__main__':
    main()
