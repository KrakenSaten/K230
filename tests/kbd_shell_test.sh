#!/bin/bash
# The physical keyboard inside the shell: the driver end to end against a
# fake controller, and the shell's own behaviour when no keyboard exists.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). The binary under test
# is built beside it by ui/shell/CMakeLists.txt.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# 1. Raw FIFO byte to character in a focused field, through the real timer,
#    key map and stream.
BIN=${SHELL_KBD_TEST:-$(dirname "$SHELL_BIN")/shell_kbd_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|shell_kbd_test:'
    check "a TCA8418 event reaches a text field" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL shell_kbd_test binary missing: $BIN"; failed=$((failed + 1))
fi

# 2. No keyboard is a normal outcome: the shell starts, says so once, and
#    does not treat it as a fault. This is the same path a board takes with
#    the base board detached (design doc §9).
out=/tmp/kbd_shell.$$
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR=$(mktemp -d) POCKETOS_LOG_DIR=$(mktemp -d) \
POCKETOS_CONFIG_DIR=$(mktemp -d) POCKETOS_STATE_DIR=$(mktemp -d) \
    "$SHELL_BIN" --exit-after-ms 600 >"$out" 2>&1
rc=$?
check "the shell starts with no keyboard attached" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" \
    "$(grep -qE ' ERROR |assert' "$out" && echo 0 || echo 1)"
check "and says once that there is no keyboard" \
    "$([ "$(grep -c 'keyboard: none' "$out")" = "1" ] && echo 1 || echo 0)"
rm -f "$out"

echo "kbd_shell_test.sh: $failed failure(s)"
exit $((failed > 0))
