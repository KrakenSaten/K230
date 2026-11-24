#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Build infer-bench twice (research only; nothing is installed):
#   build/infer-bench-host   x86-64, C reference only, for checking the reference against the float model
#   build/infer-bench        riscv64 with the nncase 2.11 runtime of the pinned SDK (WITH_NNCASE)
# Usage (WSL): poc/build.sh [SDK_OUTPUT]
#   SDK_OUTPUT defaults to the vendor tree's k230_pocketos_defconfig output (docs/BUILD_ENVIRONMENT.md).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
O=${1:-$HOME/work/t-display-k230/k230_linux_sdk/output/k230_pocketos_defconfig}
B=${BUILD_DIR:-$HOME/work/psk-neural/poc-build}
mkdir -p "$B"
g++ -std=gnu++17 -O2 -Wall -Wextra -o "$B/infer-bench-host" "$here/infer_bench.cpp" -lm
echo "host: $B/infer-bench-host"
CXX="$O/host/bin/riscv64-unknown-linux-gnu-g++"
if [ ! -x "$CXX" ]; then
    echo "no cross compiler at $CXX: riscv64 build skipped" >&2
    exit 0
fi
# the same libraries pos-vision links (Makefile VISION_LIBS), minus AI2D's use
"$CXX" -std=gnu++17 -O2 -Wall -Wextra -Wno-multichar -DWITH_NNCASE -o "$B/infer-bench" "$here/infer_bench.cpp" \
    -Wl,--start-group -lNncase.Runtime.Native -lnncase.rt_modules.k230 -lfunctional_k230 -Wl,--end-group \
    -lmmz -lpthread -ldl -lm
echo "riscv64: $B/infer-bench"
