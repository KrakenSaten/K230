#!/bin/bash
# The power key in the running shell (ui/shell/shell_evkey.c,
# shell_power_menu.c, shell.c's two hooks), fed through a FIFO named by
# POCKETOS_POWER_KEY_DEVICE with the kernel's own struct input_event:
#
#   - a short press puts a lit screen out and wakes a dark one, and leaves the
#     lock exactly as it was, at the launcher and inside an app;
#   - a hold of POWER_KEY_LONG_MS opens the power menu while still held, once;
#     the release after it does nothing; held while dark it wakes first;
#   - Cancel, a tap outside and a short press close the menu; Restart with no
#     sysd says why and keeps the menu usable;
#   - auto-repeats, duplicate presses and stray releases are ignored, a hold
#     just under the threshold is a short press, five quick presses are five;
#   - input lost mid-press (the device gone, dropped events) ends the press
#     with no action and no stuck hold; the device is opened again;
#   - after a rotation restart (exec in place) the key is read again, through
#     exactly one descriptor; POCKETOS_POWER_KEY_DEVICE=none turns it off.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell, with test hooks).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

python3 - "$SHELL_BIN" "$WORK" <<'PY'
import json, os, socket, struct, subprocess, sys, tempfile, time

shell, work = sys.argv[1], sys.argv[2]
failed = 0
checks = 0
EV_SYN, EV_KEY, SYN_REPORT, SYN_DROPPED, KEY_POWER = 0, 1, 0, 3, 116
LONG_S = 1.0


def check(what, ok):
    global failed, checks
    checks += 1
    if ok:
        print('ok   ' + what)
    else:
        failed += 1
        print('FAIL ' + what)


class Key:
    """The writing end of the FIFO the shell reads as its power key."""

    def __init__(self, path):
        self.path = path
        if not os.path.exists(path):
            os.mkfifo(path)
        self.fd = None
        self.open()

    def open(self):
        # O_RDWR: never blocks waiting for the reader, and the reader never
        # sees an end of input while this end is open.
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def ev(self, type_, code, value):
        t = time.time()
        os.write(self.fd, struct.pack('qqHHi', int(t), int((t % 1) * 1e6), type_, code, value))

    def key(self, value):
        self.ev(EV_KEY, KEY_POWER, value)
        self.ev(EV_SYN, SYN_REPORT, 0)

    def tap(self, held=0.15):
        self.key(1)
        time.sleep(held)
        self.key(0)
        time.sleep(0.25)


class Shell:
    def __init__(self, key_dev, *args):
        tmp = tempfile.mkdtemp(dir=work)
        self.run, self.logd, state, cfg = (os.path.join(tmp, d) for d in ('run', 'log', 'state', 'cfg'))
        for d in (self.run, self.logd, state, cfg):
            os.makedirs(d)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=self.run, POCKETOS_LOG_DIR=self.logd,
                   POCKETOS_CONFIG_DIR=cfg, POCKETOS_STATE_DIR=state, HOME=os.path.join(state, 'home'),
                   POCKETOS_POWER_KEY_DEVICE=key_dev, POCKETOS_BACK_KEY_DEVICE='none')
        self.p = subprocess.Popen([shell, '--no-lock'] + list(args), env=env,
                                  stdout=open(os.path.join(self.logd, 'out'), 'w'), stderr=subprocess.STDOUT)
        self.sock = os.path.join(self.run, 'shell.sock')
        self.wait_up()

    def wait_up(self):
        for _ in range(150):
            try:
                if self.call('shell.info').get('result'):
                    return True
            except (OSError, RuntimeError, ValueError):
                pass
            time.sleep(0.1)
        return False

    def call(self, method, params=None):
        body = json.dumps({'id': 1, 'method': method, **({'params': params} if params is not None else {})}).encode()
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(self.sock)
        s.sendall(struct.pack('>I', len(body)) + body)

        def rd(n):
            b = b''
            while len(b) < n:
                c = s.recv(n - len(b))
                if not c:
                    raise RuntimeError('closed')
                b += c
            return b
        r = json.loads(rd(struct.unpack('>I', rd(4))[0]))
        s.close()
        return r

    def result(self, method, params=None):
        return self.call(method, params).get('result') or {}

    def info(self):
        return self.result('shell.info')

    def pw(self):
        return self.info()['power']

    def pk(self):
        return self.pw()['key']

    def stop(self):
        self.p.terminate()
        try:
            rc = self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = -9
        return rc

    def log(self):
        try:
            return open(os.path.join(self.logd, 'shell.log')).read()
        except OSError:
            return ''


fifo = os.path.join(work, 'pwrkey')
k = Key(fifo)
s = Shell(fifo)

# ---- found and read ----
p = s.pk()
check('the named device is read: enabled, connected, nothing pressed',
      p['enabled'] and p['connected'] and p['device'] == fifo and p['state'] == 'up' and p['long_ms'] == 1000)
check('nothing grabbed or changed at start: screen on, no menu', s.pw()['screen'] == 'on' and not p['menu'])

# ---- short press: off, on ----
k.tap()
i = s.info()
check('a short press puts the screen out', i['power']['screen'] == 'off' and i['power']['screen_offs'] == 1 and
      i['power']['key']['shorts'] == 1)
check('and changes nothing under it', i['current'] == 'home' and i['lock']['locked'] is False)
k.tap()
check('the next short press wakes it', s.pw()['screen'] == 'on' and s.pk()['shorts'] == 2)

# ---- the lock is kept ----
s.result('shell.lock')
check('locked', s.info()['lock']['locked'] is True)
k.tap()
check('locked, short press: dark, still locked', s.pw()['screen'] == 'off' and s.info()['lock']['locked'] is True)
k.tap()
i = s.info()
check('locked, short press: lit, still locked (the wake does not open it)',
      i['power']['screen'] == 'on' and i['lock']['locked'] is True)

# ---- Lock after keeps counting under a screen put out by the key ----
s.result('shell.unlock')
s.result('shell.power', {'auto_lock_s': 60})
k.tap()
check('unlocked, Lock after 1 min, short press: dark, not locked yet',
      s.pw()['screen'] == 'off' and s.info()['lock']['locked'] is False)
time.sleep(62)
i = s.info()
check('a minute later the lock came down under the dark screen, which stayed dark',
      i['lock']['locked'] is True and i['power']['screen'] == 'off')
k.tap()
check('the key wakes it, locked', s.pw()['screen'] == 'on' and s.info()['lock']['locked'] is True)
s.result('shell.power', {'auto_lock_s': 0})

# ---- the hold: the menu while held, once; nothing at release ----
k.key(1)
time.sleep(LONG_S * 0.7)
check('0.7 s held: no menu yet', s.pk()['menu'] is False)
time.sleep(LONG_S * 0.6)
p = s.pk()
check('past 1 s, still held: the menu is open', p['menu'] is True and p['state'] == 'long' and p['longs'] == 1)
time.sleep(1.0)
check('held on: still one menu, one long press', s.pk()['menu_opens'] == 1 and s.pk()['longs'] == 1)
shorts = s.pk()['shorts']
k.key(0)
time.sleep(0.3)
p = s.pk()
i = s.info()
check('the release after the hold does nothing: menu open, screen on, no short press',
      p['menu'] is True and p['shorts'] == shorts and p['state'] == 'up' and i['power']['screen'] == 'on')
check('the menu is over the lock, which stays', i['lock']['locked'] is True)
r = s.call('shell.tap', {'text': 'Cancel'})
time.sleep(0.4)
i = s.info()
check('Cancel closes it; the lock is still up', 'result' in r and i['power']['key']['menu'] is False and
      i['lock']['locked'] is True)
s.result('shell.unlock')

# ---- held while dark: wake, then the menu ----
k.tap()
check('dark again', s.pw()['screen'] == 'off')
k.key(1)
time.sleep(LONG_S + 0.3)
p = s.pw()
check('held while dark: lit, and the menu is open', p['screen'] == 'on' and p['key']['menu'] is True)
k.key(0)
time.sleep(0.3)
s.call('shell.tap', {'x': 8, 'y': 8})
time.sleep(0.4)
check('a tap outside the panel is Cancel', s.pk()['menu'] is False and s.pw()['screen'] == 'on')

# ---- a short press closes it; Restart without sysd ----
k.key(1)
time.sleep(LONG_S + 0.3)
k.key(0)
time.sleep(0.2)
k.tap()
check('a short press with the menu open closes it, the screen stays on',
      s.pk()['menu'] is False and s.pw()['screen'] == 'on')
k.key(1)
time.sleep(LONG_S + 0.3)
k.key(0)
time.sleep(0.2)
r = s.call('shell.tap', {'text': 'Restart'})
time.sleep(0.6)
p = s.pk()
print('     note after Restart with no sysd: %r' % p['menu_note'])
check('Restart with no sysd: refused on the panel, the menu still usable',
      'result' in r and p['menu'] is True and p['menu_note'] != '' and 'Restarting' not in p['menu_note'])
s.call('shell.tap', {'text': 'Cancel'})
time.sleep(0.4)
check('and Cancel still closes it', s.pk()['menu'] is False)

# ---- threshold, repeats, duplicates ----
before = s.pk()
k.tap(held=0.8)
p = s.pk()
check('0.8 s is a short press, not a hold', p['shorts'] == before['shorts'] + 1 and p['longs'] == before['longs'] and
      p['menu'] is False)
k.tap()  # back on
before = s.pk()
k.key(1)
for _ in range(3):
    k.ev(EV_KEY, KEY_POWER, 2)
k.ev(EV_SYN, SYN_REPORT, 0)
k.key(1)
time.sleep(0.1)
k.key(0)
k.key(0)
time.sleep(0.3)
p = s.pk()
check('repeats, a second press and a second release: one short press, five ignored',
      p['shorts'] == before['shorts'] + 1 and p['ignored'] == before['ignored'] + 5 and p['state'] == 'up')
k.tap()  # back on
check('screen on', s.pw()['screen'] == 'on')
before = s.pw()
for _ in range(5):
    k.tap(held=0.1)
p = s.pw()
check('five quick presses: five short presses, the screen ends dark',
      p['key']['shorts'] == before['key']['shorts'] + 5 and p['screen'] == 'off' and
      p['screen_offs'] == before['screen_offs'] + 3 and p['key']['menu'] is False)
k.tap()

# ---- input lost ----
before = s.pk()
k.key(1)
time.sleep(0.2)
k.close()
time.sleep(LONG_S + 0.5)
p = s.pk()
check('the device gone mid-press: no menu, nothing held, the loss counted',
      p['menu'] is False and p['state'] == 'up' and p['connected'] is False and p['lost'] == before['lost'] + 1 and
      p['losses'] == before['losses'] + 1)
check('and no short press from it', p['shorts'] == before['shorts'] and s.pw()['screen'] == 'on')
k.open()
time.sleep(3.5)
p = s.pk()
check('opened again once it is back', p['connected'] is True and p['opens'] == before['opens'] + 1)
k.tap()
check('and it works again', s.pw()['screen'] == 'off')
k.tap()

before = s.pk()
k.key(1)
k.ev(EV_SYN, SYN_DROPPED, 0)
k.key(0)  # dropped with the rest, up to the report
time.sleep(LONG_S + 0.4)
p = s.pk()
check('events dropped mid-press: no menu, nothing held, no short press',
      p['menu'] is False and p['state'] == 'up' and p['shorts'] == before['shorts'] and p['longs'] == before['longs'])
k.tap()
check('the next press counts', s.pw()['screen'] == 'off')
k.tap()

# ---- inside an app ----
s.result('shell.open', {'id': 'calculator'})
time.sleep(0.4)
k.tap()
check('in an app, a short press: dark, the app still open',
      s.pw()['screen'] == 'off' and s.info()['current'] == 'calculator')
k.tap()
k.key(1)
time.sleep(LONG_S + 0.3)
k.key(0)
time.sleep(0.3)
i = s.info()
check('in an app, the hold: the menu over it', i['power']['key']['menu'] is True and i['current'] == 'calculator')
s.call('shell.tap', {'text': 'Cancel'})
time.sleep(0.4)
check('Cancel: back to the app as it was', s.info()['current'] == 'calculator' and s.pk()['menu'] is False)
s.result('shell.home')

# ---- under a hold (an app that keeps the screen awake) ----
s.result('shell.open', {'id': 'video'})
time.sleep(0.5)
check('Video in front holds the device awake', s.info()['current'] == 'video' and s.pw()['held'] is True)
k.tap()
time.sleep(2.5)
check('a short press puts it out anyway, and the hold does not bring it back',
      s.pw()['screen'] == 'off' and s.info()['current'] == 'video')
k.tap()
check('the next press wakes it', s.pw()['screen'] == 'on')
s.result('shell.home')

# ---- a rotation restart (exec in place) ----
opens = s.pk()['opens']
pid = s.p.pid
s.call('shell.rotation', {'mode': 'landscape'})
time.sleep(1.5)
check('the shell came back after the rotation', s.wait_up())
time.sleep(0.5)
p = s.pk()
fds = [f for f in os.listdir('/proc/%d/fd' % pid) if os.readlink('/proc/%d/fd/%s' % (pid, f)) == fifo]
print('     rotation log: %s' % ('restarting in place' in s.log()))
check('after the restart the key is read again, through one descriptor',
      'restarting in place' in s.log() and p['connected'] is True and len(fds) == 1)
k.tap()
check('and works', s.pw()['screen'] == 'off')
k.tap()
check('the shell kept running and logged no fault', s.p.poll() is None and ' ERROR ' not in s.log())
check('a clean stop', s.stop() == 0)
k.close()

# ---- turned off; missing ----
s = Shell('none')
p = s.pk()
check('POCKETOS_POWER_KEY_DEVICE=none: off, nothing open', p['enabled'] is False and p['connected'] is False)
s.stop()
s = Shell(os.path.join(work, 'no-such-device'))
p = s.pk()
check('a device that is not there: the shell runs, the key is not connected',
      p['enabled'] is True and p['connected'] is False and s.p.poll() is None)
check('said once in the log', s.log().count('power key: no device found') == 1)
time.sleep(3.5)
check('and not again on every look', s.log().count('power key: no device found') == 1)
s.stop()

print('power_key_shell_test.sh: %d checks, %d failure(s)' % (checks, failed))
sys.exit(1 if failed else 0)
PY
failed=$?
echo "power_key_shell_test.sh: exit $failed"
exit $failed
