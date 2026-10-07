/*
 * meshcored: persistence. See mesh_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mesh_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>

#include "mc_port.h"

namespace mcdstore {

namespace {

const char kIdentityName[] = "identity.id";
const char kStateName[] = "state.v1";
const char kChannelsName[] = "channels.v1";

/* state.v1, little-endian, fixed widths. */
const uint8_t kMagic[4] = { 'M', 'C', 'D', 'S' };
const uint16_t kVersion = 1;
const size_t kHeaderSize = 44;
const size_t kRecordSize = 148;

/* channels.v1, same shape. Its own magic, so a state.v1 renamed onto it - or
 * the other way round - is refused rather than read as the wrong thing. */
const uint8_t kChanMagic[4] = { 'M', 'C', 'D', 'C' };
const uint16_t kChanVersion = 1;
const size_t kChanHeaderSize = 12;
const size_t kChanRecordSize = 68;

void joinPath(char* dest, size_t dest_len, const char* dir, const char* name)
{
    snprintf(dest, dest_len, "%s/%s", dir, name);
}

void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

uint16_t get16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

#ifdef MCD_STORE_TEST_HOOKS
/* Present only in the hooked build of this file (see the Makefile), and
 * checked out of the shipped object by tests/meshcored_lint.sh.
 *
 * A directory fsync does not fail on a working filesystem, so without a seam
 * the error path below would never once be executed - and an error path that
 * has never run is a guess. The hook injects at the point the syscall
 * returns; everything after it is the shipped code taking its real path. */
int g_dirsync_fail_countdown;
bool dirsync_should_fail(void)
{
    if (g_dirsync_fail_countdown > 0) {
        g_dirsync_fail_countdown--;
        return true;
    }
    return false;
}
#else
bool dirsync_should_fail(void) { return false; }
#endif

/* Make a directory entry durable.
 *
 * fsync() on a file flushes its contents; it says nothing about the entry
 * that names it. A power cut after the data was written and before the
 * directory block was can leave a file that is complete and unreachable, or a
 * name with no file behind it. For identity.id that is the difference between
 * a node that comes back and one that has become a stranger to every peer it
 * knows, so it is worth one more syscall. */
bool syncDir(const char* dir, char* err)
{
    int fd;

    if (dirsync_should_fail()) {
        snprintf(err, ERR_SIZE, "cannot flush the directory %s: %s", dir, strerror(EIO));
        return false;
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        snprintf(err, ERR_SIZE, "cannot open %s to flush it: %s", dir, strerror(errno));
        return false;
    }
    if (fsync(fd) != 0) {
        snprintf(err, ERR_SIZE, "cannot flush the directory %s: %s", dir, strerror(errno));
        close(fd);
        return false;
    }
    if (close(fd) != 0) {
        snprintf(err, ERR_SIZE, "cannot close %s after flushing: %s", dir, strerror(errno));
        return false;
    }
    return true;
}

/* Write buf to path through a temporary in the same directory, then rename.
 * mode is the final mode; the temporary carries it from the start, so the
 * bytes are never on disk world-readable even for an instant. */
bool writeWhole(const char* path, const uint8_t* buf, size_t len, mode_t mode, char* err)
{
    char tmp[288];
    int fd;
    size_t done = 0;

    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        snprintf(err, ERR_SIZE, "path too long: %s", path);
        return false;
    }
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        snprintf(err, ERR_SIZE, "cannot create %s: %s", tmp, strerror(errno));
        return false;
    }
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            snprintf(err, ERR_SIZE, "cannot write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return false;
        }
        done += (size_t)n;
    }
    /* The rename is atomic, but it does not order the data against it. A
     * power cut on this board is ordinary, so the bytes are on the medium
     * before the name points at them. */
    if (fsync(fd) != 0) {
        snprintf(err, ERR_SIZE, "cannot flush %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return false;
    }
    if (close(fd) != 0) {
        snprintf(err, ERR_SIZE, "cannot close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, ERR_SIZE, "cannot rename %s to %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return false;
    }
    /* And the directory, because the rename is a change to the directory:
     * flushing the file made its contents durable, not the name that reaches
     * them. The caller is told when this fails rather than being allowed to
     * report a write that may not have landed. */
    {
        char dir[288];
        char* slash;

        snprintf(dir, sizeof(dir), "%s", path);
        slash = strrchr(dir, '/');
        if (slash != NULL && slash != dir) {
            *slash = '\0';
            if (!syncDir(dir, err)) {
                return false;
            }
        }
    }
    return true;
}

/* Read a whole file that must be no longer than max. Returns the length, -1
 * on an error (err set), or -2 when it does not exist. */
long readWhole(const char* path, uint8_t* buf, size_t max, char* err)
{
    FILE* f = fopen(path, "rb");
    size_t n;
    bool too_long;

    if (f == NULL) {
        if (errno == ENOENT) {
            return -2;
        }
        snprintf(err, ERR_SIZE, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    n = fread(buf, 1, max, f);
    too_long = (fgetc(f) != EOF);
    fclose(f);
    if (too_long) {
        snprintf(err, ERR_SIZE, "%s is longer than this format allows", path);
        return -1;
    }
    return (long)n;
}

}  // namespace

#ifdef MCD_STORE_TEST_HOOKS
/* At namespace scope, because it is what the test calls; the counter it sets
 * stays private above. */
void failNextDirSyncForTest(int n) { g_dirsync_fail_countdown = n; }
#endif

bool ensureDir(const char* dir, char* err)
{
    char work[256];
    size_t i;

    /* Room for the directory and the longest name this store puts inside it,
     * checked once here rather than discovered as a truncated path later. */
    if (dir == NULL || dir[0] != '/' || strlen(dir) + 1 + 32 >= sizeof(work)) {
        snprintf(err, ERR_SIZE,
                 "the state directory must be an absolute path under %zu characters",
                 sizeof(work) - 33);
        return false;
    }
    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i] != '\0'; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) {
            snprintf(err, ERR_SIZE, "cannot create %s: %s", work, strerror(errno));
            return false;
        }
        work[i] = '/';
    }
    /* The leaf is 0700: it holds a private key. The parents are the ordinary
     * state directories other services share, so they keep 0755. */
    if (mkdir(work, 0700) != 0 && errno != EEXIST) {
        snprintf(err, ERR_SIZE, "cannot create %s: %s", work, strerror(errno));
        return false;
    }
    return true;
}

int identityLoad(mesh::LocalIdentity& id, const char* dir, char* err)
{
    char path[256];
    uint8_t buf[IDENTITY_FILE_SIZE];
    long n;

    joinPath(path, sizeof(path), dir, kIdentityName);
    n = readWhole(path, buf, sizeof(buf), err);
    if (n == -2) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != IDENTITY_FILE_SIZE) {
        snprintf(err, ERR_SIZE, "%s is %ld bytes; a MeshCore identity is %zu",
                 path, n, IDENTITY_FILE_SIZE);
        return -1;
    }

    /* pub_key then prv_key, which is what LocalIdentity::writeTo(Stream&)
     * produces and what a MeshCore .id holds. */
    const uint8_t* prv = &buf[PUB_KEY_SIZE];

    /* MeshCore's own test before it will use a key for ECDH: the two halves
     * agree and the shared secret is not degenerate. Without it a file of 96
     * arbitrary bytes loads happily and fails later as an advert nobody can
     * verify, a long way from the file that caused it. */
    if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
        snprintf(err, ERR_SIZE, "%s does not hold a usable Ed25519 key pair", path);
        return -1;
    }
    /* And the stored public key must be the one that private key derives.
     * MeshCore re-derives it when only the private half is present, so a
     * disagreement here would mean signing as one node and being named as
     * another. */
    mesh::LocalIdentity derived;
    derived.readFrom(prv, PRV_KEY_SIZE);
    if (memcmp(derived.pub_key, buf, PUB_KEY_SIZE) != 0) {
        snprintf(err, ERR_SIZE,
                 "%s: the stored public key is not the one its private key derives", path);
        memset(buf, 0, sizeof(buf));
        return -1;
    }
    /* Read through MeshCore's own Stream reader, so the file is loaded by the
     * same code that wrote it rather than by a second copy of the layout. */
    {
        MemStream s(buf, sizeof(buf), sizeof(buf));

        if (!id.readFrom(s)) {
            snprintf(err, ERR_SIZE, "%s could not be read as a MeshCore identity", path);
            memset(buf, 0, sizeof(buf));
            return -1;
        }
    }
    memset(buf, 0, sizeof(buf));
    return 1;
}

bool identityCreate(mesh::LocalIdentity& id, char* err)
{
    mcport::HostRNG rng;

    /* MeshCore refuses 00- and FF-prefixed public keys: the first byte is the
     * node's path hash (Identity::copyHashTo) and those two values collide
     * with reserved markers. Draw again rather than ship a key the network
     * will not route to. */
    for (int tries = 0; tries < 8; tries++) {
        mesh::LocalIdentity candidate(&rng);

        if (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xFF) {
            continue;
        }
        id = candidate;
        return true;
    }
    snprintf(err, ERR_SIZE, "could not generate a usable identity in eight attempts");
    return false;
}

bool identitySave(const mesh::LocalIdentity& id, const char* dir, char* err)
{
    char path[256];
    uint8_t buf[IDENTITY_FILE_SIZE];
    int fd;
    size_t done = 0;

    joinPath(path, sizeof(path), dir, kIdentityName);
    {
        MemStream s(buf, sizeof(buf));

        if (!id.writeTo(s)) {
            snprintf(err, ERR_SIZE, "could not serialise the identity");
            return false;
        }
    }
    /* O_EXCL, and no temporary-and-rename: an identity file that already
     * exists is this node's identity, and the one thing this must never do is
     * replace it. */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        snprintf(err, ERR_SIZE, "cannot create %s: %s", path, strerror(errno));
        memset(buf, 0, sizeof(buf));
        return false;
    }
    while (done < sizeof(buf)) {
        ssize_t n = write(fd, buf + done, sizeof(buf) - done);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            snprintf(err, ERR_SIZE, "cannot write %s: %s", path, strerror(errno));
            close(fd);
            unlink(path);
            memset(buf, 0, sizeof(buf));
            return false;
        }
        done += (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        snprintf(err, ERR_SIZE, "cannot flush %s: %s", path, strerror(errno));
        unlink(path);
        memset(buf, 0, sizeof(buf));
        return false;
    }
    memset(buf, 0, sizeof(buf));
    /* The entry, not just the contents. This is a brand-new name in the
     * directory, and until the directory itself is durable the file may not
     * be there after a power cut - which for an identity means a node that
     * silently becomes somebody else on its next start. Reported as a
     * failure, and the file is removed, so the caller never records a
     * persisted identity that may not be persisted. */
    if (!syncDir(dir, err)) {
        unlink(path);
        return false;
    }
    return true;
}

/* Move one unreadable file in the state directory aside. Shared by state.v1
 * and channels.v1, which want exactly the same treatment: renamed, never
 * rewritten or deleted, because the file is the only evidence of whatever
 * went wrong and overwriting it before anybody has looked destroys the one
 * thing that could explain it. */
static bool quarantineFile(const char* dir, const char* name, char* kept, size_t kept_len,
                           char* err)
{
    char path[256];
    char dest[288];
    struct stat sb;
    unsigned n;

    joinPath(path, sizeof(path), dir, name);
    if (stat(path, &sb) != 0) {
        snprintf(err, ERR_SIZE, "%s: %s", path, strerror(errno));
        return false;
    }
    /* A name that does not collide with an earlier quarantine, and does not
     * depend on the wall clock: this board starts at 1970 on every boot, so a
     * timestamp would make two different faults share a name. */
    for (n = 0; n < 1000; n++) {
        snprintf(dest, sizeof(dest), "%s.corrupt.%u", path, n);
        if (stat(dest, &sb) != 0 && errno == ENOENT) {
            break;
        }
    }
    if (n >= 1000) {
        snprintf(err, ERR_SIZE, "%s: a thousand quarantined copies already exist", path);
        return false;
    }
    if (rename(path, dest) != 0) {
        snprintf(err, ERR_SIZE, "cannot move %s aside: %s", path, strerror(errno));
        return false;
    }
    if (!syncDir(dir, err)) {
        return false;
    }
    snprintf(kept, kept_len, "%s", dest);
    return true;
}

bool stateQuarantine(const char* dir, char* kept, size_t kept_len, char* err)
{
    return quarantineFile(dir, kStateName, kept, kept_len, err);
}

bool channelsQuarantine(const char* dir, char* kept, size_t kept_len, char* err)
{
    return quarantineFile(dir, kChannelsName, kept, kept_len, err);
}

int stateLoad(NodeState& st, const char* dir, char* err)
{
    char path[256];
    static uint8_t buf[kHeaderSize + kRecordSize * MAX_NODES];
    long n;
    uint16_t version;
    uint16_t count;
    size_t expect;

    st = NodeState();
    joinPath(path, sizeof(path), dir, kStateName);
    n = readWhole(path, buf, sizeof(buf), err);
    if (n == -2) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if ((size_t)n < kHeaderSize) {
        snprintf(err, ERR_SIZE, "%s is %ld bytes; the header alone is %zu", path, n, kHeaderSize);
        return -1;
    }
    if (memcmp(buf, kMagic, sizeof(kMagic)) != 0) {
        snprintf(err, ERR_SIZE, "%s does not start with the state.v1 magic", path);
        return -1;
    }
    version = get16(&buf[4]);
    if (version != kVersion) {
        snprintf(err, ERR_SIZE, "%s is version %u; this build reads version %u",
                 path, (unsigned)version, (unsigned)kVersion);
        return -1;
    }
    count = get16(&buf[40]);
    if (count > MAX_NODES) {
        snprintf(err, ERR_SIZE, "%s declares %u nodes; the limit is %d",
                 path, (unsigned)count, MAX_NODES);
        return -1;
    }
    /* The exact length, not a minimum: a truncated file that happens to
     * contain whole records would otherwise load as if nothing were missing. */
    expect = kHeaderSize + kRecordSize * (size_t)count;
    if ((size_t)n != expect) {
        snprintf(err, ERR_SIZE, "%s is %ld bytes; %u nodes need exactly %zu",
                 path, n, (unsigned)count, expect);
        return -1;
    }

    memcpy(st.name, &buf[8], sizeof(st.name));
    st.name[sizeof(st.name) - 1] = '\0';

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* r = &buf[kHeaderSize + kRecordSize * (size_t)i];
        ContactInfo& c = st.nodes[i];

        c = ContactInfo();
        memcpy(c.id.pub_key, r, PUB_KEY_SIZE);
        memcpy(c.name, &r[32], sizeof(c.name));
        c.name[sizeof(c.name) - 1] = '\0';
        c.type = r[64];
        c.flags = r[65];
        c.out_path_len = r[66];
        memcpy(c.out_path, &r[68], MAX_PATH_SIZE);
        c.last_advert_timestamp = get32(&r[132]);
        c.lastmod = get32(&r[136]);
        c.gps_lat = (int32_t)get32(&r[140]);
        c.gps_lon = (int32_t)get32(&r[144]);
        c.shared_secret_valid = false;

        /* A node with no type would be read by MeshCore as an empty contact
         * slot (ADV_TYPE_NONE is how the table marks "free"), so a stored
         * record with type 0 is corruption, not a node. */
        if (c.type == ADV_TYPE_NONE) {
            snprintf(err, ERR_SIZE, "%s: node %u has no advert type", path, (unsigned)i);
            return -1;
        }
        /* A path length must be either "unknown" or an encoding MeshCore
         * itself would accept; anything else reaches Packet::copyPath as a
         * byte count. */
        if (c.out_path_len != OUT_PATH_UNKNOWN &&
            !mesh::Packet::isValidPathLen(c.out_path_len)) {
            snprintf(err, ERR_SIZE, "%s: node %u has an impossible path length %u",
                     path, (unsigned)i, (unsigned)c.out_path_len);
            return -1;
        }
        /* Two public keys the same would give the node table two entries a
         * lookup cannot tell apart. */
        for (uint16_t j = 0; j < i; j++) {
            if (memcmp(st.nodes[j].id.pub_key, c.id.pub_key, PUB_KEY_SIZE) == 0) {
                snprintf(err, ERR_SIZE, "%s: nodes %u and %u have the same public key",
                         path, (unsigned)j, (unsigned)i);
                return -1;
            }
        }
    }
    st.count = count;
    return 1;
}

bool stateSave(const NodeState& st, const char* dir, char* err)
{
    char path[256];
    static uint8_t buf[kHeaderSize + kRecordSize * MAX_NODES];
    int count = st.count;
    size_t len;

    if (count < 0) {
        count = 0;
    }
    if (count > MAX_NODES) {
        count = MAX_NODES;
    }
    len = kHeaderSize + kRecordSize * (size_t)count;
    memset(buf, 0, len);
    memcpy(buf, kMagic, sizeof(kMagic));
    put16(&buf[4], kVersion);
    put16(&buf[6], 0);  /* flags, reserved */
    memcpy(&buf[8], st.name, sizeof(st.name));
    buf[8 + sizeof(st.name) - 1] = '\0';
    put16(&buf[40], (uint16_t)count);
    put16(&buf[42], 0);  /* reserved */

    for (int i = 0; i < count; i++) {
        uint8_t* r = &buf[kHeaderSize + kRecordSize * (size_t)i];
        const ContactInfo& c = st.nodes[i];

        memcpy(r, c.id.pub_key, PUB_KEY_SIZE);
        memcpy(&r[32], c.name, sizeof(c.name));
        r[32 + sizeof(c.name) - 1] = '\0';
        r[64] = c.type;
        r[65] = c.flags;
        r[66] = c.out_path_len;
        r[67] = 0;
        memcpy(&r[68], c.out_path, MAX_PATH_SIZE);
        put32(&r[132], c.last_advert_timestamp);
        put32(&r[136], c.lastmod);
        put32(&r[140], (uint32_t)c.gps_lat);
        put32(&r[144], (uint32_t)c.gps_lon);
    }

    joinPath(path, sizeof(path), dir, kStateName);
    return writeWhole(path, buf, len, 0600, err);
}

/* ---- channels.v1 --------------------------------------------------------
 *
 * Header 12 bytes: magic(4) version(2) flags(2) count(2) reserved(2).
 * Record 68 bytes: slot(1) key_len(1) reserved(2) name(32) secret(32).
 *
 * Every field is checked on the way in, and a record that fails any check
 * fails the whole file rather than being skipped. A channel table half read
 * is worse than none: the operator sees the channels that survived and has no
 * reason to suspect the ones that did not, so messages go quietly nowhere.
 */
int channelsLoad(ChannelState& cs, const char* dir, char* err)
{
    char path[256];
    static uint8_t buf[kChanHeaderSize + kChanRecordSize * MAX_CHANNELS];
    long n;
    uint16_t version;
    uint16_t count;
    size_t expect;

    cs = ChannelState();
    joinPath(path, sizeof(path), dir, kChannelsName);
    n = readWhole(path, buf, sizeof(buf), err);
    if (n == -2) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if ((size_t)n < kChanHeaderSize) {
        snprintf(err, ERR_SIZE, "%s is %ld bytes; the header alone is %zu", path, n,
                 kChanHeaderSize);
        return -1;
    }
    if (memcmp(buf, kChanMagic, sizeof(kChanMagic)) != 0) {
        snprintf(err, ERR_SIZE, "%s does not start with the channels.v1 magic", path);
        return -1;
    }
    version = get16(&buf[4]);
    if (version != kChanVersion) {
        snprintf(err, ERR_SIZE, "%s is version %u; this build reads version %u",
                 path, (unsigned)version, (unsigned)kChanVersion);
        return -1;
    }
    count = get16(&buf[8]);
    if (count > MAX_CHANNELS) {
        snprintf(err, ERR_SIZE, "%s declares %u channels; the limit is %d",
                 path, (unsigned)count, MAX_CHANNELS);
        return -1;
    }
    expect = kChanHeaderSize + kChanRecordSize * (size_t)count;
    if ((size_t)n != expect) {
        snprintf(err, ERR_SIZE, "%s is %ld bytes; %u channels need exactly %zu",
                 path, n, (unsigned)count, expect);
        return -1;
    }

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* r = &buf[kChanHeaderSize + kChanRecordSize * (size_t)i];
        ChannelRecord& c = cs.channels[i];
        bool all_zero = true;
        int j;

        c.slot = (int)r[0];
        c.key_len = (int)r[1];
        memcpy(c.name, &r[4], sizeof(c.name));
        c.name[sizeof(c.name) - 1] = '\0';
        memcpy(c.secret, &r[36], sizeof(c.secret));

        if (c.slot < 0 || c.slot >= MAX_CHANNELS) {
            snprintf(err, ERR_SIZE, "%s: channel %u is in slot %d; the table has %d",
                     path, (unsigned)i, c.slot, MAX_CHANNELS);
            return -1;
        }
        if (c.key_len != 16 && c.key_len != 32) {
            snprintf(err, ERR_SIZE, "%s: channel %u has a key length of %d; it must be 16 or 32",
                     path, (unsigned)i, c.key_len);
            return -1;
        }
        /* A 128-bit key must be zero-padded, because that padding is part of
         * the 32-byte HMAC key MeshCore derives the MAC with (Utils.cpp:149
         * keys the HMAC with PUB_KEY_SIZE bytes, not CIPHER_KEY_SIZE). A
         * record claiming 16 with rubbish above it would MAC differently
         * from the peer that wrote it. */
        if (c.key_len == 16) {
            for (j = 16; j < 32; j++) {
                if (c.secret[j] != 0) {
                    snprintf(err, ERR_SIZE,
                             "%s: channel %u claims a 128-bit key but is not zero-padded",
                             path, (unsigned)i);
                    return -1;
                }
            }
        } else {
            /* And the same boundary from the other side. A 256-bit key whose
             * upper half is all zero is exactly the key mcd_runtime_channel_add
             * refuses as ambiguous: MeshCore's setChannel() would read it as a
             * 128-bit key and hash it over 16 bytes, while a peer that added it
             * through addChannel() would hash it over 32. Accepting it here
             * would reintroduce through a file the key the API will not take,
             * and the node would then report key_bits 256 while deriving the
             * 128-bit hash - visible to nobody, and unreachable by half its
             * peers. The two paths have to refuse the same keys. */
            bool upper_zero = true;

            for (j = 16; j < 32; j++) {
                if (c.secret[j] != 0) {
                    upper_zero = false;
                }
            }
            if (upper_zero) {
                snprintf(err, ERR_SIZE,
                         "%s: channel %u has a 256-bit key with an all-zero upper half, which "
                         "MeshCore reads as a 128-bit key",
                         path, (unsigned)i);
                return -1;
            }
        }
        for (j = 0; j < c.key_len; j++) {
            if (c.secret[j] != 0) {
                all_zero = false;
            }
        }
        /* An all-zero key is what an unused MeshCore slot holds, so it is not
         * a channel - it is the thing the receive-path guard exists to keep
         * out (protocols/meshcore/README.md, known debt 5). */
        if (all_zero) {
            snprintf(err, ERR_SIZE, "%s: channel %u has an all-zero key", path, (unsigned)i);
            return -1;
        }
        if (c.name[0] == '\0') {
            snprintf(err, ERR_SIZE, "%s: channel %u has no name", path, (unsigned)i);
            return -1;
        }
        for (uint16_t k = 0; k < i; k++) {
            if (cs.channels[k].slot == c.slot) {
                snprintf(err, ERR_SIZE, "%s: channels %u and %u are both in slot %d",
                         path, (unsigned)k, (unsigned)i, c.slot);
                return -1;
            }
            /* The key IS the channel, so two records holding one key are two
             * names for one thing: the second could never be routed to, and
             * MeshCore would report whichever the scan reached first. */
            if (cs.channels[k].key_len == c.key_len &&
                memcmp(cs.channels[k].secret, c.secret, sizeof(c.secret)) == 0) {
                snprintf(err, ERR_SIZE, "%s: channels %u and %u hold the same key",
                         path, (unsigned)k, (unsigned)i);
                return -1;
            }
        }
    }
    cs.count = count;
    return 1;
}

bool channelsSave(const ChannelState& cs, const char* dir, char* err)
{
    char path[256];
    static uint8_t buf[kChanHeaderSize + kChanRecordSize * MAX_CHANNELS];
    int count = cs.count;
    size_t len;

    if (count < 0) {
        count = 0;
    }
    if (count > MAX_CHANNELS) {
        count = MAX_CHANNELS;
    }
    len = kChanHeaderSize + kChanRecordSize * (size_t)count;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, kChanMagic, sizeof(kChanMagic));
    put16(&buf[4], kChanVersion);
    put16(&buf[6], 0); /* flags, reserved */
    put16(&buf[8], (uint16_t)count);
    put16(&buf[10], 0); /* reserved */

    for (int i = 0; i < count; i++) {
        uint8_t* r = &buf[kChanHeaderSize + kChanRecordSize * (size_t)i];
        const ChannelRecord& c = cs.channels[i];

        r[0] = (uint8_t)c.slot;
        r[1] = (uint8_t)c.key_len;
        r[2] = 0;
        r[3] = 0;
        memcpy(&r[4], c.name, sizeof(c.name));
        r[4 + sizeof(c.name) - 1] = '\0';
        memcpy(&r[36], c.secret, sizeof(c.secret));
    }

    joinPath(path, sizeof(path), dir, kChannelsName);
    /* 0600, like identity.id: this file is key material. The whole buffer is
     * zeroed above rather than only the used part, so a table that shrinks
     * does not leave a removed channel's key in the tail of the file. */
    return writeWhole(path, buf, len, 0600, err);
}

/* ---- settings.v1 -----------------------------------------------------------
 *
 * Text, one key=value a line, so an operator can read it with cat. Small and
 * not key material, but written the same way as everything else here: a
 * temporary, fsync, rename, the directory flushed. */
static const char kSettingsName[] = "settings.v1";
static const size_t kSettingsMax = 1024;

int settingsLoad(Settings& s, const char* dir, char* err)
{
    char path[256];
    uint8_t buf[kSettingsMax + 1];
    long n;
    size_t at = 0;

    s = Settings();
    joinPath(path, sizeof(path), dir, kSettingsName);
    n = readWhole(path, buf, kSettingsMax, err);
    if (n == -2) {
        return 1;
    }
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    while (at < (size_t)n) {
        char line[128];
        size_t len = 0;
        char* eq;

        while (at < (size_t)n && buf[at] != '\n' && len + 1 < sizeof(line)) {
            line[len++] = (char)buf[at++];
        }
        while (at < (size_t)n && buf[at] != '\n') {
            at++; /* a line longer than any value: the rest of it is skipped */
        }
        at++;
        line[len] = '\0';
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (strcmp(line, "path_hash_bytes") == 0) {
            const char* v = eq + 1;

            if (v[0] < '1' || v[0] > '3' || v[1] != '\0') {
                snprintf(err, ERR_SIZE, "%s: path_hash_bytes must be 1, 2 or 3, not \"%s\"",
                         path, v);
                s = Settings();
                return -1;
            }
            s.path_hash_bytes = v[0] - '0';
        } else if (strcmp(line, "renamed_over") == 0) {
            const char* v = eq + 1;
            size_t k = 0;

            /* Sixteen lower-case hex characters or it is not a mark. One
             * that is not is left out rather than refused: without it the
             * configured name wins, which is what an operator who edited
             * this file by hand would expect, and the path hash size on the
             * next line is still worth reading. */
            while (v[k] && ((v[k] >= '0' && v[k] <= '9') || (v[k] >= 'a' && v[k] <= 'f'))) {
                k++;
            }
            if (k == (size_t)RENAMED_OVER_HEX && v[k] == '\0') {
                memcpy(s.renamed_over, v, k + 1);
            } else {
                s.renamed_over[0] = '\0';
            }
        }
    }
    return 0;
}

bool settingsSave(const Settings& s, const char* dir, char* err)
{
    char path[256];
    char text[128];
    int len;

    if (s.path_hash_bytes < 1 || s.path_hash_bytes > 3) {
        snprintf(err, ERR_SIZE, "path_hash_bytes %d is not 1, 2 or 3", s.path_hash_bytes);
        return false;
    }
    len = snprintf(text, sizeof(text), "path_hash_bytes=%d\n", s.path_hash_bytes);
    if (s.renamed_over[0]) {
        len += snprintf(text + len, sizeof(text) - (size_t)len, "renamed_over=%.*s\n",
                        RENAMED_OVER_HEX, s.renamed_over);
    }
    joinPath(path, sizeof(path), dir, kSettingsName);
    return writeWhole(path, (const uint8_t*)text, (size_t)len, 0600, err);
}

}  // namespace mcdstore
