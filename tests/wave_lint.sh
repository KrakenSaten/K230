#!/bin/bash
# Wave's and pocketaudio's boundaries, checked in the source.
#
#   - Only wave_app.c touches LVGL or the shell; the model, the presets, the
#     history, the store, the controller, the layout policy, the helper
#     client, the text rule and the modem are pure and tested on the host.
#   - Only wave_store.c touches files; it keeps no audio outside the runtime
#     directory and never stores the listen toggle.
#   - Nothing in apps/wave touches the sound card, the mixer or a GPIO: audio
#     hardware is core/pocketaudio's alone, and only pos-wave links it.
#   - No threads anywhere in the audio path (KEYBOARD_DRIVER_DESIGN §3), no
#     shell-outs, and no sleeping on the LVGL thread: the one bounded wait in
#     the app is wave_session_abandon(), inside wave_ctl_close(), called from
#     destroy() only.
#   - Messages never reach a log: ggwave's logging is compiled out and set to
#     none, and pos-wave writes to stderr only for its usage text.
#   - The K230 audio paths are marked validated only as their hardware tests
#     passed, and validation raises no limit: the -12 dBFS ceiling, the
#     modem's volume cap and Wave's default volume stay where they are.
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
      apps/wave/wave_protocol.h apps/wave/wave_preset.c apps/wave/wave_preset.h
      apps/wave/wave_history.c apps/wave/wave_history.h apps/wave/wave_store.c apps/wave/wave_store.h
      apps/wave/wave_ctl.c apps/wave/wave_ctl.h apps/wave/wave_layout.c apps/wave/wave_layout.h"

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
check "wave_ctl.c does not sleep either" \
    "$(code apps/wave/wave_ctl.c | grep -qE '\b(sleep|usleep|nanosleep|waitpid)\(' && echo 0 || echo 1)"
check "the app never reaches the session's blocking calls itself" \
    "$(code apps/wave/wave_app.c | grep -qE 'wave_session_(abandon|start_|stop)' && echo 0 || echo 1)"
check "its one bounded wait is abandon(), in wave_ctl_close() only" \
    "$([ "$(code apps/wave/wave_ctl.c | grep -c 'wave_session_abandon(')" = 1 ] &&
       sed -n '/^void wave_ctl_close/,/^}/p' apps/wave/wave_ctl.c | grep -q 'wave_session_abandon(' && echo 1 || echo 0)"
check "and wave_ctl_close() is called from destroy() only" \
    "$([ "$(code apps/wave/wave_app.c | grep -c 'wave_ctl_close(')" = 1 ] &&
       sed -n '/^static void wave_destroy/,/^}/p' apps/wave/wave_app.c | grep -q 'wave_ctl_close(' && echo 1 || echo 0)"
check "only wave_store.c touches files (the session opens /dev/null only)" \
    "$(for f in apps/wave/*.c apps/wave/*.cpp; do
           [ "$f" = apps/wave/wave_store.c ] && continue
           code "$f" | grep -qE '\b(fopen|fsync|rename|unlink|mkdir|pocketos_mkdir_p|opendir)\(' && echo "$f"
       done | grep -q . && echo 0 || echo 1)"
check "a capture lives in the runtime directory, never the state directory" \
    "$(sed -n '/^static int capture_dir/,/^}/p' apps/wave/wave_store.c | grep -q 'pocketos_runtime_dir()' &&
       ! sed -n '/^static int capture_dir/,/^}/p' apps/wave/wave_store.c | grep -q 'pocketos_state_dir' && echo 1 || echo 0)"
check "the listen toggle is never stored (no microphone at open)" \
    "$(code apps/wave/wave_store.c apps/wave/wave_store.h | grep -qi 'listen' && echo 0 || echo 1)"
check "the history is bounded" \
    "$(grep -qE '^#define WAVE_HISTORY_MAX [0-9]+$' apps/wave/wave_history.h && echo 1 || echo 0)"
check "no preset listens longer than the privacy bound (checked in the table's validity rule)" \
    "$(grep -q 'p->listen_seconds <= WAVE_LISTEN_SECONDS' apps/wave/wave_preset.c && echo 1 || echo 0)"
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
check "the K230 speaker path is marked validated (first controlled SEND, unit A, 2026-09-13)" \
    "$(sed -n '/^static const struct pocketaudio_board board_k230/,/^};/p' core/pocketaudio/pocketaudio.c |
       grep -q '\.playback_verified = 1,' && echo 1 || echo 0)"
check "Wave's default volume stays 10 (about -20 dBFS)" \
    "$(grep -q '^#define WAVE_DEFAULT_VOLUME 10$' apps/wave/wave_protocol.h && echo 1 || echo 0)"
check "the test board stays unvalidated, so the gate stays under test" \
    "$(sed -n '/^static const struct pocketaudio_board board = {/,/^};/p' tests/fake_audio_backend.c |
       grep -q '\.playback_verified = 0,' && echo 1 || echo 0)"
check "every played sample is still clamped to the ceiling" \
    "$(sed -n '/^long pocketaudio_write/,/^}/p' core/pocketaudio/pocketaudio.c | grep -q 'v > s->peak_limit' &&
       grep -q 'o->peak_limit > POCKETAUDIO_PEAK_CEILING' core/pocketaudio/pocketaudio.c && echo 1 || echo 0)"
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
for t in pocketaudio_test wave_view_test wave_session_test wave_modem_test wave_model_test \
         wave_store_test wave_layout_test wave_ctl_test wave_sim_test; do
    check "make test runs $t" "$(grep -qE "^	(TZ=UTC )?\./tests/$t( |$)" Makefile && echo 1 || echo 0)"
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
