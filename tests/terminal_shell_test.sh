#!/bin/bash
# Terminal in the running app (tests/terminal_app_test.c) and in the real
# shell: opened in both orientations with a real shell on its PTY, left, and
# nothing that the terminal started still running once the shell is gone;
# and a session kept while the app is closed, found again on the next open.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) with terminal_app_test
# beside it, and pos (make all). TERMINAL_SHOTS=<dir> keeps the real shell's
# screenshots.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${TERMINAL_APP_TEST:-$(dirname "$SHELL_BIN")/terminal_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|^note|terminal_app_test:'
    check "Terminal end to end, keys to a real shell" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL terminal_app_test missing: $BIN"; failed=$((failed + 1))
fi
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(terminal_app_test' && echo 1 || echo 0)"

# Every process the terminal starts inherits the shell's environment, so a
# mark put in it finds anything left behind, wherever it was reparented to.
MARK="terminal-shell-test-$$"
left() {
    for e in /proc/[0-9]*/environ; do
        tr '\0' '\n' 2>/dev/null < "$e" | grep -qx "TERMINAL_TEST_MARK=$MARK" && echo "${e%/environ}"
    done
}

for o in portrait landscape; do
    T=$(mktemp -d)
    mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/home"
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
    POCKETOS_STATE_DIR="$T/state" HOME="$T/home" POCKETOS_TERMINAL_SHELL=/bin/sh PS1='TPROMPT> ' \
    TERMINAL_TEST_MARK="$MARK" \
        timeout 30 "$SHELL_BIN" --rotation $o --no-lock --open terminal --screenshot "$T/shot.png" \
        --exit-after-ms 1500 >"$T/log/out" 2>&1
    rc=$?
    check "$o: the shell opens Terminal and exits cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$o: it reports the app open and closed" \
        "$(grep -q 'open app terminal' "$T/log/shell.log" && grep -q 'close app terminal' "$T/log/shell.log" &&
           echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$T/log/out" "$T/log/shell.log" 2>/dev/null && echo 0 || echo 1)"
    check "$o: the screenshot was written" "$([ -s "$T/shot.png" ] && echo 1 || echo 0)"
    [ -n "${TERMINAL_SHOTS:-}" ] && cp "$T/shot.png" "$TERMINAL_SHOTS/shell-$o.png"
    check "$o: nothing the terminal started is still running" "$([ -z "$(left)" ] && echo 1 || echo 0)"
    rm -rf "$T"
done

# Typed at, in the real shell: keys written to the simulator's key hook go
# into the input stream as the keyboard base's driver pushes them, through
# the raw key path to the real shell on the PTY. The shell's own work is the
# proof: files it writes. Then the shell is left with a job running.
T=$(mktemp -d)
mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/home"
mkfifo "$T/keys"
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
POCKETOS_STATE_DIR="$T/state" HOME="$T/home" POCKETOS_TERMINAL_SHELL=/bin/sh PS1='TPROMPT> ' \
TERMINAL_TEST_MARK="$MARK" POCKETOS_TEST_TERMINAL_KEYS="$T/keys" \
    timeout 30 "$SHELL_BIN" --rotation landscape --no-lock --open terminal --exit-after-ms 5000 \
    >"$T/log/out" 2>&1 &
pid=$!
printf '%s\n' 'echo typed-$((6*7)) > "$HOME/proof"' 'sleep 30' > "$T/k1"
{
    printf '{C-c}'
    printf '%s\n' 'echo rc=$? > "$HOME/rc"' 'printf %s "a{TAB}b{ESC}" | od -c > "$HOME/od"' 'sleep 1000 &'
} > "$T/k2"
sleep 1
# The FIFO is opened inside timeout: opening it for writing waits for a
# reader, and a shell that never opened it (no hook in this build, or gone)
# must fail this check, not hang the suite.
timeout 10 sh -c 'exec > "$3"; cat "$1"; sleep 0.5; cat "$2"' _ "$T/k1" "$T/k2" "$T/keys"
frc=$?
check "typed at: the terminal took the keys from the hook" "$([ "$frc" = "0" ] && echo 1 || echo 0)"
wait $pid
rc=$?
check "typed at: the shell exits cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "typed at: a typed command ran in the terminal's shell" \
    "$([ "$(cat "$T/home/proof" 2>/dev/null)" = "typed-42" ] && echo 1 || echo 0)"
check "typed at: Ctrl+C interrupted a running command" \
    "$([ "$(cat "$T/home/rc" 2>/dev/null)" = "rc=130" ] && echo 1 || echo 0)"
check "typed at: Tab and Esc reached the program as HT and ESC" \
    "$(grep -q 'a  \\t   b 033' "$T/home/od" 2>/dev/null && echo 1 || echo 0)"
check "typed at: the job it left running is gone with the shell" "$([ -z "$(left)" ] && echo 1 || echo 0)"
check "typed at: no fault" "$(grep -qE ' ERROR |assert' "$T/log/out" "$T/log/shell.log" 2>/dev/null && echo 0 || echo 1)"
rm -rf "$T"

# The session outlives the app, in the real shell: a job started in the
# terminal keeps running while the launcher is up, the next open (and the
# re-creation a text-size change over IPC does) is the same shell, and the
# shell's own exit ends the session with it. Needs pos (make all).
POS=${POS:-tools/pos/pos}
if [ -x "$POS" ]; then
    T=$(mktemp -d)
    mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/home"
    mkfifo "$T/keys"
    export POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
           POCKETOS_STATE_DIR="$T/state"
    SDL_VIDEODRIVER=dummy HOME="$T/home" POCKETOS_TERMINAL_SHELL=/bin/sh PS1='TPROMPT> ' \
    TERMINAL_TEST_MARK="$MARK" POCKETOS_TEST_TERMINAL_KEYS="$T/keys" \
        timeout 60 "$SHELL_BIN" --rotation portrait --no-lock --open terminal >"$T/log/out" 2>&1 &
    pid=$!
    for _ in $(seq 1 60); do [ -S "$T/run/shell.sock" ] && break; sleep 0.1; done
    sleep 0.5
    keys() { timeout 10 sh -c 'exec > "$2"; printf "%s\n" "$1"' _ "$1" "$T/keys"; }
    keys 'echo $$ > "$HOME/pid1"; (sleep 2; echo late > "$HOME/late") &'
    sleep 0.8
    "$POS" app home >/dev/null 2>&1
    sleep 0.3
    check "kept: the launcher is up" \
        "$("$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -q '"current":"home"' && echo 1 || echo 0)"
    check "kept: the job has not run yet" "$([ ! -e "$T/home/late" ] && echo 1 || echo 0)"
    sleep 2.5
    check "kept: a job started in the terminal ran on with the app closed" \
        "$([ "$(cat "$T/home/late" 2>/dev/null)" = "late" ] && echo 1 || echo 0)"
    "$POS" app start terminal >/dev/null 2>&1
    sleep 0.5
    "$POS" call shell shell.text_size size=large >/dev/null 2>&1
    sleep 0.6
    keys 'echo $$ > "$HOME/pid2"'
    sleep 1
    check "kept: reopened, and re-created at Large, it is the same shell" \
        "$([ -s "$T/home/pid1" ] && cmp -s "$T/home/pid1" "$T/home/pid2" && echo 1 || echo 0)"
    check "kept: the shell logs two opens at least" \
        "$([ "$(grep -c 'open app terminal' "$T/log/shell.log")" -ge 2 ] && echo 1 || echo 0)"
    keys 'sleep 1000 &'
    sleep 0.5
    "$POS" app home >/dev/null 2>&1
    sleep 0.3
    check "kept: with the app closed, its job is still running" "$([ -n "$(left)" ] && echo 1 || echo 0)"
    kill -TERM "$pid"
    wait "$pid"
    rc=$?
    check "kept: the shell stops cleanly with a session in the background" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "kept: and nothing the terminal started outlives it" "$([ -z "$(left)" ] && echo 1 || echo 0)"
    check "kept: no fault" "$(grep -qE ' ERROR |assert' "$T/log/out" "$T/log/shell.log" 2>/dev/null && echo 0 || echo 1)"
    unset POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
    rm -rf "$T"
else
    echo "FAIL pos missing: $POS (make all)"; failed=$((failed + 1))
fi

echo "terminal_shell_test: $failed failure(s)"
exit $((failed > 0))
