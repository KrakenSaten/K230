#!/bin/bash
# A scripted stand-in for pos-wave, for tests/wave_session_test.c. It speaks
# the helper's event protocol (tools/wave/pos_wave.c, EVENTS) and misbehaves
# on request. WAVE_FAKE picks the scenario. Never installed.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).

hex() { od -An -tx1 -v | tr -d ' \n'; }

# `recover`, whatever the scenario: the session runs it after a helper died
# by a signal. One line per run in WAVE_FAKE_RECOVER_MARK.
if [ "${1:-}" = "recover" ]; then
    [ -n "${WAVE_FAKE_RECOVER_MARK:-}" ] && echo "recover" >> "$WAVE_FAKE_RECOVER_MARK"
    exit 0
fi

# Where a test can find this process afterwards, to prove it is gone.
[ -n "${WAVE_FAKE_PIDFILE:-}" ] && echo $$ > "$WAVE_FAKE_PIDFILE"

case "${WAVE_FAKE:-}" in
send_ok)
    text=$(hex)
    echo "ready fake"
    echo "sending 1200"
    echo "received $text"
    echo "sent"
    exit 0
    ;;
args)
    echo "ready $(IFS=,; echo "$*")"
    exit 0
    ;;
listen_ok)
    trap 'echo stopped; exit 0' TERM
    echo "ready fake"
    echo "listening"
    echo "level 12"
    echo "received 444f4f5253"
    while true; do sleep 0.02; done
    ;;
ignore_term)
    trap '' TERM
    echo "listening"
    while true; do sleep 0.02; done
    ;;
crash)
    echo "ready fake"
    kill -KILL $$
    ;;
garbage)
    printf 'x%.0s' $(seq 1 700); echo
    echo "unknown words here"
    echo "received 414"
    echo "received zz"
    echo "level 101"
    echo "sending -5"
    echo "error audio_disabled speaker playback is not validated"
    echo "sent"
    exit 5
    ;;
flood)
    for i in $(seq 1 100); do echo "level 50"; done
    echo "received 41"
    exit 0
    ;;
exit_fast)
    exit 3
    ;;
fds)
    if [ -n "${WAVE_FAKE_FD:-}" ] && [ -e "/proc/$$/fd/$WAVE_FAKE_FD" ]; then
        echo "ready leaked"
    else
        echo "ready clean"
    fi
    exit 0
    ;;
*)
    echo "error fake unknown scenario"
    exit 2
    ;;
esac
