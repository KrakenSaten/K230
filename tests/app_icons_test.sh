#!/bin/bash
# The Doors app icons as the shell compiles them (DS §20):
# ui/pocketui/pos_app_icons.c must be tools/design/gen_app_icons.py's output
# for the committed artwork, each mask must be that artwork's alpha exactly,
# and the generator must refuse what is not a tint mask.
# tests/launcher_icons_shell_test.sh checks what the running shell draws.
set -u
cd "$(dirname "$0")/.." || exit 1
GEN=tools/design/gen_app_icons.py
ICONS_C=ui/pocketui/pos_app_icons.c
ART=docs/design/brand/doors-threshold/icons/png-32
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "NOT RUN app_icons_test: no python3, so the icons cannot be regenerated and compared."
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
# The ten files the owner supplied (docs/design/brand/README.md), by hash.
cat > "${TMP}/want.sha" <<'EOF'
3044610d013d949a00640fdce47da3486b5e27217b91d7ec45d6a1351517a9ab  calculator.png
6e82a0598cc3780d3210ab7659fc39d80940d8933a9810c29c9833967471a3e2  calendar.png
9be8cd73dbfc80e720513a098217d2da3a9066acdbc27fba879f02ca1823f983  clock.png
ea10791e675b96ec9dc9dce74636ff56afeb9a3555bfe3e4f98388842eef7aa8  fleet.png
76847f86b4a61d0d95965c77bcd203e527ddc3c3dc834c9eff63a3327792213f  notes.png
53563555f2a12f5050b24fea77d5e45c576570808988aca06f6654c23ceacc87  radar.png
5943609ff1ce85c7aee246c3420c09222d5b1cac6498cec419daf969a4c2cb0b  radio.png
54380c06c1d307effdd971efd23445c10fb7c4fc091150052d3a996e74a7850a  settings.png
9728382fc3a4b0c0c5347d3b8dd40b0bedaf4ef26cd583926b3126b5c440fb13  system.png
f7fd51b4ee745d2417fb14e4e08123b71bb1299d47503c2d838005a8c4ab2ad9  timber.png
EOF
(cd "${ART}" && sha256sum *.png) > "${TMP}/have.sha" 2>/dev/null
check "the icon artwork is exactly the ten files the owner supplied" \
    "$(cmp -s "${TMP}/want.sha" "${TMP}/have.sha" && echo 1 || echo 0)"
python3 "${GEN}" "${ART}" "${TMP}/icons.c" >"${TMP}/gen.txt" 2>&1
check "the generator accepts the artwork" "$([ "$?" = "0" ] && echo 1 || echo 0)"
check "the committed pos_app_icons.c is exactly the generator's output" \
    "$(cmp -s "${TMP}/icons.c" "${ICONS_C}" && echo 1 || echo 0)"
python3 "${GEN}" "${ART}" "${TMP}/icons2.c" >/dev/null 2>&1
check "generating twice gives identical files" "$(cmp -s "${TMP}/icons.c" "${TMP}/icons2.c" && echo 1 || echo 0)"
(cd "${ART}" && sha256sum *.png) > "${TMP}/after.sha" 2>/dev/null
check "generating did not touch the artwork" "$(cmp -s "${TMP}/want.sha" "${TMP}/after.sha" && echo 1 || echo 0)"

# ---- each mask is its artwork's alpha ------------------------------------------
# Decoded with the Timber art reader, not the generator's own, so a decoding
# fault in one is not hidden by the same fault in the other.
python3 - "${ICONS_C}" "${ART}" <<'PY' >"${TMP}/masks.txt" 2>&1
import re, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
src, art = sys.argv[1:3]
text = open(src, encoding="utf-8").read()
names = re.findall(r"^const lv_image_dsc_t pos_app_icon_(\w+) = \{", text, re.M)
bad = 0
for n in names:
    body = re.search(r"pos_app_icon_%s_map\[\][^{]*\{(.*?)\};" % n, text, re.S).group(1)
    data = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
    dsc = re.search(r"pos_app_icon_%s = \{(.*?)\};" % n, text, re.S).group(1)
    hdr = dict(re.findall(r"\.header\.(\w+) = (\w+),", dsc))
    W, H, rows = read_png("%s/%s.png" % (art, n))
    alpha = [p[3] for r in rows for p in r]
    ok = (hdr.get("cf"), hdr.get("w"), hdr.get("h"), hdr.get("stride")) == ("LV_COLOR_FORMAT_A8", "32", "32", "32") \
        and (W, H) == (32, 32) and data == alpha
    print("%s %s bytes %d lit %d partial %d" % ("ok" if ok else "BAD", n, len(data),
                                               sum(v == 255 for v in data), sum(0 < v < 255 for v in data)))
    bad += not ok
print("names", " ".join(names))
sys.exit(1 if bad else 0)
PY
rc=$?
check "every mask is A8, 32 x 32, and equals its PNG's alpha byte for byte" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
check "there is one mask per supplied icon, named by app id" \
    "$(grep -qx 'names calculator calendar clock fleet notes radar radio settings system timber' "${TMP}/masks.txt" && echo 1 || echo 0)"
check "the file holds 10,240 bytes of mask data and nothing else of size" \
    "$([ "$(grep -o '0x[0-9a-f][0-9a-f]' "${ICONS_C}" | wc -l)" = "10240" ] && echo 1 || echo 0)"

# ---- refusals ------------------------------------------------------------------
refused() { # <label> <input dir> <message fragment>
    rm -f "${TMP}/refused.c"
    python3 "${GEN}" "$2" "${TMP}/refused.c" >"${TMP}/out.txt" 2>&1
    rc=$?
    check "$1 is refused" "$([ "${rc}" != "0" ] && [ ! -e "${TMP}/refused.c" ] && echo 1 || echo 0)"
    check "$1: the refusal says why" "$(grep -q -- "$3" "${TMP}/out.txt" && echo 1 || echo 0)"
}
mkdir -p "${TMP}/coloured" "${TMP}/opaque" "${TMP}/mixed" "${TMP}/empty" "${TMP}/badname"
# The package's fixed-colour brand variant is a natural wrong input: 32 x 32, not white.
cp docs/design/brand/doors-threshold/brand/doors-mark-dark.png "${TMP}/coloured/radio.png"
refused "an icon in a fixed colour" "${TMP}/coloured" "not white"
cp docs/design/brand/doors-threshold/boot/doors-boot-568x1232.png "${TMP}/opaque/radio.png"
refused "an icon with no alpha channel" "${TMP}/opaque" "no alpha channel"
cp "${ART}/radio.png" "${TMP}/mixed/radio.png"
cp docs/design/brand/doors-threshold/icons/png-24/system.png "${TMP}/mixed/system.png"
refused "a set mixing 32 px and 24 px icons" "${TMP}/mixed" "same square size"
refused "a folder with no icons" "${TMP}/empty" "no PNG files"
cp "${ART}/radio.png" "${TMP}/badname/Radio-2.png"
refused "an icon whose file name is not an app id" "${TMP}/badname" "not an app id"
python3 - "${TMP}/clear.png" <<'PY'
import struct, sys, zlib
def chunk(k, b): return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xFFFFFFFF)
raw = b"".join(b"\x00" + b"\xff\xff\xff\x00" * 32 for _ in range(32))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 6, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
mkdir -p "${TMP}/clear" && mv "${TMP}/clear.png" "${TMP}/clear/radio.png"
refused "a fully transparent icon" "${TMP}/clear" "every pixel is transparent"

echo "app_icons_test: $failed failure(s)"
[ "$failed" -eq 0 ]
