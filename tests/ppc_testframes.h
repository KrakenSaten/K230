/*
 * Frame builders for the PacketShark tests (tests/ppc_*_test.c,
 * tests/ppc_mkfixtures.c). Header-only and static, so each test links what
 * it uses.
 *
 * Every address built here is a documentation address (192.0.2.0/24,
 * 198.51.100.0/24, 203.0.113.0/24, 2001:db8::/32) or a locally administered
 * MAC (02:...), and every name is under example.net: the fixtures can be
 * committed and published without carrying anyone's network.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_TESTFRAMES_H
#define PPC_TESTFRAMES_H

#include <stdint.h>
#include <string.h>

#define FB_MAX 70000u

struct fb {
    uint8_t b[FB_MAX];
    uint32_t n;
};

static const uint8_t MAC_A[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t MAC_B[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0b };
static const uint8_t MAC_BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t IP_A[4] = { 192, 0, 2, 10 };
static const uint8_t IP_B[4] = { 192, 0, 2, 20 };
static const uint8_t IP_DNS[4] = { 192, 0, 2, 53 };
static const uint8_t IP6_A[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a };
static const uint8_t IP6_B[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0b };

static inline void fb_reset(struct fb *f)
{
    f->n = 0;
}

static inline void fb_put(struct fb *f, const void *p, uint32_t n)
{
    if (f->n + n <= FB_MAX) {
        memcpy(f->b + f->n, p, n);
        f->n += n;
    }
}

static inline void fb_u8(struct fb *f, uint32_t v)
{
    uint8_t c = (uint8_t)v;

    fb_put(f, &c, 1);
}

static inline void fb_u16(struct fb *f, uint32_t v)
{
    fb_u8(f, v >> 8);
    fb_u8(f, v);
}

static inline void fb_u32(struct fb *f, uint32_t v)
{
    fb_u16(f, v >> 16);
    fb_u16(f, v);
}

static inline void fb_zero(struct fb *f, uint32_t n)
{
    while (n--) {
        fb_u8(f, 0);
    }
}

static inline void fb_set16(struct fb *f, uint32_t at, uint32_t v)
{
    f->b[at] = (uint8_t)(v >> 8);
    f->b[at + 1] = (uint8_t)v;
}

static inline void fb_eth(struct fb *f, const uint8_t *dst, const uint8_t *src, uint32_t et)
{
    fb_put(f, dst, 6);
    fb_put(f, src, 6);
    fb_u16(f, et);
}

static inline void fb_vlan(struct fb *f, uint32_t vid, uint32_t inner_et)
{
    fb_u16(f, vid & 0x0fffu);
    fb_u16(f, inner_et);
}

/* An IPv4 header without options; the total length is set by fb_ip4_end. */
static inline uint32_t fb_ip4(struct fb *f, uint32_t proto, const uint8_t *src, const uint8_t *dst,
                              uint32_t frag, uint32_t id)
{
    uint32_t at = f->n;

    fb_u8(f, 0x45);
    fb_u8(f, 0);
    fb_u16(f, 0);
    fb_u16(f, id);
    fb_u16(f, frag);
    fb_u8(f, 64);
    fb_u8(f, proto);
    fb_u16(f, 0);
    fb_put(f, src, 4);
    fb_put(f, dst, 4);
    return at;
}

static inline void fb_ip4_end(struct fb *f, uint32_t at)
{
    fb_set16(f, at + 2, f->n - at);
}

static inline uint32_t fb_ip6(struct fb *f, uint32_t nh, const uint8_t *src, const uint8_t *dst)
{
    uint32_t at = f->n;

    fb_u32(f, 0x60000000u);
    fb_u16(f, 0);
    fb_u8(f, nh);
    fb_u8(f, 64);
    fb_put(f, src, 16);
    fb_put(f, dst, 16);
    return at;
}

static inline void fb_ip6_end(struct fb *f, uint32_t at)
{
    fb_set16(f, at + 4, f->n - at - 40);
}

static inline uint32_t fb_udp(struct fb *f, uint32_t sp, uint32_t dp)
{
    uint32_t at = f->n;

    fb_u16(f, sp);
    fb_u16(f, dp);
    fb_u16(f, 0);
    fb_u16(f, 0);
    return at;
}

static inline void fb_udp_end(struct fb *f, uint32_t at)
{
    fb_set16(f, at + 4, f->n - at);
}

static inline void fb_tcp(struct fb *f, uint32_t sp, uint32_t dp, uint32_t seq, uint32_t ack,
                          uint32_t flags)
{
    fb_u16(f, sp);
    fb_u16(f, dp);
    fb_u32(f, seq);
    fb_u32(f, ack);
    fb_u8(f, 0x50);
    fb_u8(f, flags);
    fb_u16(f, 65535);
    fb_u16(f, 0);
    fb_u16(f, 0);
}

/* A DNS name in wire form from a dotted string. */
static inline void fb_dns_name(struct fb *f, const char *dotted)
{
    while (*dotted) {
        const char *dot = strchr(dotted, '.');
        uint32_t l = dot ? (uint32_t)(dot - dotted) : (uint32_t)strlen(dotted);

        fb_u8(f, l);
        fb_put(f, dotted, l);
        dotted += l;
        if (*dotted == '.') {
            dotted++;
        }
    }
    fb_u8(f, 0);
}

/* A DNS message with one question; an A answer by compression pointer when
 * answer is set. */
static inline void fb_dns(struct fb *f, uint32_t id, uint32_t flags, const char *name, uint32_t type,
                          int answer)
{
    fb_u16(f, id);
    fb_u16(f, flags);
    fb_u16(f, 1);
    fb_u16(f, answer ? 1 : 0);
    fb_u16(f, 0);
    fb_u16(f, 0);
    fb_dns_name(f, name);
    fb_u16(f, type);
    fb_u16(f, 1);
    if (answer) {
        fb_u16(f, 0xc00c);
        fb_u16(f, type);
        fb_u16(f, 1);
        fb_u32(f, 300);
        fb_u16(f, 4);
        fb_u8(f, 198);
        fb_u8(f, 51);
        fb_u8(f, 100);
        fb_u8(f, 7);
    }
}

/* A UDP datagram over IPv4 with an arbitrary payload builder already run:
 * use fb_ip4/fb_udp, write the payload, then these two ends. */
static inline void fb_udp4_end(struct fb *f, uint32_t ip, uint32_t udp)
{
    fb_udp_end(f, udp);
    fb_ip4_end(f, ip);
}

/* Ethernet's minimum: pad a short frame to 60 bytes. */
static inline void fb_pad(struct fb *f)
{
    while (f->n < 60) {
        fb_u8(f, 0);
    }
}

#endif
