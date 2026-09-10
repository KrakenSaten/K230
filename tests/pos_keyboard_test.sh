#!/bin/bash
# The touch keyboard (DS v0.1 section 17.3).
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). The binary under test
# is built beside it by ui/shell/CMakeLists.txt.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# 1. The keyboard typing into a real field under a real pointer device.
BIN=${POS_KEYBOARD_TEST:-$(dirname "$SHELL_BIN")/pos_keyboard_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|pos_keyboard_test:'
    check "the touch keyboard types into a field end to end" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL pos_keyboard_test binary missing: $BIN"; failed=$((failed + 1))
fi

# 2. DS section 17.4: the keyboard is a source of the one stream. It must
#    reach a field only through pos_input_push_key(), so it may not name a
#    text area at all.
hits=$(grep -nE 'lv_textarea|lv_keyboard' ui/pocketui/pos_keyboard.c 2>/dev/null)
check "the keyboard never names a text area (DS 17.4)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# 3. Every printable key must go through the stream. The only calls into the
#    field's world are pos_input_push_key(); nothing else may write text.
check "the keyboard's only way out is pos_input_push_key" \
    "$(grep -qE 'pos_input_push_key' ui/pocketui/pos_keyboard.c && echo 1 || echo 0)"

# 4. The keys must stay out of the focus group, or a tap would move focus.
check "keys are not click-focusable (DS 17.2)" \
    "$(grep -q 'LV_OBJ_FLAG_CLICK_FOCUSABLE' ui/pocketui/pos_keyboard.c && echo 1 || echo 0)"
check "keys never join the focus group" \
    "$(grep -q 'pos_input_add_obj' ui/pocketui/pos_keyboard.c && echo 0 || echo 1)"

# 5. The DEV-1 geometry and the repeat timings are the DS's, not invented
#    here; if these constants drift the deviation no longer describes what
#    ships.
check "keys are 52 x 64 (DEV-1)" \
    "$(grep -q 'define POS_KB_KEY_W 52' ui/pocketui/pos_keyboard.h &&
       grep -q 'define POS_KB_KEY_H 64' ui/pocketui/pos_keyboard.h && echo 1 || echo 0)"
check "the sheet is 296 tall" \
    "$(grep -q 'define POS_KB_H 296' ui/pocketui/pos_keyboard.h && echo 1 || echo 0)"
check "Backspace repeats after 400 ms, then every 60 ms" \
    "$(grep -q 'define POS_KB_REPEAT_DELAY_MS 400' ui/pocketui/pos_keyboard.h &&
       grep -q 'define POS_KB_REPEAT_MS 60' ui/pocketui/pos_keyboard.h && echo 1 || echo 0)"

# 6. No long-press accent popup, and no hide key: both are out of scope in
#    DS section 17.7 and section 17.3 respectively.
check "no long-press accent popup" \
    "$(grep -qiE 'LONG_PRESSED|accent_popup|popover' ui/pocketui/pos_keyboard.c && echo 0 || echo 1)"
check "no hide key" \
    "$(grep -qiE '"HIDE"' ui/pocketui/pos_keyboard.c && echo 0 || echo 1)"

# 7. No app may bind to the keyboard: apps see a focused field (DS 17.4).
hits=$(grep -rnE 'pos_keyboard_|lv_keyboard' apps/ 2>/dev/null)
check "no app binds to the keyboard (DS 17.4)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

echo "pos_keyboard_test.sh: $failed failure(s)"
exit $((failed > 0))
