#!/usr/bin/env python3
"""Live Neon round trips must restore the other themes exactly.

Usage: neon_live_switch.py SHELL_BIN OUTPUT_DIR

For Doors and Ice, in Normal and Outdoor, portrait and landscape: capture the
launcher, a folder, Settings, System and the lock screen; switch live to Neon
and through its Normal, Outdoor and Night modes on each screen; switch back
and capture again. The two frames must be byte-identical PNGs (the encoder is
deterministic, so equal bytes mean equal pixels). Run under the fixed wall
clock fixture (tools/design/neon_capture_clock.c).
"""
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from neon_text_captures import call  # noqa: E402

SCREENS = {
    'launcher': [('shell.home', {}), ('shell.folder', dict(id=''))],
    'folder': [('shell.folder', dict(id='utilities'))],
    'settings': [('shell.open', dict(id='settings'))],
    'system': [('shell.open', dict(id='system'))],
    'lock': [('shell.home', {}), ('shell.lock', {})],
}


def main():
    binary = str(Path(sys.argv[1]).resolve())
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    failures = checks = 0
    for theme in ('doors', 'ice'):
        for rotation in ('portrait', 'landscape'):
            with tempfile.TemporaryDirectory(prefix='neon-live-') as directory:
                run = Path(directory)
                env = os.environ | dict(SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=directory,
                                        POCKETOS_CONFIG_DIR=directory, POCKETOS_STATE_DIR=directory,
                                        POCKETOS_LOG_DIR=directory, POCKETOS_TEST_SYSFS_ROOT=str(run / 'nosys'),
                                        POCKETOS_EXTERNAL_DISPLAY='0')
                process = subprocess.Popen([binary, '--no-lock', '--rotation', rotation, '--theme', theme],
                                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    for _ in range(100):
                        if (run / 'shell.sock').exists():
                            break
                        assert process.poll() is None, 'shell exited at startup'
                        time.sleep(.05)
                    time.sleep(.3)
                    for mode in ('normal', 'outdoor'):
                        for screen, steps in SCREENS.items():
                            call(run, 'shell.unlock')
                            call(run, 'shell.theme', dict(theme=theme, mode=mode))
                            for method, params in steps:
                                call(run, method, params)
                            name = f'{theme}-{rotation}-{mode}-{screen}'
                            time.sleep(.2)
                            call(run, 'shell.screenshot', dict(path=str(output / f'{name}-start.png')))
                            for neon_mode in ('normal', 'outdoor', 'night'):
                                call(run, 'shell.theme', dict(theme='neon', mode=neon_mode))
                                time.sleep(.1)
                            call(run, 'shell.theme', dict(theme=theme, mode=mode))
                            if screen == 'settings':
                                # An open Settings keeps its Appearance summary
                                # text from before an IPC theme change (seen at
                                # 56aefe5 too); reopen it so only styles compare.
                                call(run, 'shell.home')
                                call(run, 'shell.open', dict(id='settings'))
                            time.sleep(.2)
                            call(run, 'shell.screenshot', dict(path=str(output / f'{name}-return.png')))
                            same = (output / f'{name}-start.png').read_bytes() == \
                                (output / f'{name}-return.png').read_bytes()
                            checks += 1
                            failures += 0 if same else 1
                            print(f'{"ok  " if same else "FAIL"} {name}: exact pixels after live Neon round trip',
                                  flush=True)
                finally:
                    process.terminate()
                    process.wait(timeout=10)
    print(f'neon_live_switch: {checks} checks, {failures} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
