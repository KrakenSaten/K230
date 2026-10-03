#!/bin/bash
# The Doors mark in the System app's identity row (DS §19), in the running
# shell. Each screenshot is searched for the compiled 20 x 24 mask drawn in
# that theme's accent_primary over its bg (both read from themes.json), so a
# mark in the wrong colour, the wrong place or at the wrong size is not found:
#
#   - every theme in every display mode, as selected at start;
#   - one theme in every display mode in landscape, where the identity row is
#     the first panel of System's ABOUT page, its left column (DS §52.5);
#   - a live theme change, which must repaint it without a rebuild (DS §8);
#   - reduced motion, which must change nothing about it (DS §19.4).
#
# Where it is found, its geometry is checked against §19.3: left edge on the
# panel's content edge (screen 20 + hairline + panel 20), 4 px clear space
# around it, the 8 px gap empty, and the name starting right after the gap.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) and pos (make all). Run
# from the repository root. No sysd is started, so the System screen is the
# same short one on every host. The identity row is on the ABOUT tab (DS
# §52.5), which the simulator's POCKETOS_TEST_SYSTEM_TAB opens System on.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
export SDL_VIDEODRIVER=dummy
export POCKETOS_TEST_SYSTEM_TAB=about
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# find <png> <theme> <mode>: prints "x y" of the one mark found, or why not.
find_mark() {
    python3 - "$@" <<'PY'
import json, re, sys
sys.dont_write_bytecode = True
# lodepng writes a palette PNG when the frame has few enough colours, which the
# artwork reader in tools/design refuses by design; the Timber art reader takes it.
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path, theme, mode = sys.argv[1:4]
tokens = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"][theme]["modes"][mode]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
acc, bg = hexrgb(tokens["accent_primary"]), hexrgb(tokens["bg"])
src = open("ui/pocketui/pos_brand_mark.c", encoding="utf-8").read()
body = re.search(r"_map\[\][^{]*\{(.*?)\};", src, re.S).group(1)
mask = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
MW, MH = 20, 24
W, H, rows = read_png(path)
px = [c for r in rows for p in r for c in p]
near = lambda i, c: all(abs(px[i + k] - c[k]) <= 3 for k in range(3))
at = lambda x, y: (y * W + x) * 4
found = []
for y in range(H - MH):
    for x in range(W - MW):
        if not near(at(x, y), acc):
            continue
        if all(near(at(x + mx, y + my), acc if mask[my * MW + mx] else bg)
               for my in range(MH) for mx in range(MW)):
            found.append((x, y))
if len(found) != 1:
    print("found %d marks in %s/%s (accent %s)" % (len(found), theme, mode, tokens["accent_primary"]))
    sys.exit(1)
x, y = found[0]
problems = []
for yy in range(y - 4, y + MH + 4):                     # clear space, and the gap
    for xx in range(x - 4, x + MW + 8):
        inside = x <= xx < x + MW and y <= yy < y + MH
        if not inside and not near(at(xx, yy), bg):
            problems.append("pixel (%d, %d) beside the mark is not bg" % (xx, yy))
            break
    if problems:
        break
name = [xx for xx in range(x + MW + 8, x + MW + 8 + 60)
        if any(not near(at(xx, yy), bg) for yy in range(y - 4, y + MH + 4))]
if not name or name[0] > x + MW + 8 + 3:
    problems.append("the name does not start after the 8 px gap (first ink at %s)" % (name[0] if name else None))
if problems:
    print("; ".join(problems))
    sys.exit(1)
print(x, y)
PY
}

hairline() { [ "$1" = outdoor ] && echo 2 || echo 1; }

# ---- every theme, every mode ---------------------------------------------------
for theme in doors ice brass olive slate carbon; do
    for mode in normal outdoor night; do
        png="$OUT/system-$theme-$mode.png"
        "$SHELL_BIN" --open system --theme "$theme" --mode "$mode" --screenshot "$png" \
            --exit-after-ms 900 >"$OUT/$theme-$mode.log" 2>&1
        where=$(find_mark "$png" "$theme" "$mode" 2>&1)
        check "$theme/$mode: the mark is drawn once, in accent_primary, clear space and gap intact ($where)" \
            "$(printf '%s' "$where" | grep -qE '^[0-9]+ [0-9]+$' && echo 1 || echo 0)"
        edge=$((20 + $(hairline "$mode") + 20))
        check "$theme/$mode: it starts on the panel's content edge, x = $edge" \
            "$([ "${where%% *}" = "$edge" ] && echo 1 || echo 0)"
        [ "$theme/$mode" = ice/normal ] && ICE_AT=$where
        printf '%s %s %s\n' "$theme" "$mode" "$where" >> "$OUT/where.txt"
    done
done

# ---- in landscape ----------------------------------------------------------------
# The identity panel is ABOUT's left column there (DS 52.5): the same mark on
# that panel's content edge, the screen's 20, the hairline and the panel's 20.
for mode in normal outdoor night; do
    png="$OUT/system-landscape-ice-$mode.png"
    "$SHELL_BIN" --rotation landscape --open system --theme ice --mode "$mode" --screenshot "$png" \
        --exit-after-ms 900 >"$OUT/landscape-ice-$mode.log" 2>&1
    where=$(find_mark "$png" ice "$mode" 2>&1)
    check "landscape ice/$mode: the mark is drawn once, in accent_primary, clear space and gap intact ($where)" \
        "$(printf '%s' "$where" | grep -qE '^[0-9]+ [0-9]+$' && echo 1 || echo 0)"
    edge=$((20 + $(hairline "$mode") + 20))
    check "landscape ice/$mode: it starts on the left column's panel content edge, x = $edge" \
        "$([ "${where%% *}" = "$edge" ] && echo 1 || echo 0)"
done

# SHOTS_DIR=<dir>: keep the screenshots, and make one contact sheet of the
# identity row for review - columns ice, brass, olive, slate, carbon; rows
# normal, outdoor, night; each cell 300 x 80 px from the panel's left edge.
if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR"
    cp "$OUT"/system-*.png "$SHOTS_DIR"/
    python3 - "$OUT" "$SHOTS_DIR/system-mark-contact.png" <<'PY'
import struct, sys, zlib
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
out, sheet = sys.argv[1:3]
themes, modes = ["doors", "ice", "brass", "olive", "slate", "carbon"], ["normal", "outdoor", "night"]
at = {}
for line in open(out + "/where.txt"):
    t, m, x, y = line.split()
    at[(t, m)] = int(y)
CW, CH, GAP = 300, 80, 4
W, H = len(themes) * CW + (len(themes) + 1) * GAP, len(modes) * CH + (len(modes) + 1) * GAP
img = [[(48, 48, 48)] * W for _ in range(H)]
for c, t in enumerate(themes):
    for r, m in enumerate(modes):
        _, _, rows = read_png("%s/system-%s-%s.png" % (out, t, m))
        y0 = at[(t, m)] - 28
        for dy in range(CH):
            src = rows[y0 + dy]
            dst = img[GAP + r * (CH + GAP) + dy]
            for dx in range(CW):
                dst[GAP + c * (CW + GAP) + dx] = src[20 + dx][:3]
raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in img)
ch = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
open(sheet, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                        + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
PY
    check "the contact sheet was written to $SHOTS_DIR" \
        "$([ -s "$SHOTS_DIR/system-mark-contact.png" ] && echo 1 || echo 0)"
fi

# ---- a live theme change repaints it -------------------------------------------
"$SHELL_BIN" --open system --theme ice --mode normal >"$OUT/live.log" 2>&1 & SP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
if [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ]; then
    sleep 0.5
    "$POS" shell theme carbon night >/dev/null 2>&1
    sleep 0.5
    "$POS" shell screenshot "$OUT/live-carbon-night.png" >/dev/null 2>&1
    where=$(find_mark "$OUT/live-carbon-night.png" carbon night 2>&1)
    check "after a live switch to carbon/night the mark is in that accent ($where)" \
        "$(printf '%s' "$where" | grep -qE '^[0-9]+ [0-9]+$' && echo 1 || echo 0)"
else
    check "the shell came up for the live theme change" 0
fi
kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null

# ---- reduced motion changes nothing --------------------------------------------
printf 'reduced_motion=1\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
"$SHELL_BIN" --open system --theme ice --mode normal --screenshot "$OUT/reduced.png" \
    --exit-after-ms 900 >"$OUT/reduced.log" 2>&1
where=$(find_mark "$OUT/reduced.png" ice normal 2>&1)
check "with reduced motion the mark is drawn in the same place ($where, was ${ICE_AT:-none})" \
    "$([ -n "${ICE_AT:-}" ] && [ "$where" = "$ICE_AT" ] && echo 1 || echo 0)"

check "no errors from the shell" \
    "$(cat "$OUT"/*.log | grep -qE 'ERROR|Assert|assert' && echo 0 || echo 1)"

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR"
[ "$failed" -eq 0 ] && rm -rf "$OUT" || echo "screenshots kept in $OUT"
echo "system_brand_shell_test: $failed failure(s)"
exit $((failed > 0))
