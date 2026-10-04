#!/bin/bash
# Build decoder_check against the tree's own decoder sources (host gcc).
# Usage: build.sh OUT_BINARY
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${DOORS_ROOT:-$(cd "$HERE/../../../.." && pwd)}  # DOORS_ROOT: the tree whose decoder to test
V=$ROOT/core/pocketvision
gcc -std=c11 -O2 -Wall -Wextra -Werror -I"$V" -I"$ROOT/core" -o "$1" "$HERE/decoder_check.c" \
    "$V/vision_decode.c" "$V/vision_nms.c" "$V/vision_labels.c" "$V/vision_traffic.c" -lm
