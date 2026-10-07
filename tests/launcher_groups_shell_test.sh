#!/bin/bash
# The launcher's groups and System as Settings' page (DS §47), in the running
# simulator shell: shell.info, shell.folder, shell.favorite, shell.action and
# real taps through the simulator's own pointer (shell.tap, test hooks only).
#
#   1. the launcher, portrait and landscape, at Small, Medium and Large: the
#      three favorites, then Terminal, RIFT, Browser and Settings and no other
#      app, then the Apps, Utilities and Games cells; Apps, Utilities and Games
#      hold exactly their apps; no app is in two places and every registered
#      app but System is in one; nothing scrolls; the layout audit finds
#      nothing clipped, overlapping or sizeless on the launcher's page, in any
#      folder or in the favorites' picker;
#   2. Settings -> System: a tap on Settings' System row opens System, whose
#      header says so; the back slab and Back come back to Settings, and Back
#      there goes home; Diagnostics closes first; Home goes to the launcher's
#      page; Controls' "About DOORS" opens System with the same way back; the
#      Settings, Terminal and RIFT actions still open their apps; the audit
#      finds nothing new at any size on Settings and System;
#   3. favorites: one holding an app now in a folder and one holding System,
#      kept from before, are shown set; a tap opens the app itself, and
#      coming home from it is the launcher's page, not a folder;
#   4. thirty Settings -> System -> back -> back rounds by taps and actions
#      end home, with the art held as it was and no fault.
#
# Requires SHELL_BIN (the CMake-built pocketos-shell, SDL, with its test
# hooks) and pos (make all). SHOTS_DIR=<dir> keeps the screenshots.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
yes_if() { if "$@"; then echo 1; else echo 0; fi; }

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
field() { # <json path in python syntax>
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps(d$1))" 2>/dev/null
}
cells() { # the app ids on the page showing, sorted, one line
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(sorted(c['id'] for c in d['launcher']['cells'])))" 2>/dev/null
}
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
call_out() { "$POS" call shell "$@" 2>&1; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; }
current() { field '["current"]' | tr -d '"'; }
no_fault() { ! grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log"; }
tap_text() { call shell.tap text="$1"; sleep 0.5; }
# The app header's back slab: the header's first box, 72 x 56, at its left
# padding and centred in its height (shell.c app_open, DS §7).
tap_slab() {
    local xy
    xy=$(info | python3 -c "
import json, sys
h = json.load(sys.stdin)['chrome']['header']
print(int(h['pad_left'] + 36), int(h['y'] + h['h'] / 2))" 2>/dev/null)
    [ -n "$xy" ] || return 1
    set -- $xy
    call shell.tap x="$1" y="$2"
    sleep 0.5
}
action() { call shell.action action="$1"; sleep 0.4; }
# The layout audit's clipped, overlap and zero counts on the screen now, and
# its truncated count, as "c o z t".
audit() {
    call_out shell.audit | python3 -c "
import json, sys
d = json.load(sys.stdin)
d = d.get('result', d)
c = d['count']
print(c['clipped'], c['overlap'], c['zero'], c['truncated'])
for i in d['issues']:
    if i['kind'] != 'truncated':
        print('#', i['kind'], i['path'], repr(i['text']), i['x'], i['y'], i['w'], i['h'], file=sys.stderr)" 2>>"$OUT/audit.txt"
}
clean() { # <what>: nothing clipped, overlapping or sizeless on the screen now
    local a
    a=$(audit)
    set -- $a
    [ -n "$a" ] && [ "$1" = 0 ] && [ "$2" = 0 ] && [ "$3" = 0 ]
}

APPS_FOLDER="deskbuddy mp3 photo radio video vision wave zabbix"
UTILITIES_FOLDER="calculator calendar camera clock files notes recorder"
GAMES_FOLDER="2048 blackjack fleet radar solitaire timber"
ESSENTIALS="browser rift settings terminal"

# ---- 1. the launcher -----------------------------------------------------------------
# Every registered app, from the shell itself.
fresh
start_shell --rotation portrait --no-lock
REGISTERED=$(info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(sorted(a['id'] for a in d['apps'])))")
PAGES=$(info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(a['id'] + '>' + a['page_of'] for a in d['apps'] if 'page_of' in a))")
stop_shell
check "System is registered, as a page of Settings, and nothing else is a page" \
    "$([ "$PAGES" = "system>settings" ] && case " $REGISTERED " in *" system "*) echo 1 ;; *) echo 0 ;; esac)"

for o in portrait landscape; do
    for z in small medium large; do
        fresh
        start_shell --rotation "$o" --no-lock --text-size "$z"
        tag="$o $z"
        check "$tag: the shell is at $z" "$([ "$(field '["text_size"]')" = "\"$z\"" ] && echo 1 || echo 0)"
        check "$tag: three favorite slots first" \
            "$([ "$(field '["launcher"]["favorite_cells"]')" = 3 ] && echo 1 || echo 0)"
        root=$(cells)
        check "$tag: Terminal, RIFT, Browser and Settings are the launcher's only app cells ($root)" \
            "$([ "$root" = "$ESSENTIALS" ] && echo 1 || echo 0)"
        order=$(info | python3 -c "
import json, sys
d = json.load(sys.stdin)
l = d['launcher']
f = {x['id']: x for x in l['folders'] if 'x' in x}
c = {x['id']: x for x in l['cells']}
fav = l['favorites']
seq = [('fav', s) for s in fav] + [(i, c.get(i)) for i in ('terminal', 'rift', 'browser', 'settings')] + \
      [(i, f.get(i)) for i in ('apps', 'utilities', 'games')]
ok = all(b is not None and 'x' in b for _, b in seq)
if ok:
    land = d['display']['width'] > d['display']['height']
    if land:
        # one row, left to right
        ok = len({b['y'] for _, b in seq}) == 1 and all(seq[k][1]['x'] < seq[k + 1][1]['x'] for k in range(len(seq) - 1))
    else:
        # three rows: the favorites, the four apps, the three folders
        rows = [seq[0:3], seq[3:7], seq[7:10]]
        ok = all(len({b['y'] for _, b in r}) == 1 and all(r[k][1]['x'] < r[k + 1][1]['x'] for k in range(len(r) - 1))
                 for r in rows) and rows[0][0][1]['y'] < rows[1][0][1]['y'] < rows[2][0][1]['y']
    dw, dh = d['display']['width'], d['display']['height']
    ok = ok and all(0 <= b['x'] and b['x'] + b['w'] <= dw and 0 <= b['y'] and b['y'] + b['h'] <= dh and
                    b['w'] >= 64 and b['h'] >= 64 for _, b in seq)
print(1 if ok else 0, json.dumps(l['scrolls']), l['home_cells'], l['folder_cells'])")
        set -- $order
        check "$tag: favorites, Terminal, RIFT, Browser, Settings, Apps, Utilities, Games in that order, each on screen and a touch target" "${1:-0}"
        check "$tag: seven places, three of them folders, and no scroll" \
            "$([ "${2:-}" = false ] && [ "${3:-}" = 7 ] && [ "${4:-}" = 3 ] && echo 1 || echo 0)"
        check "$tag: the launcher's page: nothing clipped, overlapping or sizeless" "$(yes_if clean)"
        [ "$z" = small ] && shot "$OUT/$o-root.png"
        placed="$root"
        for f in apps utilities games; do
            call shell.folder id=$f
            got=$(cells)
            case $f in apps) want=$APPS_FOLDER ;; utilities) want=$UTILITIES_FOLDER ;; games) want=$GAMES_FOLDER ;; esac
            check "$tag: $f holds exactly $want" \
                "$([ "$(field '["launcher"]["folder"]')" = "\"$f\"" ] && [ "$got" = "$want" ] && echo 1 || echo 0)"
            check "$tag: $f's page: nothing clipped, overlapping or sizeless, no cell off the screen" \
                "$(clean && info | python3 -c "
import json, sys
d = json.load(sys.stdin)
dw, dh = d['display']['width'], d['display']['height']
sys.exit(0 if all(c['x'] >= 0 and c['x'] + c['w'] <= dw and c['y'] + c['h'] <= dh for c in d['launcher']['cells']) else 1)" && echo 1 || echo 0)"
            [ "$z" = small ] && shot "$OUT/$o-$f.png"
            placed="$placed $got"
            call shell.folder id=
        done
        twice=$(echo $placed | tr ' ' '\n' | sort | uniq -d | tr '\n' ' ')
        missing=$(for a in $REGISTERED; do case " $placed " in *" $a "*) ;; *) echo -n "$a " ;; esac; done)
        check "$tag: no app in two places [$twice], and only System in none [$missing]" \
            "$([ -z "$twice" ] && [ "$missing" = "system " ] && echo 1 || echo 0)"
        call shell.favorite slot=1 pick=true
        picked=$(cells)
        check "$tag: the picker offers every app but System, those in folders too" \
            "$([ "$(echo $picked | wc -w)" = "$(( $(echo $REGISTERED | wc -w) - 1 ))" ] &&
               case " $picked " in *" system "*) false ;; *" photo "*) true ;; *) false ;; esac && echo 1 || echo 0)"
        check "$tag: the picker: nothing clipped, overlapping or sizeless" "$(yes_if clean)"
        call shell.folder id=
        check "$tag: no fault logged" "$(yes_if no_fault)"
        stop_shell
    done
done

# ---- 2. Settings -> System ------------------------------------------------------------
for o in portrait landscape; do
    fresh
    start_shell --rotation "$o" --no-lock
    "$POS" app start settings >/dev/null 2>&1; sleep 0.5
    check "$o: Settings is open" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
    tap_text "System"
    check "$o: a tap on Settings' System row opens System" "$([ "$(current)" = system ] && echo 1 || echo 0)"
    shot "$OUT/$o-system.png"
    tap_slab
    check "$o: System's back slab comes back to Settings" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
    tap_slab
    check "$o: Settings' back slab goes home, to the launcher's page" \
        "$([ "$(current)" = home ] && [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
    "$POS" app start settings >/dev/null 2>&1; sleep 0.4
    tap_text "System"
    action back
    check "$o: Back from System: Settings" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
    action back
    check "$o: Back from Settings: home" "$([ "$(current)" = home ] && echo 1 || echo 0)"
    action back
    check "$o: Back at the launcher's page does nothing" "$([ "$(current)" = home ] && echo 1 || echo 0)"
    "$POS" app start system >/dev/null 2>&1; sleep 0.4
    # Diagnostics is on System's SERVICES page (DS §52.5).
    tap_text "SERVICES"
    tap_text "Diagnostics"
    action back
    check "$o: Back from System's Diagnostics closes Diagnostics first, System stays" \
        "$([ "$(current)" = system ] && echo 1 || echo 0)"
    action back
    check "$o: then Back is Settings" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
    "$POS" app start system >/dev/null 2>&1; sleep 0.4
    action home
    check "$o: Home from System is the launcher's page" \
        "$([ "$(current)" = home ] && [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
    call shell.folder id=apps
    "$POS" app start system >/dev/null 2>&1; sleep 0.4
    action home
    check "$o: Home with a folder open under the app is the launcher's own page" \
        "$([ "$(current)" = home ] && [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
    call shell.controls
    sleep 0.3
    tap_text "About DOORS"
    check "$o: Controls' About DOORS opens System" "$([ "$(current)" = system ] && echo 1 || echo 0)"
    action back
    check "$o: and its way back is Settings too" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
    action home
    for a in settings terminal rift; do
        action "$a"
        check "$o: the $a action opens $a" "$([ "$(current)" = "$a" ] && echo 1 || echo 0)"
    done
    action home
    call shell.folder id=games
    action back
    check "$o: Back from a folder: the launcher's page" \
        "$([ "$(current)" = home ] && [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
    for z in small medium large; do
        call shell.text_size size=$z
        sleep 0.3
        "$POS" app start settings >/dev/null 2>&1; sleep 0.5
        check "$o $z: Settings has a System row a finger reaches, and it opens System" \
            "$(tap_text "System"; [ "$(current)" = system ] && echo 1 || echo 0)"
        check "$o $z: System: nothing clipped, overlapping or sizeless" "$(yes_if clean)"
        action back
        check "$o $z: back in Settings: nothing clipped, overlapping or sizeless" \
            "$([ "$(current)" = settings ] && clean && echo 1 || echo 0)"
        action home
    done
    call shell.text_size size=small
    check "$o: no fault logged" "$(yes_if no_fault)"
    stop_shell
done

# ---- 3. favorites kept from before ----------------------------------------------------
for o in portrait landscape; do
    fresh
    printf 'launcher_favorite_1=photo\nlauncher_favorite_2=system\nlauncher_favorite_3=clock\n' \
        > "$POCKETOS_CONFIG_DIR/settings.conf"
    start_shell --rotation "$o" --no-lock
    favs=$(info | python3 -c "import json,sys; d=json.load(sys.stdin); print(' '.join(f['id'] or '-' for f in d['launcher']['favorites']))")
    check "$o: favorites holding Photo (now in Apps), System and Clock (in Utilities) are all set ($favs)" \
        "$([ "$favs" = "photo system clock" ] && echo 1 || echo 0)"
    for k in 1 2 3; do
        xy=$(info | python3 -c "
import json, sys
f = json.load(sys.stdin)['launcher']['favorites'][$k - 1]
print(int(f['x'] + f['w'] / 2), int(f['y'] + f['h'] / 2))")
        set -- $xy
        call shell.tap x="$1" y="$2"
        sleep 0.6
        want=$(echo "$favs" | cut -d' ' -f$k)
        check "$o: a tap on favorite $k opens $want itself" "$([ "$(current)" = "$want" ] && echo 1 || echo 0)"
        if [ "$want" = system ]; then
            action back
            check "$o: and System's way back is still Settings" "$([ "$(current)" = settings ] && echo 1 || echo 0)"
        fi
        "$POS" app home >/dev/null 2>&1; sleep 0.3
        check "$o: coming home from it is the launcher's page, not a folder" \
            "$([ "$(current)" = home ] && [ "$(field '["launcher"]["folder"]')" = null ] && echo 1 || echo 0)"
    done
    check "$o: settings.conf is as it was" \
        "$(grep -qx 'launcher_favorite_2=system' "$POCKETOS_CONFIG_DIR/settings.conf" &&
           grep -qx 'launcher_favorite_1=photo' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
    call shell.favorite slot=1 id=vision
    check "$o: an app in a folder may be made a favorite" \
        "$(grep -qx 'launcher_favorite_1=vision' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
    check "$o: no fault logged" "$(yes_if no_fault)"
    stop_shell
done

# ---- 4. rounds ---------------------------------------------------------------------------
fresh
start_shell --rotation portrait --no-lock
held=$(field '["art"]["bytes_held"]')
pid=$SP
ok=0
for k in $(seq 1 30); do
    "$POS" app start settings >/dev/null 2>&1; sleep 0.3
    tap_text "System"
    [ "$(current)" = system ] || continue
    if [ $((k % 2)) = 0 ]; then tap_slab; else action back; fi
    [ "$(current)" = settings ] || continue
    action back
    [ "$(current)" = home ] && ok=$((ok + 1))
done
check "thirty Settings -> System -> back -> back rounds, by taps and by Back ($ok)" "$([ "$ok" = 30 ] && echo 1 || echo 0)"
check "the same shell, holding the art it held before" \
    "$(kill -0 "$pid" 2>/dev/null && [ "$(field '["art"]["bytes_held"]')" = "$held" ] && echo 1 || echo 0)"
check "no fault logged" "$(yes_if no_fault)"
stop_shell

if [ -s "$OUT/audit.txt" ]; then
    echo "     audit issues seen:"; sort -u "$OUT/audit.txt" | head -20
fi
if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR" && cp "$OUT"/*.png "$SHOTS_DIR"/ 2>/dev/null
fi
rm -rf "$OUT"
echo "launcher_groups_shell_test: $failed failure(s)"
exit $((failed > 0))
