#!/bin/bash
# Design System rule (§4, §8): components and apps reference role styles and
# tokens only. Colour literals and font symbols may appear solely in the theme
# engine files. Fails when any other first-party UI source names them.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
ALLOWED='ui/pocketui/pos_theme.c|ui/pocketui/pos_styles.c|ui/pocketui/fonts/'
check() { # <label> <regex>
    hits=$(grep -rnE "$2" ui apps --include='*.c' --include='*.h' 2>/dev/null | grep -vE "^($ALLOWED)")
    if [ -n "$hits" ]; then
        echo "FAIL $1:"; echo "$hits" | head -20; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}
check "no colour literals outside the theme engine" 'lv_color_hex|lv_color_make|lv_palette_|lv_color_white|lv_color_black'
check "no direct colour styles on objects" 'lv_obj_set_style_(bg|text|border|outline|shadow|line|arc|image_recolor)_color'
check "no font symbols outside pos_styles.c" 'lv_font_montserrat_|&pos_font_|lv_obj_set_style_text_font'
check "no local lv_style_t colour tables" 'lv_style_set_(bg|text|border|outline)_color'
echo "style_lint: $failed failure(s)"
exit $((failed > 0))
