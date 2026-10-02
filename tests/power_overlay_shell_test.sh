#!/bin/bash
# Power & Sleep, the time zone and the developer overlay in the running shell
# (DS §52; ui/shell/shell_power.c, tz_zones.c, shell_overlay.c), over shell.*:
#
#   - nothing of it is stored or running until it is asked for: no overlay
#     object, no timer, both timeouts never, the zone UTC and not stored;
#   - the overlay comes on and off, again and again, through shell.debug_overlay
#     and the stored debug_overlay; while on it refreshes every 2 seconds and
#     no faster, its line carries numbers and no address; while off nothing of
#     it exists and nothing refreshes; the launcher holds the same number of
#     objects after ten round trips as before; a restart brings it back on;
#   - the time zone is set by name, stored, applied, refused for a name that
#     is not one of the list, and comes back after a restart;
#   - a screen-off time puts the screen off after that long without input;
#     the touch and the key that wake it do nothing else, and the next ones
#     do what they always did; a request that acts like a person wakes it;
#   - invalid power values are refused and change nothing.
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
cfg = os.path.join(work, 'cfg')
os.makedirs(cfg)
failed = 0
checks = 0


def check(what, ok):
    global failed, checks
    checks += 1
    if ok:
        print('ok   ' + what)
    else:
        failed += 1
        print('FAIL ' + what)


class Shell:
    def __init__(self, *args):
        tmp = tempfile.mkdtemp(dir=work)
        self.run, self.logd, state = (os.path.join(tmp, d) for d in ('run', 'log', 'state'))
        for d in (self.run, self.logd, state):
            os.makedirs(d)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=self.run, POCKETOS_LOG_DIR=self.logd,
                   POCKETOS_CONFIG_DIR=cfg, POCKETOS_STATE_DIR=state, HOME=os.path.join(state, 'home'))
        self.p = subprocess.Popen([shell, '--no-lock'] + list(args), env=env,
                                  stdout=open(os.path.join(self.logd, 'out'), 'w'), stderr=subprocess.STDOUT)
        self.sock = os.path.join(self.run, 'shell.sock')
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.1)
        time.sleep(0.5)

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


def stored():
    try:
        return dict(l.strip().split('=', 1) for l in open(os.path.join(cfg, 'settings.conf')) if '=' in l and not l.startswith('#'))
    except OSError:
        return {}


# ---- 1. defaults: nothing stored, nothing running ----
s = Shell()
i = s.info()
ov, pw, tz = i.get('debug_overlay', {}), i.get('power', {}), i.get('timezone', {})
check('the overlay is off and nothing of it exists', ov.get('enabled') is False and ov.get('alive') is False and
      ov.get('text') == '')
check('both timeouts never, the screen on, sleep said to be unavailable',
      pw.get('screen_off_s') == 0 and pw.get('auto_lock_s') == 0 and pw.get('screen') == 'on' and
      pw.get('sleep') == 'unavailable')
check('the zone is UTC, and nothing is stored for it', tz.get('zone') == 'UTC' and tz.get('stored') is False)
check('nothing of it is in the settings store', not any(k in stored() for k in
                                                         ('debug_overlay', 'timezone', 'screen_off_s', 'auto_lock_s')))
before = s.result('shell.audit').get('objects')

# ---- 2. the overlay ----
r = s.result('shell.debug_overlay', {'enabled': True})
check('shell.debug_overlay on: alive, stored', r.get('enabled') is True and r.get('alive') is True and
      stored().get('debug_overlay') == '1')
n0 = s.info()['debug_overlay']['refreshes']
time.sleep(5.1)
o = s.info()['debug_overlay']
n1 = o['refreshes']
print('     overlay refreshes in 5.1 s: %d; line: %s' % (n1 - n0, o['text']))
check('it refreshes every 2 seconds, no faster', 2 <= n1 - n0 <= 3 and o['period_ms'] == 2000)
# No sysd runs here, so every figure is the dash; the words are the line's own.
check('its line: CPU, RAM, temperature, network - and no address or name',
      o['text'].startswith('CPU ') and ' RAM ' in o['text'] and 'NET ' in o['text'] and
      not any(t in o['text'] for t in ('192.168', 'eth0', 'wlan', ':')))
r = s.result('shell.debug_overlay', {'enabled': False})
check('off: nothing of it exists', r.get('enabled') is False and r.get('alive') is False and
      stored().get('debug_overlay') == '0')
n2 = s.info()['debug_overlay']['refreshes']
time.sleep(4.2)
check('and nothing refreshes while it is off', s.info()['debug_overlay']['refreshes'] == n2)
for k in range(10):
    s.result('shell.debug_overlay', {'enabled': True})
    s.result('shell.debug_overlay', {'enabled': False})
after = s.result('shell.audit').get('objects')
check('ten times on and off: the same objects on the screen as before (%s, %s)' % (before, after),
      before is not None and before == after)
e = s.call('shell.debug_overlay', {'enabled': 'yes'})
check('a value that is not true or false is refused', 'error' in e and s.info()['debug_overlay']['enabled'] is False)
s.result('shell.debug_overlay', {'enabled': True})
check('the shell kept running, and logged no fault', s.p.poll() is None and ' ERROR ' not in s.log())
s.stop()
s = Shell()
check('restarted: the overlay is on again, from the store', s.info()['debug_overlay']['alive'] is True)

# ---- 3. the time zone ----
r = s.result('shell.timezone', {'zone': 'Europe/Oslo'})
check('shell.timezone sets a zone by name', r.get('zone') == 'Europe/Oslo' and r.get('stored') is True and
      stored().get('timezone') == 'Europe/Oslo')
e = s.call('shell.timezone', {'zone': 'Mars/Olympus'})
check('a name that is not one of the list is refused, nothing changed',
      'error' in e and s.info()['timezone']['zone'] == 'Europe/Oslo')
check('the change is logged once, no fault', 'time zone: Europe/Oslo' in s.log() and ' ERROR ' not in s.log())
s.stop()
s = Shell()
check('restarted: the zone is applied from the store', s.info()['timezone']['zone'] == 'Europe/Oslo' and
      'time zone: Europe/Oslo' in s.log())
r = s.result('shell.timezone', {'zone': 'UTC'})
check('and back to UTC', r.get('zone') == 'UTC' and stored().get('timezone') == 'UTC')

# ---- 4. Power & Sleep: refusals ----
for bad in ({'screen_off_s': 45}, {'auto_lock_s': 30}, {'screen_off_s': 'never'}, {'lock_at_start': 1},
            {'screen_off_s': 30.5}):
    e = s.call('shell.power', bad)
    check('refused: %s' % json.dumps(bad), 'error' in e)
p = s.info()['power']
check('and nothing changed', p['screen_off_s'] == 0 and p['auto_lock_s'] == 0 and 'screen_off_s' not in stored())
r = s.result('shell.power', {'auto_lock_s': 1800, 'lock_at_start': False})
check('a lock time and the lock at start are stored', r.get('auto_lock_s') == 1800 and
      r.get('lock_at_start') is False and stored().get('auto_lock_s') == '1800' and stored().get('lock_screen') == '0')
s.result('shell.power', {'auto_lock_s': 0, 'lock_at_start': True})

# ---- 5. the screen going off, and waking ----
s.result('shell.home')
r = s.result('shell.power', {'screen_off_s': 30})
check('screen off after 30 s is stored', r.get('screen_off_s') == 30 and stored().get('screen_off_s') == '30')
time.sleep(25)
check('25 s without input: still on', s.info()['power']['screen'] == 'on')
time.sleep(7)
p = s.info()['power']
check('past 30 s: off, once', p['screen'] == 'off' and p['screen_offs'] == 1)
check('nothing else changed: the launcher is there under it', s.info()['current'] == 'home')
r = s.result('shell.key', {'code': 60})
time.sleep(0.5)
i = s.info()
check('F2 (Settings) while off only wakes the screen', i['power']['screen'] == 'on' and i['current'] == 'home')
s.result('shell.key', {'code': 60})
time.sleep(0.5)
check('and the next F2 opens Settings, as it always did', s.info()['current'] == 'settings')
s.result('shell.home')
time.sleep(32)
check('off again after another 30 s', s.info()['power']['screen'] == 'off')
r = s.call('shell.tap', {'text': 'Settings'})
time.sleep(0.6)
i = s.info()
check('a tap while off only wakes the screen', 'result' in r and i['power']['screen'] == 'on' and i['current'] == 'home')
s.call('shell.tap', {'text': 'Settings'})
time.sleep(0.6)
check('and the next tap does what it always did', s.info()['current'] == 'settings')
s.result('shell.home')
time.sleep(32)
s.result('shell.open', {'id': 'settings'})
i = s.info()
check('a request that acts like a person wakes it, and acts', i['power']['screen'] == 'on' and
      i['current'] == 'settings')
s.result('shell.power', {'screen_off_s': 0})
log = s.log()
check('the screen going off and on is logged', 'power: screen off after' in log and 'power: screen on (key)' in log and
      'power: screen on (touch)' in log)
check('and nothing faulted', s.p.poll() is None and ' ERROR ' not in log)
rc = s.stop()
check('the shell stops cleanly', rc == 0)

print('power_overlay_shell_test: %d checks, %d failure(s)' % (checks, failed))
sys.exit(1 if failed else 0)
PY
