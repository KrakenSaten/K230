#!/bin/bash
# Display geometry in the running shell: the safe area, and system-owned
# rotation (DS §21, ui/shell/shell_display.h).
#
#   1. display_touch_test: display and touch rotated together through LVGL's
#      real evdev driver; and the source rules that keep them one decision.
#   2. Portrait and landscape, every theme and display mode: no status bar and
#      no wordmark along the top edge (DS §36); the status cluster in the
#      top-right corner, inside the safe area and drawn whole (pixel-identical
#      to a panel with no rounded corners, only moved), and the launcher under
#      it drawn and clear of the bottom corners.
#   3. The policy in the running shell: stored Portrait and Landscape, Automatic
#      with a keyboard present, absent and unknown, an invalid stored value,
#      the bench override, a mode stored over IPC applying at the next start,
#      a theme change not touching the orientation, reduced motion, and all
#      twelve apps in landscape.
#
# Requires: SHELL_BIN (the CMake-built simulator; display_touch_test beside
# it) and pos (make all). Run from the repository root. SHOTS_DIR=<dir> keeps
# the screenshots and writes a contact sheet.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

fresh() { # a new set of state directories
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
shot() { # <png> <log> [shell args]
    local png=$1 log=$2
    shift 2
    "$SHELL_BIN" --no-lock --screenshot "$png" --exit-after-ms 900 "$@" >"$log" 2>&1
}
# The launcher over a plain background: what is measured here is where the
# status bar puts its ink, and the home photograph (DS §31) would be ink
# everywhere. The art and its fallback are tests/doors_shell_test.sh's.
mkdir -p "$OUT/noart"
export POCKETOS_ART_DIR="$OUT/noart"
info() { # shell.info's display object, compact
    "$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -oE '"display":\{[^}]*\}'
}
start_shell() { # [args]: a running shell, waited for (open: the launcher is what is looked at)
    "$SHELL_BIN" --no-lock "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.3
}
stop_shell() {
    kill "$SP" 2>/dev/null
    wait "$SP" 2>/dev/null
}

# ---- 1. display and touch, and the rules that keep them together ---------------
BIN=${DISPLAY_TOUCH_TEST:-$(dirname "$SHELL_BIN")/display_touch_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1 | grep -vE 'is greater than|smaller than zero'); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|display_touch_test:'
    check "display and touch turn together through LVGL's evdev driver, a mismatch is caught, and a restarted shell's first touch lands" \
        "$(printf '%s\n' "$log" | grep -qE '^display_touch_test: [0-9]+ checks, 0 failure' && echo 1 || echo 0)"
else
    check "display_touch_test binary present ($BIN)" 0
fi
BIN=${POCKETUI_LAYOUT_TEST:-$(dirname "$SHELL_BIN")/pocketui_layout_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|pocketui_layout_test:'
    check "the shared layout guard compares every coordinate and every inset, and a degenerate frame changes nothing" \
        "$(printf '%s\n' "$log" | grep -qE '^pocketui_layout_test: [0-9]+ checks, 0 failure' && echo 1 || echo 0)"
else
    check "pocketui_layout_test binary present ($BIN)" 0
fi
# Every responsive app opens its layout pass with that one guard, none of them
# keeps a private copy of the comparison, and the corner clearance is worked
# out in exactly one place (DS §21.3, §22.2).
RESPONSIVE="apps/calculator/calc_app.c apps/notes/notes_app.c apps/settings/settings_app.c
            apps/system/system_app.c apps/clock/clock_app.c apps/calendar/cal_app.c
            apps/fleet/fleet_app.c apps/radar/radar_app.c apps/files/files_app.c
            apps/camera/camera_app.c apps/recorder/rec_app.c"
for f in $RESPONSIVE; do
    check "$(basename "$f") opens its layout pass with the shared guard" \
        "$(grep -q 'pocketui_layout_begin(&' "$f" && echo 1 || echo 0)"
done
hits=$(grep -rnE 'memcmp\(&(box|area|in|insets)' ui apps --include='*.c')
check "and no app keeps a private layout-guard comparison of its own" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rln 'pos_display_rect_insets(' ui apps --include='*.c' | grep -v '^ui/pocketui/pos_display.c$' | tr '\n' ' ')
check "one caller works the corner clearance out, and it is PocketUI ($hits)" \
    "$([ "$hits" = "ui/pocketui/pocketui.c " ] && echo 1 || echo 0)"

check "only shell_display.c reads POCKETOS_DRM_ROTATION" \
    "$([ "$(grep -rl 'POCKETOS_DRM_ROTATION"' ui apps --include='*.c' | tr '\n' ' ')" = "ui/shell/shell_display.c " ] && echo 1 || echo 0)"
check "the DRM backend rotates the plane from the geometry it is given" \
    "$(grep -q 'lv_linux_drm_set_rotation(disp, (int)geometry->rotation)' ui/shell/platform_drm.c && echo 1 || echo 0)"
check "and derives touch from the same geometry, in one place" \
    "$([ "$(grep -c 'pos_display_touch_config(' ui/shell/platform_drm.c)" = 1 ] &&
       grep -q 'pos_display_touch_config(active.rotation' ui/shell/platform_drm.c &&
       grep -q 'active = \*geometry;' ui/shell/platform_drm.c && echo 1 || echo 0)"
check "no touch swap or calibration is set anywhere else in the backend" \
    "$([ "$(grep -c 'lv_evdev_set_swap_axes\|lv_evdev_set_calibration' ui/shell/platform_drm.c)" = 2 ] && echo 1 || echo 0)"
hits=$(grep -rnE 'lv_display_set_rotation|lv_linux_drm_set_rotation|pos_display_geometry_init|orientation_resolve|kbd_presence_publish' apps 2>/dev/null)
check "no app rotates the display, computes a geometry or publishes keyboard presence" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rlE 'kbd_presence_publish|kbd_presence_observe' ui --include='*.c' | grep -v 'ui/shell/kbd_presence.c' | sort | tr '\n' ' ')
check "one provider publishes keyboard presence, and it is the keyboard driver ($hits)" \
    "$([ "$hits" = "ui/shell/shell_kbd.c " ] && echo 1 || echo 0)"
check "the shell probes the keyboard before it opens the display" \
    "$(grep -q 'shell_kbd_probe();' ui/shell/shell.c &&
       [ "$(grep -n 'shell_kbd_probe();' ui/shell/shell.c | cut -d: -f1)" -lt \
         "$(grep -n 'shell_display_resolve(rotation_arg' ui/shell/shell.c | cut -d: -f1)" ] && echo 1 || echo 0)"
check "the simulator's keyboard hooks are compiled out of the panel's build" \
    "$([ "$(grep -c 'POCKETOS_TEST_KEYBOARD' ui/shell/shell_kbd.c)" = 2 ] &&
       sed -n '/POCKETOS_SHELL_TEST_HOOKS/,/#else/p' ui/shell/shell_kbd.c | grep -q POCKETOS_TEST_KEYBOARD_FILE &&
       grep -rq 'POCKETOS_TEST_KEYBOARD' ui/shell/shell_display.c && echo 0 || echo 1)"

# look <png> <theme> <mode> <label> <corner>: status cluster and launcher checks.
look() {
    python3 - "$@" <<'PY'
import json, re, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path, theme, mode, label, corner = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
# The top corners, where the cluster is; the bottom ones are `corner`.
top = int(sys.argv[6]) if len(sys.argv) > 6 else corner
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"][theme]["modes"][mode]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
acc, surf, bg = hexrgb(tok["accent_primary"]), hexrgb(tok["surface"]), hexrgb(tok["bg"])
W, H, rows = read_png(path)
px = lambda x, y: rows[y][x][:3]
near = lambda p, c, t: all(abs(p[k] - c[k]) <= t for k in range(3))
out = []
def check(what, ok):
    out.append(("ok   " if ok else "FAIL ") + "%s %s/%s: %s" % (label, theme, mode, what))
landscape = W > H
src = open("ui/pocketui/pos_app_icons.c", encoding="utf-8").read()
masks = {n: [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})",
                                             re.search(r"pos_app_icon_%s_map\[\][^{]*\{(.*?)\};" % n, src, re.S).group(1))]
         for n in re.findall(r"^const lv_image_dsc_t pos_app_icon_(\w+) = \{", src, re.M)}
# DS §36: no status bar and no wordmark along the top edge. Over the plain
# background the top-left of the screen is empty: the launcher's time is
# centred and starts well right of it.
barbg = px(2, 3)
left = [(x, y) for y in range(0, 64) for x in range(0, 150) if not near(px(x, y), barbg, 12)]
check("no wordmark and no bar: nothing drawn in the top-left 150 x 64 (%d points)" % len(left), not left)
# The status cluster: ink in the top-right corner band, the glass capsule
# with the radio chip in it. The launcher shows its time large, so the
# cluster holds no clock here (DS §31.1).
ink = [(x, y) for y in range(0, 70) for x in range(W - 180, W) if not near(px(x, y), barbg, 12)]
xs = sorted(set(x for x, _ in ink))
ys = sorted(set(y for _, y in ink))
check("the status cluster is drawn in the top-right corner (%d points of ink)" % len(ink), len(ink) > 200)
if ink:
    check("the cluster (x %d..%d) ends inside the %d px safe margin, not at the edge" % (xs[0], xs[-1], top),
          xs[-1] <= W - 1 - top and xs[-1] >= W - 1 - top - 4)
    check("the cluster (y %d..%d) sits in the top row, clear of the %d px corner square" % (ys[0], ys[-1], top),
          ys[0] >= 10 and ys[-1] <= 62 and xs[-1] < W - top)
    check("the cluster is as wide as its content, not the screen (%d px)" % (xs[-1] - xs[0] + 1),
          xs[-1] - xs[0] + 1 < 160)
# The DOORS launcher under it. Its geometry is tests/home_layout_test.c's
# and its art tests/doors_shell_test.sh's; here, that it is drawn and keeps
# out of the bottom corners.
drawn = sum(1 for y in range(60, H, 8) for x in range(0, W, 8) if not near(px(x, y), bg, 6))
check("the launcher is drawn (%d sampled points of ink)" % drawn, drawn > 150)
cor = [(x, y) for y in range(H - corner, H) for x in list(range(0, corner)) + list(range(W - corner, W))]
check("nothing is drawn in the bottom corner squares", all(near(px(x, y), bg, 3) for x, y in cor))
print("\n".join(out))
PY
}

# same_but_cluster <a.png> <b.png> <dx>: b's status cluster is a's shifted left
# by dx, pixel for pixel, and the launcher under it is identical.
same_but_cluster() {
    python3 - "$@" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
a, b, dx = read_png(sys.argv[1])[2], read_png(sys.argv[2])[2], int(sys.argv[3])
W = len(a[0])
# The cluster's band: the 130 px left of the right margin the rectangular
# panel keeps (20 px), in the top 64 rows - the cluster and a little air,
# and not the launcher's time, which is centred and does not move.
cluster_same = all(a[y][x][:3] == b[y][x - dx][:3] for y in range(0, 64) for x in range(W - 150, W - 20))
# The launcher, less its header (DS §31.3): its time is the wall clock's,
# and the two shots compared are seconds apart.
below_same = a[124:] == b[124:] and all(a[y][:120] == b[y][:120] for y in range(0, 124))
print("%d %d" % (cluster_same, below_same))
PY
}

# ---- 2. both orientations, every theme and mode --------------------------------
fresh
for orient in portrait landscape; do
    # The corners in force: 30 px in portrait; in landscape the top ones are
    # 50 px (platform.h, DS §21.1), and the bottom ones 30.
    corner=30; [ "$orient" = landscape ] && corner=50
    for theme in doors ice brass olive slate carbon; do
        for mode in normal outdoor night; do
            png="$OUT/$orient-$theme-$mode.png"
            shot "$png" "$OUT/$orient-$theme-$mode.log" --rotation "$orient" --theme "$theme" --mode "$mode"
            look "$png" "$theme" "$mode" "$orient" 30 $corner >"$OUT/$orient-$theme-$mode.checks" 2>&1
            grep -v '^ok' "$OUT/$orient-$theme-$mode.checks"
            failed=$((failed + $(grep -vc '^ok' "$OUT/$orient-$theme-$mode.checks")))
            check "$orient $theme/$mode: $(grep -c '^ok' "$OUT/$orient-$theme-$mode.checks") status cluster and launcher checks passed" \
                "$([ "$(grep -c '^ok' "$OUT/$orient-$theme-$mode.checks")" = 7 ] && echo 1 || echo 0)"
        done
    done
    # A rectangular panel: no rounded corners, so the cluster keeps the
    # header's own 20 px margin (POCKETUI_PAD).
    POCKETOS_SAFE_CORNERS=0,0,0,0 shot "$OUT/$orient-rect.png" "$OUT/$orient-rect.log" --rotation "$orient" \
        --theme ice --mode normal
    look "$OUT/$orient-rect.png" ice normal "$orient rectangular" 20 20 >"$OUT/$orient-rect.checks" 2>&1
    last=$(grep -oE 'cluster \(x [0-9]+\.\.[0-9]+\) ends' "$OUT/$orient-rect.checks" | grep -oE '\.\.[0-9]+' | tr -d .)
    W=568; [ "$orient" = landscape ] && W=1232
    check "$orient, rectangular panel: the cluster keeps a 20 px margin (ink to x ${last:-none})" \
        "$([ -n "$last" ] && [ "$last" -le $((W - 21)) ] && [ "$last" -ge $((W - 25)) ] && echo 1 || echo 0)"
    set -- $(same_but_cluster "$OUT/$orient-rect.png" "$OUT/$orient-ice-normal.png" $((corner - 20)))
    check "$orient: with $corner px top corners the cluster is the same pixels moved $((corner - 20)) px left (drawn whole)" "${1:-0}"
    check "$orient: and the launcher under it is identical to the rectangular panel's" "${2:-0}"
done

# ---- 3. the policy in the running shell -----------------------------------------
policy() { # <label> <expect w>x<h> [env ...] -- [shell args]: a screenshot's size and shell.info
    local label=$1 want=$2
    shift 2
    fresh
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    env "${envs[@]}" "$SHELL_BIN" --screenshot "$OUT/policy.png" --exit-after-ms 900 "$@" >"$OUT/policy.log" 2>&1
    local got
    got=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(24); print('%dx%d' % struct.unpack('>II', d[16:24]))" "$OUT/policy.png" 2>/dev/null)
    check "$label: the display is $want ($got)" "$([ "$got" = "$want" ] && echo 1 || echo 0)"
}
settings_with() { mkdir -p "$POCKETOS_CONFIG_DIR"; printf '%s\n' "$@" > "$POCKETOS_CONFIG_DIR/settings.conf"; }

fresh; settings_with "display_rotation=landscape"
"$SHELL_BIN" --screenshot "$OUT/stored-landscape.png" --exit-after-ms 900 >"$OUT/stored-l.log" 2>&1
check "stored Landscape starts landscape" "$(grep -q 'rotation mode landscape (stored), keyboard unknown: rotation 270, 1232x568' "$OUT/stored-l.log" && echo 1 || echo 0)"
fresh; settings_with "display_rotation=portrait"
POCKETOS_TEST_KEYBOARD_PRESENCE=present "$SHELL_BIN" --exit-after-ms 600 >"$OUT/stored-p.log" 2>&1
check "stored Portrait stays portrait with a keyboard present" "$(grep -q 'rotation mode portrait (stored), keyboard present: rotation 0, 568x1232' "$OUT/stored-p.log" && echo 1 || echo 0)"
fresh; settings_with "display_rotation=landscape"
POCKETOS_TEST_KEYBOARD_PRESENCE=absent "$SHELL_BIN" --exit-after-ms 600 >"$OUT/stored-l2.log" 2>&1
check "stored Landscape stays landscape with the keyboard absent" "$(grep -q 'rotation mode landscape (stored), keyboard absent: rotation 270, 1232x568' "$OUT/stored-l2.log" && echo 1 || echo 0)"
for k in present absent unknown; do
    fresh
    POCKETOS_TEST_KEYBOARD_PRESENCE=$k "$SHELL_BIN" --exit-after-ms 600 >"$OUT/auto-$k.log" 2>&1
    want="rotation 0, 568x1232"; [ $k = present ] && want="rotation 270, 1232x568"
    check "Automatic (nothing stored) with the keyboard $k: $want" \
        "$(grep -q "rotation mode automatic (default), keyboard $k: $want" "$OUT/auto-$k.log" && echo 1 || echo 0)"
done
fresh; settings_with "display_rotation=sideways"
"$SHELL_BIN" --exit-after-ms 600 >"$OUT/invalid.log" 2>&1
check "an invalid stored mode is reported and falls back to Automatic, so Portrait" \
    "$(grep -q 'stored rotation mode "sideways" is not automatic, portrait or landscape; using automatic' "$OUT/invalid.log" &&
       grep -q 'rotation mode automatic (stored), keyboard unknown: rotation 0, 568x1232' "$OUT/invalid.log" && echo 1 || echo 0)"
check "and the stored value is left as it was" "$(grep -qx 'display_rotation=sideways' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
policy "the bench override POCKETOS_DRM_ROTATION=90" 1232x568 POCKETOS_DRM_ROTATION=90 --
check "and it is logged as an override of the policy" "$(grep -q 'rotation 90 from POCKETOS_DRM_ROTATION overrides the automatic mode' "$OUT/policy.log" && echo 1 || echo 0)"
policy "an invalid POCKETOS_DRM_ROTATION=45" 568x1232 POCKETOS_DRM_ROTATION=45 --
check "is ignored with a warning" "$(grep -q 'POCKETOS_DRM_ROTATION=45 is not 0, 90, 180 or 270; ignored' "$OUT/policy.log" && echo 1 || echo 0)"

# A mode stored while the shell runs is applied by the shell restarting itself
# in place (shell.c); the keyboard's side of the same mechanism is
# tests/auto_rotation_shell_test.sh.
fresh
start_shell
d=$(info)
check "a fresh shell reports portrait, Automatic, nothing pending ($d)" \
    "$(printf '%s' "$d" | grep -q '"width":568,"height":1232' && printf '%s' "$d" | grep -q '"rotation_mode":"automatic"' &&
       printf '%s' "$d" | grep -q '"applying":false' && echo 1 || echo 0)"
r=$("$POS" call shell shell.rotation mode=landscape 2>&1 | tr -d ' \t\n')
check "shell.rotation stores Landscape and says it is being applied ($r)" \
    "$(printf '%s' "$r" | grep -q '"rotation_mode":"landscape"' && printf '%s' "$r" | grep -q '"orientation":"portrait"' &&
       printf '%s' "$r" | grep -q '"next_orientation":"landscape"' && printf '%s' "$r" | grep -q '"applying":true' && echo 1 || echo 0)"
check "an invalid mode over IPC is refused" \
    "$("$POS" call shell shell.rotation mode=upside 2>&1 | grep -q 'mode must be automatic, portrait or landscape' && echo 1 || echo 0)"
check "the mode was persisted" "$(grep -qx 'display_rotation=landscape' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
# The settle window, then the shell opening the display again, in the same
# process: the pid is the point, because that is what pos-supervise watches.
for _ in $(seq 1 40); do info | grep -q '"width":1232' && break; sleep 0.2; done
d=$(info)
check "the shell applied it by opening the display again, landscape, nothing pending ($d)" \
    "$(printf '%s' "$d" | grep -q '"width":1232,"height":568' && printf '%s' "$d" | grep -q '"orientation":"landscape"' &&
       printf '%s' "$d" | grep -q '"applying":false' && echo 1 || echo 0)"
check "in the same process, so the supervisor saw no exit" \
    "$(kill -0 "$SP" 2>/dev/null && grep -q 'restarting in place' "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"
"$POS" shell theme carbon night >/dev/null 2>&1
sleep 0.4
d=$(info)
check "a theme and mode change keeps the orientation ($d)" \
    "$(printf '%s' "$d" | grep -q '"width":1232,"height":568' && printf '%s' "$d" | grep -q '"rotation":270' && echo 1 || echo 0)"
"$POS" shell screenshot "$OUT/landscape-after-theme.png" >/dev/null 2>&1
look "$OUT/landscape-after-theme.png" carbon night "landscape after a live theme change" 30 50 >"$OUT/after-theme.checks" 2>&1
grep -v '^ok' "$OUT/after-theme.checks"; failed=$((failed + $(grep -vc '^ok' "$OUT/after-theme.checks")))
check "and the landscape launcher is drawn in that theme ($(grep -c '^ok' "$OUT/after-theme.checks") checks)" \
    "$([ "$(grep -c '^ok' "$OUT/after-theme.checks")" = 7 ] && echo 1 || echo 0)"
opened=0
for id in radio system fleet radar timber notes clock calendar calculator settings wave files camera recorder; do
    "$POS" app start "$id" >/dev/null 2>&1 && sleep 0.4 &&
        "$POS" app list 2>/dev/null | grep -qE "^$id +.* open$" && opened=$((opened + 1))
    [ "$id" = settings ] && "$POS" shell screenshot "$OUT/landscape-settings.png" >/dev/null 2>&1
    "$POS" app home >/dev/null 2>&1; sleep 0.2
done
check "in landscape every one of the fourteen apps opens and comes home ($opened)" "$([ "$opened" = 14 ] && echo 1 || echo 0)"
check "the landscape shell logged no ERROR" "$(grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/run.log" && echo 0 || echo 1)"
stop_shell

fresh
shot "$OUT/landscape-plain.png" "$OUT/lp.log" --rotation landscape
settings_with "reduced_motion=1"
shot "$OUT/landscape-reduced.png" "$OUT/lr.log" --rotation landscape
same=$(python3 - "$OUT/landscape-plain.png" "$OUT/landscape-reduced.png" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
a, b = (read_png(p)[2] for p in sys.argv[1:3])
print(1 if a[56:] == b[56:] else 0)
PY
)
check "reduced motion draws the identical landscape launcher below the status bar" "${same:-0}"
fresh
shot "$OUT/portrait-settings.png" "$OUT/ps.log" --open settings

check "no errors from the shells" "$(cat "$OUT"/*.log | grep -qE ' ERROR |Assert|assert' && echo 0 || echo 1)"

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR"
    cp "$OUT"/portrait-ice-normal.png "$OUT"/landscape-ice-normal.png "$OUT"/portrait-settings.png \
       "$OUT"/landscape-settings.png "$SHOTS_DIR"/ 2>/dev/null
    python3 - "$OUT" "$SHOTS_DIR/display-geometry-contact.png" <<'PY'
import struct, sys, zlib
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
out, dst = sys.argv[1:3]
GREY, MARK = (48, 48, 48), (255, 0, 160)
def load(n):
    W, H, rows = read_png("%s/%s.png" % (out, n))
    return W, H, [[p[:3] for p in r] for r in rows]
def half(img):
    return [[tuple(sum(img[2 * y + j][2 * x + i][k] for j in (0, 1) for i in (0, 1)) // 4 for k in range(3))
             for x in range(len(img[0]) // 2)] for y in range(len(img) // 2)]
def zoom(img, x0, y0, w, h, z):
    return [[img[y0 + y // z][x0 + x // z] for x in range(w * z)] for y in range(h * z)]
def marked_bar(img, W, corner):
    bar = [row[:] for row in img[:56]]
    for y in range(56):
        for x in (corner, W - 1 - corner):
            bar[y][x] = MARK
    return bar
def paste(canvas, img, x0, y0):
    for y, row in enumerate(img):
        canvas[y0 + y][x0:x0 + len(row)] = row
pW, pH, p = load("portrait-ice-normal")
lW, lH, l = load("landscape-ice-normal")
_, _, ps = load("portrait-settings")
pb, lb = marked_bar(p, pW, 30), marked_bar(l, lW, 30)
GAP = 12
# Row 1: portrait launcher at half size | landscape launcher at half size,
#        with the portrait Settings > Display card below it.
# Row 2: the portrait status bar's two ends at 2x | the landscape bar's ends
#        at 2x; magenta lines are the 30 px safe-area bounds.
ph, lh = half(p), half(l)
display_card = ps[330:650]
ends = lambda bar, W: [zoom(bar, 0, 0, 140, 56, 2), zoom(bar, W - 140, 0, 140, 56, 2)]
pe, le = ends(pb, pW), ends(lb, lW)
width = GAP + len(ph[0]) + GAP + max(len(lh[0]), len(display_card[0])) + GAP
row1h = max(len(ph), len(lh) + GAP + len(display_card))
row2h = 112
height = GAP + row1h + GAP + row2h + GAP + row2h + GAP
canvas = [[GREY] * width for _ in range(height)]
paste(canvas, ph, GAP, GAP)
paste(canvas, lh, GAP + len(ph[0]) + GAP, GAP)
paste(canvas, display_card, GAP + len(ph[0]) + GAP, GAP + len(lh) + GAP)
y = GAP + row1h + GAP
paste(canvas, pe[0], GAP, y); paste(canvas, pe[1], GAP + 280 + GAP, y)
y += row2h + GAP
paste(canvas, le[0], GAP, y); paste(canvas, le[1], GAP + 280 + GAP, y)
raw = b"".join(b"\x00" + bytes(v for px in r for v in px) for r in canvas)
ch = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                      + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
PY
    check "the contact sheet was written to $SHOTS_DIR" "$([ -s "$SHOTS_DIR/display-geometry-contact.png" ] && echo 1 || echo 0)"
fi

[ "$failed" -eq 0 ] && rm -rf "$OUT" || echo "screenshots kept in $OUT"
echo "display_geometry_shell_test: $failed failure(s)"
exit $((failed > 0))
