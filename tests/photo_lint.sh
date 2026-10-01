#!/bin/bash
# Photo's boundaries (docs/apps/PHOTO.md, DS §45), held statically: one
# library and one gallery, shared with Camera; no camera; no file touched on
# the LVGL thread; deletes only of a regular photo file in the photo folder.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

P=apps/photo
A=apps/camera
APP=$P/photo_app.c

# ---- one gallery -----------------------------------------------------------------
check "Photo is one file, the app descriptor and its host: no library code of its own" \
    "$([ "$(ls $P)" = "photo_app.c" ] && echo 1 || echo 0)"
check "it hosts Camera's gallery screen" \
    "$(code $APP | grep -q 'gallery_ui_create(root, &a->session)' &&
       code $APP | grep -q 'gallery_ui_enter(a->gallery)' && echo 1 || echo 0)"
check "standalone: no CAMERA in it" \
    "$(code $APP | grep -q 'gallery_ui_set_standalone(a->gallery, true)' && echo 1 || echo 0)"
hits=$(code $APP | grep -nE '\bcamera_gallery_(init|open|event|set_list|tap_[a-z]+|screen|left|right|middle)\(|pocketcam_(store|image|exif|codec)_')
check "it does not drive the gallery's model or the store itself" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(grep -rlE 'pocketcam_store_list|pocketcam_image_decode' apps ui --include='*.c' | grep -v '^core/')
check "no app lists or decodes the library itself (the helper does)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- no camera, no files, no processes -----------------------------------------------
hits=$(code $APP | grep -nE 'camera_session_(start|preview|capture|view)|cfg\.|/dev/video|VIDIOC|pocketcam\.h')
check "Photo never starts a helper itself, and never a camera" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "the gallery's only helper is the library one (cfg.library)" \
    "$([ "$(code $A/camera_gallery_screen.c | grep -c 'camera_session_start(')" = 1 ] &&
       code $A/camera_gallery_screen.c | grep -q 'cfg.library = true;' && echo 1 || echo 0)"
hits=$(code $APP | grep -nE '\b(fopen|open|openat|unlink|unlinkat|rename|mkdir|opendir|readdir|stat|lstat|remove|fork|exec[lv]p?e?|system|popen)\(')
check "Photo's screen never touches a file or starts a process" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- the delete boundary ---------------------------------------------------------------
sed -n '/^int pocketcam_store_delete(/,/^}/p' core/pocketcam/pocketcam_store.c > /tmp/photo_lint_del.$$
check "a delete takes only a photo's name (no '/', no '..')" \
    "$(grep -q 'if (!pocketcam_store_valid_name(name)) {' /tmp/photo_lint_del.$$ && echo 1 || echo 0)"
check "and removes only a regular file, never a folder or a link" \
    "$(grep -q 'lstat(path, &st)' /tmp/photo_lint_del.$$ && grep -q '!S_ISREG(st.st_mode)' /tmp/photo_lint_del.$$ &&
       echo 1 || echo 0)"
check "with unlink, never anything recursive" \
    "$(grep -q 'unlink(path)' /tmp/photo_lint_del.$$ &&
       ! grep -qE 'rmdir|nftw|ftw\(|rm -r|remove\(' /tmp/photo_lint_del.$$ && echo 1 || echo 0)"
rm -f /tmp/photo_lint_del.$$
check "the library lists regular files only, links not followed" \
    "$(sed -n '/^int pocketcam_store_list(/,/^}/p' core/pocketcam/pocketcam_store.c |
       grep -q 'AT_SYMLINK_NOFOLLOW) != 0 ||' && echo 1 || echo 0)"
check "the photo folder is Camera's, not a second store" \
    "$(! grep -rqE 'POCKETCAM_STORE_SUBDIR|/var/lib/pocketos/(camera|photo)' $P && echo 1 || echo 0)"

# ---- registered like any other app -----------------------------------------------------
check "Photo is in the shell's registry" "$(grep -q '&app_photo' ui/shell/shell.c && echo 1 || echo 0)"
check "on the launcher in the Apps folder, the files colour (DS §47)" \
    "$(grep -q '{ "photo", HOME_GROUP_FOLDERS, HOME_HUE_FILES, HOME_FOLDER_APPS }' ui/shell/home_layout.c &&
       echo 1 || echo 0)"
check "fullscreen, like Camera (DS §30.8)" \
    "$(grep -q '.chrome = POCKETOS_CHROME_NONE' $APP && echo 1 || echo 0)"
check "its icon is the icon extension's Gallery, as its mask and its portal icon" \
    "$(grep -q 'APP_IDS = {EXTENSION + "gallery.png": "photo"}' tools/design/gen_app_icons.py &&
       grep -q '"photo": (EXTENSION + "gallery.svg", "files"),' tools/design/gen_doors_ui.py &&
       grep -q 'pos_app_icon_photo = {' ui/pocketui/pos_app_icons.c &&
       [ -f ui/assets/doors/icon-photo.bin ] && echo 1 || echo 0)"
check "the shell builds it with the gallery it hosts" \
    "$(grep -q 'apps/photo/photo_app.c' ui/shell/CMakeLists.txt &&
       grep -q 'apps/camera/camera_gallery_screen.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

# ---- scope (docs/apps/PHOTO.md "What it is not") ----------------------------------------
hits=$(code $APP | grep -niE 'crop|filter|rotate|album|favou?rite|upload|share|curl|socket|video')
check "v0.1 scope: no editing, albums, favourites, sharing or video" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

echo "photo_lint: $failed failure(s)"
exit $((failed > 0))
