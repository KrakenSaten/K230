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
 * What is NOT here, by decision: messages. They are runtime-only in this
 * phase (docs/services/MESHCORED.md, "What is persistent").
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

/* The most nodes state.v1 carries. MeshCore's own contact table is
 * MAX_CONTACTS (32) plus eight reserved anonymous slots; only real contacts
 * are persisted. */
static const int MAX_NODES = 32;

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
