#!/bin/bash
# Vision's boundaries (docs/apps/VISION.md), held statically: the app never
# touches the camera or the detector (ADR-002, ADR-006 by reuse); only the
# helper does, through Camera's own camera layer; nncase is spoken in exactly
# one file, behind a C interface; the pipeline core is pure and bounded; the
# screen blocks on nothing; and the shell links nothing of the KPU.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

A=apps/vision
C=core/pocketvision
H=tools/vision/pos_vision.c
APP=$A/vision_app.c
CORE="$C/vision_decode.c $C/vision_nms.c $C/vision_track.c $C/vision_line.c $C/vision_traffic.c $C/vision_geom.c $C/vision_labels.c $C/vision_pixels.c"

# ---- layering ------------------------------------------------------------------
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer|lv_image' $C/*.[ch] $C/*.cpp $H $A/vision_model.[ch] \
       $A/vision_layout.[ch] $A/vision_session.[ch] $A/vision_settings.[ch] $A/vision_store.[ch] 2>/dev/null)
check "the pipeline, the helper, the model, the layout, the session, the settings and the store are free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
hits=$(code $A/*.c $A/*.h | grep -nE '/dev/video|videodev2|VIDIOC|ioctl\(|pocketcam_(open|start|next|still)\b|pocketcam\.h|vision_kpu|nncase|kmodel_')
check "the app never names a video device, V4L2, a backend call or the detector" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -nE 'pocketcam\.c|pocketcam_fake|pocketcam_convert|pocketcam_v4l2|vision_kpu|vision_decode|vision_nms|vision_track|vision_line|vision_geom|nncase|Nncase|functional_k230|mmz' ui/shell/CMakeLists.txt)
check "the shell links no camera backend, converter, detector or pipeline stage" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(for f in $(find apps core tools ui services -name '*.c' -o -name '*.cpp' -o -name '*.h' 2>/dev/null); do
           code "$f" | grep -qE '#include <nncase|ai2d_builder|nncase::|runtime_tensor' && echo "$f"; done |
       grep -v "$C/vision_kpu_nncase.cpp")
check "nncase is spoken in vision_kpu_nncase.cpp and nowhere else" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "and only through the C interface (no nncase type in vision_kpu.h)" \
    "$(code $C/vision_kpu.h | grep -qE 'nncase|runtime_tensor|interpreter' && echo 0 || echo 1)"
hits=$(code $CORE | grep -nE '#include <(stdio|unistd|fcntl|sys/|time|pthread)\.h>|\b(malloc|calloc|realloc|free|fopen|open|read|write|clock_gettime|time)\(')
check "the pipeline core allocates nothing, reads no clock and does no I/O" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $CORE | grep -nE '\bfloat\b|\bdouble\b' | grep -v vision_decode.c)
check "only the decoder reads floats (the tensor); the rest is integer" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $APP $A/vision_model.c $A/vision_layout.c $A/vision_settings.c |
       grep -nE '\b(fopen|open|openat|unlink|rename|mkdir|opendir|fork|exec[lv]p?e?|usleep|sleep|nanosleep|waitpid|fsync)\(')
check "the screen, the model, the layout and the settings never touch files, start processes or wait" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -lE '\b(fopen|open|rename|unlink|fsync|pocketos_mkdir_p)\(' $A/*.c | grep -vE "$A/vision_(store|session)\.c")
check "in the app only vision_store.c touches files (and the session its socket and shared memory)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "the settings are private: a 0700 directory, 0600 files, written whole or not at all" \
    "$(grep -q 'pocketos_mkdir_p(vision_store_dir(), 0700)' $A/vision_store.c &&
       grep -q 'O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600' $A/vision_store.c &&
       grep -q 'rename(tmp, path)' $A/vision_store.c && echo 1 || echo 0)"
hits=$(grep -rlE '\bfork\(' $A $C tools/vision --include='*.c' --include='*.cpp')
check "only the session starts a process" "$([ "$hits" = "$A/vision_session.c" ] && echo 1 || echo 0)"
hits=$(code $C/*.c $C/*.cpp $H | grep -nE '\b(system|popen)\(')
check "no shell is ever run" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(grep -nE 'pthread|std::thread|thrd_' $A/*.c $C/*.c $C/*.cpp $H)
check "no threads anywhere in Vision" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- bounds --------------------------------------------------------------------
check "every list has a constant bound" \
    "$(grep -q '#define VISION_MAX_CANDIDATES 256' $C/pocketvision.h && grep -q '#define VISION_MAX_DETECTIONS 32' $C/pocketvision.h &&
       grep -q '#define VISION_MAX_TRACKS 32' $C/pocketvision.h && grep -q '#define VISION_MAX_SHOWN 24' $C/pocketvision_proto.h && echo 1 || echo 0)"
check "a det line with every box fits the protocol line" \
    "$([ $((24 * 52 + 32)) -le 2048 ] && grep -q '#define VISION_LINE_MAX 2048' $C/pocketvision_proto.h && echo 1 || echo 0)"
check "the tracker's per-frame table is the two bounds, not the scene" \
    "$(grep -q 'static uint32_t iou\[VISION_MAX_TRACKS\]\[VISION_MAX_DETECTIONS\];' $C/vision_track.c && echo 1 || echo 0)"
check "the screen makes its outline objects, its three lines and the colour mark once, VISION_MAX_SHOWN outlines" \
    "$(grep -q 'lv_obj_t \*outline\[VISION_MAX_SHOWN\];' $APP && [ "$(code $APP | grep -c 'lv_obj_create(a->box)')" = 3 ] &&
       [ "$(code $APP | grep -c 'line_object(a->box')" = 4 ] && echo 1 || echo 0)"
check "the pixel modes keep their working rows static and bounded by the widest picture" \
    "$(grep -q 'static uint8_t luma\[3\]\[VISION_PIXELS_MAX_W\];' $C/vision_pixels.c && grep -q '#define VISION_PIXELS_MAX_W 1024' $C/vision_pixels.h &&
       grep -q '#define POCKETCAM_VIEW_MAX_W 1024' core/pocketcam/pocketcam_proto.h && echo 1 || echo 0)"
check "the traffic core knows no detector: classes are mapped by name, the app's names are its own" \
    "$(! code $C/vision_traffic.c | grep -qE 'vision_label|coco|yolo|kmodel' && ! grep -qE 'vision_traffic\.[ch]' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

# ---- the camera is Camera's ------------------------------------------------------
check "the helper opens the camera through pocketcam and nothing else" \
    "$(grep -q 'pocketcam_open(cam, backend, config, info)' $H && ! code $H | grep -qE 'VIDIOC|/dev/video' && echo 1 || echo 0)"
check "it closes the camera before the detector on the way out" \
    "$(awk '/^static int run_session\(/,/^}/' $H | grep -nE 'pocketcam_close|vision_kpu_close' | cut -d: -f1 | tr '\n' ' ' |
       awk '{ exit !($1 < $2) }' && echo 1 || echo 0)"
check "the device's default backend is the real one, never the fake" \
    "$(grep -q '#define CAMERA_BACKEND_DEFAULT "v4l2"' $A/vision_session.c && grep -q 'return env && \*env ? env : "v4l2";' $H && echo 1 || echo 0)"
check "and the fake detector is never the device's (the Makefile links nncase into the package build)" \
    "$(grep -q 'POCKETVISION_KPU=1' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "the helper asks the ISP for planar BGR, which the AI2D engine reads as it is" \
    "$(grep -q '#define VISION_CAMERA_CONFIG "fmt=bg3p"' $H && grep -q 'POCKETCAM_FMT_BG3P' $C/vision_kpu_nncase.cpp && echo 1 || echo 0)"
check "a made-up picture is always labelled SIMULATED" \
    "$(grep -q 'out->hint = m->simulated ? "SIMULATED" : "";' $A/vision_model.c && echo 1 || echo 0)"

# ---- lifetime ------------------------------------------------------------------
check "the helper leaves with the shell (PR_SET_PDEATHSIG)" \
    "$(grep -q 'prctl(PR_SET_PDEATHSIG, SIGTERM);' $A/vision_session.c && echo 1 || echo 0)"
# A signal handled mid-inference ends a KPU/AI2D wait early with the hardware
# still writing (a whole-unit freeze on unit B, docs/hardware/VISION_GATE.md).
check "the helper blocks SIGTERM and SIGINT for life and takes no handler for them" \
    "$(code $H | grep -q 'sigprocmask(SIG_BLOCK, &stop_signals, NULL);' &&
       ! code $H | grep -qE 'sigaction\(SIG(TERM|INT)|signal\(SIG(TERM|INT)' && echo 1 || echo 0)"
grace=$(sed -nE 's/^#define VISION_DESTROY_GRACE_MS ([0-9]+).*/\1/p' $APP)
check "the helper is given at least 1000 ms to close the camera and the KPU ($grace)" \
    "$([ -n "$grace" ] && [ "$grace" -ge 1000 ] && echo 1 || echo 0)"
check "the shared memory is sealed before the helper sees it" \
    "$(grep -q 'F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL' $A/vision_session.c && grep -q 'PROT_READ, MAP_SHARED' $A/vision_session.c && echo 1 || echo 0)"
check "the helper is polled from a timer, never waited for, except when leaving" \
    "$(code $APP | grep -q 'lv_timer_create(on_poll, VISION_POLL_MS, a)' && [ "$(code $APP | grep -c 'vision_session_abandon(')" = 3 ] && echo 1 || echo 0)"
destroy=$(awk '/^static void vision_destroy\(/,/^}/' $APP)
order=$(printf '%s\n' "$destroy" | grep -nE 'lv_timer_delete|vision_session_abandon|lv_image_set_src\(a->img, NULL\)|picture_free\(&a->preview\)' | cut -d: -f1 | tr '\n' ' ')
check "destroy: timer, then helper, then the image detached, then the buffer freed (lines $order)" \
    "$(set -- $order; [ $# = 4 ] && [ "$1" -lt "$2" ] && [ "$2" -lt "$3" ] && [ "$3" -lt "$4" ] && echo 1 || echo 0)"
check "the size handler is gone before the app is freed" \
    "$(code $APP | grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
check "pictures are copied out of the shared memory, never pointed at" "$(code $APP | grep -q 'shm' && echo 0 || echo 1)"
check "no callback crosses from the session into the app" "$(grep -qE '\(\*[a-z_]+\)\(' $A/vision_session.h && echo 0 || echo 1)"
check "every wait on the helper has a deadline" \
    "$(grep -q 'passed(s->hello_by, now_ms) || passed(s->open_by, now_ms)' $A/vision_session.c && echo 1 || echo 0)"

# ---- layout (DS 21.2) -------------------------------------------------------------
check "the layout arithmetic never asks which way the display is turned" \
    "$(code $A/vision_layout.c | grep -qE 'orientation|rotation|portrait|landscape' && echo 0 || echo 1)"
check "the screen asks only once, for the picture's shape" \
    "$([ "$(code $APP | grep -c 'pocketos_shell_orientation(')" = 1 ] && echo 1 || echo 0)"
check "the corner clearance comes from PocketUI's one rule" \
    "$(code $APP | grep -q 'pocketui_layout_begin(&a->guard, a->frame, &in)' && echo 1 || echo 0)"
check "the picture fits a shared-memory slot" \
    "$(grep -q '#define VISION_PICTURE_MAX 1024' $A/vision_layout.h && grep -q '#define POCKETCAM_VIEW_MAX_W 1024' core/pocketcam/pocketcam_proto.h && echo 1 || echo 0)"

# ---- scope of the prototype ----------------------------------------------------------
hits=$(code $A/*.[ch] $C/*.[ch] $C/*.cpp $H | grep -niE '\b(segment|pose|keypoint|AF_INET|socket\(AF_INET|curl|http|upload|cloud)\b')
check "no segmentation, pose, network or cloud: everything Vision sees stays on the unit" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "Camera's own screen is untouched by Vision (no vision words in apps/camera)" \
    "$(grep -qiE 'vision' apps/camera/*.c apps/camera/*.h tools/camera/*.c && echo 0 || echo 1)"
check "the shell knows Vision, after Recorder" \
    "$(grep -q '&app_camera, &app_browser, &app_recorder, &app_vision' ui/shell/shell.c && echo 1 || echo 0)"
check "the launcher places it in DEVICE, in the ai hue" \
    "$(grep -q '{ "vision", HOME_GROUP_DEVICE, HOME_HUE_AI[ ,}]' ui/shell/home_layout.c && echo 1 || echo 0)"
check "pos-vision is installed" "$(grep -q 'install -D -m 0755 tools/vision/pos-vision' Makefile && echo 1 || echo 0)"
for src in vision_app.c vision_model.c vision_layout.c vision_session.c vision_settings.c vision_store.c; do
    check "the shell builds $src" "$(grep -q "apps/vision/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done

echo "vision_lint: $failed failure(s)"
exit $((failed > 0))
