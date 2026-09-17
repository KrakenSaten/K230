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

# DS 22.3, 24: the layout is chosen from the body the app is given, not from
# the orientation - which Settings shows and stores for the shell, but never
# lays itself out by - and the corner clearance comes from the platform's
# description of the panel rather than a number of the app's own.
layout() { sed -n '/^\/\* ---- the layout/,/^static void rebuild/p' "$APP" | grep -vE '^[[:space:]]*(/\*|\*|//)'; }
hits=$(layout | grep -nE 'orientation|rotation|POS_ROTATION_|POCKETOS_ROTATION_|a->rot|landscape|portrait')
check "the layout never asks which way the display is turned" \
    "$([ -n "$(layout)" ] && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the screen sits in one frame that is the body's content box" \
    "$(code "$APP" | grep -q 'lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));' && echo 1 || echo 0)"
check "it is shaped again when the body changes size" \
    "$(code "$APP" | grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
destroy=$(sed -n '/^static void settings_destroy/,/^}/p' "$APP")
check "and nothing calls back into the app once it is freed" \
    "$(printf '%s\n' "$destroy" | grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' &&
       printf '%s\n' "$destroy" | grep -q 'lv_async_call_cancel(reveal_field_later, a);' && echo 1 || echo 0)"
hits=$(code "$APP" | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right')
check "the corner clearance is PocketUI's one rule, read from the display geometry" \
    "$(code "$APP" | grep -q 'pos_display_rect_insets(pocketui_display_geometry(),' && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the wide shape keeps both columns at least portrait-wide" \
    "$(code "$APP" | grep -q 'w > h && w >= 2 \* SETTINGS_COLUMN_W + SETTINGS_PANEL_GAP' && echo 1 || echo 0)"

echo "settings_lint: $failed failure(s)"
exit $((failed > 0))
