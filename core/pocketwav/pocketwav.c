/*
 * pocketwav: the WAV container. See pocketwav.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketwav.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FORMAT_PCM 1u
#define FORMAT_EXTENSIBLE 0xFFFEu

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void pocketwav_header(uint8_t out[POCKETWAV_HEADER_BYTES], unsigned rate, unsigned channels,
                      uint32_t data_bytes)
{
    unsigned block = channels * 2u;

    if (data_bytes > POCKETWAV_MAX_DATA_BYTES) {
        data_bytes = POCKETWAV_MAX_DATA_BYTES;
    }
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16u);
    put16(out + 20, FORMAT_PCM);
    put16(out + 22, channels);
    put32(out + 24, rate);
    put32(out + 28, rate * block);
    put16(out + 32, block);
    put16(out + 34, 16u);
    memcpy(out + 36, "data", 4);
    put32(out + 40, data_bytes);
}

static int read_all_at(int fd, uint8_t *buf, size_t n, off_t off, size_t *got)
{
    *got = 0;
    while (*got < n) {
        ssize_t r = pread(fd, buf + *got, n - *got, off + (off_t)*got);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            break;
        }
        *got += (size_t)r;
    }
    return 0;
}

int pocketwav_probe_fd(int fd, struct pocketwav_info *info)
{
    uint8_t buf[POCKETWAV_PROBE_BYTES];
    struct stat st;
    size_t got;
    size_t pos = 12;
    unsigned format = 0;
    unsigned bits = 0;
    int have_fmt = 0;
    uint64_t raw;

    memset(info, 0, sizeof(*info));
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        read_all_at(fd, buf, sizeof(buf), 0, &got) != 0) {
        return POCKETWAV_E_IO;
    }
    info->file_bytes = (uint64_t)st.st_size;
    if (got < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return POCKETWAV_E_INVALID;
    }
    for (;;) {
        uint32_t size;

        if (pos + 8 > got) {
            return POCKETWAV_E_INVALID; /* no data chunk where one must be */
        }
        size = le32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            const uint8_t *f = buf + pos + 8;

            if (size < 16 || size > got - pos - 8) {
                return POCKETWAV_E_INVALID;
            }
            format = le16(f);
            info->channels = le16(f + 2);
            info->rate = le32(f + 4);
            info->block = le16(f + 12);
            bits = le16(f + 14);
            if (format == FORMAT_EXTENSIBLE && size >= 26) {
                format = le16(f + 24); /* the sub-format GUID starts with the code */
            }
            have_fmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            info->data_offset = pos + 8;
            info->data_declared = size;
            break;
        }
        /* Chunks are word-aligned: an odd size is followed by a pad byte. */
        if ((uint64_t)pos + 8u + size + (size & 1u) > got) {
            return POCKETWAV_E_INVALID;
        }
        pos += 8u + size + (size & 1u);
    }
    if (!have_fmt) {
        return POCKETWAV_E_INVALID;
    }
    if (format != FORMAT_PCM || bits != 16 || info->channels < 1 || info->channels > 2 ||
        info->block != info->channels * 2u) {
        return POCKETWAV_E_UNSUPPORTED;
    }
    if (info->rate < POCKETWAV_MIN_RATE || info->rate > POCKETWAV_MAX_RATE) {
        return POCKETWAV_E_INVALID;
    }
    raw = info->file_bytes > info->data_offset ? info->file_bytes - info->data_offset : 0;
    raw -= raw % info->block;
    info->data_present = raw < info->data_declared ? raw : info->data_declared;
    info->data_present -= info->data_present % info->block;
    info->truncated = raw < info->data_declared;
    info->trailing = raw > info->data_declared;
    info->frames = info->data_present / info->block;
    return POCKETWAV_OK;
}

static int write_all_at(int fd, const uint8_t *buf, size_t n, off_t off)
{
    size_t done = 0;

    while (done < n) {
        ssize_t w = pwrite(fd, buf + done, n - done, off + (off_t)done);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int pocketwav_fix_fd(int fd, struct pocketwav_info *info)
{
    uint8_t v[4];
    uint64_t whole;
    uint64_t max;
    int rc = pocketwav_probe_fd(fd, info);

    if (rc != POCKETWAV_OK) {
        return rc;
    }
    whole = info->file_bytes - info->data_offset;
    if (info->file_bytes < info->data_offset) {
        whole = 0;
    }
    whole -= whole % info->block;
    max = POCKETWAV_MAX_DATA_BYTES - POCKETWAV_MAX_DATA_BYTES % info->block;
    if (whole > max) {
        whole = max;
    }
    if (info->file_bytes != info->data_offset + whole &&
        ftruncate(fd, (off_t)(info->data_offset + whole)) != 0) {
        return POCKETWAV_E_IO;
    }
    put32(v, (uint32_t)(info->data_offset - 8u + whole));
    if (write_all_at(fd, v, 4, 4) != 0) {
        return POCKETWAV_E_IO;
    }
    put32(v, (uint32_t)whole);
    if (write_all_at(fd, v, 4, (off_t)(info->data_offset - 4u)) != 0) {
        return POCKETWAV_E_IO;
    }
    return pocketwav_probe_fd(fd, info);
}

uint64_t pocketwav_frames_ms(uint64_t frames, unsigned rate)
{
    return rate ? frames * 1000u / rate : 0;
}

const char *pocketwav_strerror(int result)
{
    switch (result) {
    case POCKETWAV_OK: return "ok";
    case POCKETWAV_E_IO: return "cannot read the file";
    case POCKETWAV_E_INVALID: return "not a valid WAV file";
    case POCKETWAV_E_UNSUPPORTED: return "WAV format not supported (needs 16-bit PCM, 1 or 2 channels)";
    default: return "unknown WAV error";
    }
}
