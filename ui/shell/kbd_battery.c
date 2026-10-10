/*
 * A diagnostic, read-only battery probe on the keyboard base's bus. See
 * kbd_battery.h for what it reads, from which document, and why it is paced
 * the way it is.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_battery.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct reg_def {
    uint8_t addr;
    uint8_t cmd;
    uint8_t len; /* 2 for a gauge word, 1 for the charger's byte */
};

static const struct reg_def regs[KBD_BATTERY_NREGS] = {
    [KBD_BATTERY_SOC] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_STATE_OF_CHARGE, 2},
    [KBD_BATTERY_VOLTAGE] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_VOLTAGE, 2},
    [KBD_BATTERY_CURRENT] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_CURRENT, 2},
    [KBD_BATTERY_STATUS] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_BATTERY_STATUS, 2},
    [KBD_BATTERY_FCC] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_FULL_CHARGE_CAPACITY, 2},
    [KBD_BATTERY_DESIGN] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_DESIGN_CAPACITY, 2},
    [KBD_BATTERY_OPSTATUS] = {KBD_BATTERY_GAUGE_ADDR, BQ27220_CMD_OPERATION_STATUS, 2},
    [KBD_BATTERY_CHARGER] = {KBD_BATTERY_CHARGER_ADDR, BQ25896_REG_STATUS, 1},
};

#define GAUGE_REGS ((1u << KBD_BATTERY_CHARGER) - 1u)
#define CHARGER_REGS (1u << KBD_BATTERY_CHARGER)

void kbd_battery_init(struct kbd_battery *b, const struct kbd_bus *bus, uint64_t now_us)
{
    if (!b) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->bus = bus;
    b->next_us = now_us;
    b->sample_start_us = now_us;
    b->period_us = KBD_BATTERY_PERIOD_US;
}

bool kbd_battery_gauge_answered(const struct kbd_battery_sample *s)
{
    return s && (s->have & GAUGE_REGS) != 0;
}

bool kbd_battery_charger_answered(const struct kbd_battery_sample *s)
{
    return s && (s->have & CHARGER_REGS) != 0;
}

int kbd_battery_check(enum kbd_battery_reg reg, uint16_t raw)
{
    switch (reg) {
    case KBD_BATTERY_SOC:
        return raw <= 100u ? 0 : -1;
    case KBD_BATTERY_VOLTAGE:
        return raw <= 6000u ? 0 : -1;
    case KBD_BATTERY_CURRENT:
        return 0; /* every signed value is a current */
    case KBD_BATTERY_STATUS:
        return (raw & BQ27220_BS_RSVD) ? -1 : 0;
    case KBD_BATTERY_FCC:
    case KBD_BATTERY_DESIGN:
        /* Data flash I2, 0..32767 (SLUUBD4A data memory table). */
        return raw <= 32767u ? 0 : -1;
    case KBD_BATTERY_OPSTATUS:
        return (raw & BQ27220_OS_RSVD) ? -1 : 0;
    case KBD_BATTERY_CHARGER:
        return (raw <= 0xFFu && (raw & BQ25896_ST_RSVD_ONE)) ? 0 : -1;
    default:
        return -1;
    }
}

/* One transaction: the register of b->step. Returns 0 read, -1 not. */
static int read_one(struct kbd_battery *b, uint16_t *out)
{
    const struct kbd_bus *bus = b->bus;
    const struct reg_def *r = &regs[b->step];
    uint8_t buf[2] = {0, 0};
    int rc;

    if (bus->claim(bus->ctx) != 0) {
        return -1;
    }
    rc = bus->read_block_at(bus->ctx, r->addr, r->cmd, buf, r->len);
    bus->release(bus->ctx);
    if (rc != 0) {
        return -1;
    }
    /* Low byte first (SLUSCB7A §7.3.1.1); the charger's byte stands alone. */
    *out = r->len == 2 ? (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8)) : buf[0];
    return 0;
}

static bool skipped(const struct kbd_battery *b, unsigned step)
{
    return regs[step].addr == KBD_BATTERY_GAUGE_ADDR ? b->gauge_skip : b->charger_skip;
}

static int finish_sample(struct kbd_battery *b, uint64_t now_us)
{
    bool any = kbd_battery_gauge_answered(&b->cur) || kbd_battery_charger_answered(&b->cur);

    b->last = b->cur;
    b->samples++;
    if (any) {
        b->period_us = KBD_BATTERY_PERIOD_US;
    } else if (b->period_us < KBD_BATTERY_PERIOD_MAX_US) {
        b->period_us *= 2u;
        if (b->period_us > KBD_BATTERY_PERIOD_MAX_US) {
            b->period_us = KBD_BATTERY_PERIOD_MAX_US;
        }
    }
    b->step = 0;
    b->gauge_skip = false;
    b->charger_skip = false;
    memset(&b->cur, 0, sizeof(b->cur));
    /* The next sample starts a period after this one started, but never
     * sooner than the step gap after this transaction. */
    b->next_us = b->sample_start_us + b->period_us;
    if (b->next_us < now_us + KBD_BATTERY_STEP_GAP_US) {
        b->next_us = now_us + KBD_BATTERY_STEP_GAP_US;
    }
    return 1;
}

int kbd_battery_tick(struct kbd_battery *b, uint64_t now_us)
{
    uint16_t raw;

    if (!b || b->tripped || !b->bus || !b->bus->read_block_at || !b->bus->claim ||
        !b->bus->release || now_us < b->next_us) {
        return 0;
    }
    /* A device that did not answer this sample is not asked again in it:
     * its registers are passed over without a transaction. */
    while (b->step < KBD_BATTERY_NREGS && skipped(b, b->step)) {
        b->step++;
    }
    if (b->step >= KBD_BATTERY_NREGS) {
        return finish_sample(b, now_us);
    }
    if (b->step == 0) {
        b->sample_start_us = now_us;
    }
    b->transactions++;
    b->last_read_us = now_us;
    if (read_one(b, &raw) == 0) {
        b->cur.raw[b->step] = raw;
        b->cur.have |= 1u << b->step;
        if (kbd_battery_check((enum kbd_battery_reg)b->step, raw) != 0) {
            b->cur.suspect |= 1u << b->step;
        }
    } else {
        b->failures++;
        if (regs[b->step].addr == KBD_BATTERY_GAUGE_ADDR) {
            b->gauge_skip = true;
        } else {
            b->charger_skip = true;
        }
    }
    b->step++;
    b->next_us = now_us + KBD_BATTERY_STEP_GAP_US;
    while (b->step < KBD_BATTERY_NREGS && skipped(b, b->step)) {
        b->step++;
    }
    if (b->step >= KBD_BATTERY_NREGS) {
        return finish_sample(b, now_us);
    }
    return 0;
}

bool kbd_battery_keys_failed(struct kbd_battery *b, uint64_t now_us)
{
    if (!b || b->tripped || b->transactions == 0 || now_us < b->last_read_us ||
        now_us - b->last_read_us >= KBD_BATTERY_TRIP_US) {
        return false;
    }
    b->tripped = true;
    return true;
}

int16_t kbd_battery_current_ma(uint16_t raw)
{
    return raw >= 0x8000u ? (int16_t)((int32_t)raw - 0x10000) : (int16_t)raw;
}

unsigned kbd_battery_security(uint16_t opstatus)
{
    return (opstatus & BQ27220_OS_SEC_MASK) >> BQ27220_OS_SEC_SHIFT;
}

const char *kbd_battery_security_name(unsigned sec)
{
    switch (sec) {
    case 3:
        return "sealed";
    case 2:
        return "unsealed";
    case 1:
        return "full-access";
    default:
        return "undefined";
    }
}

unsigned kbd_battery_vbus_stat(uint8_t reg0b)
{
    return (unsigned)(reg0b >> BQ25896_ST_VBUS_SHIFT) & 0x7u;
}

const char *kbd_battery_vbus_name(unsigned vbus)
{
    switch (vbus) {
    case 0:
        return "no-input";
    case 1:
        return "usb-sdp";
    case 2:
        return "adapter";
    case 7:
        return "otg";
    default:
        return "undefined";
    }
}

unsigned kbd_battery_chrg_stat(uint8_t reg0b)
{
    return (unsigned)(reg0b & BQ25896_ST_CHRG_MASK) >> BQ25896_ST_CHRG_SHIFT;
}

const char *kbd_battery_chrg_name(unsigned chrg)
{
    static const char *const names[4] = {"not-charging", "pre-charge", "fast-charging",
                                         "done"};

    return names[chrg & 3u];
}

/* Append to buf without overrunning it. */
static void put(char *buf, size_t len, size_t *at, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void put(char *buf, size_t len, size_t *at, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*at >= len) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *at, len - *at, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    *at += (size_t)n < len - *at ? (size_t)n : len - *at - 1;
}

static void flags(char *buf, size_t len, size_t *at, uint16_t raw,
                  const uint16_t *bits, const char *const *names, size_t n)
{
    size_t i;
    bool first = true;

    for (i = 0; i < n; i++) {
        if (raw & bits[i]) {
            put(buf, len, at, "%s%s", first ? "" : ",", names[i]);
            first = false;
        }
    }
    if (first) {
        put(buf, len, at, "-");
    }
}

static const char *mark(const struct kbd_battery_sample *s, enum kbd_battery_reg r)
{
    return (s->suspect & (1u << r)) ? "?" : "";
}

void kbd_battery_format(const struct kbd_battery_sample *s, char *buf, size_t len)
{
    static const uint16_t bs_bits[] = {BQ27220_BS_DSG,    BQ27220_BS_SYSDWN, BQ27220_BS_TDA,
                                       BQ27220_BS_BATTPRES, BQ27220_BS_AUTH_GD, BQ27220_BS_OCVGD,
                                       BQ27220_BS_TCA,    BQ27220_BS_CHGINH, BQ27220_BS_FC,
                                       BQ27220_BS_OTD,    BQ27220_BS_OTC,    BQ27220_BS_SLEEP,
                                       BQ27220_BS_OCVFAIL, BQ27220_BS_OCVCOMP, BQ27220_BS_FD};
    static const char *const bs_names[] = {"DSG", "SYSDWN", "TDA", "BATTPRES", "AUTH_GD",
                                           "OCVGD", "TCA", "CHGINH", "FC", "OTD", "OTC",
                                           "SLEEP", "OCVFAIL", "OCVCOMP", "FD"};
    static const uint16_t os_bits[] = {BQ27220_OS_CALMD, BQ27220_OS_EDV2, BQ27220_OS_VDQ,
                                       BQ27220_OS_INITCOMP, BQ27220_OS_SMTH, BQ27220_OS_BTPINT,
                                       BQ27220_OS_CFGUPDATE};
    static const char *const os_names[] = {"CALMD", "EDV2", "VDQ", "INITCOMP", "SMTH",
                                           "BTPINT", "CFGUPDATE"};
    size_t at = 0;

    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!s) {
        return;
    }
    if (!kbd_battery_gauge_answered(s)) {
        put(buf, len, &at, "gauge 0x%02x: no answer", KBD_BATTERY_GAUGE_ADDR);
    } else {
        put(buf, len, &at, "gauge 0x%02x:", KBD_BATTERY_GAUGE_ADDR);
        if (s->have & (1u << KBD_BATTERY_SOC)) {
            put(buf, len, &at, " soc=%u%%%s", s->raw[KBD_BATTERY_SOC], mark(s, KBD_BATTERY_SOC));
        }
        if (s->have & (1u << KBD_BATTERY_VOLTAGE)) {
            put(buf, len, &at, " voltage=%umV%s", s->raw[KBD_BATTERY_VOLTAGE],
                mark(s, KBD_BATTERY_VOLTAGE));
        }
        if (s->have & (1u << KBD_BATTERY_CURRENT)) {
            put(buf, len, &at, " current=%dmA", (int)kbd_battery_current_ma(s->raw[KBD_BATTERY_CURRENT]));
        }
        if (s->have & (1u << KBD_BATTERY_FCC)) {
            put(buf, len, &at, " fcc=%umAh%s", s->raw[KBD_BATTERY_FCC], mark(s, KBD_BATTERY_FCC));
        }
        if (s->have & (1u << KBD_BATTERY_DESIGN)) {
            put(buf, len, &at, " design=%umAh%s", s->raw[KBD_BATTERY_DESIGN],
                mark(s, KBD_BATTERY_DESIGN));
        }
        if (s->have & (1u << KBD_BATTERY_STATUS)) {
            put(buf, len, &at, " status=0x%04x%s[", s->raw[KBD_BATTERY_STATUS],
                mark(s, KBD_BATTERY_STATUS));
            flags(buf, len, &at, s->raw[KBD_BATTERY_STATUS], bs_bits, bs_names,
                  sizeof(bs_bits) / sizeof(bs_bits[0]));
            put(buf, len, &at, "]");
        }
        if (s->have & (1u << KBD_BATTERY_OPSTATUS)) {
            uint16_t os = s->raw[KBD_BATTERY_OPSTATUS];

            put(buf, len, &at, " op=0x%04x%s[%s,", os, mark(s, KBD_BATTERY_OPSTATUS),
                kbd_battery_security_name(kbd_battery_security(os)));
            flags(buf, len, &at, os, os_bits, os_names, sizeof(os_bits) / sizeof(os_bits[0]));
            put(buf, len, &at, "]");
        }
    }
    if (!kbd_battery_charger_answered(s)) {
        put(buf, len, &at, "; charger 0x%02x: no answer", KBD_BATTERY_CHARGER_ADDR);
    } else {
        uint8_t r = (uint8_t)s->raw[KBD_BATTERY_CHARGER];

        put(buf, len, &at, "; charger 0x%02x: reg0b=0x%02x%s vbus=%s chrg=%s pg=%u vsys_min=%u",
            KBD_BATTERY_CHARGER_ADDR, r, mark(s, KBD_BATTERY_CHARGER),
            kbd_battery_vbus_name(kbd_battery_vbus_stat(r)),
            kbd_battery_chrg_name(kbd_battery_chrg_stat(r)), (r & BQ25896_ST_PG) ? 1u : 0u,
            (r & BQ25896_ST_VSYS) ? 1u : 0u);
    }
    if (s->suspect) {
        put(buf, len, &at, "; suspect=0x%02x", s->suspect);
    }
}
