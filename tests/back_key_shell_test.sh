#!/bin/bash
# The BOOT key in the running shell (ui/shell/shell_evkey.c, shell.c's BOOT
# key hooks), fed through a FIFO named by POCKETOS_BACK_KEY_DEVICE with the
# kernel's own struct input_event (KEY_BACK, as gpio-keys reports SW3), and a
# second FIFO as the power key, which darkens the screen and opens the menu:
#
#   - a short press is Back: a Settings page goes back to the list, then the
#     app is left; a press 1 ms-ish under the threshold is still Back;
#   - a hold of POWER_KEY_LONG_MS is Home, while still held, once; the release
#     after it is not Back as well;
#   - with the screen dark a press wakes it and is consumed - neither its
#     release nor its hold navigates;
#   - the lock screen stays: neither press opens or leaves anything under it;
#   - with the power menu open a press only closes the menu;
#   - repeats are ignored; input lost mid-press (the device gone, dropped
#     events) ends the press with no action, and the device is opened again;
#   - POCKETOS_BACK_KEY_DEVICE=none turns it off.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell, with test hooks).
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
EV_SYN, EV_KEY, SYN_REPORT, SYN_DROPPED, KEY_POWER, KEY_BACK = 0, 1, 0, 3, 116, 158
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
    """The writing end of a FIFO the shell reads as one board key."""

    def __init__(self, path, code):
        self.path = path
        self.code = code
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
        self.ev(EV_KEY, self.code, value)
        self.ev(EV_SYN, SYN_REPORT, 0)

    def tap(self, held=0.15):
        self.key(1)
        time.sleep(held)
        self.key(0)
        time.sleep(0.3)

    def hold(self, seconds=LONG_S + 0.3):
        self.key(1)
        time.sleep(seconds)


class Shell:
    def __init__(self, back_dev, power_dev, *args):
        tmp = tempfile.mkdtemp(dir=work)
        self.run, self.logd, state, cfg = (os.path.join(tmp, d) for d in ('run', 'log', 'state', 'cfg'))
        for d in (self.run, self.logd, state, cfg):
            os.makedirs(d)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=self.run, POCKETOS_LOG_DIR=self.logd,
                   POCKETOS_CONFIG_DIR=cfg, POCKETOS_STATE_DIR=state, HOME=os.path.join(state, 'home'),
                   POCKETOS_BACK_KEY_DEVICE=back_dev, POCKETOS_POWER_KEY_DEVICE=power_dev)
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

    def bk(self):
        return self.info()['back_key']

    def screen(self):
        return self.info()['power']['screen']

    def cur(self):
        return self.info()['current']

    def menu(self):
        return self.info()['power']['key']['menu']

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


def open_app(s, app_id):
    s.result('shell.open', {'id': app_id})
    time.sleep(0.5)
    return s.cur() == app_id


back_fifo = os.path.join(work, 'backkey')
power_fifo = os.path.join(work, 'pwrkey')
b = Key(back_fifo, KEY_BACK)
pw = Key(power_fifo, KEY_POWER)
s = Shell(back_fifo, power_fifo)

# ---- found and read ----
p = s.bk()
check('the named device is read: enabled, connected, nothing pressed',
      p['enabled'] and p['connected'] and p['device'] == back_fifo and p['state'] == 'up' and p['long_ms'] == 1000)
check('the power key is read beside it', s.info()['power']['key']['connected'] is True)

# ---- short press: Back, the app's own first ----
check('Settings open', open_app(s, 'settings'))
s.call('shell.tap', {'text': 'Display'})
time.sleep(0.4)
check('on its Display page', s.info().get('title') == 'Display')
b.tap()
i = s.info()
check('a short press is Back: Settings is on its list again, still open',
      i['current'] == 'settings' and i.get('title') != 'Display' and i['back_key']['shorts'] == 1)
b.tap()
check('the next short press leaves the app', s.cur() == 'home' and s.bk()['shorts'] == 2)
check('Back is logged as the shared hardware action', 'action back from boot key: done' in s.log())

check('Calculator open', open_app(s, 'calculator'))
b.tap(held=0.85)
check('0.85 s is still a short press: Back', s.cur() == 'home' and s.bk()['longs'] == 0)

# ---- the hold: Home while held, once; the release is not Back ----
check('Settings open again', open_app(s, 'settings'))
s.call('shell.tap', {'text': 'Display'})
time.sleep(0.4)
before = s.bk()
b.key(1)
time.sleep(LONG_S * 0.7)
check('0.7 s held: nothing yet', s.cur() == 'settings' and s.info().get('title') == 'Display')
time.sleep(LONG_S * 0.6)
p = s.bk()
check('past 1 s, still held: Home - the launcher, not the Settings list',
      s.cur() == 'home' and p['state'] == 'long' and p['longs'] == before['longs'] + 1)
time.sleep(1.0)
check('held on: still one long press', s.bk()['longs'] == before['longs'] + 1)
log_backs = s.log().count('action back from boot key')
b.key(0)
time.sleep(0.4)
p = s.bk()
check('the release after the hold does nothing: no short press, no Back',
      p['shorts'] == before['shorts'] and p['state'] == 'up' and s.log().count('action back from boot key') == log_backs)
check('Home is logged as the shared hardware action', 'action home from boot key: done' in s.log())

# Home from an app's deepest page, then a new short press works normally.
check('Calculator open', open_app(s, 'calculator'))
b.tap()
check('after a hold, the next short press is Back again', s.cur() == 'home')

# ---- a dark screen: wake, consume ----
check('Calculator open for the dark-screen checks', open_app(s, 'calculator'))
pw.tap()
check('the power key put the screen out', s.screen() == 'off')
before = s.bk()
b.tap()
p = s.bk()
check('dark, short press: lit, the app still open (no Back)',
      s.screen() == 'on' and s.cur() == 'calculator' and p['shorts'] == before['shorts'] and
      p['swallowed'] == before['swallowed'] + 1)
pw.tap()
check('dark again', s.screen() == 'off')
before = s.bk()
b.hold()
check('dark, held past 1 s: lit at the press, and no Home while held',
      s.screen() == 'on' and s.cur() == 'calculator' and s.bk()['longs'] == before['longs'])
b.key(0)
time.sleep(0.4)
p = s.bk()
check('and its release does nothing either', s.cur() == 'calculator' and p['shorts'] == before['shorts'] and
      p['state'] == 'up')
b.tap()
check('the next press, lit, is Back', s.cur() == 'home')

# ---- the lock screen stays ----
check('Calculator open for the lock checks', open_app(s, 'calculator'))
s.result('shell.lock')
check('locked', s.info()['lock']['locked'] is True)
b.tap()
i = s.info()
check('locked, short press: still locked, the app under it not left',
      i['lock']['locked'] is True and i['current'] == 'calculator')
b.hold()
b.key(0)
time.sleep(0.4)
i = s.info()
check('locked, hold: still locked, no Home', i['lock']['locked'] is True and i['current'] == 'calculator')
check('refused, and said so', 'action home from boot key: refused' in s.log() and
      'action back from boot key: refused' in s.log())
pw.tap()
check('locked and dark', s.screen() == 'off' and s.info()['lock']['locked'] is True)
b.tap()
i = s.info()
check('locked and dark, a press: lit, still locked (the wake does not open it)',
      i['power']['screen'] == 'on' and i['lock']['locked'] is True and i['current'] == 'calculator')
s.result('shell.unlock')
s.result('shell.home')
time.sleep(0.3)

# ---- the power menu ----
check('Calculator open for the menu checks', open_app(s, 'calculator'))
pw.hold()
pw.key(0)
time.sleep(0.3)
check('the power menu is open', s.menu() is True)
b.tap()
check('a short press closes the menu only: the app still open', s.menu() is False and s.cur() == 'calculator')
pw.hold()
pw.key(0)
time.sleep(0.3)
b.hold()
b.key(0)
time.sleep(0.4)
check('a hold with the menu open closes it only: no Home', s.menu() is False and s.cur() == 'calculator')
b.tap()
check('and then Back works', s.cur() == 'home')

# ---- repeats, a stray release ----
check('Settings open for the repeat checks', open_app(s, 'settings'))
before = s.bk()
b.key(1)
for _ in range(3):
    b.ev(EV_KEY, KEY_BACK, 2)
b.ev(EV_SYN, SYN_REPORT, 0)
time.sleep(0.1)
b.key(0)
b.key(0)
time.sleep(0.4)
p = s.bk()
check('three repeats and a second release: one Back, four ignored',
      p['shorts'] == before['shorts'] + 1 and p['ignored'] == before['ignored'] + 4 and s.cur() == 'home')

# ---- input lost ----
check('Calculator open for the loss checks', open_app(s, 'calculator'))
before = s.bk()
b.key(1)
time.sleep(0.2)
b.close()
time.sleep(LONG_S + 0.5)
p = s.bk()
check('the device gone mid-press: no Home, nothing held, the loss counted',
      s.cur() == 'calculator' and p['state'] == 'up' and p['connected'] is False and
      p['lost'] == before['lost'] + 1 and p['losses'] == before['losses'] + 1)
check('and no Back from it', p['shorts'] == before['shorts'])
b.open()
time.sleep(3.5)
p = s.bk()
check('opened again once it is back', p['connected'] is True and p['opens'] == before['opens'] + 1)
b.tap()
check('and it works again', s.cur() == 'home')

check('Calculator open for the dropped-events checks', open_app(s, 'calculator'))
before = s.bk()
b.key(1)
b.ev(EV_SYN, SYN_DROPPED, 0)
b.key(0)  # dropped with the rest, up to the report
time.sleep(LONG_S + 0.4)
p = s.bk()
check('events dropped mid-press: no Home, no Back, nothing held',
      s.cur() == 'calculator' and p['state'] == 'up' and p['shorts'] == before['shorts'] and
      p['longs'] == before['longs'])
b.tap()
check('the next press counts', s.cur() == 'home')

check('the shell kept running and logged no fault', s.p.poll() is None and ' ERROR ' not in s.log())
check('a clean stop', s.stop() == 0)
b.close()
pw.close()

# ---- turned off ----
s = Shell('none', 'none')
p = s.bk()
check('POCKETOS_BACK_KEY_DEVICE=none: off, nothing open', p['enabled'] is False and p['connected'] is False)
s.stop()

print('back_key_shell_test.sh: %d checks, %d failure(s)' % (checks, failed))
sys.exit(1 if failed else 0)
PY
failed=$?
echo "back_key_shell_test.sh: exit $failed"
exit $failed
