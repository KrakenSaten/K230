#!/bin/bash
# Text size (DS §46) in the running shell, and its LVGL half
# (tests/text_size_ui_test.c).
#
#   - absent from settings.conf it is Small, and nothing is written for it;
#   - shell.text_size stores it, and a restart (a reboot is a restart that
#     reads the same file) comes back at that size, for each of the three;
#   - a stored value that is not a size starts the shell at Small, says so
#     once, and leaves the file as it was until somebody chooses;
#   - a request for something else is refused and changes nothing;
#   - --text-size is for one run and is never stored;
#   - a size changed live draws exactly what a shell started at that size
#     draws, on the launcher and with an app open, and Small after Large is
#     Small again, pixel for pixel;
#   - the layout audit finds nothing on the launcher or in Settings at any
#     size, in either orientation.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell); text_size_ui_test
# beside it.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=$(dirname "$SHELL_BIN")/text_size_ui_test
if [ -x "$BIN" ]; then
    log=$(env -u POCKETUI_TEST_TEXT_SIZE SDL_VIDEODRIVER=dummy timeout 120 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|text_size_ui_test:'
    check "styles at every size, live, and the audit's own cases" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "and LVGL warned about nothing" "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL text_size_ui_test binary missing: $BIN"; failed=$((failed + 1))
fi

# One shell per call: start it on CFG, run the steps, stop it. Steps as in
# ipc() below; prints one line per step.
WORK=$(mktemp -d)
CFG=$WORK/cfg
mkdir -p "$CFG"
run_shell() { # args... -- steps...
    local args=() steps=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do args+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    steps=("$@")
    python3 - "$SHELL_BIN" "$CFG" "$WORK" "${#args[@]}" "${args[@]}" "${steps[@]}" <<'PY'
import json, os, socket, struct, subprocess, sys, tempfile, time, shutil
shell, cfg, work, nargs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
args, steps = sys.argv[5:5 + nargs], sys.argv[5 + nargs:]
tmp = tempfile.mkdtemp(dir=work)
run, logd, state = (os.path.join(tmp, d) for d in ('run', 'log', 'state'))
for d in (run, logd, state):
    os.makedirs(d)
env = dict(os.environ, SDL_VIDEODRIVER='dummy', POCKETOS_RUNTIME_DIR=run, POCKETOS_LOG_DIR=logd,
           POCKETOS_CONFIG_DIR=cfg, POCKETOS_STATE_DIR=state, HOME=os.path.join(state, 'home'))
env.pop('POCKETUI_TEST_TEXT_SIZE', None)
p = subprocess.Popen([shell, '--no-lock'] + args, env=env, stdout=open(os.path.join(logd, 'out'), 'w'),
                     stderr=subprocess.STDOUT)
sock = os.path.join(run, 'shell.sock')

def call(method, params=None):
    body = json.dumps({'id': 1, 'method': method, **({'params': params} if params is not None else {})}).encode()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock)
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

try:
    t0 = time.time()
    while not os.path.exists(sock):
        if time.time() - t0 > 20 or p.poll() is not None:
            raise RuntimeError('no shell')
        time.sleep(0.05)
    time.sleep(0.5)
    for st in steps:
        kind, _, rest = st.partition(':')
        if kind == 'info':
            print('info', call('shell.info')['result'].get(rest))
        elif kind == 'size':
            r = call('shell.text_size', {'size': rest} if rest else None)
            print('size', json.dumps(r.get('result', r.get('error'))))
        elif kind == 'open':
            call('shell.open', {'id': rest})
            print('open', rest)
        elif kind == 'shot':
            call('shell.screenshot', {'path': rest})
            print('shot', rest)
        elif kind == 'audit':
            r = call('shell.audit')['result']
            print('audit', rest, json.dumps(r['count']), r['text_size'])
        elif kind == 'sleep':
            time.sleep(int(rest) / 1000.0)
finally:
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    for fn in ('out', 'shell.log'):
        fp = os.path.join(logd, fn)
        if os.path.exists(fp):
            for line in open(fp, errors='replace'):
                if 'text size' in line or 'WARN' in line or 'ERROR' in line:
                    print('log', line.rstrip())
    shutil.rmtree(tmp, ignore_errors=True)
PY
}
stored() { grep -E '^text_size=' "$CFG/settings.conf" 2>/dev/null | cut -d= -f2; }

# Absent: Small, and nothing written.
out=$(run_shell -- info:text_size)
check "with nothing stored the shell starts at Small" "$(echo "$out" | grep -qx 'info small' && echo 1 || echo 0)"
check "and stores nothing for it" "$([ -z "$(stored)" ] && echo 1 || echo 0)"

# Each size, stored and back after a restart.
for z in medium large small; do
    out=$(run_shell -- "size:$z" info:text_size)
    check "shell.text_size $z answers $z" "$(echo "$out" | grep -q "^size {\"size\": \"$z\"" && echo 1 || echo 0)"
    check "and settings.conf holds text_size=$z" "$([ "$(stored)" = "$z" ] && echo 1 || echo 0)"
    out=$(run_shell -- info:text_size)
    check "a restart comes back at $z" "$(echo "$out" | grep -qx "info $z" && echo 1 || echo 0)"
done

# A value that is not a size.
printf 'theme=doors\ntext_size=huge\n' > "$CFG/settings.conf"
out=$(run_shell -- info:text_size)
check "a stored 'huge' starts the shell at Small" "$(echo "$out" | grep -qx 'info small' && echo 1 || echo 0)"
check "and says so in the log" "$(echo "$out" | grep -q "stored text size 'huge'" && echo 1 || echo 0)"
check "and leaves the file as it was" "$([ "$(stored)" = "huge" ] && grep -qx 'theme=doors' "$CFG/settings.conf" &&
                                          echo 1 || echo 0)"
out=$(run_shell -- size:huge size: info:text_size)
check "shell.text_size huge is refused" "$(echo "$out" | grep -q '^size {"code": 2,' && echo 1 || echo 0)"
check "and changes nothing" "$(echo "$out" | grep -qx 'info small' && [ "$(stored)" = "huge" ] && echo 1 || echo 0)"
check "shell.text_size with no size reads it" "$(echo "$out" | grep -q '^size {"size": "small", "pending": false}' &&
                                                echo 1 || echo 0)"

# --text-size is for the run.
rm -f "$CFG/settings.conf"
out=$(run_shell --text-size large -- info:text_size)
check "--text-size large runs at Large" "$(echo "$out" | grep -qx 'info large' && echo 1 || echo 0)"
check "and stores nothing" "$([ -z "$(stored)" ] && echo 1 || echo 0)"

# Live equals fresh. Each pair is shot back to back; a minute turning between
# the two moves the clock, so a pair that differs is taken again once.
same_png() { cmp -s "$1" "$2"; }
# The control: two sizes must not compare equal, or "equal" proves nothing.
rm -f "$CFG/settings.conf"
run_shell --open settings -- sleep:300 "shot:$WORK/neg-small.png" >/dev/null
run_shell --open settings --text-size medium -- sleep:300 "shot:$WORK/neg-medium.png" >/dev/null
check "control: Settings at Small and at Medium are different pictures" \
    "$([ -s "$WORK/neg-small.png" ] && [ -s "$WORK/neg-medium.png" ] && ! same_png "$WORK/neg-small.png" \
       "$WORK/neg-medium.png" && echo 1 || echo 0)"
for rot in portrait landscape; do
    for z in medium large; do
        ok=0
        for attempt in 1 2; do
            rm -f "$CFG/settings.conf"
            run_shell --rotation $rot -- sleep:300 "size:$z" sleep:600 "shot:$WORK/live.png" >/dev/null
            rm -f "$CFG/settings.conf"
            run_shell --rotation $rot --text-size $z -- sleep:300 "shot:$WORK/fresh.png" >/dev/null
            if same_png "$WORK/live.png" "$WORK/fresh.png"; then ok=1; break; fi
        done
        check "$rot: the launcher changed live to $z is the launcher started at $z" "$ok"
        ok=0
        for attempt in 1 2; do
            rm -f "$CFG/settings.conf"
            run_shell --rotation $rot --open settings -- sleep:300 "size:$z" sleep:600 "shot:$WORK/live.png" >/dev/null
            rm -f "$CFG/settings.conf"
            run_shell --rotation $rot --open settings --text-size $z -- sleep:300 "shot:$WORK/fresh.png" >/dev/null
            if same_png "$WORK/live.png" "$WORK/fresh.png"; then ok=1; break; fi
        done
        check "$rot: Settings open while it changes to $z is Settings opened at $z" "$ok"
    done
    ok=0
    for attempt in 1 2; do
        rm -f "$CFG/settings.conf"
        run_shell --rotation $rot --open settings -- sleep:300 size:large sleep:400 size:small sleep:600 \
            "shot:$WORK/live.png" >/dev/null
        rm -f "$CFG/settings.conf"
        run_shell --rotation $rot --open settings -- sleep:300 "shot:$WORK/fresh.png" >/dev/null
        if same_png "$WORK/live.png" "$WORK/fresh.png"; then ok=1; break; fi
    done
    check "$rot: Large and back to Small is Small again, pixel for pixel" "$ok"
done

# The audit on the shell's own screens and Settings.
rm -f "$CFG/settings.conf"
for rot in portrait landscape; do
    for z in small medium large; do
        out=$(run_shell --rotation $rot --text-size $z -- sleep:300 audit:home open:settings sleep:400 audit:settings)
        for what in home settings; do
            line=$(echo "$out" | grep "^audit $what ")
            # Nothing clipped, over anything else or without a size. A name the
            # launcher shortens with "..." is its rule for a name wider than
            # its cell (DS §46.4): counted and shown, but allowed.
            check "$rot $z: the audit finds nothing clipped, overlapping or sizeless on $what" \
                "$(echo "$line" | grep -qE '"clipped": 0, "truncated": [0-9]+, "overlap": 0, "zero": 0' &&
                   echo 1 || echo 0)"
            echo "$line" | grep -q '"truncated": 0' || echo "note $rot $z $what: $line"
        done
    done
done

rm -rf "$WORK"
echo "text_size_shell_test: $failed failure(s)"
exit $((failed > 0))
