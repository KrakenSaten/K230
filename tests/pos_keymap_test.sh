#!/bin/bash
# The physical keyboard's translation layer (DS v0.1 section 17.4).
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). The binary under test
# is built beside it by ui/shell/CMakeLists.txt.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${POS_KEYMAP_TEST:-$(dirname "$SHELL_BIN")/pos_keymap_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|pos_keymap_test:'
    check "TCA8418 codes translate to the one logical vocabulary" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL pos_keymap_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The translator is pure: no I/O, no hardware, no LVGL objects. If that ever
# stops being true it can no longer be tested without a keyboard attached.
hits=$(grep -nE 'fopen|open\(|ioctl|mmap|gpiod_|/dev/' ui/pocketui/pos_keymap.c 2>/dev/null)
check "the translator does no I/O" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(grep -nE 'lv_obj|lv_indev|lv_group' ui/pocketui/pos_keymap.c 2>/dev/null)
check "and touches no LVGL object" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# DS 17.4: one stream. Nothing may reach a field except through pos_input.
hits=$(grep -nE 'lv_textarea|pos_keyboard_' ui/pocketui/pos_keymap.c 2>/dev/null)
check "and never names a text area or the touch keyboard" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"

# The map is the vendor's, and the file must say so: a mapping with no
# provenance is a guess, and this one is not allowed to become one.
check "the map records where it came from" \
    "$(grep -q 'ui_hardware.c' ui/pocketui/pos_keymap.h && echo 1 || echo 0)"
check "and that it is not yet hardware-verified" \
    "$(grep -qi 'DOCUMENTED, not verified' ui/pocketui/pos_keymap.h && echo 1 || echo 0)"

# No app may reach the physical keyboard either.
hits=$(grep -rnE 'pos_keymap_|tca8418|TCA8418' apps/ 2>/dev/null)
check "no app binds to the physical keyboard (DS 17.4)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

echo "pos_keymap_test.sh: $failed failure(s)"
exit $((failed > 0))
