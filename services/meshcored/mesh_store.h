/*
 * meshcored's persistence: the local identity and the node table.
 *
 * Internal to the C++ half of the service. It serialises MeshCore's own
 * types, so it lives beside the runtime rather than in the C daemon, and
 * nothing above mesh_runtime.h ever sees it.
 *
 * Two files, both under the state directory, both mode 0600, both written to
 * a temporary name in the same directory and renamed over the real one so a
 * reader sees either the whole previous file or the whole new one:
 *
 *   identity.id   96 bytes, pub_key(32) then prv_key(64). This is a MeshCore
 *                 `.id` byte for byte - the same file tools/meshcore-frame
 *                 writes and reads - so a bench identity can be moved between
 *                 the two without conversion. It is created with O_EXCL and
 *                 never overwritten: a key that exists is the node's identity
 *                 and replacing it silently would make the node a stranger to
 *                 every peer that knows it.
 *
 *   state.v1      the node's advert name and its known nodes. Versioned,
 *                 fixed-width, little-endian, with its exact length implied
 *                 by the record count so a truncated file is refused rather
 *                 than half read.
 *
 *   channels.v1   the group channels this node has joined: a slot, a local
 *                 name and the pre-shared key itself. Same shape, same
 *                 rules, same mode as state.v1 - and, like identity.id, it
 *                 holds key material, which is why it is 0600 and why the
 *                 keys never leave this service through any IPC method.
 *
 * What is NOT here, by decision: messages, channel ones included. They are
 * runtime-only (docs/services/MESHCORED.md, "What is persistent").
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MCD_MESH_STORE_H
#define MCD_MESH_STORE_H

#include <stddef.h>
#include <stdint.h>

#include <Identity.h>
#include <helpers/ContactInfo.h>

namespace mcdstore {

/* The most nodes state.v1 carries: every contact MeshCore's table can hold
 * (MAX_CONTACTS, protocols/meshcore/compat/mc_contacts.h), and not its eight
 * reserved anonymous slots. The file's count is 16 bits, so the format is
 * unchanged by the number; a file written with more nodes than an older
 * build allows is refused and moved aside by that build, not half read. */
static const int MAX_NODES = MAX_CONTACTS;

static const size_t IDENTITY_FILE_SIZE = PUB_KEY_SIZE + PRV_KEY_SIZE;  /* 96 */
static const size_t ERR_SIZE = 1024;

/* Create the state directory (mode 0700) and every missing parent. Returns
 * true when it exists afterwards. */
bool ensureDir(const char* dir, char* err);

/* Load the identity from <dir>/identity.id.
 *
 *   1  loaded, and the key pair is usable
 *   0  the file does not exist
 *  -1  it exists and is not a usable identity; err says why
 *
 * The -1 case is never turned into "generate a new one". A file that is
 * there but wrong is a fault to report, not a reason to become a different
 * node. */
int identityLoad(mesh::LocalIdentity& id, const char* dir, char* err);

/* Generate a new identity and write it with O_EXCL, so a file that appeared
 * in between is not clobbered. */
bool identityCreate(mesh::LocalIdentity& id, char* err);
bool identitySave(const mesh::LocalIdentity& id, const char* dir, char* err);

/* The node state. name is NUL-terminated; nodes[0..count) are contacts. */
struct NodeState {
    char name[32];
    int count;
    ContactInfo nodes[MAX_NODES];
};

/* Load <dir>/state.v1.
 *
 *   1  loaded
 *   0  the file does not exist (st is left empty)
 *  -1  it exists and is not readable as state.v1; err says why
 *
 * Every field is range-checked on the way in: the name is forced
 * NUL-terminated, a path length other than OUT_PATH_UNKNOWN must encode a
 * path that fits MAX_PATH_SIZE, and the record count must account for the
 * file's exact length. */
int stateLoad(NodeState& st, const char* dir, char* err);
bool stateSave(const NodeState& st, const char* dir, char* err);

/* Move a state.v1 this build will not read out of the way, so the node can
 * start with an empty table instead of dying on it.
 *
 * The file is RENAMED, never rewritten or deleted: it is the only evidence of
 * whatever went wrong, and overwriting it before anybody has looked would
 * destroy the one thing that could explain it. The new name is returned in
 * kept, so the log and mesh.status can say where it went.
 *
 * Returns true when the file is out of the way. False means it is still
 * there, and the caller must then not write a new one over it. */
bool stateQuarantine(const char* dir, char* kept, size_t kept_len, char* err);

/* ---- the channels -------------------------------------------------------
 *
 * Kept in their own file rather than inside state.v1, for two reasons that
 * both matter. It holds key material and state.v1 does not, so the two have
 * different consequences when one of them is unreadable; and a corrupt node
 * table costing a rediscovery is very different from a corrupt channel table
 * costing every channel the operator typed in by hand. Separate files mean
 * one fault cannot take the other with it.
 *
 * The secret is stored as MeshCore holds it: 32 bytes, zero-padded when the
 * key is 128-bit. key_len says which it is, because that decides the derived
 * channel hash (BaseChatMesh.cpp:896-906) and a guess would put this node on
 * a channel its peers hash differently.
 */
static const int MAX_CHANNELS = 8;

struct ChannelRecord {
    int slot;          /* 0 .. MAX_CHANNELS-1; the identity a client names */
    char name[32];     /* local only; never on the air */
    uint8_t secret[32];
    int key_len;       /* 16 or 32 */
};

struct ChannelState {
    int count;
    ChannelRecord channels[MAX_CHANNELS];
};

/* Load <dir>/channels.v1.
 *
 *   1  loaded
 *   0  the file does not exist (cs is left empty)
 *  -1  it exists and is not readable as channels.v1; err says why
 *
 * Range-checked the same way stateLoad is: the exact length implied by the
 * record count, a slot inside the table, a key length of 16 or 32, a name
 * forced NUL-terminated and non-empty, no two records in one slot, no
 * all-zero key (which is what an unused MeshCore slot holds, and is
 * therefore not a channel), and no two records holding the same key. */
int channelsLoad(ChannelState& cs, const char* dir, char* err);
bool channelsSave(const ChannelState& cs, const char* dir, char* err);

/* Move a channels.v1 this build will not read out of the way. Same contract
 * as stateQuarantine: renamed, never rewritten. */
bool channelsQuarantine(const char* dir, char* kept, size_t kept_len, char* err);

#ifdef MCD_STORE_TEST_HOOKS
/* Present only in the hooked build of mesh_store.cpp, which the test binaries
 * link in place of the shipped one; tests/meshcored_lint.sh checks the symbol
 * is absent from the service. Makes the next n directory flushes fail, which
 * a working filesystem will not do on request - and an error path that has
 * never been executed is a guess rather than a behaviour. */
void failNextDirSyncForTest(int n);
#endif

}  // namespace mcdstore

#endif /* MCD_MESH_STORE_H */
