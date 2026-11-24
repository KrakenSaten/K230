#!/bin/bash
# PacketShark's boundaries (docs/apps/PACKETSHARK.md, ADR-016 accepted),
# held statically. tests/pos_capture_test.sh and the session and app tests
# hold the same rules at run time.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# Source without comments, so a rule may be named in prose.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

C=core/pocketpcap
H=tools/capture/pos_capture.c
A=apps/packetshark
ALL="$C/*.c $C/*.h $H $A/*.c $A/*.h $A/ui/*.c $A/ui/*.h"

# ---- receive only (ADR-016 rule 2, PRODUCT_SPEC S8) ------------------------------
hits=$(code $ALL | grep -nE '\b(pcap_inject|pcap_sendpacket|sendto|sendmsg|sendmmsg)\(|PACKET_MR_|SOCK_RAW|AF_PACKET|ETH_P_ALL')
check "nothing can transmit a frame: no inject, sendto, sendmsg or raw socket of our own" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -lE '\bsend\(' $ALL 2>/dev/null | sort | tr '\n' ' ')
check "send() only on the helper's own socketpair ($hits)" \
    "$([ "$hits" = "$A/psk_session.c $H " ] && echo 1 || echo 0)"
hits=$(grep -lE '#include <pcap' $ALL 2>/dev/null | tr '\n' ' ')
check "libpcap is included in one file only ($hits)" \
    "$([ "$hits" = "$C/ppc_source_pcap.c " ] && echo 1 || echo 0)"
hits=$(code $C/ppc_source_pcap.c | grep -nE 'pcap_set_promisc\([^,]+, *1\)')
check "promiscuous mode is never forced on: it is the caller's choice" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- the hot path ---------------------------------------------------------------
hits=$(code $C/ppc_decode.c $C/ppc_stats.c | grep -nE '\b(s?n?printf|fprintf|puts|malloc|calloc|realloc|free)\(')
check "the decoder and the statistics format no text and allocate nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $C/ppc_store.c | grep -cE '\bmalloc\(')
check "the store allocates in one place, at init (2 calls)" "$([ "$hits" = "2" ] && echo 1 || echo 0)"
hits=$(code $C/ppc_decode.c | grep -nE '\bwhile *\(1\)|\bfor *\(;;\)|goto')
check "the decoder has no unbounded loop and no goto" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $C/ppc_view.c | grep -cE '\b(malloc|calloc|realloc)\(')
check "the list's view allocates in one place, at init (M5)" "$([ "$hits" = "1" ] && echo 1 || echo 0)"
hits=$(code $C/ppc_dissect.c | grep -nE '\b(malloc|calloc|realloc|free)\(|\bwhile *\(1\)|\bfor *\(;;\)|goto')
check "the dissector allocates nothing and has no unbounded loop or goto (M6)" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $C/ppc_decode.c $C/ppc_stats.c $C/ppc_store.c | grep -nE 'ppc_dissect|ppc_view_')
check "the capture path calls neither the view nor the dissector" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- layering --------------------------------------------------------------------
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $C/*.[ch] $H $A/psk_session.[ch] $A/psk_model.[ch] $A/psk_net.[ch] 2>/dev/null)
check "the library, the helper, the session, the model and its words are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $A/*.c $A/*.h $A/ui/*.c $A/ui/*.h | grep -nE '#include "(pocketpcap/)?(ppc_decode|ppc_store|ppc_stats|ppc_source|ppc_pcapfile|ppc_format)\.h"')
check "the app includes nothing of the capture library but the protocol" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -oE 'core/pocketpcap/[a-z_]+\.c' ui/shell/CMakeLists.txt | sort -u | tr '\n' ' ')
check "the shell links only ppc_proto.c of core/pocketpcap ($hits)" \
    "$([ -z "$hits" ] || [ "$hits" = "core/pocketpcap/ppc_proto.c " ] && echo 1 || echo 0)"
hits=$(code $A/*.c $A/ui/*.c | grep -nE '\b(socket|connect|bind|accept|getaddrinfo)\(')
check "the app opens no network socket (only the helper's socketpair)" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $A/psk_app.c $A/psk_model.c $A/ui/*.c | grep -nE '\b(fopen|open|openat|unlink|rename|mkdir|opendir|fork|exec[lv]p?e?)\(')
check "the screens and the model never touch files or start processes" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $A/psk_saves.c | grep -nE '\b(fopen|open|openat|read|pread|fread|rename|mkdir|unlink|system|fork|exec[lv]p?e?)\(')
check "the saved captures are listed and deleted, never opened or read (M8)" \
    "$([ -z "$hits" ] && [ "$(code $A/psk_saves.c | grep -cE '\bunlinkat\(')" = "1" ] &&
       code $A/psk_saves.c | grep -q 'AT_SYMLINK_NOFOLLOW' && code $A/psk_saves.c | grep -q 'ppc_save_name_ok(name)' &&
       echo 1 || echo 0)"
hits=$(code $A/psk_app.c | grep -nE 'case (LV_KEY_ENTER|. .|.s.):' -A3 | grep -E 'PSK_ACT_START|psk_sheet_answer|psk_saves_delete')
check "no key starts a capture, answers a sheet or deletes a file (ADR-016 rule 1)" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(grep -rlE '\bfork\(' $A $C tools/capture --include='*.c' | sort | tr '\n' ' ')
check "only the session starts a process ($hits)" "$([ "$hits" = "$A/psk_session.c " ] && echo 1 || echo 0)"

# ---- nothing written (D10, ADR-016 rules 8-9) -------------------------------------
hits=$(code $H $C/*.c | grep -nE 'O_CREAT|fopen\([^)]*"[wa]')
check "the helper creates two files: its lock, and SAVE's capture, new and never followed (M8)" \
    "$([ "$(echo "$hits" | grep -c .)" = "2" ] && echo "$hits" | grep -q 'O_RDWR | O_CREAT' &&
       echo "$hits" | grep -q 'O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600' && echo 1 || echo 0)"
[ "$(echo "$hits" | grep -c .)" != "2" ] && echo "$hits" | head -5
hits=$(code $H | grep -nE 'ppc_pcap_write_(header|record)\(' | grep -vE 'save_fd')
check "packets are written to SAVE's file and nowhere else" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $H | grep -cE 'case PPC_CMD_SAVE:')
check "only the save command starts a save" \
    "$([ "$hits" = "1" ] && [ "$(code $H | grep -cE '\bcmd_save\(h\)')" = "1" ] && echo 1 || echo 0)"
hits=$(code $C/ppc_decode.c $C/ppc_signals.c | grep -nE '\b(malloc|calloc|realloc|printf|snprintf|sprintf)\(')
check "the findings allocate once, at init, and format nothing (M7)" \
    "$([ "$(echo "$hits" | grep -c .)" = "1" ] && echo "$hits" | grep -q 'calloc(flows' && echo 1 || echo 0)"
# The network intelligence (M12.1) by the same rules: one allocation, no
# text, no loop without a bound; and passive, as everything here is.
hits=$(code $C/ppc_net.c | grep -nE '\b(malloc|calloc|realloc|printf|snprintf|sprintf)\(')
check "the network intelligence allocates once, at init, and formats nothing (M12.1)" \
    "$([ "$(echo "$hits" | grep -c .)" = "1" ] && echo "$hits" | grep -q 'nt->mem = calloc(1, PPC_NET_HOSTS' && echo 1 || echo 0)"
hits=$(code $C/ppc_net.c | grep -nE '\bwhile *\(1\)|\bfor *\(;;\)|goto|#include <(sys/socket|netinet|arpa|unistd)')
check "it has no unbounded loop, no goto, and nothing of a socket in it" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'psk_session_net\(')
check "its lists are asked for in one place, while the overview or its sheet shows (psk_session.c's definition aside)" \
    "$([ "$hits" = "2" ] && sed -n -E '/^static void net_poll\(/,/^}/p' $A/psk_app.c | grep -q 'psk_session_net(a->session, a->model)' &&
       sed -n -E '/^static void net_poll\(/,/^}/p' $A/psk_app.c | grep -q 'if (!wanted || !psk_capture_present(a)) {' && echo 1 || echo 0)"
# The one mention of /tmp is the refusal to save there (ppc_captures_dir).
hits=$(code $H $C/*.c $A/*.c | grep -nE '"/tmp' | grep -vF 'strncmp(buf, "/tmp", 4) == 0')
check "nothing goes to /tmp, and a captures folder there is refused" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $H | grep -E 'LOG_(INFO|WARN|ERROR|DEBUG)' | grep -E '\b(t|row|rec|r)\.(src|dst|info)|bytes|ppc_format' )
check "no log line is built from a packet's text (S7)" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- what may be read from outside the app (psk_remote.h, capture.status) -----------
R=$A/psk_remote.c
hits=$(grep -hE '^#include' $R $A/psk_remote.h | LC_ALL=C sort -u | tr '\n' ' ')
want='#include "psk_model.h" #include "psk_remote.h" #include <cjson/cJSON.h> #include <stdbool.h> #include <stdint.h> #include <string.h> '
check "the status is built from the model's header, cJSON and the C library alone" "$([ "$hits" = "$want" ] && echo 1 || echo 0)"
[ "$hits" = "$want" ] || echo "     $hits"
# Every member of the model it reads, by name: counters, the state, the
# interface, how the helper ended. A row, the inspected packet, a saved
# file's name or the helper's own words are not among them.
hits=$(code $R | grep -oE '\bm->[a-z_]+(\.[a-z_]+)?' | LC_ALL=C sort -u | tr '\n' ' ')
want='m->c.bps m->c.bytes m->c.cls m->c.elapsed_ms m->c.fill_permille m->c.held m->c.ifdrop m->c.kdrop m->c.pps m->c.seen m->c.shed m->c.sig m->c.stored m->d.dns_answered m->d.dns_late m->d.dns_noreply m->d.dns_nxdomain m->d.dns_queries m->d.dns_refused m->d.dns_retries m->d.dns_rtt_max_us m->d.dns_rtt_sum_us m->d.dns_servfail m->d.dns_waiting m->d.tcp_aborted m->d.tcp_attempts m->d.tcp_connected m->d.tcp_hs_max_us m->d.tcp_hs_sum_us m->d.tcp_refused m->d.tcp_timed m->exit m->have_diag m->have_stats m->iface m->reason m->state '
check "it reads the model's counters, state, interface and exit, and no other member" "$([ "$hits" = "$want" ] && echo 1 || echo 0)"
[ "$hits" = "$want" ] || echo "     $hits"
hits=$(code $R | grep -nE '[(,] *m *[,)]|\*m\b|&m\b|&m->|m->(c|d) *[^.]|= *m *[;,)]' | grep -vE '^[0-9]+:void psk_remote_take\(const struct psk_model \*m,')
check "and never the model, or a block of its counters, as a whole or by another name" \
    "$([ -z "$hits" ] && [ "$(code $R | grep -c 'psk_model')" = "1" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $R | grep -nE 'rows|pkt|_seq|err_|save_|version|caps|backend|host|addr|port|hex|field|layer|info|src|dst|ssid|ipv4|psk_fmt|printf|psk_app')
check "nothing of a row, a packet, an address, a file or a formatted text is named in it" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
# The JSON is made from the figures alone: the function that writes it never
# sees the model.
hits=$(sed -n '/^struct cJSON \*psk_remote_json/,$p' $R | grep -cE 'psk_model|m->')
check "the JSON, the state event and the events' timing are made from the figures, which the model is not in" \
    "$([ "$hits" = "0" ] && echo 1 || echo 0)"
hits=$(sed -n '/^struct psk_remote_emit {/,/^};/p' $A/psk_remote.h | grep -E ';' |
       grep -vE '^[[:space:]]*(uint8_t|uint32_t|int64_t|bool) [a-z_]+;[[:space:]]*(/\*.*\*/)?$|^[[:space:]]*char iface\[PPC_IFACE_MAX\];[[:space:]]*(/\*.*\*/)?$|^};$')
check "what is remembered of the events is a state, times and one interface name" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(sed -n '/^struct psk_remote_figures {/,/^};/p' $A/psk_remote.h | grep -E ';' |
       grep -vE '^[[:space:]]*(uint8_t|uint32_t|uint64_t|bool) [a-z_]+;[[:space:]]*(/\*.*\*/)?$|^[[:space:]]*uint64_t [a-z_]+\[[A-Z_]+\];[[:space:]]*(/\*.*\*/)?$|^[[:space:]]*char iface\[PPC_IFACE_MAX\];[[:space:]]*(/\*.*\*/)?$|^};$')
check "the figures are numbers, a flag and one interface name" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
# Every text that can reach the answer: the keys and the enumerated words.
hits=$(code $R | grep -vE '^#include' | grep -oE '"[^"]*"' | LC_ALL=C sort -u | tr '\n' ' ')
want='"arp" "available" "bps" "bytes" "capturing" "classes" "dhcp" "diag" "dns" "dns_answered" "dns_late" "dns_noreply" "dns_nxdomain" "dns_queries" "dns_refused" "dns_retries" "dns_rtt_max_us" "dns_rtt_sum_us" "dns_servfail" "dns_waiting" "elapsed_ms" "end" "ended" "error" "eth" "fill_permille" "findings" "full" "headers" "held" "helper_crashed" "helper_hung" "hold" "icmp" "idle" "iface" "iface_drops" "ipv6" "keep" "kernel_drops" "left" "limit" "limit_s" "link" "none" "other" "pps" "reason" "refused" "request" "requested_by_companion" "saving" "seen" "shed" "shown" "state" "stopped" "stored" "synth" "tcp" "tcp_aborted" "tcp_attempts" "tcp_connected" "tcp_hs_max_us" "tcp_hs_sum_us" "tcp_refused" "tcp_timed" "timeout" "udp" "usb" "user" "wlan" '
check "its keys and words are the contract's list, to the letter" "$([ "$hits" = "$want" ] && echo 1 || echo 0)"
[ "$hits" = "$want" ] || echo "     $hits"
hits=$(grep -lE 'psk_remote_(take|json|state_json)\(' $A/*.c $A/ui/*.c ui/shell/*.c 2>/dev/null | LC_ALL=C sort | tr '\n' ' ')
check "the figures are taken and written in the app's own file and nowhere else ($hits)" \
    "$([ "$hits" = "$A/psk_app.c $A/psk_remote.c " ] && echo 1 || echo 0)"
hits=$(code $A/psk_app.c | grep -cE 'psk_remote_take\(')
check "and taken in one place, for what the screen shows" "$([ "$hits" = "1" ] && echo 1 || echo 0)"
# Reading reads: it sends the helper nothing and touches no part of the screen.
body() { sed -n -E "/^(static )?[a-z]+ +$1\(/,/^}/p" $A/psk_app.c; }
hits=$( { body psk_ipc_read; body remote_figures; } |
       grep -nE 'psk_session_|psk_do\(|capture_command|psk_sheet|psk_select|psk_set_|psk_open|lv_|psk_text|LOG_|a->[a-z_]+ *=[^=]')
check "capture.status sends the helper nothing, changes nothing and logs nothing" \
    "$([ -z "$hits" ] && [ -n "$(body psk_ipc_read)" ] && [ -n "$(body remote_figures)" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "it takes the app as const" \
    "$(body psk_ipc_read | grep -q '^static int psk_ipc_read(const struct psk_app \*a,' &&
       body remote_figures | grep -q '^static void remote_figures(const struct psk_app \*a,' && echo 1 || echo 0)"
# HOLD, RESUME and STOP have one way to the helper, the buttons' own; the
# request cannot reach it in a state whose bar does not offer the button.
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'psk_session_(hold|stop)\(')
check "the helper is told to hold or stop in one function only (3 calls, psk_session.c's own definitions aside)" \
    "$([ "$hits" = "5" ] && [ "$(body capture_command | grep -cE 'psk_session_(hold|stop)\(')" = "3" ] && echo 1 || echo 0)"
# A start request (capture.request_start; ADR-016 rules 1 and 1a): asking
# raises a sheet and starts nothing; a capture starts in one function, which
# only START on the setup screen and START on that sheet reach.
hits=$(code $A/*.c $A/ui/*.c | grep -cE '\bstart_capture\(a\)')
check "a capture is started in two places: START on setup, and START on a request's sheet" \
    "$([ "$hits" = "2" ] && body psk_do | grep -q 'start_capture(a);' && body request_answer | grep -q 'start_capture(a);' &&
       echo 1 || echo 0)"
hits=$(code $A/*.c $A/ui/*.c | grep -cE '\brequest_answer\(')
check "the sheet's answer is reached only from a tap on one of its buttons, or Back as REFUSE" \
    "$([ "$hits" = "2" ] && body psk_sheet_answer | grep -q 'request_answer(a, go);' &&
       [ "$(code $A/*.c $A/ui/*.c | grep -cE 'psk_sheet_answer\(')" = "4" ] &&
       code $A/ui/psk_sheets.c | grep -A3 '^static void on_go' | grep -q 'psk_sheet_answer(lv_event_get_user_data(e), true);' &&
       body psk_back | grep -q 'psk_sheet_answer(a, false); /\* Back is REFUSE \*/' && echo 1 || echo 0)"
hits=$( { body psk_ipc_ask; body psk_ipc_unask; body request_watch; body request_over; body client_text; } |
       grep -nE 'start_capture|psk_session_|psk_do\(|psk_sheet_answer|request_answer|a->(keep|limit_idx|sel|cfilter|chosen) *=|LOG_')
check "asking, cancelling and timing out start nothing, choose nothing on the setup screen and log nothing" \
    "$([ -z "$hits" ] && [ -n "$(body psk_ipc_ask)" ] && [ -n "$(body request_watch)" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "a request's START is HEADERS with a time limit of the list, whatever was asked" \
    "$(body request_answer | grep -q 'a->keep = PPC_KEEP_HEADERS;' &&
       body request_answer | grep -q 'if (psk_limit_min\[i\] && psk_limit_min\[i\] \* 60u == a->req_limit_s) {' &&
       body psk_ipc_ask | grep -q 'offered |= psk_limit_min\[i\] != 0 &&' &&
       ! body request_answer | grep -q 'PPC_KEEP_FULL' && ! body psk_ipc_ask | grep -qE 'keep|promisc|cfilter' && echo 1 || echo 0)"
hits=$(body psk_ipc_unask | grep -nE 'capture_command|psk_session_|psk_do\(|start_capture|back_to_setup|PSK_ACT_|LOG_')
check "a cancel takes down a sheet and nothing else: it cannot reach a capture that was started" \
    "$([ -z "$hits" ] && body psk_ipc_unask | grep -q 'a->sheet_kind != PSK_SHEET_REQUEST' && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "and only the request it names: a late cancel leaves a newer request alone" \
    "$(body psk_ipc_unask | grep -q 'if (strcmp(a->req_token, token ? token->valuestring : "") != 0) {' &&
       body request_over | grep -q "a->req_token\[0\] = '\\\\0';" && echo 1 || echo 0)"
check "the request's sheet goes after 60 s, and at once when the unit is locked or an alarm shows" \
    "$(grep -q '^#define PSK_REQUEST_MS 60000$' $A/psk_app.h &&
       body request_watch | grep -q 'if (!pocketos_shell_app_in_front()) {' &&
       body request_watch | grep -q 'request_over(a, PSK_RR_TIMEOUT);' &&
       code $A/psk_app.c | grep -A3 '^    react(a, changed);' | grep -q 'request_watch(a);' && echo 1 || echo 0)"
check "START on a request's sheet is not taken in the sheet's first moment" \
    "$(grep -q '^#define PSK_REQUEST_SETTLE_MS 1500$' $A/psk_app.h &&
       body psk_sheet_answer | grep -q 'if (kind == PSK_SHEET_REQUEST && go && !request_settled(a)) {' &&
       code $A/ui/psk_sheets.c | grep -q 'psk_button_look(a->sheet_go, false, kind != PSK_SHEET_REQUEST);' && echo 1 || echo 0)"
check "a requested capture borrows the interface and the limit, and gives them back" \
    "$(body request_answer | grep -q 'a->req_saved_limit = a->limit_idx;' &&
       body back_to_setup | grep -q 'request_restore(a);' && body start_failed | grep -q 'request_restore(a);' && echo 1 || echo 0)"
check "START on the setup screen ends a request that is still asking" \
    "$(body psk_do | grep -A6 'case PSK_ACT_START:' | grep -q 'request_over(a, 0);' &&
       body request_watch | grep -q 'if (a->starting || psk_capture_present(a)) {' && echo 1 || echo 0)"
check "figures are raised only while the header says a companion is reading" \
    "$(body announce | grep -q 'if (!due || (due == PSK_REMOTE_DUE_STATS && !a->reading)) {' && echo 1 || echo 0)"
check "what a request names is held only for its sheet: cleared with every outcome" \
    "$(body request_over | grep -q "a->req_client\[0\] = '\\\\0';" && body request_over | grep -q "a->req_iface\[0\] = '\\\\0';" &&
       body request_answer | grep -q 'request_over(a, 0); /\* nothing of the request is kept past its sheet \*/' &&
       code $A/ui/psk_sheets.c | grep -q 'psk_text(a->sheet_text, "");' && echo 1 || echo 0)"
# ADR-016 rule 20: the header says that a companion is reading.
check "reading (capture.status, and no other request) marks a reader, and the header and the wide top row say so" \
    "$(body psk_ipc_request | grep -q 'if (a && strcmp(method, "capture.status") == 0) {' &&
       body psk_ipc_request | grep -q 'a->reader_ms = psk_now();' &&
       code $A/ui/psk_live.c | grep -q 'snprintf(h + at, sizeof(h) - at, " · COMPANION");' &&
       code $A/ui/psk_live.c | grep -q 'static const char mark\[\] = "COMPANION · ";' &&
       grep -q '^#define PSK_READER_MS 5000$' $A/psk_app.h && echo 1 || echo 0)"
hits=$(code $A/psk_app.c | grep -cE '\bcapture_command\(a, act\)')
check "which the capture bar and capture.hold / capture.stop both call" \
    "$([ "$hits" = "2" ] && body psk_do | grep -q 'capture_command(a, act);' &&
       body psk_ipc_command | grep -q 'told && capture_command(a, act) != 0' && echo 1 || echo 0)"
check "a request is refused unless the bar offers STOP, and does only what the bar offers" \
    "$(body psk_ipc_command | grep -q 'if (!psk_action_offered(a, PSK_ACT_STOP))' &&
       body psk_ipc_command | grep -q 'told = psk_action_offered(a, act);' && echo 1 || echo 0)"
hits=$(body psk_ipc_command | grep -nE 'psk_session_|psk_sheet|psk_do\(|psk_back|pocketos_shell_go_home|start_capture|back_to_setup|PSK_ACT_(START|DISCARD|NEW|SAVE)|LOG_')
check "a request cannot start, discard, save or leave a capture, and logs nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "and is refused under a sheet, by the rule the keys are" \
    "$(body psk_ipc_command | grep -q 'if (sheet_up(a)) {' &&
       body sheet_up | grep -qF 'a->sheet_kind != PSK_SHEET_NONE || psk_fsheet_shown(a) || psk_ssheet_shown(a) || psk_dsheet_shown(a)' &&
       echo 1 || echo 0)"
check "from START on, no setup row takes a tap: the interface, the options, a sheet answered late" \
    "$([ "$(body psk_select_iface | grep -c 'if (a->starting) {')" = "1" ] &&
       [ "$(body psk_option | grep -c 'if (a->starting) {')" = "1" ] &&
       [ "$(body psk_sheet_answer | grep -c 'if (!a->starting) {')" = "2" ] && echo 1 || echo 0)"
check "nothing is made to be raised while the app is not in front" \
    "$(body announce | grep -q 'if (!pocketos_shell_app_in_front()) {' && echo 1 || echo 0)"
hits=$(grep -ohE '"capture\.[a-z_]+"' $A/*.c $A/ui/*.c | LC_ALL=C sort -u | tr '\n' ' ')
check "the app knows five methods and two events, and no other capture.* name ($hits)" \
    "$([ "$hits" = '"capture.hold" "capture.request_cancel" "capture.request_start" "capture.state" "capture.stats" "capture.status" "capture.stop" ' ] && echo 1 || echo 0)"
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'pocketos_shell_app_event\(')
check "events are raised in three places: the poll's one a turn, and leaving a capture or a request" \
    "$([ "$hits" = "3" ] && [ "$(body announce | grep -c 'pocketos_shell_app_event(')" = "1" ] &&
       [ "$(body psk_destroy | grep -c 'pocketos_shell_app_event(')" = "2" ] && echo 1 || echo 0)"
check "every event is made by psk_remote, from the figures" \
    "$(body announce | grep -q 'data = due == PSK_REMOTE_DUE_STATE ? psk_remote_state_json(&f) : psk_remote_json(&f);' &&
       [ "$(body psk_destroy | grep -c 'data = psk_remote_state_json(&f);')" = "2" ] && echo 1 || echo 0)"
check "one method opens PacketShark, the start request, and only capture.status is answered for a closed app" \
    "$(code $A/psk_app.c | grep -q 'psk_ipc_opens\[\] = { "capture.request_start", NULL };' &&
       [ "$(code $A/*.c $A/ui/*.c | grep -c 'psk_ipc_opens')" = "2" ] &&
       code $A/psk_app.c | grep -q 'psk_ipc_status\[\] = { "capture.status", NULL };' && echo 1 || echo 0)"

# ---- lifecycle ------------------------------------------------------------------
check "the helper is spawned with PR_SET_PDEATHSIG and a parent check" \
    "$(grep -q 'PR_SET_PDEATHSIG' $A/psk_session.c && grep -q 'getppid() != parent' $A/psk_session.c && echo 1 || echo 0)"
check "the helper drops its capabilities and sets no_new_privs" \
    "$(grep -q 'PR_SET_NO_NEW_PRIVS' $H && grep -q 'SYS_capset' $H && grep -q 'PR_CAPBSET_DROP' $H && echo 1 || echo 0)"
check "the helper runs at nice 5 (DESIGN_DECISIONS D5)" "$(grep -q 'define HELPER_NICE 5' $H && echo 1 || echo 0)"
if [ -f ui/shell/shell.c ] && grep -q app_packetshark ui/shell/shell.c; then
    check "the screen stays on while PacketShark is in front (awake_apps)" \
        "$(grep -E 'awake_apps\[\] *=' ui/shell/shell.c | grep -q '"packetshark"' && echo 1 || echo 0)"
fi

echo "packetshark_lint: $failed failure(s)"
exit $((failed > 0))
