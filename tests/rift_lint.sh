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
# the client; one function writes it; only ACTIVITY's two buttons call that
# function; and nothing on a timer, a poll, a snapshot or the app's creation
# reaches it.
advhits=$(grep -rln '"mesh.advert"' "$SRC" | sort | tr '\n' ' ')
check "mesh.advert is named only in the meshcored client (${advhits:-nowhere})" \
    "$([ "$advhits" = "$SRC/rift_ipc.c " ] && echo 1 || echo 0)"
check "and only one call writes it" \
    "$([ "$(grep -c 'RIFT_REQ_ADVERT, params' "$SRC/rift_ipc.c")" = "1" ] && echo 1 || echo 0)"
advcallers=$(grep -rln 'rift_ipc_send_advert' "$SRC" --include='*.c' | sort | tr '\n' ' ')
check "the advert is called only from ACTIVITY (${advcallers:-nowhere})" \
    "$([ "$advcallers" = "$SRC/rift_ipc.c $SRC/ui/rift_activity.c " ] && echo 1 || echo 0)"
check "and only from the handler of a button a reader pressed" \
    "$([ "$(grep -c 'rift_ipc_send_advert(' "$SRC/ui/rift_activity.c")" = "1" ] &&
       grep -B 12 'rift_ipc_send_advert(' "$SRC/ui/rift_activity.c" |
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

# ---- RIFT owns no colour, no font and no hardware ------------------------------
# tests/style_lint.sh covers ui/ and apps/ for colour literals; these are the
# rules that are RIFT's own.
check "no colour is named in the app" \
    "$(grep -rnE 'lv_color_hex|lv_palette_|0x[0-9a-fA-F]{6}\b' "$SRC" --include='*.c' \
        --include='*.h' >/dev/null 2>&1 && echo 0 || echo 1)"
check "no device path or GPIO is touched" \
    "$(grep -rnE '/dev/|gpiod_|spidev' "$SRC" >/dev/null 2>&1 && echo 0 || echo 1)"
# The prose here names MeshCore constantly - it is the protocol on the other
# end of the API - so what is checked is what is *included* and what is
# *called*, not what is written about.
check "no radio or protocol library is included" \
    "$(grep -rnE '#include[[:space:]]*[<\"].*(RadioLib|SX126|Mesh|Dispatcher|Packet)' "$SRC" \
        >/dev/null 2>&1 && echo 0 || echo 1)"
check "and no MeshCore symbol is called" \
    "$(grep -rnE 'mesh::|Mesh[A-Za-z]*\(' "$SRC" --include='*.c' --include='*.h' \
        >/dev/null 2>&1 && echo 0 || echo 1)"
check "the app stores nothing of its own" \
    "$(ls "$SRC"/*store* >/dev/null 2>&1 && echo 0 || echo 1)"
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
# above anything here and well below a monolith.
big=$(find "$SRC" -name '*.c' -exec wc -l {} + | awk '$1 > 900 && $2 != "total" {print $2}')
check "no source file has become a monolith${big:+ ($big)}" "$([ -z "$big" ] && echo 1 || echo 0)"
for part in rift_model.c rift_messages.c rift_channels.c rift_actions.c rift_order.c \
            rift_format.c rift_format_msg.c rift_ipc.c rift_app.c \
            ui/rift_widgets.c ui/rift_activity.c ui/rift_nodes.c ui/rift_detail.c \
            ui/rift_comms.c ui/rift_thread.c; do
    check "$part is its own file" "$([ -f "$SRC/$part" ] && echo 1 || echo 0)"
done
# The model's other translation units are held to the same rule as the first:
# no LVGL, and the screens do not reach into them.
for part in rift_messages.c rift_channels.c rift_actions.c rift_order.c; do
    check "$part knows nothing about LVGL" \
        "$(grep -q 'lvgl' "$SRC/$part" && echo 0 || echo 1)"
done

# ---- the lifecycle -------------------------------------------------------------
# A timer that outlives the app reaches a freed block on its next pass, and
# a subscription that is merely dropped leaves the service writing to a
# socket nobody is reading.
check "the app deletes its timer when it is destroyed" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'lv_timer_delete' && echo 1 || echo 0)"
check "and closes its connection" \
    "$(sed -n '/^static void rift_destroy/,/^}/p' "$SRC/rift_app.c" |
       grep -q 'rift_ipc_close' && echo 1 || echo 0)"
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
check "and NET says it is not in this build rather than showing an empty view" \
    "$(grep -q 'not in this build' "$SRC/rift_app.c" && echo 1 || echo 0)"
check "there is no command parser in this phase" \
    "$(grep -rqE 'strcmp\(.*"/msg"|"/nodes"|"/advert"' "$SRC" && echo 0 || echo 1)"

# ---- channels are the service's, and this app only shows them -----------------
# The approved design merges channels into the COMMS list with a "#" glyph,
# and now there is something to merge. What must stay true is that every
# channel on screen is one the service reported: this app holds no key,
# derives no channel from a name, and cannot join one. A channel row that did
# not come from mesh.channels would be this app inventing a place to write
# that nothing would carry.
check "channels are compiled into the protocol core" \
    "$(grep -q 'MAX_GROUP_CHANNELS' protocols/meshcore/compat/mc_channels.h && echo 1 || echo 0)"
check "the app holds no channel key" \
    "$(grep -rqiE 'psk|pre_shared|secret\[|channel_key\[|base64' "$SRC" && echo 0 || echo 1)"
# The quoted method string, which is what a call looks like - rift_ipc.h
# names both methods in prose to say why they are not used, and a check that
# could not tell the two apart would fail on the explanation.
check "and cannot join or leave one: that takes a key" \
    "$(grep -rq --include='*.c' '"mesh\.channel_add"\|"mesh\.channel_remove"' "$SRC" &&
       echo 0 || echo 1)"
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
    "$(grep -q '"%s?", msg->sender_name' "$SRC/rift_format_msg.c" &&
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
