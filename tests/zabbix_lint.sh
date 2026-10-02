#!/bin/bash
# Zabbix's boundaries (docs/apps/ZABBIX.md, ADR-007 accepted), held statically.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

A=apps/zabbix
C=core/zabbix
H=tools/zabbix/pos_zabbix.c
APP=$A/zabbix_app.c

# ---- layering ------------------------------------------------------------------
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $C/*.[ch] tools/zabbix/*.c $A/zabbix_session.[ch] \
       $A/zabbix_view.[ch] 2>/dev/null)
check "the client layer, the helper, the session and the view are free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# The shell links the model and the protocol, and nothing else of core/zabbix:
# no JSON parser for the server's answers, no transport, no secret.
hits=$(grep -oE 'core/zabbix/[a-z_]+\.c' ui/shell/CMakeLists.txt | sort -u | tr '\n' ' ')
check "the shell links only zbx_model.c and zbx_proto.c of core/zabbix ($hits)" \
    "$([ "$hits" = "core/zabbix/zbx_model.c core/zabbix/zbx_proto.c " ] && echo 1 || echo 0)"
hits=$(code $A/*.c $A/*.h | grep -nE 'cjson|cJSON|curl|openssl|zbx_api\.h|zbx_client\.h|zbx_config\.h|zbx_http\.h|zbx_fake\.h')
check "the app never includes JSON, curl, TLS, the API, the client, the config or the fake" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $A/*.c | grep -nE '\b(socket|connect|getaddrinfo|gethostbyname|bind|accept)\(')
check "the app opens no network socket (only the helper's socketpair)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $APP $A/zabbix_view.c | grep -nE '\b(fopen|open|openat|unlink|rename|mkdir|opendir|fork|exec[lv]p?e?)\(')
check "the screen and the view never touch files or start processes" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rlE '\bfork\(' $A $C tools/zabbix --include='*.c' | sort | tr '\n' ' ')
check "only the session starts a process, and the helper its logout child ($hits)" \
    "$([ "$hits" = "$A/zabbix_session.c tools/zabbix/pos_zabbix.c " ] &&
       [ "$(grep -c '\bfork()' tools/zabbix/pos_zabbix.c)" = 1 ] && echo 1 || echo 0)"
check "the logout child never execs anything" "$(grep -qE '\bexec[lv]p?e?\(' tools/zabbix/pos_zabbix.c && echo 0 || echo 1)"
hits=$(code $C/*.c tools/zabbix/*.c $A/*.c | grep -nE '\b(system|popen)\(')
check "no shell is ever run" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(grep -lE '#include <curl/' $C/*.c $C/*.h tools/zabbix/*.c $A/*.c 2>/dev/null | tr '\n' ' ')
check "only zbx_http_curl.c speaks libcurl ($hits)" \
    "$([ "$hits" = "$C/zbx_http_curl.c " ] && echo 1 || echo 0)"

# ---- read-only ----------------------------------------------------------------
hits=$(grep -ohE '"[a-z]+\.(get|login|version|create|update|delete|acknowledge|massupdate|massadd|execute|logout)"' \
       $C/zbx_api.c | sort -u | tr '\n' ' ')
# user.logout ends the viewer's own session (password setups), which the API
# asks of every user.login; it changes nothing the server monitors.
check "the API layer only reads: apiinfo.version, user.login/logout and *.get ($hits)" \
    "$(printf '%s\n' $hits | grep -vqE '^"(apiinfo\.version|user\.login|user\.logout|[a-z]+\.get)"$' && echo 0 || echo 1)"
check "and it does log out what it logs in (the helper, when it ends)" \
    "$(grep -q 'zbx_client_logout(' tools/zabbix/pos_zabbix.c && grep -q '"user.logout"' $C/zbx_api.c && echo 1 || echo 0)"

# ---- TLS and the token ----------------------------------------------------------
check "a redirect is never followed" \
    "$(grep -q 'CURLOPT_FOLLOWLOCATION, 0L' $C/zbx_http_curl.c && ! grep -q 'CURLOPT_FOLLOWLOCATION, 1' $C/zbx_http_curl.c &&
       echo 1 || echo 0)"
check "curl is never verbose (it would print the Authorization header)" \
    "$(grep -q 'CURLOPT_VERBOSE' $C/zbx_http_curl.c && echo 0 || echo 1)"
check "verification follows the configuration, which defaults to on" \
    "$(grep -q 'CURLOPT_SSL_VERIFYPEER, req->verify ? 1L : 0L' $C/zbx_http_curl.c &&
       grep -q 'CURLOPT_SSL_VERIFYHOST, req->verify ? 2L : 0L' $C/zbx_http_curl.c &&
       grep -q 'c->verify = true;' $C/zbx_config.c && echo 1 || echo 0)"
check "the header holding the token is wiped and freed after each request" \
    "$(grep -q 'explicit_bzero(auth, sizeof(auth));' $C/zbx_http_curl.c &&
       grep -q 'curl_slist_free_all(hdr);' $C/zbx_http_curl.c && echo 1 || echo 0)"
hits=$(code $C/*.c tools/zabbix/*.c $A/*.c |
       grep -nE 'LOG_(DEBUG|INFO|WARN|ERROR)\(.*((->|\.)(secret|session|password)\b|\bbearer\b|\bvalue\b)')
check "no log line is given the secret, the bearer, the password or the session" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the secret file is refused when others may read it" \
    "$(grep -q '(st.st_mode & 077) != 0' $C/zbx_config.c && echo 1 || echo 0)"
check "and written 0600 under a 0700 directory, atomically" \
    "$(grep -q 'O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600' $C/zbx_config.c &&
       grep -q 'pocketos_mkdir_p(dir, 0700)' $C/zbx_config.c && grep -q 'rename(tmp, path)' $C/zbx_config.c &&
       echo 1 || echo 0)"
check "a token in the world-readable zabbix.conf is refused" \
    "$(grep -q 'must not hold a %s: use pos-zabbix set-secret' $C/zbx_config.c && echo 1 || echo 0)"
check "http:// needs allow_insecure_http=1" \
    "$(grep -q 'if (!c->https && !c->allow_http)' $C/zbx_config.c && echo 1 || echo 0)"
check "a secret never reaches argv: the helper is started with session and --fake only" \
    "$(code $A/zabbix_session.c | grep -E 'argv\[argc\+\+\] =' | grep -vqE '"session"|"--fake"|helper|fake' && echo 0 || echo 1)"
check "and set-secret reads stdin" "$(grep -q 'fgets(value, sizeof(value), stdin)' $H && echo 1 || echo 0)"

# ---- the CONNECTION screen (DS 50, docs/apps/ZABBIX.md section 7) ---------------
S=$C/zbx_settings.c
check "the typed secret is masked from its first character" \
    "$(grep -q 'lv_textarea_set_password_mode(a->cn_secret, true);' $APP &&
       grep -q 'lv_textarea_set_password_show_time(a->cn_secret, 0);' $APP && echo 1 || echo 0)"
check "the secret field is never filled: the only text it is given is empty" \
    "$(code $APP | grep -q 'lv_textarea_set_text(a->cn_secret' && echo 0 || echo 1)"
check "the settings line carries no secret, only whether one is stored" \
    "$(grep -q 'int zbx_proto_settings(FILE \*out, const char \*url, const char \*auth, const char \*user,' $C/zbx_proto.c &&
       grep -A1 'int zbx_proto_settings(' $C/zbx_proto.c | grep -q 'bool stored, const char \*note)' && echo 1 || echo 0)"
check "the typed secret is wiped: the app's line, the field, the helper's input and command" \
    "$(grep -q 'explicit_bzero(line, sizeof(line));' $A/zabbix_session.c &&
       grep -q 'explicit_bzero(t, strlen(t));' $APP && [ "$(code $APP | grep -c 'wipe_field(a, a->cn_secret)')" -ge 4 ] &&
       grep -q 'explicit_bzero(s->in, sizeof(s->in));' $H && grep -q 'explicit_bzero(&cmd, sizeof(cmd));' $H &&
       echo 1 || echo 0)"
check "only what connected is stored" \
    "$(grep -q 'if (!save || r != ZBX_CRESULT_CONNECTED) {' $S && echo 1 || echo 0)"
check "the conf is staged (0644, or its own mode), the secret through the secret store's own path, renames undone on failure" \
    "$(grep -q 'O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644' $S && grep -q 'fchmod(fd, mode)' $S &&
       grep -q 'zbx_config_stage_secret(' $S &&
       grep -q 'link(secret_path, prev)' $S && grep -q 'rename(prev, secret_path);' $S && echo 1 || echo 0)"
check "a settings change never reaches argv: the helper hears it on the socketpair" \
    "$(code $A/zabbix_session.c | grep -q 'zbx_proto_cmd_settings(line, sizeof(line)' &&
       grep -q 'send(s->fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT)' $A/zabbix_session.c && echo 1 || echo 0)"

# ---- lifetime ------------------------------------------------------------------
check "the helper leaves with the shell (PR_SET_PDEATHSIG)" \
    "$(grep -q 'prctl(PR_SET_PDEATHSIG, SIGTERM);' $A/zabbix_session.c && echo 1 || echo 0)"
check "the helper is polled from a timer, never waited for, except when leaving" \
    "$(code $APP | grep -q 'lv_timer_create(on_poll, ZABBIX_POLL_MS, a)' &&
       [ "$(code $APP | grep -c 'zabbix_session_abandon(')" = 2 ] && echo 1 || echo 0)"
check "every read of the helper's socket is non-blocking" \
    "$(grep -q 'recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)' $A/zabbix_session.c &&
       ! code $A/zabbix_session.c | grep -qE '\bread\(s->fd' && echo 1 || echo 0)"
check "the helper never makes its shared socket non-blocking (its writes would fail)" \
    "$(code $H | grep -q 'O_NONBLOCK' && echo 0 || echo 1)"
destroy=$(awk '/^static void zabbix_destroy\(/,/^}/' $APP)
order=$(printf '%s\n' "$destroy" | grep -nE 'lv_obj_remove_event_cb_with_user_data|lv_timer_delete|zabbix_session_abandon|free\(a\);' | cut -d: -f1 | tr '\n' ' ')
check "destroy: size handler, timer, then helper, then memory (lines $order)" \
    "$(set -- $order; [ $# = 4 ] && [ "$1" -lt "$2" ] && [ "$2" -lt "$3" ] && [ "$3" -lt "$4" ] && echo 1 || echo 0)"
check "no callback crosses from the session into the app" \
    "$(grep -qE '\(\*[a-z_]+\)\(' $A/zabbix_session.h && echo 0 || echo 1)"
check "the size handler only sets a flag (no layout inside an LVGL event)" \
    "$(awk '/^static void on_frame_size\(/,/^}/' $APP | grep -qE 'layout\(|lv_obj_update_layout' && echo 0 || echo 1)"

# ---- bounded ---------------------------------------------------------------------
check "the lists are pools bounded by the model" \
    "$(grep -q 'calloc(ZBX_PROBLEM_MAX, sizeof(\*a->prows))' $APP && grep -q 'calloc(ZBX_HOST_MAX, sizeof(\*a->hrows))' $APP &&
       grep -q 'i < n && i < ZBX_PROBLEM_MAX' $APP && grep -q 'i < n && i < ZBX_HOST_MAX' $APP && echo 1 || echo 0)"
check "every answer is capped (ZBX_RESPONSE_MAX) and every request has a limit" \
    "$(grep -q 'req.max_bytes = ZBX_RESPONSE_MAX;' $C/zbx_client.c &&
       grep -q 'if (s->len + add > s->max)' $C/zbx_http_curl.c &&
       grep -q 'ZBX_PROBLEM_FETCH), x.id' $C/zbx_client.c && grep -q 'ZBX_HOST_FETCH), x.id' $C/zbx_client.c && echo 1 || echo 0)"

# ---- the fake ----------------------------------------------------------------------
check "the device's default is the real server, never the fake" \
    "$(grep -q '#define ZABBIX_FAKE_DEFAULT NULL' $A/zabbix_session.c && echo 1 || echo 0)"
check "a fake is always labelled SIMULATED" \
    "$(grep -q 'const char \*hint = a->model->fake ? "SIMULATED" : "";' $APP && echo 1 || echo 0)"
check "the mock server is never installed" \
    "$(sed -n '/^install:/,/^$/p' Makefile | grep -q 'pos-zabbix-mock' && echo 0 || echo 1)"

# ---- layout (DS 21.3, 30.8) ----------------------------------------------------------
check "fullscreen (DS 30.8)" "$(grep -q '.chrome = POCKETOS_CHROME_NONE' $APP && echo 1 || echo 0)"
check "the corners are cleared through PocketUI's guard" \
    "$(grep -q 'pocketui_layout_begin(&a->guard, a->frame, &in)' $APP && echo 1 || echo 0)"

echo "zabbix_lint: $failed failure(s)"
exit $((failed > 0))
