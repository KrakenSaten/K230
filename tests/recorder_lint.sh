#!/bin/bash
# The Recorder's boundaries, as source rules (docs/apps/RECORDER.md):
#
#   - the app never touches audio: no alsa-lib, no pocketaudio, no device
#     names; the helper is the only owner (ADR-010);
#   - it does not reach into Wave: no apps/wave header, no pos-wave;
#   - no threads, no network, no logging of names or audio anywhere in it;
#   - the screen blocks on nothing: no sleeps, syncs or writes in rec_app.c,
#     and no touch keyboard (nothing is typed);
#   - filesystem changes happen in rec_store.c only, on the app side;
#   - the shell builds every app source and links no audio library.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
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

APP=$(ls apps/recorder/*.c apps/recorder/*.h)
TOOL=$(ls tools/recorder/*.c tools/recorder/*.h)

none "the app does not include the audio layer or alsa-lib" 'pocketaudio/pocketaudio\.h|alsa/|asoundlib' $APP
none "the app names no audio device" 'hw:|/dev/snd|/dev/gpiochip|K230I2SINNO' $APP
none "nothing in the Recorder reaches into Wave" '#include .*wave|"[^"]*pos-wave"' $APP $TOOL
none "no threads" 'pthread|thrd_|std::thread' $APP $TOOL
none "no network" '\bconnect\(|getaddrinfo|curl|AF_INET|sendto\(' $APP $TOOL
none "no logging (no names, no audio, in any log)" 'pocketlog|syslog|LOG_(INFO|ERROR|WARN)' $APP $TOOL
none "the helper prints to stderr only its usage" 'fprintf\(stderr' tools/recorder/rec_*.c
check "pos_record.c's only stderr output is the usage text" \
    "$([ "$(grep -c 'fprintf(stderr' tools/recorder/pos_record.c)" = 1 ] && echo 1 || echo 0)"
none "the screen blocks on nothing" '\b(usleep|sleep|nanosleep|fsync|fdatasync|sync)\(|\bwrite\(|fopen\(|waitpid\(' \
    apps/recorder/rec_app.c
none "no touch keyboard: nothing is typed" 'pocketos_shell_keyboard_show|pocketui_text_field' apps/recorder/rec_app.c
none "only rec_store.c changes the filesystem on the app side" \
    '\b(unlink|unlinkat|rename|renameat|mkdir|chmod|ftruncate)\(' \
    apps/recorder/rec_app.c apps/recorder/rec_ctl.c apps/recorder/rec_view.c apps/recorder/rec_state.c \
    apps/recorder/rec_session.c apps/recorder/rec_names.c
none "the state machine and the view are pure (no I/O)" '#include <(unistd|fcntl|sys/)' \
    apps/recorder/rec_state.c apps/recorder/rec_view.c apps/recorder/rec_names.c
check "the recordings are created 0600 and nothing else" \
    "$(grep -q '0600)' tools/recorder/rec_file.c && ! grep -qE '06[46]4\)|0666\)' tools/recorder/rec_file.c && echo 1 || echo 0)"
check "the helper drops every permission for others whatever its umask" \
    "$(grep -q 'umask(077)' tools/recorder/pos_record.c && echo 1 || echo 0)"
for src in rec_app.c rec_ctl.c rec_view.c rec_state.c rec_session.c rec_store.c rec_names.c; do
    check "the shell builds $src" "$(grep -q "apps/recorder/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the shell links the WAV reader, and no audio layer or alsa-lib" \
    "$(grep -q 'core/pocketwav/pocketwav.c' ui/shell/CMakeLists.txt &&
       ! grep -qE 'pocketaudio|asound' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the shell knows Recorder, after Camera and Browser" \
    "$(grep -q '&app_camera, &app_browser, &app_recorder' ui/shell/shell.c && echo 1 || echo 0)"
check "the launcher places it in the Utilities folder" \
    "$(grep -q '{ "recorder", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_UTILITIES }' ui/shell/home_layout.c && echo 1 || echo 0)"
check "pos-record is installed" \
    "$(grep -q 'install -D -m 0755 tools/recorder/pos-record' Makefile && echo 1 || echo 0)"
check "the core audio layer is unchanged by the Recorder (no recorder words in it)" \
    "$(grep -qiE 'recorder|rec_' core/pocketaudio/*.c core/pocketaudio/*.h && echo 0 || echo 1)"

echo "recorder_lint: $failed failure(s)"
exit $((failed > 0))
