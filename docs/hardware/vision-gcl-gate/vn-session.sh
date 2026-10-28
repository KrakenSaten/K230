#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# The timed real-camera session (portrait), one owner action per phase; the
# screen's mode button says which phase runs:
#   DETECT (GENERAL 80), 30 s: everyday objects in front of the camera.
#   COLOR, 60 s: a plain red object over the centre square, then an object of
#     another colour, then one tap on the picture elsewhere. MATCH is pressed
#     (keys) at 14 s and CLEAR at 22 s.
#   LINE TRACE, 50 s: a dark line on light paper, top to bottom of the
#     picture; taken away at about 20 s; back at about 35 s.
#   Then Vision closes, the helper's exit is checked and Camera opens.
# Captures into ./shots/session-*.png, a timeline into ./log/session.txt.
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
T0=$(date +%s)
mark() { echo "$(( $(date +%s) - T0 )) s $*" | tee -a log/session.txt; }
at() { while [ $(( $(date +%s) - PH )) -lt "$1" ]; do sleep 1; done; }
: > log/session.txt

./vn-start.sh s-detect detect upstream portrait > log/session-start-detect.txt 2>&1; mark "DETECT up: $(tail -1 log/session-start-detect.txt)"
PH=$(date +%s)
for t in 8 16 24 30; do at $t; ./vn-shot.sh session-detect-$t > /dev/null; mark "shot detect-$t"; done

./vn-start.sh s-color color r0 portrait > log/session-start-color.txt 2>&1; mark "COLOR up: $(tail -1 log/session-start-color.txt)"
PH=$(date +%s)
at 6; ./vn-shot.sh session-color-06 > /dev/null; mark "shot color-06"
at 12; ./vn-shot.sh session-color-12 > /dev/null; mark "shot color-12"
at 14; for k in 1 1 21; do pos call shell shell.key code=$k > /dev/null; sleep 1; done; mark "MATCH pressed"
at 19; ./vn-shot.sh session-color-19-match > /dev/null; mark "shot color-19-match"
at 22; pos call shell shell.key code=21 > /dev/null; mark "CLEAR pressed"
at 25; ./vn-shot.sh session-color-25-clear > /dev/null; mark "shot color-25-clear"
for t in 32 40 48 56; do at $t; ./vn-shot.sh session-color-$t > /dev/null; mark "shot color-$t"; done

./vn-start.sh s-trace trace r0 portrait > log/session-start-trace.txt 2>&1; mark "TRACE up: $(tail -1 log/session-start-trace.txt)"
PH=$(date +%s)
for t in 8 14 20 25 30 35 40 46; do at $t; ./vn-shot.sh session-trace-$t > /dev/null; mark "shot trace-$t"; done

mark "closing Vision"
pos call shell shell.home > /dev/null 2>&1 || pos call shell shell.key code=40 > /dev/null
i=0; while [ -n "$(pidof pos-vision)" ] && [ $i -lt 8 ]; do sleep 1; i=$((i+1)); done
mark "pos-vision after leaving Vision: '$(pidof pos-vision)' (${i} s)"
pos call shell shell.open id=camera > /dev/null; sleep 6
mark "camera helper: $(for p in $(pidof pos-camera); do readlink /proc/$p/exe; done)"
./vn-shot.sh session-camera > /dev/null; mark "shot camera"
pos call shell shell.home > /dev/null 2>&1; sleep 3
mark "after Camera closed: pos-camera '$(pidof pos-camera)' pos-vision '$(pidof pos-vision)'"
mark "done"
