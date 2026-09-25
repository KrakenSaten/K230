/*
 * Placeholder until meshcored serves app datagrams (P5): a link that is never
 * up. Replaced by the real client in the same file.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_link_mesh.h"

#include <stdlib.h>

static void m_poll(void *ctx, int64_t now) { (void)ctx; (void)now; }
static enum fleet_link_send m_send(void *ctx, const uint8_t to[FLEET_KEY_BYTES],
                                   const uint8_t *b, size_t n)
{
    (void)ctx; (void)to; (void)b; (void)n;
    return FLEET_LINK_FAILED;
}
static int m_recv(void *ctx, uint8_t from[FLEET_KEY_BYTES], uint8_t *b, size_t *n)
{
    (void)ctx; (void)from; (void)b; (void)n;
    return 0;
}
static int m_self(void *ctx, uint8_t key[FLEET_KEY_BYTES]) { (void)ctx; (void)key; return -1; }
static enum fleet_link_state m_state(void *ctx) { (void)ctx; return FLEET_LINK_NO_SERVICE; }
static int m_peers(void *ctx, struct fleet_link_peer *o, int max) { (void)ctx; (void)o; (void)max; return 0; }
static const char *m_name(void *ctx, const uint8_t k[FLEET_KEY_BYTES]) { (void)ctx; (void)k; return NULL; }
static int m_advert(void *ctx) { (void)ctx; return -1; }
static uint32_t m_base(void *ctx) { (void)ctx; return 0; }
static void m_close(void *ctx) { free(ctx); }

static const struct fleet_link_ops OPS = {
    m_poll, m_send, m_recv, m_self, m_state, m_peers, m_name, m_advert, m_base, m_close,
};

struct fleet_link *fleet_link_mesh_open(const char *service)
{
    struct fleet_link *l = calloc(1, sizeof(*l));

    (void)service;
    if (l) {
        l->ops = &OPS;
        l->ctx = l;
    }
    return l;
}
