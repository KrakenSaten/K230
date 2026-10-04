#!/bin/bash
# RIFT's boundaries, checked by inspection rather than by intention.
#
# The app is a client and nothing else. It owns no colour, no radio, no
# protocol and no file; it must not transmit; and the one thing this phase
# must not quietly become is a monolith with the model, the socket and the
# LVGL in one file, which is the failure mode the handoff names outright.
#
# It also records what RIFT does not have yet, so a gap is a line in a test
# rather than a sentence in a report somebody has to remember.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
SRC="apps/rift"

# ---- the app exists, and is where it says it is --------------------------------
check "the app lives under apps/rift" "$([ -d "$SRC" ] && echo 1 || echo 0)"
check "the shell knows about RIFT" \
    "$(grep -q 'app_rift' ui/shell/shell.c && echo 1 || echo 0)"
check "and builds it" \
    "$(grep -q 'apps/rift/rift_app.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the parts without a display are built and tested by the root Makefile" \
    "$(grep -q 'tests/rift_model_test' Makefile && grep -q 'tests/rift_ipc_test' Makefile &&
       echo 1 || echo 0)"

# ---- what may transmit, and from where -----------------------------------------
# The API has exactly two methods that put a packet on the air (docs/api/
# mesh.md). Phase 1 named neither. Phase 2 sends messages, so the gate is no
# longer "never" - it is "from one place, on purpose, and never on its own".
#
# mesh.advert follows the same rule since the ADVERT buttons: named once, in
# the client; one function writes it; only SYSTEM's two ADVERT buttons call that
# function; and nothing on a timer, a poll, a snapshot or the app's creation
# reaches it.
advhits=$(grep -rln '"mesh.advert"' "$SRC" | sort | tr '\n' ' ')
check "mesh.advert is named only in the meshcored client (${advhits:-nowhere})" \
    "$([ "$advhits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
check "and only one call writes it" \
    "$([ "$(grep -c 'RIFT_REQ_ADVERT, params' "$SRC/rift_ipc.c")" = "1" ] && echo 1 || echo 0)"
advcallers=$(grep -rln 'rift_ipc_send_advert' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "the advert is called only from SYSTEM's DEVICE panel (${advcallers:-nowhere})" \
    "$([ "$advcallers" = "$SRC/rift_ipc.c $SRC/ui/rift_system.c " ] && echo 1 || echo 0)"
check "and only from the handler of a button a reader pressed" \
    "$([ "$(grep -c 'rift_ipc_send_advert(' "$SRC/ui/rift_system.c")" = "1" ] &&
       grep -B 12 'rift_ipc_send_advert(' "$SRC/ui/rift_system.c" |
       grep -q 'static void on_advert(lv_event_t' && echo 1 || echo 0)"
# Forgetting a node, or its route, transmits nothing but changes what the
# service holds, so it is held to the same rule: named once, called only from
# the node's detail - and forgetting only after the reader confirmed it.
nodehits=$(grep -rlnE '"mesh\.node_(remove|reset_path)"' "$SRC" | sort | tr '\n' ' ')
check "the node changes are named only in the meshcored client (${nodehits:-nowhere})" \
    "$([ "$nodehits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
nodecallers=$(grep -rlnE 'rift_ipc_(forget_node|reset_path)' "$SRC" --include='*.c' |
              sort | tr '\n' ' ')
check "and are called only from the node's detail (${nodecallers:-nowhere})" \
    "$([ "$nodecallers" = "$SRC/rift_ipc.c $SRC/ui/rift_detail.c " ] && echo 1 || echo 0)"
check "a node is forgotten only from the confirmation, never from the first press" \
    "$([ "$(grep -c 'rift_ipc_forget_node(' "$SRC/ui/rift_detail.c")" = "1" ] &&
       grep -B 12 'rift_ipc_forget_node(' "$SRC/ui/rift_detail.c" |
       grep -q 'static void on_forget_confirm(lv_event_t' && echo 1 || echo 0)"
# mesh.send is named once, in the client, and nowhere else - not in a screen,
# not in the model, not in the chrome.
sendhits=$(grep -rln '"mesh.send"' "$SRC" | sort | tr '\n' ' ')
check "mesh.send is named only in the meshcored client (${sendhits:-nowhere})" \
    "$([ "$sendhits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
check "and only one call writes it" \
    "$([ "$(grep -c 'RIFT_REQ_SEND, params' "$SRC/rift_ipc.c")" = "1" ] && echo 1 || echo 0)"
# Nothing automatic may reach the transmit. The only callers of the send are
# the two composers - the portrait SEND button and the landscape command
# line - both of which go through rift_comms_submit.
callers=$(grep -rln 'rift_ipc_send_message' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "the send is called only from COMMS (${callers:-nowhere})" \
    "$([ "$callers" = "$SRC/rift_ipc.c $SRC/ui/rift_comms.c " ] && echo 1 || echo 0)"
check "no timer, poll or create path sends or adverts" \
    "$(grep -nE 'rift_ipc_send_(message|advert)' "$SRC/rift_app.c" >/dev/null 2>&1 && echo 0 ||
       echo 1)"
# Everything that reaches the submit passes it the contents of a text field.
# A call with a string literal or a built buffer would be this app choosing
# what goes on the air, which is the thing it must never do.
badsubmit=$(grep -rn 'rift_comms_submit(' "$SRC" --include='*.c' |
            grep -v 'lv_textarea_get_text' | grep -v 'void rift_comms_submit')
check "every send carries text a reader typed${badsubmit:+ (}${badsubmit:+)}" \
    "$([ -z "$badsubmit" ] && echo 1 || echo 0)"

# ---- repeater control: four more ways to transmit, held to the same rule -------
# SCAN 0-HOP (mesh.discover), LOGIN (mesh.remote_login), STATUS / NEIGHBOURS /
# VERSION (mesh.remote_request) and a repeater command (mesh.remote_cli) each
# put a packet on the air. Each is named once in the client, written by one
# call, and reached only from the handler of a button a reader pressed; a
# command that changes the repeater only from its confirmation; and nothing
# automatic reaches any of them. Logout transmits nothing.
for m in mesh.remote_login mesh.remote_request mesh.remote_cli; do
    hits=$(grep -rln "\"$m\"" "$SRC" | sort | tr '\n' ' ')
    check "$m is named only in the meshcored client (${hits:-nowhere})" \
        "$([ "$hits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
done
# mesh.discover is also the name of the EVENT a round raises, which the
# repeater block reads (rift_repeater.c) - and that file writes nothing.
hits=$(grep -rln '"mesh.discover"' "$SRC" | sort | tr '\n' ' ')
check "mesh.discover is named only by the client and, as an event, the repeater block (${hits:-nowhere})" \
    "$([ "$hits" = "$SRC/rift_ipc.c $SRC/rift_repeater.c " ] &&
       ! grep -q 'rift_ipc_' "$SRC/rift_repeater.c" && echo 1 || echo 0)"
RIPC="$SRC/rift_ipc_repeater.c"
check "and each is written by one call" \
    "$([ "$(grep -c 'RIFT_REQ_DISCOVER, NULL' "$RIPC")" = 1 ] &&
       [ "$(grep -c 'RIFT_REQ_REMOTE_LOGIN, RIFT_REP_LOGIN' "$RIPC")" = 1 ] &&
       [ "$(grep -c 'RIFT_REQ_REMOTE_REQUEST, kind' "$RIPC")" = 1 ] &&
       [ "$(grep -c 'RIFT_REQ_REMOTE_CLI, RIFT_REP_CLI' "$RIPC")" = 1 ] && echo 1 || echo 0)"
caller_ok() { # <function> <screen file> <handler...>: called there, only from those handlers
    local fn=$1 file=$2; shift 2
    local callers
    callers=$(grep -rln "$fn(" "$SRC" --include='*.c' | sort | tr '\n' ' ')
    [ "$callers" = "$RIPC $SRC/$file " ] || return 1
    local n_calls n_ok=0 h
    n_calls=$(grep -c "$fn(" "$SRC/$file")
    for h in "$@"; do
        n_ok=$((n_ok + $(sed -n "/^static void $h(/,/^}/p" "$SRC/$file" | grep -c "$fn(")))
    done
    [ "$n_calls" -ge 1 ] && [ "$n_ok" = "$n_calls" ]
}
check "SCAN 0-HOP is asked only from its button" \
    "$(caller_ok rift_ipc_scan_repeaters ui/rift_scan.c on_scan && echo 1 || echo 0)"
check "a login only from LOGIN (or Enter in its field)" \
    "$(caller_ok rift_ipc_repeater_login ui/rift_repeater_view.c on_login && echo 1 || echo 0)"
check "STATUS, NEIGHBOURS and VERSION only from their buttons" \
    "$(caller_ok rift_ipc_repeater_ask ui/rift_repeater_view.c on_ask && echo 1 || echo 0)"
check "a command only from the console, read-only at once and the rest from its confirmation" \
    "$(caller_ok rift_ipc_repeater_cli ui/rift_repeater_cmd.c submit on_confirm_send &&
       sed -n '/^static void submit(/,/^}/p' "$SRC/ui/rift_repeater_cmd.c" |
       grep -B3 'rift_ipc_repeater_cli(' | grep -q 'case RIFT_CLI_READ' && echo 1 || echo 0)"
check "and the client refuses a command the rule refuses, whoever asks" \
    "$(sed -n '/^int rift_ipc_repeater_cli(/,/^}/p' "$RIPC" | grep -q 'RIFT_CLI_REFUSED' &&
       echo 1 || echo 0)"
check "no timer, poll or create path scans, logs in, asks or commands" \
    "$(grep -nE 'rift_ipc_(scan_repeaters|repeater_(login|ask|cli))' "$SRC/rift_app.c" \
        "$SRC/rift_background.c" "$SRC/rift_ipc.c" >/dev/null 2>&1 && echo 0 || echo 1)"
check "a repeater password is held by no model, store or client field" \
    "$(grep -qiE 'password\[|passwd|pw\[' "$SRC/rift_repeater.h" "$SRC/rift_model.h" \
        "$SRC/rift_ipc.h" "$SRC/rift_store.h" && echo 0 || echo 1)"
check "and the login field is wiped when it is used and when the page goes" \
    "$(sed -n '/^static void on_login(/,/^}/p' "$SRC/ui/rift_repeater_view.c" |
       grep -q 'scrub_field' &&
       sed -n '/^void rift_repeater_view_cancel(/,/^}/p' "$SRC/ui/rift_repeater_view.c" |
       grep -q 'scrub_field' && echo 1 || echo 0)"
check "leaving RIFT ends a repeater session" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" | grep -q 'rift_ipc_repeater_leave' &&
       sed -n '/^void rift_bg_end/,/^}/p' "$SRC/rift_background.c" | grep -q 'rift_ipc_repeater_leave' &&
       echo 1 || echo 0)"
for part in rift_repeater.c rift_ipc_repeater.c; do
    check "$part knows nothing about LVGL" \
        "$(grep -q 'lvgl' "$SRC/$part" && echo 0 || echo 1)"
done

# ---- MAP: positions as the nodes claim them, and nothing fetched ---------------
check "MAP fetches nothing: no tile, map service, URL or API key" \
    "$(grep -niE 'https?://|tile|mapbox|google|openstreetmap|api[_-]?key|curl_|socket\(' \
        "$SRC/rift_map.c" "$SRC/rift_map.h" "$SRC/ui/rift_mapview.c" "$SRC/ui/rift_mapview.h" |
       grep -viE 'no tile|no basemap|tiles, no|no tiles' >/dev/null && echo 0 || echo 1)"
check "and places only nodes that claimed a location" \
    "$(grep -c 'have_location' "$SRC/rift_map.c" | awk '{print ($1 >= 3) ? 1 : 0}')"

# ---- RIFT owns no colour, no font and no hardware ------------------------------
# tests/style_lint.sh covers ui/ and apps/ for colour literals; these are the
# rules that are RIFT's own.
check "no colour is named in the app" \
    "$(grep -rnE 'lv_color_hex|lv_palette_|0x[0-9a-fA-F]{6}\b' "$SRC" --include='*.c' \
        --include='*.h' >/dev/null 2>&1 && echo 0 || echo 1)"
# /dev/null is where the sound helper's stderr goes; it is not a device.
check "no device path or GPIO is touched" \
    "$(grep -rnE '/dev/|gpiod_|spidev' "$SRC" | grep -v '"/dev/null"' >/dev/null 2>&1 &&
       echo 0 || echo 1)"
# The prose here names MeshCore constantly - it is the protocol on the other
# end of the API - so what is checked is what is *included* and what is
# *called*, not what is written about.
check "no radio or protocol library is included" \
    "$(grep -rnE '#include[[:space:]]*[<\"].*(RadioLib|SX126|Mesh|Dispatcher|Packet)' "$SRC" \
        >/dev/null 2>&1 && echo 0 || echo 1)"
check "and no MeshCore symbol is called" \
    "$(grep -rnE 'mesh::|Mesh[A-Za-z]*\(' "$SRC" --include='*.c' --include='*.h' \
        >/dev/null 2>&1 && echo 0 || echo 1)"
# RIFT keeps the reader's own choices and nothing else: no message, node,
# key or read mark is ever written. One store, and the only file I/O in the
# app is in it.
stores=$(ls "$SRC"/*store*.[ch] "$SRC"/ui/*store*.[ch] 2>/dev/null | sort | tr '\n' ' ')
check "the app stores only the reader's preferences (${stores:-nothing})" \
    "$([ "$stores" = "$SRC/rift_store.c $SRC/rift_store.h " ] && echo 1 || echo 0)"
# The other file RIFT writes is not a store: the two short WAV files its
# sounds are played from, made from code into the runtime directory (a
# tmpfs) by the sound backend, and nothing about the mesh in them.
fileio=$(grep -rlE '\bfopen\(|\brename\(|\bunlink\(' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "and the only files it writes are that one and its two sounds (${fileio:-none})" \
    "$([ "$fileio" = "$SRC/rift_sound_helper.c $SRC/rift_store.c " ] && echo 1 || echo 0)"
check "the sounds are written to the runtime directory, never the state directory" \
    "$(grep -q 'pocketos_runtime_dir()' "$SRC/rift_sound_helper.c" &&
       ! grep -q 'pocketos_state_dir' "$SRC/rift_sound_helper.c" && echo 1 || echo 0)"
check "which holds nothing about the mesh" \
    "$(grep -qE 'struct rift_(model|message|node|conv)|peer_key|self_key|read_mark|cJSON' \
        "$SRC/rift_store.c" "$SRC/rift_store.h" && echo 0 || echo 1)"
check "and creates no keyboard: there is one and the shell owns it" \
    "$(grep -rq 'pos_keyboard_create' "$SRC" && echo 0 || echo 1)"

# ---- the split the handoff asked for -------------------------------------------
check "the model has no LVGL in it" \
    "$(grep -q 'lvgl' "$SRC/rift_model.c" "$SRC/rift_model.h" && echo 0 || echo 1)"
check "neither has the formatting" \
    "$(grep -q 'lvgl' "$SRC/rift_format.c" "$SRC/rift_format_msg.c" "$SRC/rift_format.h" &&
       echo 0 || echo 1)"
check "nor the meshcored client" \
    "$(grep -q 'lvgl' "$SRC/rift_ipc.c" "$SRC/rift_ipc.h" && echo 0 || echo 1)"
check "the formatting does no I/O" \
    "$(grep -qE 'fopen|socket|read\(|write\(' "$SRC/rift_format.c" "$SRC/rift_format_msg.c" &&
       echo 0 || echo 1)"
check "and the screens parse no JSON of their own" \
    "$(grep -rq 'cJSON_Parse' "$SRC/ui" && echo 0 || echo 1)"

# One screen to a file, and no file that is all of them. 900 lines is well
# above anything here and well below a monolith. The generated emoji tables
# (tools/design/gen_rift_emoji.js) are data, not code, and are not counted.
big=$(find "$SRC" -name '*.c' ! -name 'rift_emoji_img.c' ! -name 'rift_emoji_seq.c' -exec wc -l {} + |
      awk '$1 > 900 && $2 != "total" {print $2}')
check "no source file has become a monolith${big:+ ($big)}" "$([ -z "$big" ] && echo 1 || echo 0)"
for part in rift_model.c rift_messages.c rift_arrivals.c rift_channels.c rift_actions.c \
            rift_order.c rift_format.c rift_format_msg.c rift_ipc.c rift_notify.c rift_sound.c \
            rift_store.c rift_dm_sound.c rift_app.c rift_background.c rift_traffic.c \
            rift_strip.c rift_net.c \
            ui/rift_widgets.c ui/rift_fit.c ui/rift_graph.c ui/rift_activity.c ui/rift_nodes.c \
            ui/rift_node_row.c ui/rift_detail.c ui/rift_comms.c ui/rift_conv_list.c \
            ui/rift_thread.c ui/rift_find.c ui/rift_netview.c ui/rift_session.c \
            ui/rift_system.c rift_map.c ui/rift_mapview.c; do
    check "$part is its own file" "$([ -f "$SRC/$part" ] && echo 1 || echo 0)"
done
# The model's other translation units are held to the same rule as the first:
# no LVGL, and the screens do not reach into them.
for part in rift_messages.c rift_arrivals.c rift_channels.c rift_actions.c rift_order.c \
            rift_notify.c rift_sound.c rift_store.c rift_traffic.c rift_net.c rift_map.c \
            rift_emoji_pick.c; do
    check "$part knows nothing about LVGL" \
        "$(grep -q 'lvgl' "$SRC/$part" && echo 0 || echo 1)"
done

# The clock. RIFT reads the system clock in one file, rift_clock.c, so that
# rift_app_test can link a virtual clock in its place and draw the same ages
# in every run. That substitution is a test's only: the shell links the real
# file, and the virtual one appears in the rift_app_test target and nowhere
# else.
clocks=$(grep -rlE 'clock_gettime|gettimeofday|time\(NULL\)' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "the system clock is read in rift_clock.c and nowhere else in RIFT (${clocks% })" \
    "$([ "$clocks" = "$SRC/rift_clock.c " ] && echo 1 || echo 0)"
CMAKE=ui/shell/CMakeLists.txt
check "the shell links the real clock" \
    "$(awk '/^add_executable\(pocketos-shell/,/\)$/' "$CMAKE" | grep -q 'apps/rift/rift_clock.c' &&
       awk '/^add_executable\(pocketos-shell/,/\)$/' "$CMAKE" | grep -q 'rift_test_clock' && echo 0 || echo 1)"
check "and the virtual clock is linked by rift_app_test alone, in place of it" \
    "$([ "$(grep -cE '^[[:space:]]*\$\{REPO_DIR\}/tests/rift_test_clock\.c$' "$CMAKE")" = 1 ] &&
       awk '/add_executable\(rift_app_test/,/\)$/' "$CMAKE" | grep -q 'tests/rift_test_clock.c' &&
       ! awk '/add_executable\(rift_app_test/,/\)$/' "$CMAKE" | grep -q 'apps/rift/rift_clock.c' && echo 1 || echo 0)"

# ---- the DM sound ----------------------------------------------------------------
# A sound for a new direct message, and for nothing else. Which messages are
# new is decided once, in the model, on the live event path; the sound goes
# through one seam, called from one place, and RIFT opens no sound device of
# its own - apps never touch hardware (ADR-002), and ADR-004's exception is
# Wave's.
check "a DM arrival is counted in one place" \
    "$([ "$(grep -rl 'dm_arrivals++' "$SRC" --include='*.c' | tr '\n' ' ')" = "$SRC/rift_arrivals.c " ] &&
       echo 1 || echo 0)"
livecallers=$(grep -rln 'rift_model_apply_live_message(' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "reached only from a live event, never a snapshot (${livecallers:-nowhere})" \
    "$([ "$livecallers" = "$SRC/rift_arrivals.c $SRC/rift_model.c " ] &&
       ! grep -n 'rift_model_apply_live_message' "$SRC/rift_messages.c" >/dev/null && echo 1 || echo 0)"
playcallers=$(grep -rln 'rift_sound_play(' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "the sound is asked for in one place (${playcallers:-nowhere})" \
    "$([ "$playcallers" = "$SRC/rift_dm_sound.c $SRC/rift_sound.c " ] && echo 1 || echo 0)"
check "and only when the policy says so" \
    "$(grep -B8 'rift_sound_play(' "$SRC/rift_dm_sound.c" | grep -q 'rift_notify_poll(' &&
       echo 1 || echo 0)"
check "RIFT opens no sound device of its own" \
    "$(grep -rnE '#include[[:space:]]*[<\"](alsa/|pocketaudio|sound/)|snd_pcm_|pocketaudio_' \
        --include='*.c' --include='*.h' "$SRC" >/dev/null 2>&1 && echo 0 || echo 1)"
# The sound is played by Doors's existing audio helper, pos-record (ADR-010
# Amendment 1), started from one file and for nothing else: no shell, no
# other program, and only its play and recover commands.
spawners=$(grep -rlE '\b(fork|execv[pe]?|execl[pe]?|posix_spawn[p]?|popen|system)\(' \
    --include='*.c' "$SRC" | sort | tr '\n' ' ')
check "and starts a helper only from the sound backend (${spawners:-nowhere})" \
    "$([ "$spawners" = "$SRC/rift_sound_helper.c " ] && echo 1 || echo 0)"
check "that helper is pos-record, by execv, never a shell" \
    "$(grep -q '#define HELPER_DEFAULT "/usr/bin/pos-record"' "$SRC/rift_sound_helper.c" &&
       ! grep -nE '\b(popen|system|execl[pe]?|execvp)\(|"/bin/sh"' "$SRC/rift_sound_helper.c" \
           >/dev/null && echo 1 || echo 0)"
check "asked only to play or to recover" \
    "$([ "$(grep -oE '= "(play|recover|record|info)"|"(play|recover|record|info)", NULL' \
            "$SRC/rift_sound_helper.c" | grep -oE 'play|recover|record|info' | sort -u |
            tr '\n' ' ')" = "play recover " ] && echo 1 || echo 0)"
check "and stops its sound when it goes" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'rift_sound_stop' && echo 1 || echo 0)"

# ---- the lifecycle -------------------------------------------------------------
# The session outlives the screen (DS §51): destroy lets the screen go and
# keeps the timer and the connection, which end with the session. A timer
# that outlives the session reaches a freed block on its next pass, a timer
# that touches a screen that is gone does the same, and a subscription that is
# merely dropped leaves the service writing to a socket nobody is reading.
check "the session's end deletes its timer" \
    "$(sed -n '/^void rift_bg_end/,/^}/p' "$SRC/rift_background.c" |
       grep -q 'lv_timer_delete' && echo 1 || echo 0)"
check "and closes its connection" \
    "$(sed -n '/^void rift_bg_end/,/^}/p' "$SRC/rift_background.c" |
       grep -q 'rift_ipc_close' && echo 1 || echo 0)"
check "destroy ends the session when CLOSE RIFT asked, and the shell's shutdown always" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" | grep -q 'rift_bg_end' &&
       sed -n '/^static void rift_shutdown/,/^}/p' "$SRC/rift_app.c" | grep -q 'rift_bg_end' &&
       grep -q '\.shutdown = rift_shutdown' "$SRC/rift_app.c" && echo 1 || echo 0)"
check "destroy clears the screen's half of the block" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'rift_bg_forget_screen' && echo 1 || echo 0)"
check "with no screen the timer reads the socket and touches nothing of a screen" \
    "$(sed -n '/^static void pump/,/^}/p' "$SRC/rift_app.c" | grep -A12 'if (!a->frame)' |
       grep -q 'return;' && echo 1 || echo 0)"
check "the status cluster's mark is set and cleared by RIFT alone" \
    "$([ "$(grep -rl 'pocketos_shell_set_background' "$SRC" --include='*.c' | sort | tr '\n' ' ')" = \
         "$SRC/rift_app.c $SRC/rift_background.c " ] && echo 1 || echo 0)"
check "and drops the layout callback before the objects go" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'lv_obj_remove_event_cb_with_user_data' && echo 1 || echo 0)"
check "the connection gives the subscription back rather than just closing" \
    "$(grep -q 'RIFT_REQ_UNSUBSCRIBE' "$SRC/rift_ipc.c" && echo 1 || echo 0)"

# ---- nothing on the LVGL thread waits ------------------------------------------
# pocketipc_call and pocketipc_call_timeout both wait for a reply. This
# client matches replies by id instead, so the only bounded wait in it is
# the connect (docs/api/pocketipc.md, request deadlines).
check "the client never calls pocketipc_call" \
    "$(grep -q 'pocketipc_call' "$SRC/rift_ipc.c" && echo 0 || echo 1)"
check "and its one bounded wait is the connect" \
    "$(grep -q 'pocketipc_connect_timeout' "$SRC/rift_ipc.c" && echo 1 || echo 0)"

# ---- what this phase does not have ---------------------------------------------
# Recorded here so the gaps are checked rather than remembered.
check "all four sections keep their place in the navigation" \
    "$(grep -q 'RIFT_SEC_COMMS' "$SRC/rift_app.h" && grep -q 'RIFT_SEC_NET' "$SRC/rift_app.h" &&
       echo 1 || echo 0)"
# NET is drawn now: the hop rings of handoff §7, placed from what the node
# cache holds (rift_net.c) and drawn by its own screen. What must stay true is
# that it draws only what was observed - a hop count the service reported, a
# route it learned - and that looking at it asks the service for nothing.
check "NET is drawn, from the node cache" \
    "$(grep -q 'rift_net_view_refresh' "$SRC/rift_app.c" && grep -q 'rift_net_build' \
        "$SRC/ui/rift_netview.c" && ! grep -q 'not in this build' "$SRC/rift_app.c" &&
       echo 1 || echo 0)"
check "a ring is placed by a learned route or an advert's hop count, and nothing else" \
    "$(grep -q 'RIFT_NET_SOURCE_ROUTE' "$SRC/rift_net.c" && grep -q 'RIFT_NET_SOURCE_ADVERT' \
        "$SRC/rift_net.c" && ! grep -qE 'rssi|snr' "$SRC/rift_net.c" && echo 1 || echo 0)"
check "and NET asks the service for nothing" \
    "$(grep -q 'rift_ipc_' "$SRC/ui/rift_netview.c" && echo 0 || echo 1)"
# The find bar narrows the node list and changes nothing. The one request it
# makes is a fresh node list when the zero-hop view is turned on - a question,
# not a packet: discovering repeaters by transmitting is SCAN 0-HOP on ACTIVITY
# (ui/rift_scan.c), never the find bar.
findipc=$(grep -o 'rift_ipc_[a-z_]*' "$SRC/ui/rift_find.c" | sort -u | tr '\n' ' ')
check "the find bar only ever asks for the node list (${findipc:-nothing})" \
    "$([ "$findipc" = "rift_ipc_request_nodes " ] && echo 1 || echo 0)"
check "and searching writes no node" \
    "$(grep -qE 'rift_model_(apply|drop)' "$SRC/ui/rift_find.c" "$SRC/rift_order.c" &&
       echo 0 || echo 1)"
check "there is no command parser in this phase" \
    "$(grep -rqE 'strcmp\(.*"/msg"|"/nodes"|"/advert"' "$SRC" && echo 0 || echo 1)"

# ---- channels are the service's; this app joins and leaves them on request --
# The approved design merges channels into the COMMS list with a "#" glyph.
# Since feat/rift-management the owner asked for channels to be managed here
# too, and the rules are the ones every other change to the service follows:
# named once, reached only from the panel a reader pressed, leaving only from
# its confirmation. And a key is never kept: it is made or checked in
# rift_keys.c, typed into the CHANNELS form, written into one request and
# wiped - the service's channels.v1 is its only home. No key in the model, the
# store, the client's own state or any other screen.
check "channels are compiled into the protocol core" \
    "$(grep -q 'MAX_GROUP_CHANNELS' protocols/meshcore/compat/mc_channels.h && echo 1 || echo 0)"
keyfiles=$(grep -rliE 'psk|pre_shared|secret\[|channel_key\[|base64' "$SRC" --include='*.c' \
            --include='*.h' | sort | tr '\n' ' ')
check "key material is handled in the key module and the CHANNELS form only (${keyfiles:-nowhere})" \
    "$([ "$keyfiles" = "$SRC/rift_keys.c $SRC/rift_keys.h $SRC/ui/rift_manage.c " ] && echo 1 || echo 0)"
check "and the model holds no key" \
    "$(grep -qiE '\bkey_b64|shared_key|psk' "$SRC/rift_model.h" "$SRC/rift_store.h" &&
       echo 0 || echo 1)"
chanhits=$(grep -rlnE '"mesh\.channel_(add|remove)"' "$SRC" | sort | tr '\n' ' ')
check "joining and leaving are named only in the meshcored client (${chanhits:-nowhere})" \
    "$([ "$chanhits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
chancallers=$(grep -rlnE 'rift_ipc_channel_(add|remove)\(' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "and are called only from the CHANNELS panel (${chancallers:-nowhere})" \
    "$([ "$chancallers" = "$SRC/rift_ipc_manage.c $SRC/ui/rift_manage.c " ] && echo 1 || echo 0)"
check "a channel is left only from the confirmation, never from the first press" \
    "$([ "$(grep -c 'rift_ipc_channel_remove(' "$SRC/ui/rift_manage.c")" = "1" ] &&
       grep -B 14 'rift_ipc_channel_remove(' "$SRC/ui/rift_manage.c" |
       grep -q 'static void on_leave_confirm(lv_event_t' && echo 1 || echo 0)"
devcallers=$(grep -rlnE 'rift_ipc_set_(name|path_hash)\(' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "a rename and the path hash size are asked only from THIS DEVICE (${devcallers:-nowhere})" \
    "$([ "$devcallers" = "$SRC/rift_ipc_manage.c $SRC/ui/rift_device.c " ] && echo 1 || echo 0)"
check "and a size other than 1 only from its confirmation" \
    "$(grep -B 12 'rift_ipc_set_path_hash(&a->ipc, v->bytes_pending)' "$SRC/ui/rift_device.c" |
       grep -q 'static void on_bytes_confirm(lv_event_t' &&
       [ "$(grep -c 'rift_ipc_set_path_hash(' "$SRC/ui/rift_device.c")" = "2" ] &&
       grep -q 'rift_ipc_set_path_hash(&a->ipc, 1)' "$SRC/ui/rift_device.c" && echo 1 || echo 0)"
check "a random key comes from the kernel's source, never a weaker one" \
    "$(grep -q 'getrandom(' "$SRC/rift_keys.c" && ! grep -rqE '\brand\(|srand\(|random\(\)' "$SRC" &&
       echo 1 || echo 0)"
check "the channel list comes from the service" \
    "$(grep -q 'mesh.channels' "$SRC/rift_ipc.c" && echo 1 || echo 0)"
check "and a channel row is drawn only from it" \
    "$(grep -q 'm->channel_count' "$SRC/ui/rift_comms.c" && echo 1 || echo 0)"
# The one thing a channel is not: acknowledged. A group frame is flooded and
# unacknowledged, so nothing in this app may draw a delivery for one.
check "a channel message is never shown as delivered" \
    "$(grep -q 'NO ACK ON CHANNELS' "$SRC/rift_format_msg.c" && echo 1 || echo 0)"
# An advert the service answered was accepted - queued for its dispatcher -
# and nothing in this app may call it sent: the transmit's outcome is the
# service's to report, in the activity feed.
check "an answered advert is called accepted, never sent" \
    "$(grep -q 'ACCEPTED %s AGO' "$SRC/rift_format_msg.c" &&
       ! grep -qE 'ADVERT[^"]*SENT' "$SRC/rift_format_msg.c" && echo 1 || echo 0)"
check "and the delivery tally counts channel sends apart" \
    "$(grep -q 'unacknowledgeable' "$SRC/rift_messages.c" && echo 1 || echo 0)"
# A sender's name on a channel is a claim: nothing signs a group frame. It
# must not be drawn the way a peer's name is.
check "a claimed sender name is marked as a claim" \
    "$(grep -q 'rift_emoji_fold(msg->sender_name' "$SRC/rift_format_msg.c" &&
       grep -q '"%s?", shown' "$SRC/rift_format_msg.c" &&
       grep -q 'rift_fmt_msg_meta' "$SRC/ui/rift_thread.c" && echo 1 || echo 0)"
# And the thread prints the body without the "<sender>: " MeshCore writes
# into a channel payload - the caption names the sender, once, as a claim.
check "a channel body is printed without the sender prefix" \
    "$(grep -q 'rift_msg_body(msg)' "$SRC/ui/rift_thread.c" && echo 1 || echo 0)"

# ---- the composer ---------------------------------------------------------------
# A message is not shown as delivered before the service says it was, and the
# app does not invent a local copy to reconcile later: mesh.message carries
# the message and the id is what keeps one message one row.
# A message's state is only ever copied from the service's own word. The one
# place a state is assigned is where the API's word is mapped, so a screen
# cannot decide that something arrived.
assigns=$(grep -rn -- 'state = RIFT_MSG_' "$SRC" --include='*.c' |
          sed 's/:.*//' | sort -u | tr '\n' ' ')
check "no screen decides a message's state${assigns:+ ($assigns)}" \
    "$([ -z "$assigns" ] && echo 1 || echo 0)"
check "and the one that sets it reads the service's word" \
    "$(grep -q 'msg->state = msg_state_from_word' "$SRC/rift_messages.c" && echo 1 || echo 0)"
check "the outbox never turns into a sent message" \
    "$(grep -q 'Nothing here marks it sent' "$SRC/rift_messages.c" && echo 1 || echo 0)"
check "and no message is created without an id from the service" \
    "$(grep -q 'there is no way to tell an update from a second copy' \
        "$SRC/rift_messages.c" && echo 1 || echo 0)"
check "the touch keyboard is given back when the app goes" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'pocketos_shell_keyboard_hide' && echo 1 || echo 0)"
# DS §20 wants an icon mask on every launcher tile. RIFT has no png-32 tint
# artwork: the approved package carries the mark as a design sheet, not as
# the artwork tools/design/gen_app_icons.py generates from, and drawing one
# here would be inventing branding. The launcher falls back to the text icon
# until the artwork exists, and this says so rather than leaving it to be
# discovered.
check "RIFT has no launcher icon mask yet, and says so where the app is declared" \
    "$(grep -q '.icon_mask = NULL' "$SRC/rift_app.c" && echo 1 || echo 0)"
check "and no artwork has appeared without the generator being told about it" \
    "$(ls docs/design/brand/*/png-32/rift.png >/dev/null 2>&1 &&
       ! grep -q 'rift' tools/design/gen_app_icons.py && echo 0 || echo 1)"

echo "rift_lint: $failed failure(s)"
exit $((failed > 0))
