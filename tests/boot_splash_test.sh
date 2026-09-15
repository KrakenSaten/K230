#!/bin/bash
# The boot splash converter, tools/design/png2xrgb.py.
#
# U-Boot shows /logo.xrgb only when it is exactly 568 x 1232 x 4 bytes, and
# shows it in whatever byte order it is given. A wrong size fails silently on
# the bench (no splash), and a wrong order or a flipped row order shows the
# wrong picture, so both are checked here with inputs whose every pixel says
# where it belongs. The expected output is built from the same pixel function,
# never by reading a PNG back, so the reader cannot vouch for itself.
set -u
cd "$(dirname "$0")/.." || exit 1
TOOL=tools/design/png2xrgb.py
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "NOT RUN boot_splash_test: no python3, so the splash converter cannot be checked."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 - continuing, but this gate did NOT pass."
        exit 0
    fi
    exit 77
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# ---- inputs ------------------------------------------------------------------
# Pixel (x, y) is R = x & 255, G = y & 255, B = (x >> 8) | (y >> 8) << 4 | 0x80,
# so every byte of the output names its own position.
python3 - "${TMP}" <<'PY'
import os, random, struct, sys, zlib
out = sys.argv[1]
W, H = 568, 1232

def px(x, y):
    return (x & 255, y & 255, (x >> 8) | ((y >> 8) << 4) | 0x80)

_cache = {}
def row(w, y, bpp):
    """The RGB(A) bytes of row y, built by slices: per-pixel Python is too slow here."""
    key = (w, y >> 8)
    if key not in _cache:
        _cache[key] = (bytes(x & 255 for x in range(w)),
                       bytes(px(x, y)[2] for x in range(w)))
    reds, blues = _cache[key]
    line = bytearray(w * bpp)
    line[0::bpp] = reds
    line[1::bpp] = bytes([y & 255]) * w
    line[2::bpp] = blues
    if bpp == 4:
        line[3::4] = b"\xff" * w
    return line

def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

def encode_row(kind, line, prev, bpp):
    if kind == 0:
        return bytes(line)
    res = bytearray(len(line))
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if kind == 1: pred = a
        elif kind == 2: pred = b
        elif kind == 3: pred = (a + b) >> 1
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        res[i] = (line[i] - pred) & 255
    return bytes(res)

def png(name, w, h, ctype=2, depth=8, interlace=0, filters=(0,), hole=None, extra=()):
    bpp = 4 if ctype == 6 else 3
    raw = bytearray()
    prev = bytearray(w * bpp)
    for y in range(h):
        line = row(w, y, bpp)
        if hole and hole[1] == y:               # (x, y, alpha): one pixel that is not opaque
            line[hole[0] * 4 + 3] = hole[2]
        kind = filters[y % len(filters)]
        raw.append(kind)
        raw.extend(encode_row(kind, line, prev, bpp))
        prev = line
    body = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    for kind, data in extra:
        body += chunk(kind, data)
    body += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(os.path.join(out, name), "wb") as f:
        f.write(body)
    return body

# every PNG row filter, so the reader's reconstruction is exercised end to end
ok = png("ok_rgb.png", W, H, filters=(0, 1, 2, 3, 4))
png("ok_rgba.png", W, H, ctype=6)
png("srgb.png", W, H, extra=[(b"sRGB", b"\x00")])
png("narrow.png", W - 1, H)
png("short.png", W, H - 1)
png("landscape.png", H, W)
png("translucent.png", W, H, ctype=6, hole=(10, 20, 254))
png("iccp.png", W, H, extra=[(b"iCCP", b"x\x00\x00" + zlib.compress(b"profile"))])
png("gama.png", W, H, extra=[(b"gAMA", struct.pack(">I", 45455))])
# refused on the header alone, so their pixel data does not matter
def header_only(name, ctype, depth, interlace):
    body = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, depth, ctype, 0, 0, interlace))
    body += chunk(b"IDAT", zlib.compress(b"\x00")) + chunk(b"IEND", b"")
    open(os.path.join(out, name), "wb").write(body)
header_only("interlaced.png", 2, 8, 1)
header_only("deep.png", 2, 16, 0)
header_only("palette.png", 3, 8, 0)
header_only("grey.png", 0, 8, 0)
damaged = bytearray(ok); damaged[-1] ^= 0xFF          # last byte of the IEND CRC
open(os.path.join(out, "badcrc.png"), "wb").write(damaged)
open(os.path.join(out, "truncated.png"), "wb").write(ok[:len(ok) // 2])
open(os.path.join(out, "notpng.png"), "wb").write(b"GIF89a" + bytes(64))

expected = bytearray(W * H * 4)
for y in range(H):
    rgb = row(W, y, 3)
    line = bytearray(W * 4)
    line[0::4] = rgb[2::3]
    line[1::4] = rgb[1::3]
    line[2::4] = rgb[0::3]
    line[3::4] = b"\xff" * W
    expected[y * W * 4:(y + 1) * W * 4] = line
open(os.path.join(out, "expected.xrgb"), "wb").write(expected)
PY
check "test inputs were generated" "$([ -f "${TMP}/expected.xrgb" ] && echo 1 || echo 0)"

conv() { python3 "${TOOL}" "$@" >"${TMP}/out.txt" 2>&1; echo "$?"; }

# ---- the reader ----------------------------------------------------------------
# Smooth gradients rarely tie the Paeth predictor, so a reconstruction bug can
# pass the converter checks below. Seeded noise through every filter does not.
python3 - "${TMP}" <<'PY' >"${TMP}/reader.txt" 2>&1
import os, random, struct, sys, zlib
sys.dont_write_bytecode = True
sys.path.insert(0, "tools/design")
from pngstrict import read_png
rng = random.Random(230)
def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
bad = 0
for ctype, bpp in ((2, 3), (6, 4)):
    w, h = 61, 50
    pixels = bytes(rng.randrange(256) for _ in range(w * h * bpp))
    raw, prev = bytearray(), bytes(w * bpp)
    for y in range(h):
        line = pixels[y * w * bpp:(y + 1) * w * bpp]
        kind = y % 5
        raw.append(kind)
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = (0, a, b, (a + b) >> 1,
                    a if (pa <= pb and pa <= pc) else (b if pb <= pc else c))[kind]
            raw.append((line[i] - pred) & 255)
        prev = line
    path = os.path.join(sys.argv[1], "noise%d.png" % ctype)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))
    got = read_png(path).rgba
    want = pixels if bpp == 4 else b"".join(pixels[i:i + 3] + b"\xff" for i in range(0, len(pixels), 3))
    bad += got != want
sys.exit(bad)
PY
rc=$?
check "the reader reconstructs seeded noise through all five filters, RGB and RGBA" \
    "$([ "${rc}" = "0" ] && echo 1 || echo 0)"

# ---- the format ----------------------------------------------------------------
rc="$(conv "${TMP}/ok_rgb.png" "${TMP}/rgb.xrgb")"
check "an RGB 568 x 1232 source converts" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
check "the splash is exactly 2,799,104 bytes (568 x 1232 x 4)" \
    "$([ "$(stat -c %s "${TMP}/rgb.xrgb" 2>/dev/null)" = "2799104" ] && echo 1 || echo 0)"
check "every pixel is B, G, R, 0xFF in row order from the top, across all five PNG filters" \
    "$(cmp -s "${TMP}/rgb.xrgb" "${TMP}/expected.xrgb" && echo 1 || echo 0)"
# The same thing, said as the four corners, so a failure reads as a direction.
corner() { od -An -tx1 -j "$1" -N4 "${TMP}/rgb.xrgb" | tr -d ' \n'; }
check "top-left pixel (0,0) is bytes 80 00 00 ff" "$([ "$(corner 0)" = "800000ff" ] && echo 1 || echo 0)"
check "top-right pixel (567,0) is bytes 82 00 37 ff" \
    "$([ "$(corner $((567 * 4)))" = "820037ff" ] && echo 1 || echo 0)"
check "bottom-left pixel (0,1231) is bytes c0 cf 00 ff" \
    "$([ "$(corner $((1231 * 568 * 4)))" = "c0cf00ff" ] && echo 1 || echo 0)"
check "bottom-right pixel (567,1231) is bytes c2 cf 37 ff" \
    "$([ "$(corner $(((1231 * 568 + 567) * 4)))" = "c2cf37ff" ] && echo 1 || echo 0)"
rc="$(conv --check "${TMP}/rgb.xrgb")"
check "--check accepts it" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"

rc="$(conv "${TMP}/ok_rgba.png" "${TMP}/rgba.xrgb")"
check "a fully opaque RGBA source gives the same bytes as RGB" \
    "$([ "${rc}" = "0" ] && cmp -s "${TMP}/rgba.xrgb" "${TMP}/expected.xrgb" && echo 1 || echo 0)"
conv "${TMP}/ok_rgba.png" "${TMP}/rgba2.xrgb" >/dev/null
check "converting the same source twice gives identical bytes" \
    "$(cmp -s "${TMP}/rgba.xrgb" "${TMP}/rgba2.xrgb" && echo 1 || echo 0)"
rc="$(conv "${TMP}/srgb.png" "${TMP}/srgb.xrgb")"
check "an sRGB chunk is accepted and changes nothing" \
    "$([ "${rc}" = "0" ] && cmp -s "${TMP}/srgb.xrgb" "${TMP}/expected.xrgb" && echo 1 || echo 0)"
check "no temporary file is left behind" "$(ls "${TMP}"/*.tmp >/dev/null 2>&1 && echo 0 || echo 1)"

# ---- refusals: nothing is adjusted to fit ------------------------------------
refused() { # <label> <input> <message fragment>
    rm -f "${TMP}/refused.xrgb"
    rc="$(conv "${TMP}/$2" "${TMP}/refused.xrgb")"
    check "$1 is refused" "$([ "${rc}" != "0" ] && [ ! -e "${TMP}/refused.xrgb" ] && echo 1 || echo 0)"
    check "$1: the refusal says why" "$(grep -q -- "$3" "${TMP}/out.txt" && echo 1 || echo 0)"
}
refused "a source one pixel narrow" narrow.png "567 x 1232"
refused "a source one row short" short.png "568 x 1231"
refused "a landscape source" landscape.png "1232 x 568"
refused "a source with one translucent pixel" translucent.png "pixel (10, 20) is not opaque"
refused "an ICC-profiled source" iccp.png "iCCP"
refused "a gamma-tagged source" gama.png "gAMA"
refused "an interlaced source" interlaced.png "interlaced"
refused "a 16-bit source" deep.png "16-bit"
refused "a palette source" palette.png "palette"
refused "a greyscale source" grey.png "greyscale"
refused "a source with a bad chunk CRC" badcrc.png "CRC mismatch"
refused "a truncated source" truncated.png "truncated"
refused "a file that is not a PNG" notpng.png "not a PNG"

cp "${TMP}/ok_rgba.png" "${TMP}/same.xrgb"
before="$(sha256sum "${TMP}/same.xrgb" | cut -c1-64)"
rc="$(conv "${TMP}/same.xrgb" "${TMP}/same.xrgb")"
check "writing the output over its own source is refused" \
    "$([ "${rc}" != "0" ] && [ "$(sha256sum "${TMP}/same.xrgb" | cut -c1-64)" = "${before}" ] && echo 1 || echo 0)"
rc="$(conv "${TMP}/ok_rgba.png" "${TMP}/splash.png")"
check "an output that is not a .xrgb file is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"

# ---- --check refuses what U-Boot would skip ----------------------------------
head -c 2798848 "${TMP}/rgb.xrgb" > "${TMP}/short.xrgb"
rc="$(conv --check "${TMP}/short.xrgb")"
check "--check refuses 2,798,848 bytes (64 pixels short)" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
{ cat "${TMP}/rgb.xrgb"; printf '\377\377\377\377'; } > "${TMP}/long.xrgb"
rc="$(conv --check "${TMP}/long.xrgb")"
check "--check refuses one pixel too many" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
cp "${TMP}/rgb.xrgb" "${TMP}/pad.xrgb"
printf '\000' | dd of="${TMP}/pad.xrgb" bs=1 seek=$(((20 * 568 + 10) * 4 + 3)) conv=notrunc status=none
rc="$(conv --check "${TMP}/pad.xrgb")"
check "--check refuses an X byte that is not 0xFF, and names the pixel" \
    "$([ "${rc}" != "0" ] && grep -q 'pixel (10, 20)' "${TMP}/out.txt" && echo 1 || echo 0)"
rc="$(conv --check "${TMP}/missing.xrgb")"
check "--check refuses a missing file" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"

# ---- the splash the image ships ------------------------------------------------
# apply_to_sdk.sh merges platforms/k230/rootfs_overlay into the vendor overlay,
# and the vendor post-image.sh copies logo.xrgb from there to the boot
# partition. It must pass --check, and it must be exactly what the tool makes
# of the committed artwork, so nobody can replace one without the other.
SPLASH=platforms/k230/rootfs_overlay/logo.xrgb
ART=docs/design/brand/doors-threshold/boot/doors-boot-568x1232.png
ART_SHA=e98aacc22f8e9fd8ab18dc20bd8219021ccfd7bba6f7851fadfdb4ff537a1690
SPLASH_SHA=434f4a6cf697544764ccbd111d4b3a60eaf5ced5f86e41be731f1f7ce4f58f94
check "the boot artwork is the file the owner supplied (sha256 e98aacc2...)" \
    "$([ "$(sha256sum "${ART}" 2>/dev/null | cut -c1-64)" = "${ART_SHA}" ] && echo 1 || echo 0)"
rc="$(conv --check "${SPLASH}")"
check "the shipped splash passes --check" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
rc="$(conv "${ART}" "${TMP}/regenerated.xrgb")"
check "the shipped splash is byte-for-byte the conversion of the committed artwork" \
    "$([ "${rc}" = "0" ] && cmp -s "${TMP}/regenerated.xrgb" "${SPLASH}" && echo 1 || echo 0)"
check "the shipped splash is the one documented (sha256 434f4a6c...)" \
    "$([ "$(sha256sum "${SPLASH}" 2>/dev/null | cut -c1-64)" = "${SPLASH_SHA}" ] && echo 1 || echo 0)"
check "converting did not touch the artwork" \
    "$([ "$(sha256sum "${ART}" 2>/dev/null | cut -c1-64)" = "${ART_SHA}" ] && echo 1 || echo 0)"

echo "boot_splash_test: $failed failure(s)"
[ "$failed" -eq 0 ]
