#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# The automated Relay Nine device pass (run B in docs/apps/RELAY_NINE_DEVICE.md),
# run on unit B from the kit directory: standalone audio, title, full-level
# input/mute/volume/pause/leave, then the stairs+combat slice; restores at the end.
set -u
K=$(cd "$(dirname "$0")" && pwd); cd "$K"
D="python3 $K/rn-drive.py play"
hp() { for h in $(pidof pos-audio-effects); do echo "helper $h args=$(tr '\0' ' ' < /proc/$h/cmdline)"; done; echo "pcm=$(head -1 /proc/asound/card0/pcm0p/sub0/status)"; }
echo "### start full"; ./rn-start.sh b-full || exit 1
echo "### standalone audio: 50, 100, muted"
python3 ./rn-audio.py 50 700 | tail -2; python3 ./rn-audio.py 100 700 | tail -2; python3 ./rn-audio.py 0 200 | tail -2
echo "### title"; $D open wait:2500 shot:b01-title
./rn-measure.sh b-title-idle 8 | tail -1
echo "### play + system mute/volume while the helper runs"; $D tap:enter wait:1500; hp
$D mute:1 wait:1200; hp; $D mute:0 wait:1200; hp; $D vol:40 wait:1200; hp
./rn-measure.sh b-full-play 36 > log/measure-b-full-play.txt &
M=$!
$D space:2500 w:1300 tap:f wait:1200 w:600 left:654 w+space:3100 wait:300 tap:q wait:300 space:1200 tap:r wait:1800 \
   space:900 tap:q wait:400 tap:r wait:2000 right:1300 w+right+space:2000 s:800 left+space:1500 a:600 d:600 w+left:1500 space:1500 wait:2000
wait $M; tail -1 log/measure-b-full-play.txt
$D shot:b02-after-route; hp
echo "### pause, then leave while holding"; $D tap:p wait:800; hp; $D tap:enter wait:600 down:w down:space wait:500 home wait:1200 up:w up:space; hp
echo "### restart into the slice"; ./rn-restore.sh | tail -1; ./rn-start.sh b-slice slice | tail -1
$D open wait:2000 tap:enter wait:1500 shot:b03-slice-start
./rn-measure.sh b-slice-play 30 > log/measure-b-slice.txt &
M=$!
$D w:900 space:1500 w+space:1500 shot:b04-slice-combat w:1200 left:600 right:1200 w+right:1500 s:1000 space:1200 tap:r wait:1800 w+left+space:2500 wait:1500
wait $M; tail -1 log/measure-b-slice.txt
$D shot:b05-slice-end; hp; $D home wait:1200; hp
cat log/stats-b-full.txt log/stats-b-slice.txt | cut -c1-430
./rn-restore.sh | tail -3
