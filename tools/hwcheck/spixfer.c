/* pos-spixfer: one full-duplex SPI transfer through spidev, for pos-hwcheck.
 *
 * Performs exactly what radiod's Linux HAL does for a RadioLib SPI stream
 * (services/radiod/hal_linux.cpp, PocketRadioHal::spiBegin/spiTransfer):
 * the node is opened O_RDWR and locked with flock(LOCK_EX | LOCK_NB), mode 0,
 * 8 bits per word and the clock are written to the device, and all the bytes
 * go out in ONE SPI_IOC_MESSAGE(1) transaction with speed_hz and
 * bits_per_word set on the transfer itself, so chip select frames the whole
 * command. The clock defaults to radiod's 4 MHz. MISO is written raw to
 * stdout, one byte per byte sent, nothing else.
 *
 * Usage: pos-spixfer [-s <hz>] <spidev> <hex byte>...
 *        Exit 0 on success; 1 when the device cannot be opened, locked,
 *        configured or transferred (message on stderr); 2 on a usage error.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SPIXFER_MAX_BYTES 64
#define SPIXFER_DEFAULT_HZ 4000000u  /* radiod: new PocketRadioHal(spi_path, 4000000) */

static int usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-s <hz>] <spidev> <hex byte>...\n", argv0);
    return 2;
}

static int parse_hex_byte(const char *s, uint8_t *out)
{
    char *end;
    long v;

    if (s[0] == '\0' || strlen(s) > 2) {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 16);
    if (errno != 0 || *end != '\0' || v < 0 || v > 255) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev;
    uint32_t speed = SPIXFER_DEFAULT_HZ;
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint8_t tx[SPIXFER_MAX_BYTES];
    uint8_t rx[SPIXFER_MAX_BYTES];
    struct spi_ioc_transfer tr;
    size_t len = 0;
    int argi = 1;
    int fd;

    if (argi + 1 < argc && strcmp(argv[argi], "-s") == 0) {
        char *end;
        unsigned long hz = strtoul(argv[argi + 1], &end, 10);
        if (*argv[argi + 1] == '\0' || *end != '\0' || hz == 0 || hz > 0xffffffffUL) {
            return usage(argv[0]);
        }
        speed = (uint32_t)hz;
        argi += 2;
    }
    if (argi + 1 >= argc) {
        return usage(argv[0]);
    }
    dev = argv[argi++];
    for (; argi < argc; argi++) {
        if (len >= SPIXFER_MAX_BYTES || parse_hex_byte(argv[argi], &tx[len]) < 0) {
            return usage(argv[0]);
        }
        len++;
    }

    fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "pos-spixfer: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    /* The same advisory lock radiod takes (review finding F14, bench defect
     * B3): a radiod holding the radio makes this transfer fail rather than
     * interleave with its own. */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "pos-spixfer: %s: already in use by another process (%s)\n", dev, strerror(errno));
        close(fd);
        return 1;
    }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        fprintf(stderr, "pos-spixfer: %s: configure failed: %s\n", dev, strerror(errno));
        close(fd);
        return 1;
    }
    memset(rx, 0, sizeof(rx));
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)(uintptr_t)tx;
    tr.rx_buf = (unsigned long)(uintptr_t)rx;
    tr.len = (uint32_t)len;
    tr.speed_hz = speed;
    tr.bits_per_word = 8;
    if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        fprintf(stderr, "pos-spixfer: %s: spi transfer failed: %s\n", dev, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    if (fwrite(rx, 1, len, stdout) != len || fflush(stdout) != 0) {
        fprintf(stderr, "pos-spixfer: stdout: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
