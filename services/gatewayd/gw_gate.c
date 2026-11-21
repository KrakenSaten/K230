/*
 * gatewayd: the device-password gate. See gw_gate.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_gate.h"

#include <stdio.h>
#include <string.h>

/* A password field that can admit somebody: a crypt(3) hash. The modular
 * forms start with '$'; the traditional DES form is 13 characters of
 * [./0-9A-Za-z]. Anything else - empty, "!", "*", "x", a locked hash - is
 * not a password a person set. */
static bool field_is_hash(const char *f, size_t n)
{
    size_t i;

    if (n >= 4 && f[0] == '$') {
        return true;
    }
    if (n != 13) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = f[i];

        if (!(c == '.' || c == '/' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

enum root_field { ROOT_NONE, ROOT_HASH, ROOT_SHADOWED };

static enum root_field root_field(const char *path)
{
    char line[1024];
    enum root_field r = ROOT_NONE;
    FILE *f = fopen(path, "re");

    if (f == NULL) {
        return ROOT_NONE;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        const char *field, *end;

        if (strncmp(line, "root:", 5) != 0) {
            continue;
        }
        field = line + 5;
        end = field + strcspn(field, ":\n");
        if (field_is_hash(field, (size_t)(end - field))) {
            r = ROOT_HASH;
        } else if (end - field == 1 && field[0] == 'x') {
            r = ROOT_SHADOWED;
        }
        break; /* the first root entry is the one login uses */
    }
    /* The hash was in this buffer: it is not left on the stack. */
    memset(line, 0, sizeof(line));
    fclose(f);
    return r;
}

bool gw_gate_password_set(const char *passwd, const char *shadow)
{
    switch (root_field(passwd)) {
    case ROOT_HASH:
        return true;
    case ROOT_SHADOWED:
        return root_field(shadow) == ROOT_HASH;
    default:
        return false; /* empty, locked, absent or unreadable */
    }
}
