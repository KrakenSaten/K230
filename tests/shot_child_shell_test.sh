#!/bin/bash
# F7's capture child in the running shell (v0.3.0 cold review item 3). On the
# card a screenshot is a forked ffmpeg (kmsgrab), reaped from the tick. Two
# ways out of the shell have no tick after them: an exit, and a rotation
# re-executing the shell in place - which keeps the pid, so the capture stays
# a child, but the image that follows starts knowing nothing about it. Before
# the fix the capture outlived the shell that started it: never reaped, never
# timed out, its half-written file left in the screenshots folder.
#
# The simulator can snapshot the screen itself, so it is asked to use the child
# instead (POCKETOS_TEST_SHOT_CHILD=1, a test hook the card's build does not
# have) with a stand-in ffmpeg first on PATH that records its pid, writes part
# of a file and then either finishes or hangs.
#
# Requires: SHELL_BIN (CMake-built pocketos-shell, SDL) and `make all` (pos).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

export SDL_VIDEODRIVER=dummy POCKETOS_TEST_SHOT_CHILD=1
TMP=$(mktemp -d)
SP=""
cleanup() {
    [ -n "$SP" ] && kill "$SP" 2>/dev/null
    [ -s "$TMP/ffmpeg.pid" ] && kill -9 "$(cat "$TMP/ffmpeg.pid")" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# The stand-in: the output file is its last argument. With $TMP/hang present it
# leaves a partial file and waits as long as a stuck kmsgrab would; it execs
# sleep so the pid it recorded is the one that has to go.
mkdir -p "$TMP/bin"
cat > "$TMP/bin/ffmpeg" <<EOD
#!/bin/sh
echo \$\$ > "$TMP/ffmpeg.pid"
for a; do out=\$a; done
printf 'partial' > "\$out"
[ -e "$TMP/hang" ] && exec sleep 60
[ -e "$TMP/slow" ] && sleep 0.5
printf 'png' >> "\$out"
exit 0
EOD
chmod 0755 "$TMP/bin/ffmpeg"
export PATH="$TMP/bin:$PATH"

field() { # <json> <path>
    printf '%s' "$1" | python3 -c '
import json, sys
try:
    v = json.load(sys.stdin)
    for k in sys.argv[1].split("."):
        v = v[k]
    print(json.dumps(v) if isinstance(v, bool) else v)
except Exception:
    print("?")' "$2"
}
hw() { field "$("$POS" call shell shell.info 2>&1)" "hardware.$1"; }
shots() { ls "$POCKETOS_STATE_DIR"/screenshots/screenshot-*.png 2>/dev/null | wc -l; }
alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
# Gone, and not a zombie either: a zombie answers kill -0.
gone_within() { # <pid> <tenths of a second>
    n=0
    while alive "$1" && [ $n -lt "$2" ]; do sleep 0.1; n=$((n + 1)); done
    ! alive "$1"
}
logged() { grep -q -- "$1" "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log" 2>/dev/null; }
restarted() { for _ in $(seq 1 60); do logged 'restarting in place' && break; sleep 0.2; done; logged 'restarting in place'; }
start_shell() {
    rm -rf "$TMP/run" "$TMP/log" "$TMP/state" "$TMP/cfg" "$TMP/ffmpeg.pid" "$TMP/hang" "$TMP/slow"
    export POCKETOS_RUNTIME_DIR=$TMP/run POCKETOS_LOG_DIR=$TMP/log POCKETOS_STATE_DIR=$TMP/state
    export POCKETOS_CONFIG_DIR=$TMP/cfg
    mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_STATE_DIR" "$POCKETOS_CONFIG_DIR"
    "$SHELL_BIN" --no-lock >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 80); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
}
capture() { "$POS" call shell shell.action action=screenshot >/dev/null 2>&1; wait_for_pid; }
wait_for_pid() { for _ in $(seq 1 50); do [ -s "$TMP/ffmpeg.pid" ] && break; sleep 0.1; done; cat "$TMP/ffmpeg.pid" 2>/dev/null; }

# ---- the child itself, reaped from the tick -----------------------------------
start_shell
check "the shell starts" "$([ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && echo 1 || echo 0)"
FF=$(capture)
check "F7 forks the capture child" "$([ -n "$FF" ] && echo 1 || echo 0)"
for _ in $(seq 1 30); do [ "$(hw screenshot.saved)" = "1" ] && break; sleep 0.1; done
check "a capture that finishes is reaped and counted" "$([ "$(hw screenshot.saved)" = "1" ] && ! alive "$FF" && echo 1 || echo 0)"
check "and its file is kept" "$([ "$(shots)" -eq 1 ] && echo 1 || echo 0)"

# ---- a capture still running when the shell rotates in place -------------------
rm -f "$TMP/ffmpeg.pid"
: > "$TMP/hang"
FF=$(capture)
check "a hanging capture is running" "$(alive "$FF" && [ "$(hw screenshot.busy)" = "true" ] && echo 1 || echo 0)"
"$POS" call shell shell.rotation mode=landscape >/dev/null 2>&1
restarted
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && "$POS" shell info >/dev/null 2>&1 && break; sleep 0.1; done
check "the shell restarted in place, same pid" "$(logged 'restarting in place' && alive "$SP" && echo 1 || echo 0)"
check "the capture does not outlive the image that started it" "$(gone_within "$FF" 20 && echo 1 || echo 0)"
check "and the reason is logged" "$(logged 'screenshot: still being written as the shell leaves, stopped' && echo 1 || echo 0)"
check "and its half-written file is not left behind" "$([ "$(shots)" -eq 1 ] && echo 1 || echo 0)"
check "the new image is free to capture again" "$([ "$(hw screenshot.busy)" = "false" ] && echo 1 || echo 0)"

# ---- a capture still running when the shell is stopped --------------------------
rm -f "$TMP/ffmpeg.pid"
FF=$(capture)
check "a second hanging capture is running" "$(alive "$FF" && echo 1 || echo 0)"
kill "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
SP=""
check "a stopped shell takes its capture with it" "$(gone_within "$FF" 20 && echo 1 || echo 0)"
check "and leaves no half-written file" "$([ "$(shots)" -eq 1 ] && echo 1 || echo 0)"

# ---- a capture that finishes inside the grace period is kept ---------------------
# Half a second to go when the shell is told to stop: the way out waits for it
# rather than throwing a good screenshot away.
start_shell
: > "$TMP/slow"
FF=$(capture)
kill "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
SP=""
check "a capture that finishes as the shell leaves is waited for and kept" \
    "$([ "$(shots)" -eq 1 ] && ! alive "$FF" && grep -q 'png' "$(ls "$POCKETOS_STATE_DIR"/screenshots/screenshot-*.png | head -1)" && echo 1 || echo 0)"
check "and logged as written" "$(logged 'screenshot written to' && echo 1 || echo 0)"

echo "shot_child_shell_test: $failed failure(s)"
exit $((failed > 0))
