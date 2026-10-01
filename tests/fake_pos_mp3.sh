#!/bin/bash
# A scripted stand-in for pos-mp3 that misbehaves on request, for
# tests/mp3_session_test.c and tests/mp3_ctl_test.c. The ordinary cases are
# tested against the real helper (tests/pos-mp3-testhooks); this covers what
# the real one should never do. MP3_FAKE picks the scenario. Never
# installed.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0

# `recover` is what a session runs, detached, after a helper died by a
# signal: one line per run in MP3_FAKE_RECOVER_MARK.
if [ "${1:-}" = "recover" ]; then
    [ -n "${MP3_FAKE_RECOVER_MARK:-}" ] && echo "recover" >> "$MP3_FAKE_RECOVER_MARK"
    exit 0
fi
[ -n "${MP3_FAKE_PIDFILE:-}" ] && echo $$ > "$MP3_FAKE_PIDFILE"
[ -n "${MP3_FAKE_LOG:-}" ] && echo "start $$ $*" >> "$MP3_FAKE_LOG"
bye() { [ -n "${MP3_FAKE_LOG:-}" ] && echo "end $$" >> "$MP3_FAKE_LOG"; exit "$1"; }

case "${MP3_FAKE:-}" in
flood)
    # Far more progress readings than anyone polls, then the lines that
    # matter.
    echo "ready fake"
    echo "playing 60000 1 44100 2 mp3"
    for i in $(seq 1 3000); do echo "progress $i"; done
    echo "played"
    bye 0
    ;;
overlong)
    printf 'progress %05000d\n' 1
    echo "stopped"
    bye 0
    ;;
garbage)
    echo "progress abc"
    echo "playing 1000 2 44100 2 mp3"
    echo "playing 1000 1 44100 2 MP3!"
    echo "playing 1000 1 0 2 mp3"
    echo "meta album Something"
    printf 'meta title bad\001text\n'
    echo "novel-event 1"
    echo "stopped"
    bye 2
    ;;
deaf)
    # Ignores every way of asking it to stop.
    trap '' TERM INT
    echo "ready fake"
    echo "playing 60000 1 44100 2 mp3"
    while :; do sleep 1; done
    ;;
crash)
    echo "ready fake"
    echo "playing 60000 1 44100 2 mp3"
    kill -KILL $$
    ;;
decode)
    echo "meta title Broken Song"
    echo "ready fake"
    echo "playing 60000 1 44100 2 mp3"
    echo "progress 1200"
    echo "error decode the file stopped decoding"
    bye 5
    ;;
busy)
    # The device is taken: nothing plays.
    echo "error audio_busy audio device in use"
    bye 3
    ;;
echo)
    # Answers each command the way the real helper would, and plays for
    # MP3_FAKE_LEN_MS (default: until told otherwise).
    trap 'echo stopped; bye 0' TERM
    echo "meta title Fake Song"
    echo "meta artist Fake Artist"
    echo "ready fake"
    echo "playing ${MP3_FAKE_TOTAL_MS:-60000} 1 44100 2 mp3"
    echo "progress 0"
    paused=0
    left=$(( ${MP3_FAKE_LEN_MS:-0} / 100 ))
    while :; do
        if IFS= read -r -t 0.1 line; then
            case "$line" in
            pause) paused=1; echo "paused" ;;
            resume)
                if [ -n "${MP3_FAKE_RESUME_BUSY:-}" ]; then
                    echo "error audio_busy audio device in use"
                else
                    paused=0; echo "resumed"
                fi
                ;;
            seek\ *) echo "progress ${line#seek }" ;;
            volume\ *) [ -n "${MP3_FAKE_LOG:-}" ] && echo "volume ${line#volume }" >> "$MP3_FAKE_LOG" ;;
            stop) echo "stopped"; bye 0 ;;
            esac
        else
            # read times out (> 128) or sees the end of stdin (1): the app is
            # gone when stdin has ended.
            [ $? -le 128 ] && { echo "stopped"; bye 0; }
        fi
        if [ "$paused" = 0 ] && [ "$left" -gt 0 ]; then
            left=$((left - 1))
            if [ "$left" = 0 ]; then echo "played"; bye 0; fi
        fi
    done
    ;;
*)
    echo "error usage unknown fake scenario"
    bye 2
    ;;
esac
