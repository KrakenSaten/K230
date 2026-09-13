#!/bin/bash
# Wave's and pocketaudio's boundaries, checked in the source.
#
#   - Only wave_app.c touches LVGL or the shell; the view, the helper client,
#     the text rule and the modem are pure and tested on the host.
#   - Nothing in apps/wave touches the sound card, the mixer or a GPIO: audio
#     hardware is core/pocketaudio's alone, and only pos-wave links it.
#   - No threads anywhere in the audio path (KEYBOARD_DRIVER_DESIGN §3), no
#     shell-outs, and no sleeping on the LVGL thread: the one bounded wait in
#     the app is wave_session_abandon(), called from destroy() only.
#   - Messages never reach a log: ggwave's logging is compiled out and set to
#     none, and pos-wave writes to stderr only for its usage text.
#   - The K230 audio paths stay gated until their hardware test says
#     otherwise.
#   - The tests are part of make test.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# Comments stripped, so a rule explained in prose does not trip its own check.
# Non-greedy and across lines: a sed line range would swallow the code between
# a one-line comment and the next comment's end.
code() { cat "$@" 2>/dev/null | perl -0777 -pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g'; }

PURE="apps/wave/wave_view.c apps/wave/wave_view.h apps/wave/wave_session.c apps/wave/wave_session.h
      apps/wave/wave_text.c apps/wave/wave_text.h apps/wave/wave_modem.cpp apps/wave/wave_modem.h
      apps/wave/wave_protocol.h"

for f in $PURE; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done
check "no LVGL or shell outside wave_app.c" \
    "$(code $PURE | grep -qE 'lvgl\.h|lv_[a-z]+\(|app\.h|pocketui|pos_input' && echo 0 || echo 1)"
check "wave_app.c is an app (app.h, pocketui)" \
    "$(grep -q '#include "app.h"' apps/wave/wave_app.c && grep -q '#include "pocketui.h"' apps/wave/wave_app.c && echo 1 || echo 0)"
check "apps/wave never touches the sound card, the mixer or a GPIO" \
    "$(code apps/wave/* | grep -qE 'asound|snd_|/dev/snd|gpiochip|gpiod|amixer|aplay|arecord|pocketaudio_(open|write|read)' && echo 0 || echo 1)"
check "the shell app never includes the modem" \
    "$(grep -q 'wave_modem' apps/wave/wave_app.c && echo 0 || echo 1)"
check "only core/pocketaudio names ALSA" \
    "$(grep -rlE --include='*.c' --include='*.cpp' --include='*.h' 'alsa/asoundlib|snd_pcm_' apps tools ui core services 2>/dev/null |
       grep -v '^core/pocketaudio/pocketaudio_alsa.c$' | grep -q . && echo 0 || echo 1)"
check "no threads in the audio path" \
    "$(code apps/wave/* core/pocketaudio/* tools/wave/* | grep -qE 'pthread_|std::thread|<thread>' && echo 0 || echo 1)"
check "no shell-outs in the audio path" \
    "$(code apps/wave/* core/pocketaudio/* tools/wave/* | grep -qE '\b(system|popen)\(' && echo 0 || echo 1)"
check "wave_app.c does not sleep" \
    "$(code apps/wave/wave_app.c | grep -qE '\b(sleep|usleep|nanosleep|waitpid)\(' && echo 0 || echo 1)"
check "its one bounded wait is abandon(), in destroy() only" \
    "$([ "$(code apps/wave/wave_app.c | grep -c 'wave_session_abandon(')" = 1 ] &&
       sed -n '/^static void wave_destroy/,/^}/p' apps/wave/wave_app.c | grep -q 'wave_session_abandon(' && echo 1 || echo 0)"
check "the helper gets its message on stdin, never in argv" \
    "$(sed -n '/^int wave_session_start_send/,/^}/p' apps/wave/wave_session.c | grep -q 'argv\[\] = .*text' && echo 0 || echo 1)"
check "the helper dies with the shell (PR_SET_PDEATHSIG)" \
    "$(grep -q 'prctl(PR_SET_PDEATHSIG, SIGTERM)' apps/wave/wave_session.c && echo 1 || echo 0)"
check "ggwave is compiled without its logging" \
    "$(grep -q '^GGWAVE_CXXFLAGS.*-DGGWAVE_DISABLE_LOG' Makefile && grep -q '^WAVE_CXXFLAGS.*-DGGWAVE_DISABLE_LOG' Makefile && echo 1 || echo 0)"
check "and its log file is set to none at run time as well" \
    "$(grep -q 'GGWave::setLogFile(nullptr)' apps/wave/wave_modem.cpp && echo 1 || echo 0)"
check "pos-wave writes to stderr only for usage" \
    "$(code tools/wave/pos_wave.c | grep -c 'stderr' | grep -qx 1 && echo 1 || echo 0)"
check "the K230 speaker path stays gated until its hardware test" \
    "$(sed -n '/^static const struct pocketaudio_board board_k230/,/^};/p' core/pocketaudio/pocketaudio.c |
       grep -q '\.playback_verified = 0,' && echo 1 || echo 0)"
check "the K230 microphone path is marked validated (unit A, 2026-09-13)" \
    "$(sed -n '/^static const struct pocketaudio_board board_k230/,/^};/p' core/pocketaudio/pocketaudio.c |
       grep -q '\.capture_verified = 1,' && echo 1 || echo 0)"
check "the K230 capture discards the codec's 500 ms startup transient" \
    "$(grep -q '^#define K230_CAPTURE_SETTLE_FRAMES (POCKETAUDIO_RATE / 2)$' core/pocketaudio/pocketaudio.c &&
       sed -n '/^static const struct pocketaudio_board board_k230/,/^};/p' core/pocketaudio/pocketaudio.c |
       grep -q '\.capture_settle_frames = K230_CAPTURE_SETTLE_FRAMES,' && echo 1 || echo 0)"
check "nothing shipped sets the unverified-audio override" \
    "$(grep -rqn 'POCKETOS_AUDIO_ALLOW_UNVERIFIED' platforms apps ui services core 2>/dev/null && echo 0 || echo 1)"
check "and the Wave app never passes --allow-unverified" \
    "$(grep -rn 'allow-unverified' apps ui 2>/dev/null | grep -q . && echo 0 || echo 1)"
check "the playback ceiling is -12 dBFS" \
    "$(grep -q '#define POCKETAUDIO_PEAK_CEILING 8192' core/pocketaudio/pocketaudio.h && echo 1 || echo 0)"
check "the modem's loudest volume stays under it" \
    "$(grep -q '#define WAVE_MODEM_MAX_VOLUME 25' apps/wave/wave_modem.h && echo 1 || echo 0)"
check "ggwave is pinned" \
    "$(grep -qE '^[0-9a-f]{40}$' platforms/k230/vendor_ggwave_commit.txt && echo 1 || echo 0)"
for t in pocketaudio_test wave_view_test wave_session_test wave_modem_test; do
    check "make test runs $t" "$(grep -qE "^	\./tests/$t( |$)" Makefile && echo 1 || echo 0)"
done
check "a helper killed by a signal gets its state recovered by the session" \
    "$(sed -n '/^static void finish/,/^}/p' apps/wave/wave_session.c | grep -q 'recover_detached(s)' &&
       sed -n '/^void wave_session_abandon/,/^}/p' apps/wave/wave_session.c | grep -q 'recover_detached(s)' && echo 1 || echo 0)"
check "the recovery process is detached (own session, not the shell's death signal)" \
    "$(sed -n '/^static void recover_detached/,/^}/p' apps/wave/wave_session.c | grep -q 'setsid()' &&
       ! sed -n '/^static void recover_detached/,/^}/p' apps/wave/wave_session.c | grep -q 'PR_SET_PDEATHSIG' && echo 1 || echo 0)"
check "open reconciles a dead owner's record before touching the hardware" \
    "$(sed -n '/^int pocketaudio_open/,/^}/p' core/pocketaudio/pocketaudio.c | awk '/reconcile\(/{r=NR} /ctl_get_bool/{g=NR} END{exit !(r && g && r < g)}' && echo 1 || echo 0)"
check "the test hook is compiled only into the test build" \
    "$(code tools/wave/pos_wave.c | awk '/#ifdef POS_WAVE_TEST_HOOKS/{h=1} /#endif/{h=0} /POS_WAVE_FAKE_AUDIO|fake_audio_/{if(!h) bad=1} END{exit bad}' && echo 1 || echo 0)"
check "make test runs wave_session_test against the real helper too" \
    "$(grep -qE '^	\./tests/wave_session_test tests/fake_pos_wave\.sh tests/pos-wave-testhooks$' Makefile && echo 1 || echo 0)"
for s in wave_tool_test.sh wave_lint.sh audio_recovery_test.sh capture_settle_test.sh; do
    check "make test runs $s" "$(grep -q "^	bash tests/$s" Makefile && echo 1 || echo 0)"
done
check "pos-wave is installed" "$(grep -q 'install -D -m 0755 tools/wave/pos-wave' Makefile && echo 1 || echo 0)"

echo "wave_lint: $failed failure(s)"
exit $((failed > 0))
