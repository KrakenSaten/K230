#!/usr/bin/env bash
# gatewayd lint: the rules of ADR-019 that are facts about the source text.
#
#   1. No bypass of peer verification anywhere in the service (rule 13).
#   2. TLS 1.3 only; no resumption (rule 10).
#   3. OpenSSL is called from one file.
#   4. The allowlist holds nothing that changes the unit and nothing of the
#      shell (rules 6 and 25).
#   5. No log line carries a token, a name, a key or request text (rule 9).
#   6. The service links nothing of the radio, the mesh or packet capture.
#   7. The test hooks exist only in the hooks build.
#   8. The init script ships disabled and parses.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

cd "$(dirname "$0")/.."
D=services/gatewayd
failed=0
check() { # <what> <1|0>
    if [ "$2" = 1 ]; then echo "  ok   $1"; else echo "  FAIL $1"; failed=$((failed + 1)); fi
}
none() { # <what> <pattern> <files...>: no line matches
    local what=$1 pat=$2; shift 2
    local hits
    hits=$(grep -nE "$pat" "$@" 2>/dev/null | grep -vE '^\S+:[0-9]+:\s*(/\*|\*|//)' || true)
    if [ -z "$hits" ]; then echo "  ok   $what"; else echo "  FAIL $what"; echo "$hits" | sed 's/^/       /'; failed=$((failed + 1)); fi
}
SRC=$(ls $D/*.c $D/*.h)

echo "gatewayd lint"

# 1. Verification is never switched off and never decided by a constant.
none "no SSL_VERIFY_NONE" 'SSL_VERIFY_NONE' $SRC
none "no insecure switch read from the environment" 'getenv' $SRC
none "no option that names a bypass" '(insecure|no[-_]verify|skip[-_]verify|trust[-_]all|allow[-_]any)' $SRC
check "peer verification is required with a certificate" \
    "$(grep -q 'SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT' $D/gw_tls.c && echo 1 || echo 0)"
check "exactly one verification callback is installed" \
    "$([ "$(grep -c 'SSL_CTX_set_cert_verify_callback' $D/gw_tls.c)" = 1 ] && echo 1 || echo 0)"
# The callback returns 1 once, after the admit function said yes.
body=$(awk '/^static int verify_peer\(/,/^}/' $D/gw_tls.c)
check "the verification callback has one accepting return" \
    "$([ "$(echo "$body" | grep -c 'return 1;')" = 1 ] && echo 1 || echo 0)"
check "that return follows the admit decision" \
    "$(echo "$body" | awk '/admitted != 1/{seen=1} /return 1;/{print seen+0}' | grep -qx 1 && echo 1 || echo 0)"
check "the callback fails closed before anything else" \
    "$(echo "$body" | grep -q 'X509_V_ERR_APPLICATION_VERIFICATION' && echo 1 || echo 0)"

# 2. The protocol version and resumption.
check "minimum and maximum protocol are TLS 1.3" \
    "$(grep -q 'SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)' $D/gw_tls.c &&
       grep -q 'SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION)' $D/gw_tls.c && echo 1 || echo 0)"
none "no older protocol version is named" '(TLS1_VERSION|TLS1_1_VERSION|TLS1_2_VERSION|SSL3_VERSION|DTLS)' $SRC
check "no tickets and no session cache" \
    "$(grep -q 'SSL_CTX_set_num_tickets(ctx, 0)' $D/gw_tls.c && grep -q 'SSL_OP_NO_TICKET' $D/gw_tls.c &&
       grep -q 'SSL_SESS_CACHE_OFF' $D/gw_tls.c && echo 1 || echo 0)"
check "a finished handshake is checked for version and resumption" \
    "$(grep -q 'SSL_session_reused(ssl) != 0' $D/gw_tls.c && grep -q 'gw_tls_established_ok' $D/main.c && echo 1 || echo 0)"

# 3. One file talks to OpenSSL's certificate and key APIs.
others=$(grep -lE 'X509_|EVP_PKEY_|PEM_|SSL_CTX_' $D/*.c | grep -v gw_tls.c | grep -v main.c || true)
check "X509/EVP/PEM calls are in gw_tls.c only" "$([ -z "$others" ] && echo 1 || echo 0)"
check "main.c makes no trust decision of its own" \
    "$(grep -qE 'X509_|PEM_|SSL_CTX_set' $D/main.c && echo 0 || echo 1)"

# 4. The allowlist. Every method a client can name is a string in the table.
table=$(awk '/^static const struct gw_method METHODS\[\]/,/^};/' $D/gw_policy.c)
names=$(echo "$table" | grep -oE '\{"[a-z_.]+"' | tr -d '{"')
check "the method table was found" "$([ -n "$names" ] && echo 1 || echo 0)"
bad=$(echo "$names" | grep -E '^shell\.|reboot|poweroff|expand|eject|radio_setup|connect|forget|set_enabled|configure|\.send|set_name|set_path_hash|channel_add|channel_remove|node_remove|reset_path|advert|discover|remote_|app_send|pair_begin|pair_decide|revoke|set_scopes' || true)
check "no method that changes the unit, and none of the shell" "$([ -z "$bad" ] && echo 1 || echo 0)"
[ -n "$bad" ] && echo "$bad" | sed 's/^/       /'
none "no shell socket is opened in M2" '"shell"' $SRC
check "upstream calls pass no client parameter" \
    "$(grep -q 'pocketipc_call_timeout(u->fd, m->name, NULL,' $D/main.c && echo 1 || echo 0)"
check "upstream methods are called only by table name" \
    "$([ "$(grep -c 'pocketipc_call' $D/main.c)" = 1 ] && echo 1 || echo 0)"

# 4b. The mesh group (M4.1): reading only, by mesh_read only, and the
# gateway's own connection to meshcored asks for three things by name.
meshrows=$(echo "$table" | grep -E '\{"mesh\.' | grep -v '"mesh.status"')
check "the mesh rows were found" "$([ "$(echo "$meshrows" | grep -c .)" -ge 6 ] && echo 1 || echo 0)"
check "every mesh row but mesh.status needs mesh_read" \
    "$([ -z "$(echo "$meshrows" | grep -v 'GW_S_MESH_READ')" ] && echo 1 || echo 0)"
none "no sending in this milestone" '"mesh\.(send|resend|app_send)' $SRC
none "the receive log is never asked for" 'rx_log' $SRC
own=$(grep -oE 'gw_uplink_call\(G\.mesh, "[a-z_.]+"' $D/main.c | grep -oE '"[a-z_.]+"' | sort | tr -d '\n')
check "the gateway's own calls to meshcored are status, subscribe and messages" \
    "$([ "$own" = '"mesh.messages""mesh.status""mesh.subscribe"' ] && echo 1 || echo 0)"
check "every other call to meshcored is a table row by its name" \
    "$([ "$(grep -c 'gw_uplink_call(' $D/main.c)" = 5 ] && [ "$(grep -c 'gw_uplink_call(G.mesh, m->name,' $D/main.c)" = 2 ] && echo 1 || echo 0)"
check "what goes upstream with a client's request is the row's own parameter check" \
    "$(grep -q 'gw_uplink_call(G.mesh, m->name, up,' $D/main.c && grep -q 'm->params(params, &up)' $D/main.c && echo 1 || echo 0)"
passed=$(awk '/^cJSON \*gw_filter_event\(/,/^}/' $D/gw_policy.c | grep -oE '"mesh\.[a-z_]+"' | sort | tr -d '\n')
check "three events of meshcored are passed on by name and no other" \
    "$([ "$passed" = '"mesh.channel""mesh.node""mesh.state"' ] && echo 1 || echo 0)"
none "no field list of the mesh group names a key or a secret" \
    '^\s*"[a-z_", ]*"(key|secret|private_key|prv|psk|password|passphrase|token)"' $D/gw_policy.c
check "the journal is memory only: no file is written by it" \
    "$(grep -qE 'fopen|open\(|write\(|fwrite' $D/gw_journal.c $D/gw_feed.c && echo 0 || echo 1)"

# 5. Logs. No LOG_ line takes a %s fed by a token, a name, a URI, a method,
# text or key material; fingerprints are logged as an 8-byte prefix only.
logs=$(grep -nE 'LOG_(INFO|WARN|ERROR|DEBUG)\(' -A2 $D/*.c)
none "no token, uri, name, method or text in a log call" \
    'LOG_[A-Z]+\(.*(token|t64|uri|->name|pair\.name|method|valuestring|pem|passphrase)' $D/*.c
check "log calls exist to be checked" "$([ -n "$logs" ] && echo 1 || echo 0)"
none "full fingerprints are not logged" 'LOG_[A-Z]+\(.*GW_FP_HEX' $D/*.c
none "nothing is printed to stdout or stderr outside usage" '(^|[^a-z_])printf\(|fputs\(|[^a-z_]puts\(' $D/gw_*.c

# 6. Ownership: no radio, mesh or capture code, and no device node.
none "no include of capture, radio or mesh code" '#include.*(pocketpcap|RadioLib|meshcore|pcap|radiod|mesh_)' $SRC
none "no device node is opened" '"/dev/' $SRC

# 7. The hooks.
hooks=$(grep -n 'test-listen\|test-port\|test-shadow\|test-passwd' $D/main.c | wc -l)
guarded=$(awk '/#ifdef GATEWAYD_TEST_HOOKS/,/#endif/' $D/main.c | grep -c 'test-listen\|test-port\|test-shadow\|test-passwd')
check "every test option is inside GATEWAYD_TEST_HOOKS" "$([ "$hooks" -gt 0 ] && [ "$hooks" = "$guarded" ] && echo 1 || echo 0)"
if [ -x $D/gatewayd ]; then
    check "the shipped binary has no test option" \
        "$(grep -aq -- '--test-' $D/gatewayd && echo 0 || echo 1)"
else
    echo "  skip the shipped binary has no test option (not built)"
fi

# 7b. Rule 8 is enforced, not attempted.
check "the service does not start with capabilities held" \
    "$(grep -q 'if (!drop_capabilities())' $D/main.c && grep -q 'SYS_capget' $D/main.c && echo 1 || echo 0)"
check "OpenSSL reads no configuration file" \
    "$(grep -q 'OPENSSL_INIT_NO_LOAD_CONFIG' $D/gw_tls.c && echo 1 || echo 0)"

# 8. The init script.
check "S70gatewayd parses" "$(sh -n $D/S70gatewayd 2>/dev/null && echo 1 || echo 0)"
check "S70gatewayd ships disabled" "$(grep -qx 'GATEWAYD_ENABLE=0' $D/S70gatewayd && echo 1 || echo 0)"
check "S70gatewayd never runs the daemon unsupervised" \
    "$([ "$(grep -c '"\$DAEMON"' $D/S70gatewayd)" = "$(grep -c '"\$SUPERVISE" gatewayd "\$DAEMON"\|-x "\$DAEMON"' $D/S70gatewayd)" ] && echo 1 || echo 0)"
check "the overlay does not carry it" \
    "$([ -e platforms/k230/rootfs_overlay/etc/init.d/S70gatewayd ] && echo 0 || echo 1)"

if [ "$failed" -eq 0 ]; then echo "gatewayd lint: all checks passed"; else echo "gatewayd lint: $failed check(s) FAILED"; fi
exit "$failed"
