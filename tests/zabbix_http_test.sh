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
