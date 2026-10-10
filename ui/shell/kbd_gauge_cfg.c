/*
 * The owner-triggered BQ27220 capacity configuration. See kbd_gauge_cfg.h
 * for the sequence, its sources and what it never does.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_gauge_cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum step {
    S_PRE_OPSTATUS,
    S_PRE_DESIGN,
    S_PRE_FCC,
    S_KEY1,
    S_KEY2,
    S_ENTER,
    S_WAIT_ENTER,
    S_SELECT,
    S_READ_BACKUP,
    S_WAIT_BACKUP, /* no bus: the shell stores the backup */
    S_WRITE_DATA,
    S_WRITE_SUM,
    S_SELECT_VERIFY,
    S_READ_VERIFY,
    S_EXIT,
    S_WAIT_EXIT,
    S_POST_DESIGN,
    S_POST_FCC,
    S_DONE,
};

/* ---- pure helpers ------------------------------------------------------- */

uint16_t kbd_gauge_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint8_t kbd_gauge_block_sum(const uint8_t *addr2, const uint8_t *data32)
{
    unsigned sum = (unsigned)addr2[0] + addr2[1];
    unsigned i;

    for (i = 0; i < KBD_GAUGE_BLOCK_LEN; i++) {
        sum += data32[i];
    }
    return (uint8_t)(0xFFu - (sum & 0xFFu));
}

int kbd_gauge_block_check(const uint8_t *read36, uint16_t dm_addr)
{
    if (!read36 || read36[0] != (uint8_t)(dm_addr & 0xFFu) ||
        read36[1] != (uint8_t)(dm_addr >> 8) ||
        read36[2 + KBD_GAUGE_BLOCK_LEN + 1] != KBD_GAUGE_MAC_LEN) {
        return -1;
    }
    return kbd_gauge_block_sum(read36, read36 + 2) == read36[2 + KBD_GAUGE_BLOCK_LEN] ? 0 : -1;
}

const char *kbd_gauge_result_name(enum kbd_gauge_result r)
{
    switch (r) {
    case KBD_GAUGE_IDLE:
        return "idle";
    case KBD_GAUGE_RUNNING:
        return "running";
    case KBD_GAUGE_OK:
        return "ok";
    case KBD_GAUGE_REFUSED:
        return "refused";
    case KBD_GAUGE_FAILED_UNCHANGED:
        return "failed-unchanged";
    case KBD_GAUGE_FAILED_CHANGED:
        return "failed-after-commit";
    case KBD_GAUGE_FAILED_UNKNOWN:
        return "failed-unknown";
    }
    return "?";
}

bool kbd_gauge_cfg_running(const struct kbd_gauge_cfg *g)
{
    return g && g->result == KBD_GAUGE_RUNNING;
}

/* ---- transactions -------------------------------------------------------- */

static int xact_write(struct kbd_gauge_cfg *g, uint8_t reg, const uint8_t *buf, unsigned len)
{
    const struct kbd_bus *b = g->bus;
    int rc;

    g->transactions++;
    if (b->claim(b->ctx) != 0) {
        g->failures++;
        return -1;
    }
    rc = b->write_block_at(b->ctx, KBD_GAUGE_ADDR, reg, buf, len);
    b->release(b->ctx);
    if (rc != 0) {
        g->failures++;
    }
    return rc;
}

static int xact_read(struct kbd_gauge_cfg *g, uint8_t reg, uint8_t *buf, unsigned len)
{
    const struct kbd_bus *b = g->bus;
    int rc;

    g->transactions++;
    if (b->claim(b->ctx) != 0) {
        g->failures++;
        return -1;
    }
    rc = b->read_block_at(b->ctx, KBD_GAUGE_ADDR, reg, buf, len);
    b->release(b->ctx);
    if (rc != 0) {
        g->failures++;
    }
    return rc;
}

/* Standard commands are little-endian (SLUSCB7A §7.3.1.1). */
static int read16(struct kbd_gauge_cfg *g, uint8_t reg, uint16_t *out)
{
    uint8_t b[2];

    if (xact_read(g, reg, b, 2) != 0) {
        return -1;
    }
    *out = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return 0;
}

/* A Control() subcommand, low byte first (SLUUBD4A §6.1: wr 0x00 0x90 0x00). */
static int control(struct kbd_gauge_cfg *g, uint16_t sub)
{
    uint8_t b[2] = {(uint8_t)(sub & 0xFFu), (uint8_t)(sub >> 8)};

    return xact_write(g, KBD_GAUGE_REG_CONTROL, b, 2);
}

static int select_block(struct kbd_gauge_cfg *g)
{
    uint8_t b[2] = {(uint8_t)(KBD_GAUGE_DM_BLOCK & 0xFFu), (uint8_t)(KBD_GAUGE_DM_BLOCK >> 8)};

    return xact_write(g, KBD_GAUGE_REG_MAC, b, 2);
}

/* The block with the two new values in place, big-endian. */
static void new_block(const struct kbd_gauge_cfg *g, uint8_t *data32)
{
    memcpy(data32, g->backup + 2, KBD_GAUGE_BLOCK_LEN);
    data32[KBD_GAUGE_DM_FCC_OFF] = (uint8_t)(g->want_fcc >> 8);
    data32[KBD_GAUGE_DM_FCC_OFF + 1] = (uint8_t)(g->want_fcc & 0xFFu);
    data32[KBD_GAUGE_DM_DESIGN_OFF] = (uint8_t)(g->want_design >> 8);
    data32[KBD_GAUGE_DM_DESIGN_OFF + 1] = (uint8_t)(g->want_design & 0xFFu);
}

/* ---- the state machine --------------------------------------------------- */

static enum kbd_gauge_tick finish(struct kbd_gauge_cfg *g, enum kbd_gauge_result r,
                                  const char *why)
{
    g->result = r;
    if (why) {
        g->why = why;
    }
    g->step = S_DONE;
    return KBD_GAUGE_TICK_DONE;
}

/* Something went wrong. Before ENTER nothing needs undoing; after it the
 * gauge is taken out of CONFIG UPDATE, with re-initialisation only if the
 * commit may have happened. */
static enum kbd_gauge_tick fail(struct kbd_gauge_cfg *g, const char *why, uint64_t now_us)
{
    if (!g->why) {
        g->why = why;
    }
    if (g->step >= S_EXIT) {
        return KBD_GAUGE_TICK_NONE; /* already leaving; its own bounds apply */
    }
    if (g->step <= S_PRE_FCC) {
        return finish(g, KBD_GAUGE_REFUSED, NULL); /* only reads so far */
    }
    if (g->step < S_ENTER) {
        /* Keys only: the access level may have changed, the data has not. */
        return finish(g, KBD_GAUGE_FAILED_UNCHANGED, NULL);
    }
    /* ENTER was sent (perhaps not received): leave CONFIG UPDATE either way. */
    g->step = S_EXIT;
    g->exit_tries = 0;
    g->next_us = now_us + KBD_GAUGE_STEP_GAP_US;
    return KBD_GAUGE_TICK_NONE;
}

int kbd_gauge_cfg_start(struct kbd_gauge_cfg *g, const struct kbd_bus *bus, unsigned fcc_mah,
                        unsigned design_mah, uint64_t now_us)
{
    if (!g) {
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->bus = bus;
    g->step = S_DONE;
    if (!bus || !bus->claim || !bus->release || !bus->read_block_at || !bus->write_block_at) {
        g->result = KBD_GAUGE_REFUSED;
        g->why = "the bus cannot do block transfers";
        return -1;
    }
    if (fcc_mah < KBD_GAUGE_MAH_MIN || fcc_mah > KBD_GAUGE_MAH_MAX ||
        design_mah < KBD_GAUGE_MAH_MIN || design_mah > KBD_GAUGE_MAH_MAX) {
        g->result = KBD_GAUGE_REFUSED;
        g->why = "capacity outside 1..32767 mAh";
        return -1;
    }
    g->want_fcc = (uint16_t)fcc_mah;
    g->want_design = (uint16_t)design_mah;
    g->result = KBD_GAUGE_RUNNING;
    g->step = S_PRE_OPSTATUS;
    g->next_us = now_us;
    g->op_until_us = now_us + KBD_GAUGE_OP_MAX_US;
    return 0;
}

void kbd_gauge_cfg_backup_stored(struct kbd_gauge_cfg *g, bool stored)
{
    if (!g || g->step != S_WAIT_BACKUP) {
        return;
    }
    if (stored) {
        g->backup_stored = true;
    } else {
        g->why = "the backup could not be stored";
    }
}

bool kbd_gauge_cfg_keys_failed(struct kbd_gauge_cfg *g)
{
    if (!kbd_gauge_cfg_running(g)) {
        return false;
    }
    g->why = "the keyboard controller failed during the operation; bus left alone "
             "(the gauge leaves CONFIG UPDATE by itself after about 240 s)";
    finish(g, g->committed ? KBD_GAUGE_FAILED_UNKNOWN
                           : (g->step <= S_KEY1 ? KBD_GAUGE_REFUSED : KBD_GAUGE_FAILED_UNCHANGED),
           NULL);
    return true;
}

enum kbd_gauge_tick kbd_gauge_cfg_tick(struct kbd_gauge_cfg *g, uint64_t now_us)
{
    uint16_t v;
    uint8_t data[KBD_GAUGE_BLOCK_LEN];

    if (!kbd_gauge_cfg_running(g)) {
        return KBD_GAUGE_TICK_NONE;
    }
    /* The overall bound. The leaving steps have their own, shorter ones. */
    if (now_us >= g->op_until_us && g->step < S_EXIT) {
        return fail(g, "the operation took too long", now_us);
    }
    if (g->step == S_WAIT_BACKUP) {
        if (g->backup_stored) {
            g->step = S_WRITE_DATA; /* still paced from the block read */
        } else if (g->why || now_us >= g->wait_until_us) {
            return fail(g, "the backup was not stored in time", now_us);
        } else {
            return KBD_GAUGE_TICK_NONE;
        }
    }
    if (now_us < g->next_us) {
        return KBD_GAUGE_TICK_NONE;
    }
    g->next_us = now_us + KBD_GAUGE_STEP_GAP_US;

    switch (g->step) {
    case S_PRE_OPSTATUS:
        if (read16(g, KBD_GAUGE_REG_OPSTATUS, &v) != 0) {
            return fail(g, "OperationStatus() could not be read", now_us);
        }
        g->before_opstatus = v;
        if ((v & KBD_GAUGE_OS_SEC_MASK) == KBD_GAUGE_OS_SEC_SEALED) {
            return fail(g, "the gauge is SEALED; it is not unsealed here", now_us);
        }
        if (v & KBD_GAUGE_OS_CFGUPDATE) {
            return fail(g, "the gauge is already in CONFIG UPDATE", now_us);
        }
        g->step = S_PRE_DESIGN;
        break;
    case S_PRE_DESIGN:
        if (read16(g, KBD_GAUGE_REG_DESIGN, &g->before_design) != 0) {
            return fail(g, "DesignCapacity() could not be read", now_us);
        }
        g->step = S_PRE_FCC;
        break;
    case S_PRE_FCC:
        if (read16(g, KBD_GAUGE_REG_FCC, &g->before_fcc) != 0) {
            return fail(g, "FullChargeCapacity() could not be read", now_us);
        }
        g->step = S_KEY1;
        break;
    case S_KEY1:
    case S_KEY2:
        if (control(g, KBD_GAUGE_SUB_FULL_ACCESS_KEY) != 0) {
            return fail(g, "the FULL ACCESS key was not accepted on the bus", now_us);
        }
        g->step++;
        break;
    case S_ENTER:
        g->wait_until_us = now_us + KBD_GAUGE_ENTER_MAX_US;
        if (control(g, KBD_GAUGE_SUB_ENTER_CFG) != 0) {
            return fail(g, "ENTER_CFG_UPDATE was not accepted on the bus", now_us);
        }
        g->step = S_WAIT_ENTER;
        break;
    case S_WAIT_ENTER:
        if (read16(g, KBD_GAUGE_REG_OPSTATUS, &v) == 0 && (v & KBD_GAUGE_OS_CFGUPDATE)) {
            g->entered = true;
            g->step = S_SELECT;
        } else if (now_us >= g->wait_until_us) {
            return fail(g, "CONFIG UPDATE was not entered in time", now_us);
        }
        break;
    case S_SELECT:
    case S_SELECT_VERIFY:
        if (select_block(g) != 0) {
            return fail(g, "the data-memory address was not accepted", now_us);
        }
        g->step++;
        break;
    case S_READ_BACKUP:
        if (xact_read(g, KBD_GAUGE_REG_MAC, g->backup, KBD_GAUGE_READ_LEN) != 0) {
            return fail(g, "the data-memory block could not be read", now_us);
        }
        if (kbd_gauge_block_check(g->backup, KBD_GAUGE_DM_BLOCK) != 0) {
            return fail(g, "the data-memory block read back with a bad address, length or sum",
                        now_us);
        }
        g->step = S_WAIT_BACKUP;
        g->wait_until_us = now_us + KBD_GAUGE_BACKUP_MAX_US;
        return KBD_GAUGE_TICK_NEED_BACKUP;
    case S_WRITE_DATA:
        new_block(g, data);
        if (xact_write(g, KBD_GAUGE_REG_MACDATA, data, 4) != 0) {
            return fail(g, "the new values were not accepted on the bus", now_us);
        }
        g->step = S_WRITE_SUM;
        break;
    case S_WRITE_SUM: {
        uint8_t sl[2];

        new_block(g, data);
        sl[0] = kbd_gauge_block_sum(g->backup, data);
        sl[1] = KBD_GAUGE_MAC_LEN;
        /* From here the RAM may have changed, whatever the bus says. */
        g->committed = true;
        if (xact_write(g, KBD_GAUGE_REG_MACSUM, sl, 2) != 0) {
            return fail(g, "MACDataSum/MACDataLen were not accepted on the bus", now_us);
        }
        g->step = S_SELECT_VERIFY;
        break;
    }
    case S_READ_VERIFY:
        new_block(g, data);
        if (xact_read(g, KBD_GAUGE_REG_MAC, g->readback, KBD_GAUGE_READ_LEN) != 0) {
            return fail(g, "the block could not be read back", now_us);
        }
        if (kbd_gauge_block_check(g->readback, KBD_GAUGE_DM_BLOCK) != 0 ||
            memcmp(g->readback + 2, data, KBD_GAUGE_BLOCK_LEN) != 0) {
            return fail(g, "the block read back differs from what was written", now_us);
        }
        g->step = S_EXIT;
        g->exit_tries = 0;
        break;
    case S_EXIT:
        g->exit_tries++;
        if (control(g, g->committed ? KBD_GAUGE_SUB_EXIT_CFG_REINIT : KBD_GAUGE_SUB_EXIT_CFG) == 0) {
            g->step = S_WAIT_EXIT;
            g->wait_until_us = now_us + KBD_GAUGE_EXIT_MAX_US;
        } else if (g->exit_tries >= KBD_GAUGE_EXIT_TRIES) {
            g->why = g->why ? g->why : "the exit subcommand was not accepted";
            return finish(g, g->committed ? KBD_GAUGE_FAILED_UNKNOWN : KBD_GAUGE_FAILED_UNCHANGED,
                          NULL);
        }
        break;
    case S_WAIT_EXIT:
        if (read16(g, KBD_GAUGE_REG_OPSTATUS, &v) == 0 && !(v & KBD_GAUGE_OS_CFGUPDATE)) {
            g->after_opstatus = v;
            if (!g->committed) {
                return finish(g, KBD_GAUGE_FAILED_UNCHANGED, NULL);
            }
            g->step = S_POST_DESIGN;
        } else if (now_us >= g->wait_until_us) {
            if (g->exit_tries < KBD_GAUGE_EXIT_TRIES) {
                g->step = S_EXIT; /* send it again */
            } else {
                if (!g->why) {
                    g->why = "leaving CONFIG UPDATE was not confirmed (the gauge leaves by "
                             "itself after about 240 s)";
                }
                return finish(g, g->committed ? KBD_GAUGE_FAILED_UNKNOWN
                                              : KBD_GAUGE_FAILED_UNCHANGED,
                              NULL);
            }
        }
        break;
    case S_POST_DESIGN:
        if (read16(g, KBD_GAUGE_REG_DESIGN, &g->after_design) != 0) {
            return finish(g, KBD_GAUGE_FAILED_UNKNOWN, "DesignCapacity() could not be read after");
        }
        g->step = S_POST_FCC;
        break;
    case S_POST_FCC:
        if (read16(g, KBD_GAUGE_REG_FCC, &g->after_fcc) != 0) {
            return finish(g, KBD_GAUGE_FAILED_UNKNOWN,
                          "FullChargeCapacity() could not be read after");
        }
        if (g->why) {
            return finish(g, KBD_GAUGE_FAILED_CHANGED, NULL); /* an earlier failure */
        }
        if (g->after_design != g->want_design || g->after_fcc != g->want_fcc) {
            return finish(g, KBD_GAUGE_FAILED_CHANGED,
                          "the standard commands do not show the new values");
        }
        return finish(g, KBD_GAUGE_OK, NULL);
    default:
        return finish(g, KBD_GAUGE_FAILED_UNKNOWN, "internal: unknown step");
    }
    return KBD_GAUGE_TICK_NONE;
}

/* ---- backups and requests ------------------------------------------------ */

int kbd_gauge_backup_format(const struct kbd_gauge_cfg *g, const char *utc, char *buf,
                            size_t len)
{
    char hex[2 * KBD_GAUGE_READ_LEN + 1];
    unsigned i;
    int n;

    if (!g || !buf || len == 0) {
        return -1;
    }
    for (i = 0; i < KBD_GAUGE_READ_LEN; i++) {
        snprintf(hex + 2 * i, 3, "%02x", g->backup[i]);
    }
    n = snprintf(buf, len,
                 "doors-gauge-backup 1\n"
                 "utc %s\n"
                 "gauge bq27220 0x%02x\n"
                 "dm-address 0x%04X\n"
                 "read %s\n"
                 "full-charge-capacity-mah %u\n"
                 "design-capacity-mah %u\n"
                 "before-operation-status 0x%04x\n"
                 "before-std-design-capacity-mah %u\n"
                 "before-std-full-charge-capacity-mah %u\n",
                 utc ? utc : "unknown", KBD_GAUGE_ADDR, KBD_GAUGE_DM_BLOCK, hex,
                 kbd_gauge_be16(g->backup + 2 + KBD_GAUGE_DM_FCC_OFF),
                 kbd_gauge_be16(g->backup + 2 + KBD_GAUGE_DM_DESIGN_OFF), g->before_opstatus,
                 g->before_design, g->before_fcc);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

/* The value after "key " on its own line, or NULL. */
static const char *field(const char *text, const char *key)
{
    size_t k = strlen(key);
    const char *p = text;

    while (p && *p) {
        if (strncmp(p, key, k) == 0 && p[k] == ' ') {
            return p + k + 1;
        }
        p = strchr(p, '\n');
        if (p) {
            p++;
        }
    }
    return NULL;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

static int field_uint(const char *text, const char *key, unsigned *out)
{
    const char *v = field(text, key);
    char *end;
    unsigned long n;

    if (!v || !isdigit((unsigned char)*v)) {
        return -1;
    }
    n = strtoul(v, &end, 10);
    if (*end != '\n' && *end != '\0') {
        return -1;
    }
    *out = (unsigned)n;
    return n <= 0xFFFFu ? 0 : -1;
}

int kbd_gauge_backup_parse(const char *text, unsigned *fcc_mah, unsigned *design_mah)
{
    uint8_t read36[KBD_GAUGE_READ_LEN];
    const char *h;
    unsigned i;
    unsigned fcc;
    unsigned design;

    if (!text || !fcc_mah || !design_mah || strncmp(text, "doors-gauge-backup 1\n", 21) != 0) {
        return -1;
    }
    h = field(text, "read");
    if (!h) {
        return -1;
    }
    for (i = 0; i < KBD_GAUGE_READ_LEN; i++) {
        int hi = hexval((unsigned char)h[2 * i]);
        int lo = hi < 0 ? -1 : hexval((unsigned char)h[2 * i + 1]);

        if (lo < 0) {
            return -1;
        }
        read36[i] = (uint8_t)(hi << 4 | lo);
    }
    if (h[2 * KBD_GAUGE_READ_LEN] != '\n' && h[2 * KBD_GAUGE_READ_LEN] != '\0') {
        return -1;
    }
    if (kbd_gauge_block_check(read36, KBD_GAUGE_DM_BLOCK) != 0) {
        return -1;
    }
    fcc = kbd_gauge_be16(read36 + 2 + KBD_GAUGE_DM_FCC_OFF);
    design = kbd_gauge_be16(read36 + 2 + KBD_GAUGE_DM_DESIGN_OFF);
    /* The readable lines must agree with the bytes they summarise. */
    {
        unsigned f2;
        unsigned d2;

        if (field_uint(text, "full-charge-capacity-mah", &f2) != 0 ||
            field_uint(text, "design-capacity-mah", &d2) != 0 || f2 != fcc || d2 != design) {
            return -1;
        }
    }
    if (fcc < KBD_GAUGE_MAH_MIN || fcc > KBD_GAUGE_MAH_MAX || design < KBD_GAUGE_MAH_MIN ||
        design > KBD_GAUGE_MAH_MAX) {
        return -1;
    }
    *fcc_mah = fcc;
    *design_mah = design;
    return 0;
}

enum kbd_gauge_request_kind kbd_gauge_request_parse(const char *line, unsigned *mah, char *path,
                                                    size_t path_len)
{
    const char *p;
    char *end;
    unsigned long n;
    size_t l;

    if (!line) {
        return KBD_GAUGE_REQ_INVALID;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (strncmp(line, "capacity ", 9) == 0 && mah) {
        p = line + 9;
        if (!isdigit((unsigned char)*p)) {
            return KBD_GAUGE_REQ_INVALID;
        }
        n = strtoul(p, &end, 10);
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            end++;
        }
        if (*end != '\0' || n < KBD_GAUGE_MAH_MIN || n > KBD_GAUGE_MAH_MAX) {
            return KBD_GAUGE_REQ_INVALID;
        }
        *mah = (unsigned)n;
        return KBD_GAUGE_REQ_CAPACITY;
    }
    if (strncmp(line, "restore ", 8) == 0 && path && path_len > 1) {
        p = line + 8;
        l = strcspn(p, " \t\r\n");
        if (p[0] != '/' || l == 0 || l >= path_len || strstr(p, "..")) {
            return KBD_GAUGE_REQ_INVALID;
        }
        end = (char *)p + l;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            end++;
        }
        if (*end != '\0') {
            return KBD_GAUGE_REQ_INVALID;
        }
        memcpy(path, p, l);
        path[l] = '\0';
        return KBD_GAUGE_REQ_RESTORE;
    }
    return KBD_GAUGE_REQ_INVALID;
}
