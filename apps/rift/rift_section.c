/*
 * RIFT's sections: which screen is showing, the tab it is reached from, and
 * what leaving one cancels. Split from rift_app.c, which builds the screens,
 * so neither is a monolith (tests/rift_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_app.h"

#include "app.h"
#include "rift_nodes.h"
#include "rift_repeater_view.h"
#include "rift_strip.h"
#include "rift_system.h"

/* ---- sections -------------------------------------------------------------- */

enum rift_section rift_tab_of(enum rift_section section)
{
    return section == RIFT_SEC_NET                                  ? RIFT_SEC_NODES
           : section == RIFT_SEC_REPEATER || section == RIFT_SEC_RXLOG ? RIFT_SEC_ACTIVITY
           : section == RIFT_SEC_CONTACTS                             ? RIFT_SEC_COMMS
                                                                     : section;
}

static void show_only(struct rift_app *a, lv_obj_t *keep)
{
    uint32_t n = lv_obj_get_child_count(a->content);
    uint32_t i;

    for (i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(a->content, (int32_t)i);

        if (child == keep) {
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void rift_app_show_section(struct rift_app *a, enum rift_section section)
{
    if (!a || section < 0 || section >= RIFT_SEC_COUNT) {
        return;
    }
    /* The touch keyboard was asked for by a field on the screen being left
     * (the composer, the find bar) and does not outlive it: the sheet is the
     * shell's and would stay up over the next screen, its Done going to a
     * field nobody can see. The same section shown again (a rebuild) keeps it. */
    if (section != a->section && pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    a->section = section;
    if (section != RIFT_SEC_COMMS) {
        /* The composer is COMMS': the keys go back to the list's sink, from
         * the timer (this may be running inside the field's own key event). */
        a->focus_composer_pending = 0;
        if (a->composer_focused) {
            a->focus_list_pending = 1;
        }
    } else if (a->have_conv) {
        a->focus_composer_pending = 1;
    }
    if (section != RIFT_SEC_NODES) {
        a->detail_open = 0;
        /* Leaving NODES is a Cancel for any confirmation left up there. */
        rift_nodes_cancel_confirm(a);
    }
    if (section != RIFT_SEC_SYSTEM) {
        /* And leaving SYSTEM for what is open there - a form, a LEAVE, path
         * hash or CLOSE RIFT confirmation, a key shown for sharing. */
        rift_system_cancel(a);
    }
    if (section != RIFT_SEC_REPEATER) {
        rift_repeater_view_cancel(a); /* a command confirmation, the fields */
    }
    rift_tabs_paint(a);
    switch (section) {
    case RIFT_SEC_ACTIVITY:
        show_only(a, a->activity_root);
        break;
    case RIFT_SEC_NODES:
        show_only(a, a->nodes_root);
        break;
    case RIFT_SEC_COMMS:
        show_only(a, a->comms_root);
        break;
    case RIFT_SEC_SYSTEM:
        show_only(a, a->system_root);
        break;
    case RIFT_SEC_MAP:
        show_only(a, a->map_root);
        break;
    case RIFT_SEC_REPEATER:
        show_only(a, a->repeater_root);
        break;
    case RIFT_SEC_RXLOG:
        show_only(a, a->rxlog_root);
        break;
    case RIFT_SEC_CONTACTS:
        show_only(a, a->contacts_root);
        break;
    default:
        show_only(a, a->net_root);
        break;
    }
    rift_app_refresh(a);
}
