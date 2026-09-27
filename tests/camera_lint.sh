#!/bin/bash
# Camera's boundaries (docs/apps/CAMERA.md, ADR-006), held statically.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

A=apps/camera
C=core/pocketcam
H=tools/camera/pos_camera.c
APP=$A/camera_app.c
GAL=$A/camera_gallery_screen.c

# ---- layering ------------------------------------------------------------------
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer|lv_image' $C/*.[ch] $H $A/camera_state.[ch] \
       $A/camera_layout.[ch] $A/camera_session.[ch] $A/camera_gallery.[ch] 2>/dev/null)
check "the camera layer, the helper, the state machines, the layout and the session are free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# ADR-002 and ADR-006: the app never touches the camera.
hits=$(code $A/*.c $A/*.h | grep -nE '/dev/video|videodev2|VIDIOC|ioctl\(|pocketcam_(open|start|next|still)\b|pocketcam\.h')
check "the app never names a video device, V4L2 or a backend call (only a backend's name, for the helper)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -nE 'pocketcam\.c|pocketcam_fake|pocketcam_convert|pocketcam_store|pocketcam_codec|pocketcam_image|pocketcam_exif' ui/shell/CMakeLists.txt)
check "the shell links no camera backend, converter, store, encoder or decoder" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $APP $GAL $A/camera_state.c $A/camera_layout.c $A/camera_gallery.c |
       grep -nE '\b(fopen|open|openat|unlink|unlinkat|rename|mkdir|opendir|readdir|stat|remove|fork|exec[lv]p?e?)\(')
check "the screens and the state machines never touch files or start processes" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rlE '\bfork\(' $A $C tools/camera --include='*.c')
check "only the session starts a process" "$([ "$hits" = "$A/camera_session.c" ] && echo 1 || echo 0)"
hits=$(code $C/*.c $H | grep -nE '\b(system|popen)\(')
check "no shell is ever run" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- the real backend (CAMERA_PLATFORM_RESEARCH.md §10) -----------------------------
hits=$(grep -lE 'VIDIOC|videodev2|/dev/video' $C/*.c $H | tr '\n' ' ')
check "only pocketcam_v4l2.c speaks V4L2 ($hits)" \
    "$([ "$hits" = "$C/pocketcam_v4l2.c " ] && echo 1 || echo 0)"
check "it never enumerates controls (the vvcam main path spins on it)" \
    "$(code $C/pocketcam_v4l2.c | grep -qE 'QUERYCTRL|QUERY_EXT_CTRL|ENUM_FMT' && echo 0 || echo 1)"
check "every wait on the driver is a bounded poll on a non-blocking node" \
    "$(grep -q 'O_RDWR | O_NONBLOCK | O_CLOEXEC' $C/pocketcam_v4l2.c && grep -q 'pr = poll(&p, 1, (int)left);' $C/pocketcam_v4l2.c &&
       echo 1 || echo 0)"
check "a frame is mapped read-only" \
    "$(grep -q 'PROT_READ, MAP_SHARED, n->fd' $C/pocketcam_v4l2.c && ! grep -q 'PROT_WRITE' $C/pocketcam_v4l2.c && echo 1 || echo 0)"
check "the device's default backend is the real one, never the fake" \
    "$(grep -q '#define CAMERA_BACKEND_DEFAULT "v4l2"' $A/camera_session.c &&
       grep -q 'return env && \*env ? env : "v4l2";' $H && echo 1 || echo 0)"
check "the simulator alone defaults to the fake" \
    "$(sed -n '/if(POCKETOS_DISPLAY STREQUAL "sdl")/,/endif()/p' ui/shell/CMakeLists.txt |
       grep -q 'CAMERA_BACKEND_DEFAULT="fake"' &&
       [ "$(grep -c 'CAMERA_BACKEND_DEFAULT="fake"' ui/shell/CMakeLists.txt)" = 1 ] && echo 1 || echo 0)"
check "and a fake picture is always labelled SIMULATED" \
    "$(grep -q 'out->hint = m->simulated ? "SIMULATED" : "";' $A/camera_state.c && echo 1 || echo 0)"

# ---- lifetime ------------------------------------------------------------------
check "the helper leaves with the shell (PR_SET_PDEATHSIG)" \
    "$(grep -q 'prctl(PR_SET_PDEATHSIG, SIGTERM);' $A/camera_session.c && echo 1 || echo 0)"
check "the shared memory is sealed before the helper sees it" \
    "$(grep -q 'F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL' $A/camera_session.c &&
       grep -q 'PROT_READ, MAP_SHARED' $A/camera_session.c && echo 1 || echo 0)"
# Waits on the LVGL thread, all bounded: destroy, Try again, and going to the
# gallery (the camera's helper); starting and leaving it (the library's).
check "the helper is polled from a timer, never waited for, except when leaving" \
    "$(code $APP | grep -q 'lv_timer_create(on_poll, CAMERA_POLL_MS, a)' &&
       [ "$(code $APP | grep -c 'camera_session_abandon(')" = 3 ] &&
       [ "$(code $GAL | grep -c 'camera_session_abandon(')" = 2 ] && echo 1 || echo 0)"
destroy=$(awk '/^static void camera_destroy\(/,/^}/' $APP)
order=$(printf '%s\n' "$destroy" | grep -nE 'lv_timer_delete|camera_session_abandon|lv_image_set_src\(a->img, NULL\)|picture_free\(&a->preview\)' | cut -d: -f1 | tr '\n' ' ')
check "destroy: timer, then helper, then images detached, then buffers freed (lines $order)" \
    "$(set -- $order; [ $# = 4 ] && [ "$1" -lt "$2" ] && [ "$2" -lt "$3" ] && [ "$3" -lt "$4" ] && echo 1 || echo 0)"
gorder=$(printf '%s\n' "$destroy" | grep -nE 'gallery_ui_leave|camera_session_abandon|gallery_ui_destroy|free\(a\)' | cut -d: -f1 | tr '\n' ' ')
check "destroy: the gallery's helper goes with the camera's, before anything is freed (lines $gorder)" \
    "$(set -- $gorder; [ $# = 4 ] && [ "$1" -lt "$2" ] && [ "$2" -lt "$3" ] && [ "$3" -lt "$4" ] && echo 1 || echo 0)"
entry=$(awk '/^static void enter_gallery\(/,/^}/' $APP)
eorder=$(printf '%s\n' "$entry" | grep -nE 'camera_session_abandon|gallery_ui_enter' | cut -d: -f1 | tr '\n' ' ')
check "the camera is closed before the gallery's helper starts (lines $eorder)" \
    "$(set -- $eorder; [ $# = 2 ] && [ "$1" -lt "$2" ] && echo 1 || echo 0)"
check "the library helper never opens the camera" \
    "$(awk '/^static int run_library\(/,/^}/' $H | grep -qE 'open_camera|pocketcam_open|pocketcam_start' && echo 0 || echo 1)"
check "and is started without a backend or a fake script" \
    "$(grep -q 'argv\[argc++\] = "library";' $A/camera_session.c &&
       grep -q 'cfg && !cfg->library && cfg->fake' $A/camera_session.c && echo 1 || echo 0)"
hits=$(code $A/*.c $A/*.h | grep -nE 'jpeglib|jpeg_|pocketcam_image|pocketcam_exif')
check "the app decodes nothing itself: every picture comes from the helper" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "the size handler is gone before the app is freed" \
    "$(code $APP | grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
check "pictures are copied out of the shared memory, never pointed at" \
    "$(code $APP $GAL | grep -q 'shm' && echo 0 || echo 1)"
check "no callback crosses from the session into the app" \
    "$(grep -qE '\(\*[a-z_]+\)\(' $A/camera_session.h && echo 0 || echo 1)"

# ---- layout (DS 21.2, 22.3) ------------------------------------------------------
check "the layout arithmetic never asks which way the display is turned" \
    "$(code $A/camera_layout.c | grep -qE 'orientation|rotation|portrait|landscape' && echo 0 || echo 1)"
check "the screen asks only once, for the photo's shape and the sensor's turn" \
    "$([ "$(code $APP | grep -c 'pocketos_shell_orientation(')" = 1 ] && echo 1 || echo 0)"
check "the corner clearance comes from PocketUI's one rule" \
    "$(code $APP | grep -q 'pocketui_layout_begin(&a->guard, a->frame, &in)' && echo 1 || echo 0)"
check "it is laid out again when the body changes size" \
    "$(code $APP | grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "the gallery holds one page of thumbnails, at most as many as its layout has cells" \
    "$(grep -q '#define GALLERY_PAGE_MAX 24' $A/camera_gallery.h &&
       grep -q '#define GALLERY_CELLS_MAX 24 ' $A/camera_layout.h && echo 1 || echo 0)"
lib_app=$(sed -nE 's/^#define CAMERA_LIBRARY_MAX ([0-9]+).*/\1/p' $A/camera_session.h)
lib_core=$(sed -nE 's/^#define POCKETCAM_LIBRARY_MAX ([0-9]+)u.*/\1/p' $C/pocketcam_store.h)
check "the app takes as long a list as the helper sends ($lib_app = $lib_core)" \
    "$([ -n "$lib_app" ] && [ "$lib_app" = "$lib_core" ] && echo 1 || echo 0)"
year_app=$(sed -nE 's/^#define CAMERA_DATE_YEAR_MIN ([0-9]+).*/\1/p' $A/camera_session.h)
year_core=$(sed -nE 's/^#define POCKETCAM_EXIF_YEAR_MIN ([0-9]+).*/\1/p' $C/pocketcam_exif.h)
check "a date in a name is believed from the year an EXIF date is ($year_app = $year_core)" \
    "$([ -n "$year_app" ] && [ "$year_app" = "$year_core" ] && echo 1 || echo 0)"
check "the picture fits a shared-memory slot" \
    "$(grep -q '#define CAMERA_PICTURE_MAX 1024' $A/camera_layout.h &&
       grep -q '#define POCKETCAM_VIEW_MAX_W 1024' $C/pocketcam_proto.h && echo 1 || echo 0)"

# ---- store ---------------------------------------------------------------------
clock=$(sed -nE 's/^#define CLOCK_WALL_VALID_FROM ([0-9]+)LL.*/\1/p' apps/clock/clock_engine.h)
cam=$(sed -nE 's/^#define POCKETCAM_WALL_VALID_FROM ([0-9]+)LL.*/\1/p' $C/pocketcam_store.h)
check "a photo's date uses PocketClock's floor for a valid clock ($cam = $clock)" \
    "$([ -n "$cam" ] && [ "$cam" = "$clock" ] && echo 1 || echo 0)"
check "photos are written to a temporary and renamed into place" \
    "$(grep -q 'O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC' $C/pocketcam_store.c &&
       grep -q 'rename(tmp, final)' $C/pocketcam_store.c && grep -q 'fsync(k.fd)' $C/pocketcam_store.c &&
       echo 1 || echo 0)"
check "an export never replaces a file in Files (link, not rename)" \
    "$(awk '/^int pocketcam_store_export\(/,/^}/' $C/pocketcam_store.c | grep -q 'link(tmp, final)' &&
       awk '/^int pocketcam_store_export\(/,/^}/' $C/pocketcam_store.c | grep -q 'O_WRONLY | O_CREAT | O_EXCL' &&
       echo 1 || echo 0)"
check "a date is written into a photo only when the clock is valid" \
    "$(grep -q 'taken < POCKETCAM_WALL_VALID_FROM' $C/pocketcam_exif.c && echo 1 || echo 0)"
check "nothing is ever deleted to make room" \
    "$(awk '/^int pocketcam_store_room\(/,/^}/' $C/pocketcam_store.c | grep -q 'unlink' && echo 0 || echo 1)"

# ---- scope of v1 -------------------------------------------------------------------
hits=$(code $A/*.[ch] $C/*.[ch] $H | grep -niE 'record|h264|mp4|qr|zbar|filter|upload|AF_INET|curl|http|face')
check "no video, QR, filters, AI or network (QR: docs/apps/CAMERA.md, not yet)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(grep -nE 'pos_keyboard|lv_keyboard|keyboard_show' $A/*.c 2>/dev/null)
check "Camera never names a keyboard (DS 17.4)" "$([ -z "$hits" ] && echo 1 || echo 0)"

echo "camera_lint: $failed failure(s)"
exit $((failed > 0))
