#!/bin/bash
# The logical key stream and the text field (DS v0.1 section 17), plus the
# shell's own wiring of them.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). The binary under test
# is built beside it by ui/shell/CMakeLists.txt.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# 1. The input layer itself, under real LVGL devices.
BIN=${POS_INPUT_TEST:-$(dirname "$SHELL_BIN")/pos_input_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|pos_input_test:'
    check "the logical key stream and the text field" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL pos_input_test binary missing: $BIN"; failed=$((failed + 1))
fi

# 2. The shell owns the stream: DS section 17.4 says the shell creates it, so
#    a shell that starts without one would leave every future field deaf.
#    Nothing is typed here; this only proves the wiring is in place and that
#    adopting the host keyboard did not upset start-up.
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR=$(mktemp -d) POCKETOS_LOG_DIR=$(mktemp -d) \
POCKETOS_CONFIG_DIR=$(mktemp -d) POCKETOS_STATE_DIR=$(mktemp -d) \
    "$SHELL_BIN" --exit-after-ms 600 >/tmp/pos_input_shell.$$ 2>&1
rc=$?
check "the shell starts with the input stream wired" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "start-up logs no fault" \
    "$(grep -qE ' ERROR |assert' /tmp/pos_input_shell.$$ && echo 0 || echo 1)"
# A keyboard that is created but never adopted looks exactly like a dead one,
# so the shell says which happened and this reads it back. This is the only
# check that exercises LVGL's real SDL keyboard device rather than a stub.
check "the host keyboard was adopted as a source" \
    "$(grep -q 'host keyboard adopted as an input source' /tmp/pos_input_shell.$$ && echo 1 || echo 0)"
check "the host keyboard was not left unadopted" \
    "$(grep -q 'not adopted' /tmp/pos_input_shell.$$ && echo 0 || echo 1)"
rm -f /tmp/pos_input_shell.$$

# 3. The style lint (tests/style_lint.sh) already forbids colour literals
#    outside the theme engine; the field's four states are role styles, so
#    check the roles exist rather than trusting the component.
for role in POS_STYLE_FIELD POS_STYLE_FIELD_FOCUSED POS_STYLE_FIELD_DISABLED \
            POS_STYLE_FIELD_ERROR POS_STYLE_FIELD_PLACEHOLDER POS_STYLE_FIELD_CURSOR; do
    check "$role is a role style" \
        "$(grep -q "$role\]" ui/pocketui/pos_styles.c && echo 1 || echo 0)"
done

# 4. DS section 17.4: an app must not bind to a keyboard or branch on a key's
#    source. No app may name the keyboard widget or a source-specific API.
hits=$(grep -rnE 'lv_keyboard|lv_sdl_keyboard|pos_input_add_source' apps/ 2>/dev/null)
check "no app binds to a keyboard or a source (DS 17.4)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

echo "pos_input_test.sh: $failed failure(s)"
exit $((failed > 0))
