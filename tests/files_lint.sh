#!/bin/bash
# Files layering and safety rules, held statically.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

D=apps/files
APP=$D/files_app.c

hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $D/files_fs.[ch] $D/files_job.[ch] $D/files_view.[ch] 2>/dev/null)
check "the filesystem layer, the worker and the text are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# Only files_fs.c touches the filesystem; the app goes through it, and the
# worker, so no file operation can run in LVGL's layout or draw path.
hits=$(code $APP $D/files_view.c $D/files_job.c | grep -nE '\b(fopen|open|openat|unlink|unlinkat|rmdir|mkdir|rename|renameat|opendir|readdir|stat|lstat|remove|symlink|chmod|truncate)\(')
check "only files_fs.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "copy, move and delete go to the worker, not the LVGL thread" \
    "$(code $APP | grep -qE 'files_(copy|move|delete)\(' && echo 0 || echo 1)"
check "the app starts them through files_job_start" \
    "$(code $APP | grep -q 'files_job_start(' && echo 1 || echo 0)"
check "and collects them from its timer" \
    "$(code $APP | grep -q 'files_job_poll(&a->job' && echo 1 || echo 0)"
check "no thread outlives the app" \
    "$(code $APP | grep -q 'files_job_abandon(&a->job)' && echo 1 || echo 0)"
hits=$(code $D/files_job.c $D/files_fs.c | grep -nE 'lv_|pocketos_shell_')
check "the worker never calls into LVGL or the shell" "$([ -z "$hits" ] && echo 1 || echo 0)"

# Nothing is ever replaced, and a copy is only ever renamed into place.
check "renames never replace (RENAME_NOREPLACE)" \
    "$(grep -q 'RENAME_NOREPLACE' $D/files_fs.c && echo 1 || echo 0)"
check "a copied file is created exclusively" \
    "$(grep -q 'O_CREAT | O_EXCL' $D/files_fs.c && echo 1 || echo 0)"
check "a copy is synced before a move removes its original" \
    "$(grep -q 'fsync(out)' $D/files_fs.c && echo 1 || echo 0)"
check "deletes never follow a link" \
    "$(grep -q 'O_DIRECTORY | O_NOFOLLOW' $D/files_fs.c && grep -q 'AT_SYMLINK_NOFOLLOW' $D/files_fs.c && echo 1 || echo 0)"
hits=$(code $D/files_fs.c | grep -nE '\b(system|popen|exec[lv]p?e?)\(')
check "no shell is ever run" "$([ -z "$hits" ] && echo 1 || echo 0)"

# Every changing operation asks the policy itself.
for op in files_mkdir files_rename files_copy files_move files_delete; do
    body=$(awk "/^int $op\\(/,/^}/" $D/files_fs.c)
    check "$op checks the policy itself" \
        "$(printf '%s\n' "$body" | grep -qE 'files_policy_(entry|dir)\(' && echo 1 || echo 0)"
done
check "Doors' own directories are protected by default" \
    "$(grep -q 'pocketos_state_dir()' $D/files_fs.c && grep -q 'pocketos_config_dir()' $D/files_fs.c && echo 1 || echo 0)"

# Scope of v1.
hits=$(code $D/*.c $D/*.h | grep -niE 'zip|thumbnail|smb|nfs|cifs|lv_image_set_src|png|jpe?g')
check "no archives, shares, thumbnails or image preview in v1" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# DS 17.4: the keyboard is the shell's.
hits=$(grep -nE 'pos_keyboard|lv_keyboard' $D/*.c $D/*.h 2>/dev/null)
check "Files never names a keyboard (DS 17.4)" "$([ -z "$hits" ] && echo 1 || echo 0)"

# DS 22.3: the layout is chosen from the body the app is given.
hits=$(code $APP | grep -nE 'pocketos_shell_orientation|POS_ROTATION_|lv_display_get_rotation|landscape|portrait')
check "the layout never asks which way the display is turned" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the screens sit in one frame that is the body's content box" \
    "$(code $APP | grep -q 'lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));' && echo 1 || echo 0)"
check "it is laid out again when the body changes size" \
    "$(code $APP | grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "and the handler is gone before the app is freed" \
    "$(code $APP | grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
check "the corner clearance comes from PocketUI's one rule" \
    "$(code $APP | grep -q 'pocketui_layout_begin(&a->layout_guard, a->frame' && echo 1 || echo 0)"
check "the wide shape keeps the list at least portrait-wide beside the pane" \
    "$(code $APP | grep -q 'w > h && w >= FILES_LIST_MIN_W + POCKETUI_PAD + FILES_SIDE_W' && echo 1 || echo 0)"

# A row is not rebuilt from inside its own event.
check "opening from a row is deferred to the timer" \
    "$(awk '/^static void on_row\(/,/^}/' $APP | grep -q 'a->open_pending = true' && echo 1 || echo 0)"

echo "files_lint: $failed failure(s)"
exit $((failed > 0))
