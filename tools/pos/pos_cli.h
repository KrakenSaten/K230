/*
 * The name the command-line tool answers to (ADR-005 Phase 2).
 *
 * One binary, two entry names: the image installs it as /usr/bin/doors and
 * makes /usr/bin/pos a symlink to it. Invoked as `doors`, it names itself
 * doors in every usage line and message, and `doors version` says Doors.
 * Invoked by any other name - `pos`, or a path such as tools/pos/pos - it
 * prints exactly what pos printed through v0.0.9, so an operator script that
 * reads pos output keeps working. pos stays a permanent alias (ADR-005,
 * decision 5).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POS_CLI_H
#define POS_CLI_H

/* "doors" or "pos". Set once by main() from argv[0], before any command runs. */
extern const char *pos_cli_name;

/* Nonzero when the tool was invoked as doors. */
int pos_cli_is_doors(void);

#endif
