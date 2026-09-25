#!/bin/bash
# pos-zabbix over a real socket (docs/apps/ZABBIX.md "Validation"): the
# helper's libcurl transport against pos-zabbix-mock, which serves the fake
# server's answers over HTTP and, where this host has OpenSSL, over HTTPS.
# The configuration rules and the secret file are checked whatever the
# build; the socket half only when pos-zabbix has a transport ("pos-zabbix
# transport" says curl) and is SKIPPED, loudly, otherwise - a host without
# libcurl headers builds the helper with the fake alone.
#
# Requires: tools/zabbix/pos-zabbix and tools/zabbix/pos-zabbix-mock (make
# test); openssl(1) for the HTTPS half.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
skip() { echo "SKIP $1"; }

H=$(pwd)/tools/zabbix/pos-zabbix
M=$(pwd)/tools/zabbix/pos-zabbix-mock
if [ ! -x "$H" ] || [ ! -x "$M" ]; then
    echo "FAIL build them first: make tools/zabbix/pos-zabbix tools/zabbix/pos-zabbix-mock"
    exit 1
fi
T=$(mktemp -d)
MOCK=""
cleanup() { [ -n "$MOCK" ] && kill "$MOCK" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
export POCKETOS_LOG_DIR="$T/log" POCKETOS_LOG_STDERR=0 POCKETOS_STATE_DIR="$T/state" \
       POCKETOS_CONFIG_DIR="$T/etc"
unset POCKETOS_ZABBIX_BACKEND POCKETOS_ZABBIX_FAKE
mkdir -p "$T/etc"
TOKEN=doors-test-token-5f2a8c01d3e94b7a
conf() { printf '%s\n' "$@" > "$T/etc/zabbix.conf"; }
state_of() { "$H" check 2>/dev/null | grep '^state' | tail -1 | cut -f2,5; }

# ---- configuration and secret, any build ------------------------------------------
conf "url=http://127.0.0.1:1/zabbix/"
out=$("$H" check 2>&1); rc=$?
check "http:// is refused without allow_insecure_http (exit 2, says why)" \
    "$([ $rc = 2 ] && printf '%s' "$out" | grep -q unencrypted && echo 1 || echo 0)"
conf "url=https://127.0.0.1:1/zabbix/" "token=abc"
out=$("$H" check 2>&1); rc=$?
check "a token in zabbix.conf is refused" \
    "$([ $rc = 2 ] && printf '%s' "$out" | grep -q set-secret && echo 1 || echo 0)"
conf "url=https://127.0.0.1:1/zabbix/"
out=$("$H" check 2>&1); rc=$?
check "no secret stored: not configured, and it says how" \
    "$([ $rc = 2 ] && printf '%s' "$out" | grep -q 'pos-zabbix set-secret' && echo 1 || echo 0)"
printf '%s\n' "$TOKEN" | "$H" set-secret token 2>/dev/null
check "set-secret stores the token 0600 under a 0700 directory" \
    "$([ "$(stat -c %a "$T/state/zabbix/secret")" = 600 ] && [ "$(stat -c %a "$T/state/zabbix")" = 700 ] &&
       grep -qx "token=$TOKEN" "$T/state/zabbix/secret" && echo 1 || echo 0)"
chmod 0644 "$T/state/zabbix/secret"
out=$("$H" check 2>&1); rc=$?
check "a secret file others may read is refused" \
    "$([ $rc = 2 ] && printf '%s' "$out" | grep -q 'chmod 600' && echo 1 || echo 0)"
chmod 0600 "$T/state/zabbix/secret"
out=$(printf 'has space\n' | "$H" set-secret token 2>&1); rc=$?
check "a token with a space is refused, the stored one kept" \
    "$([ $rc = 2 ] && grep -qx "token=$TOKEN" "$T/state/zabbix/secret" && echo 1 || echo 0)"

# The fake needs no transport: the whole session path, any build.
out=$("$H" check --fake demo 2>/dev/null); rc=$?
check "the fake answers through the helper" \
    "$([ $rc = 0 ] && printf '%s\n' "$out" | grep -q '^pend' && echo 1 || echo 0)"

if [ "$("$H" transport)" != "curl" ]; then
    conf "url=https://127.0.0.1:1/zabbix/"
    out=$(printf 'quit\n' | "$H" session 2>/dev/null)
    check "without a transport a live configuration says so, and nothing is tried" \
        "$(printf '%s\n' "$out" | grep -q 'no HTTP client' && echo 1 || echo 0)"
    skip "HTTP and HTTPS: this pos-zabbix has no libcurl (make ZABBIX_CURL=1, docs/apps/ZABBIX.md)"
    echo "zabbix_http_test: $failed failure(s) (socket half skipped)"
    exit $((failed > 0))
fi

# ---- HTTP -------------------------------------------------------------------------
start_mock() { # [--tls cert key]
    rm -f "$T/port"
    "$M" --port 0 --port-file "$T/port" --scenario demo "$@" 2>"$T/mock.log" &
    MOCK=$!
    for _ in $(seq 50); do [ -s "$T/port" ] && break; sleep 0.1; done
    PORT=$(cat "$T/port" 2>/dev/null)
}
stop_mock() { kill "$MOCK" 2>/dev/null; wait "$MOCK" 2>/dev/null; MOCK=""; }
scenario() { # name, over whichever scheme is running
    curl -s -k "$SCHEME://127.0.0.1:$PORT/__mock/scenario/$1" >/dev/null 2>&1 ||
        "$H" check --fake demo >/dev/null 2>&1 # no curl(1): nothing better to do
}

start_mock
SCHEME=http
check "the mock listens" "$([ -n "$PORT" ] && echo 1 || echo 0)"
conf "url=http://127.0.0.1:$PORT/zabbix/" "label=Mock" "allow_insecure_http=1" "timeout_s=3"
out=$("$H" check 2>/dev/null); rc=$?
check "HTTP: online, the demo's nine problems and 36 hosts" \
    "$([ $rc = 0 ] && printf '%s\n' "$out" | grep -q $'^pend\t.*\t9$' &&
       printf '%s\n' "$out" | grep -q $'^hend\t.*\t36$' && echo 1 || echo 0)"
check "HTTP: the server's clock from the Date header" \
    "$(printf '%s\n' "$out" | awk -F'\t' -v now="$(date +%s)" '/^pbegin/ { d = $3 - now; print (d > -60 && d < 60) ? 1 : 0 }')"
if command -v curl >/dev/null 2>&1; then
    for pair in auth:authfail/auth expired:authfail/auth http500:retrying/http malformed:retrying/malformed \
                apierror:retrying/api huge:retrying/toolarge refused:retrying/connect old:online/none \
                v74:online/none demo:online/none; do
        sc=${pair%%:*}; want=${pair#*:}
        scenario "$sc"
        got=$(state_of | tr '\t' /)
        check "HTTP $sc: $want" "$([ "$got" = "$want" ] && echo 1 || echo 0)"
        [ "$got" = "$want" ] || echo "     got $got"
    done
    scenario timeout
    t0=$(date +%s)
    got=$(state_of | tr '\t' /)
    t1=$(date +%s)
    check "HTTP timeout: retrying/timeout after about timeout_s (3 s)" \
        "$([ "$got" = retrying/timeout ] && [ $((t1 - t0)) -ge 2 ] && [ $((t1 - t0)) -le 8 ] && echo 1 || echo 0)"
    scenario demo
else
    skip "HTTP error scenarios: no curl(1) to switch the mock's scenario"
fi
stop_mock
conf "url=http://127.0.0.1:1/zabbix/" "allow_insecure_http=1" "timeout_s=3"
check "HTTP: nothing listening is a connection failure" \
    "$([ "$(state_of | tr '\t' /)" = retrying/connect ] && echo 1 || echo 0)"
conf "url=http://doors-zabbix-test.invalid/zabbix/" "allow_insecure_http=1" "timeout_s=3"
check "HTTP: a name that does not resolve is a DNS failure" \
    "$([ "$(state_of | tr '\t' /)" = retrying/dns ] && echo 1 || echo 0)"

# ---- a user and password: user.login, a renewed session, user.logout -------------
# A password with spaces at both ends, UTF-8, '=' and '#': kept byte for byte.
PASSWORD=' Doors pässord = #7 '
if command -v curl >/dev/null 2>&1; then
    start_mock
    SCHEME=http
    stat_of() {
        curl -s "http://127.0.0.1:$PORT/__mock/stats" |
            awk -v k="$1" '{ for (i = 1; i < NF; i++) if ($i == k) print $(i + 1) }'
    }
    wait_open0() { for _ in $(seq 60); do [ "$(stat_of open)" = 0 ] && return 0; sleep 0.1; done; return 1; }
    conf "url=http://127.0.0.1:$PORT/zabbix/" "allow_insecure_http=1" "timeout_s=3" "auth=password" "user=demo"
    printf '%s\n' "$PASSWORD" | "$H" set-secret password 2>/dev/null
    check "set-secret keeps the password byte for byte" \
        "$([ "$(cat "$T/state/zabbix/secret")" = "password=$PASSWORD" ] && echo 1 || echo 0)"
    check "zabbix.conf holds no password" "$(grep -qF "$PASSWORD" "$T/etc/zabbix.conf" && echo 0 || echo 1)"
    out=$("$H" check 2>/dev/null); rc=$?
    check "password: online with the data" \
        "$([ $rc = 0 ] && printf '%s\n' "$out" | grep -q $'^pend\t.*\t9$' && echo 1 || echo 0)"
    check "password check: one login, logged out again (no session left open)" \
        "$([ "$(stat_of logins)" = 1 ] && [ "$(stat_of logouts)" = 1 ] && [ "$(stat_of open)" = 0 ] &&
           echo 1 || echo 0)"

    # The session helper as the app runs it: the password is not on its
    # command line or in its environment, and it logs out after quit.
    mkfifo "$T/in"
    "$H" session <"$T/in" >"$T/session.out" 2>/dev/null &
    SPID=$!
    exec 9>"$T/in"
    for _ in $(seq 50); do grep -q $'^state\tonline' "$T/session.out" 2>/dev/null && break; sleep 0.1; done
    check "password session: online" "$(grep -q $'^state\tonline' "$T/session.out" && echo 1 || echo 0)"
    check "the password is not on the helper's command line or in its environment" \
        "$(cat "/proc/$SPID/cmdline" "/proc/$SPID/environ" 2>/dev/null | tr '\0' '\n' | grep -qF "$PASSWORD" &&
           echo 0 || echo 1)"
    printf 'quit\n' >&9
    exec 9>&-
    wait "$SPID"
    check "password session: logged out after quit (by the helper's detached child)" \
        "$(wait_open0 && [ "$(stat_of logouts)" = 2 ] && echo 1 || echo 0)"

    # Sessions the server ends every few requests: logged in again, silently.
    scenario short
    before=$(stat_of logins)
    out=$( { sleep 1; printf 'refresh\n'; sleep 1.5; printf 'quit\n'; } | "$H" session 2>/dev/null)
    check "short sessions: renewed without going offline" \
        "$([ "$(stat_of logins)" -ge $((before + 2)) ] && [ "$(printf '%s\n' "$out" | grep -c '^pend')" -ge 2 ] &&
           ! printf '%s\n' "$out" | grep -qE $'^state\t(retrying|authfail)' && echo 1 || echo 0)"
    check "short sessions: none left open" "$(wait_open0 && echo 1 || echo 0)"
    scenario demo

    # A refused password is tried once and not again on its own (account lockout).
    before=$(stat_of failed_logins)
    conf "url=http://127.0.0.1:$PORT/zabbix/" "allow_insecure_http=1" "timeout_s=3" "auth=password" "user=nobody"
    got=$(state_of | tr '\t' /)
    check "a refused password: authfail/auth, one failed login" \
        "$([ "$got" = authfail/auth ] && [ "$(stat_of failed_logins)" = $((before + 1)) ] && echo 1 || echo 0)"
    out=$( { sleep 2; printf 'quit\n'; } | "$H" session 2>/dev/null)
    check "a refused password in a session: no retry countdown, one failed login" \
        "$(printf '%s\n' "$out" | grep -qE $'^state\tauthfail\t[0-9]+\t0\t' &&
           [ "$(stat_of failed_logins)" = $((before + 2)) ] && echo 1 || echo 0)"
    # tools/zabbix/try-server.sh, the real-server trial (§10.1), against the mock.
    before=$(stat_of logouts)
    out=$(printf '%s\n' "$PASSWORD" | TMPDIR="$T" tools/zabbix/try-server.sh "http://127.0.0.1:$PORT/zabbix/" demo \
          allow_insecure_http=1 2>&1); rc=$?
    check "try-server.sh: one round online, logged out, its private directory removed" \
        "$([ $rc = 0 ] && printf '%s\n' "$out" | grep -q 'try-server: ONLINE' &&
           printf '%s\n' "$out" | grep -q '^problems fetched: 9' && [ "$(stat_of logouts)" = $((before + 1)) ] &&
           ! ls -d "$T"/doors-zabbix.* >/dev/null 2>&1 && echo 1 || echo 0)"
    check "try-server.sh never prints the password" "$(printf '%s\n' "$out" | grep -qF "$PASSWORD" && echo 0 || echo 1)"
    out=$(printf 'wrong\n' | TMPDIR="$T" tools/zabbix/try-server.sh "http://127.0.0.1:$PORT/zabbix/" nobody \
          allow_insecure_http=1 2>&1); rc=$?
    check "try-server.sh: a refused login says so, exit 1" \
        "$([ $rc = 1 ] && printf '%s\n' "$out" | grep -q $'^state\tauthfail' && echo 1 || echo 0)"
    stop_mock
    check "the password is in no log" "$(grep -rqsF "$PASSWORD" "$T/log" && echo 0 || echo 1)"
else
    skip "user and password: no curl(1) to read the mock's session counts"
fi
printf '%s\n' "$TOKEN" | "$H" set-secret token 2>/dev/null

# ---- HTTPS ------------------------------------------------------------------------
if ! command -v openssl >/dev/null 2>&1; then
    skip "HTTPS: no openssl(1) to make a certificate"
elif "$M" --port 0 --tls /dev/null /dev/null 2>&1 | grep -q 'built without TLS'; then
    skip "HTTPS: the mock was built without OpenSSL headers"
else
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 2 \
        -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >/dev/null 2>&1
    start_mock --tls "$T/cert.pem" "$T/key.pem"
    SCHEME=https
    conf "url=https://127.0.0.1:$PORT/zabbix" "timeout_s=3"
    got=$(state_of | tr '\t' /)
    check "HTTPS: an untrusted certificate is refused (tls)" "$([ "$got" = retrying/tls ] && echo 1 || echo 0)"
    conf "url=https://127.0.0.1:$PORT/zabbix" "timeout_s=3" "ca_file=$T/cert.pem"
    out=$("$H" check 2>/dev/null); rc=$?
    check "HTTPS: trusted through ca_file, online with the data" \
        "$([ $rc = 0 ] && printf '%s\n' "$out" | grep -q $'^pend\t.*\t9$' && echo 1 || echo 0)"
    conf "url=https://localhost:$PORT/zabbix" "timeout_s=3" "ca_file=$T/cert.pem"
    got=$(state_of | tr '\t' /)
    check "HTTPS: a certificate for another name is refused (tls)" \
        "$([ "$got" = retrying/tls ] && echo 1 || echo 0)"
    conf "url=https://127.0.0.1:$PORT/zabbix" "timeout_s=3" "verify_tls=0"
    got=$(state_of | tr '\t' /)
    check "HTTPS: verify_tls=0 takes it anyway (and STATUS says so)" \
        "$([ "$got" = online/none ] && echo 1 || echo 0)"
    stop_mock
fi

# ---- the token stays secret ----------------------------------------------------------
check "the token is in no log" "$(grep -rqs "$TOKEN" "$T/log" && echo 0 || echo 1)"
check "and the helper never prints it" \
    "$("$H" check --fake demo 2>&1 | grep -q "$TOKEN" && echo 0 || echo 1)"

echo "zabbix_http_test: $failed failure(s)"
exit $((failed > 0))
