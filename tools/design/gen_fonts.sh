#!/bin/bash
# Convert the IBM Plex sources into LVGL bitmap fonts (DS v0.1 §3, D4: no
# 10 px mini font). Host-only; the generated C files are committed under
# ui/pocketui/fonts/ so builds never need Node or the TTFs.
#
# Usage: gen_fonts.sh <font source dir> [lv_font_conv binary]
#   The source dir holds IBMPlexSans-Regular.ttf, IBMPlexSans-SemiBold.ttf,
#   IBMPlexMono-Regular.ttf, IBMPlexMono-Medium.ttf (OFL-1.1).
#
# Symbols are named pos_font_<family>_<size>[_<weight>]; user-facing text
# must not call them "IBM Plex" (OFL Reserved Font Name "Plex", see
# docs/LICENSING.md).
set -eu
SRC=${1:?font source dir}
CONV=${2:-lv_font_conv}
OUT="$(cd "$(dirname "$0")/../.." && pwd)/ui/pocketui/fonts"
# ASCII, Latin-1 (æøå, °, ·, ×), general punctuation (– — ‘ ’ “ ” • … ‹ ›),
# arrows through the minus sign. Status dots (●) are drawn as discs, not
# glyphs; Plex has no U+25CF.
RANGE="0x20-0x7E,0xA0-0xFF,0x2013-0x2026,0x2039-0x203A,0x2190-0x2212"
gen() { # name ttf size
  local name=$1 ttf=$2 size=$3
  "$CONV" --font "$SRC/$ttf" --range "$RANGE" --size "$size" --bpp 4 --format lvgl \
          --no-compress --lv-font-name "$name" -o "$OUT/$name.c"
  echo "$name: $(wc -c < "$OUT/$name.c") bytes of C"
}
mkdir -p "$OUT"
gen pos_font_sans_16          IBMPlexSans-Regular.ttf  16
gen pos_font_sans_20          IBMPlexSans-Regular.ttf  20
gen pos_font_sans_20_semibold IBMPlexSans-SemiBold.ttf 20
gen pos_font_sans_24_semibold IBMPlexSans-SemiBold.ttf 24
gen pos_font_sans_40_semibold IBMPlexSans-SemiBold.ttf 40
gen pos_font_sans_48_semibold IBMPlexSans-SemiBold.ttf 48
gen pos_font_mono_14          IBMPlexMono-Regular.ttf  14
gen pos_font_mono_16_medium   IBMPlexMono-Medium.ttf   16
gen pos_font_mono_20          IBMPlexMono-Regular.ttf  20
gen pos_font_mono_24          IBMPlexMono-Regular.ttf  24
gen pos_font_mono_32          IBMPlexMono-Regular.ttf  32
# Text size Medium and Large (DS §46): the same four faces at the sizes the
# semantic type roles take there (ui/pocketui/pos_type.c). Small uses only
# the sizes above.
gen pos_font_sans_19          IBMPlexSans-Regular.ttf  19
gen pos_font_sans_22          IBMPlexSans-Regular.ttf  22
gen pos_font_sans_24          IBMPlexSans-Regular.ttf  24
gen pos_font_sans_28          IBMPlexSans-Regular.ttf  28
gen pos_font_sans_28_semibold IBMPlexSans-SemiBold.ttf 28
gen pos_font_sans_32_semibold IBMPlexSans-SemiBold.ttf 32
gen pos_font_mono_17          IBMPlexMono-Regular.ttf  17
gen pos_font_mono_19          IBMPlexMono-Regular.ttf  19
gen pos_font_mono_28          IBMPlexMono-Regular.ttf  28
gen pos_font_mono_19_medium   IBMPlexMono-Medium.ttf   19
gen pos_font_mono_22_medium   IBMPlexMono-Medium.ttf   22
echo "done: $(ls "$OUT"/*.c | wc -l) fonts in $OUT"
# The DOORS clock (lock screen and launcher header): digits, colon, minus and
# space only, so two display sizes cost a few kilobytes rather than a full
# Latin-1 set each. "--:--" is what an unset clock draws.
CLOCK_RANGE="0x20,0x2D,0x30-0x3A"
gen_clock() { # name ttf size
  local name=$1 ttf=$2 size=$3
  "$CONV" --font "$SRC/$ttf" --range "$CLOCK_RANGE" --size "$size" --bpp 4 --format lvgl \
          --no-compress --lv-font-name "$name" -o "$OUT/$name.c"
  echo "$name: $(wc -c < "$OUT/$name.c") bytes of C"
}
gen_clock pos_font_clock_64 IBMPlexSans-Regular.ttf 64
gen_clock pos_font_clock_96 IBMPlexSans-Regular.ttf 96
