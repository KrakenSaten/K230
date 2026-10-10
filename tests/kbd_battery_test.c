/*
 * The battery probe against a register model of the BQ27220 and BQ25896
 * (ui/shell/kbd_battery.h): decoding against TI's register definitions,
 * devices that do not answer, the pacing the gauge's datasheet demands, and
 * that nothing is ever written.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_battery.h"

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

/* ---- the model ---------------------------------------------------------- */

static struct model {
    bool gauge;              /* answers at 0x55 */
    bool charger;            /* answers at 0x6B */
    bool claim_fails;
    uint16_t word[0x40];     /* gauge standard commands, by command code */
    uint8_t reg0b;
    int claims;
    int releases;
    int depth;               /* claims not yet released */
    int outside_claim;       /* reads attempted without a claim */
    int writes;              /* any write of any kind: must stay 0 */
    unsigned reads;
    uint64_t now;            /* the test's clock, for the read log */
    uint64_t read_at[256];   /* when each read happened */
    uint8_t read_addr[256];
} m;

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
    m.releases++;
    m.depth--;
}

static int m_read_reg(void *ctx, uint8_t reg, uint8_t *v)
{
    (void)ctx; (void)reg; (void)v;
    return -1; /* the keyboard controller is not the probe's business */
}

static int m_write_reg(void *ctx, uint8_t reg, uint8_t v)
{
    (void)ctx; (void)reg; (void)v;
    m.writes++;
    return -1;
}

static int m_write_reg_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t v)
{
    (void)ctx; (void)addr; (void)reg; (void)v;
    m.writes++;
    return -1;
}

static int m_read_block_at(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len)
{
    (void)ctx;
    if (m.depth <= 0) {
        m.outside_claim++;
        return -1;
    }
    if (m.reads < 256) {
        m.read_at[m.reads] = m.now;
        m.read_addr[m.reads] = addr;
    }
    m.reads++;
    if (addr == KBD_BATTERY_GAUGE_ADDR && m.gauge && len == 2 && reg < 0x40) {
        buf[0] = (uint8_t)(m.word[reg] & 0xFFu);
        buf[1] = (uint8_t)(m.word[reg] >> 8);
        return 0;
    }
    if (addr == KBD_BATTERY_CHARGER_ADDR && m.charger && len == 1 && reg == BQ25896_REG_STATUS) {
        buf[0] = m.reg0b;
        return 0;
    }
    return -1; /* no acknowledge */
}

static struct kbd_bus model_bus(void)
{
    struct kbd_bus b;

    memset(&b, 0, sizeof(b));
    b.claim = m_claim;
    b.release = m_release;
    b.read_reg = m_read_reg;
    b.write_reg = m_write_reg;
    b.write_reg_at = m_write_reg_at;
    b.read_block_at = m_read_block_at;
    return b;
}

static void model_reset(void)
{
    memset(&m, 0, sizeof(m));
    m.gauge = true;
    m.charger = true;
    m.word[BQ27220_CMD_STATE_OF_CHARGE] = 87;
    m.word[BQ27220_CMD_VOLTAGE] = 4012;
    m.word[BQ27220_CMD_CURRENT] = 0xFEC8; /* -312 mA */
    m.word[BQ27220_CMD_BATTERY_STATUS] = BQ27220_BS_DSG | BQ27220_BS_BATTPRES;
    m.word[BQ27220_CMD_FULL_CHARGE_CAPACITY] = 5800;
    m.word[BQ27220_CMD_DESIGN_CAPACITY] = 6000;
    m.word[BQ27220_CMD_OPERATION_STATUS] = (3u << BQ27220_OS_SEC_SHIFT) | BQ27220_OS_INITCOMP;
    /* No input, not charging, not power good, reserved bit set. */
    m.reg0b = BQ25896_ST_RSVD_ONE;
}

/* Run the probe from t0 to t1 in steps of dt, as the watch would. Returns
 * how many samples completed. */
static unsigned run(struct kbd_battery *b, uint64_t t0, uint64_t t1, uint64_t dt)
{
    unsigned done = 0;

    for (m.now = t0; m.now < t1; m.now += dt) {
        done += (unsigned)kbd_battery_tick(b, m.now);
    }
    return done;
}

#define SEC 1000000ULL

int main(void)
{
    struct kbd_bus bus;
    struct kbd_battery b;
    char line[512];
    unsigned i;
    unsigned done;

    /* ---- 1. decoding, from TI's definitions ---------------------------- */

    check("Current() is two's complement: 0xFEC8 is -312 mA",
          kbd_battery_current_ma(0xFEC8) == -312);
    check("and 0x0140 is +320 mA", kbd_battery_current_ma(0x0140) == 320);
    check("and 0x8000 is the most negative", kbd_battery_current_ma(0x8000) == -32768);
    check("SOC 100 is plausible", kbd_battery_check(KBD_BATTERY_SOC, 100) == 0);
    check("SOC 101 is suspect", kbd_battery_check(KBD_BATTERY_SOC, 101) != 0);
    check("6000 mV is the top of Voltage()'s range",
          kbd_battery_check(KBD_BATTERY_VOLTAGE, 6000) == 0);
    check("6001 mV is suspect", kbd_battery_check(KBD_BATTERY_VOLTAGE, 6001) != 0);
    check("a capacity above 32767 mAh is suspect",
          kbd_battery_check(KBD_BATTERY_DESIGN, 0x8000) != 0 &&
              kbd_battery_check(KBD_BATTERY_FCC, 0xFFFF) != 0);
    check("BatteryStatus() with its reserved bit set is suspect",
          kbd_battery_check(KBD_BATTERY_STATUS, BQ27220_BS_RSVD) != 0);
    check("OperationStatus() may have CFGUPDATE in its high byte",
          kbd_battery_check(KBD_BATTERY_OPSTATUS, BQ27220_OS_CFGUPDATE) == 0);
    check("but not a reserved bit",
          kbd_battery_check(KBD_BATTERY_OPSTATUS, 0x0100) != 0 &&
              kbd_battery_check(KBD_BATTERY_OPSTATUS, 0x8000) != 0);
    check("REG0B bit 1 always reads 1, so 0 there is suspect",
          kbd_battery_check(KBD_BATTERY_CHARGER, 0x00) != 0 &&
              kbd_battery_check(KBD_BATTERY_CHARGER, 0x02) == 0);
    check("SEC 3 is sealed", strcmp(kbd_battery_security_name(kbd_battery_security(0x0006)),
                                    "sealed") == 0);
    check("SEC 2 is unsealed", strcmp(kbd_battery_security_name(kbd_battery_security(0x0004)),
                                      "unsealed") == 0);
    check("SEC 1 is full access", strcmp(kbd_battery_security_name(kbd_battery_security(0x0002)),
                                         "full-access") == 0);
    check("VBUS_STAT 001 is a USB host", kbd_battery_vbus_stat(0x20) == 1 &&
                                            strcmp(kbd_battery_vbus_name(1), "usb-sdp") == 0);
    check("VBUS_STAT 111 is OTG", kbd_battery_vbus_stat(0xE0) == 7 &&
                                     strcmp(kbd_battery_vbus_name(7), "otg") == 0);
    check("a VBUS_STAT the datasheet does not name is said to be undefined",
          strcmp(kbd_battery_vbus_name(4), "undefined") == 0);
    check("CHRG_STAT 10 is fast charging", kbd_battery_chrg_stat(0x10) == 2 &&
                                              strcmp(kbd_battery_chrg_name(2), "fast-charging") == 0);
    check("CHRG_STAT 11 is termination done", kbd_battery_chrg_stat(0x18) == 3 &&
                                                 strcmp(kbd_battery_chrg_name(3), "done") == 0);

    /* ---- 2. one full sample -------------------------------------------- */

    model_reset();
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 20 * SEC, SEC);
    check("a sample completes", done == 1);
    check("in eight reads, one per register", m.reads == KBD_BATTERY_NREGS);
    check("seven of them at the gauge and the last at the charger",
          m.read_addr[0] == KBD_BATTERY_GAUGE_ADDR && m.read_addr[6] == KBD_BATTERY_GAUGE_ADDR &&
              m.read_addr[7] == KBD_BATTERY_CHARGER_ADDR);
    check("every read inside a claim", m.outside_claim == 0);
    check("every claim released", m.claims == m.releases && m.depth == 0);
    check("one claim per read", m.claims == (int)m.reads);
    check("and nothing written, ever", m.writes == 0);
    check("all registers read", b.last.have == (1u << KBD_BATTERY_NREGS) - 1u);
    check("nothing suspect", b.last.suspect == 0);
    check("the words little-endian", b.last.raw[KBD_BATTERY_VOLTAGE] == 4012 &&
                                        b.last.raw[KBD_BATTERY_DESIGN] == 6000);
    kbd_battery_format(&b.last, line, sizeof(line));
    check("the line carries SOC, voltage and signed current",
          strstr(line, "soc=87%") && strstr(line, "voltage=4012mV") &&
              strstr(line, "current=-312mA"));
    check("and both capacities", strstr(line, "fcc=5800mAh") && strstr(line, "design=6000mAh"));
    check("and the flags by name", strstr(line, "[DSG,BATTPRES]") &&
                                      strstr(line, "op=0x0026[sealed,INITCOMP]"));
    check("and the charger decoded",
          strstr(line, "reg0b=0x02 vbus=no-input chrg=not-charging pg=0 vsys_min=0") != NULL);
    check("and no suspect mark", strstr(line, "suspect") == NULL && strchr(line, '?') == NULL);

    /* ---- 3. pacing ------------------------------------------------------ *
     * SLUSCB7A §7.3.1.3: no more than two standard commands a second. Called
     * every 100 ms - ten times the watch's rate - the probe still keeps its
     * gap, and a sample a minute is all it takes. */

    model_reset();
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 300 * SEC, SEC / 10);
    check("a sample a minute: five in five minutes", done == 5);
    check("eight reads each", m.reads == 5u * KBD_BATTERY_NREGS);
    {
        bool gap_ok = true;
        bool two_per_second = true;

        for (i = 1; i < m.reads && i < 256; i++) {
            if (m.read_at[i] - m.read_at[i - 1] < KBD_BATTERY_STEP_GAP_US) {
                gap_ok = false;
            }
            if (i >= 2 && m.read_at[i] - m.read_at[i - 2] < SEC) {
                two_per_second = false;
            }
        }
        check("never two reads within the step gap", gap_ok);
        check("never three reads within one second", two_per_second);
    }
    check("samples start a period apart", m.read_at[KBD_BATTERY_NREGS] == 60 * SEC &&
                                             m.read_at[2 * KBD_BATTERY_NREGS] == 120 * SEC);
    check("still nothing written", m.writes == 0);

    /* ---- 4. a gauge that does not answer -------------------------------- */

    model_reset();
    m.gauge = false;
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 10 * SEC, SEC);
    check("a sample still completes without the gauge", done == 1);
    check("the gauge asked once, not seven times", m.reads == 2 &&
                                                       m.read_addr[0] == KBD_BATTERY_GAUGE_ADDR &&
                                                       m.read_addr[1] == KBD_BATTERY_CHARGER_ADDR);
    check("the charger still read", kbd_battery_charger_answered(&b.last) &&
                                       !kbd_battery_gauge_answered(&b.last));
    kbd_battery_format(&b.last, line, sizeof(line));
    check("and the line says so", strstr(line, "gauge 0x55: no answer") &&
                                     strstr(line, "charger 0x6b: reg0b=0x02") != NULL);
    check("the period is not stretched while something answers",
          b.period_us == KBD_BATTERY_PERIOD_US);

    /* ---- 5. nothing answers: bounded and backing off --------------------- */

    model_reset();
    m.gauge = false;
    m.charger = false;
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 3600 * SEC, SEC);
    check("two reads per empty sample", m.reads == 2u * done);
    /* Each empty sample doubles the period before the next is due: samples
     * start at 0, 120, 360, 840, then every 600 s - eight in the hour. */
    check("the period doubles to its ceiling", b.period_us == KBD_BATTERY_PERIOD_MAX_US);
    check("eight samples in an hour, not sixty", done == 8);
    kbd_battery_format(&b.last, line, sizeof(line));
    check("an empty sample reads as two absences",
          strcmp(line, "gauge 0x55: no answer; charger 0x6b: no answer") == 0);
    check("every claim released", m.claims == m.releases && m.depth == 0);
    m.gauge = true;
    m.charger = true;
    done = run(&b, 3600 * SEC, 4800 * SEC, SEC);
    check("devices that come back are read again", done >= 1 &&
                                                      kbd_battery_gauge_answered(&b.last));
    check("and the period returns to a minute", b.period_us == KBD_BATTERY_PERIOD_US);

    /* ---- 6. suspect readings are marked, not dropped -------------------- */

    model_reset();
    m.word[BQ27220_CMD_STATE_OF_CHARGE] = 0x00FF;
    m.word[BQ27220_CMD_OPERATION_STATUS] = 0xFFFF;
    m.reg0b = 0x00;
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    run(&b, 0, 20 * SEC, SEC);
    check("an impossible SOC is suspect", (b.last.suspect & (1u << KBD_BATTERY_SOC)) != 0);
    check("so is a reserved bit", (b.last.suspect & (1u << KBD_BATTERY_OPSTATUS)) != 0 &&
                                     (b.last.suspect & (1u << KBD_BATTERY_CHARGER)) != 0);
    kbd_battery_format(&b.last, line, sizeof(line));
    check("and the line marks them", strstr(line, "soc=255%?") && strstr(line, "suspect=0x") != NULL);

    /* ---- 7. a bus that cannot do it -------------------------------------- */

    model_reset();
    bus = model_bus();
    bus.read_block_at = NULL;
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 120 * SEC, SEC);
    check("no block read, no probe", done == 0 && m.claims == 0 && m.reads == 0);

    model_reset();
    m.claim_fails = true;
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    done = run(&b, 0, 20 * SEC, SEC);
    check("a bus that cannot be claimed is never read", m.reads == 0 && m.outside_claim == 0);
    check("and counts as two absences", done == 1 && b.last.have == 0 && b.failures == 2);

    /* ---- 8. the line is always terminated --------------------------------- */

    model_reset();
    bus = model_bus();
    kbd_battery_init(&b, &bus, 0);
    run(&b, 0, 20 * SEC, SEC);
    memset(line, 'x', sizeof(line));
    kbd_battery_format(&b.last, line, 16);
    check("a short buffer is truncated, not overrun", strlen(line) == 15 && line[16] == 'x');
    kbd_battery_format(NULL, line, sizeof(line));
    check("no sample, an empty line", line[0] == '\0');
    check("no tick without a probe", kbd_battery_tick(NULL, 0) == 0);

    printf("kbd_battery_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
