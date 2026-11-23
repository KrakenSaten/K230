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
bad=$(echo "$names" | grep -vE '^mesh\.(send|resend)$' | grep -E '^shell\.|reboot|poweroff|expand|eject|radio_setup|connect|forget|set_enabled|configure|\.send|set_name|set_path_hash|channel_add|channel_remove|node_remove|reset_path|advert|discover|remote_|app_send|pair_begin|pair_decide|revoke|set_scopes|^gateway\.scope$|^vision\.(pull|frame|state|stats|enrol|open|start|save)$' || true)
check "no method that changes the unit, and none of the shell" "$([ -z "$bad" ] && echo 1 || echo 0)"
[ -n "$bad" ] && echo "$bad" | sed 's/^/       /'
# The shell (ADR-019 rule 6 as amended 2026-11-22): one connection to its
# socket, on which the gateway calls five methods of the PacketShark app and
# no other - no shell.* method, and no subscription to its events. Four of
# the five are rows a client with the scope can reach; the fifth, the
# cancel, is the gateway's own and is no row.
# As amended again 2026-11-23 (ADR-019 amendment 1): on the same connection
# the gateway also calls six methods of the Vision app - five of them rows a
# client with the scope vision can reach, the sixth, vision.pull, the
# gateway's own - and still no other (section 4d below).
# "shell" is written 14 times: 9 rows, the one connection, the service name
# in the two gateway.upstream events, and the two answers of
# gateway.subscribe.
check "the shell's socket is opened in one place" \
    "$([ "$(grep -c 'gw_uplink_new("shell"' $D/main.c)" = 1 ] && [ "$(grep -ho '"shell"' $SRC | wc -l)" = 14 ] && echo 1 || echo 0)"
shell_rows=$(echo "$table" | grep '"shell"' | grep -oE '\{"[a-z_.]+"' | tr -d '{"' | sort | tr '\n' ' ')
check "its rows are the four capture methods and the five of Vision ($shell_rows)" \
    "$([ "$shell_rows" = "capture.hold capture.request_start capture.status capture.stop vision.mode vision.reset vision.set vision.share vision.status " ] && echo 1 || echo 0)"
check "every capture row is of the capture kind and needs a capture scope" \
    "$([ "$(echo "$table" | grep -cE '\{"capture\.[a-z_]+", GW_K_CAPTURE, GW_W_PAIRED, GW_S_CAPTURE_(VIEW|CONTROL), "shell", gw_capture_')" = 4 ] &&
       [ "$(echo "$table" | grep -c '"capture\.')" = 4 ] && echo 1 || echo 0)"
called=$(grep -ohE 'gw_uplink_call\(G\.shell, ("[a-z_.]+"|m->name)' $D/main.c | sed 's/.*G\.shell, //' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' ')
check "on it the gateway calls capture.status, capture.request_cancel, vision.status, vision.share, vision.pull and a row by its name, and nothing else ($called)" \
    "$([ "$called" = '"capture.request_cancel"x1 "capture.status"x1 "vision.pull"x1 "vision.share"x1 "vision.status"x1 m->namex2 ' ] && echo 1 || echo 0)"
check "the gateway asks capture.status only for somebody: no question of its own on connecting" \
    "$(awk '/^static void cap_on_state\(/,/^}/' $D/main.c | grep -q 'gw_uplink_call' && echo 0 ||
       { awk '/^static void cap_tick\(/,/^}/' $D/main.c | grep -B3 'gw_uplink_call(G.shell, "capture.status"' |
         grep -q '(topic_held(GW_T_CAPTURE) || G.cap.asking)' && echo 1 || echo 0; })"
check "a link that is no longer wanted stays until a cancel on its way is answered" \
    "$(awk '/^static void cap_tick\(/,/^}/' $D/main.c | grep -q 'if (G.cap_say_n > 0 && now < G.cap_close_ms) {' && echo 1 || echo 0)"
none "no shell.* method is named anywhere, and no subscription to the shell" '"shell\.[a-z_]+"|shell\.subscribe' $SRC
check "the gateway's cancel is no method of a client: not a row, and sent from one function" \
    "$(echo "$names" | grep -q 'request_cancel' && echo 0 ||
       { [ "$(grep -c '"capture.request_cancel"' $D/main.c)" = 1 ] &&
         awk '/^static bool cap_send_cancel\(/,/^}/' $D/main.c | grep -q '"capture.request_cancel"' && echo 1 || echo 0; })"
check "and it always names the request it takes back" \
    "$(awk '/^static bool cap_send_cancel\(/,/^}/' $D/main.c | grep -q 'cJSON_AddStringToObject(p, "request", token) == NULL' &&
       [ "$(grep -c 'cap_send_cancel(' $D/main.c)" = 4 ] &&
       [ "$(grep -cE 'cap_send_cancel\(G\.cap\.token, (say|NULL), TAG_CAP_CANCEL\)|cap_send_cancel\(G\.cap_late, NULL, TAG_CAP_LATE\)' $D/main.c)" = 3 ] && echo 1 || echo 0)"
check "a cancel says a request was refused only when the unit answers that it took the sheet down" \
    "$(awk '/^static void cap_on_reply\(/,/^}/' $D/main.c |
       grep -q 'if (say != NULL && result != NULL && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "shown"))) {' && echo 1 || echo 0)"
check "a request whose answer did not come is taken back, not forgotten" \
    "$(awk '/^static void cap_ask_answered\(/,/^}/' $D/main.c | grep -q 'cap_cancel("its answer did not come", NULL);' &&
       awk '/^static void cap_ask_answered\(/,/^}/' $D/main.c | grep -q 'cap_cancel_later(G.cap.token);' &&
       awk '/^static void cap_tick\(/,/^}/' $D/main.c | grep -q 'cap_send_cancel(G.cap_late, NULL, TAG_CAP_LATE);' &&
       awk '/^static void cap_on_state\(/,/^}/' $D/main.c | grep -q 'cap_cancel_later(G.cap.token);' && echo 1 || echo 0)"
check "the capability is offered by the client's own scope, so a client without it sees no change" \
    "$(grep -B1 -A6 'if ((scopes & GW_S_CAPTURE_VIEW) != 0) {' $D/main.c | grep -q 'cJSON_CreateString("capture")' && echo 1 || echo 0)"
check "who asks is the paired name, and the request's name is the gateway's own random one" \
    "$(awk '/^static bool do_capture\(/,/^}/' $D/main.c | grep -q 'cJSON_AddStringToObject(up, "client", G.store.c\[idx\].name)' &&
       awk '/^static bool do_capture\(/,/^}/' $D/main.c | grep -q 'gw_tls_random(rnd, sizeof(rnd))' && echo 1 || echo 0)"
check "the shell's events are taken from nobody: the callback is empty" \
    "$([ "$(awk '/^static void cap_on_event\(/,/^}/' $D/main.c | grep -c ';')" = 3 ] && echo 1 || echo 0)"
check "an answer of the unit reaches a client only through its row's filter" \
    "$(awk '/^static void cap_on_reply\(/,/^}/' $D/main.c | grep -q 'gw_filter_result(q->m, result)' &&
       [ "$(awk '/^static void cap_on_reply\(/,/^}/' $D/main.c | grep -c 'req_done(')" = 1 ] && echo 1 || echo 0)"
capblock=$(awk '/^\/\* ---- PacketShark.s capture/,/^static bool do_subscribe\(/' $D/main.c)
check "an event of a capture is made by gw_capture, never from the unit's answer itself" \
    "$(echo "$capblock" | grep -E 'mesh_emit\(|pocketipc_event\(' |
       grep -vqE 'mesh_emit\(GW_T_CAPTURE, "capture\.(state|stats)", (state|stats|gw_capture_outcome\((say|outcome)\)), (true|false)\);|mesh_emit\(GW_T_CAPTURE, "gateway\.upstream", d, false\);|pocketipc_event\("capture\.state", again\)' &&
       echo 0 || echo 1)"
check "the capture block was found, and names result only to filter it, to read shown, or to hand it to gw_capture" \
    "$([ -n "$capblock" ] && [ -z "$(echo "$capblock" | grep -E '\bresult\b' |
       grep -vE 'result (!=|==) NULL|gw_filter_result\(q->m, result\)|GetObjectItemCaseSensitive\(result, "shown"\)|gw_capture_step\(&G\.cap, result,|cap_ask_answered\(result, code, now\)|const cJSON \*result, int code')" ] && echo 1 || echo 0)"
none "the capture code is free of I/O and of the session's types" 'SSL|socket\(|epoll|read\(|write\(|LOG_|struct sess' $D/gw_capture.c $D/gw_capture.h
check "upstream calls pass no client parameter" \
    "$(grep -q 'pocketipc_call_timeout(u->fd, m->name, NULL,' $D/main.c && echo 1 || echo 0)"
check "upstream methods are called only by table name" \
    "$([ "$(grep -c 'pocketipc_call' $D/main.c)" = 1 ] && echo 1 || echo 0)"

# 4d. Vision (ADR-019 amendment 1; rule 6 as extended 2026-11-23). Ten names
# and no other: six the gateway calls on the shell's socket (vision.status,
# vision.share, vision.pull, vision.mode, vision.set, vision.reset), one a
# client calls that reaches nothing (vision.ack), and three events
# (vision.frame, vision.state, vision.stats).
visnames=$(grep -ohE '"vision\.[a-z_.]*"' $SRC | sort -u | tr -d '"' | tr '\n' ' ')
check "the vision names in the source are the ten of the amendment ($visnames)" \
    "$([ "$visnames" = "vision.ack vision.frame vision.mode vision.pull vision.reset vision.set vision.share vision.state vision.stats vision.status " ] && echo 1 || echo 0)"
visrows=$(echo "$table" | grep -oE '\{"vision\.[a-z_]+"' | tr -d '{"' | sort | tr '\n' ' ')
check "the vision rows are six, and vision.pull is none of them ($visrows)" \
    "$([ "$visrows" = "vision.ack vision.mode vision.reset vision.set vision.share vision.status " ] && echo 1 || echo 0)"
check "every vision row is of the Vision kind, for a paired client, by the scope vision" \
    "$([ "$(echo "$table" | grep -cE '\{"vision\.[a-z_]+", GW_K_VISION, GW_W_PAIRED, GW_S_VISION, ("shell", gw_vision_[a-z_]+|NULL, NULL), ')" = 6 ] &&
       [ "$(echo "$table" | grep -cE 'GW_K_VISION|GW_S_VISION|"vision\.')" = 6 ] && echo 1 || echo 0)"
check "vision.ack reaches no service: the gateway answers it alone" \
    "$(echo "$table" | grep -q '{"vision.ack", GW_K_VISION, GW_W_PAIRED, GW_S_VISION, NULL, NULL, gw_vision_params_ack},' && echo 1 || echo 0)"
check "the scope vision is no part of what a new pairing gets" \
    "$(grep -q '^#define GW_S_DEFAULT (GW_S_STATUS | GW_S_MESH_READ | GW_S_MESH_SEND)$' $D/gw_policy.h && echo 1 || echo 0)"
check "FACE, RECOGNIZE and READ cannot be chosen: vision.mode reads the list only up to them" \
    "$(grep -A1 '^static const char \*const MODES\[\]' $D/gw_vision.c | tr -d '\n' | tr -s ' ' |
       grep -q '{"detect", "track", "traffic", "segment", "instances", "color", "edge", "trace", "face", "recognize", "read", NULL};' &&
       grep -q '^#define GW_VISION_REMOTE 8$' $D/gw_vision.c &&
       awk '/^int gw_vision_params_mode\(/,/^}/' $D/gw_vision.c | grep -q 'i < GW_VISION_REMOTE; i++' && echo 1 || echo 0)"
check "the picture is asked for in one place, for a viewer that has taken the last" \
    "$([ "$(grep -c '"vision.pull"' $D/main.c)" = 1 ] &&
       awk '/^static void vis_tick\(/,/^}/' $D/main.c | grep -q 'gw_uplink_call(G.shell, "vision.pull", p, TAG_VIS_PULL,' &&
       awk '/^static void vis_tick\(/,/^}/' $D/main.c | grep -q 'if (G.vis.viewing && vis_ready() && !G.vis_pulling && gw_vision_pull_due(&G.vis, now)) {' && echo 1 || echo 0)"
check "a picture goes to one session, from one place, and never to a topic's listeners" \
    "$([ "$(grep -c '"vision.frame"' $D/main.c)" = 1 ] &&
       awk '/^static void vis_pulled\(/,/^}/' $D/main.c | grep -q '(void)sess_send(s, pocketipc_event("vision.frame", data));' && echo 1 || echo 0)"
check "before it is sent the viewer is looked up again, and the unit's mode" \
    "$(awk '/^static void vis_pulled\(/,/^}/' $D/main.c |
       awk '/s = vis_viewer\(&why\);/{a=NR} /if \(G.vis_biometric\) \{/{b=NR} /gw_vision_frame\(result, G.vis.last_seq, &data, &seq\)/{c=NR} /sess_send\(/{d=NR} END{print (a && b && c && d && a<b && b<c && c<d) ? 1 : 0}')"
check "the viewer is one whose scope and topic hold now" \
    "$(awk '/^static struct sess \*vis_viewer\(/,/^}/' $D/main.c | grep -q '(scopes & GW_S_VISION) == 0' &&
       awk '/^static struct sess \*vis_viewer\(/,/^}/' $D/main.c | grep -q '(s->topics & GW_T_VISION) == 0' && echo 1 || echo 0)"
check "a picture is made by gw_vision_frame and its answer is named for nothing else" \
    "$([ -z "$(awk '/^static void vis_pulled\(/,/^}/' $D/main.c | grep -E '\bresult\b' |
       grep -vE 'result == NULL|gw_vision_frame\(result, G\.vis\.last_seq, &data, &seq\)|const cJSON \*result, int code')" ] && echo 1 || echo 0)"
check "nothing of a picture is logged: no log call in the pull takes a value" \
    "$(awk '/^static void vis_pulled\(/,/^}/' $D/main.c | grep 'LOG_' | grep -q '%' && echo 0 || echo 1)"
check "the gateway's own vision.share is only ever off" \
    "$([ "$(grep -c '"vision.share"' $D/main.c)" = 2 ] &&
       awk '/^static void vis_send_off\(/,/^}/' $D/main.c | grep -q 'cJSON_AddBoolToObject(p, "on", false) == NULL' &&
       awk '/^static void vis_send_off\(/,/^}/' $D/main.c | grep -q 'gw_uplink_call(G.shell, "vision.share", p, TAG_VIS_OFF,' && echo 1 || echo 0)"
check "who views is the start of the paired key and the paired name, added by the gateway" \
    "$(awk '/^static bool do_vision\(/,/^}/' $D/main.c | grep -q 'cJSON_AddStringToObject(up, "client", p) == NULL' &&
       awk '/^static bool do_vision\(/,/^}/' $D/main.c | grep -q 'cJSON_AddStringToObject(up, "name", G.store.c\[idx\].name) == NULL' &&
       awk '/^int gw_vision_params_share\(/,/^}/' $D/gw_vision.c | grep -q 'static const char \*const names\[\] = {"on", NULL};' && echo 1 || echo 0)"
check "the gateway asks vision.status only for somebody: no question of its own on connecting" \
    "$(awk '/^static void vis_on_state\(/,/^}/' $D/main.c | grep -q 'gw_uplink_call' && echo 0 ||
       { awk '/^static void vis_tick\(/,/^}/' $D/main.c | grep -B3 'gw_uplink_call(G.shell, "vision.status"' |
         grep -q '(topic_held(GW_T_VISION) || G.vis.viewing)' && echo 1 || echo 0; })"
visblock=$(awk '/^\/\* ---- Vision \(ADR-019 amendment 1/,/^\/\* ---- requests from clients/' $D/main.c)
check "the Vision block was found, and its events are the three of Vision and the link's, made here and never the unit's answer" \
    "$([ -n "$visblock" ] && echo "$visblock" | grep -E 'mesh_emit\(|pocketipc_event\(' |
       grep -vqE 'mesh_emit\(GW_T_VISION, "vision\.state", cJSON_CreateObject\(\), false\);|mesh_emit\(GW_T_VISION, "vision\.stats", stats, true\);|pocketipc_event\("vision\.frame", data\)|pocketipc_event\("gateway\.upstream", d\)' &&
       echo 0 || echo 1)"
check "an answer of the unit reaches a client only through gw_vision_status or its row's filter" \
    "$(awk '/^static bool vis_on_reply\(/,/^}/' $D/main.c | grep -q 'filtered = gw_vision_status(result, gw_vision_view_of(&G.vis, q->sid));' &&
       awk '/^static bool vis_on_reply\(/,/^}/' $D/main.c | grep -q 'filtered = gw_filter_result(q->m, result);' &&
       [ "$(awk '/^static bool vis_on_reply\(/,/^}/' $D/main.c | grep -c 'req_done(')" = 1 ] && echo 1 || echo 0)"
check "the capability is offered by the client's own scope vision" \
    "$(grep -A4 'if ((scopes & GW_S_VISION) != 0) {' $D/main.c | grep -q 'cJSON_CreateString("vision")' && echo 1 || echo 0)"
check "the shell's events are still taken from nobody" \
    "$(grep -q 'static const struct gw_uplink_cb shell_cb = {shell_on_state, cap_on_event, shell_on_reply};' $D/main.c && echo 1 || echo 0)"
check "gateway.scope is a method of the local control socket alone" \
    "$([ "$(grep -c '"gateway.scope"' $SRC | awk -F: '{n+=$2} END{print n}')" = 1 ] &&
       awk '/^static void ctl_handler\(/,/^}/' $D/main.c | grep -q 'strcmp(method, "gateway.scope") == 0' && echo 1 || echo 0)"
none "the vision code is free of I/O and of the session's types" 'SSL|socket\(|epoll|read\(|write\(|LOG_|struct sess|fopen|open\(' $D/gw_vision.c $D/gw_vision.h

# 4b. The mesh group (M4.1): reading only, by mesh_read only, and the
# gateway's own connection to meshcored asks for three things by name.
meshrows=$(echo "$table" | grep -E '\{"mesh\.' | grep -vE '"mesh\.(status|send|resend)"')
check "the mesh rows were found" "$([ "$(echo "$meshrows" | grep -c .)" -ge 6 ] && echo 1 || echo 0)"
check "every mesh row but mesh.status needs mesh_read" \
    "$([ -z "$(echo "$meshrows" | grep -v 'GW_S_MESH_READ')" ] && echo 1 || echo 0)"
none "no app datagram is ever sent" '"mesh\.app_send' $SRC
none "the receive log is never asked for" 'rx_log' $SRC
own=$(grep -oE 'gw_uplink_call\(G\.mesh, "[a-z_.]+"' $D/main.c | grep -oE '"[a-z_.]+"' | sort -u | tr -d '\n')
check "the gateway's own calls to meshcored are status, subscribe, messages and send" \
    "$([ "$own" = '"mesh.messages""mesh.send""mesh.status""mesh.subscribe"' ] && echo 1 || echo 0)"
check "every other call to meshcored is a table row by its name" \
    "$([ "$(grep -c 'gw_uplink_call(G.mesh,' $D/main.c)" = 7 ] && [ "$(grep -c 'gw_uplink_call(G.mesh, m->name,' $D/main.c)" = 2 ] &&
       [ "$(grep -c 'gw_uplink_call(' $D/main.c)" = 14 ] && [ "$(grep -c 'gw_uplink_call(G.shell,' $D/main.c)" = 7 ] && echo 1 || echo 0)"
check "what goes upstream with a client's request is the row's own parameter check" \
    "$(grep -q 'gw_uplink_call(G.mesh, m->name, up,' $D/main.c && grep -q 'm->params(params, &up)' $D/main.c && echo 1 || echo 0)"
# 4c. Sending (M4.2): two rows, by mesh_send only; one place passes a
# message to meshcored, and what it passes was written by gw_send.c.
sendrows=$(echo "$table" | grep 'GW_S_MESH_SEND' | grep -oE '\{"[a-z_.]+"' | tr -d '{"' | sort | tr '\n' ' ')
check "mesh_send is needed by mesh.send and mesh.resend and by nothing else" \
    "$([ "$sendrows" = 'mesh.resend mesh.send ' ] && echo 1 || echo 0)"
check "the send rows need mesh_send and no other scope" \
    "$([ "$(echo "$table" | grep -cE '"mesh\.(send|resend)", GW_K_(SEND|RESEND), GW_W_PAIRED, GW_S_MESH_SEND, ')" = 2 ] && echo 1 || echo 0)"
check "a message is passed to meshcored in one place" \
    "$([ "$(grep -c 'G.mesh, "mesh.send"' $D/main.c)" = 1 ] && grep -q 'gw_uplink_call(G.mesh, "mesh.send", up, r->tag' $D/main.c && echo 1 || echo 0)"
check "and what is passed was written by gw_send_upstream" \
    "$([ "$(grep -c 'gw_send_upstream(&q, params)' $D/main.c)" = 2 ] && echo 1 || echo 0)"
check "the lookup for an unanswered send asks for the list with no parameter" \
    "$(grep -q 'gw_uplink_call(G.mesh, "mesh.messages", NULL, r->tag' $D/main.c && echo 1 || echo 0)"
check "meshcored keeps at least the 16 messages of a kind that the absence proof assumes" \
    "$([ "$(grep -cE '^const int (MSG_RING|CHAN_MSG_RING) = (1[6-9]|[2-9][0-9]|[0-9]{3,});' services/meshcored/mesh_runtime.cpp)" = 2 ] && grep -q 'define GW_SEND_RING_MIN 16$' $D/gw_send.h && echo 1 || echo 0)"
check "absence is proof only on the connection the message was passed on" \
    "$(awk '/^static bool send_on_reply\(/,/^}/' $D/main.c | grep -q 'r->conn != G.mesh_conn' && echo 1 || echo 0)"
check "a proven-absent message is sent only after the scope was looked up again" \
    "$(awk '/^static bool send_on_reply\(/,/^}/' $D/main.c | grep -q 'GW_S_MESH_SEND' && echo 1 || echo 0)"
passed=$(awk '/^cJSON \*gw_filter_event\(/,/^}/' $D/gw_policy.c | grep -oE '"mesh\.[a-z_]+"' | sort | tr -d '\n')
check "three events of meshcored are passed on by name and no other" \
    "$([ "$passed" = '"mesh.channel""mesh.node""mesh.state"' ] && echo 1 || echo 0)"
none "no field list of the mesh group names a key or a secret" \
    '^\s*"[a-z_", ]*"(key|secret|private_key|prv|psk|password|passphrase|token)"' $D/gw_policy.c
check "the journal is memory only: no file is written by it" \
    "$(grep -qE 'fopen|open\(|write\(|fwrite' $D/gw_journal.c $D/gw_feed.c $D/gw_send.c && echo 0 || echo 1)"

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
