#!/bin/bash
# PR #98 (Neon theme) K230 test build, run inside WSL as the build user.
#
#   build_neon_k230.sh <src.tar from `git archive <commit>`> <short commit>
#
# 1. LVGL 59dc7e4 from the vendor package's already patched source
#    (0001-0004, incl. the K230 DRM staging scanout buffer), built the way
#    Buildroot's cmake-package builds it, twice:
#      control: the package's lv_conf.defaults unchanged (QR off, as v0.3.6)
#      qr     : the same plus `LV_USE_QRCODE 1`, which is exactly what
#               apply_to_sdk.sh adds since v0.3.7 (offline Help & Feedback QR).
# 2. A hard-link sysroot: the pinned vendor sysroot with only usr/include/lvgl
#    and usr/lib/liblvgl* replaced by the qr build. The vendor sysroot is not
#    modified and not copied.
# 3. The shell with pocketos.mk's CMake flags, in sysroot mode against it.
#
# Nothing here writes outside $W.
set -euo pipefail
TAR=${1:?source tar}
SHORT=${2:?short commit}
VEND=$HOME/work/doors-vendor/k230_linux_sdk
O=$VEND/output/k230_canmv_t_display_rm69a10_defconfig
SR=$O/host/riscv64-buildroot-linux-gnu/sysroot
TC=$O/host/share/buildroot/toolchainfile.cmake
LSRC=$O/build/lvgl-59dc7e436ae97a25e32656739ea6a943f9f11b6a
X=$O/host/bin/riscv64-unknown-linux-gnu-
W=$HOME/work/neon-p98/$SHORT
mkdir -p "$W/logs"
exec > >(tee "$W/logs/build.log") 2>&1
echo "== $(date -Is) commit $SHORT"
sha256sum "$TAR"
cat "$LSRC/.applied_patches_list"

# --- 1. LVGL --------------------------------------------------------------
rm -rf "$W/lvgl-src"
rsync -a --exclude buildroot-build --exclude '.stamp_*' --exclude '.files-list*' "$LSRC/" "$W/lvgl-src/"
cp "$W/lvgl-src/lv_conf.defaults" "$W/lvgl-src/configs/doors-control.defaults"
{ grep -v '^[[:space:]]*LV_USE_QRCODE[[:space:]]' "$W/lvgl-src/lv_conf.defaults"; printf '\nLV_USE_QRCODE 1\n'; } \
    > "$W/lvgl-src/configs/doors-qr.defaults"
diff "$W/lvgl-src/configs/doors-control.defaults" "$W/lvgl-src/configs/doors-qr.defaults" || true
for v in control qr; do
    B=$W/lvgl-build-$v
    rm -rf "$B" "$W/lvgl-stage-$v"
    cmake -S "$W/lvgl-src" -B "$B" -DCONFIG=doors-$v \
        -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_COLOR_MAKEFILE=OFF -DBUILD_DOC=OFF -DBUILD_DOCS=OFF \
        -DBUILD_EXAMPLE=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TEST=OFF -DBUILD_TESTS=OFF -DBUILD_TESTING=OFF \
        > "$W/logs/lvgl-$v-configure.log" 2>&1
    cmake --build "$B" -j8 > "$W/logs/lvgl-$v-build.log" 2>&1
    DESTDIR="$W/lvgl-stage-$v" cmake --install "$B" > "$W/logs/lvgl-$v-install.log" 2>&1
    echo "lvgl $v: rc 0, $(grep -c 'warning:' "$W/logs/lvgl-$v-build.log" || true) warnings"
    grep -E '^#define LV_USE_QRCODE' "$W/lvgl-stage-$v/usr/include/lvgl/lv_conf.h"
done
diff <(grep -E '^#define ' "$W/lvgl-stage-control/usr/include/lvgl/lv_conf.h") \
     <(grep -E '^#define ' "$W/lvgl-stage-qr/usr/include/lvgl/lv_conf.h") || true

# --- 2. hard-link sysroot ---------------------------------------------------
# Hard links (same filesystem): no data copied, and ld sees the libc linker
# scripts inside the sysroot, which a symlink farm defeats. Only the LVGL
# entries are unlinked here and replaced; the vendor files are untouched.
NS=$W/sysroot
rm -rf "$NS"; cp -al "$SR" "$NS"
rm -rf "$NS/usr/include/lvgl" "$NS"/usr/lib/liblvgl*
cp -a "$W/lvgl-stage-qr/usr/include/lvgl" "$NS/usr/include/lvgl"
cp -a "$W/lvgl-stage-qr/usr/lib/"liblvgl* "$NS/usr/lib/"
sha256sum "$SR/usr/lib/liblvgl.so.9.5.0" "$NS/usr/lib/liblvgl.so.9.5.0"

# --- 3. shell ---------------------------------------------------------------
rm -rf "$W/src"; mkdir -p "$W/src"; tar -xf "$TAR" -C "$W/src"
echo "$SHORT" > "$W/src/BUILD_ID"
SB=$W/src/ui/shell/build-k230
PKG_CONFIG_SYSROOT_DIR="$NS" PKG_CONFIG_LIBDIR="$NS/usr/lib/pkgconfig:$NS/usr/share/pkgconfig" \
cmake -S "$W/src/ui/shell" -B "$SB" -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_SYSROOT="$NS" \
    -DCMAKE_BUILD_TYPE=Release -DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot \
    > "$W/logs/shell-configure.log" 2>&1
grep -E 'PocketTimber|lv_conf|sysroot' "$W/logs/shell-configure.log" || true
PKG_CONFIG_SYSROOT_DIR="$NS" PKG_CONFIG_LIBDIR="$NS/usr/lib/pkgconfig:$NS/usr/share/pkgconfig" \
cmake --build "$SB" -j8 --target pocketos-shell > "$W/logs/shell-build.log" 2>&1
echo "shell: rc 0, $(grep -c 'warning:' "$W/logs/shell-build.log" || true) warnings"

# --- artefacts ------------------------------------------------------------
A=$W/artifacts; rm -rf "$A"; mkdir -p "$A/lib"
cp "$SB/pocketos-shell" "$A/doors-shell.unstripped"
${X}strip --remove-section=.comment --remove-section=.note -o "$A/doors-shell" "$SB/pocketos-shell"
for f in liblvgl.so.9.5.0 liblvgl_thorvg.so.9.5.0 liblvgl_demos.so.9.5.0 liblvgl_examples.so.9.5.0; do
    ${X}strip --remove-section=.comment --remove-section=.note -o "$A/lib/$f" "$W/lvgl-stage-qr/usr/lib/$f"
    ln -sf "$f" "$A/lib/${f%.5.0}"
done
${X}strip -o "$A/control-liblvgl.so.9.5.0" "$W/lvgl-stage-control/usr/lib/liblvgl.so.9.5.0"
(cd "$A" && sha256sum doors-shell doors-shell.unstripped lib/*.so.9.5.0 control-liblvgl.so.9.5.0 > SHA256SUMS)
cat "$A/SHA256SUMS"
file "$A/doors-shell"
${X}readelf -d "$A/doors-shell" | grep -E 'NEEDED|RUNPATH|RPATH'
${X}readelf -d "$A/lib/liblvgl.so.9.5.0" | grep -E 'NEEDED|SONAME'
echo "== done $(date -Is)"
