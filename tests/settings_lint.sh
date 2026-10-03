#!/bin/bash
# The Settings app's boundaries, checked in the source.
#
# Settings shows and changes things that belong to others: Wi-Fi is netd's
# and everything else - brightness, sound, Power & Sleep, the time zone, the
# overlay - is the shell's. The ways that goes wrong are a UI that grows
# its own hardware code, reads a file, or lets a passphrase leak - so those are
# what this refuses.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# Code lines only: the files explain their rules in prose as well.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@"; }

APP=apps/settings/settings_app.c
WIFI=apps/settings/settings_wifi.c
VIEW=apps/settings/settings_view.c
ALL="apps/settings/settings_app.c apps/settings/settings_pages.c apps/settings/settings_wifi.c
     apps/settings/settings_view.c apps/settings/settings_view.h apps/settings/settings_internal.h"

check "the view model has no LVGL" "$(code "$VIEW" apps/settings/settings_view.h | grep -q 'lvgl\|lv_' && echo 0 || echo 1)"
check "no file I/O anywhere in Settings" \
    "$(code $ALL | grep -qE '\b(fopen|open|openat|read|write|unlink|rename|mkdir)\s*\(' && echo 0 || echo 1)"
check "no sysfs path in Settings" "$(code $ALL | grep -q '/sys' && echo 0 || echo 1)"
check "brightness goes through the shell, never the HAL" \
    "$(code $ALL | grep -qE 'brightness_(probe|set_percent|get_percent)' && echo 0 || echo 1)"
check "and the shell entry points are what it uses" \
    "$(code $ALL | grep -q 'pocketos_shell_brightness_set' && echo 1 || echo 0)"
check "Wi-Fi goes through netd over IPC only" \
    "$(code $ALL | grep -qE 'wpa_|udhcpc|wifi_store|wifi_mgr|netd_sys' && echo 0 || echo 1)"
check "every IPC call carries the UI deadline" \
    "$(code $ALL | grep -q 'shell_ipc_call(' && echo 0 || echo 1)"
check "Settings never logs" "$(code $ALL | grep -qE 'LOG_(DEBUG|INFO|WARN|ERROR)|\bprintf\s*\(|\bfprintf|\bputs\s*\(' && echo 0 || echo 1)"
check "Settings creates no keyboard of its own" \
    "$(grep -q 'pos_keyboard' $ALL && echo 0 || echo 1)"
check "the passphrase field is masked" "$(code "$WIFI" | grep -q 'lv_textarea_set_password_mode(a->w.field, true)' && echo 1 || echo 0)"
check "the passphrase field is cleared when the sheet closes" \
    "$(sed -n '/^void settings_sheet_close/,/^}/p' "$WIFI" | grep -q 'clear_field' && echo 1 || echo 0)"
check "and when the app is destroyed" \
    "$(sed -n '/^static void settings_destroy/,/^}/p' "$APP" | grep -q 'settings_wifi_teardown(a);' &&
       sed -n '/^void settings_wifi_teardown/,/^}/p' "$WIFI" | grep -q 'clear_field' && echo 1 || echo 0)"
check "an open network is joined only with allow_open" \
    "$(code "$VIEW" | grep -q 'allow_open' && echo 1 || echo 0)"
check "Settings writes no settings store key itself" \
    "$(code $ALL | grep -qE '\bsettings_(set|init)\(' && echo 0 || echo 1)"
check "Power & Sleep, the time zone and the overlay are set through the shell" \
    "$(code $ALL | grep -q 'pocketos_shell_set_screen_off_after' && code $ALL | grep -q 'pocketos_shell_set_timezone' &&
       code $ALL | grep -q 'pocketos_shell_set_debug_overlay' && echo 1 || echo 0)"
check "and Settings applies no time zone of its own" \
    "$(code $ALL | grep -qE '\b(setenv|tzset|putenv)\s*\(' && echo 0 || echo 1)"
# DS §52.2: one box scrolls on a page - the page. Nothing Settings builds is
# made to scroll; boxes it makes clear the flag.
check "Settings makes nothing scroll but the page" \
    "$(code $ALL | grep -qE 'lv_obj_add_flag\([^)]*SCROLLABLE' && echo 0 || echo 1)"
check "Settings has no store" "$(ls apps/settings/*store* >/dev/null 2>&1 && echo 0 || echo 1)"

# DS 22.3, 24: the layout is chosen from the body the app is given, not from
# the orientation - which Settings shows and stores for the shell, but never
# lays itself out by - and the corner clearance comes from the platform's
# description of the panel rather than a number of the app's own.
layout() { sed -n '/^\/\* ---- the layout/,/^void settings_rebuild/p' "$APP" | grep -vE '^[[:space:]]*(/\*|\*|//)'; }
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
       sed -n '/^void settings_wifi_teardown/,/^}/p' "$WIFI" | grep -q 'lv_async_call_cancel(reveal_field_later, a);' &&
       echo 1 || echo 0)"
hits=$(code "$APP" | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right|pos_display_rect_insets')
check "the corner clearance is PocketUI's one rule, taken through the shared layout guard" \
    "$(code "$APP" | grep -q 'pocketui_layout_begin(&a->layout_guard, a->frame' && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the wide shape keeps both columns at least portrait-wide" \
    "$(code "$APP" | grep -q 'w > h && w >= 2 \* SETTINGS_COLUMN_W + SETTINGS_PANEL_GAP' && echo 1 || echo 0)"

echo "settings_lint: $failed failure(s)"
exit $((failed > 0))
