/*
 * The receive log's bookkeeping (struct mcd_rx_obs, mesh_runtime.h), on the
 * protocol side of the seam.
 *
 * mesh_runtime.cpp's Node owns the hooks: the dispatcher tells it about a
 * frame twice - its raw bytes (logRxRaw) and, if they parsed, the packet
 * (logRx) - and MeshCore's handler takes the packet some time later, at once
 * for most frames and after the receive delay for a flood heard weakly. This
 * is what carries an observation across those three points without touching
 * any of them:
 *
 *   capture()  reads the packet as it came off the air, before MeshCore has
 *              changed anything in it, and counts the reception by its hash
 *   hold() / take()   keep it, keyed by the packet, until the handler runs
 *   noteRaw() / takeRaw()   a frame the dispatcher never parsed
 *   rejected() a frame that never reached the dispatcher at all
 *
 * Nothing here decides anything about the packet. MeshCore's own seen-table
 * is not read or written: the repeat count is this log's, in its own table,
 * so the log cannot change what MeshCore deduplicates.
 *
 * Bounded throughout: PENDING observations in flight (the packet pool holds
 * 32, so it cannot fill), MCD_RX_SEEN_HASHES hashes remembered. No
 * allocation.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MCD_MESH_RXLOG_H
#define MCD_MESH_RXLOG_H

#include <Packet.h>

#include "mesh_runtime.h"

namespace mcdrx {

/* Observations waiting for MeshCore's handler. More than the packet pool, so
 * a packet the pool holds always has a slot. */
const int PENDING = 40;

class Log {
public:
    Log();

    /* The packet as received: seq, signal, size, header, path, hash, the
     * addressing hashes and the repeat count. Everything MeshCore will later
     * say about it is left zero for the caller to fill. */
    void capture(const mesh::Packet* pkt, int len, const mcd_rx_meta& meta, mcd_rx_obs& out);

    /* Keep obs until take(pkt). When every slot is taken the oldest is given
     * up - copied to *evicted, verdict MCD_RX_UNRESOLVED - and true is
     * returned so the caller reports it rather than losing it silently. */
    bool hold(const mesh::Packet* pkt, const mcd_rx_obs& obs, mcd_rx_obs* evicted);
    /* The observation held for pkt, if there is one; its slot is freed. */
    bool take(const mesh::Packet* pkt, mcd_rx_obs& out);

    /* The dispatcher took bytes off the air; logRx follows only if they
     * parsed. no_buffer: the packet pool was empty, so they were never even
     * offered to the parser. */
    void noteRaw(int len, bool header_known, uint8_t header, bool no_buffer,
                 const mcd_rx_meta& meta);
    void clearRaw() { _raw_pending = false; }
    /* The raw frame noted and never parsed, as a rejected observation. */
    bool takeRaw(mcd_rx_obs& out);

    /* A frame turned away before the dispatcher saw it. */
    void rejected(const uint8_t* bytes, int len, const mcd_rx_meta& meta,
                  enum mcd_rx_reject why, mcd_rx_obs& out);

    /* This node transmitted pkt (logTx): a later reception of the same hash
     * is its own packet repeated back. */
    void noteOwn(const mesh::Packet* pkt);

private:
    struct Seen {
        uint8_t hash[MCD_RX_HASH_LEN];
        uint32_t count;
        bool own;
        bool used;
    };
    struct Slot {
        bool used;
        const mesh::Packet* pkt;
        uint64_t order;
        mcd_rx_obs obs;
    };

    Seen* seen(const uint8_t hash[MCD_RX_HASH_LEN], bool add);
    void header(uint8_t h, mcd_rx_obs& out);

    uint64_t _seq;
    uint64_t _order;
    Seen _seen[MCD_RX_SEEN_HASHES];
    int _seen_next;
    Slot _slot[PENDING];

    bool _raw_pending;
    int _raw_len;
    bool _raw_header_known;
    uint8_t _raw_header;
    bool _raw_no_buffer;
    mcd_rx_meta _raw_meta;
};

}  // namespace mcdrx

#endif
