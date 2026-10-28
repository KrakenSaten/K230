#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Drive the test shell and take a screenshot, through the shell's own API
# (docs/api/shell.md): vn-shot.sh <name> [key code...]. Keys are keyboard-base
# matrix codes (pos_keymap.c): 1 RIGHT, 2 LEFT, 12 DOWN, 22 UP, 21 ENTER, 40 ESC.
# The device's LVGL has no snapshot support, so the capture is F7's: the DRM
# plane, written into <state>/screenshots (the kit's ./state) and moved to
# ./shots/<name>.png.
K=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?name}; shift
for k in "$@"; do pos call shell shell.key code="$k" > /dev/null; sleep 1; done
sleep 1
before=$(ls "$K/state/screenshots" 2>/dev/null | wc -l)
pos call shell shell.action action=screenshot > /dev/null
i=0
while [ "$(ls "$K/state/screenshots" 2>/dev/null | wc -l)" -le "$before" ] && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
f=$(ls -t "$K/state/screenshots"/*.png 2>/dev/null | head -1)
[ -n "$f" ] && mv "$f" "$K/shots/$NAME.png" && ls -l "$K/shots/$NAME.png" | awk '{print $5, $9}' || echo "no screenshot"
