#!/bin/bash
# The Settings app's boundaries, checked in the source.
#
# Settings shows and changes things that belong to others: Wi-Fi is netd's
# and brightness is the shell's. The ways that goes wrong are a UI that grows
# its own hardware code, reads a file, or lets a passphrase leak - so those are
# what this refuses.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# Code lines only: the files explain their rules in prose as well.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@"; }

APP=apps/settings/settings_app.c
VIEW=apps/settings/settings_view.c
ALL="apps/settings/settings_app.c apps/settings/settings_view.c apps/settings/settings_view.h"

check "the view model has no LVGL" "$(code "$VIEW" apps/settings/settings_view.h | grep -q 'lvgl\|lv_' && echo 0 || echo 1)"
check "no file I/O anywhere in Settings" \
    "$(code $ALL | grep -qE '\b(fopen|open|openat|read|write|unlink|rename|mkdir)\s*\(' && echo 0 || echo 1)"
check "no sysfs path in Settings" "$(code $ALL | grep -q '/sys' && echo 0 || echo 1)"
check "brightness goes through the shell, never the HAL" \
    "$(code $ALL | grep -qE 'brightness_(probe|set_percent|get_percent)' && echo 0 || echo 1)"
check "and the shell entry points are what it uses" \
    "$(code "$APP" | grep -q 'pocketos_shell_brightness_set' && echo 1 || echo 0)"
check "Wi-Fi goes through netd over IPC only" \
    "$(code $ALL | grep -qE 'wpa_|udhcpc|wifi_store|wifi_mgr|netd_sys' && echo 0 || echo 1)"
check "every IPC call carries the UI deadline" \
    "$(code "$APP" | grep -q 'shell_ipc_call(' && echo 0 || echo 1)"
check "Settings never logs" "$(code $ALL | grep -qE 'LOG_(DEBUG|INFO|WARN|ERROR)|\bprintf\s*\(|\bfprintf|\bputs\s*\(' && echo 0 || echo 1)"
check "Settings creates no keyboard of its own" \
    "$(grep -q 'pos_keyboard' $ALL && echo 0 || echo 1)"
check "the passphrase field is masked" "$(code "$APP" | grep -q 'lv_textarea_set_password_mode(a->field, true)' && echo 1 || echo 0)"
check "the passphrase field is cleared when the sheet closes" \
    "$(sed -n '/^static void close_sheet/,/^}/p' "$APP" | grep -q 'clear_field' && echo 1 || echo 0)"
check "and when the app is destroyed" \
    "$(sed -n '/^static void settings_destroy/,/^}/p' "$APP" | grep -q 'clear_field' && echo 1 || echo 0)"
check "an open network is joined only with allow_open" \
    "$(code "$VIEW" | grep -q 'allow_open' && echo 1 || echo 0)"
check "Settings writes no settings store key itself" \
    "$(code $ALL | grep -q 'settings_set' && echo 0 || echo 1)"
check "Settings has no store" "$(ls apps/settings/*store* >/dev/null 2>&1 && echo 0 || echo 1)"

echo "settings_lint: $failed failure(s)"
exit $((failed > 0))
