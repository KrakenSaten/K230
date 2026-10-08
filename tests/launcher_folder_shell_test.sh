#!/bin/bash
# The launcher's folders (app groups, ui/shell/home.h) in the running
# simulator shell:
#
#   1. home_folder_test, built beside the shell: taps, keys and their
#      boundaries, focus coming back, empty / one / missing / thirty-three
#      games, landscape, fifty open/back rounds and twenty rebuilds without
#      a leak;
#   2. over shell.*: one Games cell and no game cell on the launcher, one
#      Utilities cell holding its seven tools, one Apps cell holding its
#      eight apps (DS §47), the
#      folder opened and closed by shell.folder, an app opened from it comes
#      home to the folder, the keys are the launcher's at home and not while
#      an app is open, forty open/close rounds hold no more art, a folder
#      that is not there is refused;
#   3. a rotation restart comes back in the open folder (and only a
#      restart: a cold start with the mark is on the launcher's page), in
#      both orientations, for Games and for Apps, with the screenshots drawn.
#
# Requires SHELL_BIN (the CMake-built pocketos-shell) and pos (make all).
# SHOTS_DIR=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

fresh() {
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
start_shell() { # [args]
    "$SHELL_BIN" --theme ice --mode normal "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 60); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
info() { "$POS" shell info 2>/dev/null; }
field() { # <json path in python syntax, e.g. ["launcher"]["folder"]>
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps(d$1))" 2>/dev/null
}
cells() { # the app ids on the page showing, sorted, one line
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(sorted(c['id'] for c in d['launcher']['cells'])))" 2>/dev/null
}
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
call_out() { "$POS" call shell "$@" 2>&1; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; }
no_fault() { ! grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log"; }

# ---- 1. the launcher itself, under a pointer and the keys --------------------------
BIN=$(dirname "$SHELL_BIN")/home_folder_test
if [ -x "$BIN" ]; then
    log=$(timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|^home_folder_test:'
    check "home_folder_test passes" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
else
    check "home_folder_test binary present ($BIN)" 0
fi

# ---- 2. over shell.* ---------------------------------------------------------------
fresh
start_shell --rotation portrait --no-lock
check "the launcher has one Games cell" \
    "$([ "$(field '["launcher"]["folders"][0]["id"]')" = '"games"' ] &&
       [ "$(field '["launcher"]["folders"][0]["apps"]')" = 7 ] &&
       [ "$(field '["launcher"]["folders"][0]["w"]')" -ge 64 ] && echo 1 || echo 0)"
check "seven cells for twenty-seven apps, three of them folders, after three favorites (DS §47)" \
    "$([ "$(field '["launcher"]["apps"]')" = 27 ] && [ "$(field '["launcher"]["home_cells"]')" = 7 ] &&
       [ "$(field '["launcher"]["folder_cells"]')" = 3 ] && [ "$(field '["launcher"]["favorite_cells"]')" = 3 ] &&
       echo 1 || echo 0)"
root_cells=$(cells)
check "and no game cell on the launcher's page" \
    "$(case " $root_cells " in *" fleet "*|*" radar "*|*" timber "*) echo 0 ;; *) echo 1 ;; esac)"
check "the launcher's page shows Terminal, RIFT, Browser and Settings, and no other app: $root_cells" \
    "$([ "$root_cells" = "browser rift settings terminal" ] && echo 1 || echo 0)"
check "the Apps cell is the third folder, holding eight" \
    "$([ "$(field '["launcher"]["folders"][2]["id"]')" = '"apps"' ] &&
       [ "$(field '["launcher"]["folders"][2]["name"]')" = '"Apps"' ] &&
       [ "$(field '["launcher"]["folders"][2]["apps"]')" = 8 ] &&
       [ "$(field '["launcher"]["folders"][2]["w"]')" -ge 64 ] && echo 1 || echo 0)"
check "the Utilities cell is the second folder, holding seven" \
    "$([ "$(field '["launcher"]["folders"][1]["id"]')" = '"utilities"' ] &&
       [ "$(field '["launcher"]["folders"][1]["apps"]')" = 7 ] &&
       [ "$(field '["launcher"]["folders"][1]["w"]')" -ge 64 ] && echo 1 || echo 0)"
check "nothing is open, and the keys are the launcher's" \
    "$([ "$(field '["launcher"]["folder"]')" = null ] && [ "$(field '["launcher"]["keys"]')" = true ] && echo 1 || echo 0)"
held_root=$(field '["art"]["bytes_held"]')
shot "$OUT/p-root.png"
reply=$(call_out shell.folder id=games)
check "shell.folder opens Games and says so" "$(printf '%s' "$reply" | grep -q '"folder":[[:space:]]*"games"' && echo 1 || echo 0)"
check "whose page holds the seven games" "$([ "$(cells)" = "2048 blackjack fleet poker radar solitaire timber" ] && echo 1 || echo 0)"
check "and shell.info says it is open" "$([ "$(field '["launcher"]["folder"]')" = '"games"' ] && echo 1 || echo 0)"
shot "$OUT/p-games.png"
"$POS" app start radar >/dev/null 2>&1; sleep 0.4
check "a game opens from the folder" "$([ "$(field '["current"]')" = '"radar"' ] && echo 1 || echo 0)"
check "and has the keys, not the launcher" "$([ "$(field '["launcher"]["keys"]')" = false ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "coming home comes back to the folder" \
    "$([ "$(field '["current"]')" = '"home"' ] && [ "$(field '["launcher"]["folder"]')" = '"games"' ] &&
       [ "$(cells)" = "2048 blackjack fleet poker radar solitaire timber" ] && echo 1 || echo 0)"
check "with the keys the launcher's again" "$([ "$(field '["launcher"]["keys"]')" = true ] && echo 1 || echo 0)"
"$POS" app start fleet >/dev/null 2>&1; sleep 0.4
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "a second game, the same" "$([ "$(field '["launcher"]["folder"]')" = '"games"' ] && echo 1 || echo 0)"
reply=$(call_out shell.folder id=)
check "shell.folder with no id goes back to the launcher's page" \
    "$(printf '%s' "$reply" | grep -q '"folder":[[:space:]]*null' && [ "$(cells)" = "$root_cells" ] && echo 1 || echo 0)"
check "holding the art it held before the folder opened" \
    "$([ "$(field '["art"]["bytes_held"]')" = "$held_root" ] && echo 1 || echo 0)"
call shell.controls
call shell.folder id=games
check "from Controls, shell.folder goes home to the folder" \
    "$([ "$(field '["launcher"]["controls"]')" = false ] && [ "$(field '["launcher"]["folder"]')" = '"games"' ] &&
       [ "$(field '["launcher"]["keys"]')" = true ] && echo 1 || echo 0)"
for k in $(seq 1 40); do
    call shell.folder id=games
    call shell.folder id=
done
check "forty open/close rounds end on the launcher's page with the same art held" \
    "$([ "$(field '["launcher"]["folder"]')" = null ] && [ "$(field '["art"]["bytes_held"]')" = "$held_root" ] &&
       echo 1 || echo 0)"
check "a folder that is not there is refused" \
    "$(call_out shell.folder id=tools | grep -qi 'no such folder' && [ "$(field '["launcher"]["folder"]')" = null ] &&
       echo 1 || echo 0)"
check "and an id that is not a string" "$(call_out shell.folder id=3 | grep -qi 'folder id' && echo 1 || echo 0)"
reply=$(call_out shell.folder id=utilities)
check "shell.folder opens Utilities" "$(printf '%s' "$reply" | grep -q '"folder":[[:space:]]*"utilities"' && echo 1 || echo 0)"
check "whose page holds Clock, Calendar, Calculator, Notes, Files, Recorder and Camera" \
    "$([ "$(cells)" = "calculator calendar camera clock files notes recorder" ] && echo 1 || echo 0)"
check "and not Zabbix, Vision, RIFT, DeskBuddy, MP3 or Video" \
    "$(case " $(cells) " in *" zabbix "*|*" vision "*|*" rift "*|*" deskbuddy "*|*" mp3 "*|*" video "*) echo 0 ;; *) echo 1 ;; esac)"
"$POS" app start calculator >/dev/null 2>&1; sleep 0.4
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "a tool opened from it comes home to it" "$([ "$(field '["launcher"]["folder"]')" = '"utilities"' ] && echo 1 || echo 0)"
reply=$(call_out shell.folder id=apps)
check "shell.folder opens Apps" "$(printf '%s' "$reply" | grep -q '"folder":[[:space:]]*"apps"' && echo 1 || echo 0)"
check "whose page holds DeskBuddy, MP3, Photo, Radio, Video, Vision, Wave and Zabbix" \
    "$([ "$(cells)" = "deskbuddy mp3 photo radio video vision wave zabbix" ] && echo 1 || echo 0)"
"$POS" app start vision >/dev/null 2>&1; sleep 0.4
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "an app opened from it comes home to it" "$([ "$(field '["launcher"]["folder"]')" = '"apps"' ] && echo 1 || echo 0)"
call shell.folder id=games
check "opening Games from Apps swaps the folder" \
    "$([ "$(field '["launcher"]["folder"]')" = '"games"' ] && [ "$(cells)" = "2048 blackjack fleet poker radar solitaire timber" ] &&
       echo 1 || echo 0)"
call shell.folder id=
check "and back, holding the art it held before" "$([ "$(field '["art"]["bytes_held"]')" = "$held_root" ] && echo 1 || echo 0)"
call shell.folder id=games
call shell.lock
call shell.unlock
check "the lock over an open folder leaves it open, the keys back with the launcher" \
    "$([ "$(field '["launcher"]["folder"]')" = '"games"' ] && [ "$(field '["launcher"]["keys"]')" = true ] &&
       echo 1 || echo 0)"
check "the log says what opened and closed" \
    "$(grep -q 'launcher: folder games open, 7 app(s)' "$POCKETOS_LOG_DIR/shell.log" &&
       grep -q 'launcher: folder games closed' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"
stop_shell

# ---- 3. a rotation restart, both orientations ------------------------------------
for o in portrait landscape; do
    fresh
    DOORS_SHELL_RESUMED=open DOORS_LAUNCHER_FOLDER=games start_shell --rotation "$o"
    check "$o: a restart with Games open comes back in it, unlocked" \
        "$([ "$(field '["launcher"]["folder"]')" = '"games"' ] && [ "$(field '["lock"]["locked"]')" = false ] &&
           [ "$(cells)" = "2048 blackjack fleet poker radar solitaire timber" ] && echo 1 || echo 0)"
    check "$o: and says so" "$(grep -q 'folder games open again after the restart' "$POCKETOS_LOG_DIR/shell.log" &&
                               echo 1 || echo 0)"
    info > "$OUT/$o-info.json"
    shot "$OUT/$o-games.png"
    python3 - "$OUT/$o-info.json" "$OUT/$o-games.png" > "$OUT/$o-check.txt" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
d = json.load(open(sys.argv[1]))
W, H, rows = read_png(sys.argv[2])
cells = d["launcher"]["cells"]
dw, dh = d["display"]["width"], d["display"]["height"]
inside = all(0 <= c["x"] and c["x"] + c["w"] <= dw and 0 <= c["y"] and c["y"] + c["h"] <= dh for c in cells)
# Four across in portrait, up to nine in landscape (DS §39.3): as many rows
# as that makes, and no more.
cols = 4 if dh > dw else 9
one_row = len({c["y"] for c in cells}) == (len(cells) + cols - 1) // cols
# Something is drawn in every cell: its portal icon, not the photograph alone.
def busy(c):
    xs = range(c["x"] + c["w"] // 2 - 30, c["x"] + c["w"] // 2 + 30, 3)
    ys = range(c["y"] + 20, c["y"] + 80, 3)
    px = [rows[y][x][:3] for y in ys for x in xs if 0 <= x < W and 0 <= y < H]
    return len(set(px)) > 12
print(int(W == dw and H == dh), int(inside), int(one_row), int(all(busy(c) for c in cells)))
PY
    set -- $(cat "$OUT/$o-check.txt")
    check "$o: the screenshot is the display's size" "${1:-0}"
    check "$o: every game's cell is on the screen, in as few rows as the columns allow" \
        "$([ "${2:-0}" = 1 ] && [ "${3:-0}" = 1 ] && echo 1 || echo 0)"
    check "$o: and drawn" "${4:-0}"
    call shell.folder id=
    check "$o: back on the launcher's page, the Games cell there" \
        "$([ "$(field '["launcher"]["folder"]')" = null ] && [ "$(field '["launcher"]["folders"][0]["w"]')" -ge 64 ] &&
           echo 1 || echo 0)"
    check "$o: no fault logged" "$(no_fault && echo 1 || echo 0)"
    stop_shell
    fresh
    DOORS_SHELL_RESUMED=open DOORS_LAUNCHER_FOLDER=apps start_shell --rotation "$o"
    check "$o: a restart with Apps open comes back in it" \
        "$([ "$(field '["launcher"]["folder"]')" = '"apps"' ] &&
           [ "$(cells)" = "deskbuddy mp3 photo radio video vision wave zabbix" ] && echo 1 || echo 0)"
    shot "$OUT/$o-apps.png"
    check "$o: no fault logged there either" "$(no_fault && echo 1 || echo 0)"
    stop_shell
done
fresh
DOORS_LAUNCHER_FOLDER=games start_shell --rotation portrait --no-lock
check "a cold start with only the folder mark opens on the launcher's page" \
    "$([ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
stop_shell

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR" && cp "$OUT"/*.png "$SHOTS_DIR"/ 2>/dev/null
fi
rm -rf "$OUT"
echo "launcher_folder_shell_test: $failed failure(s)"
exit $((failed > 0))
