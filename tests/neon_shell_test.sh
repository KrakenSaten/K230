#!/bin/bash
# Native shell smoke: registration, physical key translation, pointer, Back/Home and captures.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
SHELL_BIN=$(realpath "${SHELL_BIN:?set SHELL_BIN to the SDL pocketos-shell}")
POS=${POS:-tools/pos/pos}
SHOTS_DIR=${SHOTS_DIR:-out/relay-nine/screenshots}
mkdir -p "$SHOTS_DIR"
SHOTS_DIR=$(realpath "$SHOTS_DIR")
work=$(mktemp -d)
pid=
cleanup() { if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; rm -rf "$work"; }
trap cleanup EXIT
export SDL_VIDEODRIVER=dummy
export POCKETOS_RUNTIME_DIR="$work/run" POCKETOS_CONFIG_DIR="$work/config"
export POCKETOS_STATE_DIR="$work/state" POCKETOS_LOG_DIR="$work/log"
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR" "$POCKETOS_LOG_DIR"
audit() {
    "$POS" call shell shell.audit > "$SHOTS_DIR/shell-$mode-$1-audit.json"
    python3 - "$SHOTS_DIR/shell-$mode-$1-audit.json" <<'PY_AUDIT'
import json,sys
s=json.load(open(sys.argv[1]));assert not s['issues'],s['issues']
PY_AUDIT
}
for text_size in ${TEXT_SIZES:-large}; do
for mode in portrait landscape hdmi720; do
    ext=0; rotation=$mode
    if [ "$mode" = hdmi720 ]; then ext=1280x720; rotation=portrait; fi
    POCKETOS_EXTERNAL_DISPLAY=$ext "$SHELL_BIN" --no-lock --open neon-outpost --rotation "$rotation" \
        --text-size "$text_size" > "$work/$mode.log" 2>&1 & pid=$!
    for _ in $(seq 1 80); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep .1; done
    "$POS" shell info > "$work/info.json"
    python3 - "$work/info.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1]))
assert s['current']=='neon-outpost'
assert any(a['id']=='neon-outpost' for a in s['apps'])
PY
    sleep .2
    "$POS" shell info > "$SHOTS_DIR/shell-$mode-title-info.json"
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-title.png" > /dev/null
    "$POS" call shell shell.tap text=SOUND > /dev/null
    sleep .2
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-sound.png" > /dev/null
    "$POS" call shell shell.tap text=BACK > /dev/null
    sleep .2
    "$POS" call shell shell.tap text=CONTROLS > /dev/null
    sleep .2
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-controls.png" > /dev/null
    audit controls
    "$POS" call shell shell.tap text=BACK > /dev/null
    sleep .2
    "$POS" call shell shell.tap text=PLAY > /dev/null
    sleep .2
    python3 - <<'PY_KEYS'
import json,os,socket,struct,time
for raw in ([167,129,133],[39,1,5]):
 with socket.socket(socket.AF_UNIX) as sock:
  sock.connect(os.environ['POCKETOS_RUNTIME_DIR']+'/shell.sock')
  b=json.dumps({'id':1,'method':'shell.key','params':{'raw':raw}}).encode()
  sock.sendall(struct.pack('!I',len(b))+b)
  def read(n):
   b=b''
   while len(b)<n:
    part=sock.recv(n-len(b));assert part;b+=part
   return b
  result=json.loads(read(struct.unpack('!I',read(4))[0]));assert 'error' not in result,result
 time.sleep(.3)
PY_KEYS
    sleep .2
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-gameplay.png" > /dev/null
    "$POS" shell info > "$SHOTS_DIR/shell-$mode-gameplay-info.json"
    python3 - "$SHOTS_DIR/shell-$mode-gameplay-info.json" <<'PY_FULL'
import json,sys
s=json.load(open(sys.argv[1]));c=s['chrome'];h=c['header'];d=s['display']
assert c['fullscreen'] and c['policy']=='none' and not c['cluster']['shown'],c
assert not h['present'] and h['body_y']==0,h
assert (h['body_content_w'],h['body_content_h'])==(d['width'],d['height']),h
PY_FULL
    "$POS" call shell shell.key code=42 > /dev/null # keyboard base P
    sleep .2
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-pause.png" > /dev/null
    audit pause
    "$POS" shell info > "$SHOTS_DIR/shell-$mode-pause-info.json"
    python3 - "$SHOTS_DIR/shell-$mode-pause-info.json" <<'PY_MENU'
import json,sys
s=json.load(open(sys.argv[1]));c=s['chrome']
assert not c['fullscreen'] and c['policy']=='cluster' and c['cluster']['shown'],c
assert c['header']['present'] and c['header']['body_y']>0,c
PY_MENU
    "$POS" call shell shell.tap text=PLAY > /dev/null # present only when pause succeeded
    sleep .2
    "$POS" call shell shell.audit > "$SHOTS_DIR/shell-$mode-audit.json"
    python3 - "$SHOTS_DIR/shell-$mode-audit.json" <<'PY_AUDIT'
import json,sys
s=json.load(open(sys.argv[1]))
assert not s['issues'],s['issues']
PY_AUDIT
    "$POS" call shell shell.action action=back > /dev/null
    sleep .2
    "$POS" shell info > "$work/info.json"
    python3 - "$work/info.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1]));assert s['current']=='home'
assert not s['chrome']['fullscreen'] and s['chrome']['cluster']['shown']
PY
    "$POS" shell screenshot "$SHOTS_DIR/shell-$mode-launcher.png" > /dev/null
    "$POS" app start neon-outpost > /dev/null
    sleep .2
    "$POS" call shell shell.tap text=PLAY > /dev/null
    sleep .2
    "$POS" app home > /dev/null
    sleep .2
    kill "$pid"; wait "$pid"; pid=
    rm -f "$POCKETOS_RUNTIME_DIR/shell.sock"
done
done
echo "relay_shell_test: three layouts, text sizes ${TEXT_SIZES:-large}, fullscreen/menu geometry, pointer, physical key path, Back/Home and reopen passed"
