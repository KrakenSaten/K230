#!/bin/bash
# The MP3 app's boundaries, as source rules (docs/apps/MP3.md):
#
#   - the app never touches audio or decodes: no alsa-lib, no pocketaudio,
#     no FFmpeg, no device names; pos-mp3 is the only owner (the shape of
#     ADR-010);
#   - it reaches into no other app: no apps/recorder or apps/wave header, no
#     pos-record or pos-wave;
#   - no network anywhere in it, and FFmpeg is opened on local files only;
#   - nothing is logged: no names, no audio;
#   - the screen blocks on nothing: no sleeps, syncs, file writes or threads
#     in mp3_app.c, and no touch keyboard (nothing is typed);
#   - the one thread is the folder scanner's, in mp3_library.c;
#   - the shared audio layer is unchanged by MP3;
#   - the shipped helper carries no test hooks;
#   - the shell registers the app and builds every source; the image
#     installs the helper and builds it with FFmpeg.
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

APP=$(ls apps/mp3/*.c apps/mp3/*.h)
TOOL=$(ls tools/mp3/*.c tools/mp3/*.h)

none "the app does not include the audio layer, alsa-lib or FFmpeg" \
    'pocketaudio/pocketaudio\.h|alsa/|asoundlib|libav|libswresample|mp3_decoder\.h' $APP
none "the app names no audio device" 'hw:|/dev/snd|/dev/gpiochip|K230I2SINNO' $APP
none "nothing in MP3 reaches into Recorder or Wave" \
    '#include .*(rec_|wave_)|"[^"]*pos-(record|wave)"' $APP $TOOL
none "no network" '\bconnect\(|getaddrinfo|curl|AF_INET|sendto\(|"https?:' $APP $TOOL
check "FFmpeg opens only local files (file: and a protocol whitelist of file)" \
    "$(grep -q '"file:%s"' tools/mp3/mp3_decoder_ffmpeg.c &&
       grep -q '"protocol_whitelist", "file"' tools/mp3/mp3_decoder_ffmpeg.c && echo 1 || echo 0)"
none "no logging (no names, no audio, in any log)" 'pocketlog|syslog|LOG_(INFO|ERROR|WARN)' $APP $TOOL
none "the helper prints to stderr only its usage" 'fprintf\(stderr' tools/mp3/mp3_*.c
check "pos_mp3.c's only stderr output is the usage text" \
    "$([ "$(grep -c 'fprintf(stderr' tools/mp3/pos_mp3.c)" = 1 ] && echo 1 || echo 0)"
none "the screen does not sleep, sync, write files or start threads" \
    'usleep|nanosleep|sleep\(|fsync|fdatasync|\bsync\(|fopen|fwrite|rename\(|unlink\(|pthread' apps/mp3/mp3_app.c
none "no touch keyboard in the app" 'pocketos_shell_keyboard|pos_keyboard' apps/mp3/mp3_app.c
none "the only thread is the scanner's" 'pthread_create' $(ls apps/mp3/*.c | grep -v mp3_library.c) $TOOL
check "the scanner's thread is there" "$(grep -q 'pthread_create' apps/mp3/mp3_library.c && echo 1 || echo 0)"
none "no LVGL outside the screen" 'lvgl\.h|lv_obj|lv_timer' $(ls apps/mp3/*.c apps/mp3/*.h | grep -v mp3_app.c) $TOOL
check "the core audio layer is unchanged by MP3 (no mp3 words in it)" \
    "$(grep -qiE 'mp3' core/pocketaudio/*.c core/pocketaudio/*.h && echo 0 || echo 1)"
if [ -x tools/mp3/pos-mp3 ]; then
    check "the shipped helper has no test hooks" \
        "$(grep -qa 'POS_MP3_FAKE_AUDIO' tools/mp3/pos-mp3 && echo 0 || echo 1)"
fi
if [ -x tests/pos-mp3-testhooks ]; then
    check "the test helper has them" "$(grep -qa 'POS_MP3_FAKE_AUDIO' tests/pos-mp3-testhooks && echo 1 || echo 0)"
fi
check "the shell registers the app, after 2048" \
    "$(grep -q '&app_2048, &app_mp3' ui/shell/shell.c && grep -q 'extern const struct pocketos_app app_mp3;' ui/shell/shell.c &&
       echo 1 || echo 0)"
check "the launcher puts it in the Apps folder, after DeskBuddy (DS §47)" \
    "$(grep -A1 '{ "deskbuddy", HOME_GROUP_FOLDERS' ui/shell/home_layout.c | grep -q '{ "mp3", HOME_GROUP_FOLDERS, HOME_HUE_APPS, HOME_FOLDER_APPS }' &&
       echo 1 || echo 0)"
for src in mp3_app.c mp3_ctl.c mp3_view.c mp3_player.c mp3_session.c mp3_library.c; do
    check "the shell builds apps/mp3/$src" \
        "$(grep -q "apps/mp3/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "make install puts pos-mp3 in /usr/bin" \
    "$(grep -q 'install -D -m 0755 tools/mp3/pos-mp3' Makefile && echo 1 || echo 0)"
check "the image builds pos-mp3 with FFmpeg and depends on it" \
    "$(grep -q 'MP3_FFMPEG=1' platforms/k230/package/pocketos/pocketos.mk &&
       grep -qE '^POCKETOS_DEPENDENCIES = .*\bffmpeg\b' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "the deploy script carries the helper" \
    "$(grep -q 'pos-mp3' platforms/k230/scripts/deploy.sh && echo 1 || echo 0)"

echo "mp3_lint: $failed failure(s)"
exit $((failed > 0))
