#!/bin/bash
# The Doors app icons on the launcher (DS §20), in the running shell.
#
#   1. pocketui_tile_test: the icon-mask tile against the glyph tile it
#      replaces - same boxes, same click area, no focus group, same accent in
#      every theme and mode - under a real pointer device.
#   2. Every theme in every display mode: each screenshot is searched, at the
#      tile's icon origin, for each app's compiled 32 x 32 mask blended from
#      that theme's surface to its accent_primary (both from themes.json). A
#      wrong icon, colour, size or place is not found, and every one of the
#      eleven apps must have one: no tile may fall back to a glyph. The tile
#      grid and the labels are checked where the launcher always put them.
#   3. A live theme change repaints the icons, and they survive an app round
#      trip; every app still opens.
#   4. Reduced motion draws the identical launcher.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell; pocketui_tile_test is
# built beside it) and pos (make all). Run from the repository root.
# SHOTS_DIR=<dir> keeps the screenshots and writes two contact sheets.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# ---- 1. the tile, against the tile it replaces --------------------------------
BIN=${POCKETUI_TILE_TEST:-$(dirname "$SHELL_BIN")/pocketui_tile_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|pocketui_tile_test:'
    check "the icon-mask tile keeps the glyph tile's geometry, click area and accent" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    check "pocketui_tile_test binary present ($BIN)" 0
fi

# look <png> <theme> <mode> <label>: prints check lines for one launcher shot.
look() {
    python3 - "$@" <<'PY'
import json, re, sys
sys.dont_write_bytecode = True
# lodepng writes a palette PNG when the frame has few enough colours, which the
# artwork reader in tools/design refuses by design; the Timber art reader takes it.
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path, theme, mode, label = sys.argv[1:5]
tokens = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"][theme]["modes"][mode]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
acc, surf, bg = hexrgb(tokens["accent_primary"]), hexrgb(tokens["surface"]), hexrgb(tokens["bg"])
src = open("ui/pocketui/pos_app_icons.c", encoding="utf-8").read()
masks = {n: [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})",
                                             re.search(r"pos_app_icon_%s_map\[\][^{]*\{(.*?)\};" % n, src, re.S).group(1))]
         for n in re.findall(r"^const lv_image_dsc_t pos_app_icon_(\w+) = \{", src, re.M)}
# The launcher's order (ui/shell/shell.c apps[]) and the tiles' boxes: two
# 254 px columns from x 20 with a 20 px gutter, 150 px rows from y 76 (status
# bar 56 + padding 20) with a 20 px gap, the icon at the 12 px inset.
order = ["radio", "system", "fleet", "radar", "timber", "notes", "clock", "calendar", "calculator", "settings", "wave"]
W, H, rows = read_png(path)
px = lambda x, y: rows[y][x][:3]
near = lambda p, c, tol: all(abs(p[k] - c[k]) <= tol for k in range(3))
# A blended pixel may be a rounding step off the exact mix: at most 2 was
# measured over all fifteen pairs, and the nearest wrong mask is 99 away.
TOL = 4
blend = lambda a: tuple((surf[k] * (255 - a) + acc[k] * a + 127) // 255 for k in range(3))
def err(x0, y0, mask):
    return max(abs(px(x0 + i % 32, y0 + i // 32)[k] - blend(a)[k]) for i, a in enumerate(mask) for k in range(3))
out = []
def check(what, ok):
    out.append(("ok   " if ok else "FAIL ") + "%s %s/%s: %s" % (label, theme, mode, what))
for i, app in enumerate(order):
    tx, ty = 20 + (i % 2) * 274, 76 + (i // 2) * 170
    edges = [((tx, ty + 75), surf), ((tx + 253, ty + 75), surf), ((tx + 127, ty), surf), ((tx + 127, ty + 149), surf),
             ((tx - 1, ty + 75), bg), ((tx + 254, ty + 75), bg), ((tx + 127, ty - 1), bg), ((tx + 127, ty + 150), bg)]
    check("%s tile is 254 x 150 at (%d, %d)" % (app, tx, ty), all(near(px(x, y), c, 3) for (x, y), c in edges))
    ix, iy = tx + 12, ty + 12
    scores = {n: err(ix, iy, m) for n, m in masks.items()}
    best = min(scores, key=scores.get)
    if app not in masks:
        check("%s has an icon mask compiled in (no glyph fallback on the launcher)" % app, False)
        continue
    check("%s icon is its own mask in accent_primary at (%d, %d), max error %d"
          % (app, ix, iy, scores[app]), scores[app] <= TOL)
    check("%s icon matches no other app's mask (nearest other: %d)"
          % (app, min(v for n, v in scores.items() if n != app)),
          best == app and all(v > TOL for n, v in scores.items() if n != app))
    ring = [(x, y) for y in range(iy - 4, iy + 36) for x in range(ix - 4, ix + 36)
            if not (ix <= x < ix + 32 and iy <= y < iy + 32)]
    check("%s icon has clear tile around it" % app, all(near(px(x, y), surf, 3) for x, y in ring))
    blank = [(x, y) for y in range(ty + 52, ty + 104) for x in range(tx + 2, tx + 252)]
    check("%s tile is empty between icon and label" % app, all(near(px(x, y), surf, 3) for x, y in blank))
    ink = [x for y in range(ty + 104, ty + 146) for x in range(tx + 2, tx + 252) if not near(px(x, y), surf, 3)]
    check("%s label is drawn from the 12 px inset (first ink x %s)" % (app, min(ink) - tx if ink else None),
          bool(ink) and 12 <= min(ink) - tx <= 14)
below = [(x, y) for y in range(76 + 6 * 170 - 20 + 1, H, 4) for x in range(0, W, 4)]
check("nothing is drawn below the last row", all(near(px(x, y), bg, 3) for x, y in below))
print("\n".join(out))
PY
}

# ---- 2. every theme, every mode ------------------------------------------------
for theme in ice brass olive slate carbon; do
    for mode in normal outdoor night; do
        png="$OUT/launcher-$theme-$mode.png"
        "$SHELL_BIN" --theme "$theme" --mode "$mode" --screenshot "$png" \
            --exit-after-ms 900 >"$OUT/$theme-$mode.log" 2>&1
        look "$png" "$theme" "$mode" start >"$OUT/$theme-$mode.checks" 2>&1
        # Only the failures and one summary line per pair, or this is 800 lines.
        grep -v '^ok' "$OUT/$theme-$mode.checks"
        failed=$((failed + $(grep -vc '^ok' "$OUT/$theme-$mode.checks")))
        check "$theme/$mode: $(grep -c '^ok' "$OUT/$theme-$mode.checks") launcher checks passed" \
            "$([ "$(grep -c '^ok' "$OUT/$theme-$mode.checks")" = 67 ] && echo 1 || echo 0)"
    done
done

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR"
    cp "$OUT"/launcher-*.png "$SHOTS_DIR"/
    python3 - "$OUT" "$SHOTS_DIR" <<'PY'
import struct, sys, zlib
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
out, dst = sys.argv[1:3]
themes, modes = ["ice", "brass", "olive", "slate", "carbon"], ["normal", "outdoor", "night"]
def write(path, img):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in img)
    ch = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
shots = {(t, m): read_png("%s/launcher-%s-%s.png" % (out, t, m))[2] for t in themes for m in modes}
GAP, GREY = 8, (48, 48, 48)
# Whole launchers at half size (2 x 2 average): columns themes, rows modes.
CW, CH = 284, 616
img = [[GREY] * (5 * CW + 6 * GAP) for _ in range(3 * CH + 4 * GAP)]
for c, t in enumerate(themes):
    for r, m in enumerate(modes):
        rows = shots[(t, m)]
        for y in range(CH):
            dst_row = img[GAP + r * (CH + GAP) + y]
            for x in range(CW):
                p = [rows[2 * y + j][2 * x + i] for j in (0, 1) for i in (0, 1)]
                dst_row[GAP + c * (CW + GAP) + x] = tuple(sum(q[k] for q in p) // 4 for k in range(3))
write(dst + "/launcher-contact.png", img)
# Every tile's icon cell, 48 x 48 at twice size: rows theme/mode pairs,
# columns the eleven apps in launcher order.
S, Z = 48, 2
img = [[GREY] * (11 * S * Z + 12 * GAP) for _ in range(15 * S * Z + 16 * GAP)]
for r, (t, m) in enumerate((t, m) for t in themes for m in modes):
    rows = shots[(t, m)]
    for i in range(11):
        x0, y0 = 20 + (i % 2) * 274 + 4, 76 + (i // 2) * 170 + 4
        for y in range(S * Z):
            dst_row = img[GAP + r * (S * Z + GAP) + y]
            for x in range(S * Z):
                dst_row[GAP + i * (S * Z + GAP) + x] = rows[y0 + y // Z][x0 + x // Z][:3]
write(dst + "/launcher-icons-contact.png", img)
PY
    check "the contact sheets were written to $SHOTS_DIR" \
        "$([ -s "$SHOTS_DIR/launcher-contact.png" ] && [ -s "$SHOTS_DIR/launcher-icons-contact.png" ] && echo 1 || echo 0)"
fi

# ---- 3. live: a theme change, an app round trip, every app opens ---------------
"$SHELL_BIN" --theme ice --mode normal >"$OUT/live.log" 2>&1 & SP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
if [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ]; then
    sleep 0.5
    "$POS" shell theme carbon night >/dev/null 2>&1
    sleep 0.5
    "$POS" shell screenshot "$OUT/live-carbon-night.png" >/dev/null 2>&1
    look "$OUT/live-carbon-night.png" carbon night "after a live switch" >"$OUT/live.checks" 2>&1
    grep -v '^ok' "$OUT/live.checks"; failed=$((failed + $(grep -vc '^ok' "$OUT/live.checks")))
    check "after a live switch to carbon/night every icon is in that accent ($(grep -c '^ok' "$OUT/live.checks") checks)" \
        "$([ "$(grep -c '^ok' "$OUT/live.checks")" = 67 ] && echo 1 || echo 0)"
    opened=0
    for id in radio system fleet radar timber notes clock calendar calculator settings wave; do
        "$POS" app start "$id" >/dev/null 2>&1 && sleep 0.4 &&
            "$POS" app list 2>/dev/null | grep -qE "^$id +.* open$" && opened=$((opened + 1))
        "$POS" app home >/dev/null 2>&1; sleep 0.2
    done
    check "every one of the eleven apps opens from the launcher's list and comes home ($opened)" \
        "$([ "$opened" = "11" ] && echo 1 || echo 0)"
    check "the shell logged each app opening" \
        "$([ "$(grep -cE 'open app (radio|system|fleet|radar|timber|notes|clock|calendar|calculator|settings|wave)$' "$OUT/live.log")" = "11" ] && echo 1 || echo 0)"
    "$POS" shell screenshot "$OUT/after-apps.png" >/dev/null 2>&1
    look "$OUT/after-apps.png" carbon night "after eleven apps" >"$OUT/after.checks" 2>&1
    grep -v '^ok' "$OUT/after.checks"; failed=$((failed + $(grep -vc '^ok' "$OUT/after.checks")))
    check "back home after all eleven, the launcher is drawn the same ($(grep -c '^ok' "$OUT/after.checks") checks)" \
        "$([ "$(grep -c '^ok' "$OUT/after.checks")" = 67 ] && echo 1 || echo 0)"
else
    check "the shell came up for the live checks" 0
fi
kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null

# ---- 4. reduced motion draws the same launcher ---------------------------------
printf 'reduced_motion=1\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
"$SHELL_BIN" --theme ice --mode normal --screenshot "$OUT/reduced.png" \
    --exit-after-ms 900 >"$OUT/reduced.log" 2>&1
same=$(python3 - "$OUT/launcher-ice-normal.png" "$OUT/reduced.png" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
a, b = (read_png(p)[2] for p in sys.argv[1:3])
# Below the status bar only: its clock reads the wall time.
print(1 if a[56:] == b[56:] else 0)
PY
)
check "with reduced motion the launcher below the status bar is pixel-identical" "${same:-0}"

check "no errors from the shell" \
    "$(cat "$OUT"/*.log | grep -qE ' ERROR |Assert|assert' && echo 0 || echo 1)"

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR"
[ "$failed" -eq 0 ] && rm -rf "$OUT" || echo "screenshots kept in $OUT"
echo "launcher_icons_shell_test: $failed failure(s)"
exit $((failed > 0))
