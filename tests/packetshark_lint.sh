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
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $C/*.[ch] $H $A/psk_session.[ch] $A/psk_model.[ch] $A/psk_net.[ch] \
       $A/psk_ev.[ch] 2>/dev/null)
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
# Marks and their evidence (N1.0, docs/apps/PACKETSHARK.md §4d) by the same
# rules: one allocation, no text, no loop without a bound, nothing of a
# socket; and it keeps figures and addresses, never a packet's bytes or a
# name it carried (HEADERS).
hits=$(code $C/ppc_evid.c | grep -nE '\b(malloc|calloc|realloc|printf|snprintf|sprintf)\(')
check "the evidence history allocates once, at init, and formats nothing (N1.0)" \
    "$([ "$(echo "$hits" | grep -c .)" = "1" ] && echo "$hits" | grep -q 'ev->mem = calloc(1, ' && echo 1 || echo 0)"
hits=$(code $C/ppc_evid.c | grep -nE '\bwhile *\(1\)|\bfor *\(;;\)|goto|#include <(sys/socket|netinet|arpa|unistd|time)')
check "it has no unbounded loop, no goto, no clock of its own and nothing of a socket in it" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(sed -n '/^struct ppc_ev_obs {/,/^};/p' $C/ppc_evid.h | grep -E ';' |
       grep -vE '^[[:space:]]*uint(8|32)_t [a-z0-9_]+(\[[0-9]\])?;[[:space:]]*(/\*.*)?$|^};')
check "an observation is 32 bytes of figures and addresses: no text, no bytes of the packet" \
    "$([ -z "$hits" ] && code $C/ppc_evid.h | grep -q "sizeof(struct ppc_ev_obs) == 32" && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $C/ppc_evid.c | grep -nE 'qname|->name\b|payload|->data\b|ppc_store_copy|ppc_dissect')
check "nothing of a DNS name or a payload reaches the history" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'psk_session_evid\(')
check "a mark's evidence is asked for in one function, and never while its page does not show" \
    "$([ "$hits" = "2" ] && sed -n -E '/^static void ev_ask\(/,/^}/p' $A/psk_app.c | grep -q 'psk_session_evid(a->session, a->model, a->ev_id)' &&
       sed -n -E '/^static void ev_poll\(/,/^}/p' $A/psk_app.c | grep -q 'if ((!psk_evp_shown(a) && !a->mark_wait) || !psk_capture_present(a)) {' &&
       echo 1 || echo 0)"
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'psk_session_mark\(')
check "a mark is set in one function, which sends the helper nothing else" \
    "$([ "$hits" = "2" ] && sed -n -E '/^void psk_mark\(/,/^}/p' $A/psk_app.c | grep -q 'psk_session_mark(a->session)' &&
       ! sed -n -E '/^void psk_mark\(/,/^}/p' $A/psk_app.c | grep -qE 'psk_session_(hold|stop|capture|save|discard|view)|capture_command|psk_do\(' &&
       echo 1 || echo 0)"
# What a mark's evidence may claim (the review of PR #188, 2026-11-25). The
# helper's body of a function, as the app's above.
hbody() { sed -n -E "/^(static )?[a-z ]+\*?$1\(/,/^}/p" $H; }
check "shedding is told to the evidence at both of its edges, not left to the 250 ms tick" \
    "$(hbody ingest | grep -B1 -A1 'h->shed = 1;' | grep -c 'ev_tick(h);' | grep -qx 2 &&
       hbody ingest | grep -B1 -A1 'h->shed = 0;' | grep -c 'ev_tick(h);' | grep -qx 2 && echo 1 || echo 0)"
check "and every frame that is only counted marks its own second" \
    "$(hbody on_frame | grep -A6 'if (h->shed) {' | grep -q 'ppc_evid_bypass(&h->ev, t_ms);' && echo 1 || echo 0)"
check "the evidence reports the capture filter only a live source applies, and the analyses that could not start" \
    "$(hbody cmd_start | grep -q 'ppc_evid_coverage(&h->ev, h->src.live ? c->cfilter : PPC_CFILTER_ALL,' &&
       hbody cmd_start | grep -q 'ppc_evid_unanalyzed(h->sig_ready, h->net_ready));' && echo 1 || echo 0)"
# What is lost is known only where the counters were read (the follow-up
# review of PR #188): one function reads them, and tells the history every
# reading, made or not.
check "the drop counters are read in one function, which tells the history whether each reading was made" \
    "$([ "$(code $H | grep -c 'ops->stats(')" = "1" ] && hbody read_drops | grep -q 'h->src.ops->stats(&h->src, &kd, &ifd) == 0' &&
       hbody read_drops | grep -q 'ppc_evid_counters(&h->ev, ev_tick(h), ok, h->st.kdrop + h->st.ifdrop);' &&
       ! hbody read_drops | grep -q 'return 0;' && echo 1 || echo 0)"
check "the lost-frame counters are read at the mark, before it is set" \
    "$(hbody cmd_mark | grep -n 'read_drops(h);\|ppc_evid_mark(' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'read_drops(h);.*ppc_evid_mark(' && echo 1 || echo 0)"
check "and for every answer of evidence, at STOP before the windows close, and on the 250 ms tick" \
    "$(hbody cmd_evid | grep -n 'read_drops(h);\|hd.now_ms = ev_tick(h);' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'read_drops(h);.*hd.now_ms = ev_tick(h);' &&
       hbody stop_capture | grep -n 'read_drops(h);\|ppc_evid_stop(' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'read_drops(h);.*ppc_evid_stop(' && hbody tick | grep -q 'read_drops(h);' && echo 1 || echo 0)"
# Least privilege before untrusted data (F1 hardening, ADR-016 rule 17
# amended): the helper leaves root for its own identity after the source
# opens and before a frame is read, and a failed reduction refuses the
# capture; SAVE reaches its folder only through the descriptor prepared
# before the drop; the failure switch exists only in the test build.
P=tools/capture/capture_priv.c
check "a start drops privileges after the source opens and before the capture state, and fails closed" \
    "$(hbody cmd_start | grep -n 'h->src_open = 1;\|capture_captures_dir(\|drop_privileges(h, why, sizeof(why))\|start_undo(h);\|say_error(h, \"privilege\", why);\|_exit(3);\|h->state = PPC_STATE_CAPTURING;' |
       cut -d: -f2 | tr '\n' ' ' |
       grep -q 'h->src_open = 1;.*capture_captures_dir(.*drop_privileges(h, why, sizeof(why)).*start_undo(h);.*say_error(h, "privilege", why);.*_exit(3);.*h->state = PPC_STATE_CAPTURING;' &&
       [ "$(code $H | grep -c 'drop_privileges(h')" = "1" ] && echo 1 || echo 0)"
check "frames are analysed only while capturing, so none before the drop" \
    "$(hbody on_frame | grep -q 'if (h->state != PPC_STATE_CAPTURING) {' && echo 1 || echo 0)"
check "the identity switch: no_new_privs, bounding set, own network namespace, groups, gid, uid, capabilities, checks read back" \
    "$(code $P | grep -n 'PR_SET_NO_NEW_PRIVS, 1\|PR_CAPBSET_DROP, cap\|unshare(CLONE_NEWNET)\|setgroups(0, NULL)\|setresgid(gid, gid, gid)\|setresuid(id, id, id)\|SYS_capset\|read_status(p)\|network namespace check\|setresuid(0, 0, 0)' |
       cut -d: -f2 | tr '\n' ' ' |
       grep -q 'PR_SET_NO_NEW_PRIVS, 1.*PR_CAPBSET_DROP, cap.*unshare(CLONE_NEWNET).*setgroups(0, NULL).*setresgid(gid, gid, gid).*setresuid(id, id, id).*SYS_capset.*read_status(p).*network namespace check.*setresuid(0, 0, 0)' &&
       echo 1 || echo 0)"
check "the parent-death signal is set again after the identity change" \
    "$(hbody drop_privileges | grep -n 'capture_drop(&p)\|prctl(PR_SET_PDEATHSIG, SIGTERM' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'capture_drop(&p).*prctl(PR_SET_PDEATHSIG, SIGTERM' && echo 1 || echo 0)"
check "the log keeps its descriptor across the drop" \
    "$(hbody drop_privileges | grep -n 'pocketlog_keep();\|capture_drop(&p)' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'pocketlog_keep();.*capture_drop(&p)' && echo 1 || echo 0)"
# The system call filter (amendment 3): the drop's last step, after the
# parent-death signal and its check; once, from there alone; ends what it
# does not list, with no logging, tracing or user-notification action that
# would let a call through; the network side, copies of descriptors,
# execution and prctl are never in its plain allow list; after STOP the
# socket's number stays taken.
cpbody() { sed -n -E "/^(static )?[a-z ]+\*?$1\(/,/^}/p" $P; }
check "the system call filter is the drop's last step, after the parent-death signal and its check, and fails closed" \
    "$(hbody drop_privileges | grep -n 'capture_drop(&p)\|prctl(PR_SET_PDEATHSIG, SIGTERM\|getppid() != h->parent\|capture_confine(sfd, &p)\|return 0;' |
       cut -d: -f2 | tr '\n' ' ' |
       grep -q 'capture_drop(&p).*prctl(PR_SET_PDEATHSIG, SIGTERM.*getppid() != h->parent.*capture_confine(sfd, &p).*return 0;' &&
       [ "$(code $H | grep -c 'capture_confine(')" = "1" ] && echo 1 || echo 0)"
check "the filter checks the architecture, ends whatever it does not list, and has no log, trace, trap or notify action" \
    "$(cpbody build_filter | grep -q 'offsetof(struct seccomp_data, arch)' &&
       cpbody build_filter | code /dev/stdin | tail -3 | tr -d '\n ' | grep -q 'EMIT(F_RET(SECCOMP_RET_KILL_PROCESS));returnn;}' &&
       ! code $P | grep -qE 'SECCOMP_RET_(LOG|TRACE|TRAP|USER_NOTIF)' && code $P | grep -q 'SECCOMP_FILTER_FLAG_TSYNC' &&
       echo 1 || echo 0)"
SY=platforms/k230/rootfs_overlay/etc/sysctl.d/60-doors-links.conf
check "the image protects hard and symbolic links (fs.protected_hardlinks and fs.protected_symlinks = 1, nothing else)" \
    "$([ "$(grep -vE '^[[:space:]]*(#|$)' $SY | tr -d ' ')" = "$(printf 'fs.protected_hardlinks=1\nfs.protected_symlinks=1')" ] &&
       echo 1 || echo 0)"
check "the leak-scan opt-out exists only in sanitizer builds, and only once the filter is on" \
    "$([ "$(code $P | grep -c '__lsan_is_turned_off\|confined')" = "$(sed -n '/^#if defined(__SANITIZE_ADDRESS__)$/,/^#endif$/p' $P | code /dev/stdin | grep -c '__lsan_is_turned_off\|confined')" ] &&
       sed -n '/^#if defined(__SANITIZE_ADDRESS__)$/,/^#endif$/p' $P | grep -q 'confined = 1;' && echo 1 || echo 0)"
plain=$(sed -n '/^static const int allow_plain\[\] = {$/,/^};$/p' $P | code /dev/stdin)
check "the plain allow list never names the network side, a descriptor copy, execution, prctl, ptrace or memory protection" \
    "$([ -n "$plain" ] &&
       ! echo "$plain" | grep -qwE 'SYS_(socket|socketpair|bind|connect|listen|accept|accept4|setsockopt|sendto|sendmsg|sendmmsg|write|writev|pwrite64|pwritev|pwritev2|ioctl|dup|dup2|dup3|fcntl|execve|execveat|prctl|ptrace|mmap|mprotect|seccomp|process_vm_writev|splice|vmsplice|tee|sendfile|copy_file_range|io_uring_setup|io_submit|pidfd_getfd|pidfd_open|clone|clone3|fork|vfork|kill|tgkill|tkill|linkat|symlinkat|mknodat|mkdirat)' &&
       echo 1 || echo 0)"
check "the filter is built only through EMIT, which never writes past its array; tgkill goes to the helper itself alone" \
    "$(! cpbody build_filter | code /dev/stdin | grep -q 'f\[n++\]' && code $P | grep -q 'if (n >= max) {' &&
       cpbody build_filter | grep -q 'SYS_tgkill, 0, 4' && cpbody build_filter | grep -q '(uint32_t)self, 0, 1' &&
       code $P | grep -q 'build_filter(f, PROG_MAX, capture_fd, getpid())' && echo 1 || echo 0)"
check "writes and sendto are refused on the capture socket, sendto to any address; after STOP its number stays taken" \
    "$(cpbody build_filter | grep -q 'SYS_write, 1, 0' && cpbody build_filter | grep -q 'SYS_sendto, 0, 8' &&
       cpbody build_filter | grep -q 'F_LD(ARG_LO(4))' &&
       hbody stop_capture | grep -n 'ppc_source_close(&h->src);\|capture_reserve_fd(h->conf_fd)' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'ppc_source_close(&h->src);.*capture_reserve_fd(h->conf_fd)' && echo 1 || echo 0)"
hits=$(code $H | grep -nE '\b(open|unlink|rename|lstat|statvfs)\(h->save_' )
check "SAVE writes, renames and removes only through the captures folder's descriptor" \
    "$([ -z "$hits" ] && code $H | grep -q 'openat(h->cap_dirfd, h->save_part' &&
       code $H | grep -q 'renameat(h->cap_dirfd, h->save_part, h->cap_dirfd, h->save_path)' && echo 1 || echo 0)"
check "the privilege-failure switch is in the test build alone" \
    "$([ "$(code $H | grep -c 'POS_CAPTURE_TEST_DROP_FAIL')" = "1" ] &&
       sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $H | grep -q 'POS_CAPTURE_TEST_DROP_FAIL' &&
       sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $P | grep -q 'test_fail' &&
       [ "$(code $P | grep -c 'test_fail')" = "$(sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $P | code /dev/stdin | grep -c 'test_fail')" ] &&
       echo 1 || echo 0)"
check "a session closes every inherited descriptor, however high, before it opens its own" \
    "$(sed -n '/strcmp(argv\[1\], \"session\")/,/run_session(spec)/p' $H | grep -n 'close_inherited();\|pocketlog_init(' | cut -d: -f2 | tr '\n' ' ' |
       grep -q 'close_inherited();.*pocketlog_init(' && hbody close_inherited | grep -q 'SYS_close_range, 3u, ~0u' &&
       hbody close_inherited | grep -q '_SC_OPEN_MAX' && echo 1 || echo 0)"
check "a captures folder is handed to the identity only if just made, already its own, or a root-only Captures" \
    "$(code $P | grep -q 'int may = ours || (sb.st_uid == 0 && (created || captures_by_name(fd, dir)));' &&
       code $P | grep -q 'strcmp(base, \"Captures\") == 0' && code $P | grep -q '(psb.st_mode & 022) == 0' && echo 1 || echo 0)"
check "the saved captures list only single-name regular files: no link, no second name of a file elsewhere" \
    "$(code $A/psk_saves.c | grep -q '!S_ISREG(sb.st_mode) || sb.st_nlink != 1' && echo 1 || echo 0)"
check "the counter-failure switch is in the test build alone: read once, inside #if POS_CAPTURE_TEST_HOOKS" \
    "$([ "$(code $H | grep -c 'getenv("POS_CAPTURE_TEST_STATS_FAIL")')" = "1" ] &&
       sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $H | grep -q 'getenv("POS_CAPTURE_TEST_STATS_FAIL")' &&
       [ "$(code $H | grep -c 'h->test_fail')" = "$(sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $H | code /dev/stdin | grep -c 'h->test_fail')" ] &&
       echo 1 || echo 0)"
# The window after's count of lost frames closes with a reading, not with a
# tick (the closure check of PR #188): final only after a reading at or past
# its end, or STOP; a reading past it closes the count before it is booked.
cbody() { sed -n -E "/^(static )?[a-z ]+\*?$1\(/,/^}/p" $C/ppc_evid.c; }
check "a window's count of lost frames is final only after a reading at or past its end, or STOP; a reading past it closes the count before it is booked" \
    "$([ "$(code $C/ppc_evid.c | grep -c 'loss_done = 1;')" = "2" ] &&
       cbody after_refresh | grep -q 'if (ev->stopped || ev->read_ms >= end) {' &&
       cbody ppc_evid_counters | grep -n 'after_refresh(ev, m, end);\|m->side\[1\].lost_near += lost;\|second(ev, now_ms / 1000u)' |
       cut -d: -f2- | tr '\n' ' ' | grep -q 'after_refresh(ev, m, end);.*lost_near += lost;.*second(ev, now_ms / 1000u)' &&
       echo 1 || echo 0)"
# And it is counted as each reading is told, never again from the 64-second
# ring of seconds, which a spell of failed readings can outlast.
check "the window after's count of lost frames is kept as readings are told, never taken again from the ring of seconds" \
    "$(! cbody after_refresh | code /dev/stdin | grep -q 'lost_in(\|s->lost = ' &&
       cbody ppc_evid_counters | grep -q 'm->side\[1\].lost += lost;' &&
       cbody spell_to_mark | grep -q 'm->side\[1\].lost += sp->lost;' && echo 1 || echo 0)"
check "the analysis-off switch is in the test build alone: read once, inside #if POS_CAPTURE_TEST_HOOKS, which is 0 unless the test object sets it"     "$([ "$(code $H | grep -c 'getenv("POS_CAPTURE_TEST_NO_ANALYZER")')" = "1" ] &&
       sed -n '/^#if POS_CAPTURE_TEST_HOOKS$/,/^#endif$/p' $H | grep -q 'getenv("POS_CAPTURE_TEST_NO_ANALYZER")' &&
       grep -q '^#define POS_CAPTURE_TEST_HOOKS 0$' $H && grep -q '^#define POS_CAPTURE_TEST_HOOKS 0$' tools/capture/capture_priv.h &&
       [ "$(grep -c 'DPOS_CAPTURE_TEST_HOOKS=1' Makefile)" = "4" ] &&
       grep -q '^tests/pos_capture_hooks.o: tools/capture/pos_capture.c$' Makefile &&
       grep -q '^tests/capture_priv_hooks.o: tools/capture/capture_priv.c' Makefile &&
       ! grep -rq 'POS_CAPTURE_TEST_HOOKS' platforms ui/shell/CMakeLists.txt && echo 1 || echo 0)"
# A window whose losses are not known never says every frame was seen; the
# page names its capture filter every time it is painted.
check "a window says its losses are not known before it may say every frame was seen" \
    "$(sed -n -E '/^unsigned psk_ev_window\(/,/^}/p' $A/psk_ev.c | grep -n '"LOSSES UNKNOWN"\|EVERY FRAME SEEN' | cut -d: -f2- |
       tr '\n' ' ' | grep -q '"LOSSES UNKNOWN".*EVERY FRAME SEEN' &&
       sed -n -E '/^unsigned psk_ev_window\(/,/^}/p' $A/psk_ev.c | grep -q 'sev = w->shed_s || loss_limited(ev, side)' &&
       echo 1 || echo 0)"
check "the evidence page names its capture filter on every paint, never hidden" \
    "$(sed -n -E '/^void psk_evp_paint\(/,/^}/p' $A/ui/psk_evidence.c | grep -q 'psk_text(a->ev_filter, psk_ev_filter(ev, b, sizeof(b)));' &&
       ! grep -q 'psk_set_hidden(a->ev_filter' $A/ui/psk_evidence.c && echo 1 || echo 0)"
# The page never calls a protocol it could not see quiet.
check "a protocol the capture could not see is said as NOT CAPTURED or NOT ANALYZED, never as NONE SEEN" \
    "$(sed -n -E '/^unsigned psk_ev_counts\(/,/^}/p' $A/psk_ev.c | grep -q '"NOT CAPTURED" : "NOT ANALYZED"' &&
       sed -n -E '/^unsigned psk_ev_counts\(/,/^}/p' $A/psk_ev.c | grep -B12 'NOT CAPTURED' | grep -q 'continue;\|if ((excluded | unanalyzed) & bit)' &&
       echo 1 || echo 0)"
# The page's objects are off the active screen while it does not show: a
# layout pass walks every object of the active screen, hidden or not.
hits=$(code $A/*.c $A/ui/*.c | grep -cE 'lv_obj_set_parent\(a->ev_page,')
check "the evidence page is put on and taken off the active screen in one function, and its own screen goes with the app" \
    "$([ "$hits" = "1" ] && sed -n -E '/^void psk_evp_place\(/,/^}/p' $A/ui/psk_evidence.c | grep -q 'lv_obj_t \*home = shown ? a->content : a->ev_park;' &&
       code $A/ui/psk_live.c | grep -q 'psk_evp_place(a);' &&
       sed -n -E '/^static void psk_destroy\(/,/^}/p' $A/psk_app.c | grep -q 'psk_evp_destroy(a);' && echo 1 || echo 0)"
# What the page says is made in one file, and that file scores nothing.
hits=$(code $A/psk_ev.c | grep -oE '"[^"]*"' | grep -niE 'missing|unanswered|not answered|no reply|no answer|fault|score|confidence|health|likely|probably|diagnos|%%')
check "the evidence's words name no missing reply, no fault, no score and no percentage" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $A/ui/psk_evidence.c | grep -oE '"[^"]*"' | grep -vE '^"(psk_app\.h|psk_ev\.h|%s|%u|! %s|%s" DOT "PACKET %s|)"$' |
       grep -viE '^"(Mark|BACK|BEFORE|AFTER|WHAT WAS WATCHED|OBSERVED|MESSAGES COUNTED|OBSERVATIONS|TAP FOR THE PACKET|THE MARK|PACKET %s|DHCP, ARP AND DNS IN PACKETS|WHAT THIS CAN AND CANNOT TELL|Reading what the capture has seen\.| \\xc2\\xb7 |%s)"$')
check "the page itself adds headings and no sentence of its own to them" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -8
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
check "the helper leaves root for its own identity, drops its capabilities and sets no_new_privs (F1 hardening)" \
    "$(P=tools/capture/capture_priv.c; grep -q 'PR_SET_NO_NEW_PRIVS' $P && grep -q 'SYS_capset' $P && grep -q 'PR_CAPBSET_DROP' $P &&
       grep -q 'setresuid(id, id, id)' $P && grep -q 'define POS_CAPTURE_ID 65532' tools/capture/capture_priv.h &&
       grep -q 'capture_drop(&p)' $H && echo 1 || echo 0)"
check "the helper runs at nice 5 (DESIGN_DECISIONS D5)" "$(grep -q 'define HELPER_NICE 5' $H && echo 1 || echo 0)"
if [ -f ui/shell/shell.c ] && grep -q app_packetshark ui/shell/shell.c; then
    check "the screen stays on while PacketShark is in front (awake_apps)" \
        "$(grep -E 'awake_apps\[\] *=' ui/shell/shell.c | grep -q '"packetshark"' && echo 1 || echo 0)"
fi

echo "packetshark_lint: $failed failure(s)"
exit $((failed > 0))
