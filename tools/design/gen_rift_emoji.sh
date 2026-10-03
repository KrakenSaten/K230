#!/bin/bash
# RIFT's colour emoji from Noto Color Emoji (tools/design/gen_rift_emoji.js):
# apps/rift/rift_emoji_seq.c, apps/rift/ui/rift_emoji_img.c and
# apps/rift/ui/rift_emoji_px.bin. Host-only; the outputs are committed, so a
# build needs neither Node nor the PNGs.
#
# Usage: gen_rift_emoji.sh <noto-emoji checkout> <font source dir> [lv_font_conv binary]
#   The checkout is googlefonts/noto-emoji at NOTO_EMOJI_COMMIT (2D/png/72 and
#   third_party/region-flags/png are all it reads; a sparse checkout does).
#   The font dir holds IBMPlexSans-Regular.ttf, so what Plex draws is left to
#   Plex. Node and lv_font_conv are the ones gen_fonts.sh uses: its pngjs and
#   opentype.js read the PNGs and the font.
set -eu
NOTO=${1:?noto-emoji checkout}
SRC=${2:?font source dir}
CONV=${3:-lv_font_conv}
NOTO_EMOJI_COMMIT=e20cbc2bbec1926686be9f9bee7d1d2cfa1fea0e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
have=$(git -C "$NOTO" rev-parse HEAD)
if [ "$have" != "$NOTO_EMOJI_COMMIT" ]; then
  echo "gen_rift_emoji: $NOTO is at $have, not the pinned $NOTO_EMOJI_COMMIT" >&2
  exit 1
fi
# gen_fonts.sh's RANGE: what the Plex bitmaps hold.
RANGE=$(sed -n 's/^RANGE="\(.*\)"$/\1/p' "$REPO/tools/design/gen_fonts.sh")
node "$REPO/tools/design/gen_rift_emoji.js" "$(command -v "$CONV")" "$NOTO" "$SRC" "$RANGE" "$REPO"
