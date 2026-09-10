#!/bin/bash
# PocketNotes in the running app, and the shell's ownership of the keyboard.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${NOTES_APP_TEST:-$(dirname "$SHELL_BIN")/notes_app_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|notes_app_test:'
    check "PocketNotes end to end, tapped on a keyboard" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL notes_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The shell must build exactly one keyboard, and it must start hidden.
check "the shell creates the one keyboard" \
    "$(grep -q 'sh.keyboard = pos_keyboard_create' ui/shell/shell.c && echo 1 || echo 0)"
check "exactly one, not one per app" \
    "$([ "$(grep -c 'pos_keyboard_create' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
check "no app creates a keyboard" \
    "$(grep -rq 'pos_keyboard_create' apps/ && echo 0 || echo 1)"
check "closing an app puts the keyboard away" \
    "$(grep -q 'pocketos_shell_keyboard_hide' ui/shell/shell.c && echo 1 || echo 0)"

# Notes is on the launcher and answers over shell.*.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open notes --exit-after-ms 900 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Notes" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "Notes reports itself open" \
    "$(grep -q 'open app notes' "$LOGD/log/shell.log" 2>/dev/null ||
       grep -q 'open app notes' "$LOGD/out" && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "notes_shell_test: $failed failure(s)"
exit $((failed > 0))
