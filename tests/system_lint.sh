#!/bin/bash
# The System app's boundaries, checked in the source.
#
# System shows what sysd reports and can ask sysd to restart or power off the
# machine. The ways that goes wrong are a screen that reads the machine
# itself, a call that can hang the UI, or a power method reached without a
# confirmation - and, since it has two shapes, a layout that asks which way
# the display is turned or carries its own idea of the rounded corners. So
# those are what this refuses.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# Code lines only: the files explain their rules in prose as well.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@"; }

APP=apps/system/system_app.c
VIEW=apps/system/system_view.c
ALL="apps/system/system_app.c apps/system/system_net.c apps/system/system_internal.h apps/system/system_view.c
     apps/system/system_view.h apps/system/diag_view.c apps/system/diag_view.h"

check "the view model has no LVGL" "$(code "$VIEW" apps/system/system_view.h | grep -q 'lvgl\|lv_' && echo 0 || echo 1)"
check "nor does the Diagnostics model" "$(code apps/system/diag_view.c apps/system/diag_view.h | grep -q 'lvgl\|lv_' && echo 0 || echo 1)"
check "System reads nothing from the machine itself: no file I/O" \
    "$(code $ALL | grep -qE '\b(fopen|open|openat|read|write|opendir|statvfs)\s*\(' && echo 0 || echo 1)"
check "and no /proc, /sys or /run path" "$(code $ALL | grep -qE '"/(proc|sys|run)' && echo 0 || echo 1)"
check "every IPC call carries the UI deadline" \
    "$(code $ALL | grep -q 'shell_ipc_call(' && echo 0 || echo 1)"
# DS §52.2: one box scrolls on a page - the page. Nothing System builds is
# made to scroll; boxes it makes clear the flag.
check "System makes nothing scroll but the page" \
    "$(code $ALL | grep -qE 'lv_obj_add_flag\([^)]*SCROLLABLE' && echo 0 || echo 1)"
hits=$(code "$APP" apps/system/system_net.c | grep -nE '"system\.(reboot|poweroff)"')
check "the app never names a power method: it calls only what a confirmation hands over" \
    "$([ -z "$hits" ] && [ "$(code "$APP" | grep -c 'system_view_confirm(')" = 1 ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "System creates no keyboard and asks for none" \
    "$(code $ALL | grep -qE 'pos_keyboard|lv_keyboard|pocketos_shell_keyboard' && echo 0 || echo 1)"
check "System has no store" "$(ls apps/system/*store* >/dev/null 2>&1 && echo 0 || echo 1)"

# DS 21.2, 22.3: the layout is chosen from the body the app is given, not from
# the orientation, and the corner clearance comes from the platform's
# description of the panel rather than a number of the app's own.
layout() { sed -n '/^\/\* ---- the layout/,/^static void rebuild(struct system_app \*a)$/p' "$APP" | code; }
hits=$(layout | grep -nE 'orientation|rotation|POS_ROTATION_|POCKETOS_ROTATION_|landscape|portrait')
check "the layout never asks which way the display is turned" \
    "$([ -n "$(layout)" ] && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the screen sits in one frame that is the body's content box" \
    "$(code "$APP" | grep -q 'lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));' && echo 1 || echo 0)"
check "it is arranged again when the body changes size" \
    "$(code "$APP" | grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "and arranged whenever it is built" \
    "$(sed -n '/^static void rebuild(struct system_app \*a)$/,/^}/p' "$APP" | code | grep -q '^    arrange(a);' && echo 1 || echo 0)"
check "nothing calls back into the app once it is freed" \
    "$(sed -n '/^static void system_destroy/,/^}/p' "$APP" |
       grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
hits=$(code "$APP" | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right|pos_display_rect_insets')
check "the corner clearance is PocketUI's one rule, taken through the shared layout guard" \
    "$(code "$APP" | grep -q 'pocketui_layout_begin(&a->layout_guard, a->frame' && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the wide shape keeps both columns at least portrait-wide" \
    "$(code "$APP" | grep -q 'w > h && w >= 2 \* SYSTEM_COLUMN_W + SYSTEM_PANEL_GAP' && echo 1 || echo 0)"

echo "system_lint: $failed failure(s)"
exit $((failed > 0))
