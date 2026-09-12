/*
 * The keyboard suppression policy. See shell_kb_state.h.
 *
 * No LVGL, no objects, no lifecycle: one boolean and the rule that reads it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_kb_state.h"

static int suppressed;

void pocketos_shell_keyboard_set_suppressed(int on)
{
    suppressed = on ? 1 : 0;
}

int pocketos_shell_keyboard_suppressed(void)
{
    return suppressed;
}

int pocketos_shell_keyboard_may_show(void)
{
    return !suppressed;
}
