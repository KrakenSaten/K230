#!/usr/bin/env bash
# Does this sshd_config let root sign in with a password?
#
# Usage: sshd_root_login.sh <sshd_config>
# Prints one word and exits 0 for "hardened", 1 otherwise:
#
#   hardened  at least one PermitRootLogin before the first Match, and every
#             one in the file is prohibit-password, without-password,
#             forced-commands-only or no
#   open      any other PermitRootLogin, or none before the first Match (the
#             compiled-in default is not relied on)
#   unknown   an Include line (the included files are not read), or the file
#             cannot be read
#
# The same rule as sysd's (services/sysd/sysd_companion.c, judged on the unit
# before a device password or the Companion gateway; ADR-019 amendment 2), for
# the image gate (verify_image.sh): an image must not ship an sshd that lets
# root in by password, because the device password that Companion needs would
# then open root over SSH to the whole network (issue #172).
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -u
f="${1:-}"
if [ -z "${f}" ] || [ ! -r "${f}" ]; then
    echo unknown
    exit 1
fi
# A NUL byte: what awk reads is not what sshd reads. Unknown, as sysd says.
if [ "$(tr -d '\000' < "${f}" | wc -c)" != "$(wc -c < "${f}")" ]; then
    echo unknown
    exit 1
fi
verdict=$(tr -d '\r' < "${f}" | awk '
{
    line = $0
    sub(/^[ \t]+/, "", line)
    if (line == "" || substr(line, 1, 1) == "#") next
    # What sshd may read otherwise than this does: a line with no keyword
    # before its "=", or a quoted keyword. Judged unknown, as sysd does.
    if (substr(line, 1, 1) == "=") { odd = 1; next }
    key = line
    sub(/[ \t=].*$/, "", key)
    val = substr(line, length(key) + 1)
    key = tolower(key)
    if (key ~ /["\047]/) odd = 1
    sub(/^[ \t]*=?[ \t]*/, "", val)
    gsub(/"/, "", val)
    sub(/[ \t#].*$/, "", val)
    val = tolower(val)
    if (key == "include") inc = 1
    else if (key == "match") inmatch = 1
    else if (key == "permitrootlogin") {
        if (val != "prohibit-password" && val != "without-password" && val != "forced-commands-only" && val != "no") bad = 1
        if (!inmatch) global = 1
    }
}
END {
    if (inc || odd) print "unknown"
    else if (bad || !global) print "open"
    else print "hardened"
}')
echo "${verdict}"
[ "${verdict}" = hardened ]
