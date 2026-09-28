#!/bin/bash
# The Doors app icons as the shell compiles them (DS §20):
# ui/pocketui/pos_app_icons.c must be tools/design/gen_app_icons.py's output
# for the committed artwork - one icon per launcher app, from the package that
# supplied it, and nothing else - each mask must be that artwork's alpha
# exactly, and the generator must refuse what is not a tint mask.
# tests/doors_shell_test.sh checks what the running shell draws (DS §31).
set -u
cd "$(dirname "$0")/.." || exit 1
GEN=tools/design/gen_app_icons.py
ICONS_C=ui/pocketui/pos_app_icons.c
ART=docs/design/brand/doors-threshold/icons/png-32
EXT=docs/design/brand/doors-icon-extension/png-32
FIRST=docs/design/doors-app-icons/png-32
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
# The fourteen launcher icons as the owner supplied them (docs/design/brand/
# README.md), by hash: ten from the Threshold package, Wave, Files, Camera and
# Recorder from the icon extension.
cat > "${TMP}/want.sha" <<'EOF'
3044610d013d949a00640fdce47da3486b5e27217b91d7ec45d6a1351517a9ab  docs/design/brand/doors-threshold/icons/png-32/calculator.png
6e82a0598cc3780d3210ab7659fc39d80940d8933a9810c29c9833967471a3e2  docs/design/brand/doors-threshold/icons/png-32/calendar.png
9be8cd73dbfc80e720513a098217d2da3a9066acdbc27fba879f02ca1823f983  docs/design/brand/doors-threshold/icons/png-32/clock.png
ea10791e675b96ec9dc9dce74636ff56afeb9a3555bfe3e4f98388842eef7aa8  docs/design/brand/doors-threshold/icons/png-32/fleet.png
76847f86b4a61d0d95965c77bcd203e527ddc3c3dc834c9eff63a3327792213f  docs/design/brand/doors-threshold/icons/png-32/notes.png
53563555f2a12f5050b24fea77d5e45c576570808988aca06f6654c23ceacc87  docs/design/brand/doors-threshold/icons/png-32/radar.png
5943609ff1ce85c7aee246c3420c09222d5b1cac6498cec419daf969a4c2cb0b  docs/design/brand/doors-threshold/icons/png-32/radio.png
54380c06c1d307effdd971efd23445c10fb7c4fc091150052d3a996e74a7850a  docs/design/brand/doors-threshold/icons/png-32/settings.png
9728382fc3a4b0c0c5347d3b8dd40b0bedaf4ef26cd583926b3126b5c440fb13  docs/design/brand/doors-threshold/icons/png-32/system.png
f7fd51b4ee745d2417fb14e4e08123b71bb1299d47503c2d838005a8c4ab2ad9  docs/design/brand/doors-threshold/icons/png-32/timber.png
c0f97b8789219ba0605d875f82f00fc69d043f407fb0999d4f82c87edf48cc02  docs/design/brand/doors-icon-extension/png-32/wave.png
4fb00b75d67bfb2b1c7167490425c785c4f97abf4325da2bdcab932522a198d2  docs/design/brand/doors-icon-extension/png-32/files.png
ef55db431904fccb8dabc5a367ddea63c40a7c7f45cabec479b4a734c15dc39a  docs/design/brand/doors-icon-extension/png-32/camera.png
9393ddfcc6ea3f97dfb86a6d47a6ee96f3084469987fecf3e83447e4e9be3512  docs/design/brand/doors-icon-extension/png-32/recorder.png
EOF
cut -c67- "${TMP}/want.sha" > "${TMP}/paths.txt"
xargs sha256sum < "${TMP}/paths.txt" > "${TMP}/have.sha" 2>/dev/null
check "the fourteen launcher icons are exactly the files the owner supplied" \
    "$(cmp -s "${TMP}/want.sha" "${TMP}/have.sha" && echo 1 || echo 0)"
python3 "${GEN}" -o "${TMP}/icons.c" >"${TMP}/gen.txt" 2>&1
check "the generator accepts the artwork" "$([ "$?" = "0" ] && echo 1 || echo 0)"
check "the committed pos_app_icons.c is exactly the generator's output" \
    "$(cmp -s "${TMP}/icons.c" "${ICONS_C}" && echo 1 || echo 0)"
python3 "${GEN}" -o "${TMP}/icons2.c" >/dev/null 2>&1
check "generating twice gives identical files" "$(cmp -s "${TMP}/icons.c" "${TMP}/icons2.c" && echo 1 || echo 0)"
xargs sha256sum < "${TMP}/paths.txt" > "${TMP}/after.sha" 2>/dev/null
check "generating did not touch the artwork" "$(cmp -s "${TMP}/want.sha" "${TMP}/after.sha" && echo 1 || echo 0)"
named=0
while read -r sha path; do
    grep -qx " \*   ${path}" "${ICONS_C}" && grep -qx " \*     sha256 ${sha}" "${ICONS_C}" && named=$((named + 1))
done < "${TMP}/want.sha"
check "the committed file names each of the fourteen sources and its hash (${named})" \
    "$([ "${named}" = "14" ] && echo 1 || echo 0)"
# One more icon is first-party (docs/design/doors-app-icons/README.md): drawn
# in the repository in the extension's line language, for an app no package
# has an icon for. It is kept apart from the owner's artwork above, pinned by
# its own hash, and must be exactly what its SVG master renders to.
ZABBIX_SHA=8de1165ce12ae53ea4cfc4aa190aaa26bea8053adf92f8a3035e260f1294d490
check "the first-party Zabbix icon is the committed file (sha256)" \
    "$([ "$(sha256sum "${FIRST}/zabbix.png" | cut -c1-64)" = "${ZABBIX_SHA}" ] && echo 1 || echo 0)"
python3 tools/design/render_app_icon.py docs/design/doors-app-icons/svg/zabbix.svg "${TMP}/first" >/dev/null 2>&1
check "it is exactly what render_app_icon.py draws from its SVG, at 24 and 32 px" \
    "$(cmp -s "${TMP}/first/png-32/zabbix.png" "${FIRST}/zabbix.png" &&
       cmp -s "${TMP}/first/png-24/zabbix.png" docs/design/doors-app-icons/png-24/zabbix.png && echo 1 || echo 0)"
check "pos_app_icons.c names it and its hash, as its source" \
    "$(grep -qx " \*   ${FIRST}/zabbix.png" "${ICONS_C}" && grep -qx " \*     sha256 ${ZABBIX_SHA}" "${ICONS_C}" &&
       echo 1 || echo 0)"
# Browser's is the second first-party icon (a globe), held the same way.
BROWSER_SHA=4a0d177ff87feece2d206008a535a8178bf0e1c3183e6591e65f6ac37991e399
check "the first-party Browser icon is the committed file (sha256)" \
    "$([ "$(sha256sum "${FIRST}/browser.png" | cut -c1-64)" = "${BROWSER_SHA}" ] && echo 1 || echo 0)"
python3 tools/design/render_app_icon.py docs/design/doors-app-icons/svg/browser.svg "${TMP}/first" >/dev/null 2>&1
check "it is exactly what render_app_icon.py draws from its SVG, at 24 and 32 px" \
    "$(cmp -s "${TMP}/first/png-32/browser.png" "${FIRST}/browser.png" &&
       cmp -s "${TMP}/first/png-24/browser.png" docs/design/doors-app-icons/png-24/browser.png && echo 1 || echo 0)"
check "pos_app_icons.c names it and its hash, as its source" \
    "$(grep -qx " \*   ${FIRST}/browser.png" "${ICONS_C}" && grep -qx " \*     sha256 ${BROWSER_SHA}" "${ICONS_C}" &&
       echo 1 || echo 0)"
# Vision's is the third (an eye), held the same way.
VISION_SHA=8f6b1bc052a7a5b201d7fe52775596e3cf874fe4ea5353f739e0888afb9cedb2
check "the first-party Vision icon is the committed file (sha256)" \
    "$([ "$(sha256sum "${FIRST}/vision.png" | cut -c1-64)" = "${VISION_SHA}" ] && echo 1 || echo 0)"
python3 tools/design/render_app_icon.py docs/design/doors-app-icons/svg/vision.svg "${TMP}/first" >/dev/null 2>&1
check "it is exactly what render_app_icon.py draws from its SVG, at 24 and 32 px" \
    "$(cmp -s "${TMP}/first/png-32/vision.png" "${FIRST}/vision.png" &&
       cmp -s "${TMP}/first/png-24/vision.png" docs/design/doors-app-icons/png-24/vision.png && echo 1 || echo 0)"
check "pos_app_icons.c names it and its hash, as its source" \
    "$(grep -qx " \*   ${FIRST}/vision.png" "${ICONS_C}" && grep -qx " \*     sha256 ${VISION_SHA}" "${ICONS_C}" &&
       echo 1 || echo 0)"

# The three Pocket Games titles' are the fourth to sixth (a column of cards,
# a card and a chip, four tiles), held the same way, one by one.
for game in "solitaire 701a7ee3c5359c6ebfd79f8fb6d7e933ec6d9245bf61e116b5826d9913ef6cc3" \
            "blackjack a9e16fac4b66e18c5a80139bf7954f2272c46e30dcd87dd6ef49c6228b149e5f" \
            "2048 704b20ec346c2fc573f2b2225c3b07a044f51fcab8f66273ac25679b2c384bb0"; do
    set -- ${game}
    check "the first-party $1 icon is the committed file (sha256)" \
        "$([ "$(sha256sum "${FIRST}/$1.png" | cut -c1-64)" = "$2" ] && echo 1 || echo 0)"
    python3 tools/design/render_app_icon.py "docs/design/doors-app-icons/svg/$1.svg" "${TMP}/first" >/dev/null 2>&1
    check "it is exactly what render_app_icon.py draws from its SVG, at 24 and 32 px" \
        "$(cmp -s "${TMP}/first/png-32/$1.png" "${FIRST}/$1.png" &&
           cmp -s "${TMP}/first/png-24/$1.png" "docs/design/doors-app-icons/png-24/$1.png" && echo 1 || echo 0)"
    check "pos_app_icons.c names it and its hash, as its source" \
        "$(grep -qx " \*   ${FIRST}/$1.png" "${ICONS_C}" && grep -qx " \*     sha256 $2" "${ICONS_C}" &&
           echo 1 || echo 0)"
done

# ---- each mask is its artwork's alpha ------------------------------------------
# Decoded with the Timber art reader, not the generator's own, so a decoding
# fault in one is not hidden by the same fault in the other.
python3 - "${ICONS_C}" "${ART}" "${EXT}" "${FIRST}" <<'PY' >"${TMP}/masks.txt" 2>&1
import re, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
src, art, ext, first = sys.argv[1:5]
text = open(src, encoding="utf-8").read()
names = re.findall(r"^const lv_image_dsc_t pos_app_icon_(\w+) = \{", text, re.M)
bad = 0
for n in names:
    body = re.search(r"pos_app_icon_%s_map\[\][^{]*\{(.*?)\};" % n, text, re.S).group(1)
    data = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", body)]
    dsc = re.search(r"pos_app_icon_%s = \{(.*?)\};" % n, text, re.S).group(1)
    hdr = dict(re.findall(r"\.header\.(\w+) = (\w+),", dsc))
    W, H, rows = read_png("%s/%s.png" % (ext if n in ("wave", "files", "camera", "recorder") else
                                         first if n in ("zabbix", "browser", "vision", "solitaire", "blackjack", "2048") else art,
                                         n))
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
check "there is one mask per launcher app, named by app id, and no other" \
    "$(grep -qx 'names 2048 blackjack browser calculator calendar camera clock files fleet notes radar radio recorder settings solitaire system timber vision wave zabbix' "${TMP}/masks.txt" && echo 1 || echo 0)"
check "the file holds 20,480 bytes of mask data and nothing else of size" \
    "$([ "$(grep -o '0x[0-9a-f][0-9a-f]' "${ICONS_C}" | wc -l)" = "20480" ] && echo 1 || echo 0)"
extra=$(grep -c 'doors-icon-extension' "${ICONS_C}")
check "of the extension's thirteen icons only Wave, Files, Camera and Recorder are compiled in (${extra} sources)" \
    "$([ "${extra}" = "4" ] && grep -qx " \*   ${EXT}/wave.png" "${ICONS_C}" &&
       grep -qx " \*   ${EXT}/files.png" "${ICONS_C}" &&
       grep -qx " \*   ${EXT}/camera.png" "${ICONS_C}" &&
       grep -qx " \*   ${EXT}/recorder.png" "${ICONS_C}" && echo 1 || echo 0)"
first=$(grep -c 'doors-app-icons' "${ICONS_C}")
check "and six first-party icons, Zabbix's, Browser's, Vision's and the three card and tile games' (${first} sources)" \
    "$([ "${first}" = "6" ] && echo 1 || echo 0)"

# ---- refusals ------------------------------------------------------------------
refused() { # <label> <message fragment> <source>...
    local label=$1 why=$2
    shift 2
    rm -f "${TMP}/refused.c"
    python3 "${GEN}" -o "${TMP}/refused.c" "$@" >"${TMP}/out.txt" 2>&1
    rc=$?
    check "${label} is refused" "$([ "${rc}" != "0" ] && [ ! -e "${TMP}/refused.c" ] && echo 1 || echo 0)"
    check "${label}: the refusal says why" "$(grep -q -- "${why}" "${TMP}/out.txt" && echo 1 || echo 0)"
}
mkdir -p "${TMP}/coloured" "${TMP}/opaque" "${TMP}/mixed" "${TMP}/empty" "${TMP}/badname" "${TMP}/clear"
# The package's fixed-colour brand variant is a natural wrong input: 32 x 32, not white.
cp docs/design/brand/doors-threshold/brand/doors-mark-dark.png "${TMP}/coloured/radio.png"
refused "an icon in a fixed colour" "not white" "${TMP}/coloured"
cp docs/design/brand/doors-threshold/boot/doors-boot-568x1232.png "${TMP}/opaque/radio.png"
refused "an icon with no alpha channel" "no alpha channel" "${TMP}/opaque"
cp "${ART}/radio.png" "${TMP}/mixed/radio.png"
cp docs/design/brand/doors-threshold/icons/png-24/system.png "${TMP}/mixed/system.png"
refused "a set mixing 32 px and 24 px icons" "same square size" "${TMP}/mixed"
refused "a folder with no icons" "no PNG files" "${TMP}/empty"
refused "two icons for one app id" "a second icon for app id wave" "${EXT}/wave.png" "${ART}/radio.png" \
    docs/design/brand/doors-icon-extension/png-24/wave.png
cp "${ART}/radio.png" "${TMP}/badname/Radio-2.png"
refused "an icon whose file name is not an app id" "not an app id" "${TMP}/badname"
python3 - "${TMP}/clear/radio.png" <<'PY'
import struct, sys, zlib
def chunk(k, b): return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xFFFFFFFF)
raw = b"".join(b"\x00" + b"\xff\xff\xff\x00" * 32 for _ in range(32))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 6, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
refused "a fully transparent icon" "every pixel is transparent" "${TMP}/clear"

# ---- who draws them, and how ---------------------------------------------------
# Every app the launcher lists, by its descriptor: each points at its own mask,
# so no launcher tile is left on the text-glyph fallback.
#
# An app with no mask is written "-" here, whatever the descriptor says. It
# used to be written whatever the field held, so `.icon_mask = NULL` - which
# is exactly the case this check exists to catch - read as a mask named NULL
# and passed.
python3 - <<'PY' >"${TMP}/apps.txt" 2>&1
import glob, re, sys
shell = open("ui/shell/shell.c", encoding="utf-8").read()
listed = re.findall(r"&(app_\w+)", re.search(r"apps\[\]\s*=\s*\{(.*?)\};", shell, re.S).group(1))
# The apps a shell can be configured without (OPTIONAL_APPS) are in the
# registry by default, so the launcher lists them too.
optional = re.search(r"^#define OPTIONAL_APPS (.+)$", shell, re.M)
listed += re.findall(r"&(app_\w+)", optional.group(1)) if optional else []
descs = {}
for path in sorted(glob.glob("apps/*/*.c")):
    text = open(path, encoding="utf-8").read()
    for var, body in re.findall(r"^const struct pocketos_app (app_\w+) = \{(.*?)^\};", text, re.S | re.M):
        field = lambda f: (re.search(r"\.%s = &?([\w\"]+)," % f, body) or [None, None])[1]
        mask = field("icon_mask")
        if mask in (None, "NULL", "0"):
            mask = "-"
        descs[var] = (path, field("id").strip('"'), field("icon"), mask)
for var in listed:
    path, app_id, icon, mask = descs[var]
    print(app_id, icon, mask, path)
PY
listed=$(sed -n '/static const struct pocketos_app \*const apps\[\]/,/};/p' ui/shell/shell.c |
         grep -o '&app_[a-z_]*' | wc -l)
listed=$((listed + $(grep '^#define OPTIONAL_APPS ' ui/shell/shell.c | grep -o '&app_[a-z_]*' | wc -l)))
check "every launcher app's descriptor was read (${listed} listed)" \
    "$([ "$(grep -c . "${TMP}/apps.txt")" = "${listed}" ] && ! grep -q Traceback "${TMP}/apps.txt" &&
       echo 1 || echo 0)"
for id in radio system fleet radar timber notes clock calendar calculator settings wave files camera recorder zabbix browser \
          vision solitaire blackjack 2048; do
    check "${id} uses its own icon, pos_app_icon_${id}" \
        "$(grep -qE "^${id} LV_SYMBOL_[A-Z_]+ pos_app_icon_${id} " "${TMP}/apps.txt" && echo 1 || echo 0)"
done
# DS §20 wants a mask on every launcher tile, and this is the check that says
# so. One app does not have one: RIFT, whose approved design package carries
# its mark as a design sheet (docs/design/rift/shots/identity-sheet.png) and
# not as the png-32 tint artwork the generator above turns into a mask.
# Drawing one here would be inventing branding, so the gap is named rather
# than hidden: the moment that artwork is supplied and generated, this list
# goes back to empty and the exception with it. tests/rift_lint.sh records
# the same gap from the app's side.
NO_MASK_ALLOWED="rift"
nomask=$(awk '$3 == "-" {print $1}' "${TMP}/apps.txt" | tr '\n' ' ')
unexpected=""
for id in ${nomask}; do
    case " ${NO_MASK_ALLOWED} " in *" ${id} "*) ;; *) unexpected="${unexpected} ${id}" ;; esac
done
check "no launcher app is without an icon mask except the one known to have no artwork${unexpected:+ (${unexpected})}" \
    "$([ -z "${unexpected}" ] && echo 1 || echo 0)"
check "and that one is still without it, so this exception has not gone stale (${nomask:-none})" \
    "$([ "$(echo ${nomask})" = "${NO_MASK_ALLOWED}" ] && echo 1 || echo 0)"
users=$(grep -rl 'pos_app_icon_' apps ui --include='*.c' --include='*.h' | grep -v "^${ICONS_C}$" | wc -l)
check "the masks are referenced only by the twenty app descriptors that have one (found in ${users} files)" \
    "$([ "${users}" = "20" ] && echo 1 || echo 0)"
check "the brand mark is not used as an app icon (DS §19.1)" \
    "$(grep -rqE 'icon_mask = &pos_brand_mark' apps ui && echo 0 || echo 1)"
# The DOORS launcher (DS §31) draws each app's portal icon from the runtime
# art; the app's own mask is what it draws, on the empty portal, when that
# art is missing (tests/doors_shell_test.sh runs that fallback).
check "the launcher falls back to the app's icon_mask, drawn in the environment's glyph style" \
    "$(grep -q 'lv_image_set_src(img, c->app->icon_mask)' ui/shell/home.c &&
       grep -q 'pos_style_add(img, POS_STYLE_ENV_GLYPH, 0)' ui/shell/home.c && echo 1 || echo 0)"
sed -n '/^lv_obj_t \*pocketui_tile_mask(/,/^}/p' ui/pocketui/pocketui.c > "${TMP}/tile.txt"
check "the tile draws a mask as an image in POS_STYLE_APP_ICON" \
    "$(grep -q 'pos_style_add(ic, POS_STYLE_APP_ICON, 0)' "${TMP}/tile.txt" &&
       grep -q 'lv_image_set_src(ic, mask)' "${TMP}/tile.txt" && echo 1 || echo 0)"
check "without a mask the tile still draws the text icon (the API's fallback, unused by the launcher)" \
    "$(grep -q 'lv_label_set_text(ic, icon)' "${TMP}/tile.txt" &&
       grep -q 'pos_style_add(ic, POS_STYLE_SYMBOL_LARGE, 0)' "${TMP}/tile.txt" &&
       grep -q 'pos_style_add(ic, POS_STYLE_ACCENT_TEXT, 0)' "${TMP}/tile.txt" && echo 1 || echo 0)"
check "the icon is not animated (reduced motion has nothing to change)" \
    "$(grep -q 'lv_anim' "${TMP}/tile.txt" && echo 0 || echo 1)"
sed -n '/styles\[POS_STYLE_APP_ICON\]/,/^}/p' ui/pocketui/pos_styles.c > "${TMP}/role.txt"
check "the icon role draws in accent_primary at full opacity" \
    "$(grep -q 'lv_style_set_image_recolor(s, tok(POS_COLOR_ACCENT_PRIMARY))' "${TMP}/role.txt" &&
       grep -q 'lv_style_set_image_recolor_opa(s, LV_OPA_COVER)' "${TMP}/role.txt" && echo 1 || echo 0)"
check "the shell build compiles the masks" \
    "$(grep -q '^    ../pocketui/pos_app_icons.c$' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

echo "app_icons_test: $failed failure(s)"
[ "$failed" -eq 0 ]
