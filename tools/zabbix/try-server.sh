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
# It then checks its own run: the secret is looked for (through a pipe, never
# as an argument) in the helper's command line and environment while it runs,
# and in the log, the configuration and the output afterwards; it says
# whether user.logout succeeded and whether the private directory is gone.
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
# SECRET is a plain shell variable, never exported; it is kept only for the
# leak checks below and cleared before the result is printed.
printf '%s\n' "$SECRET" | "$H" set-secret "$kind" >/dev/null 2>"$W/err"
rc=$?
if [ $rc != 0 ]; then
    SECRET=
    cat "$W/err" >&2
    exit 2
fi
# Is the secret in any of these files? The pattern goes to grep through a
# pipe from a builtin, so it is never on a command line either.
has_secret() { [ -n "$SECRET" ] && grep -qsF -f <(printf '%s\n' "$SECRET") "$@"; }

echo "== one round against $URL"
"$H" check >"$W/out" 2>"$W/err" &
pid=$!
n=0
while kill -0 "$pid" 2>/dev/null; do
    # The helper may exit between the two reads: the redirect then fails,
    # which is not worth a message.
    { tr '\0' '\n' <"/proc/$pid/cmdline" >"$W/proc.$n"; } 2>/dev/null
    { tr '\0' '\n' <"/proc/$pid/environ" >>"$W/proc.$n"; } 2>/dev/null
    n=$((n + 1))
    sleep 0.05
done
wait "$pid"
rc=$?
leak=""
has_secret "$W"/proc.* && leak="$leak helper-cmdline/environ"
has_secret "$W/log"/* && leak="$leak log"
has_secret "$W/etc/zabbix.conf" && leak="$leak zabbix.conf"
has_secret "$W/out" "$W/err" && leak="$leak output"
SECRET=
unset SECRET

cat "$W/err" >&2
grep -E '^(version|state)' "$W/out"
awk -F'\t' '
    $1 == "pbegin" {
        print "open problems:", $4, ($5 == 1 ? "(exact)" : "(at least)"), "unacknowledged", $7
        print "by severity (not classified, information, warning, average, high, disaster):",
              $8, $9, $10, $11, $12, $13, ($6 == 1 ? "(exact)" : "(over those fetched)")
    }
    $1 == "pend" { print "problems fetched:", $3 }
    $1 == "hbegin" {
        print "monitored hosts:", $4, ($5 == 1 ? "(exact)" : "(at least)")
        print "availability over", $6, "hosts: down", $7, ($10 == 1 ? "(exact)" : "(over those fetched)"),
              "unknown", $8, "in maintenance", $9
    }
    $1 == "ho" { kept++; if ($3 == 1) up++; else if ($3 == 2) down++; else unk++ }
    $1 == "hend" { printf "hosts kept: %d (up %d, down %d, unknown %d)\n", $3, up, down, unk }' "$W/out"
echo "== the ten newest problems (severity 0-5, host, name)"
awk -F'\t' '$1 == "pb" { print "  " $5 "  " $9 "  " $10 }' "$W/out" | head -10
echo "== the helper's log (it never holds the secret)"
cat "$W/log/"*.log 2>/dev/null | sed 's/^/  /'
echo "== checks"
if grep -qs 'zabbix: logged out' "$W/log/"*.log; then
    echo "user.logout: OK"
elif grep -qs 'logout failed' "$W/log/"*.log; then
    echo "user.logout: FAILED (the session ends by itself on the server)"
else
    echo "user.logout: not needed (no session: a token, or no login)"
fi
echo "helper samples taken while it ran: $n"
if [ -n "$leak" ]; then
    echo "secret found in:$leak  <-- LEAK"
else
    echo "secret found in: nothing (helper cmdline/environ, log, zabbix.conf, output)"
fi
cleanup
if [ -e "$W" ]; then echo "private directory: STILL PRESENT ($W)"; else echo "private directory: removed"; fi
if [ $rc = 0 ] && [ -z "$leak" ]; then
    echo "try-server: ONLINE"
elif [ $rc = 0 ]; then
    echo "try-server: ONLINE, but the secret leaked"
    rc=3
else
    echo "try-server: NOT ONLINE (exit $rc); see the state line above"
fi
exit $rc
