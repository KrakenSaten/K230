#!/bin/bash
# Build what pos-library needs that Doors' image does not have - libzim
# (reader only) and, for the K230, zstd - into a prefix of its own, outside
# this repository, the SDK, its sysroot and the toolchain, none of which is
# written to. docs/apps/LIBRARY.md "Building" says how the result is used.
#
#   tools/library/build_libzim.sh host <workdir>
#       libzim 9.8.2 with Xapian (full-text search), against the build host's
#       zstd, liblzma, Xapian and ICU (development files from
#       $HOSTDEPS_ROOT, e.g. Ubuntu packages unpacked with dpkg -x).
#   tools/library/build_libzim.sh k230 <workdir>
#       zstd 1.5.7 (Buildroot 2025.02.1's version and hash) and libzim 9.8.2
#       without Xapian, static, with the pinned Xuantie toolchain against the
#       SDK's sysroot (read only; liblzma comes from there, as in the image).
#       Xapian and ICU are not built for the K230: see LIBRARY.md "Search".
#
# Needs meson >= 1.0 and ninja on PATH (MESON, NINJA override), git, curl.
# Results: <workdir>/prefix/<target>/{include,lib}.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
TARGET=${1:?usage: build_libzim.sh host|k230 <workdir>}
W=$(realpath -m "${2:?usage: build_libzim.sh host|k230 <workdir>}")
LIBZIM_TAG=9.8.2
LIBZIM_COMMIT=26ec526f74e8342a40da5ab14e364546988e0e1a
ZSTD_VERSION=1.5.7
ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
TOOLCHAIN=${TOOLCHAIN:-/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2}
K230_CFLAGS="-mcpu=c908v -mtune=c908 -O2"
MESON=${MESON:-meson}
NINJA=${NINJA:-ninja}
PREFIX=$W/prefix/$TARGET
mkdir -p "$W/src" "$W/build" "$PREFIX"

if [ ! -d "$W/src/libzim" ]; then
    git clone -q --depth 1 -b "$LIBZIM_TAG" https://github.com/openzim/libzim.git "$W/src/libzim"
fi
got=$(git -C "$W/src/libzim" rev-parse HEAD)
[ "$got" = "$LIBZIM_COMMIT" ] || { echo "libzim $LIBZIM_TAG is $got, expected $LIBZIM_COMMIT" >&2; exit 1; }

libzim_options=(-Dwithout_writer=true -Dtests=false -Dexamples=false -Ddoc=false -Ddefault_library=static
                --buildtype=release -Dwerror=false --prefix "$PREFIX" --libdir lib)

case "$TARGET" in
host)
    R=${HOSTDEPS_ROOT:?set HOSTDEPS_ROOT to the unpacked development packages}
    cat >"$W/build/pkg-config-host" <<EOF
#!/bin/sh
PKG_CONFIG_LIBDIR=$R/usr/lib/x86_64-linux-gnu/pkgconfig:$R/usr/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig \\
PKG_CONFIG_SYSROOT_DIR=$R exec ${PKGCONF:-pkgconf} "\$@"
EOF
    chmod +x "$W/build/pkg-config-host"
    printf "[binaries]\npkg-config = '%s'\n" "$W/build/pkg-config-host" >"$W/build/host.ini"
    "$MESON" setup --wipe "$W/build/libzim-host" "$W/src/libzim" --native-file "$W/build/host.ini" \
        "${libzim_options[@]}" -Dwith_xapian=true
    "$NINJA" -C "$W/build/libzim-host" install
    ;;
k230)
    SYSROOT=${K230_SYSROOT:?set K230_SYSROOT to the SDK sysroot (host/riscv64-buildroot-linux-gnu/sysroot)}
    X=$TOOLCHAIN/bin/riscv64-unknown-linux-gnu
    # zstd: the release tarball, checked against Buildroot's hash.
    if [ ! -d "$W/src/zstd-$ZSTD_VERSION" ]; then
        curl -sfL -o "$W/src/zstd-$ZSTD_VERSION.tar.gz" \
            "https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/zstd-$ZSTD_VERSION.tar.gz"
        echo "$ZSTD_SHA256  $W/src/zstd-$ZSTD_VERSION.tar.gz" | sha256sum -c --quiet
        tar -xzf "$W/src/zstd-$ZSTD_VERSION.tar.gz" -C "$W/src"
    fi
    make -C "$W/src/zstd-$ZSTD_VERSION/lib" -j8 clean >/dev/null
    make -C "$W/src/zstd-$ZSTD_VERSION/lib" -j8 CC="$X-gcc --sysroot=$SYSROOT" AR="$X-ar" \
        CFLAGS="$K230_CFLAGS" ZSTD_LEGACY_SUPPORT=0 HAVE_PTHREAD=1 libzstd.a libzstd.pc
    make -C "$W/src/zstd-$ZSTD_VERSION/lib" PREFIX="$PREFIX" LIBDIR="$PREFIX/lib" install-static install-includes \
        install-pc >/dev/null
    # libzim: liblzma from the sysroot, zstd from the prefix, no Xapian.
    # The sysroot's .pc files name /usr, which pkgconf leaves out as a
    # system directory; the compiler's --sysroot finds it.
    cat >"$W/build/pkg-config-k230" <<EOF
#!/bin/sh
PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR= \\
PKG_CONFIG_SYSTEM_INCLUDE_PATH=/usr/include PKG_CONFIG_SYSTEM_LIBRARY_PATH=/usr/lib:/lib \\
exec ${PKGCONF:-pkgconf} "\$@"
EOF
    chmod +x "$W/build/pkg-config-k230"
    cat >"$W/build/k230.ini" <<EOF
[binaries]
c = ['$X-gcc', '--sysroot=$SYSROOT']
cpp = ['$X-g++', '--sysroot=$SYSROOT']
ar = '$X-ar'
strip = '$X-strip'
pkg-config = '$W/build/pkg-config-k230'

[built-in options]
c_args = ['-mcpu=c908v', '-mtune=c908']
cpp_args = ['-mcpu=c908v', '-mtune=c908']

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'c908'
endian = 'little'
EOF
    "$MESON" setup --wipe "$W/build/libzim-k230" "$W/src/libzim" --cross-file "$W/build/k230.ini" \
        "${libzim_options[@]}" -Dwith_xapian=false
    "$NINJA" -C "$W/build/libzim-k230" install
    ;;
*)
    echo "usage: build_libzim.sh host|k230 <workdir>" >&2
    exit 2
    ;;
esac
echo "libzim $LIBZIM_TAG ($TARGET) in $PREFIX"
