/*
 * The owner-triggered BQ27220 capacity configuration (BATTERY_PROBE.md §7.5).
 *
 * One operation writes exactly two data-memory words of CEDV Profile 1, both
 * in one 32-byte block, and nothing else:
 *
 *   0x929D  Full Charge Capacity  I2, mAh  (the profile's learned FCC, which
 *                                          FullChargeCapacity() is set from
 *                                          at re-initialisation)
 *   0x929F  Design Capacity       I2, mAh
 *
 * The same operation restores a backup: it is "set these two values", and a
 * restore passes the values a backup recorded. Every run backs up the block
 * first, with a verified read-back, and does not write until the shell has
 * stored that backup.
 *
 * The sequence is TI's (SLUUBD4A §6.1 "Data Memory Parameter Update
 * Example", DOCUMENTED):
 *
 *   1. FULL ACCESS: Control() 0xFFFF, 0xFFFF. The gauge must already be
 *      UNSEALED; a SEALED gauge is refused, never unsealed here.
 *   2. ENTER_CFG_UPDATE: Control() 0x0090, then OperationStatus()[CFGUPDATE]
 *      polled until set ("may take up to 1 second").
 *   3. ManufacturerAccessControl() 0x3E/0x3F = the block address, low byte
 *      first; the gauge loads the block into MACData() 0x40-0x5F.
 *   4. Read 0x3E-0x61 in one read: address echo, 32 data bytes,
 *      MACDataSum(), MACDataLen(). Checked: the echo, the length 0x24, and
 *      the sum = 255 - (8-bit sum of the address bytes and the 32 data
 *      bytes) (SLUUBD4A §2.30; LILYGO's launcher uses the same rule).
 *   5. Write the four changed bytes at 0x40, big-endian (§6.1 writes 0x04
 *      0xB0 for 1200 mAh).
 *   6. Write MACDataSum() and MACDataLen() together as one word (§2.31); the
 *      data reaches RAM only when both are right (§6.1 step 13).
 *   7. Read the block back and compare all 32 bytes.
 *   8. EXIT_CFG_UPDATE_REINIT: Control() 0x0091, [CFGUPDATE] polled clear.
 *   9. DesignCapacity() and FullChargeCapacity() read and compared.
 *
 * Failure before step 6 leaves the gauge's RAM as it was: the operation
 * leaves CONFIG UPDATE with EXIT_CFG_UPDATE 0x0092 (no re-initialisation)
 * and reports "unchanged". From step 6 on it always leaves with 0x0091 and
 * reports what the standard commands then say. If leaving cannot be
 * confirmed, the gauge leaves CONFIG UPDATE by itself after about 240 s
 * (SLUUBD4A §4.6).
 *
 * What it does not do: seal, unseal a sealed gauge, write OTP, touch the
 * charger, or run on its own. Afterwards the gauge stays in FULL ACCESS
 * until its next power-on reset; TI has no subcommand back to UNSEALED
 * except sealing, which §6.1 step 16 only does for a gauge that was sealed.
 *
 * Data memory is volatile RAM (SLUUBD4A §3.1): a gauge power-on reset
 * returns both words to ROM defaults.
 *
 * Bounded: one bus transaction per tick, at least KBD_GAUGE_STEP_GAP_US
 * apart; each wait for the gauge has its own deadline; the whole operation
 * has KBD_GAUGE_OP_MAX_US. Each transaction claims and releases the bus
 * itself, so the keyboard keeps its turns between them. Pure: struct
 * kbd_bus only; tests/kbd_gauge_cfg_test.c drives it against a model.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_KBD_GAUGE_CFG_H
#define POCKETOS_KBD_GAUGE_CFG_H

#include "kbd_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KBD_GAUGE_ADDR 0x55u

/* Standard commands and MAC registers (SLUUBD4A §2). */
#define KBD_GAUGE_REG_CONTROL 0x00u
#define KBD_GAUGE_REG_FCC 0x12u
#define KBD_GAUGE_REG_OPSTATUS 0x3Au
#define KBD_GAUGE_REG_DESIGN 0x3Cu
#define KBD_GAUGE_REG_MAC 0x3Eu      /* ManufacturerAccessControl() */
#define KBD_GAUGE_REG_MACDATA 0x40u  /* MACData(), 32 bytes */
#define KBD_GAUGE_REG_MACSUM 0x60u   /* MACDataSum(), then MACDataLen() */

#define KBD_GAUGE_SUB_FULL_ACCESS_KEY 0xFFFFu
#define KBD_GAUGE_SUB_ENTER_CFG 0x0090u
#define KBD_GAUGE_SUB_EXIT_CFG_REINIT 0x0091u
#define KBD_GAUGE_SUB_EXIT_CFG 0x0092u

#define KBD_GAUGE_OS_CFGUPDATE 0x0400u
#define KBD_GAUGE_OS_SEC_MASK 0x0006u
#define KBD_GAUGE_OS_SEC_SEALED 0x0006u

/* The block: CEDV Profile 1 from Full Charge Capacity on. */
#define KBD_GAUGE_DM_BLOCK 0x929Du
#define KBD_GAUGE_DM_FCC_OFF 0u    /* 0x929D */
#define KBD_GAUGE_DM_DESIGN_OFF 2u /* 0x929F */
#define KBD_GAUGE_BLOCK_LEN 32u
#define KBD_GAUGE_MAC_LEN 0x24u    /* 32 data + address + sum + length */
/* What a read of 0x3E-0x61 returns: address (2), data (32), sum, length. */
#define KBD_GAUGE_READ_LEN (2u + KBD_GAUGE_BLOCK_LEN + 2u)

/* Data memory type I2 (SLUUBD4A Table 3-2): 0..32767. Zero is refused. */
#define KBD_GAUGE_MAH_MIN 1u
#define KBD_GAUGE_MAH_MAX 32767u

#define KBD_GAUGE_STEP_GAP_US 600000ULL     /* the probe's pacing */
#define KBD_GAUGE_ENTER_MAX_US 3000000ULL   /* TI: up to 1 s */
#define KBD_GAUGE_EXIT_MAX_US 3000000ULL    /* TI: up to 1 s */
#define KBD_GAUGE_OP_MAX_US 45000000ULL     /* the whole operation */
#define KBD_GAUGE_BACKUP_MAX_US 10000000ULL /* the shell storing the backup */
#define KBD_GAUGE_EXIT_TRIES 3

enum kbd_gauge_result {
    KBD_GAUGE_IDLE,
    KBD_GAUGE_RUNNING,
    KBD_GAUGE_OK,               /* written, read back, re-initialised, verified */
    KBD_GAUGE_REFUSED,          /* nothing sent beyond reads */
    KBD_GAUGE_FAILED_UNCHANGED, /* failed before the commit; RAM as it was */
    KBD_GAUGE_FAILED_CHANGED,   /* after the commit was sent: values reported, may differ */
    KBD_GAUGE_FAILED_UNKNOWN,   /* cannot say: run a restore after checking */
};

/* What tick() asks of its caller. */
enum kbd_gauge_tick {
    KBD_GAUGE_TICK_NONE,
    KBD_GAUGE_TICK_NEED_BACKUP, /* store g->backup, then kbd_gauge_cfg_backup_stored() */
    KBD_GAUGE_TICK_DONE,        /* g->result is final */
};

struct kbd_gauge_cfg {
    const struct kbd_bus *bus;
    enum kbd_gauge_result result;
    const char *why; /* a short reason for a non-OK result */
    int step;
    uint64_t next_us;
    uint64_t wait_until_us;  /* the current wait's deadline */
    uint64_t op_until_us;
    uint16_t want_fcc;
    uint16_t want_design;
    bool entered;    /* CFGUPDATE was seen set */
    bool committed;  /* sum and length were sent (RAM may have changed) */
    bool backup_stored;
    unsigned exit_tries;
    uint16_t before_opstatus;
    uint16_t before_design;
    uint16_t before_fcc;
    uint16_t after_opstatus;
    uint16_t after_design;
    uint16_t after_fcc;
    uint8_t backup[KBD_GAUGE_READ_LEN]; /* address, block, sum, length as read */
    uint8_t readback[KBD_GAUGE_READ_LEN];
    unsigned transactions;
    unsigned failures;
};

/* Start setting Full Charge Capacity and Design Capacity. Values outside
 * KBD_GAUGE_MAH_MIN..MAX, or a bus without block reads and writes, give
 * KBD_GAUGE_REFUSED at once with nothing sent. 0 started, -1 refused. */
int kbd_gauge_cfg_start(struct kbd_gauge_cfg *g, const struct kbd_bus *bus, unsigned fcc_mah,
                        unsigned design_mah, uint64_t now_us);

/* At most one bus transaction. */
enum kbd_gauge_tick kbd_gauge_cfg_tick(struct kbd_gauge_cfg *g, uint64_t now_us);

/* The caller stored g->backup (true) or could not (false: the operation
 * leaves CONFIG UPDATE without writing). */
void kbd_gauge_cfg_backup_stored(struct kbd_gauge_cfg *g, bool stored);

/* The keyboard controller failed: stop touching the bus at once. The keys
 * come first; the gauge leaves CONFIG UPDATE by itself after about 240 s.
 * True when an operation was running (its result is then final). */
bool kbd_gauge_cfg_keys_failed(struct kbd_gauge_cfg *g);

bool kbd_gauge_cfg_running(const struct kbd_gauge_cfg *g);
const char *kbd_gauge_result_name(enum kbd_gauge_result r);

/* ---- the block and its checksum (pure) --------------------------------- */

/* 255 - (8-bit sum of the address bytes and the 32 data bytes). */
uint8_t kbd_gauge_block_sum(const uint8_t *addr2, const uint8_t *data32);

/* A read of 0x3E-0x61 is whole: echo, length and sum all right. 0 or -1. */
int kbd_gauge_block_check(const uint8_t *read36, uint16_t dm_addr);

/* Big-endian, as data memory stores it. */
uint16_t kbd_gauge_be16(const uint8_t *p);

/* ---- backups and requests (pure text) ---------------------------------- */

/* The backup file's text for g->backup. 0, or -1 when it did not fit. */
int kbd_gauge_backup_format(const struct kbd_gauge_cfg *g, const char *utc, char *buf,
                            size_t len);

/* A backup's text back to its two values, after checking the block it holds
 * (address, length, sum). 0, or -1 for anything not exactly right. */
int kbd_gauge_backup_parse(const char *text, unsigned *fcc_mah, unsigned *design_mah);

enum kbd_gauge_request_kind {
    KBD_GAUGE_REQ_INVALID,
    KBD_GAUGE_REQ_CAPACITY, /* "capacity <mAh>": both words to that value */
    KBD_GAUGE_REQ_RESTORE,  /* "restore <backup file>" */
};

/* One request line. For RESTORE the path is copied into path (absolute,
 * at most path_len - 1 bytes). */
enum kbd_gauge_request_kind kbd_gauge_request_parse(const char *line, unsigned *mah, char *path,
                                                    size_t path_len);

#endif
