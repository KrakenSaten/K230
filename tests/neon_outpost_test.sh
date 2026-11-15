#!/bin/bash
# Focused native engine/renderer checks, optionally with sanitizers.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
build=${NEON_BUILD_DIR:-out/neon-outpost}
mkdir -p "$build"
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
if [ "${NEON_SANITIZE:-0}" = 1 ]; then
    flags+=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" -Iapps/neon-outpost tests/neon_outpost_test.c \
    apps/neon-outpost/neon.c apps/neon-outpost/neon_render.c -lm -o "$build/engine-test"
"$build/engine-test"
