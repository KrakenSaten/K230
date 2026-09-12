#!/bin/bash
# The system alert's keyboard isolation and focus ownership (DS v0.1 §18.5,
# §18.6, §18.8) - the gate that stands between the physical keyboard and a
# shippable milestone.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). The binary under test
# is built beside it by ui/shell/CMakeLists.txt.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# 1. The alert over a real field, with real LVGL devices and the shell's own
#    suppression rule. The runtime saves acknowledged alarms, so it gets a
#    scratch state directory rather than the developer's.
BIN=${SHELL_ALARM_TEST:-$(dirname "$SHELL_BIN")/shell_alarm_test}
if [ -x "$BIN" ]; then
    log=$(POCKETOS_STATE_DIR=$(mktemp -d) POCKETOS_CONFIG_DIR=$(mktemp -d) \
          "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|^note:|shell_alarm_test:'
    check "keys cannot reach the app under an alert" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL shell_alarm_test binary missing: $BIN"; failed=$((failed + 1))
fi

# 2. DS §18.8: the alert owns the keyboard's show path while it is up. The
#    rule lives in one place, and the shell must ask it rather than keep a
#    second copy of the condition.
check "the shell asks the suppression rule before showing the keyboard" \
    "$(grep -q 'pocketos_shell_keyboard_may_show' ui/shell/shell.c && echo 1 || echo 0)"
check "and the alert is what raises and lowers it" \
    "$([ "$(grep -c 'pocketos_shell_keyboard_set_suppressed' ui/shell/shell_alarm.c)" -ge 2 ] && echo 1 || echo 0)"

# 3. The suppression state is shell-internal: an app must not be able to see
#    that an alert exists, let alone suppress the keyboard itself.
check "suppression is not part of the app API" \
    "$(grep -q 'suppress' ui/shell/app.h && echo 0 || echo 1)"
hits=$(grep -rnE 'keyboard_set_suppressed|keyboard_may_show|shell_kb_state' apps/ 2>/dev/null)
check "no app touches the suppression state" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

echo "alert_shell_test.sh: $failed failure(s)"
exit $((failed > 0))
