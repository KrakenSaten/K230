#!/bin/bash
# The Doors mark as the shell compiles it (DS §19): ui/pocketui/pos_brand_mark.c
# must be tools/design/gen_brand_mark.py's output for the committed artwork,
# that artwork must be the mark's vector geometry, and the mark must be drawn
# only through its tint role and only in the one place §19.3 allows.
# tests/system_brand_shell_test.sh checks what the running shell draws.
set -u
cd "$(dirname "$0")/.." || exit 1
GEN=tools/design/gen_brand_mark.py
MARK_C=ui/pocketui/pos_brand_mark.c
ART=docs/design/brand/doors-threshold/brand/doors-mark.png
ART_SHA=5540077f5a0c14b1e56f629983fbbf4eacf55ad8da3d321944240d0ded432714
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "NOT RUN brand_mark_test: no python3, so the mark cannot be regenerated and compared."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 - continuing, but this gate did NOT pass."
        exit 0
    fi
    exit 77
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# ---- provenance ----------------------------------------------------------------
check "the mark artwork is the file the owner supplied (sha256 5540077f...)" \
    "$([ "$(sha256sum "${ART}" 2>/dev/null | cut -c1-64)" = "${ART_SHA}" ] && echo 1 || echo 0)"
python3 "${GEN}" "${ART}" "${TMP}/mark.c" >"${TMP}/gen.txt" 2>&1
check "the generator accepts the artwork" "$([ "$?" = "0" ] && echo 1 || echo 0)"
check "the committed pos_brand_mark.c is exactly the generator's output" \
    "$(cmp -s "${TMP}/mark.c" "${MARK_C}" && echo 1 || echo 0)"
python3 "${GEN}" "${ART}" "${TMP}/mark2.c" >/dev/null 2>&1
check "generating twice gives identical files" "$(cmp -s "${TMP}/mark.c" "${TMP}/mark2.c" && echo 1 || echo 0)"
check "generating did not touch the artwork" \
    "$([ "$(sha256sum "${ART}" | cut -c1-64)" = "${ART_SHA}" ] && echo 1 || echo 0)"

# ---- the mask is the mark ------------------------------------------------------
# Rasterise the package's vector path (brand/doors-mark.svg) at pixel centres,
# independently of the PNG, and compare with the bytes the shell compiles.
python3 - "${MARK_C}" <<'PY' >"${TMP}/geometry.txt" 2>&1
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
w = int(re.search(r"\.header\.w = (\d+)", src).group(1))
h = int(re.search(r"\.header\.h = (\d+)", src).group(1))
cf = re.search(r"\.header\.cf = (\w+)", src).group(1)
body = re.search(r"_map\[\][^{]*\{(.*?)\};", src, re.S).group(1)
data = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
# M0 24V0H20V24H10V20H16V4H4V24Z
poly = [(0, 24), (0, 0), (20, 0), (20, 24), (10, 24), (10, 20), (16, 20), (16, 4), (4, 4), (4, 24)]
def inside(px, py):
    hit = False
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        if (y1 > py) != (y2 > py) and px < x1 + (py - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit
want = [255 if inside(x + 0.5, y + 0.5) else 0 for y in range(h) for x in range(w)]
print("format", cf, "size", w, h, "bytes", len(data), "lit", sum(v == 255 for v in data),
      "partial", sum(0 < v < 255 for v in data))
sys.exit(0 if (cf, w, h) == ("LV_COLOR_FORMAT_A8", 20, 24) and data == want else 1)
PY
rc=$?
check "the compiled mask is A8, 20 x 24, and pixel-exact to the vector path" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
check "it lights 264 px with no partial alpha" \
    "$(grep -q 'lit 264 partial 0' "${TMP}/geometry.txt" && echo 1 || echo 0)"

# ---- refusals ------------------------------------------------------------------
refused() { # <label> <input> <message fragment>
    python3 "${GEN}" "$2" "${TMP}/refused.c" >"${TMP}/out.txt" 2>&1
    rc=$?
    check "$1 is refused" "$([ "${rc}" != "0" ] && [ ! -e "${TMP}/refused.c" ] && echo 1 || echo 0)"
    check "$1: the refusal says why" "$(grep -q -- "$3" "${TMP}/out.txt" && echo 1 || echo 0)"
}
# The package's own fixed-colour variant is the natural wrong input.
refused "the fixed-colour doors-mark-dark.png" docs/design/brand/doors-threshold/brand/doors-mark-dark.png "not white"
refused "an image with no alpha channel" docs/design/brand/doors-threshold/boot/doors-boot-568x1232.png "no alpha channel"
python3 - "${TMP}/clear.png" <<'PY'
import struct, sys, zlib
def chunk(k, b): return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xFFFFFFFF)
raw = b"".join(b"\x00" + b"\xff\xff\xff\x00" * 8 for _ in range(8))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
refused "a fully transparent image" "${TMP}/clear.png" "every pixel is transparent"

# ---- how it is drawn, and where ------------------------------------------------
sed -n '/styles\[POS_STYLE_BRAND_MARK\]/,/^}/p' ui/pocketui/pos_styles.c > "${TMP}/role.txt"
check "the tint role draws it in accent_primary at full opacity" \
    "$(grep -q 'lv_style_set_image_recolor(s, tok(POS_COLOR_ACCENT_PRIMARY))' "${TMP}/role.txt" &&
       grep -q 'lv_style_set_image_recolor_opa(s, LV_OPA_COVER)' "${TMP}/role.txt" && echo 1 || echo 0)"
users=$(grep -rl 'pos_brand_mark' apps ui --include='*.c' --include='*.h' | grep -v "^${MARK_C}$" | sort | tr '\n' ' ')
check "only the System app draws the mark (DS §19.3; found in: ${users})" \
    "$([ "${users}" = "apps/system/system_app.c " ] && echo 1 || echo 0)"
check "the System app draws it through POS_STYLE_BRAND_MARK" \
    "$(grep -q 'pos_style_add(mark, POS_STYLE_BRAND_MARK, 0)' apps/system/system_app.c &&
       grep -q 'lv_image_set_src(mark, &pos_brand_mark)' apps/system/system_app.c && echo 1 || echo 0)"
check "the mark is not animated (DS §19.4)" \
    "$(sed -n '/^static lv_obj_t \*identity_row/,/^}/p' apps/system/system_app.c | grep -q 'lv_anim' && echo 0 || echo 1)"
check "the shell build compiles the mask" \
    "$(grep -q 'pocketui/pos_brand_mark.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

echo "brand_mark_test: $failed failure(s)"
[ "$failed" -eq 0 ]
