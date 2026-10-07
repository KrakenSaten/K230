#!/bin/bash
# A scripted stand-in for pos-wave, for tests/wave_session_test.c. It speaks
# the helper's event protocol (tools/wave/pos_wave.c, EVENTS) and misbehaves
# on request. WAVE_FAKE picks the scenario. Never installed.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0

hex() { od -An -tx1 -v | tr -d ' \n'; }

# `recover`, whatever the scenario: the session runs it after a helper died
# by a signal. One line per run in WAVE_FAKE_RECOVER_MARK.
if [ "${1:-}" = "recover" ]; then
    [ -n "${WAVE_FAKE_RECOVER_MARK:-}" ] && echo "recover" >> "$WAVE_FAKE_RECOVER_MARK"
    exit 0
fi

# Where a test can find this process afterwards, to prove it is gone.
[ -n "${WAVE_FAKE_PIDFILE:-}" ] && echo $$ > "$WAVE_FAKE_PIDFILE"

# One line per run in WAVE_FAKE_LOG: the subcommand and its arguments, so a
# test can read back the order in which helpers were started.
[ -n "${WAVE_FAKE_LOG:-}" ] && echo "$*" >> "$WAVE_FAKE_LOG"

# "auto": behave by subcommand, the way the real helper does, so the
# controller can run whole sequences (listen, pause, send, resume, capture,
# decode) against one scenario. Knobs:
#   WAVE_FAKE_RX=<hex>          a listen hears this ...
#   WAVE_FAKE_RX_TIMES=<n>      ... this many times (default 1)
#   WAVE_FAKE_LISTEN_END=1      a listen ends by itself (its --seconds ran out)
#   WAVE_FAKE_SEND_FAIL=<code>  a send fails with this error word, exit 3
#   WAVE_FAKE_LISTEN_FAIL=<code> a listen fails with this error word, exit 3
#   WAVE_FAKE_CAPTURE=<hex|noise> what a recording holds (default: nothing)
#   WAVE_FAKE_SEND_MS=<ms>      how long a send plays (default 100)
#   WAVE_FAKE_RECORD_MS=<ms>    how long a recording runs (default 200)
if [ "${WAVE_FAKE:-}" = "auto" ]; then
    cmd=${1:-}
    last=""
    for a in "$@"; do last=$a; done
    case "$cmd" in
    send)
        text=$(hex)
        [ -n "${WAVE_FAKE_LOG:-}" ] && echo "text $text" >> "$WAVE_FAKE_LOG"
        if [ -n "${WAVE_FAKE_SEND_FAIL:-}" ]; then
            echo "error ${WAVE_FAKE_SEND_FAIL} the speaker could not be opened"
            exit 3
        fi
        trap 'echo stopped; exit 0' TERM
        echo "ready fake"
        ms=${WAVE_FAKE_SEND_MS:-100}
        echo "sending $ms"
        sleep "$(awk "BEGIN{print $ms/1000}")" &
        wait $!
        echo "sent"
        exit 0
        ;;
    listen)
        if [ -n "${WAVE_FAKE_LISTEN_FAIL:-}" ]; then
            echo "error ${WAVE_FAKE_LISTEN_FAIL} the microphone could not be opened"
            exit 3
        fi
        trap 'echo stopped; exit 0' TERM
        echo "ready fake"
        echo "listening"
        echo "level 12"
        if [ -n "${WAVE_FAKE_RX:-}" ]; then
            for i in $(seq 1 "${WAVE_FAKE_RX_TIMES:-1}"); do
                echo "received $WAVE_FAKE_RX"
            done
        fi
        if [ "${WAVE_FAKE_LISTEN_END:-}" = "1" ]; then
            exit 0
        fi
        while true; do sleep 0.02; done
        ;;
    record)
        # Finished early by SIGTERM: what was recorded is still written.
        trap 'printf "%s" "${WAVE_FAKE_CAPTURE:-}" > "$last"; echo stopped; exit 0' TERM
        echo "ready fake"
        echo "listening"
        echo "level 30"
        sleep "$(awk "BEGIN{print ${WAVE_FAKE_RECORD_MS:-200}/1000}")" &
        wait $!
        printf "%s" "${WAVE_FAKE_CAPTURE:-}" > "$last"
        exit 0
        ;;
    decode)
        if [ ! -e "$last" ]; then
            echo "error usage cannot read $last"
            exit 2
        fi
        body=$(cat "$last")
        if [ "$body" = "noise" ]; then
            echo "missed"
            exit 1
        fi
        if [ -z "$body" ]; then
            exit 1
        fi
        echo "received $body"
        exit 0
        ;;
    esac
    echo "error usage unknown command"
    exit 2
fi

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
