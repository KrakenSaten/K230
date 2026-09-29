#!/bin/bash
# The status chrome in the running shell (DS §30, §36, ui/shell/chrome.h).
#
#   1. The rules in the source: no full-width status bar and no wordmark;
#      one resolver, one content box, one cluster; the chrome resolved before
#      an app is created; every app header along the top edge, clear of the
#      corners and of the cluster; apps that declare and never manipulate;
#      the fullscreen apps declare NONE and no other app declares anything.
#   2. The pure test (tests/chrome_test, make test) when it has been built.
#   3. The running shell, both orientations, every app, from shell.info and
#      from the pixels: the content area from the top edge; the cluster in
#      the top-right corner, as wide as its content, inside the screen and
#      its corner margin, shown at home and under every app but the
#      fullscreen ones; every app header at the top edge with its body
#      straight under it (no stale bar padding), its title and hint clear of
#      the cluster; no full-width rule drawn; open, close and reopen over
#      IPC; the radio poll answering; the lock over a fullscreen app showing
#      the cluster and hiding it again once open; the simulator's hook.
#   4. The radio chip: its text drawn whole from a real radiod poll, and RX,
#      TX, OFF and -- each given a whole line in a chip inside the cluster,
#      at home and in apps, both orientations.
#
# Requires: SHELL_BIN (the CMake-built simulator) and pos and radiod (make
# all). Run from the repository root. SHOTS_DIR=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
RADIOD=${RADIOD:-services/radiod/radiod}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
# Zabbix opens on its fake server (tests/zabbix_shell_test.sh), so it is as
# quiet as every other app here: without a helper it warns that it has none.
export POCKETOS_ZABBIX_HELPER=$(pwd)/tools/zabbix/pos-zabbix POCKETOS_ZABBIX_BACKEND=fake POCKETOS_ZABBIX_FAKE=demo
# Browser opens on its start page and starts no helper until a page is asked for.
export POCKETOS_BROWSER_HELPER=$(pwd)/tools/browser/pos-browser POCKETOS_BROWSER_BACKEND=fake
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
line() { grep -n "$1" "$2" | head -1 | cut -d: -f1; }
APPS="radio system fleet radar timber notes clock calendar calculator settings wave rift files camera recorder zabbix browser
      solitaire blackjack 2048 deskbuddy mp3"
# DS §30.8, §34, §35: the apps that declare NONE, fullscreen in both orientations.
FULLSCREEN="rift notes wave fleet radar timber camera recorder zabbix browser solitaire blackjack 2048 deskbuddy mp3"
is_fullscreen() { case " $FULLSCREEN " in *" $1 "*) return 0 ;; esac; return 1; }

# ---- 1. the rules in the source ------------------------------------------------
check "no full-width status bar is left: no bar height anywhere, no bar object in the shell" \
    "$(grep -rqE 'POCKETUI_STATUS_BAR_H|POCKETOS_CHROME_(FULL|COMPACT)' ui apps tests --include='*.c' --include='*.h' ||
       grep -qE 'status_bar|POS_STYLE_(STATUS|ENV)_BAR' ui/shell/shell.c ui/pocketui/pos_styles.h && echo 0 || echo 1)"
check "no DOORS wordmark in the chrome" \
    "$(grep -q '"DOORS"' ui/shell/shell.c && echo 0 || echo 1)"
check "one status cluster, anchored to the top-right corner" \
    "$([ "$(grep -c 'lv_obj_create(screen)' ui/shell/shell.c)" -ge 1 ] &&
       grep -q 'lv_obj_align(c, LV_ALIGN_TOP_RIGHT' ui/shell/shell.c &&
       grep -q 'lv_obj_set_size(c, LV_SIZE_CONTENT, POCKETOS_CHROME_CLUSTER_H)' ui/shell/shell.c && echo 1 || echo 0)"
check "the cluster takes no touch" \
    "$(sed -n '/^static void status_cluster_create/,/^}/p' ui/shell/shell.c | grep -q 'lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE)' && echo 1 || echo 0)"
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
check "the launcher is laid out after the launcher's own chrome, clear of the cluster" \
    "$(grep -A6 'chrome_apply(chrome_resolve(POCKETOS_CHROME_DEFAULT, is_landscape(sh.display.geometry.rotation), true),' ui/shell/shell.c |
       grep -q 'home_build();' &&
       grep -q 'in.height = lv_obj_get_height(parent);' ui/shell/home.c &&
       grep -q 'home_create(sh.content, apps, APP_COUNT, sh.landscape, &home_keepout' ui/shell/shell.c &&
       grep -q 'controls_create(sh.content, sh.landscape, &controls_keepout' ui/shell/shell.c && echo 1 || echo 0)"
check "the keyboard reserve goes through the same box" \
    "$([ "$(grep -c 'content_box(POS_KB_H)' ui/shell/shell.c)" = 1 ] &&
       [ "$(grep -c 'content_box(0)' ui/shell/shell.c)" = 1 ] && echo 1 || echo 0)"
check "NONE hides the cluster and deletes nothing of it" \
    "$(grep -q 'lv_obj_add_flag(sh.cluster, LV_OBJ_FLAG_HIDDEN)' ui/shell/shell.c &&
       ! grep -q 'lv_obj_delete(sh.cluster\|lv_obj_delete(sh.status' ui/shell/shell.c && echo 1 || echo 0)"
check "every app header takes the top edge's corner insets, whatever the chrome" \
    "$(sed -n '/^static void app_open/,/^}/p' ui/shell/shell.c | grep -B1 -A1 'pocketui_apply_bar_insets(header, POS_EDGE_TOP)' |
       grep -q 'if (sh.chrome' && echo 0 || echo 1)"
check "and stops short of the cluster by the one reserve (chrome_row_reserve)" \
    "$(sed -n '/^static void app_open/,/^}/p' ui/shell/shell.c | grep -q 'chrome_row_reserve(sh.chrome' && echo 1 || echo 0)"
check "every app header carries the hint" \
    "$(sed -n '/^static void app_open/,/^}/p' ui/shell/shell.c | grep -q 'sh.header_hint = pocketui_label(header, sh.status_hint' &&
       ! sed -n '/^static void app_open/,/^}/p' ui/shell/shell.c | grep -q 'if (sh.chrome == POCKETOS_CHROME_NONE)' && echo 1 || echo 0)"
hits=$(grep -rnE 'chrome_(apply|resolve|height|content_box|cluster_box|cluster_width|row_reserve)|POCKETOS_CHROME_[A-Z_]*_H\b|POCKETOS_CHROME_CLUSTER' apps --include='*.c' --include='*.h')
check "no app resolves, reads or touches the chrome" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rn '\.chrome = ' apps --include='*.c' | grep -v 'POCKETOS_CHROME_NONE')
declared=$(grep -rln '\.chrome = POCKETOS_CHROME_NONE' apps --include='*.c' | cut -d/ -f2 | sort | tr '\n' ' ')
check "the fifteen fullscreen apps declare NONE and no app declares anything else ($declared) (DS §30.8, §34, §35)" \
    "$([ -z "$hits" ] &&
       [ "$declared" = "2048 blackjack browser camera deskbuddy fleet mp3 notes radar recorder rift solitaire timber video vision wave zabbix " ] &&
       echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the test hook that forces a chrome is compiled out of the panel's build" \
    "$(sed -n '/POCKETOS_SHELL_TEST_HOOKS/,/#endif/p' ui/shell/shell.c | grep -q 'getenv("POCKETOS_TEST_CHROME")' &&
       [ "$(grep -c 'POCKETOS_TEST_CHROME' ui/shell/shell.c)" = 1 ] && echo 1 || echo 0)"
check "the shell, and every app test, is built with the one resolver" \
    "$([ "$(grep -c '^ *chrome\.c$' ui/shell/CMakeLists.txt)" -ge 10 ] && echo 1 || echo 0)"
hits=$(grep -lE 'define STATUS_H' tests/*_app_test.c tests/timber_input_test.c | while read -r f; do
           grep -A2 'define STATUS_H' "$f" | grep -q 'chrome_height(chrome_resolve(' || echo "$f"; done | tr '\n' ' ')
check "every app test builds its frame from the resolver (${hits:-all of them})" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- 2. the pure test ----------------------------------------------------------
if [ -x tests/chrome_test ]; then
    log=$(./tests/chrome_test 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL'
    check "the resolver, the content box, the cluster's box and the chip (tests/chrome_test)" \
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
info_json() { "$POS" shell info 2>/dev/null; }
chrome_of() { info_json | tr -d ' \t\n' | grep -oE '"policy":"[a-z]+"'; }
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

# frame <info.json> <want policy> <want shown 0|1> <label>: the geometry
# shell.info reports for the screen in front, held to DS §36 - one "ok"/"FAIL"
# line per rule.
frame() {
    python3 - "$@" <<'PY'
import json, sys
path, want, shown, label = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]
out = []
def check(what, ok):
    out.append(("ok   " if ok else "FAIL ") + "%s: %s" % (label, what))
try:
    info = json.load(open(path))
except ValueError:
    print("FAIL %s: shell.info did not answer" % label)
    sys.exit()
d, c = info["display"], info["chrome"]
W, H = d["width"], d["height"]
k = c["cluster"]
top = 50 if W > H else 30            # the top corners (DS §21.1)
check("policy %s (%s)" % (want, c["policy"]), c["policy"] == want)
check("the content area starts at the top edge and fills the display (y %d, %d of %d)"
      % (c["content_y"], c["content_h"], H), c["content_y"] == 0 and c["content_h"] == H)
check("the cluster is %s" % ("shown" if shown else "hidden"), k["shown"] == shown)
if k["shown"]:
    check("the cluster (x %d..%d, y %d..%d) is inside the screen and ends %d px from the right edge"
          % (k["x"], k["x"] + k["w"], k["y"], k["y"] + k["h"], W - k["x"] - k["w"]),
          k["x"] >= top and k["x"] + k["w"] == W - top and 0 < k["y"] and k["y"] + k["h"] <= 72)
    check("the cluster is as wide as its content, not the screen (%d of %d px)" % (k["w"], W),
          0 < k["w"] <= k["reserve_w"] and k["w"] < W // 3)
    check("the cluster lies inside its reserve (x %d.. , reserve from %d)" % (k["x"], k["reserve_x"]),
          k["x"] >= k["reserve_x"])
h = c.get("header")
if h and not h.get("present", True):
    # An app that draws its own top row in landscape (app.h `header`, DS
    # §37.2): the shell builds no header, and the body is the top row.
    check("no app header: the body starts at the top edge (body at %d, header %d px)"
          % (h["body_y"], h["h"]), h["h"] == 0 and h["body_y"] == 0)
elif h:
    check("the app header is the top row: y %d, %d px, body straight under it at %d (no bar padding)"
          % (h["y"], h["h"], h["body_y"]), h["y"] == 0 and h["h"] == 72 and h["body_y"] == 72)
    check("the header clears the top corners (padding %d, %d)" % (h["pad_left"], h["pad_right"]),
          h["pad_left"] >= top and h["pad_right"] >= top)
    if k["shown"] and want == "cluster":
        check("the header's title and hint end before the cluster (at %d, %d; cluster from %d)"
              % (h["content_x2"], h.get("hint_x2", -1), k["reserve_x"]),
              h["content_x2"] < k["reserve_x"] - 8 and h.get("hint_x2", -1) < k["reserve_x"])
print("\n".join(out))
PY
}
# rule <png>: the first row, in the top 80, that is drawn in the line token
# across 90 % of the width - the full-width bar's bottom hairline; -1: none.
# slab <png>: "<top y> <left x>" of the header's back slab (surface), looked
# for at x 56 in the top 140 rows and 20 px under its top.
pixels() {
    python3 - "$1" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
surf, line = hexrgb(tok["surface"]), hexrgb(tok["line"])
W, H, rows = read_png(sys.argv[1])
px = lambda x, y: rows[y][x][:3]
near = lambda p, c: all(abs(p[k] - c[k]) <= 3 for k in range(3))
rule = next((y for y in range(0, 80) if sum(1 for x in range(0, W, 4) if near(px(x, y), line)) >= (W // 4) * 9 // 10), -1)
slab = next((y for y in range(0, 140) if near(px(56, y), surf)), -1)
left = next((x for x in range(0, 120) if slab >= 0 and near(px(x, slab + 20), surf)), -1)
print(rule, slab, left)
PY
}

for rot in portrait landscape; do
    r=${rot:0:1}
    fresh
    shot "$OUT/$r-home.png" "$OUT/$r-home.log" --rotation $rot --no-lock
    check "$rot, home: the cluster, content from the top edge" \
        "$(logs "$OUT/$r-home.log" | grep -q 'chrome: cluster, content from y 0, cluster shown, for home' && echo 1 || echo 0)"
    set -- $(pixels "$OUT/$r-home.png")
    check "$rot, home: no full-width rule drawn (got $1)" "$([ "$1" = -1 ] && echo 1 || echo 0)"
    n=0
    for id in $APPS; do
        want="chrome: cluster, content from y 0, cluster shown, for $id"
        is_fullscreen "$id" && want="chrome: none, content from y 0, cluster hidden, for $id"
        fresh
        shot "$OUT/$r-$id.png" "$OUT/$r-$id.log" --rotation $rot --open "$id"
        logs "$OUT/$r-$id.log" | grep -q "$want" &&
            ! logs "$OUT/$r-$id.log" | grep -qE ' ERROR |assert' && n=$((n + 1))
    done
    check "$rot: the seven other apps open under the cluster and the fourteen fullscreen ones under NONE, faulting nothing ($n of 21)" \
        "$([ "$n" = 21 ] && echo 1 || echo 0)"
    corner=30; [ $rot = landscape ] && corner=50
    for id in system timber; do
        set -- $(pixels "$OUT/$r-$id.png")
        check "$rot $id: no full-width rule, the back slab from row 8 at x $corner, clear of the corner (got $1 $2 $3)" \
            "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = $corner ] && echo 1 || echo 0)"
    done
    hits=$(for id in $APPS; do logs "$OUT/$r-$id.log" | grep -hE ' WARN |\[Warn\]' | grep -v 'radiod unavailable'; done)
    check "$rot: no warning from any app but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done

# Over IPC, both orientations: home, then every app opened, measured from
# shell.info, and home again - the chrome follows every transition.
for rot in portrait landscape; do
    fresh
    start_shell --rotation $rot --no-lock
    info_json >"$OUT/i-$rot-home.json"
    frame "$OUT/i-$rot-home.json" cluster 1 "$rot home" >"$OUT/f-$rot-home.checks"
    python3 - "$OUT/i-$rot-home.json" >>"$OUT/f-$rot-home.checks" <<'PY'
import json, sys
i = json.load(open(sys.argv[1]))
k, cells = i["chrome"]["cluster"], i["launcher"]["cells"]
t, d = i["launcher"].get("time"), i["launcher"].get("date")
W = i["display"]["width"]
clear = lambda a: a["x"] + a["w"] <= k["x"] or a["y"] >= k["y"] + k["h"] or a["y"] + a["h"] <= k["y"]
centred = lambda a: abs(a["x"] - (W - a["x"] - a["w"])) <= 1
print(("ok   " if not k["clock"] else "FAIL ") + "home: the cluster holds no clock (the launcher shows the time large)")
print(("ok   " if t and d and clear(t) and clear(d) else "FAIL ")
      + "home: the time and the date keep clear of the cluster (%s, %s)" % (t, d))
print(("ok   " if all(clear(c) for c in cells) else "FAIL ") + "home: no launcher cell lies under the cluster")
print(("ok   " if t and d and centred(t) and centred(d) else "FAIL ") + "home: the time and the date stay centred")
PY
    n=0
    for id in $APPS; do
        want=cluster; shown=1
        is_fullscreen "$id" && want=none && shown=0
        "$POS" app start "$id" >/dev/null 2>&1; sleep 0.4
        info_json >"$OUT/i-$rot-$id.json"
        frame "$OUT/i-$rot-$id.json" $want $shown "$rot $id" >"$OUT/f-$rot-$id.checks"
        "$POS" app home >/dev/null 2>&1; sleep 0.3
        [ "$(chrome_of)" = '"policy":"cluster"' ] && n=$((n + 1))
    done
    grep -h '^FAIL' "$OUT"/f-$rot-*.checks
    total=$(cat "$OUT"/f-$rot-*.checks | wc -l)
    bad=$(grep -h '^FAIL' "$OUT"/f-$rot-*.checks | wc -l)
    check "$rot: home and all twenty apps measured from shell.info: $((total - bad)) of $total rules hold" \
        "$([ "$bad" = 0 ] && [ "$total" -ge 100 ] && echo 1 || echo 0)"
    check "$rot: coming home from each restores the launcher's cluster ($n of 21)" "$([ "$n" = 21 ] && echo 1 || echo 0)"
    stop_shell
    check "$rot: the running shell logged no fault" \
        "$(grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log" && echo 0 || echo 1)"
done

# Open, close and reopen over IPC; the log says what each transition applied.
fresh
start_shell --rotation landscape --no-lock
cycles=0
for id in system $FULLSCREEN $FULLSCREEN; do
    want='"policy":"cluster"'
    is_fullscreen "$id" && want='"policy":"none"'
    "$POS" app start "$id" >/dev/null 2>&1; sleep 0.4
    a=$(chrome_of)
    "$POS" app home >/dev/null 2>&1; sleep 0.3
    b=$(chrome_of)
    [ "$a" = "$want" ] && [ "$b" = '"policy":"cluster"' ] && cycles=$((cycles + 1))
done
check "System, then each fullscreen app opened, closed and reopened: the cluster or NONE while open, the cluster at home, every time ($cycles of 29)" \
    "$([ "$cycles" = 29 ] && echo 1 || echo 0)"
check "each opening and each return logged its chrome" \
    "$([ "$(grep -c 'chrome: cluster, content from y 0, cluster shown, for system' "$POCKETOS_LOG_DIR/shell.log")" = 1 ] &&
       [ "$(grep -c 'chrome: none, content from y 0, cluster hidden' "$POCKETOS_LOG_DIR/shell.log")" = 28 ] &&
       [ "$(grep -c 'chrome: cluster, content from y 0, cluster shown, for home' "$POCKETOS_LOG_DIR/shell.log")" = 30 ] && echo 1 || echo 0)"
# The lock over a fullscreen app: the lock lies under the cluster, so while
# it is engaged the cluster comes back and the lock looks the same over
# either; opened again, the app is fullscreen as it was.
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
shown_of() { info_json | tr -d ' \t\n' | grep -oE '"shown":(true|false)'; }
"$POS" app start system >/dev/null 2>&1; sleep 0.4
call shell.lock; sleep 0.6
"$POS" shell screenshot "$OUT/l-lock-system.png" >/dev/null 2>&1
call shell.unlock; sleep 0.6
"$POS" app start notes >/dev/null 2>&1; sleep 0.4
call shell.lock; sleep 0.6
"$POS" shell screenshot "$OUT/l-lock-notes.png" >/dev/null 2>&1
check "locked over fullscreen Notes: the top band is drawn as over System" \
    "$(band_same "$OUT/l-lock-system.png" "$OUT/l-lock-notes.png" 72)"
check "and the chrome in force is still Notes' NONE, with the cluster shown over the lock ($(chrome_of) $(shown_of))" \
    "$([ "$(chrome_of)" = '"policy":"none"' ] && [ "$(shown_of)" = '"shown":true' ] && echo 1 || echo 0)"
call shell.unlock; sleep 0.6
check "unlocked: no cluster over Notes again ($(shown_of))" \
    "$([ "$(shown_of)" = '"shown":false' ] && echo 1 || echo 0)"
"$POS" shell screenshot "$OUT/l-unlocked-notes.png" >/dev/null 2>&1
set -- $(pixels "$OUT/l-unlocked-notes.png")
check "opened again: Notes is fullscreen, its back slab from row 8 (got $1 $2 $3)" \
    "$([ "$1" = -1 ] && [ "$2" = 8 ] && [ "$3" = 50 ] && echo 1 || echo 0)"
if [ -x "$RADIOD" ]; then
    "$POS" app start system >/dev/null 2>&1; sleep 0.4
    "$RADIOD" --backend mock >"$OUT/radiod.log" 2>&1 &
    RP=$!
    for _ in $(seq 1 40); do grep -q 'radiod is answering again' "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null && break; sleep 0.1; done
    check "radiod started while System is open under the cluster: the status poll sees it within the next ticks" \
        "$(grep -q 'radiod is answering again' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
    kill "$RP" 2>/dev/null; wait "$RP" 2>/dev/null
else
    check "radiod present for the poll check ($RADIOD)" 0
fi
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "the landscape shell logged no fault" \
    "$(grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log" && echo 0 || echo 1)"
stop_shell

# The simulator's hook: NONE forced on an app that shows the cluster, the
# cluster forced on a fullscreen one, and a value that is neither.
fresh
POCKETOS_TEST_CHROME=none shot "$OUT/l-none.png" "$OUT/l-none.log" --rotation landscape --open system
check "landscape, NONE forced on System: the shell says so, and home before it still had the cluster" \
    "$(logs "$OUT/l-none.log" | grep -q 'chrome: none, content from y 0, cluster hidden, for system' &&
       logs "$OUT/l-none.log" | grep -q 'chrome: cluster, content from y 0, cluster shown, for home' && echo 1 || echo 0)"
set -- $(pixels "$OUT/l-none.png")
check "landscape NONE: the back slab from row 8 at x 50, clear of the corner (got $1 $2 $3)" \
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
fresh
POCKETOS_TEST_CHROME=cluster shot "$OUT/p-cluster-fleet.png" "$OUT/p-cluster-fleet.log" --rotation portrait --open fleet
check "portrait, the cluster forced on fullscreen Fleet: honoured, its hint written to the header faults nothing" \
    "$(logs "$OUT/p-cluster-fleet.log" | grep -q 'chrome: cluster, content from y 0, cluster shown, for fleet' &&
       ! logs "$OUT/p-cluster-fleet.log" | grep -qE ' ERROR |assert' && echo 1 || echo 0)"
fresh
POCKETOS_TEST_CHROME=sideways shot "$OUT/l-bad.png" "$OUT/l-bad.log" --rotation landscape --open system
check "a hook value that is not a chrome is ignored with a warning, and the default applies" \
    "$(logs "$OUT/l-bad.log" | grep -q "test hook: chrome 'sideways'" &&
       logs "$OUT/l-bad.log" | grep -q 'chrome: cluster, content from y 0, cluster shown, for system' && echo 1 || echo 0)"

# ---- 4. the radio chip in the cluster -------------------------------------------
# The chip is a label, and a label clips what its content box cannot hold.
# Under the old 32 px bar a 24 px chip with 5 px of padding left 14 px for
# the 22 px line of the symbol font it draws in, and "RX" lost its top on
# unit A. The simulator never showed it: with no radiod the chip says "--",
# which sits mid-line. So the chip is drawn here with a radio that answers
# RX, and measured: its text as tall in landscape as in portrait.
chip_ink() { # <png>: rows of text inside the RX chip's fill, in the top 72 rows
    python3 - "$1" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"]["ice"]["modes"]["normal"]
fill = tuple(int(tok["radio_rx"][i:i + 2], 16) for i in (1, 3, 5))
W, H, rows = read_png(sys.argv[1])
near = lambda p, c, d: all(abs(p[k] - c[k]) <= d for k in range(3))
pts = [(x, y) for y in range(0, 72) for x in range(W // 2, W) if near(rows[y][x], fill, 3)]
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
    shot "$OUT/chip-portrait.png" "$OUT/chip-portrait.log" --rotation portrait --open system
    shot "$OUT/chip-landscape.png" "$OUT/chip-landscape.log" --rotation landscape --open system
    kill "$RP" 2>/dev/null; wait "$RP" 2>/dev/null
    p=$(chip_ink "$OUT/chip-portrait.png")
    l=$(chip_ink "$OUT/chip-landscape.png")
    check "the RX chip is drawn with its text in the cluster, from a real radiod poll ($p rows of ink)" \
        "$([ "$p" -ge 10 ] && echo 1 || echo 0)"
    check "and its text is the same in landscape: as many rows of ink ($l of $p)" \
        "$([ "$l" = "$p" ] && echo 1 || echo 0)"
else
    check "radiod present for the chip check ($RADIOD)" 0
fi
# Every state the chip has, at home and in apps, both orientations, through
# the simulator's hook that holds the chip in one state (the mock radio
# leaves TX as soon as it enters it): shell.info reports the chip as drawn,
# and its text must get a whole line inside a chip that stays inside the
# cluster's border.
chip_of() { info_json | tr -d ' \t\n' | grep -oE '"chip":\{[^}]*\}'; }
chip_fits() { # <chip json>
    python3 - "$1" <<'PY'
import json, sys
try:
    c = json.loads(sys.argv[1].split(":", 1)[1])
except (ValueError, IndexError):
    print(0)
    sys.exit()
print(1 if c["content_h"] >= c["line_h"] and c["y"] >= 1 and c["y"] + c["h"] <= 44 - 1 else 0)
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
        [ "$(chip_fits "$c")" = 1 ] && n=$((n + 1)) || bad="$bad $state/$rot/home:$c"
        for id in system calendar; do
            "$POS" app start "$id" >/dev/null 2>&1; sleep 0.4
            c=$(chip_of)
            [ "$(chip_fits "$c")" = 1 ] && n=$((n + 1)) || bad="$bad $state/$rot/$id:$c"
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
