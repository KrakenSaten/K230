#!/bin/bash
# PocketFleet structural rules (see apps/fleet/engine/fleet_ai.h):
#   1. the game engine stays free of LVGL, so it is unit-tested natively;
#   2. the AI cannot see the hidden state: fleet_ai.[ch] must not include
#      fleet_rules.h nor name struct fleet_board or struct fleet_game;
#   3. fleet_store.c is the only part of the game that touches the filesystem;
#   4. the layout is chosen from the body, never from an orientation, and the
#      corner clearance is taken from the one platform rule rather than worked
#      out here (DS 21.3, 22.2, 28.1);
#   5. the boards' geometry has one source, so what is drawn and what is hit
#      can never disagree;
#   6. a normal turn of Battle is on the display, whole, and nothing it needs
#      is reached by scrolling (DS 28.6);
#   7. and no square on the board has to be hit exactly to be aimed at.
# Fails the build when any of them is broken.
set -u
cd "$(dirname "$0")/.." || exit 1
ENGINE=apps/fleet/engine
failed=0
check() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "FAIL $1:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}
check "engine is free of LVGL" "$ENGINE/*.c $ENGINE/*.h" 'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "the view model is free of LVGL" "apps/fleet/ui/fleet_view.c apps/fleet/ui/fleet_view.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "AI does not include the rules" "$ENGINE/fleet_ai.c $ENGINE/fleet_ai.h" 'include[[:space:]]*"fleet_rules\.h"'
check "AI does not name the hidden state" "$ENGINE/fleet_ai.c" 'fleet_board|fleet_game'
check "only fleet_store.c touches the filesystem" \
      "$(ls $ENGINE/*.c | grep -v fleet_store.c)" \
      '\b(fopen|open|creat|unlink|rename|mkdir|remove)[[:space:]]*\('

# 8. The multiplayer protocol (docs/apps/FLEET_MULTIPLAYER.md) is pure: it
#    draws nothing, talks to no service, opens no file and reads no clock or
#    random source of its own, so a match replays exactly and the simulator
#    can run thousands of them.
NET=apps/fleet/net
check "the multiplayer protocol is free of LVGL" "$NET/*.c $NET/*.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "nor talks to a service itself" "$NET/*.c $NET/*.h" \
      'pocketipc|socket[[:space:]]*\(|connect[[:space:]]*\(|mesh\.|radio\.'
check "nor touches the filesystem" "$NET/*.c" \
      '\b(fopen|open|creat|unlink|rename|mkdir|remove|fsync)[[:space:]]*\('
check "nor reads a clock or a random source of its own" "$NET/*.c" \
      '\b(time|clock_gettime|gettimeofday|rand|random|getrandom|arc4random)[[:space:]]*\('

want() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 2>/dev/null)
    if [ -z "$hits" ]; then
        echo "FAIL $1: not found"; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

# 9. Multiplayer's joints (ADR-008, ADR-017). The view model and the link
#    layer draw nothing; exactly two files talk to a service, the mesh link
#    (to meshcored) and the LAN link (to peerd), each to its own and no other;
#    nothing in Fleet names a radio method or radiod's socket; the session
#    pumps nothing until the player engages; and the virtual opponent exists
#    only when POCKETFLEET_MP_FAKE asks for it.
LINK=apps/fleet/link
check "the multiplayer view model is free of LVGL" \
      "apps/fleet/ui/fleet_view_mp.c apps/fleet/ui/fleet_view_mp.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "the link layer is free of LVGL" "$LINK/*.c $LINK/*.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "only the mesh link and the LAN link talk to a service" \
      "$(ls apps/fleet/*.c apps/fleet/*/*.c | grep -v "$LINK/fleet_link_mesh.c" | grep -v "$LINK/fleet_link_peer.c")" \
      'pocketipc_|include[[:space:]]*"pocketipc'
check "the mesh link asks peerd nothing" "$LINK/fleet_link_mesh.c" '"peer\.[a-z_]+"|"peerd"'
check "the LAN link asks meshcored nothing" "$LINK/fleet_link_peer.c $LINK/fleet_link_peer.h" \
      '"mesh\.[a-z_]+"|"meshcored"'
# The LAN link opens no socket to the network: peerd holds the UDP socket,
# the identity and the cryptography (ADR-017). It never renames the device,
# and it announces no table (the lobby's, M3).
check "the LAN link opens no network socket and does no cryptography" \
      "$LINK/fleet_link_peer.c" \
      '\b(socket|sendto|recvfrom|bind)[[:space:]]*\(|sodium|crypto_|AF_INET'
check "nor renames the device or announces a table" "$LINK/fleet_link_peer.c" \
      '"peer\.(set_name|announce|withdraw)"'
want "it is on the LAN profile" "$LINK/fleet_link_peer.c" 'return &fleet_profile_lan;'
want "it ends the visibility lease when closed" "$LINK/fleet_link_peer.c" \
     'cJSON_AddBoolToObject\(p, "visible", 0\)'
want "it keeps or releases nobody before the session has said who" "$LINK/fleet_link_peer.c" \
     'if \(!cJSON_IsArray\(arr\) \|\| !m->match_known\) \{'
want "the session says who only once the saved match has been read" "$LINK/fleet_session.c" \
     'if \(s->link->ops->match_peer\) \{'
want "and never says none of a save it could not read" "$LINK/fleet_session.c" \
     'if \(in_hand \|\| !s->store_fault \|\| s->peer_said\) \{'
# The app opens the LAN link in one place, beside the mesh link, and asks it
# for its own detail only where that is shown: the lobby and the session's
# owner. Whether peerd exists is a look at the filesystem, never a connection.
check "only the session's owner opens a link" \
      "$(ls apps/fleet/*.c apps/fleet/*/*.c | grep -v 'apps/fleet/fleet_mp.c' | grep -v "$LINK/")" \
      'fleet_link_(peer|mesh)_open'
check "only the lobby and the session's owner ask the LAN link for its detail" \
      "$(ls apps/fleet/*.c apps/fleet/*/*.c | grep -v 'apps/fleet/fleet_mp.c' | grep -v 'apps/fleet/ui/fleet_screen_lobby.c' | grep -v "$LINK/")" \
      'fleet_link_peer_(info|probe|probe_state|probe_key|proved|present|set_visible)[[:space:]]*\('
want "whether the network is offered is a look at the socket, not a connection" \
     "$LINK/fleet_link_peer.c" 'stat\(path, &sb\) == 0 && S_ISSOCK'
want "an unproved player is said to be one" "apps/fleet/ui/fleet_screen_lobby.c" \
     'fleet_link_peer_proved\(link, ui->peers\[i\]\.key\)'
check "Fleet never names a radio method or radiod" "apps/fleet/*.c apps/fleet/*/*.c" \
      '"radio\.[a-z_]+"|radiod\.sock|"radiod"'
want "the session pumps nothing before the player engages" "$LINK/fleet_session.c" \
     'if \(!s->engaged\) \{'
want "the virtual opponent only when asked for" "apps/fleet/fleet_mp.c" \
     'getenv\("POCKETFLEET_MP_FAKE"\)'
check "and nowhere else" "$(ls apps/fleet/*.c apps/fleet/*/*.c | grep -v fleet_mp.c | grep -v fleet_link_loop.c)" \
      'fleet_link_loop_open'
# What the mesh link may ask meshcored to transmit: app datagrams, and a
# zero-hop advert from a button. Never a chat message, and nothing that
# changes what the service holds.
check "the mesh link sends no chat message and changes nothing the service holds" \
      "$LINK/fleet_link_mesh.c" '"mesh\.(send|node_remove|node_reset_path|channel_add|channel_remove)"'
want "its only advert is zero-hop" "$LINK/fleet_link_mesh.c" 'cJSON_AddBoolToObject\(p, "zero_hop", 1\)'


UI="apps/fleet/fleet_app.c apps/fleet/ui/fleet_screen_battle.c     apps/fleet/ui/fleet_screen_command.c apps/fleet/ui/fleet_screen_deploy.c     apps/fleet/ui/fleet_screen_result.c"

# 4. The shape comes from the body's size. Nothing in the app may ask which way
#    up the display is, or reach for the rotation the shell owns.
check "the app never asks for the orientation" "$UI apps/fleet/ui/fleet_grid.c"       'pocketos_shell_orientation|pocketos_rotation_mode|POCKETOS_ROTATION|lv_display_get_rotation'
check "nor works the corner clearance out for itself" "$UI"       'corners\.|top_left|bottom_left|bottom_right|top_right|pos_display_rect_insets'
check "nor keeps a layout-guard comparison of its own" "apps/fleet/fleet_app.c"       'memcmp\(&(box|area|in|insets)'
want "the corner clearance comes from the platform rule, through PocketUI" "apps/fleet/fleet_app.c"      'pocketui_layout_begin\(&app->layout_guard, app->frame'
want "and the shape from the body's own box" "apps/fleet/fleet_app.c"      'LV_EVENT_SIZE_CHANGED'
want "a layout pass that found nothing changed is not counted" "apps/fleet/fleet_app.c"      'app->layouts\+\+;'

# 5. One stored cell size per board. A screen may ask the grid to change size;
#    it may not draw or hit a cell itself.
check "only the grid works in cells" "$UI"       'FLEET_GRID_GAP|FLEET_GRID_GUTTER'
want "the span has one definition" "apps/fleet/ui/fleet_grid.c"      'int fleet_grid_span_for'
want "and the hit test reads the width the drawing uses" "apps/fleet/ui/fleet_grid.c"      'g->cell_w \+ FLEET_GRID_GAP'
want "and the height too" "apps/fleet/ui/fleet_grid.c"      'g->cell_h \+ FLEET_GRID_GAP'

# 6. A normal turn of Battle is on the display, whole, and no part of playing
#    it is reached by scrolling (DS 28.6). The screen may not make itself a
#    scroller and may not scroll anything into view: something that does not
#    fit has to be laid out, not scrolled. The test that holds the screen to
#    this has to still be there, and still cover both type sizes and both
#    corner shapes.
BATTLE=apps/fleet/ui/fleet_screen_battle.c
check "Battle never turns scrolling on" "$BATTLE" 'lv_obj_add_flag\([^;]*LV_OBJ_FLAG_SCROLLABLE'
check "nor scrolls anything into view" "$BATTLE" 'lv_obj_scroll_to|lv_obj_set_scroll_dir|lv_obj_scroll_by'
want "the frame scrolls only in the tall shape" "apps/fleet/fleet_app.c" \
     'lv_obj_remove_flag\(app->frame, LV_OBJ_FLAG_SCROLLABLE\)'
want "a turn is held to fitting on the display" tests/fleet_app_test.c \
     'static void check_battle_never_scrolls'
want "through a whole match played across the page" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("normal", PANEL_CORNER\)'
want "in Outdoor type as well" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("outdoor", PANEL_CORNER\)'
want "and on a panel with square corners" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("(normal|outdoor)", 0\)'

# 7. A row across the page is 34 px and no layout can make it more, so the
#    player must never have to hit one exactly. The two ways of aiming that
#    ask no precision of them have to stay: the square under the finger is
#    reported throughout a drag, and the four one-square nudges are each a
#    finger's size.
want "aiming follows the finger, not just the tap" "apps/fleet/ui/fleet_grid.c"      'LV_EVENT_PRESSING'
want "and a square can be reached one step at a time" "$BATTLE"      'void fleet_screen_battle_nudge'
want "by buttons that are a finger's size" "$BATTLE"      'POCKETUI_TOUCH_MIN'
want "the nudges are held to that size" tests/fleet_app_test.c      'every one of them is a finger.s size'
want "and to reaching every square" tests/fleet_app_test.c      'static void test_aim_without_precision'

# 9. One save per transport (docs/decisions/ADR-018, point 3). match.v1 is
#    LoRa's, in the format it always had, and nothing that is not the mesh's
#    can name it: the store turns a transport into a file in exactly one
#    place, a link never touches a save, and only the app's mesh session
#    (fleet_mp.c) and the store itself speak of the mesh's file.
STORE=apps/fleet/engine/fleet_store
OTHERS=$(git ls-files 'apps/fleet/*.c' 'apps/fleet/*.h' 2>/dev/null | grep -v "^$STORE\.[ch]$")
[ -n "$OTHERS" ] || OTHERS=$(find apps/fleet -name '*.[ch]' | grep -v "^$STORE\.[ch]$")
check "match.v1 is named by the store alone" "$OTHERS" 'FLEET_STORE_MATCH_FILE|"match\.v1"|match-lan\.v1'
check "a link never touches a save" "apps/fleet/link/fleet_link*.c apps/fleet/link/fleet_link*.h" 'fleet_store_'
check "nothing but the session's owner asks for a transport's save" \
      "$(echo "$OTHERS" | grep -v '^apps/fleet/fleet_mp\.c$')" \
      'FLEET_STORE_MESH|fleet_store_match_(path|save|load)[[:space:]]*\('
# fleet_mp.c holds the app's one session, on one transport at a time. A
# transport becomes a save file in exactly one function there, every read and
# write goes through it, and the development opponent's profile follows the
# transport and never its own spec - so a match is only ever saved and read
# by the transport it is played on.
n=$(grep -cE 'FLEET_STORE_(LAN|MESH)' apps/fleet/fleet_mp.c)
if [ "$n" = 1 ]; then echo "ok   the app maps a transport to a save in one line"; else
    echo "FAIL the stores are named $n times in apps/fleet/fleet_mp.c"; failed=$((failed + 1)); fi
want "in one function" "apps/fleet/fleet_mp.c" \
     'return t == FLEET_MP_LAN \? FLEET_STORE_LAN : FLEET_STORE_MESH;'
check "and never reads or writes a match save but through it" "apps/fleet/fleet_mp.c" \
      'fleet_store_match_(path|save|load)[[:space:]]*\('
want "the development opponent's profile follows the transport" "apps/fleet/fleet_mp.c" \
     'cfg\.lan = t == FLEET_MP_LAN;'
want "the session is told of a match in hand on the other transport" "apps/fleet/fleet_mp.c" \
     'fleet_session_set_other_in_hand\(app->mp, app->mp_other_in_hand\);'
want "the store maps a transport to a file in one place" "$STORE.c" 'static const char \*match_file\('
n=$(grep -c 'FLEET_STORE_MATCH_FILE' "$STORE.c")
if [ "$n" = 1 ]; then echo "ok   and match.v1 is spelled there once"; else
    echo "FAIL match.v1 is spelled $n times in $STORE.c"; failed=$((failed + 1)); fi
want "match.v1 keeps its name" "$STORE.h" '#define FLEET_STORE_MATCH_FILE "match\.v1"'
want "a build before this one wrote the fixture the codec is held to" tests/fleet_match_test.c \
     'tests/fixtures/fleet/match\.v1\.golden'

echo "fleet_lint: $failed failure(s)"
exit $((failed > 0))
