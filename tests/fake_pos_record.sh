#!/bin/bash
# A scripted stand-in for pos-record that misbehaves on request, for
# tests/rec_session_test.c and tests/rec_ctl_test.c. The ordinary cases are
# tested against the real helper (tests/pos-record-testhooks); this covers
# what the real one should never do. REC_FAKE picks the scenario. Never
# installed.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).

# `recover` without --dir is what a session runs, detached, after a helper
# died by a signal: one line per run in REC_FAKE_RECOVER_MARK.
if [ "${1:-}" = "recover" ] && [ "${2:-}" != "--events" ]; then
    [ -n "${REC_FAKE_RECOVER_MARK:-}" ] && echo "recover" >> "$REC_FAKE_RECOVER_MARK"
    exit 0
fi
[ -n "${REC_FAKE_PIDFILE:-}" ] && echo $$ > "$REC_FAKE_PIDFILE"
[ -n "${REC_FAKE_LOG:-}" ] && echo "$*" >> "$REC_FAKE_LOG"

case "${REC_FAKE:-}" in
flood)
    # Far more meter readings than anyone polls, then the one line that
    # matters.
    echo "ready fake"
    echo "recording 16000 REC-0001.wav"
    for i in $(seq 1 3000); do echo "level $((i % 30000)) 100"; done
    echo "saved 1000 32044 0 REC-0001.wav"
    exit 0
    ;;
overlong)
    printf 'level %05000d\n' 1
    echo "stopped"
    exit 0
    ;;
garbage)
    echo "level abc 1"
    echo "level 40000 1"
    echo "saved 1 2 3"
    echo "saved 1 2 3 a/b.wav"
    echo "recording REC-0001.wav"
    echo "playing 1 16000 3"
    echo "limit maybe"
    echo "novel-event 1"
    echo "stopped"
    exit 2
    ;;
deaf)
    # Ignores every way of asking it to stop.
    trap '' TERM INT
    echo "ready fake"
    echo "recording 16000 REC-0001.wav"
    while :; do sleep 1; done
    ;;
crash)
    echo "ready fake"
    echo "recording 16000 REC-0001.wav"
    kill -KILL $$
    ;;
echo)
    # Answers each command the way the real helper would.
    trap 'echo "saved 500 16044 0 REC-0001.wav"; echo stopped; exit 0' TERM
    echo "ready fake"
    echo "recording 16000 REC-0001.wav"
    while IFS= read -r line; do
        case "$line" in
        pause) echo "paused" ;;
        resume) echo "resumed" ;;
        stop) echo "saved 500 16044 0 REC-0001.wav"; echo "stopped"; exit 0 ;;
        esac
    done
    # stdin closed: the app is gone.
    echo "saved 500 16044 0 REC-0001.wav"
    exit 0
    ;;
*)
    echo "error usage unknown fake scenario"
    exit 2
    ;;
esac
