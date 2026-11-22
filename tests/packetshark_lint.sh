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
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $C/*.[ch] $H $A/psk_session.[ch] $A/psk_model.[ch] 2>/dev/null)
check "the library, the helper, the session and the model are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
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
check "the JSON is written from struct psk_remote_figures, which the model is not in" "$([ "$hits" = "0" ] && echo 1 || echo 0)"
hits=$(sed -n '/^struct psk_remote_figures {/,/^};/p' $A/psk_remote.h | grep -E ';' |
       grep -vE '^[[:space:]]*(uint8_t|uint32_t|uint64_t|bool) [a-z_]+;[[:space:]]*(/\*.*\*/)?$|^[[:space:]]*uint64_t [a-z_]+\[[A-Z_]+\];[[:space:]]*(/\*.*\*/)?$|^[[:space:]]*char iface\[PPC_IFACE_MAX\];[[:space:]]*(/\*.*\*/)?$|^};$')
check "the figures are numbers, a flag and one interface name" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
# Every text that can reach the answer: the keys and the enumerated words.
hits=$(code $R | grep -vE '^#include' | grep -oE '"[^"]*"' | LC_ALL=C sort -u | tr '\n' ' ')
want='"arp" "available" "bps" "bytes" "capturing" "classes" "dhcp" "diag" "dns" "dns_answered" "dns_late" "dns_noreply" "dns_nxdomain" "dns_queries" "dns_refused" "dns_retries" "dns_rtt_max_us" "dns_rtt_sum_us" "dns_servfail" "dns_waiting" "elapsed_ms" "end" "ended" "error" "eth" "fill_permille" "findings" "full" "headers" "held" "helper_crashed" "helper_hung" "hold" "icmp" "idle" "iface" "iface_drops" "ipv6" "keep" "kernel_drops" "limit" "limit_s" "link" "none" "other" "pps" "reason" "requested_by_companion" "saving" "seen" "shed" "state" "stopped" "stored" "synth" "tcp" "tcp_aborted" "tcp_attempts" "tcp_connected" "tcp_hs_max_us" "tcp_hs_sum_us" "tcp_refused" "tcp_timed" "udp" "usb" "user" "wlan" '
check "its keys and words are the contract's list, to the letter" "$([ "$hits" = "$want" ] && echo 1 || echo 0)"
[ "$hits" = "$want" ] || echo "     $hits"
hits=$(grep -lE 'psk_remote_(take|json)\(' $A/*.c $A/ui/*.c ui/shell/*.c 2>/dev/null | LC_ALL=C sort | tr '\n' ' ')
check "one caller: the app's capture.status ($hits)" "$([ "$hits" = "$A/psk_app.c $A/psk_remote.c " ] && echo 1 || echo 0)"
# The handler reads: it sends the helper nothing and touches no part of the screen.
hits=$(sed -n '/^static int psk_ipc_request/,/^}/p' $A/psk_app.c |
       grep -nE 'psk_session_|psk_do\(|psk_sheet|psk_select|psk_set_|psk_open|lv_|psk_text|LOG_|a->[a-z_]+ *=[^=]')
check "capture.status sends the helper nothing, changes nothing and logs nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "its handler takes the app as const" \
    "$(sed -n '/^static int psk_ipc_request/,/^}/p' $A/psk_app.c | grep -q 'const struct psk_app \*a = priv;' && echo 1 || echo 0)"
hits=$(grep -ohE '"capture\.[a-z_]+"' $A/*.c $A/ui/*.c | LC_ALL=C sort -u | tr '\n' ' ')
check "capture.status is the one method the app knows ($hits)" "$([ "$hits" = '"capture.status" ' ] && echo 1 || echo 0)"
check "no method opens PacketShark, and it raises no event" \
    "$(code $A/*.c $A/ui/*.c | grep -qE 'ipc_opens|pocketos_shell_app_event' && echo 0 || echo 1)"

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
