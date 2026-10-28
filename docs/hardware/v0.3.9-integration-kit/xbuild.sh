#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# K230 cross-build of doors-shell and pos-vision from an integration tar,
# against the existing k230_pocketos_defconfig output (not modified).
#   xbuild.sh <src.tar> <build_id>
set -euo pipefail
TAR=$1; BID=$2
O=$HOME/work/t-display-k230/k230_linux_sdk/output/k230_pocketos_defconfig
SR=$O/host/riscv64-buildroot-linux-gnu/sysroot
TC=$O/host/share/buildroot/toolchainfile.cmake
X=$O/host/bin/riscv64-unknown-linux-gnu-
W=${XBUILD_DIR:-$HOME/work/xbuild-$BID}
rm -rf "$W"; mkdir -p "$W/logs" "$W/src"
exec > >(tee "$W/logs/build.log") 2>&1
echo "== $(date -Is) build $BID"; sha256sum "$TAR"
tar -xf "$TAR" -C "$W/src" --strip-components=1
echo "$BID" > "$W/src/BUILD_ID"
export PATH=$O/host/bin:$PATH
# pos-vision as pocketos.mk builds it (KPU engine, nncase runtime from the sysroot)
make -C "$W/src" CC=${X}gcc CXX=${X}g++ AR=${X}ar POCKETVISION_KPU=1 tools/vision/pos-vision > "$W/logs/vision.log" 2>&1
echo "pos-vision: rc 0, $(grep -c 'warning:' "$W/logs/vision.log" || true) warnings"
# shell: sysroot LVGL (already carries LV_USE_QRCODE)
SB=$W/src/ui/shell/build-k230
cmake -S "$W/src/ui/shell" -B "$SB" -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
    -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot > "$W/logs/shell-configure.log" 2>&1
cmake --build "$SB" -j8 --target pocketos-shell > "$W/logs/shell-build.log" 2>&1
echo "shell: rc 0, $(grep -c 'warning:' "$W/logs/shell-build.log" || true) warnings"
A=$W/artifacts; mkdir -p "$A"
${X}strip --remove-section=.comment --remove-section=.note -o "$A/doors-shell" "$SB/pocketos-shell"
${X}strip --remove-section=.comment --remove-section=.note -o "$A/pos-vision" "$W/src/tools/vision/pos-vision"
(cd "$A" && sha256sum doors-shell pos-vision > SHA256SUMS && cat SHA256SUMS && ls -l)
for b in doors-shell pos-vision; do echo "-- $b"; ${X}readelf -d "$A/$b" | grep -E 'NEEDED|RPATH|RUNPATH'; done
echo "-- GLIBC versions needed"
for b in doors-shell pos-vision; do ${X}objdump -T "$A/$b" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1; done
echo "-- identity strings"; strings "$A/doors-shell" | grep -m3 -E "^$BID$|0\.3\.[0-9]+ ?$BID|$BID"; strings "$A/pos-vision" | grep -m2 "$BID" || echo "(pos-vision carries no build id)"
echo "== done $(date -Is)"
