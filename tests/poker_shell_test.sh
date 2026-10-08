#!/bin/bash
# Poker in the real SDL shell: registration, portraits, typography, themes,
# screenshot/audit, restart confirmation, navigation and clean shutdown.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the built pocketos-shell}
SHELL_BIN=$(realpath "$SHELL_BIN")
POS=${POS:-tools/pos/pos}
SHOTS_DIR=${SHOTS_DIR:-out/poker/screenshots}
mkdir -p "$SHOTS_DIR"
SHOTS_DIR=$(realpath "$SHOTS_DIR")
work=$(mktemp -d)
shell_pid=
cleanup() {
    if [ -n "$shell_pid" ]; then kill "$shell_pid" 2>/dev/null || true; wait "$shell_pid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
export SDL_VIDEODRIVER=dummy
export POCKETOS_RUNTIME_DIR="$work/run" POCKETOS_LOG_DIR="$work/log"
export POCKETOS_CONFIG_DIR="$work/config" POCKETOS_STATE_DIR="$work/state"
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR"
"$(dirname "$SHELL_BIN")/poker_app_test"
checks=0
run_case() {
    local screen=$1 size=$2 mode=$3 theme=$4
    DOORS_POKER_SCREEN="$screen" "$SHELL_BIN" --no-lock --open poker --text-size "$size" --mode "$mode" --theme "$theme" > "$work/run.log" 2>&1 &
    shell_pid=$!
    for _ in $(seq 1 60); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    "$POS" shell info > "$work/info.json"
    python3 - "$work/info.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
assert d['current']=='poker'
assert any(a['id']=='poker' for a in d['apps'])
assert d['display']['width']==568 and d['display']['height']==1232
PY
    "$POS" call shell shell.audit > "$work/audit.json"
    python3 - "$work/audit.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
bad=[i for i in d['issues'] if i['kind'] in ('clipped','overlap','sizeless','truncated')]
assert not bad,bad
PY
    "$POS" shell screenshot "$SHOTS_DIR/$screen-$size-$mode-$theme.png" > /dev/null
    if [ "$screen" = preflop ]; then
        "$POS" call shell shell.tap text=RESTART > /dev/null
        sleep 0.1
        "$POS" call shell shell.action action=back > /dev/null
        "$POS" shell info > "$work/info.json"
        python3 - "$work/info.json" <<'PY'
import json,sys
assert json.load(open(sys.argv[1]))['current']=='poker'
PY
    fi
    "$POS" app home > /dev/null
    "$POS" app start poker > /dev/null
    "$POS" app home > /dev/null
    kill "$shell_pid";wait "$shell_pid";shell_pid=
    ! grep -qE ' ERROR |assert|segmentation' "$work/run.log" "$POCKETOS_LOG_DIR/shell.log"
    test ! -S "$POCKETOS_RUNTIME_DIR/shell.sock"
    checks=$((checks+1))
}
for screen in preflop flop showdown split gameover; do
    for size in small medium large; do run_case "$screen" "$size" normal doors; done
done
for theme in ice slate brass olive carbon; do run_case flop medium normal "$theme"; done
for mode in outdoor night; do run_case flop medium "$mode" doors; done
python3 - "$SHOTS_DIR" <<'PY'
from pathlib import Path
import struct,sys
for p in Path(sys.argv[1]).glob('*.png'):
    b=p.read_bytes()
    assert b[:8]==b'\x89PNG\r\n\x1a\n' and struct.unpack('>II',b[16:24])==(568,1232),p
PY
echo "poker_shell_test: $checks simulator cases passed; screenshots in $SHOTS_DIR"
