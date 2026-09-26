#!/bin/bash
# The status chrome policy in the running shell (DS §30, ui/shell/chrome.h).
#
#   1. The rules in the source: one resolver, one content box, the chrome
#      resolved before an app is created, apps that declare and never
#      manipulate, and stage 2 - the seven fullscreen apps declare NONE and
#      no other app declares anything.
#   2. The pure test (tests/chrome_test, make test) when it has been built.
#   3. The running shell: FULL at home in both orientations and under every
#      other app in portrait; NONE under the seven fullscreen apps in both,
#      their hint drawn in the header instead; COMPACT under every other app
#      in landscape, and drawn so -
#      the 32 px bar with its hairline, the header straight under it, the
#      wordmark, the chip and the clock in the bar, a hint drawn in it; FULL
#      again on coming home; open, close and open again over IPC; the radio
#      poll answering under COMPACT; the lock over a fullscreen app showing
#      the bar an ordinary app has, and hiding it again once open; and NONE
#      through the simulator's test hook - no bar, the content from the top
#      edge, the header's back slab moved clear of the rounded corner, the
#      hint written and nothing faulting.
#   4. The radio chip (DS §30.1, §32.4): its text drawn whole under COMPACT
#      from a real radiod poll, and RX, TX, OFF and -- each given a whole
#      line in a chip inside its bar, at home and in apps, both orientations.
#
# Requires: SHELL_BIN (the CMake-built simulator) and pos and radiod (make
# all). Run from the repository root. SHOTS_DIR=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
RADIOD=${RADIOD:-services/radiod/radiod}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
line() { grep -n "$1" "$2" | head -1 | cut -d: -f1; }
APPS="radio system fleet radar timber notes clock calendar calculator settings wave rift files camera"
# DS §30.4 stage 2: the apps that declare NONE, fullscreen in both orientations.
FULLSCREEN="rift notes wave fleet radar timber camera"
is_fullscreen() { case " $FULLSCREEN " in *" $1 "*) return 0 ;; esac; return 1; }

# ---- 1. the rules in the source ------------------------------------------------
check "the shell names the DS §7 bar twice: to build it, and to check FULL is it" \
    "$([ "$(grep -c 'POCKETUI_STATUS_BAR_H' ui/shell/shell.c)" = 2 ] && echo 1 || echo 0)"
check "one place sets the content area's top and height, from the chrome in force" \
    "$([ "$(grep -c 'lv_obj_set_y(sh.content' ui/shell/shell.c)" = 1 ] &&
       [ "$(grep -c 'lv_obj_set_height(sh.content' ui/shell/shell.c)" = 1 ] &&
       grep -q 'chrome_content_box(sh.chrome' ui/shell/shell.c && echo 1 || echo 0)"
check "and the content area is never sized from a constant" \
    "$(grep -q 'lv_obj_set_size(sh.content' ui/shell/shell.c && echo 0 || echo 1)"
check "an app's chrome is resolved before its header and body exist" \
    "$([ "$(line 'chrome_apply(chrome_resolve(declared_chrome(app)' ui/shell/shell.c)" -lt \
         "$(line 'header = lv_obj_create(sh.app_root)' ui/shell/shell.c)" ] && echo 1 || echo 0)"
check "and so before the app is created" \
    "$([ "$(line 'chrome_apply(chrome_resolve(declared_chrome(app)' ui/shell/shell.c)" -lt \
         "$(line 'sh.app_priv = app->create(body)' ui/shell/shell.c)" ] && echo 1 || echo 0)"
check "coming home puts the launcher's chrome back" \
    "$(sed -n '/^void pocketos_shell_go_home/,/^}/p' ui/shell/shell.c |
       grep -q 'chrome_apply(chrome_resolve(POCKETOS_CHROME_DEFAULT' && echo 1 || echo 0)"
# The DOORS launcher (DS §31.3) lays out in the content area as the
# launcher's own chrome left it: that chrome is applied, then the launcher
# built, and the launcher measures its parent rather than assuming a bar.
check "the launcher is laid out below the launcher's own chrome" \
    "$(grep -A6 'chrome_apply(chrome_resolve(POCKETOS_CHROME_DEFAULT, is_landscape(sh.display.geometry.rotation), true),' ui/shell/shell.c |
       grep -q 'home_build();' &&
       grep -q 'in.height = lv_obj_get_height(parent);' ui/shell/home.c && echo 1 || echo 0)"
check "the keyboard reserve goes through the same box" \
    "$([ "$(grep -c 'content_box(POS_KB_H)' ui/shell/shell.c)" = 1 ] &&
       [ "$(grep -c 'content_box(0)' ui/shell/shell.c)" = 1 ] && echo 1 || echo 0)"
check "NONE hides the bar and deletes nothing of it" \
    "$(grep -q 'lv_obj_add_flag(sh.status_bar, LV_OBJ_FLAG_HIDDEN)' ui/shell/shell.c &&
       ! grep -q 'lv_obj_delete(sh.status' ui/shell/shell.c && echo 1 || echo 0)"
check "under NONE the header takes the bar's corner insets" \
    "$(sed -n '/if (sh.chrome == POCKETOS_CHROME_NONE)/,/}/p' ui/shell/shell.c |
       grep -q 'pocketui_apply_bar_insets(header, POS_EDGE_TOP)' && echo 1 || echo 0)"
hits=$(grep -rnE 'chrome_(apply|resolve|height|content_box)|POCKETUI_STATUS_BAR_H|POCKETOS_CHROME_[A-Z_]*_H' apps --include='*.c' --include='*.h')
check "no app resolves, reads or touches the chrome" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rn '\.chrome = ' apps --include='*.c' | grep -v 'POCKETOS_CHROME_NONE')
declared=$(grep -rln '\.chrome = POCKETOS_CHROME_NONE' apps --include='*.c' | cut -d/ -f2 | sort | tr '\n' ' ')
# Zabbix (DS §35) is in the shell by default and declares NONE like the seven.
check "stage 2: the seven fullscreen apps, and Zabbix, declare NONE and no app declares anything else ($declared) (DS §30.4, §34, §35)" \
    "$([ -z "$hits" ] && [ "$declared" = "camera fleet notes radar rift timber wave zabbix " ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the test hook that forces a chrome is compiled out of the panel's build" \
    "$(sed -n '/POCKETOS_SHELL_TEST_HOOKS/,/#endif/p' ui/shell/shell.c | grep -q 'getenv("POCKETOS_TEST_CHROME")' &&
       [ "$(grep -c 'POCKETOS_TEST_CHROME' ui/shell/shell.c)" = 1 ] && echo 1 || echo 0)"
check "the shell, and every app test, is built with the one resolver" \
    "$([ "$(grep -c '^ *chrome\.c$' ui/shell/CMakeLists.txt)" -ge 10 ] && echo 1 || echo 0)"
hits=$(grep -ln 'define STATUS_H POCKETUI_STATUS_BAR_H' tests/*_app_test.c | grep -v 'wave_app_test' | tr '\n' ' ')
check "every app test that turns its display builds its frame from the resolver (${hits:-none left})" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- 2. the pure test ----------------------------------------------------------
if [ -x tests/chrome_test ]; then
    log=$(./tests/chrome_test 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL'
    check "the resolver, the heights and the content box (tests/chrome_test)" \
        "$([ "$rc" = 0 ] && printf '%s\n' "$log" | grep -qE '^chrome_test: [0-9]+ checks, 0 failure' && echo 1 || echo 0)"
else
    echo "note: tests/chrome_test is not built here (make test builds it); its checks are skipped"
fi

# ---- 3. the running shell -------------------------------------------------------
fresh() { # a new set of state directories
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
shot() { # <png> <log> [shell args]: one run, drawn
    local png=$1 log=$2
    shift 2
    "$SHELL_BIN" --theme ice --mode normal --screenshot "$png" --exit-after-ms 900 "$@" >"$log" 2>&1
}
logs() { cat "$1" "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null; }
chrome_of() { "$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -oE '"chrome":\{[^}]*\}'; }
start_shell() { # [args]: a running shell, waited for
    "$SHELL_BIN" --theme ice --mode normal "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.3
}
stop_shell() {
    kill "$SP" 2>/dev/null
    wait "$SP" 2>/dev/null
}
# geometry <png>: "<hairline y> <slab top y> <slab left x> <ink groups in the bar band>"
#   hairline: the first row at mid-width drawn in the line token (the bar's
#             bottom rule), -1 when there is none in the top 64 rows;
#   slab:     the first row at x 56 drawn in surface - the header's back slab,
#             whether its left edge is at 20 or, inset for the corner, 30 (50
#             at the top of landscape);
#   left:     that slab's left edge, 20 px below its top;
#   groups:   ink runs across the band above the hairline, separated by more
#             than 24 px: wordmark, [hint,] chip, clock.
geometry() {
    python3 - "$1" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
bg, surf, line = hexrgb(tok["bg"]), hexrgb(tok["surface"]), hexrgb(tok["line"])
W, H, rows = read_png(sys.argv[1])
px = lambda x, y: rows[y][x][:3]
near = lambda p, c: all(abs(p[k] - c[k]) <= 3 for k in range(3))
hair = next((y for y in range(0, 64) if near(px(W // 2, y), line)), -1)
slab = next((y for y in range(0, 140) if near(px(56, y), surf)), -1)
left = next((x for x in range(0, 120) if slab >= 0 and near(px(x, slab + 20), surf)), -1)
band = hair if hair > 4 else 32
barbg = px(2, 1)
xs = sorted(set(x for y in range(2, band - 1) for x in range(W) if not near(px(x, y), barbg)))
groups, last = 0, -100
for x in xs:
    if x - last > 24:
        groups += 1
    last = x
print(hair, slab, left, groups)
PY
}

# header_hint <png>: 1 when something is drawn at the right end of an app
# header that starts at the top edge (NONE) - the hint the bar would have held.
# The title stops well short of the right 40 %, and the header's right inset
# (30 or 50 px) is left out.
header_hint() {
    python3 - "$1" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
bg = tuple(int(tok["bg"][i:i + 2], 16) for i in (1, 3, 5))
W, H, rows = read_png(sys.argv[1])
near = lambda p, c: all(abs(p[k] - c[k]) <= 3 for k in range(3))
ink = any(not near(rows[y][x][:3], bg) for y in range(8, 64) for x in range(W * 6 // 10, W - 30))
print(1 if ink else 0)
PY
}

# Portrait: FULL at home and under every other app, and drawn as it always was -
# the hairline at row 55, the back slab from row 64 at x 20.
fresh
shot "$OUT/p-home.png" "$OUT/p-home.log" --rotation portrait --no-lock
check "portrait, home: FULL, 56 px" \
    "$(logs "$OUT/p-home.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for home' && echo 1 || echo 0)"
n=0
for id in $APPS; do
    want="chrome: full, status bar 56 px, content from y 56, for $id"
    is_fullscreen "$id" && want="chrome: none, status bar 0 px, content from y 0, for $id"
    fresh
    shot "$OUT/p-$id.png" "$OUT/p-$id.log" --rotation portrait --open "$id"
    logs "$OUT/p-$id.log" | grep -q "$want" &&
        ! logs "$OUT/p-$id.log" | grep -qE ' ERROR |assert' && n=$((n + 1))
done
check "portrait: the seven other apps open under FULL and the seven fullscreen ones under NONE, faulting nothing ($n of 14)" \
    "$([ "$n" = 14 ] && echo 1 || echo 0)"
set -- $(geometry "$OUT/p-system.png")
check "portrait System: the bar's hairline is row 55, the back slab starts at row 64, x 20 (got $1 $2 $3)" \
    "$([ "$1" = 55 ] && [ "$2" = 64 ] && [ "$3" = 20 ] && echo 1 || echo 0)"
check "portrait System: the wordmark, the chip and the clock are in the bar ($4 ink groups)" \
    "$([ "$4" = 3 ] && echo 1 || echo 0)"
set -- $(geometry "$OUT/p-timber.png")
check "portrait Timber, fullscreen: no bar, the back slab from row 8 at x 30, clear of the corner (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 30 ] && echo 1 || echo 0)"
check "portrait Timber: its STANDBY hint is drawn at the header's right end" \
    "$(header_hint "$OUT/p-timber.png")"

# Landscape: FULL at home, COMPACT under every app - the hairline at row 31,
# the back slab from row 40 at x 20, the same three things in the bar, and
# Fleet's COMMAND hint drawn as a fourth.
fresh
shot "$OUT/l-home.png" "$OUT/l-home.log" --rotation landscape --no-lock
check "landscape, home: FULL, 56 px" \
    "$(logs "$OUT/l-home.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for home' && echo 1 || echo 0)"
check "landscape, home: the grouped launcher below a 56 px bar" \
    "$(logs "$OUT/l-home.log" | grep -q 'launcher: 4 group(s), 15 app(s), landscape' && echo 1 || echo 0)"
# On the launcher the bar lies on the home photograph with no fill and no
# rule (DS §31.1); its height is still FULL's, as the log line above says.
set -- $(geometry "$OUT/l-home.png")
check "landscape home: the bar draws no hairline over the photograph (got $1)" \
    "$([ "$1" = -1 ] && echo 1 || echo 0)"
n=0
for id in $APPS; do
    # Every app takes the landscape default but the seven fullscreen ones,
    # which declare NONE (DS §30.4, stage 2).
    want="chrome: compact, status bar 32 px, content from y 32, for $id"
    is_fullscreen "$id" && want="chrome: none, status bar 0 px, content from y 0, for $id"
    fresh
    shot "$OUT/l-$id.png" "$OUT/l-$id.log" --rotation landscape --open "$id"
    logs "$OUT/l-$id.log" | grep -q "$want" &&
        logs "$OUT/l-$id.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for home' &&
        ! logs "$OUT/l-$id.log" | grep -qE ' ERROR |assert' && n=$((n + 1))
done
check "landscape: the seven DEFAULT apps open under COMPACT and the seven fullscreen ones under NONE, all after a FULL home, faulting nothing ($n of 14)" \
    "$([ "$n" = 14 ] && echo 1 || echo 0)"
set -- $(geometry "$OUT/l-system.png")
check "landscape System: the bar's hairline is row 31, the back slab starts at row 40, x 20 (got $1 $2 $3)" \
    "$([ "$1" = 31 ] && [ "$2" = 40 ] && [ "$3" = 20 ] && echo 1 || echo 0)"
check "landscape System: the wordmark, the chip and the clock are in the compact bar ($4 ink groups)" \
    "$([ "$4" = 3 ] && echo 1 || echo 0)"
set -- $(geometry "$OUT/l-timber.png")
check "landscape Timber, fullscreen: no bar, the back slab from row 8 at x 50, clear of the corner (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 50 ] && echo 1 || echo 0)"
check "landscape Timber: its STANDBY hint is drawn at the header's right end" \
    "$(header_hint "$OUT/l-timber.png")"
set -- $(geometry "$OUT/l-fleet.png")
check "landscape Fleet, fullscreen: no bar, the back slab from row 8 at x 50 (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 50 ] && echo 1 || echo 0)"
check "landscape Fleet: its COMMAND hint is drawn at the header's right end" \
    "$(header_hint "$OUT/l-fleet.png")"
hits=$(for id in $APPS; do logs "$OUT/l-$id.log" | grep -hE ' WARN |\[Warn\]' | grep -v 'radiod unavailable'; done)
check "landscape: no warning from any app but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# Over IPC: open, home, open again - the chrome follows every transition, and
# shell.info says what is in force. Then radiod arrives while an app is open
# under COMPACT, and the poll that drives the chip sees it.
fresh
start_shell --rotation landscape
c=$(chrome_of)
check "over IPC, at home: shell.info reports FULL, 56 ($c)" \
    "$(printf '%s' "$c" | grep -q '"policy":"full","status_bar_height":56' && echo 1 || echo 0)"
cycles=0
for id in system $FULLSCREEN $FULLSCREEN; do
    want='"policy":"compact","status_bar_height":32'
    is_fullscreen "$id" && want='"policy":"none","status_bar_height":0'
    "$POS" app start "$id" >/dev/null 2>&1; sleep 0.4
    a=$(chrome_of)
    "$POS" app home >/dev/null 2>&1; sleep 0.3
    b=$(chrome_of)
    printf '%s' "$a" | grep -q "$want" &&
        printf '%s' "$b" | grep -q '"policy":"full","status_bar_height":56' && cycles=$((cycles + 1))
done
check "System, then each fullscreen app opened, closed and reopened: COMPACT or NONE while open, FULL again at home, every time ($cycles of 15)" \
    "$([ "$cycles" = 15 ] && echo 1 || echo 0)"
check "each opening and each return logged its chrome" \
    "$([ "$(grep -c 'chrome: compact, status bar 32 px' "$POCKETOS_LOG_DIR/shell.log")" = 1 ] &&
       [ "$(grep -c 'chrome: none, status bar 0 px' "$POCKETOS_LOG_DIR/shell.log")" = 14 ] &&
       [ "$(grep -c 'chrome: full, status bar 56 px, content from y 56, for home' "$POCKETOS_LOG_DIR/shell.log")" = 16 ] && echo 1 || echo 0)"
# The lock over a fullscreen app: the lock lies under the bar, so while it is
# engaged the bar comes back as an ordinary app would have it here (COMPACT
# in landscape) and the lock looks the same over either; opened again, the
# app is fullscreen as it was. Compared with the lock over System, the band
# the bar sits in is drawn the same (the environment bar has no clock).
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
band_same() { # <png a> <png b> <rows>
    python3 - "$1" "$2" "$3" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
Wa, Ha, a = read_png(sys.argv[1])
Wb, Hb, b = read_png(sys.argv[2])
n = int(sys.argv[3])
same = Wa == Wb and all(all(abs(a[y][x][k] - b[y][x][k]) <= 3 for k in range(3))
                        for y in range(n) for x in range(Wa))
print(1 if same else 0)
PY
}
"$POS" app start system >/dev/null 2>&1; sleep 0.4
call shell.lock; sleep 0.6
"$POS" shell screenshot "$OUT/l-lock-system.png" >/dev/null 2>&1
call shell.unlock; sleep 0.6
"$POS" app start notes >/dev/null 2>&1; sleep 0.4
call shell.lock; sleep 0.6
"$POS" shell screenshot "$OUT/l-lock-notes.png" >/dev/null 2>&1
check "locked over fullscreen Notes: the bar band is drawn as over System" \
    "$(band_same "$OUT/l-lock-system.png" "$OUT/l-lock-notes.png" 32)"
check "and the chrome in force is still Notes' NONE, with the compact bar shown over the lock ($(chrome_of))" \
    "$(chrome_of | grep -q '"policy":"none","status_bar_height":0,"shown_height":32' && echo 1 || echo 0)"
call shell.unlock; sleep 0.6
check "unlocked: no bar shown over Notes again ($(chrome_of))" \
    "$(chrome_of | grep -q '"policy":"none","status_bar_height":0,"shown_height":0' && echo 1 || echo 0)"
"$POS" shell screenshot "$OUT/l-unlocked-notes.png" >/dev/null 2>&1
set -- $(geometry "$OUT/l-unlocked-notes.png")
check "opened again: Notes is fullscreen, no bar, its back slab from row 8 (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 50 ] && echo 1 || echo 0)"
"$POS" app start system >/dev/null 2>&1; sleep 0.4
"$POS" shell screenshot "$OUT/l-reopened.png" >/dev/null 2>&1
set -- $(geometry "$OUT/l-reopened.png")
check "reopened over IPC: drawn under the 32 px bar again (got $1 $2 $3, $4 groups)" \
    "$([ "$1" = 31 ] && [ "$2" = 40 ] && [ "$3" = 20 ] && [ "$4" = 3 ] && echo 1 || echo 0)"
if [ -x "$RADIOD" ]; then
    "$RADIOD" --backend mock >"$OUT/radiod.log" 2>&1 &
    RP=$!
    for _ in $(seq 1 40); do grep -q 'radiod is answering again' "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null && break; sleep 0.1; done
    check "radiod started while System is open under COMPACT: the status poll sees it within the next ticks" \
        "$(grep -q 'radiod is answering again' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
    check "and it was seen after the compact chrome was applied, not before" \
        "$([ "$(line 'radiod is answering again' "$POCKETOS_LOG_DIR/shell.log")" -gt \
             "$(grep -n 'chrome: compact' "$POCKETOS_LOG_DIR/shell.log" | tail -1 | cut -d: -f1)" ] && echo 1 || echo 0)"
    kill "$RP" 2>/dev/null; wait "$RP" 2>/dev/null
else
    check "radiod present for the poll check ($RADIOD)" 0
fi
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "the landscape shell logged no fault" \
    "$(grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log" && echo 0 || echo 1)"
stop_shell

# NONE forced on an app that does not declare it, through the simulator's hook: no bar, the
# content from the top edge, the header's back slab moved to x 50 so it clears
# the 50 px corner square landscape has at its top (platform.h, DS §21.1), and
# an app that writes a hint faulting nothing.
fresh
POCKETOS_TEST_CHROME=none shot "$OUT/l-none.png" "$OUT/l-none.log" --rotation landscape --open system
check "landscape, NONE forced: the shell says so" \
    "$(logs "$OUT/l-none.log" | grep -q 'chrome: none, status bar 0 px, content from y 0, for system' && echo 1 || echo 0)"
check "and home before it was still FULL: the hook reaches apps only" \
    "$(logs "$OUT/l-none.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for home' && echo 1 || echo 0)"
set -- $(geometry "$OUT/l-none.png")
check "landscape NONE: no bar hairline, the back slab from row 8 at x 50, clear of the corner (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 50 ] && echo 1 || echo 0)"
check "landscape NONE: nothing drawn in the top-left corner square" \
    "$(python3 - "$OUT/l-none.png" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
bg = tuple(int(tok["bg"][i:i + 2], 16) for i in (1, 3, 5))
W, H, rows = read_png(sys.argv[1])
print(1 if all(all(abs(rows[y][x][k] - bg[k]) <= 3 for k in range(3)) for y in range(50) for x in range(50)) else 0)
PY
)"
check "landscape NONE: faulted nothing" "$(logs "$OUT/l-none.log" | grep -qE ' ERROR |assert' && echo 0 || echo 1)"
fresh
POCKETOS_TEST_CHROME=none shot "$OUT/l-none-fleet.png" "$OUT/l-none-fleet.log" --rotation landscape --open fleet
check "landscape, NONE, Fleet: a hint written to the hidden bar and the header faults nothing" \
    "$(logs "$OUT/l-none-fleet.log" | grep -q 'chrome: none, status bar 0 px' &&
       ! logs "$OUT/l-none-fleet.log" | grep -qE ' ERROR |assert' && echo 1 || echo 0)"
fresh
POCKETOS_TEST_CHROME=full shot "$OUT/l-full.png" "$OUT/l-full.log" --rotation landscape --open system
check "landscape, FULL declared: honoured, 56 px" \
    "$(logs "$OUT/l-full.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for system' && echo 1 || echo 0)"
set -- $(geometry "$OUT/l-full.png")
check "landscape FULL: drawn as the 56 px bar (got $1 $2 $3)" \
    "$([ "$1" = 55 ] && [ "$2" = 64 ] && [ "$3" = 20 ] && echo 1 || echo 0)"
fresh
POCKETOS_TEST_CHROME=none shot "$OUT/p-none.png" "$OUT/p-none.log" --rotation portrait --open system
check "portrait, NONE declared: honoured, the content from the top edge (DS §30.4 stage 2)" \
    "$(logs "$OUT/p-none.log" | grep -q 'chrome: none, status bar 0 px, content from y 0, for system' && echo 1 || echo 0)"
fresh
POCKETOS_TEST_CHROME=compact shot "$OUT/p-compact.png" "$OUT/p-compact.log" --rotation portrait --open system
check "portrait, COMPACT declared: still FULL - stage 1 holds for it in the running shell" \
    "$(logs "$OUT/p-compact.log" | grep -q 'chrome: full, status bar 56 px, content from y 56, for system' && echo 1 || echo 0)"
fresh
POCKETOS_TEST_CHROME=sideways shot "$OUT/l-bad.png" "$OUT/l-bad.log" --rotation landscape --open system
check "a hook value that is not a chrome is ignored with a warning, and the default applies" \
    "$(logs "$OUT/l-bad.log" | grep -q "test hook: chrome 'sideways'" &&
       logs "$OUT/l-bad.log" | grep -q 'chrome: compact, status bar 32 px' && echo 1 || echo 0)"

# ---- 4. the radio chip under every chrome (DS §30.1) ----------------------------
# The chip is a label, and a label clips what its content box cannot hold.
# Under the 32 px bar a 24 px chip with 5 px of padding left 14 px for the
# 22 px line of the symbol font it draws in, and "RX" lost its top on unit A.
# The simulator never showed it: with no radiod the chip says "--", which
# sits mid-line. So the chip is drawn here with a radio that answers RX, and
# measured: its text must be as tall under COMPACT as under FULL.
chip_ink() { # <png> <bar height>: rows of text inside the RX chip's fill, in the bar
    python3 - "$1" "$2" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
fill = tuple(int(tok["radio_rx"][i:i + 2], 16) for i in (1, 3, 5))
W, H, rows = read_png(sys.argv[1])
near = lambda p, c, d: all(abs(p[k] - c[k]) <= d for k in range(3))
pts = [(x, y) for y in range(0, int(sys.argv[2])) for x in range(W) if near(rows[y][x], fill, 3)]
if not pts:
    print(0)
    sys.exit()
x1, x2 = min(p[0] for p in pts), max(p[0] for p in pts)
y1, y2 = min(p[1] for p in pts), max(p[1] for p in pts)
ink = [y for y in range(y1, y2 + 1) if any(not near(rows[y][x], fill, 40) for x in range(x1 + 6, x2 - 5))]
print(len(ink))
PY
}
if [ -x "$RADIOD" ]; then
    fresh
    "$RADIOD" --backend mock >"$OUT/radiod-chip.log" 2>&1 &
    RP=$!
    sleep 0.4
    shot "$OUT/chip-full.png" "$OUT/chip-full.log" --rotation portrait --open system
    shot "$OUT/chip-compact.png" "$OUT/chip-compact.log" --rotation landscape --open system
    kill "$RP" 2>/dev/null; wait "$RP" 2>/dev/null
    full=$(chip_ink "$OUT/chip-full.png" 56)
    compact=$(chip_ink "$OUT/chip-compact.png" 32)
    check "the RX chip is drawn with its text in FULL, from a real radiod poll ($full rows of ink)" \
        "$([ "$full" -ge 10 ] && echo 1 || echo 0)"
    check "and its text is not clipped under COMPACT: as many rows of ink as in FULL ($compact of $full)" \
        "$([ "$compact" = "$full" ] && echo 1 || echo 0)"
else
    check "radiod present for the chip check ($RADIOD)" 0
fi
# Every state the chip has, in every bar it can sit in, through the
# simulator's hook that holds the chip in one state (the mock radio leaves TX
# as soon as it enters it): shell.info reports the chip as drawn, and its
# text must get a whole line inside a chip that stays clear of the hairline.
chip_of() { "$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -oE '"chip":\{[^}]*\}'; }
chip_fits() { # <chip json> <bar height>
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    c = json.loads(sys.argv[1].split(":", 1)[1])
except (ValueError, IndexError):
    print(0)
    sys.exit()
bar = int(sys.argv[2])
print(1 if c["content_h"] >= c["line_h"] and c["y"] >= 0 and c["y"] + c["h"] <= bar - 2 else 0)
PY
}
n=0
bad=""
for state in rx tx off na; do
    for rot in portrait landscape; do
        fresh
        POCKETOS_TEST_RADIO_STATE=$state start_shell --rotation $rot --no-lock
        sleep 1.1
        c=$(chip_of)
        [ "$(chip_fits "$c" 56)" = 1 ] && n=$((n + 1)) || bad="$bad $state/$rot/home:$c"
        for id in system calendar; do
            "$POS" app start "$id" >/dev/null 2>&1; sleep 0.4
            bar=56
            [ "$rot" = landscape ] && bar=32
            c=$(chip_of)
            [ "$(chip_fits "$c" $bar)" = 1 ] && n=$((n + 1)) || bad="$bad $state/$rot/$id:$c"
            "$POS" app home >/dev/null 2>&1; sleep 0.3
        done
        stop_shell
    done
done
check "RX, TX, OFF and -- each get a whole line in the chip at home, in System and in Calendar, portrait and landscape ($n of 24)" \
    "$([ "$n" = 24 ] && echo 1 || echo 0)"
[ -n "$bad" ] && echo "$bad" | tr ' ' '\n' | head -4

check "no errors from any shell" "$(cat "$OUT"/*.log | grep -qE ' ERROR |Assert|assert' && echo 0 || echo 1)"

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR"
    cp "$OUT"/*.png "$SHOTS_DIR"/ 2>/dev/null
fi
rm -rf "$OUT"
echo "chrome_shell_test: $failed failure(s)"
exit $((failed > 0))
