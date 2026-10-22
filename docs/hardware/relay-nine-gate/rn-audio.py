#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Unit B, Relay Nine test session only: run this kit's pos-audio-effects the
way the game does (one SOCK_SEQPACKET client on stdin/stdout, volume as the
argument) with no shell involved, play every cue in order, then close.
Samples the playback PCM status meanwhile: a second trigger_time while
RUNNING means the stream was restarted; the helper also reports its xruns.
  rn-audio.py VOLUME [GAP_MS]     VOLUME 0 = muted (the helper opens nothing)
Prints one line per cue and a summary; exit 0 when the helper behaved.
"""
import os
import socket
import struct
import subprocess
import sys
import time

KIT = os.path.dirname(os.path.abspath(__file__))
STATUS = '/proc/asound/card0/pcm0p/sub0/status'
CUES = ['carbine shot', 'impact', 'empty', 'carbine reload', 'wind-up', 'attack', 'door',
        'pickup', 'shotgun shot', 'shotgun reload']


def pcm():
    try:
        with open(STATUS) as f:
            lines = f.read().split('\n')
    except OSError:
        return 'unreadable', None
    state = lines[0].split(':', 1)[-1].strip()
    trig = next((l.split(':', 1)[1].strip() for l in lines if l.startswith('trigger_time')), None)
    return state, trig


def main(argv):
    volume = int(argv[1])
    gap = int(argv[2]) if len(argv) > 2 else 900
    ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    t0 = time.monotonic()
    helper = subprocess.Popen([os.path.join(KIT, 'pos-audio-effects'), str(volume)], stdin=theirs,
                              stdout=theirs, stderr=subprocess.PIPE, pass_fds=())
    theirs.close()
    ours.settimeout(3)
    hello = ours.recv(64).decode(errors='replace').strip()
    print('helper %d says %r after %.0f ms' % (helper.pid, hello, (time.monotonic() - t0) * 1000), flush=True)
    states, triggers = set(), set()
    for kind, name in enumerate(CUES):
        stamp = int(time.monotonic() * 1000) & 0xffffffff
        ours.send(bytes([kind, 0, 0, 0]) + struct.pack('<I', stamp))
        end = time.monotonic() + gap / 1000
        while time.monotonic() < end:
            s, t = pcm()
            states.add(s)
            if t and s == 'RUNNING':
                triggers.add(t)
            time.sleep(0.05)
        print('cue %d %s sent; pcm %s' % (kind, name, pcm()[0]), flush=True)
    t1 = time.monotonic()
    ours.close()  # EOF: the helper drains nothing further and exits
    try:
        rc = helper.wait(timeout=3)
    except subprocess.TimeoutExpired:
        helper.kill()
        rc = 'killed'
    err = helper.stderr.read().decode(errors='replace').strip()
    after = pcm()[0]
    recovery = os.path.exists('/run/pocketos/audio.recovery')
    print('summary volume=%d hello=%s exit=%s exit_ms=%.0f pcm_states=%s stream_starts=%d pcm_after=%s '
          'recovery_record=%s stderr=%r' % (volume, hello, rc, (time.monotonic() - t1) * 1000,
                                             '/'.join(sorted(states)), len(triggers), after, recovery, err))
    want = 'MUTED' if volume == 0 else 'READY'
    xruns = int(err.split()[-1]) if err.startswith('xruns') else (0 if volume == 0 else -1)
    ok = hello == want and rc == 0 and after == 'closed' and not recovery and len(triggers) <= 1 and xruns == 0
    print('AUDIO: %s' % ('OK' if ok else 'CHECK'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
