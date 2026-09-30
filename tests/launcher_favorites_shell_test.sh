#!/bin/bash
# The launcher's favorites (ui/shell/home.h, DS §42) in the running
# simulator shell. The finger and the keys are home_folder_test's (run by
# launcher_folder_shell_test.sh); this is what the shell adds around them:
#
#   1. a fresh shell: three empty favorites first on the launcher's page,
#      the thirteen cells and two folders after them, nothing in
#      settings.conf;
#   2. shell.favorite: set, change, clear, refused for a duplicate, an app
#      that is not installed, a slot that is not there and an id that is
#      not a string; each change in settings.conf as launcher_favorite_N,
#      and a clear taking its key out; the picker opened for a slot offers
#      every installed app the other slots do not hold, and closes as a
#      folder does;
#   3. kept: a shell restart, and a start in the other orientation, has the
#      same favorites in the first three places; a rotation restart too;
#   4. fails safe: a stored app this build does not have, and a value that
#      is no app id, are empty slots, logged, and the shell runs;
#   5. rounds: forty set/clear/picker rounds hold the art flat, no fault.
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
    "$SHELL_BIN" --theme ice --mode normal "$@" >>"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 60); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
info() { "$POS" shell info 2>/dev/null; }
field() { # <json path in python syntax, e.g. ["launcher"]["favorites"][0]["id"]>
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps(d$1))" 2>/dev/null
}
favs() { # the three slots' installed apps, "-" for empty, one line
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(f['id'] or '-' for f in d['launcher']['favorites']))" 2>/dev/null
}
cells() { # the app ids on the page showing, sorted, one line
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(sorted(c['id'] for c in d['launcher']['cells'])))" 2>/dev/null
}
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
call_out() { "$POS" call shell "$@" 2>&1; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; }
conf() { cat "$POCKETOS_CONFIG_DIR/settings.conf" 2>/dev/null; }
no_fault() { ! grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log"; }
# Where the favorites are against the other cells: 1 when the three slots
# are one row, left to right, above (portrait) or at the head of the first
# line of (landscape) every app's cell, each at least 64 px, all on screen.
first_row() {
    info | python3 -c '
import json, sys
d = json.load(sys.stdin)
f = d["launcher"]["favorites"]
c = d["launcher"]["cells"]
dw, dh = d["display"]["width"], d["display"]["height"]
ok = all("x" in s for s in f) and len({s["y"] for s in f}) == 1
ok = ok and f[0]["x"] < f[1]["x"] < f[2]["x"] and all(s["w"] >= 64 and s["h"] >= 64 for s in f)
ok = ok and all(0 <= s["x"] and s["x"] + s["w"] <= dw and 0 <= s["y"] and s["y"] + s["h"] <= dh for s in f)
y = f[0]["y"]
ok = ok and all(a["y"] > y or (a["y"] == y and a["x"] > f[2]["x"]) for a in c)
print(1 if ok else 0)' 2>/dev/null
}

# ---- 1. a fresh shell ------------------------------------------------------------
fresh
start_shell --rotation portrait --no-lock
check "three favorite slots, all empty" \
    "$([ "$(field '["launcher"]["favorite_cells"]')" = 3 ] && [ "$(favs)" = "- - -" ] &&
       [ "$(field '["launcher"]["favorites_set"]')" = 0 ] && echo 1 || echo 0)"
check "the first row of the launcher, above every app" "$(first_row)"
check "fifteen cells after them, two of them folders" \
    "$([ "$(field '["launcher"]["home_cells"]')" = 15 ] && [ "$(field '["launcher"]["folder_cells"]')" = 2 ] &&
       echo 1 || echo 0)"
check "no picker up, nothing in settings.conf" \
    "$([ "$(field '["launcher"]["picker"]')" = null ] && ! conf | grep -q launcher_favorite && echo 1 || echo 0)"
shot "$OUT/p-empty.png"

# ---- 2. shell.favorite ---------------------------------------------------------------
reply=$(call_out shell.favorite slot=1 id=rift)
check "slot 1 is given RIFT, and says so" \
    "$(printf '%s' "$reply" | grep -q '"id":[[:space:]]*"rift"' && [ "$(favs)" = "rift - -" ] && echo 1 || echo 0)"
check "kept in settings.conf" "$(conf | grep -qx 'launcher_favorite_1=rift' && echo 1 || echo 0)"
call shell.favorite slot=3 id=calculator
check "slot 3 is given Calculator, from inside Utilities" \
    "$([ "$(favs)" = "rift - calculator" ] && conf | grep -qx 'launcher_favorite_3=calculator' &&
       [ "$(field '["launcher"]["favorites_set"]')" = 2 ] && echo 1 || echo 0)"
check "RIFT keeps its own cell, Calculator stays in its folder" \
    "$(case " $(cells) " in *" rift "*) case " $(cells) " in *" calculator "*) echo 0 ;; *) echo 1 ;; esac ;; *) echo 0 ;; esac)"
check "a duplicate is refused: RIFT for slot 2" \
    "$(call_out shell.favorite slot=2 id=rift | grep -qi 'another favorite' && [ "$(favs)" = "rift - calculator" ] &&
       echo 1 || echo 0)"
check "an app that is not installed is refused" \
    "$(call_out shell.favorite slot=2 id=ghost | grep -qi 'no such app' && [ "$(favs)" = "rift - calculator" ] &&
       echo 1 || echo 0)"
check "a slot that is not there is refused (0, 4, 1.5)" \
    "$(call_out shell.favorite slot=0 id=clock | grep -qi 'slot must be' &&
       call_out shell.favorite slot=4 id=clock | grep -qi 'slot must be' &&
       call_out shell.favorite slot=1.5 id=clock | grep -qi 'slot must be' && echo 1 || echo 0)"
check "an id that is not a string is refused" \
    "$(call_out shell.favorite slot=2 id=3 | grep -qi 'app id' && [ "$(favs)" = "rift - calculator" ] && echo 1 || echo 0)"
call shell.favorite slot=1 id=vision
check "slot 1 changed to Vision" \
    "$([ "$(favs)" = "vision - calculator" ] && conf | grep -qx 'launcher_favorite_1=vision' && echo 1 || echo 0)"
call shell.favorite slot=3 id=
check "slot 3 cleared, its key gone from settings.conf" \
    "$([ "$(favs)" = "vision - -" ] && ! conf | grep -q 'launcher_favorite_3' && echo 1 || echo 0)"
call shell.favorite slot=2 id=zabbix
reply=$(call_out shell.favorite slot=3 pick=true)
check "the picker for slot 3 opens at home" \
    "$(printf '%s' "$reply" | grep -q '"picker":[[:space:]]*3' && [ "$(field '["launcher"]["picker"]')" = 3 ] &&
       [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
picked=$(cells)
check "offering the twenty-four apps slots 1 and 2 do not hold" \
    "$([ "$(echo "$picked" | wc -w)" = 24 ] &&
       case " $picked " in *" vision "*|*" zabbix "*) false ;; *) true ;; esac &&
       case " $picked " in *" calculator "*) true ;; *) false ;; esac && echo 1 || echo 0)"
shot "$OUT/p-picker.png"
call shell.folder id=
check "shell.folder with no id closes it, the slot as it was" \
    "$([ "$(field '["launcher"]["picker"]')" = null ] && [ "$(favs)" = "vision zabbix -" ] && echo 1 || echo 0)"
call shell.favorite slot=1 pick=true
call shell.favorite slot=3 id=clock
check "setting a slot closes an open picker" \
    "$([ "$(field '["launcher"]["picker"]')" = null ] && [ "$(favs)" = "vision zabbix clock" ] && echo 1 || echo 0)"
"$POS" app start rift >/dev/null 2>&1; sleep 0.3
call shell.favorite slot=2 pick=true
check "from an app, the picker goes home first" \
    "$([ "$(field '["current"]')" = '"home"' ] && [ "$(field '["launcher"]["picker"]')" = 2 ] && echo 1 || echo 0)"
call shell.folder id=
check "the log says what was set and cleared" \
    "$(grep -q 'launcher: favorite 1 is vision' "$POCKETOS_LOG_DIR/shell.log" &&
       grep -q 'launcher: favorite 3 cleared' "$POCKETOS_LOG_DIR/shell.log" &&
       grep -q 'launcher: favorite 3 picker open' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
shot "$OUT/p-set.png"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"
stop_shell

# ---- 3. kept ---------------------------------------------------------------------------
start_shell --rotation portrait --no-lock
check "a shell restart has the same three" "$([ "$(favs)" = "vision zabbix clock" ] && echo 1 || echo 0)"
check "still the first row" "$(first_row)"
stop_shell
start_shell --rotation landscape --no-lock
check "landscape: the same three" "$([ "$(favs)" = "vision zabbix clock" ] && echo 1 || echo 0)"
check "landscape: the first three places, at the head of the first line" "$(first_row)"
check "landscape: the rest as in portrait" \
    "$([ "$(field '["launcher"]["home_cells"]')" = 15 ] && [ "$(field '["launcher"]["folder_cells"]')" = 2 ] &&
       echo 1 || echo 0)"
shot "$OUT/l-set.png"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"
stop_shell
# No --rotation here: a rotation restart keeps the arguments it was started with.
start_shell --no-lock
call shell.rotation mode=landscape
for _ in $(seq 1 50); do
    [ "$(field '["display"]["orientation"]')" = '"landscape"' ] && break
    sleep 0.2
done
check "a rotation restart has them too" \
    "$([ "$(field '["display"]["orientation"]')" = '"landscape"' ] && [ "$(favs)" = "vision zabbix clock" ] &&
       grep -q 'restarting in place' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"
stop_shell

# ---- 4. fails safe --------------------------------------------------------------------
fresh
printf 'launcher_favorite_1=ghost\nlauncher_favorite_2=../../etc\nlauncher_favorite_3=radio\n' \
    > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell --rotation portrait --no-lock
check "a stored app that is not installed, and a value that is no id: empty slots, the shell runs" \
    "$([ "$(favs)" = "- - radio" ] && [ "$(field '["launcher"]["favorites"][0]["stored"]')" = '"ghost"' ] &&
       [ "$(field '["launcher"]["favorites"][1]["stored"]')" = null ] && echo 1 || echo 0)"
check "and logged" "$(grep -q 'favorite 1 holds ghost, which is not installed' "$POCKETOS_LOG_DIR/shell.log" &&
                      grep -q 'favorite 2 holds' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
check "the stored value is kept until the slot is set" \
    "$(conf | grep -qx 'launcher_favorite_1=ghost' && echo 1 || echo 0)"
call shell.favorite slot=1 id=
check "and cleared on request" "$(! conf | grep -q 'launcher_favorite_1' && echo 1 || echo 0)"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"

# ---- 5. rounds ------------------------------------------------------------------------
held=$(field '["art"]["bytes_held"]')
for k in $(seq 1 40); do
    call shell.favorite slot=1 id=rift
    call shell.favorite slot=2 pick=true
    call shell.folder id=
    call shell.favorite slot=1 id=
done
check "forty set/picker/clear rounds end with the art it held before" \
    "$([ "$(field '["art"]["bytes_held"]')" = "$held" ] && [ "$(favs)" = "- - radio" ] &&
       [ "$(field '["launcher"]["picker"]')" = null ] && echo 1 || echo 0)"
check "no fault logged" "$(no_fault && echo 1 || echo 0)"
stop_shell

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR" && cp "$OUT"/*.png "$SHOTS_DIR"/ 2>/dev/null
fi
rm -rf "$OUT"
echo "launcher_favorites_shell_test: $failed failure(s)"
exit $((failed > 0))
