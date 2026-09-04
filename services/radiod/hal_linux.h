/*
 * PocketOS RadioLib HAL for Linux: spidev for SPI, libgpiod v2 for GPIO.
 * Written from the libgpiod v2 and spidev documentation; no vendor code.
 *
 * Pin numbers are SoC GPIO numbers. On the K230 they map to
 * gpiochip<pin/32> line <pin%32> (DOCUMENTED: BSP pinmap and DTS use the
 * same numbering); the chip name pattern is configurable for other boards.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RADIOD_HAL_LINUX_H
#define RADIOD_HAL_LINUX_H

#include "RadioLib.h"

#include <cstdint>

struct gpiod_chip;
struct gpiod_line_request;

class PocketRadioHal : public RadioLibHal {
public:
    static const uint32_t MAX_PINS = 64;
    static const uint32_t LINES_PER_CHIP = 32;

    PocketRadioHal(const char *spi_path, uint32_t spi_speed_hz);
    ~PocketRadioHal() override;

    void init() override;
    void term() override;
    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override;
    void spiEndTransaction() override;
    void spiEnd() override;
    void yield() override;

    /* File descriptor that becomes readable on the interrupt pin's edge, or -1. */
    int irqFd() const;
    /* Consume pending edge events; calls the attached callback once if any arrived. */
    void serviceIrq();
    const char *lastError() const;
    bool spiReady() const;

private:
    struct Line {
        gpiod_line_request *request;
        bool output;
        bool edge;
    };

    bool requestLine(uint32_t pin, bool output, bool initial_high, bool edge_rising, bool edge_falling);
    void releaseLine(uint32_t pin);
    gpiod_chip *openChip(uint32_t index);
    void setError(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    const char *spi_path_;
    uint32_t spi_speed_;
    int spi_fd_;
    gpiod_chip *chips_[MAX_PINS / LINES_PER_CHIP];
    Line lines_[MAX_PINS];
    uint32_t irq_pin_;
    void (*irq_cb_)(void);
    char error_[160];
};

#endif
