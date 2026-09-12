#!/bin/bash
# The physical keyboard driver's boundaries (design doc §1 and §11).
#
# The point of the split is that the controller logic can be tested without
# hardware, and that only one file knows what a K230 is. Both are easy to
# lose in a hurry, so they are checked rather than trusted.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# 1. The chip layer is pure: no device, no GPIO, no mapping, no LVGL. If that
#    stops being true it can no longer be tested without a keyboard attached.
hits=$(grep -nE 'open\(|/dev/|gpiod_|mmap|ioctl' ui/shell/kbd_tca8418.c 2>/dev/null)
check "the controller logic opens nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(grep -nE 'lv_|LV_' ui/shell/kbd_tca8418.c 2>/dev/null)
check "and touches no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# 2. One file knows the board. K230 pin numbers, the iomux and /dev/mem live
#    in the bus backend and nowhere else (ARCHITECTURE.md: K230 specifics stay
#    below the backend boundary).
#    The match is on real use - the string literal and the register address -
#    rather than on the words, so a comment explaining the design is not a
#    violation of it.
hits=$(grep -rln --include='*.c' -E '"/dev/mem"|0x91105000' ui/ apps/ core/ 2>/dev/null \
       | grep -v 'kbd_bus_k230.c')
check "only the bus backend opens /dev/mem or names the iomux" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# 3. DS 17.4: one stream. The driver pushes into pos_input and never reaches
#    a field, a text area or the touch keyboard itself.
hits=$(grep -nE 'lv_textarea|pos_keyboard_|lv_group_' ui/shell/shell_kbd.c 2>/dev/null)
check "the driver never names a field or the touch keyboard" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

check "and it delivers through pos_input_push_key" \
    "$(grep -q 'pos_input_push_key' ui/shell/shell_kbd.c 2>/dev/null && echo 1 || echo 0)"

# 4. A physical keyboard must not be adopted as an LVGL source: that path
#    loses LV_KEY_NEXT and LV_KEY_PREV (docs/KNOWN_ISSUES.md).
check "and is not adopted as an LVGL input source" \
    "$(grep -q 'pos_input_add_source' ui/shell/shell_kbd.c 2>/dev/null && echo 0 || echo 1)"

# 5. No app may reach the keyboard, its codes or its controller (DS 17.4).
hits=$(grep -rnE 'kbd_tca8418|kbd_bus|TCA8418' apps/ 2>/dev/null)
check "no app binds to the physical keyboard" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# 6. The driver owns no thread. pos_input has no lock, and the repository has
#    no threads at all (design doc §3).
hits=$(grep -nE 'pthread_|std::thread' ui/shell/kbd_*.c ui/shell/shell_kbd.c 2>/dev/null)
check "the driver creates no thread" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

echo "kbd_lint.sh: $failed failure(s)"
exit $((failed > 0))
