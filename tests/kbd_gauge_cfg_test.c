/*
 * The owner-triggered BQ27220 capacity configuration (ui/shell/
 * kbd_gauge_cfg.h) against a model of the gauge's MAC and data-memory
 * interface as SLUUBD4A §2.29-2.31 and §6.1 describe it: success, timeouts,
 * partial failures, a keyboard failure part way, and restoring a backup.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_gauge_cfg.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

#define SEC 1000000ULL

/* ---- the model ---------------------------------------------------------- */

static struct model {
    /* gauge state */
    unsigned sec;          /* 3 sealed, 2 unsealed, 1 full access */
    unsigned keys;         /* consecutive 0xFFFF keys */
    bool cfg;
    bool entering;
    bool exiting;
    int enter_polls;       /* polls until CFGUPDATE shows; -1 never */
    int exit_polls;        /* polls until it clears; -1 never */
    int enter_in;          /* the running countdowns */
    int exit_in;
    bool reinit_pending;
    uint8_t dm[KBD_GAUGE_BLOCK_LEN];  /* RAM data memory at 0x929D */
    uint16_t sel;                     /* ManufacturerAccessControl() */
    uint8_t mac[KBD_GAUGE_BLOCK_LEN]; /* MACData() */
    uint16_t std_design;
    uint16_t std_fcc;
    unsigned commits;
    /* faults */
    bool absent;
    bool claim_fails;
    bool corrupt_read;     /* the block reads back with a wrong sum */
    bool corrupt_commit;   /* the commit lands one byte wrong */
    int fail_write_reg;    /* -1 none; a write to this register fails */
    /* observation */
    int depth;
    int claims;
    int outside_claim;
    unsigned transactions;
    uint64_t now;
    uint64_t at[512];
    unsigned subs[64];     /* Control() subcommands, in order */
    unsigned nsubs;
    bool wrote_data_before_backup;
    bool backup_asked;
    int other_addr;        /* transactions at anything but 0x55 */
    int other_reg_writes;  /* writes to registers outside the procedure */
} m;

static void model_reset(void)
{
    static const uint8_t block[KBD_GAUGE_BLOCK_LEN] = {
        0x0D, 0xB8, /* 0x929D Full Charge Capacity 3512 */
        0x0B, 0xB8, /* 0x929F Design Capacity 3000 */
        0x00, 0x00, /* 0x92A1 */
        0x0E, 0x74, /* 0x92A3 Design Voltage 3700 */
        0x00, 0x64, /* 0x92A5 Charge Termination Voltage 100 */
        0x0E, 0x9F, /* 0x92A7 EMF 3743 */
        0x00, 0x95, 0x03, 0x63, 0x0F, 0xBE, 0x01, 0x3C, 0x09, 0x00, 0x00,
        0x0B, 0xD7, 0x01, 0x0D, 0x39, 0x01, 0x0D, 0xAD, 0x01,
    };

    memset(&m, 0, sizeof(m));
    m.sec = 2;
    m.enter_polls = 2;
    m.exit_polls = 2;
    m.fail_write_reg = -1;
    memcpy(m.dm, block, sizeof(block));
    m.std_design = 3000;
    m.std_fcc = 3512;
}

static uint16_t opstatus(void)
{
    return (uint16_t)((m.sec << 1) | (m.cfg ? KBD_GAUGE_OS_CFGUPDATE : 0u) | 0x0020u);
}

static int m_claim(void *ctx)
{
    (void)ctx;
    if (m.claim_fails) {
        return -1;
    }
    m.claims++;
    m.depth++;
    return 0;
}

static void m_release(void *ctx)
{
    (void)ctx;
    m.depth--;
}

static void note(uint8_t addr)
{
    if (m.transactions < 512) {
        m.at[m.transactions] = m.now;
    }
    m.transactions++;
    if (addr != KBD_GAUGE_ADDR) {
        m.other_addr++;
    }
}

static int m_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len)
{
    uint16_t v;

    (void)ctx;
    if (m.depth <= 0) {
        m.outside_claim++;
        return -1;
    }
    note(addr);
    if (m.absent || addr != KBD_GAUGE_ADDR) {
        return -1;
    }
    if (reg == KBD_GAUGE_REG_MAC && len == KBD_GAUGE_READ_LEN) {
        buf[0] = (uint8_t)(m.sel & 0xFF);
        buf[1] = (uint8_t)(m.sel >> 8);
        memcpy(buf + 2, m.mac, KBD_GAUGE_BLOCK_LEN);
        buf[34] = kbd_gauge_block_sum(buf, buf + 2);
        if (m.corrupt_read) {
            buf[34] ^= 0x01;
        }
        buf[35] = KBD_GAUGE_MAC_LEN;
        return 0;
    }
    if (len != 2) {
        return -1;
    }
    switch (reg) {
    case KBD_GAUGE_REG_OPSTATUS:
        /* Entering and leaving take a few polls, or never happen. */
        if (m.exiting && m.cfg && m.exit_in >= 0 && m.exit_in-- == 0) {
            m.cfg = false;
            m.exiting = false;
            if (m.reinit_pending) {
                /* Re-initialisation: DesignCapacity() and the FCC come from
                 * data memory (SLUUBD4A §1.1.2, §2.28). */
                m.std_fcc = (uint16_t)(m.dm[0] << 8 | m.dm[1]);
                m.std_design = (uint16_t)(m.dm[2] << 8 | m.dm[3]);
                m.reinit_pending = false;
            }
        } else if (m.exiting && !m.cfg) {
            m.exiting = false; /* nothing to leave */
        } else if (m.entering && !m.cfg && m.enter_in >= 0 && m.enter_in-- == 0) {
            m.cfg = true;
            m.entering = false;
        }
        v = opstatus();
        break;
    case KBD_GAUGE_REG_DESIGN:
        v = m.std_design;
        break;
    case KBD_GAUGE_REG_FCC:
        v = m.std_fcc;
        break;
    default:
        return -1;
    }
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)(v >> 8);
    return 0;
}

static int m_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *buf, unsigned len)
{
    (void)ctx;
    if (m.depth <= 0) {
        m.outside_claim++;
        return -1;
    }
    note(addr);
    if (m.absent || addr != KBD_GAUGE_ADDR || (int)reg == m.fail_write_reg) {
        return -1;
    }
    if (reg == KBD_GAUGE_REG_CONTROL && len == 2) {
        unsigned sub = (unsigned)(buf[0] | buf[1] << 8);

        if (m.nsubs < 64) {
            m.subs[m.nsubs++] = sub;
        }
        if (sub == 0xFFFF) {
            if (++m.keys >= 2 && m.sec == 2) {
                m.sec = 1;
            }
        } else {
            m.keys = 0;
        }
        if (sub == 0x0090 && m.sec == 1) {
            m.entering = true; /* full access first (SLUUBD4A §6.1 step 2) */
            m.enter_in = m.enter_polls;
        }
        if (sub == 0x0091 || sub == 0x0092) {
            m.exiting = true;
            m.exit_in = m.exit_polls;
            m.reinit_pending = sub == 0x0091;
        }
        return 0;
    }
    if (reg == KBD_GAUGE_REG_MAC && len == 2) {
        m.sel = (uint16_t)(buf[0] | buf[1] << 8);
        if (m.sel == KBD_GAUGE_DM_BLOCK) {
            memcpy(m.mac, m.dm, KBD_GAUGE_BLOCK_LEN);
        }
        return 0;
    }
    if (reg == KBD_GAUGE_REG_MACDATA && len <= KBD_GAUGE_BLOCK_LEN) {
        if (!m.backup_asked) {
            m.wrote_data_before_backup = true;
        }
        memcpy(m.mac, buf, len);
        return 0;
    }
    if (reg == KBD_GAUGE_REG_MACSUM && len == 2) {
        uint8_t a[2] = {(uint8_t)(m.sel & 0xFF), (uint8_t)(m.sel >> 8)};

        /* Into RAM only in CONFIG UPDATE, with full access, and with the
         * right sum and length (SLUUBD4A §6.1 steps 2, 3, 13). */
        if (m.cfg && m.sec == 1 && m.sel == KBD_GAUGE_DM_BLOCK && buf[1] == KBD_GAUGE_MAC_LEN &&
            buf[0] == kbd_gauge_block_sum(a, m.mac)) {
            memcpy(m.dm, m.mac, KBD_GAUGE_BLOCK_LEN);
            if (m.corrupt_commit) {
                m.dm[3] ^= 0x01;
            }
            m.commits++;
        }
        return 0;
    }
    m.other_reg_writes++;
    return 0;
}

static struct kbd_bus model_bus(void)
{
    struct kbd_bus b;

    memset(&b, 0, sizeof(b));
    b.claim = m_claim;
    b.release = m_release;
    b.read_block_at = m_read;
    b.write_block_at = m_write;
    return b;
}

/* Run an operation to the end, storing the backup when asked (or not).
 * Returns the simulated time it took. */
static uint64_t run(struct kbd_gauge_cfg *g, int store_backup, char *backup_text, size_t len)
{
    uint64_t t0 = m.now;
    enum kbd_gauge_tick t;

    while (kbd_gauge_cfg_running(g) && m.now - t0 < 600 * SEC) {
        t = kbd_gauge_cfg_tick(g, m.now);
        if (t == KBD_GAUGE_TICK_NEED_BACKUP) {
            m.backup_asked = true;
            if (backup_text) {
                kbd_gauge_backup_format(g, "2026-10-10T00:00:00Z", backup_text, len);
            }
            if (store_backup >= 0) {
                kbd_gauge_cfg_backup_stored(g, store_backup != 0);
            }
        }
        m.now += SEC / 10;
    }
    return m.now - t0;
}

static bool sub_sent(unsigned sub)
{
    unsigned i;

    for (i = 0; i < m.nsubs; i++) {
        if (m.subs[i] == sub) {
            return true;
        }
    }
    return false;
}

static bool paced(void)
{
    unsigned i;

    for (i = 1; i < m.transactions && i < 512; i++) {
        if (m.at[i] - m.at[i - 1] < KBD_GAUGE_STEP_GAP_US) {
            return false;
        }
    }
    return true;
}

static bool clean_bus(void)
{
    return m.depth == 0 && m.outside_claim == 0 && m.other_addr == 0 && m.other_reg_writes == 0;
}

int main(void)
{
    struct kbd_bus bus;
    struct kbd_gauge_cfg g;
    char backup[1024];
    char backup2[1024];
    uint8_t orig[KBD_GAUGE_BLOCK_LEN];
    unsigned fcc;
    unsigned design;
    unsigned mah;
    char path[128];
    uint64_t took;

    /* ---- 1. the sum, TI's example in reverse ----------------------------- */
    {
        uint8_t a[2] = {0x9D, 0x92};
        uint8_t d[KBD_GAUGE_BLOCK_LEN] = {0};
        unsigned s;

        d[0] = 0x17;
        d[1] = 0x70;
        s = (0x9D + 0x92 + 0x17 + 0x70) & 0xFF;
        check("the sum is 255 minus the 8-bit sum of address and data",
              kbd_gauge_block_sum(a, d) == (uint8_t)(0xFF - s));
        check("data memory words are big-endian", kbd_gauge_be16(d) == 6000);
    }

    /* ---- 2. success ------------------------------------------------------- */

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    bus = model_bus();
    check("a capacity request starts", kbd_gauge_cfg_start(&g, &bus, 6000, 6000, m.now) == 0);
    took = run(&g, 1, backup, sizeof(backup));
    check("and succeeds", g.result == KBD_GAUGE_OK);
    check("the two words are 6000 in data memory",
          kbd_gauge_be16(m.dm) == 6000 && kbd_gauge_be16(m.dm + 2) == 6000);
    check("and nothing else in the block changed", memcmp(m.dm + 4, orig + 4, 28) == 0);
    check("one commit", m.commits == 1);
    check("the standard commands show it after re-initialisation",
          g.after_design == 6000 && g.after_fcc == 6000 && m.std_design == 6000);
    check("the backup holds the block as it was", memcmp(g.backup + 2, orig, 32) == 0 &&
                                                       g.before_design == 3000 &&
                                                       g.before_fcc == 3512);
    check("the backup was taken before any data was written", !m.wrote_data_before_backup);
    check("full access, enter, exit-with-reinit, in that order",
          m.nsubs == 4 && m.subs[0] == 0xFFFF && m.subs[1] == 0xFFFF && m.subs[2] == 0x0090 &&
              m.subs[3] == 0x0091);
    check("never sealed, never exit-without-reinit", !sub_sent(0x0030) && !sub_sent(0x0092));
    check("only the gauge, only the procedure's registers", clean_bus());
    check("one transaction at a time, paced", paced());
    check("bounded: done well inside the operation's limit", took < KBD_GAUGE_OP_MAX_US);
    check("the gauge left CONFIG UPDATE", !m.cfg);
    check("and stays in full access, as TI's procedure leaves it", m.sec == 1);

    /* ---- 3. restoring the backup ------------------------------------------- */

    check("the backup text parses back to its values",
          kbd_gauge_backup_parse(backup, &fcc, &design) == 0 && fcc == 3512 && design == 3000);
    m.nsubs = 0;
    m.backup_asked = false;
    check("a restore starts", kbd_gauge_cfg_start(&g, &bus, fcc, design, m.now) == 0);
    run(&g, 1, backup2, sizeof(backup2));
    check("and succeeds", g.result == KBD_GAUGE_OK);
    if (g.result != KBD_GAUGE_OK) {
        printf("     restore: %s: %s\n", kbd_gauge_result_name(g.result), g.why ? g.why : "-");
    }
    check("the block is byte for byte what it was", memcmp(m.dm, orig, 32) == 0);
    check("the standard commands are back", m.std_design == 3000 && m.std_fcc == 3512);
    check("and the restore backed up the 6000 first",
          kbd_gauge_backup_parse(backup2, &fcc, &design) == 0 && fcc == 6000 && design == 6000);

    /* ---- 4. refusals: nothing written -------------------------------------- */

    model_reset();
    bus = model_bus();
    check("0 mAh is refused", kbd_gauge_cfg_start(&g, &bus, 0, 0, 0) != 0 &&
                                  g.result == KBD_GAUGE_REFUSED && m.transactions == 0);
    check("32768 mAh is refused", kbd_gauge_cfg_start(&g, &bus, 32768, 32768, 0) != 0 &&
                                      m.transactions == 0);
    bus.write_block_at = NULL;
    check("a bus without block writes is refused",
          kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0) != 0 && m.transactions == 0);

    model_reset();
    m.sec = 3;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("a sealed gauge is refused after one read", g.result == KBD_GAUGE_REFUSED &&
                                                          m.transactions == 1 && m.nsubs == 0);

    model_reset();
    m.cfg = true;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("a gauge already in CONFIG UPDATE is refused", g.result == KBD_GAUGE_REFUSED &&
                                                             m.nsubs == 0);

    model_reset();
    m.absent = true;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("an absent gauge is refused after one read", g.result == KBD_GAUGE_REFUSED &&
                                                           m.transactions == 1);
    check("and the bus is left released", m.depth == 0);

    model_reset();
    m.claim_fails = true;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("a bus that cannot be claimed is refused", g.result == KBD_GAUGE_REFUSED);

    /* ---- 5. timeouts -------------------------------------------------------- */

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    m.enter_polls = -1;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    took = run(&g, 1, NULL, 0);
    check("CONFIG UPDATE never entered: failed, unchanged",
          g.result == KBD_GAUGE_FAILED_UNCHANGED && memcmp(m.dm, orig, 32) == 0);
    check("left with EXIT_CFG_UPDATE, no re-initialisation",
          sub_sent(0x0092) && !sub_sent(0x0091));
    check("within the enter bound and a few steps",
          took < KBD_GAUGE_ENTER_MAX_US + KBD_GAUGE_EXIT_MAX_US + 10 * KBD_GAUGE_STEP_GAP_US);
    check("bus clean", clean_bus());

    model_reset();
    m.exit_polls = -1;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    took = run(&g, 1, NULL, 0);
    check("an exit that is never confirmed after the commit: unknown",
          g.result == KBD_GAUGE_FAILED_UNKNOWN && g.why != NULL);
    check("after three tries, not forever",
          g.exit_tries == KBD_GAUGE_EXIT_TRIES && took < KBD_GAUGE_OP_MAX_US);

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    took = run(&g, -1, NULL, 0); /* the shell never answers */
    check("a backup that is never stored: failed, unchanged",
          g.result == KBD_GAUGE_FAILED_UNCHANGED && memcmp(m.dm, orig, 32) == 0 &&
              m.commits == 0 && sub_sent(0x0092));
    check("after the backup bound", took >= KBD_GAUGE_BACKUP_MAX_US && took < KBD_GAUGE_OP_MAX_US);

    /* ---- 6. partial failures ------------------------------------------------ */

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 0, NULL, 0); /* the shell could not store it */
    check("a backup that could not be stored: no write at all",
          g.result == KBD_GAUGE_FAILED_UNCHANGED && m.commits == 0 &&
              memcmp(m.dm, orig, 32) == 0 && !sub_sent(0x0091));

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    m.corrupt_read = true;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("a block that reads back with a bad sum: no backup, no write",
          g.result == KBD_GAUGE_FAILED_UNCHANGED && !m.backup_asked && m.commits == 0 &&
              memcmp(m.dm, orig, 32) == 0);

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    m.fail_write_reg = KBD_GAUGE_REG_MACDATA;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("the data write fails: unchanged, left without re-initialisation",
          g.result == KBD_GAUGE_FAILED_UNCHANGED && memcmp(m.dm, orig, 32) == 0 &&
              sub_sent(0x0092) && !m.cfg);

    model_reset();
    memcpy(orig, m.dm, sizeof(orig));
    m.fail_write_reg = KBD_GAUGE_REG_MACSUM;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("the sum write fails: reported as after-commit, with what the gauge says",
          g.result == KBD_GAUGE_FAILED_CHANGED && g.after_design == 3000 &&
              g.after_fcc == 3512 && memcmp(m.dm, orig, 32) == 0);
    check("and left with re-initialisation, out of CONFIG UPDATE", sub_sent(0x0091) && !m.cfg);

    model_reset();
    m.corrupt_commit = true;
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    run(&g, 1, NULL, 0);
    check("a commit that lands wrong is caught by the read-back",
          g.result == KBD_GAUGE_FAILED_CHANGED && g.why && strstr(g.why, "read back") && !m.cfg);

    /* ---- 7. the keys come first --------------------------------------------- */

    model_reset();
    bus = model_bus();
    kbd_gauge_cfg_start(&g, &bus, 6000, 6000, 0);
    while (kbd_gauge_cfg_running(&g) && !m.cfg) {
        kbd_gauge_cfg_tick(&g, m.now);
        m.now += SEC / 10;
    }
    {
        unsigned before = m.transactions;

        check("a keyboard failure stops it", kbd_gauge_cfg_keys_failed(&g) &&
                                                 g.result == KBD_GAUGE_FAILED_UNCHANGED);
        run(&g, 1, NULL, 0);
        check("and nothing more touches the bus", m.transactions == before && m.depth == 0);
        check("no second stop", !kbd_gauge_cfg_keys_failed(&g));
    }

    /* ---- 8. backups and requests as text ------------------------------------- */
    {
        char t[1024];
        char *p;

        strcpy(t, backup);
        p = strstr(t, "read ");
        p[5 + 10] = p[5 + 10] == '0' ? '1' : '0'; /* one nibble of the data */
        check("a tampered block is rejected", kbd_gauge_backup_parse(t, &fcc, &design) != 0);
        strcpy(t, backup);
        p = strstr(t, "design-capacity-mah 3000");
        p[20] = '1';
        check("a summary that disagrees with the bytes is rejected",
              kbd_gauge_backup_parse(t, &fcc, &design) != 0);
        check("something else entirely is rejected",
              kbd_gauge_backup_parse("hello\n", &fcc, &design) != 0);
    }
    check("'capacity 6000' asks for 6000",
          kbd_gauge_request_parse("capacity 6000\n", &mah, path, sizeof(path)) ==
                  KBD_GAUGE_REQ_CAPACITY && mah == 6000);
    check("'capacity' alone is invalid",
          kbd_gauge_request_parse("capacity\n", &mah, path, sizeof(path)) == KBD_GAUGE_REQ_INVALID);
    check("'capacity 0' and 'capacity 6000x' are invalid",
          kbd_gauge_request_parse("capacity 0", &mah, path, sizeof(path)) ==
                  KBD_GAUGE_REQ_INVALID &&
              kbd_gauge_request_parse("capacity 6000x", &mah, path, sizeof(path)) ==
                  KBD_GAUGE_REQ_INVALID);
    check("a restore names an absolute file",
          kbd_gauge_request_parse("restore /var/lib/pocketos/battery/a.txt\n", &mah, path,
                                  sizeof(path)) == KBD_GAUGE_REQ_RESTORE &&
              strcmp(path, "/var/lib/pocketos/battery/a.txt") == 0);
    check("a relative or climbing path is invalid",
          kbd_gauge_request_parse("restore a.txt", &mah, path, sizeof(path)) ==
                  KBD_GAUGE_REQ_INVALID &&
              kbd_gauge_request_parse("restore /a/../b", &mah, path, sizeof(path)) ==
                  KBD_GAUGE_REQ_INVALID);
    check("there is no default capacity",
          kbd_gauge_request_parse("capacity default", &mah, path, sizeof(path)) ==
              KBD_GAUGE_REQ_INVALID);

    printf("kbd_gauge_cfg_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
