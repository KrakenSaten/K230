/*
 * meshcored: persistence. See mesh_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

/* state.v1, little-endian, fixed widths. */
const uint8_t kMagic[4] = { 'M', 'C', 'D', 'S' };
const uint16_t kVersion = 1;
const size_t kHeaderSize = 44;
const size_t kRecordSize = 148;

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
    return true;
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

}  // namespace mcdstore
