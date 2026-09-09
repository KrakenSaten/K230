/*
 * PocketOS RadioLib HAL for Linux. See hal_linux.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "hal_linux.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* RadioLib expects the HAL to define these constants for pinMode and friends. */
enum {
    HAL_INPUT = 0,
    HAL_OUTPUT = 1,
    HAL_LOW = 0,
    HAL_HIGH = 1,
    HAL_RISING = 1,
    HAL_FALLING = 2,
};

PocketRadioHal::PocketRadioHal(const char *spi_path, uint32_t spi_speed_hz)
    : RadioLibHal(HAL_INPUT, HAL_OUTPUT, HAL_LOW, HAL_HIGH, HAL_RISING, HAL_FALLING),
      spi_path_(spi_path), spi_speed_(spi_speed_hz), spi_fd_(-1),
      irq_pin_(RADIOLIB_NC), irq_cb_(nullptr)
{
    memset(chips_, 0, sizeof(chips_));
    memset(lines_, 0, sizeof(lines_));
    error_[0] = '\0';
}

PocketRadioHal::~PocketRadioHal()
{
    term();
}

void PocketRadioHal::setError(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(error_, sizeof(error_), fmt, ap);
    va_end(ap);
}

const char *PocketRadioHal::lastError() const
{
    return error_;
}

bool PocketRadioHal::spiReady() const
{
    return spi_fd_ >= 0;
}

void PocketRadioHal::init()
{
    spiBegin();
}

void PocketRadioHal::term()
{
    spiEnd();
    for (uint32_t pin = 0; pin < MAX_PINS; pin++) {
        releaseLine(pin);
    }
    for (size_t i = 0; i < sizeof(chips_) / sizeof(chips_[0]); i++) {
        if (chips_[i]) {
            gpiod_chip_close(chips_[i]);
            chips_[i] = nullptr;
        }
    }
}

/* ---- GPIO ------------------------------------------------------------- */

gpiod_chip *PocketRadioHal::openChip(uint32_t index)
{
    char path[32];

    if (index >= sizeof(chips_) / sizeof(chips_[0])) {
        return nullptr;
    }
    if (!chips_[index]) {
        snprintf(path, sizeof(path), "/dev/gpiochip%u", index);
        chips_[index] = gpiod_chip_open(path);
        if (!chips_[index]) {
            setError("%s: %s", path, strerror(errno));
        }
    }
    return chips_[index];
}

void PocketRadioHal::releaseLine(uint32_t pin)
{
    if (pin < MAX_PINS && lines_[pin].request) {
        gpiod_line_request_release(lines_[pin].request);
        lines_[pin].request = nullptr;
        lines_[pin].output = false;
        lines_[pin].edge = false;
    }
}

bool PocketRadioHal::requestLine(uint32_t pin, bool output, bool initial_high,
                                 bool edge_rising, bool edge_falling)
{
    gpiod_chip *chip;
    gpiod_line_settings *settings;
    gpiod_line_config *line_cfg;
    gpiod_request_config *req_cfg;
    gpiod_line_request *req;
    unsigned int offset = pin % LINES_PER_CHIP;

    if (pin >= MAX_PINS) {
        setError("gpio %u out of range", pin);
        return false;
    }
    releaseLine(pin);
    chip = openChip(pin / LINES_PER_CHIP);
    if (!chip) {
        return false;
    }
    settings = gpiod_line_settings_new();
    line_cfg = gpiod_line_config_new();
    req_cfg = gpiod_request_config_new();
    if (!settings || !line_cfg || !req_cfg) {
        setError("gpio %u: allocation failed", pin);
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);
        return false;
    }
    gpiod_line_settings_set_direction(settings, output ? GPIOD_LINE_DIRECTION_OUTPUT
                                                        : GPIOD_LINE_DIRECTION_INPUT);
    if (output) {
        gpiod_line_settings_set_output_value(settings, initial_high ? GPIOD_LINE_VALUE_ACTIVE
                                                                    : GPIOD_LINE_VALUE_INACTIVE);
    } else if (edge_rising && edge_falling) {
        gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
    } else if (edge_rising) {
        gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);
    } else if (edge_falling) {
        gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);
    }
    gpiod_request_config_set_consumer(req_cfg, "radiod");
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) != 0) {
        setError("gpio %u: line config failed: %s", pin, strerror(errno));
        req = nullptr;
    } else {
        req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
        if (!req) {
            setError("gpio %u: request failed: %s", pin, strerror(errno));
        }
    }
    gpiod_line_settings_free(settings);
    gpiod_line_config_free(line_cfg);
    gpiod_request_config_free(req_cfg);
    if (!req) {
        return false;
    }
    lines_[pin].request = req;
    lines_[pin].output = output;
    lines_[pin].edge = edge_rising || edge_falling;
    return true;
}

void PocketRadioHal::pinMode(uint32_t pin, uint32_t mode)
{
    if (pin == RADIOLIB_NC) {
        return;
    }
    if (pin < MAX_PINS && lines_[pin].request && lines_[pin].output == (mode == HAL_OUTPUT) &&
        !lines_[pin].edge) {
        return;
    }
    requestLine(pin, mode == HAL_OUTPUT, false, false, false);
}

void PocketRadioHal::digitalWrite(uint32_t pin, uint32_t value)
{
    if (pin == RADIOLIB_NC || pin >= MAX_PINS) {
        return;
    }
    if (!lines_[pin].request || !lines_[pin].output) {
        if (!requestLine(pin, true, value == HAL_HIGH, false, false)) {
            return;
        }
    }
    if (gpiod_line_request_set_value(lines_[pin].request, pin % LINES_PER_CHIP,
                                     value == HAL_HIGH ? GPIOD_LINE_VALUE_ACTIVE
                                                       : GPIOD_LINE_VALUE_INACTIVE) < 0) {
        setError("gpio %u: write failed: %s", pin, strerror(errno));
    }
}

uint32_t PocketRadioHal::digitalRead(uint32_t pin)
{
    if (pin == RADIOLIB_NC || pin >= MAX_PINS) {
        return HAL_LOW;
    }
    if (!lines_[pin].request) {
        if (!requestLine(pin, false, false, false, false)) {
            return HAL_LOW;
        }
    }
    return gpiod_line_request_get_value(lines_[pin].request, pin % LINES_PER_CHIP) ==
                   GPIOD_LINE_VALUE_ACTIVE
               ? HAL_HIGH
               : HAL_LOW;
}

void PocketRadioHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode)
{
    if (interruptNum == RADIOLIB_NC) {
        return;
    }
    if (requestLine(interruptNum, false, false, mode != HAL_FALLING, mode == HAL_FALLING)) {
        irq_pin_ = interruptNum;
        irq_cb_ = interruptCb;
    }
}

void PocketRadioHal::detachInterrupt(uint32_t interruptNum)
{
    if (interruptNum == irq_pin_) {
        irq_cb_ = nullptr;
        irq_pin_ = RADIOLIB_NC;
        requestLine(interruptNum, false, false, false, false);
    }
}

int PocketRadioHal::irqFd() const
{
    if (irq_pin_ == RADIOLIB_NC || !lines_[irq_pin_].request) {
        return -1;
    }
    return gpiod_line_request_get_fd(lines_[irq_pin_].request);
}

void PocketRadioHal::serviceIrq()
{
    gpiod_edge_event_buffer *buf;
    int fd = irqFd();
    int n = 0;

    if (fd < 0) {
        return;
    }
    buf = gpiod_edge_event_buffer_new(8);
    if (!buf) {
        return;
    }
    /* The request fd is non-blocking only if we ask; use a zero-timeout wait. */
    if (gpiod_line_request_wait_edge_events(lines_[irq_pin_].request, 0) == 1) {
        n = gpiod_line_request_read_edge_events(lines_[irq_pin_].request, buf, 8);
    }
    gpiod_edge_event_buffer_free(buf);
    if (n > 0 && irq_cb_) {
        irq_cb_();
    }
}

/* ---- timing ----------------------------------------------------------- */

void PocketRadioHal::delay(RadioLibTime_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

void PocketRadioHal::delayMicroseconds(RadioLibTime_t us)
{
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000L };

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

RadioLibTime_t PocketRadioHal::millis()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (RadioLibTime_t)ts.tv_sec * 1000 + (RadioLibTime_t)ts.tv_nsec / 1000000;
}

RadioLibTime_t PocketRadioHal::micros()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (RadioLibTime_t)ts.tv_sec * 1000000 + (RadioLibTime_t)ts.tv_nsec / 1000;
}

long PocketRadioHal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout)
{
    (void)pin;
    (void)state;
    (void)timeout;
    return 0; /* not needed by SX126x */
}

void PocketRadioHal::yield()
{
    struct timespec ts = { 0, 100000L };

    nanosleep(&ts, nullptr);
}

/* ---- SPI -------------------------------------------------------------- */

void PocketRadioHal::spiBegin()
{
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;

    if (spi_fd_ >= 0) {
        return;
    }
    spi_fd_ = open(spi_path_, O_RDWR | O_CLOEXEC);
    if (spi_fd_ < 0) {
        setError("%s: %s", spi_path_, strerror(errno));
        return;
    }
    /* The GPIO lines are exclusive because libgpiod requests them from the
     * kernel, and the bench confirmed radiod holding all four. spidev has no
     * such thing: two processes can open the same node and interleave
     * transfers, and file permissions cannot help while everything runs as
     * root. An advisory whole-file lock closes that, and is released by the
     * kernel when the fd goes, including on a crash. It is what makes
     * "pos-hwcheck --lora while radiod owns the radio" impossible rather than
     * merely discouraged (review finding F14, bench defect B3). */
    if (flock(spi_fd_, LOCK_EX | LOCK_NB) < 0) {
        setError("%s: already in use by another process (%s)", spi_path_, strerror(errno));
        close(spi_fd_);
        spi_fd_ = -1;
        return;
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed_) < 0) {
        setError("%s: configure failed: %s", spi_path_, strerror(errno));
        close(spi_fd_);
        spi_fd_ = -1;
    }
}

void PocketRadioHal::spiBeginTransaction()
{
}

void PocketRadioHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    struct spi_ioc_transfer tr;

    if (len == 0 || spi_fd_ < 0) {
        if (in) {
            memset(in, 0, len);
        }
        return;
    }
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)(uintptr_t)out;
    tr.rx_buf = (unsigned long)(uintptr_t)in;
    tr.len = (uint32_t)len;
    tr.speed_hz = spi_speed_;
    tr.bits_per_word = 8;
    if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        setError("spi transfer failed: %s", strerror(errno));
        if (in) {
            memset(in, 0, len);
        }
    }
}

void PocketRadioHal::spiEndTransaction()
{
}

void PocketRadioHal::spiEnd()
{
    if (spi_fd_ >= 0) {
        close(spi_fd_);
        spi_fd_ = -1;
    }
}
