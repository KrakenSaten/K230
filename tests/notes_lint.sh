#!/bin/bash
# PocketNotes layering, the same rules the other apps are held to.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

hits=$(grep -lE 'lvgl|lv_obj|lv_label' apps/notes/notes_view.c apps/notes/notes_view.h apps/notes/notes_store.c apps/notes/notes_store.h 2>/dev/null)
check "the text rules and the store are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

hits=$(grep -lE 'fopen|open\(|unlink|mkdir|rename|opendir' apps/notes/notes_view.c apps/notes/notes_app.c 2>/dev/null)
check "only notes_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# DS 17.4: an app asks the shell for the keyboard and never holds one.
hits=$(grep -nE 'pos_keyboard|lv_keyboard' apps/notes/*.c apps/notes/*.h 2>/dev/null)
check "Notes never names a keyboard (DS 17.4)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

check "Notes asks the shell for it instead" \
    "$(grep -q 'pocketos_shell_keyboard_show' apps/notes/notes_app.c && echo 1 || echo 0)"

# A title is user input; it must never reach a path.
check "note filenames are generated, not derived from a title" \
    "$(grep -q 'NOTES_FILE_FMT' apps/notes/notes_store.c && echo 1 || echo 0)"
hits=$(grep -nE 'title.*(fopen|path)|path.*title' apps/notes/notes_store.c 2>/dev/null)
check "no title ever reaches a filename" "$([ -z "$hits" ] && echo 1 || echo 0)"

# The write must stay atomic.
for want in 'fsync' 'rename' '\.tmp'; do
    check "the store write uses $want" \
        "$(grep -qE "$want" apps/notes/notes_store.c && echo 1 || echo 0)"
done

# Scope: the MVP has none of these. Prose is not code, so comment lines are
# dropped first - the editor's confirmation says a delete cannot be undone,
# and that sentence is not an undo system.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' apps/notes/*.c apps/notes/*.h 2>/dev/null; }
for banned in search folder tag undo sort; do
    hits=$(code | grep -niE "[a-z_]*${banned}[a-z_]*[[:space:]]*[(=]|${banned}_")
    check "no $banned machinery in the MVP" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done

# DS 22.3: the layout is chosen from the body the app is given, not from the
# orientation, and the corner clearance comes from the platform's description
# of the panel rather than a number of the app's own.
APP=apps/notes/notes_app.c
appcode() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$APP" | sed 's|/\*.*\*/||'; }
hits=$(appcode | grep -nE 'pocketos_shell_orientation|POS_ROTATION_|lv_display_get_rotation|landscape|portrait')
check "the layout never asks which way the display is turned" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the screens sit in one frame that is the body's content box" \
    "$(appcode | grep -q 'lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));' && echo 1 || echo 0)"
check "it is laid out again when the body changes size" \
    "$(appcode | grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "and the handler is gone before the app is freed" \
    "$(appcode | grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
check "with no gap between the screens for LVGL to take from the one on show" \
    "$(appcode | grep -q 'lv_obj_set_style_pad_row(frame, 0, 0);' && echo 1 || echo 0)"
check "the corner clearance is read from the display geometry" \
    "$(appcode | grep -q 'pocketui_display_geometry()' && echo 1 || echo 0)"
hits=$(appcode | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right')
check "by PocketUI's one rule for it, not a copy of the app's own" \
    "$(appcode | grep -q 'pos_display_rect_insets(pocketui_display_geometry(),' && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the wide shape keeps the content at least portrait-wide beside the rail" \
    "$(appcode | grep -q 'w > h && w >= NOTES_COLUMN_W + POCKETUI_PAD + NOTES_RAIL_W' && echo 1 || echo 0)"
hits=$(appcode | grep -nE 'lv_obj_clean\(')
check "the list rebuilds its rows, not the objects around them" "$([ -z "$hits" ] && echo 1 || echo 0)"

echo "notes_lint: $failed failure(s)"
exit $((failed > 0))
