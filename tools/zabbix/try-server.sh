#!/bin/bash
# One read-only round against a real Zabbix server from the development host
# (docs/apps/ZABBIX.md §10.1), with a user and password or an API token.
#
#   tools/zabbix/try-server.sh URL USER [KEY=VALUE...]     user and password
#   tools/zabbix/try-server.sh URL --token [KEY=VALUE...]  API token
#
# KEY=VALUE lines are added to the trial zabbix.conf (ca_file=..., timeout_s=15,
# allow_insecure_http=1 for the mock). The secret is asked for, never echoed,
# and handed to `pos-zabbix set-secret` through a pipe from a shell builtin:
# it is never an argument of any process, never in the environment, never in
# the shell history, and never in this repository. It lives in a private 0700
# directory that is removed on exit, and the check logs out after itself.
#
# One run is one login. A wrong password counts towards Zabbix's lockout
# (5 failures by default): stop and check it rather than re-running in a loop.
#
# Requires: tools/zabbix/pos-zabbix built with libcurl (make ZABBIX_CURL=1 ...).
set -u
cd "$(dirname "$0")/../.." || exit 1
H=$(pwd)/tools/zabbix/pos-zabbix

if [ $# -lt 2 ]; then
    sed -n '5,6p' "$0" | sed 's/^# */usage: /' >&2
    exit 2
fi
if [ ! -x "$H" ] || [ "$("$H" transport)" != curl ]; then
    echo "try-server: build pos-zabbix with libcurl first (docs/apps/ZABBIX.md §10.1)" >&2
    exit 2
fi
URL=$1
WHO=$2
shift 2

W=$(mktemp -d "${TMPDIR:-/tmp}/doors-zabbix.XXXXXX") || exit 2
chmod 700 "$W"
export POCKETOS_CONFIG_DIR="$W/etc" POCKETOS_STATE_DIR="$W/state" POCKETOS_LOG_DIR="$W/log" \
       POCKETOS_LOG_STDERR=0
unset POCKETOS_ZABBIX_BACKEND POCKETOS_ZABBIX_FAKE
cleanup() {
    "$H" clear-secret >/dev/null 2>&1
    rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir -p "$W/etc"

{
    printf 'url=%s\nlabel=Trial\ntimeout_s=15\n' "$URL"
    if [ "$WHO" = --token ]; then
        printf 'auth=token\n'
    else
        printf 'auth=password\nuser=%s\n' "$WHO"
    fi
    for kv in "$@"; do printf '%s\n' "$kv"; done
} >"$W/etc/zabbix.conf"

if [ "$WHO" = --token ]; then
    kind=token
    prompt='Zabbix API token (not shown): '
else
    kind=password
    prompt="Zabbix password for $WHO (not shown): "
fi
if [ -t 0 ]; then
    IFS= read -rs -p "$prompt" SECRET
    echo >&2
else
    IFS= read -r SECRET # a pipe, for the test against the mock
fi
printf '%s\n' "$SECRET" | "$H" set-secret "$kind" >/dev/null 2>"$W/err"
rc=$?
SECRET=
unset SECRET
if [ $rc != 0 ]; then
    cat "$W/err" >&2
    exit 2
fi

echo "== one round against $URL"
"$H" check >"$W/out" 2>"$W/err"
rc=$?
cat "$W/err" >&2
grep -E '^(version|state)' "$W/out"
awk -F'\t' '$1 == "pbegin" { print "open problems:", $4, "(unacknowledged " $7 ")" }
            $1 == "pend" { print "problems fetched:", $3 } $1 == "hend" { print "hosts fetched:", $3 }' "$W/out"
echo "== the ten newest problems (severity 0-5, host, name)"
awk -F'\t' '$1 == "pb" { print "  " $5 "  " $9 "  " $10 }' "$W/out" | head -10
echo "== the helper's log (it never holds the secret)"
cat "$W/log/"*.log 2>/dev/null | sed 's/^/  /'
if [ $rc = 0 ]; then
    echo "try-server: ONLINE"
else
    echo "try-server: NOT ONLINE (exit $rc); see the state line above"
fi
exit $rc
