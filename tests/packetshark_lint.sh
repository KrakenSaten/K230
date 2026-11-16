#!/bin/bash
# PacketShark's boundaries (docs/apps/PACKETSHARK.md, ADR-016 accepted),
# held statically. tests/pos_capture_test.sh and the session and app tests
# hold the same rules at run time.
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
hits=$(grep -rlE '\bfork\(' $A $C tools/capture --include='*.c' | sort | tr '\n' ' ')
check "only the session starts a process ($hits)" "$([ "$hits" = "$A/psk_session.c " ] && echo 1 || echo 0)"

# ---- nothing written (D10, ADR-016 rules 8-9) -------------------------------------
hits=$(code $H $C/*.c | grep -nE 'O_CREAT|fopen\([^)]*"[wa]')
check "the helper creates one file, its lock, and writes no capture (M8 adds SAVE)" \
    "$([ "$(echo "$hits" | grep -c .)" = "1" ] && echo "$hits" | grep -q 'O_RDWR | O_CREAT' && echo 1 || echo 0)"
[ "$(echo "$hits" | grep -c .)" != "1" ] && echo "$hits" | head -5
hits=$(code $H $C/*.c $A/*.c | grep -nE '"/tmp')
check "nothing goes to /tmp" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $H | grep -E 'LOG_(INFO|WARN|ERROR|DEBUG)' | grep -E '\b(t|row|rec|r)\.(src|dst|info)|bytes|ppc_format' )
check "no log line is built from a packet's text (S7)" "$([ -z "$hits" ] && echo 1 || echo 0)"

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
