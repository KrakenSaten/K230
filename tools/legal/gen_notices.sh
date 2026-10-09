#!/usr/bin/env bash
# THIRD_PARTY_NOTICES.txt from third_party/notices/SOURCES.
#
#   gen_notices.sh                 write THIRD_PARTY_NOTICES.txt, and the hashes
#                                  of every licence file the package hands to
#                                  legal-info (LICENSE, NOTICE and the notices)
#                                  in platforms/k230/package/pocketos/pocketos.hash
#   gen_notices.sh --check         fail if THIRD_PARTY_NOTICES.txt is not what
#                                  SOURCES and the texts produce, or if
#                                  pocketos.hash does not match those files
#   gen_notices.sh --verify-upstream [--sdk DIR] [--strict]
#                                  compare every copied text with the pinned
#                                  upstream file, byte for byte; with --strict
#                                  a text that cannot be compared fails too
#   gen_notices.sh --refresh [--sdk DIR]
#                                  copy the pinned upstream files into texts/
#   gen_notices.sh --verify-lvconf FILE
#                                  fail if the vendor LVGL configuration
#                                  compiles in a bundled component that has no
#                                  entry here
#
# Upstream files are read from git objects at the pinned commits (vendor/
# RadioLib, vendor/ggwave, vendor/lvgl, vendor/RIFT, vendor/Crypto) or, for LVGL and lv_port_linux, from
# the source archives in the SDK's download directory (--sdk), which are what
# the image is actually built from.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NOTICES="${REPO}/third_party/notices"
OUT="${REPO}/THIRD_PARTY_NOTICES.txt"
HASH="${REPO}/platforms/k230/package/pocketos/pocketos.hash"
# Doors' own licence and NOTICE (Apache-2.0, ADR-013): not generated, but
# collected by legal-info with the notices, so pocketos.hash covers them too.
OWN_FILES="LICENSE NOTICE"
SDK=""
MODE="generate"
STRICT=0
LVCONF=""

while [ $# -gt 0 ]; do
    case "$1" in
        --check|--verify-upstream|--refresh) MODE="${1#--}" ;;
        --verify-lvconf) MODE="verify-lvconf"; LVCONF="${2:?--verify-lvconf needs a file}"; shift ;;
        --sdk) SDK="${2:?--sdk needs a directory}"; shift ;;
        --strict) STRICT=1 ;;
        *) echo "usage: gen_notices.sh [--check | --verify-upstream [--sdk DIR] [--strict] | --refresh [--sdk DIR] | --verify-lvconf FILE]" >&2; exit 2 ;;
    esac
    shift
done

trim() { sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'; }

# id|component|version|licence|in image|text, one per line, comments dropped.
entries() {
    tr -d '\r' < "${NOTICES}/SOURCES" | grep -v -E '^[[:space:]]*(#|$)' | awk -F ' [|] ' 'NF != 6 {
        printf "gen_notices.sh: SOURCES line %d has %d fields, not 6\n", NR, NF > "/dev/stderr"; exit 1 }
        { print }'
}
field() { printf '%s\n' "$1" | awk -F ' [|] ' -v n="$2" '{print $n}' | trim; }

# Where an entry's text lives in this repository.
text_file() { # <id> <text spec>
    case "$2" in
        repo:*) printf '%s/%s\n' "${REPO}" "${2#repo:}" ;;
        text:*) printf '%s/texts/%s\n' "${NOTICES}" "${2#text:}" ;;
        *)      printf '%s/texts/%s.txt\n' "${NOTICES}" "$1" ;;
    esac
}

# Carriage returns stripped: a Windows checkout may hand these over with CRLF.
pin_radiolib() { tr -d '\r\n' < "${REPO}/platforms/k230/vendor_radiolib_commit.txt"; }
pin_ggwave() { tr -d '\r\n' < "${REPO}/platforms/k230/vendor_ggwave_commit.txt"; }
# MeshCore and rweather's Crypto are pinned by protocols/meshcore, which is what
# builds them into meshcored; the notices name the same commits.
pin_meshcore() { tr -d '\r\n' < "${REPO}/protocols/meshcore/vendor_rift_commit.txt"; }
pin_arduinolibs() { tr -d '\r\n' < "${REPO}/protocols/meshcore/vendor_crypto_commit.txt"; }
# LVGL: the commit the vendor board defconfig builds, pinned here, since Doors
# keeps no copy of that defconfig; apply_to_sdk.sh refuses a composed
# defconfig that builds another.
pin_lvgl() { tr -d '\r\n' < "${REPO}/platforms/k230/vendor_lvgl_commit.txt"; }
commit_in() { printf '%s\n' "$1" | grep -o -E 'commit [0-9a-f]{40}' | head -1 | cut -d' ' -f2; }

# The upstream bytes of an entry on stdout; returns 3 when nothing here can
# supply them.
upstream() { # <text spec> <version field>
    local origin="${1%%:*}" path="${1#*:}" commit tarball top
    case "${origin}" in
        radiolib)
            commit="$(pin_radiolib)"
            [ -n "${commit}" ] || { echo "gen_notices.sh: no RadioLib pin" >&2; return 2; }
            git -C "${REPO}/vendor/RadioLib" cat-file -e "${commit}:${path}" 2>/dev/null || return 3
            git -C "${REPO}/vendor/RadioLib" show "${commit}:${path}" ;;
        ggwave)
            commit="$(pin_ggwave)"
            [ -n "${commit}" ] || { echo "gen_notices.sh: no ggwave pin" >&2; return 2; }
            git -C "${REPO}/vendor/ggwave" cat-file -e "${commit}:${path}" 2>/dev/null || return 3
            git -C "${REPO}/vendor/ggwave" show "${commit}:${path}" ;;
        meshcore)
            commit="$(pin_meshcore)"
            [ -n "${commit}" ] || { echo "gen_notices.sh: no MeshCore pin" >&2; return 2; }
            git -C "${REPO}/vendor/RIFT" cat-file -e "${commit}:${path}" 2>/dev/null || return 3
            git -C "${REPO}/vendor/RIFT" show "${commit}:${path}" ;;
        arduinolibs)
            commit="$(pin_arduinolibs)"
            [ -n "${commit}" ] || { echo "gen_notices.sh: no Crypto pin" >&2; return 2; }
            git -C "${REPO}/vendor/Crypto" cat-file -e "${commit}:${path}" 2>/dev/null || return 3
            git -C "${REPO}/vendor/Crypto" show "${commit}:${path}" ;;
        lvgl)
            commit="$(pin_lvgl)"
            [ -n "${commit}" ] || { echo "gen_notices.sh: no LVGL pin (platforms/k230/vendor_lvgl_commit.txt)" >&2; return 2; }
            tarball="${SDK:+${SDK}/dl/lvgl/lvgl-${commit}.tar.gz}"
            if [ -n "${tarball}" ] && [ -f "${tarball}" ]; then
                top="$(tar -tzf "${tarball}" | head -1 | cut -d/ -f1)"
                tar -xOzf "${tarball}" "${top}/${path}"
            elif git -C "${REPO}/vendor/lvgl" cat-file -e "${commit}:${path}" 2>/dev/null; then
                git -C "${REPO}/vendor/lvgl" show "${commit}:${path}"
            else
                return 3
            fi ;;
        lv_port_linux)
            commit="$(commit_in "$2")"
            tarball="${SDK:+${SDK}/dl/lvgl/${commit}.tar.gz}"
            [ -n "${tarball}" ] && [ -f "${tarball}" ] || return 3
            top="$(tar -tzf "${tarball}" | head -1 | cut -d/ -f1)"
            tar -xOzf "${tarball}" "${top}/${path}" ;;
        *) echo "gen_notices.sh: unknown origin '${origin}'" >&2; return 2 ;;
    esac
}

generate() { # <output file>
    local n=0 line id component version licence where text
    {
        cat <<'EOF'
Doors third-party notices
=========================

Doors itself (called PocketOS through v0.0.9; its source files name
"PocketOS authors") is licensed under the Apache License, Version 2.0. The
licence is in LICENSE and Doors' notices in NOTICE, installed beside this
file in /usr/share/doors.

The components below are other people's work and are not covered by Doors'
licence. Doors binaries contain them, or load libraries that contain them,
and each is used under its own terms, reproduced in full below. The last
entries are the code and data Vision's R0 detector model was trained with:
none of them is in the image, and they are credited as their terms ask. The
model itself is listed in MODEL_LICENSES.md in the project repository.

Not reproduced here: the other libraries Doors and LVGL load - cJSON,
libgpiod, alsa-lib, libdrm, libevdev, libcurl, OpenSSL, libjpeg, libpng,
FreeType and FFmpeg - are separate Buildroot packages whose licences and
sources Buildroot's `make legal-info` collects. The C and C++ runtime
libraries come with the external toolchain, which legal-info does not
collect, so their licences are reproduced below with where their source is.
Of the vendor's nncase runtime linked into pos-vision, the generic runtime is
Apache-2.0 (below); its K230 modules state no licence. See docs/LICENSING.md
and docs/licensing/APACHE_2_READINESS.md in the project repository.

Generated by tools/legal/gen_notices.sh from third_party/notices/SOURCES.
Do not edit by hand.

Components
----------
EOF
        while IFS= read -r line; do
            n=$((n + 1))
            component="$(field "${line}" 2)"; version="$(field "${line}" 3)"
            licence="$(field "${line}" 4)"; where="$(field "${line}" 5)"
            printf '\n%2d. %s\n    Version: %s\n    Licence: %s\n    In the image: %s\n' \
                "${n}" "${component}" "${version}" "${licence}" "${where}"
        done < <(entries)
        n=0
        while IFS= read -r line; do
            n=$((n + 1))
            id="$(field "${line}" 1)"; component="$(field "${line}" 2)"
            text="$(text_file "${id}" "$(field "${line}" 6)")"
            [ -f "${text}" ] || { echo "gen_notices.sh: missing text ${text}" >&2; exit 1; }
            printf '\n\n'
            printf '%s\n' "================================================================================"
            printf '%2d. %s\n' "${n}" "${component}"
            printf '%s\n\n' "================================================================================"
            cat "${text}"
            # A text without a final newline must not run into the next header.
            [ -z "$(tail -c 1 "${text}")" ] || printf '\n'
        done < <(entries)
    } > "$1"
}

# The Buildroot hash file for the package's licence files (Buildroot manual,
# "The .hash file"). legal-info refuses a LICENSE, NOTICE or
# THIRD_PARTY_NOTICES.txt that does not match it; without a line it collects
# the file unchecked and warns.
hash_file() { # <notices file>
    local f
    printf '# Locally computed by tools/legal/gen_notices.sh, which writes it together\n'
    printf '# with THIRD_PARTY_NOTICES.txt. Do not edit by hand.\n'
    for f in ${OWN_FILES}; do
        [ -f "${REPO}/${f}" ] || { echo "gen_notices.sh: no ${f} at the repository root" >&2; return 1; }
        printf 'sha256  %s  %s\n' "$(sha256sum < "${REPO}/${f}" | cut -d' ' -f1)" "${f}"
    done
    printf 'sha256  %s  THIRD_PARTY_NOTICES.txt\n' "$(sha256sum < "$1" | cut -d' ' -f1)"
}

case "${MODE}" in
    generate)
        generate "${OUT}"
        hash_file "${OUT}" > "${HASH}"
        echo "wrote ${OUT#"${REPO}"/} and ${HASH#"${REPO}"/} (with ${OWN_FILES})"
        ;;
    check)
        tmp="$(mktemp)"; trap 'rm -f "${tmp}"' EXIT
        generate "${tmp}"
        bad=0
        if cmp -s "${tmp}" "${OUT}"; then
            echo "ok   THIRD_PARTY_NOTICES.txt is current"
        else
            echo "FAIL THIRD_PARTY_NOTICES.txt is not what third_party/notices produces; run tools/legal/gen_notices.sh"
            bad=1
        fi
        if [ -f "${HASH}" ] && cmp -s "${HASH}" <(hash_file "${OUT}"); then
            echo "ok   pocketos.hash holds the sha256 of ${OWN_FILES} and THIRD_PARTY_NOTICES.txt"
        else
            echo "FAIL ${HASH#"${REPO}"/} is missing or does not match ${OWN_FILES} and THIRD_PARTY_NOTICES.txt; run tools/legal/gen_notices.sh"
            bad=1
        fi
        exit "${bad}"
        ;;
    verify-upstream|refresh)
        bad=0; unverified=0
        while IFS= read -r line; do
            id="$(field "${line}" 1)"; spec="$(field "${line}" 6)"; version="$(field "${line}" 3)"
            case "${spec}" in repo:*|text:*) continue ;; esac
            case "${spec%%:*}" in
                radiolib) pinned="$(pin_radiolib)" ;;
                ggwave) pinned="$(pin_ggwave)" ;;
                meshcore) pinned="$(pin_meshcore)" ;;
                arduinolibs) pinned="$(pin_arduinolibs)" ;;
                lvgl) pinned="$(pin_lvgl)" ;;
                *) pinned="" ;;
            esac
            if [ -n "${pinned}" ] && [ "$(commit_in "${version}")" != "${pinned}" ] &&
               ! printf '%s\n' "$(field "${line}" 3)" | grep -q "^as in"; then
                echo "FAIL ${id}: SOURCES names commit $(commit_in "${version}"), the build pins ${pinned}"
                bad=1; continue
            fi
            # lv_port_linux is pinned by the vendor LVGL package, not by us.
            if [ "${spec%%:*}" = "lv_port_linux" ] && [ -n "${SDK}" ] &&
               ! grep -q "$(commit_in "${version}")" "${SDK}/buildroot-overlay/package/lvgl/lvgl.mk" 2>/dev/null; then
                echo "FAIL ${id}: the SDK's LVGL package no longer downloads lv_port_linux $(commit_in "${version}")"
                bad=1; continue
            fi
            tmp="$(mktemp)"
            if upstream "${spec}" "${version}" > "${tmp}"; then
                if [ "${MODE}" = "refresh" ]; then
                    cp "${tmp}" "${NOTICES}/texts/${id}.txt"
                    echo "refreshed texts/${id}.txt from ${spec}"
                elif cmp -s "${tmp}" "${NOTICES}/texts/${id}.txt"; then
                    echo "ok   ${id}: texts/${id}.txt is byte-identical to ${spec} at the pinned commit"
                else
                    echo "FAIL ${id}: texts/${id}.txt differs from ${spec} at the pinned commit"
                    bad=1
                fi
            else
                echo "note ${id}: ${spec} cannot be read here (no pinned upstream source available)"
                unverified=$((unverified + 1))
            fi
            rm -f "${tmp}"
        done < <(entries)
        if [ "${MODE}" = "verify-upstream" ] && [ "${STRICT}" -eq 1 ] && [ "${unverified}" -gt 0 ]; then
            echo "FAIL ${unverified} text(s) could not be verified against their upstream source"
            bad=1
        fi
        exit "${bad}"
        ;;
    verify-lvconf)
        [ -f "${LVCONF}" ] || { echo "FAIL no LVGL configuration at ${LVCONF}"; exit 1; }
        on() { grep -q -E "^[[:space:]]*#[[:space:]]*define[[:space:]]+$1[[:space:]]+(1|LV_STDLIB_BUILTIN)\b" "${LVCONF}"; }
        bad=0
        # Bundled code that brings a licence of its own, and the entry that covers it.
        for pair in LV_USE_LODEPNG:lodepng LV_USE_TJPGD:tjpgd LV_USE_THORVG_INTERNAL:thorvg \
                    'LV_FONT_MONTSERRAT_[0-9]+(_COMPRESSED)?:montserrat' \
                    'LV_FONT_MONTSERRAT_[0-9]+(_COMPRESSED)?:font-awesome-5' \
                    LV_FONT_DEJAVU_16_PERSIAN_HEBREW:dejavu-sans \
                    'LV_FONT_SOURCE_HAN_SANS_SC_1[46]_CJK:source-han-sans-sc' LV_FONT_UNSCII_8:unscii-8 \
                    LV_FONT_UNSCII_16:none \
                    LV_USE_GIF:none LV_USE_QRCODE:none LV_USE_BARCODE:none LV_USE_LZ4_INTERNAL:none \
                    LV_USE_TINY_TTF:none LV_USE_XML:none LV_USE_FROGFS:none LV_USE_FT81X:none \
                    LV_USE_STDLIB_MALLOC:none LV_USE_STDLIB_SPRINTF:none LV_USE_DRAW_VG_LITE:none; do
            flag="${pair%%:*}"; entry="${pair##*:}"
            if on "${flag}"; then
                if [ "${entry}" = "none" ] || ! entries | awk -F ' [|] ' '{print $1}' | trim | grep -qx "${entry}"; then
                    echo "FAIL the vendor LVGL configuration enables ${flag}, which bundles code with its own licence and has no entry in third_party/notices/SOURCES"
                    bad=1
                else
                    echo "ok   ${flag} is enabled and covered by ${entry}"
                fi
            fi
        done
        exit "${bad}"
        ;;
esac
