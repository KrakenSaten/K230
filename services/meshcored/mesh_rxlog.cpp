/*
 * The receive log's bookkeeping. See mesh_rxlog.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mesh_rxlog.h"

#include <string.h>

static_assert(MCD_RX_HASH_LEN == MAX_HASH_SIZE, "packet hash size drifted from MeshCore");

namespace mcdrx {

Log::Log()
    : _seq(0), _order(0), _seen_next(0), _raw_pending(false), _raw_len(0),
      _raw_header_known(false), _raw_header(0), _raw_no_buffer(false)
{
    memset(_seen, 0, sizeof(_seen));
    memset(_slot, 0, sizeof(_slot));
    memset(&_raw_meta, 0, sizeof(_raw_meta));
}

Log::Seen* Log::seen(const uint8_t hash[MCD_RX_HASH_LEN], bool add)
{
    for (int i = 0; i < MCD_RX_SEEN_HASHES; i++) {
        if (_seen[i].used && memcmp(_seen[i].hash, hash, MCD_RX_HASH_LEN) == 0) {
            return &_seen[i];
        }
    }
    if (!add) {
        return NULL;
    }
    /* A ring: the oldest hash is forgotten first, as in MeshCore's own
     * table, so a packet heard again long after has its count start over. */
    Seen* s = &_seen[_seen_next];

    _seen_next = (_seen_next + 1) % MCD_RX_SEEN_HASHES;
    memset(s, 0, sizeof(*s));
    memcpy(s->hash, hash, MCD_RX_HASH_LEN);
    s->used = true;
    return s;
}

void Log::header(uint8_t h, mcd_rx_obs& out)
{
    out.header = h;
    out.header_known = true;
    out.route_type = h & PH_ROUTE_MASK;
    out.payload_type = (h >> PH_TYPE_SHIFT) & PH_TYPE_MASK;
    out.payload_ver = (h >> PH_VER_SHIFT) & PH_VER_MASK;
}

void Log::capture(const mesh::Packet* pkt, int len, const mcd_rx_meta& meta, mcd_rx_obs& out)
{
    memset(&out, 0, sizeof(out));
    out.seq = ++_seq;
    out.meta = meta;
    out.bytes = len;
    out.parsed = true;
    header(pkt->header, out);

    /* The path as it arrived. The dispatcher has already refused a path
     * longer than MAX_PATH_SIZE, so the copy is whole; the bound is kept
     * anyway because this is a copy into a fixed buffer. */
    out.path_is_snr = out.payload_type == PAYLOAD_TYPE_TRACE;
    out.path_hash_size = pkt->getPathHashSize();
    out.path_hops = pkt->getPathHashCount();
    out.path_bytes = pkt->getPathByteLen();
    if (out.path_bytes > MCD_MAX_PATH) {
        out.path_bytes = MCD_MAX_PATH;
    }
    memcpy(out.path, pkt->path, out.path_bytes);

    /* MeshCore's own packet hash - the one its seen-table keys on - so a
     * repeat here is a repeat there. */
    pkt->calculatePacketHash(out.hash);
    out.hash_known = true;

    switch (out.payload_type) {
    case PAYLOAD_TYPE_GRP_TXT:
    case PAYLOAD_TYPE_GRP_DATA:
        if (pkt->payload_len >= 1) {
            out.has_channel_hash = true;
            out.channel_hash = pkt->payload[0];
        }
        break;
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_TXT_MSG:
    case PAYLOAD_TYPE_PATH:
        if (pkt->payload_len >= 2) {
            out.has_dest_hash = true;
            out.dest_hash = pkt->payload[0];
            out.has_src_hash = true;
            out.src_hash = pkt->payload[1];
        }
        break;
    case PAYLOAD_TYPE_ANON_REQ:
        /* The sender is an ephemeral key, not a node: no source hash. */
        if (pkt->payload_len >= 1) {
            out.has_dest_hash = true;
            out.dest_hash = pkt->payload[0];
        }
        break;
    case PAYLOAD_TYPE_ADVERT:
        if (pkt->payload_len >= 1) {
            out.has_src_hash = true;
            out.src_hash = pkt->payload[0];
        }
        break;
    case PAYLOAD_TYPE_CONTROL:
        if (pkt->payload_len >= 1) {
            out.has_control_flags = true;
            out.control_flags = pkt->payload[0];
        }
        break;
    default:
        break;
    }

    Seen* s = seen(out.hash, true);

    s->count++;
    out.dup = s->count;
    out.own = s->own;
}

void Log::noteOwn(const mesh::Packet* pkt)
{
    uint8_t hash[MCD_RX_HASH_LEN];

    pkt->calculatePacketHash(hash);
    Seen* s = seen(hash, true);

    s->own = true;
}

bool Log::hold(const mesh::Packet* pkt, const mcd_rx_obs& obs, mcd_rx_obs* evicted)
{
    int free_slot = -1;
    int oldest = 0;
    bool gave_up = false;

    for (int i = 0; i < PENDING; i++) {
        if (_slot[i].used && _slot[i].pkt == pkt) {
            /* The pool handed this packet out again while an observation was
             * still held for it: whatever MeshCore did with the first one,
             * the log was not told. */
            free_slot = i;
            break;
        }
        if (!_slot[i].used && free_slot < 0) {
            free_slot = i;
        }
        if (_slot[i].used && _slot[i].order < _slot[oldest].order) {
            oldest = i;
        }
    }
    if (free_slot < 0) {
        free_slot = oldest;
    }
    if (_slot[free_slot].used) {
        if (evicted) {
            *evicted = _slot[free_slot].obs;
            evicted->verdict = MCD_RX_UNRESOLVED;
        }
        gave_up = true;
    }
    _slot[free_slot].used = true;
    _slot[free_slot].pkt = pkt;
    _slot[free_slot].order = ++_order;
    _slot[free_slot].obs = obs;
    return gave_up;
}

bool Log::take(const mesh::Packet* pkt, mcd_rx_obs& out)
{
    for (int i = 0; i < PENDING; i++) {
        if (_slot[i].used && _slot[i].pkt == pkt) {
            out = _slot[i].obs;
            _slot[i].used = false;
            _slot[i].pkt = NULL;
            return true;
        }
    }
    return false;
}

void Log::noteRaw(int len, bool header_known, uint8_t hdr, bool no_buffer, const mcd_rx_meta& meta)
{
    _raw_pending = true;
    _raw_len = len;
    _raw_header_known = header_known;
    _raw_header = hdr;
    _raw_no_buffer = no_buffer;
    _raw_meta = meta;
}

bool Log::takeRaw(mcd_rx_obs& out)
{
    if (!_raw_pending) {
        return false;
    }
    _raw_pending = false;
    memset(&out, 0, sizeof(out));
    out.seq = ++_seq;
    out.meta = _raw_meta;
    out.bytes = _raw_len;
    if (_raw_header_known) {
        header(_raw_header, out);
    }
    out.verdict = MCD_RX_REJECTED;
    out.reject = _raw_no_buffer ? MCD_RX_REJECT_NO_BUFFER : MCD_RX_REJECT_UNPARSED;
    return true;
}

void Log::rejected(const uint8_t* bytes, int len, const mcd_rx_meta& meta, enum mcd_rx_reject why,
                   mcd_rx_obs& out)
{
    memset(&out, 0, sizeof(out));
    out.seq = ++_seq;
    out.meta = meta;
    out.bytes = len;
    if (bytes && len >= 1) {
        header(bytes[0], out);
    }
    out.verdict = MCD_RX_REJECTED;
    out.reject = why;
}

}  // namespace mcdrx
