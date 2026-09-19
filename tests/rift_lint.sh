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

# ---- nothing here transmits ----------------------------------------------------
# The API has exactly two methods that put a packet on the air (docs/api/
# mesh.md). Opening a screen must not reach either. tests/rift_ipc_test.c
# proves the same thing from the other end, by recording what the service was
# asked for.
for method in mesh.send mesh.advert; do
    check "no source under apps/rift names $method" \
        "$(grep -rn "\"$method\"" "$SRC" >/dev/null 2>&1 && echo 0 || echo 1)"
done

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
    "$(grep -q 'lvgl' "$SRC/rift_format.c" "$SRC/rift_format.h" && echo 0 || echo 1)"
check "nor the meshcored client" \
    "$(grep -q 'lvgl' "$SRC/rift_ipc.c" "$SRC/rift_ipc.h" && echo 0 || echo 1)"
check "the formatting does no I/O" \
    "$(grep -qE 'fopen|socket|read\(|write\(' "$SRC/rift_format.c" && echo 0 || echo 1)"
check "and the screens parse no JSON of their own" \
    "$(grep -rq 'cJSON_Parse' "$SRC/ui" && echo 0 || echo 1)"

# One screen to a file, and no file that is all of them. 900 lines is well
# above anything here and well below a monolith.
big=$(find "$SRC" -name '*.c' -exec wc -l {} + | awk '$1 > 900 && $2 != "total" {print $2}')
check "no source file has become a monolith${big:+ ($big)}" "$([ -z "$big" ] && echo 1 || echo 0)"
for part in rift_model.c rift_format.c rift_ipc.c rift_app.c ui/rift_widgets.c \
            ui/rift_activity.c ui/rift_nodes.c ui/rift_detail.c; do
    check "$part is its own file" "$([ -f "$SRC/$part" ] && echo 1 || echo 0)"
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
check "COMMS and NET keep their place in the navigation" \
    "$(grep -q 'RIFT_SEC_COMMS' "$SRC/rift_app.h" && grep -q 'RIFT_SEC_NET' "$SRC/rift_app.h" &&
       echo 1 || echo 0)"
check "and say they are not in this build rather than showing an empty list" \
    "$(grep -q 'not in this build' "$SRC/rift_app.c" && echo 1 || echo 0)"
check "there is no command parser in this phase" \
    "$(grep -rqE 'strcmp\(.*"/msg"|"/nodes"|"/advert"' "$SRC" && echo 0 || echo 1)"
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
