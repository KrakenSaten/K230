#!/bin/bash
# RIFT's colour emoji: the inline image-glyph proof (tests/rift_emoji_glyph_test.c),
# the body and preview styles drawing the sample at every text size
# (tests/rift_emoji_ui_test.c), and what the generated assets must and must
# not hold.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) with rift_emoji_glyph_test
# and rift_emoji_ui_test beside it. EMOJI_SHOTS=<dir> keeps the PNGs.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

for t in rift_emoji_glyph_test rift_emoji_ui_test; do
    BIN=$(dirname "$SHELL_BIN")/$t
    if [ -x "$BIN" ]; then
        log=$(SDL_VIDEODRIVER=dummy timeout 120 "$BIN" 2>&1); rc=$?
        printf '%s\n' "$log" | grep -E "^FAIL|^note|$t:"
        check "$t" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
        check "$t: LVGL warned about nothing" "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
    else
        echo "FAIL $t binary missing: $BIN"; failed=$((failed + 1))
    fi
done
check "the tests are host-only targets" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(rift_emoji_glyph_test' && echo 1 || echo 0)"

IMG=apps/rift/ui/rift_emoji_img.c
SEQ=apps/rift/rift_emoji_seq.c
count=$(sed -n 's/^const unsigned rift_emoji_img_count = \([0-9]*\);$/\1/p' $IMG)
rows=$(grep -c '^    { 0x' $IMG)
size=$(sed -n 's/^const unsigned rift_emoji_px_size = \([0-9]*\);$/\1/p' $IMG)
check "the image index has its $count rows, and the pixels are the $size bytes it says" \
    "$([ "$count" = "$rows" ] && [ "$(wc -c < apps/rift/ui/rift_emoji_px.bin)" = "$size" ] && echo 1 || echo 0)"
# The product rule: no skin-tone artwork, and RIFT strips the modifier.
check "no image is keyed by a skin-tone modifier, no sequence holds one" \
    "$(! grep -q -E '^    \{ 0x1F3F[B-F],' $IMG && ! grep -q -E '0x1F3F[B-F]' $SEQ && echo 1 || echo 0)"
check "the folding strips skin tones" \
    "$(grep -q '0x1F3FBu && cp <= 0x1F3FFu' apps/rift/rift_emoji.c && echo 1 || echo 0)"
# Plex is RIFT's text font and stays untouched: no Plex font has a fallback.
check "no Plex font is changed: none has a fallback" \
    "$(! grep -l -E '\.fallback = &' ui/pocketui/fonts/pos_font_*.c >/dev/null 2>&1 && echo 1 || echo 0)"
check "no runtime PNG, FreeType or imgfont: the font driver reads compiled-in RGB565A8" \
    "$(! grep -q -E 'lodepng|lv_freetype|lv_imgfont|\.png' apps/rift/ui/rift_emoji_font.c &&
       grep -q 'LV_COLOR_FORMAT_RGB565A8' apps/rift/ui/rift_emoji_font.c && echo 1 || echo 0)"
# The body's role is MSG_ROLE_BODY (a POS_STYLE_* alias since PR #39); its
# colour style must be added for the same role the label is built with.
check "the body and the preview use the colour styles" \
    "$(grep -q -E '^#define MSG_ROLE_BODY POS_STYLE_[A-Z_]+$' apps/rift/ui/rift_thread.c &&
       grep -q 'r->body = fit_label(r->column, MSG_ROLE_BODY);' apps/rift/ui/rift_thread.c &&
       grep -q 'rift_emoji_style_add(r->body, MSG_ROLE_BODY)' apps/rift/ui/rift_thread.c &&
       grep -q 'rift_emoji_style_add(r->preview, POS_STYLE_CAPTION)' apps/rift/ui/rift_conv_list.c &&
       grep -q 'rift_emoji_fold(rift_msg_body(msg)' apps/rift/ui/rift_thread.c &&
       grep -q 'rift_emoji_fold(rift_msg_body(msg)' apps/rift/rift_format_msg.c && echo 1 || echo 0)"
check "no emoji is written as an ASCII smiley" \
    "$(! grep -q -E 'smiley_for|"\(y\)"|"<3"' apps/rift/rift_format.c && echo 1 || echo 0)"

echo "rift_emoji_shell_test: $failed failure(s)"
exit $((failed > 0))
