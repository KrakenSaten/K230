#!/bin/bash
# Video's boundaries, as source rules (docs/apps/VIDEO.md, ADR-012):
#
#   - the app never decodes and never touches hardware: no FFmpeg, no audio
#     layer, no alsa-lib, no device names; the helper is the only owner;
#   - it does not reach into Camera, Vision, Recorder or Wave;
#   - no threads in the app; the helper's one extra thread is the sound's,
#     in video_player.c only;
#   - no network anywhere, and no file names in any log;
#   - the screen blocks on nothing but the helper's bounded leaving;
#   - FFmpeg is linked into the helper only, and only its backend file
#     includes it; the device build passes POCKETVIDEO_FFMPEG=1 and the
#     shell's device default is the real backend;
#   - the test hooks are in the test build only;
#   - registration: shell order, launcher row, install line, every source
#     built by the shell.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
none() { # <label> <regex> <files...>
    local label=$1 re=$2
    shift 2
    local hits
    hits=$(grep -nE "$re" "$@" 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "FAIL $label:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $label"
    fi
}

APP=$(ls apps/video/*.c apps/video/*.h)
TOOL=$(ls tools/video/*.c tools/video/*.h)

none "the app does not decode: no FFmpeg" 'libav|libsw|avcodec|avformat|swscale' $APP
none "the app does not include the audio layer or alsa-lib" 'pocketaudio/pocketaudio\.h|alsa/|asoundlib' $APP
none "the app names no device" 'hw:|/dev/snd|/dev/video|/dev/gpiochip|video_vpu|K230I2SINNO' $APP
none "Video reaches into no other app" \
    '#include .*(camera|pocketcam|vision|pocketvision|rec_|wave|pocketwav)|"[^"]*pos-(camera|vision|record|wave)"' \
    $APP $TOOL
none "no threads in the app" 'pthread|thrd_|std::thread' $APP
none "the helper's only thread is the sound's, in the engine" 'pthread|thrd_' \
    tools/video/pos_video.c tools/video/video_backend_fake.c tools/video/video_backend_ffmpeg.c \
    tools/video/video_backend.h
none "no network" '\bconnect\(|getaddrinfo|curl|AF_INET|sendto\(' $APP $TOOL
none "the helper logs nothing" 'pocketlog|syslog|\bLOG_(INFO|ERROR|WARN)\b' $TOOL
none "the app never logs a file name or a path" 'LOG_[A-Z]+\(.*(m->name|model\.name|path|a->dir)' \
    apps/video/video_app.c
none "the screen blocks on nothing" '\b(usleep|sleep|nanosleep|fsync|fdatasync|sync)\(|\bwrite\(|fopen\(|waitpid\(' \
    apps/video/video_app.c
none "no touch keyboard: nothing is typed" 'pocketos_shell_keyboard_show|pocketui_text_field' \
    apps/video/video_app.c
none "the state machine and the layout are pure (no I/O)" '#include <(unistd|fcntl|sys/|dirent)' \
    apps/video/video_state.c apps/video/video_layout.c
# (pocketos_mkdir_p for the videos folder is the app's one change to the
# filesystem; the session's ftruncate is on its memfd, not a file.)
none "the app deletes, renames or re-modes nothing" '\b(unlink|unlinkat|rename|renameat|mkdir|chmod)\(' $APP
none "only the FFmpeg backend includes FFmpeg" 'include <lib(av|sw)' \
    tools/video/pos_video.c tools/video/video_player.c tools/video/video_backend_fake.c
check "the FFmpeg backend is FFmpeg's hardware decoder, not a software one" \
    "$(grep -q 'h264_v4l2m2m' tools/video/video_backend_ffmpeg.c &&
       ! grep -qE 'avcodec_find_decoder\(AV_CODEC_ID_H264|avcodec_find_decoder\(f->vs' tools/video/video_backend_ffmpeg.c &&
       echo 1 || echo 0)"
check "every wait on the helper has a deadline" \
    "$(for d in VIDEO_HELLO_MS VIDEO_OPEN_MS VIDEO_REPLY_MS VIDEO_SILENCE_MS; do
           grep -q "$d" apps/video/video_session.c || exit 0; done; echo 1)"
check "the helper leaves with the shell (PR_SET_PDEATHSIG)" \
    "$(grep -q 'PR_SET_PDEATHSIG' apps/video/video_session.c && echo 1 || echo 0)"
check "the shared memory is sealed and mapped read-only by the shell" \
    "$(grep -q 'F_SEAL_SHRINK' apps/video/video_session.c && grep -q 'PROT_READ, MAP_SHARED' apps/video/video_session.c &&
       echo 1 || echo 0)"
check "the test hooks are behind POS_VIDEO_TEST_HOOKS only" \
    "$(grep -q '#ifdef POS_VIDEO_TEST_HOOKS' tools/video/pos_video.c &&
       ! grep -qE 'POS_VIDEO_(FAKE_AUDIO|NO_AUDIO)' <(sed '/#ifdef POS_VIDEO_TEST_HOOKS/,/#endif/d' tools/video/pos_video.c) &&
       echo 1 || echo 0)"
for src in video_app.c video_state.c video_layout.c video_session.c video_files.c; do
    check "the shell builds $src" "$(grep -q "apps/video/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the shell links no FFmpeg, no audio layer, no alsa-lib" \
    "$(! grep -qE 'avcodec|avformat|pocketaudio|asound|tools/video' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the device's default backend is FFmpeg; the simulator's the fake" \
    "$(grep -q '#define VIDEO_BACKEND_DEFAULT "ffmpeg"' apps/video/video_session.c &&
       grep -q 'VIDEO_BACKEND_DEFAULT="fake"' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the package builds the helper with FFmpeg" \
    "$(grep -q 'POCKETVIDEO_FFMPEG=1' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "the package depends on the image's FFmpeg" \
    "$(grep -qE '^POCKETOS_DEPENDENCIES = .*\bffmpeg\b' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "the shell knows Video, after Vision" \
    "$(grep -q '&app_vision,' ui/shell/shell.c && grep -qE '^ *&app_video, &app_solitaire' ui/shell/shell.c &&
       [ "$(grep -c '&app_video\b' ui/shell/shell.c)" = 1 ] && echo 1 || echo 0)"
check "the launcher places it in the Apps folder (DS §47)" \
    "$(grep -q '{ "video", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_APPS }' ui/shell/home_layout.c && echo 1 || echo 0)"
check "pos-video is installed" \
    "$(grep -q 'install -D -m 0755 tools/video/pos-video' Makefile && echo 1 || echo 0)"
check "the deploy script carries pos-video" \
    "$(grep -q 'pos-video' platforms/k230/scripts/deploy.sh && echo 1 || echo 0)"
check "the core audio layer is unchanged by Video (no video words in it)" \
    "$(grep -qiE 'video' core/pocketaudio/*.c core/pocketaudio/*.h && echo 0 || echo 1)"

echo "video_lint: $failed failure(s)"
exit $((failed > 0))
