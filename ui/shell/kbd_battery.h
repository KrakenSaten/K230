/*
 * A diagnostic, read-only battery probe on the keyboard base's bus.
 *
 * The keyboard base carries a BQ27220 fuel gauge at 0x55 and a BQ25896
 * charger at 0x6B on the same bit-banged GPIO46/47 bus as the TCA8418
 * (vendor pin map, DOCUMENTED). The shell owns that bus (KEYBOARD_DRIVER_
 * DESIGN_2026-09-12.md §0.2), so the probe lives here, on the same thread,
 * behind the same claim and release, and nothing else touches the lines.
 *
 * It only reads. Every transaction is a register-address write followed by
 * a read: no configuration, no subcommand, no charger ADC conversion, no
 * gauge reset, unseal or calibration. What it reads, from TI's own
 * documents:
 *
 *   BQ27220 (SLUUBD4A Technical Reference Manual, SLUSCB7A datasheet)
 *     0x2C StateOfCharge()      unsigned, %, 0..100 of FullChargeCapacity()
 *     0x08 Voltage()            unsigned, mV, 0..6000
 *     0x0C Current()            signed, mA, through the sense resistor
 *     0x0A BatteryStatus()      flags (Table 2-6)
 *     0x12 FullChargeCapacity() unsigned, mAh
 *     0x3C DesignCapacity()     unsigned, mAh (data flash "Design Capacity
 *                               mAh", TI default 3000)
 *     0x3A OperationStatus()    flags (Table 2-7)
 *   BQ25896 (SLUSC76C datasheet)
 *     0x0B REG0B                read-only status: VBUS_STAT, CHRG_STAT,
 *                               PG_STAT, bit 1 reserved (always reads 1),
 *                               VSYS_STAT
 *
 * Gauge words are read as one incremental two-byte read, low byte first
 * (SLUSCB7A §7.3.1.1 (d)), so the two halves of a word come from one packet.
 *
 * Two limits from SLUSCB7A §7.3.1.3 and §7.3.1.4 shape the pacing:
 *
 *   - "the host must not issue any standard command more than two times per
 *     second", or the gauge's watchdog may reset it. The probe performs at
 *     most one transaction per call and never two within
 *     KBD_BATTERY_STEP_GAP_US, whatever rate it is called at.
 *   - the gauge may stretch the clock (about 100 us waking from SLEEP, up to
 *     4 ms otherwise). This bus does not read SCL back, exactly like the
 *     vendor launcher's, so a stretched read can come back wrong rather than
 *     fail. Decoding therefore checks ranges and reserved bits and marks a
 *     sample suspect; nothing here can prove a value right.
 *
 * Pure: it talks to struct kbd_bus only (read_block_at), so all of it runs on
 * the host against a register model (tests/kbd_battery_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_KBD_BATTERY_H
#define POCKETOS_KBD_BATTERY_H

#include "kbd_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KBD_BATTERY_GAUGE_ADDR 0x55u
#define KBD_BATTERY_CHARGER_ADDR 0x6Bu

/* BQ27220 standard commands (SLUUBD4A §2). */
#define BQ27220_CMD_VOLTAGE 0x08u
#define BQ27220_CMD_BATTERY_STATUS 0x0Au
#define BQ27220_CMD_CURRENT 0x0Cu
#define BQ27220_CMD_FULL_CHARGE_CAPACITY 0x12u
#define BQ27220_CMD_STATE_OF_CHARGE 0x2Cu
#define BQ27220_CMD_OPERATION_STATUS 0x3Au
#define BQ27220_CMD_DESIGN_CAPACITY 0x3Cu

/* BatteryStatus() bits, the word as read (low byte first). Table 2-6. */
#define BQ27220_BS_DSG 0x0001u      /* discharging when set */
#define BQ27220_BS_SYSDWN 0x0002u
#define BQ27220_BS_TDA 0x0004u
#define BQ27220_BS_BATTPRES 0x0008u
#define BQ27220_BS_AUTH_GD 0x0010u
#define BQ27220_BS_OCVGD 0x0020u
#define BQ27220_BS_TCA 0x0040u
#define BQ27220_BS_RSVD 0x0080u
#define BQ27220_BS_CHGINH 0x0100u
#define BQ27220_BS_FC 0x0200u
#define BQ27220_BS_OTD 0x0400u
#define BQ27220_BS_OTC 0x0800u
#define BQ27220_BS_SLEEP 0x1000u
#define BQ27220_BS_OCVFAIL 0x2000u
#define BQ27220_BS_OCVCOMP 0x4000u
#define BQ27220_BS_FD 0x8000u

/* OperationStatus() bits. Table 2-7: the high byte is reserved apart from
 * CFGUPDATE. */
#define BQ27220_OS_CALMD 0x0001u
#define BQ27220_OS_SEC_SHIFT 1u
#define BQ27220_OS_SEC_MASK 0x0006u
#define BQ27220_OS_EDV2 0x0008u
#define BQ27220_OS_VDQ 0x0010u
#define BQ27220_OS_INITCOMP 0x0020u
#define BQ27220_OS_SMTH 0x0040u
#define BQ27220_OS_BTPINT 0x0080u
#define BQ27220_OS_CFGUPDATE 0x0400u
#define BQ27220_OS_RSVD 0xFB00u

/* BQ25896 REG0B (SLUSC76C Table 17). */
#define BQ25896_REG_STATUS 0x0Bu
#define BQ25896_ST_VBUS_SHIFT 5u
#define BQ25896_ST_CHRG_SHIFT 3u
#define BQ25896_ST_CHRG_MASK 0x18u
#define BQ25896_ST_PG 0x04u
#define BQ25896_ST_RSVD_ONE 0x02u /* reserved, always reads 1 */
#define BQ25896_ST_VSYS 0x01u

/* Pacing. One transaction per step; a sample is every register once. */
#define KBD_BATTERY_STEP_GAP_US 600000ULL        /* <= 2 commands per second */
#define KBD_BATTERY_PERIOD_US 60000000ULL        /* a sample a minute */
#define KBD_BATTERY_PERIOD_MAX_US 600000000ULL   /* nothing answers: back off */

/* The registers of one sample, in the order they are read. */
enum kbd_battery_reg {
    KBD_BATTERY_SOC,
    KBD_BATTERY_VOLTAGE,
    KBD_BATTERY_CURRENT,
    KBD_BATTERY_STATUS,
    KBD_BATTERY_FCC,
    KBD_BATTERY_DESIGN,
    KBD_BATTERY_OPSTATUS,
    KBD_BATTERY_CHARGER,
    KBD_BATTERY_NREGS
};

/* One sample: the raw words and which of them were read at all. Decoding is
 * done on demand by the functions below, so the raw value is always there to
 * log next to what it was taken to mean. */
struct kbd_battery_sample {
    uint16_t raw[KBD_BATTERY_NREGS];
    unsigned have;   /* bit per enum kbd_battery_reg: read successfully */
    unsigned suspect; /* bit per register: read, but out of range or with a
                       * reserved bit wrong (see kbd_battery_check) */
};

struct kbd_battery {
    const struct kbd_bus *bus;
    unsigned step;          /* next register of the sample being read */
    uint64_t next_us;       /* nothing before this */
    uint64_t sample_start_us;
    uint64_t period_us;     /* the current sample period, with back-off */
    bool gauge_skip;        /* the gauge did not answer this sample */
    bool charger_skip;
    struct kbd_battery_sample cur;
    struct kbd_battery_sample last; /* the last complete sample */
    unsigned samples;
    unsigned transactions;  /* bus transactions, for tests and the log */
    unsigned failures;      /* transactions that failed */
    uint64_t last_read_us;  /* when the last transaction ran; 0 none yet */
    bool tripped;           /* kbd_battery_keys_failed(): off for good */
};

/* The breaker. The keys come first: when the keyboard controller stops
 * answering within this long after a probe transaction, the probe is taken
 * to have caused it and never touches the bus again (until re-initialised by
 * a new process). The watch period plus the controller's own detection. */
#define KBD_BATTERY_TRIP_US 2000000ULL

/* Start from nothing; the first transaction may run at now_us. */
void kbd_battery_init(struct kbd_battery *b, const struct kbd_bus *bus, uint64_t now_us);

/* At most one bus transaction, and only when one is due. Claims and releases
 * the bus itself. Returns 1 when this call completed a sample (now in
 * b->last), 0 otherwise. A device that does not answer is skipped for the
 * rest of that sample, and a sample in which neither answers doubles the
 * period up to KBD_BATTERY_PERIOD_MAX_US; any answer restores it. */
int kbd_battery_tick(struct kbd_battery *b, uint64_t now_us);

/* The keyboard controller just stopped answering. Returns true when that is
 * within KBD_BATTERY_TRIP_US of a probe transaction, and trips the probe:
 * every later tick does nothing. False, and no change, otherwise. */
bool kbd_battery_keys_failed(struct kbd_battery *b, uint64_t now_us);

/* Whether a device answered in a sample: at least one of its registers. */
bool kbd_battery_gauge_answered(const struct kbd_battery_sample *s);
bool kbd_battery_charger_answered(const struct kbd_battery_sample *s);

/* Range and reserved-bit checks on one raw value: 0 plausible, -1 suspect. */
int kbd_battery_check(enum kbd_battery_reg reg, uint16_t raw);

/* Decoders. */
int16_t kbd_battery_current_ma(uint16_t raw);        /* two's complement */
unsigned kbd_battery_security(uint16_t opstatus);    /* SEC[1:0] */
const char *kbd_battery_security_name(unsigned sec); /* sealed/unsealed/full */
unsigned kbd_battery_vbus_stat(uint8_t reg0b);
const char *kbd_battery_vbus_name(unsigned vbus);
unsigned kbd_battery_chrg_stat(uint8_t reg0b);
const char *kbd_battery_chrg_name(unsigned chrg);

/* One log line for a sample, raw values beside decoded ones. Always
 * terminated; truncated to len. */
void kbd_battery_format(const struct kbd_battery_sample *s, char *buf, size_t len);

#endif
