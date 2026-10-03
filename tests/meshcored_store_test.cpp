/*
 * meshcored: persistence (services/meshcored/mesh_store.cpp).
 *
 * The identity is the part of this service that cannot be reconstructed. A
 * node's peers know it by its public key; lose the private half and every
 * one of them holds a contact for a node that no longer exists, and a new
 * key makes this machine a stranger to the network it was part of. So the
 * cases here are mostly about refusing to do damage: a file that is there
 * and wrong stops the service rather than being replaced, a file that
 * already exists is never overwritten, and a key pair that does not agree
 * with itself is refused before it signs anything.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mesh_store.h"

#include <helpers/AdvertDataHelpers.h>

static int failed;
static int checks;

static void check(const char* name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    failed += !ok;
}

static char g_dir[256];
static char g_err[mcdstore::ERR_SIZE];

static void joinp(char* dest, size_t n, const char* name)
{
    snprintf(dest, n, "%s/%s", g_dir, name);
}

static void removeFile(const char* name)
{
    char p[512];

    joinp(p, sizeof(p), name);
    unlink(p);
}

static bool writeRaw(const char* name, const uint8_t* bytes, size_t len, mode_t mode)
{
    char p[512];
    FILE* f;

    joinp(p, sizeof(p), name);
    unlink(p);
    f = fopen(p, "wb");
    if (!f) {
        return false;
    }
    if (len > 0 && fwrite(bytes, 1, len, f) != len) {
        fclose(f);
        return false;
    }
    fclose(f);
    chmod(p, mode);
    return true;
}

static long fileSize(const char* name)
{
    char p[512];
    struct stat st;

    joinp(p, sizeof(p), name);
    if (stat(p, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static mode_t fileMode(const char* name)
{
    char p[512];
    struct stat st;

    joinp(p, sizeof(p), name);
    if (stat(p, &st) != 0) {
        return 0;
    }
    return st.st_mode & 07777;
}

/* ---- the identity ------------------------------------------------------- */

static void test_identity_generate_and_reload(void)
{
    mesh::LocalIdentity a;
    mesh::LocalIdentity b;

    removeFile("identity.id");
    check("an absent identity file is reported as absent, not as an error",
          mcdstore::identityLoad(a, g_dir, g_err) == 0);

    check("an identity can be generated", mcdstore::identityCreate(a, g_err));
    check("it is not a reserved-prefix key", a.pub_key[0] != 0x00 && a.pub_key[0] != 0xFF);
    check("it can be saved", mcdstore::identitySave(a, g_dir, g_err));
    check("the file is 96 bytes, a MeshCore .id", fileSize("identity.id") == 96);
    /* It holds a private key. Nothing else on the device may read it. */
    check("and is mode 0600", fileMode("identity.id") == 0600);

    check("it loads again", mcdstore::identityLoad(b, g_dir, g_err) == 1);
    check("as the same public key", memcmp(a.pub_key, b.pub_key, PUB_KEY_SIZE) == 0);

    /* The private half is what makes it the same node, and it is not
     * directly readable from outside the class - so it is checked by use: a
     * signature made by the reloaded identity must verify against the key
     * the first one had. */
    {
        const char* msg = "the same node after a restart";
        uint8_t sig[SIGNATURE_SIZE];

        b.sign(sig, (const uint8_t*)msg, (int)strlen(msg));
        check("and with the same private key, proved by a signature it verifies",
              a.verify(sig, (const uint8_t*)msg, (int)strlen(msg)));
    }

    /* Generated once, not on every start. */
    {
        mesh::LocalIdentity c;

        check("a second start loads rather than generating",
              mcdstore::identityLoad(c, g_dir, g_err) == 1 &&
              memcmp(c.pub_key, a.pub_key, PUB_KEY_SIZE) == 0);
    }

    /* And saving over an existing one is refused: O_EXCL, deliberately. */
    {
        mesh::LocalIdentity other;

        check("a fresh identity for the overwrite case",
              mcdstore::identityCreate(other, g_err));
        check("saving over an existing identity is refused",
              !mcdstore::identitySave(other, g_dir, g_err));
        check("and the file still holds the original key",
              mcdstore::identityLoad(b, g_dir, g_err) == 1 &&
              memcmp(b.pub_key, a.pub_key, PUB_KEY_SIZE) == 0);
    }
}

static void test_identity_corruption(void)
{
    mesh::LocalIdentity id;
    uint8_t good[96];

    /* Keep a good copy to restore between cases. */
    {
        char p[512];
        FILE* f;

        joinp(p, sizeof(p), "identity.id");
        f = fopen(p, "rb");
        check("the good identity can be read back as bytes",
              f != NULL && fread(good, 1, sizeof(good), f) == sizeof(good));
        if (f) {
            fclose(f);
        }
    }

    {
        uint8_t zeros[96] = { 0 };

        writeRaw("identity.id", zeros, sizeof(zeros), 0600);
        check("96 zero bytes are refused, not used",
              mcdstore::identityLoad(id, g_dir, g_err) == -1);
        check("and the reason names the key pair", strstr(g_err, "key pair") != NULL);
    }
    {
        writeRaw("identity.id", good, 95, 0600);
        check("a file one byte short is refused",
              mcdstore::identityLoad(id, g_dir, g_err) == -1);
    }
    {
        uint8_t longer[97];

        memcpy(longer, good, 96);
        longer[96] = 0x41;
        writeRaw("identity.id", longer, sizeof(longer), 0600);
        check("a file one byte long is refused",
              mcdstore::identityLoad(id, g_dir, g_err) == -1);
    }
    {
        writeRaw("identity.id", good, 0, 0600);
        check("an empty file is refused", mcdstore::identityLoad(id, g_dir, g_err) == -1);
    }
    {
        /* The subtle one: a real private key with somebody else's public
         * key in front of it. Both halves are individually valid, and
         * MeshCore would sign as one node while being named as the other. */
        mesh::LocalIdentity other;
        uint8_t mixed[96];

        mcdstore::identityCreate(other, g_err);
        memcpy(mixed, other.pub_key, PUB_KEY_SIZE);
        memcpy(&mixed[PUB_KEY_SIZE], &good[PUB_KEY_SIZE], PRV_KEY_SIZE);
        writeRaw("identity.id", mixed, sizeof(mixed), 0600);
        check("a public key that its private key does not derive is refused",
              mcdstore::identityLoad(id, g_dir, g_err) == -1);
        check("and the reason says so", strstr(g_err, "derives") != NULL);
    }
    {
        /* One flipped bit in the private half. */
        uint8_t bent[96];

        memcpy(bent, good, sizeof(bent));
        bent[PUB_KEY_SIZE + 3] ^= 0x01;
        writeRaw("identity.id", bent, sizeof(bent), 0600);
        check("one flipped bit in the private key is refused",
              mcdstore::identityLoad(id, g_dir, g_err) == -1);
    }

    writeRaw("identity.id", good, sizeof(good), 0600);
    check("the good file loads again once restored",
          mcdstore::identityLoad(id, g_dir, g_err) == 1);
}

/* ---- the node state ----------------------------------------------------- */

static ContactInfo makeNode(uint8_t first, const char* name, uint8_t path_len)
{
    ContactInfo c = ContactInfo();

    for (int i = 0; i < PUB_KEY_SIZE; i++) {
        c.id.pub_key[i] = (uint8_t)(first + i);
    }
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.type = ADV_TYPE_CHAT;
    c.flags = 0;
    c.out_path_len = path_len;
    if (path_len != OUT_PATH_UNKNOWN) {
        for (int i = 0; i < MAX_PATH_SIZE; i++) {
            c.out_path[i] = (uint8_t)(0x10 + i);
        }
    }
    c.last_advert_timestamp = 1789000000u + first;
    c.lastmod = 1789000100u + first;
    c.gps_lat = -123456;
    c.gps_lon = 654321;
    return c;
}

static void test_state_round_trip(void)
{
    mcdstore::NodeState out;
    mcdstore::NodeState in;

    removeFile("state.v1");
    check("an absent state file is reported as absent",
          mcdstore::stateLoad(in, g_dir, g_err) == 0);
    check("and leaves nothing behind", in.count == 0 && in.name[0] == '\0');

    out = mcdstore::NodeState();
    snprintf(out.name, sizeof(out.name), "K230-A");
    out.nodes[0] = makeNode(0x11, "HYTTA", 3);
    out.nodes[1] = makeNode(0x22, "RPT-NORD", OUT_PATH_UNKNOWN);
    out.count = 2;

    check("the state saves", mcdstore::stateSave(out, g_dir, g_err));
    check("and is mode 0600", fileMode("state.v1") == 0600);
    check("its length is the header plus its records", fileSize("state.v1") == 44 + 2 * 148);

    check("it loads", mcdstore::stateLoad(in, g_dir, g_err) == 1);
    check("with the node name", strcmp(in.name, "K230-A") == 0);
    check("and both nodes", in.count == 2);
    check("the public key survives",
          memcmp(in.nodes[0].id.pub_key, out.nodes[0].id.pub_key, PUB_KEY_SIZE) == 0);
    check("the name survives", strcmp(in.nodes[0].name, "HYTTA") == 0);
    check("the advert type survives", in.nodes[0].type == ADV_TYPE_CHAT);
    check("the path survives",
          in.nodes[0].out_path_len == 3 &&
          memcmp(in.nodes[0].out_path, out.nodes[0].out_path, MAX_PATH_SIZE) == 0);
    check("a node with no path keeps having none",
          in.nodes[1].out_path_len == OUT_PATH_UNKNOWN);
    check("the advert timestamp survives",
          in.nodes[1].last_advert_timestamp == out.nodes[1].last_advert_timestamp);
    check("the position survives, sign and all",
          in.nodes[1].gps_lat == -123456 && in.nodes[1].gps_lon == 654321);
    /* The shared secret is derived, never stored: a file that carried one
     * would be a file carrying key material for every contact. */
    check("the shared secret is not carried over, it is recomputed",
          !in.nodes[0].shared_secret_valid);

    /* Written whole or not at all: saving again replaces the file. */
    out.count = 1;
    check("saving fewer nodes replaces the file", mcdstore::stateSave(out, g_dir, g_err));
    check("and the new length is exact", fileSize("state.v1") == 44 + 148);
    check("and it loads as one node",
          mcdstore::stateLoad(in, g_dir, g_err) == 1 && in.count == 1);

    out.count = 0;
    check("an empty table saves", mcdstore::stateSave(out, g_dir, g_err));
    check("as a header alone", fileSize("state.v1") == 44);
    check("and loads as empty",
          mcdstore::stateLoad(in, g_dir, g_err) == 1 && in.count == 0);
}

/* ---- the node limit, at its edge -----------------------------------------
 *
 * state.v1 carries every contact MeshCore's table can hold, 1000. A full
 * table must come back whole and in order, and a well-formed file one node
 * longer - which only a build with a bigger table could have written - must
 * be refused by its count rather than read as far as fits. */
static void test_state_capacity(void)
{
    mcdstore::NodeState* out = new mcdstore::NodeState();
    mcdstore::NodeState* in = new mcdstore::NodeState();
    const int limit = mcdstore::MAX_NODES;
    const size_t full = 44 + 148 * (size_t)limit;
    uint8_t* raw = (uint8_t*)malloc(full + 148);

    check("the persisted limit is MeshCore's table, 1000",
          limit == 1000 && limit == MAX_CONTACTS);
    check("and a full state.v1 is 148,044 bytes", full == 148044);
    snprintf(out->name, sizeof(out->name), "K230-A");
    for (int i = 0; i < limit; i++) {
        char name[16];

        snprintf(name, sizeof(name), "N%03d", i);
        out->nodes[i] = makeNode((uint8_t)i, name, 0);
        /* The first byte repeats past 256; the second keeps every key its
         * own, which the loader insists on. */
        out->nodes[i].id.pub_key[1] = (uint8_t)(i >> 8);
    }

    out->count = limit - 1;
    check("999 nodes save", mcdstore::stateSave(*out, g_dir, g_err));
    check("999 nodes load as 999",
          mcdstore::stateLoad(*in, g_dir, g_err) == 1 && in->count == limit - 1);

    out->count = limit;
    check("1000 nodes save", mcdstore::stateSave(*out, g_dir, g_err));
    check("as exactly 1000 records", fileSize("state.v1") == (long)full);
    check("1000 nodes load as 1000",
          mcdstore::stateLoad(*in, g_dir, g_err) == 1 && in->count == limit);
    {
        bool same = true;

        for (int i = 0; i < limit; i++) {
            if (memcmp(in->nodes[i].id.pub_key, out->nodes[i].id.pub_key, PUB_KEY_SIZE) != 0 ||
                strcmp(in->nodes[i].name, out->nodes[i].name) != 0 ||
                in->nodes[i].lastmod != out->nodes[i].lastmod) {
                same = false;
            }
        }
        check("every one of them, in order", same);
    }

    /* 1001: the full file plus one well-formed record with a key of its own,
     * and a count that says so. */
    {
        char p[512];
        FILE* f;
        bool read_ok;

        joinp(p, sizeof(p), "state.v1");
        f = fopen(p, "rb");
        read_ok = raw && f && fread(raw, 1, full, f) == full;
        if (f) {
            fclose(f);
        }
        check("the full file can be read as bytes", read_ok);
        if (read_ok) {
            memcpy(&raw[full], &raw[44], 148);
            raw[full + 1] = 0xEE;  /* no other key starts 00 EE */
            raw[40] = (uint8_t)((limit + 1) & 0xff);  /* 1001 */
            raw[41] = (uint8_t)((limit + 1) >> 8);
            writeRaw("state.v1", raw, full + 148, 0600);
            check("a well-formed file of 1001 nodes is refused",
                  mcdstore::stateLoad(*in, g_dir, g_err) == -1);
            /* Before its count is even read: the file is longer than 1000
             * records can be. A count of 1001 in a short file is refused by
             * the count itself - see test_state_corruption. */
            check("as longer than 1000 nodes can be", strstr(g_err, "longer than") != NULL);
        }
    }
    removeFile("state.v1");
    free(raw);
    delete in;
    delete out;
}

static void test_state_corruption(void)
{
    mcdstore::NodeState in;
    mcdstore::NodeState out = mcdstore::NodeState();
    uint8_t buf[44 + 148 * 2];

    snprintf(out.name, sizeof(out.name), "K230-A");
    out.nodes[0] = makeNode(0x33, "PEER", 1);
    out.count = 1;
    check("a good file to mutate", mcdstore::stateSave(out, g_dir, g_err));
    {
        char p[512];
        FILE* f;

        joinp(p, sizeof(p), "state.v1");
        f = fopen(p, "rb");
        check("which can be read as bytes",
              f != NULL && fread(buf, 1, 44 + 148, f) == 44 + 148);
        if (f) {
            fclose(f);
        }
    }

    {
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        bad[0] = 'X';
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a wrong magic is refused", mcdstore::stateLoad(in, g_dir, g_err) == -1);
        check("and the reason names the magic", strstr(g_err, "magic") != NULL);
    }
    {
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        bad[4] = 2;  /* version 2 */
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a version this build does not read is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
        check("and says which version it found", strstr(g_err, "version 2") != NULL);
    }
    {
        /* Truncated in the middle of a record. This is the case a
         * "at least N bytes" check would let through. */
        writeRaw("state.v1", buf, 44 + 100, 0600);
        check("a file truncated inside a record is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
    }
    {
        /* A count that claims more records than the file holds. */
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        bad[40] = 5;
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a count larger than the file is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
    }
    {
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        /* One more than the limit, little-endian. */
        bad[40] = (uint8_t)((mcdstore::MAX_NODES + 1) & 0xff);
        bad[41] = (uint8_t)((mcdstore::MAX_NODES + 1) >> 8);
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a count beyond the node limit is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
        check("and names the limit", strstr(g_err, "limit") != NULL);
    }
    {
        /* A path length MeshCore's own encoding rejects. Left unchecked it
         * would reach Packet::copyPath as a byte count. */
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        /* Hash size 4 is reserved and isValidPathLen refuses it. Ten hops,
         * not 63, because 0xFF is OUT_PATH_UNKNOWN and would be read - quite
         * correctly - as "this node has no path". */
        bad[44 + 66] = 0xC0 | 10;
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("an impossible path length is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
        check("and says so", strstr(g_err, "path length") != NULL);
    }
    {
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        bad[44 + 64] = ADV_TYPE_NONE;
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a node with no advert type is refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
    }
    {
        /* Two records with the same public key: a table a lookup cannot
         * tell apart. */
        uint8_t bad[44 + 148 * 2];

        memcpy(bad, buf, 44 + 148);
        memcpy(&bad[44 + 148], &buf[44], 148);
        bad[40] = 2;
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("two nodes with the same public key are refused",
              mcdstore::stateLoad(in, g_dir, g_err) == -1);
        check("and the reason says why", strstr(g_err, "same public key") != NULL);
    }
    {
        /* A name with no terminator must not be read past its field. */
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        memset(&bad[44 + 32], 'A', 32);
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("a name that fills its field loads", mcdstore::stateLoad(in, g_dir, g_err) == 1);
        check("and is terminated at the field's end", strlen(in.nodes[0].name) == 31);
    }
    {
        /* The same for the node's own name in the header. */
        uint8_t bad[44 + 148];

        memcpy(bad, buf, sizeof(bad));
        memset(&bad[8], 'B', 32);
        writeRaw("state.v1", bad, sizeof(bad), 0600);
        check("an unterminated node name loads", mcdstore::stateLoad(in, g_dir, g_err) == 1);
        check("terminated at its field's end", strlen(in.name) == 31);
    }

    writeRaw("state.v1", buf, 44 + 148, 0600);
    check("the good file loads again once restored",
          mcdstore::stateLoad(in, g_dir, g_err) == 1 && in.count == 1);
}

/* ---- durability ---------------------------------------------------------
 *
 * fsync() on a file flushes its contents and says nothing about the entry
 * that names it. A power cut in between can leave a complete file nothing can
 * reach - for identity.id, a node that silently becomes somebody else on its
 * next start. The hooked build lets that syscall fail on request, because a
 * working filesystem will not do it for us, and an error path nothing has
 * executed is a guess.
 */
static void test_directory_durability(void)
{
    mesh::LocalIdentity id;
    mcdstore::NodeState st;

    removeFile("identity.id");
    check("an identity to save", mcdstore::identityCreate(id, g_err));

    mcdstore::failNextDirSyncForTest(1);
    check("a directory flush that fails makes the save fail",
          !mcdstore::identitySave(id, g_dir, g_err));
    check("and says which directory", strstr(g_err, g_dir) != NULL);
    /* And it does not leave a file behind that a later start would load as a
     * persisted identity nobody could promise was persisted. */
    check("and leaves no half-promised identity file", fileSize("identity.id") == -1);
    check("so a reload reports no identity at all",
          mcdstore::identityLoad(id, g_dir, g_err) == 0);

    /* Without the hook it succeeds, which is what proves the hook was the
     * thing that failed it rather than something else being wrong. */
    check("the same save succeeds when the flush works",
          mcdstore::identityCreate(id, g_err) && mcdstore::identitySave(id, g_dir, g_err));
    check("and the file is there", fileSize("identity.id") == 96);

    /* state.v1 goes through the same flush, after its rename. */
    st = mcdstore::NodeState();
    snprintf(st.name, sizeof(st.name), "K230-A");
    st.count = 0;
    removeFile("state.v1");
    mcdstore::failNextDirSyncForTest(1);
    check("a failed flush makes the node-state save fail",
          !mcdstore::stateSave(st, g_dir, g_err));
    check("the same save succeeds when the flush works",
          mcdstore::stateSave(st, g_dir, g_err));
    check("and the file is there", fileSize("state.v1") == 44);
}

/* ---- a state file this build will not read ------------------------------
 *
 * The identity is fatal and stays fatal. state.v1 is a cache the mesh refills,
 * so it is moved aside rather than allowed to take the node off the air - and
 * moved, not rewritten: it is the only evidence of whatever went wrong.
 */
static void test_quarantine(void)
{
    mcdstore::NodeState st;
    uint8_t rubbish[64];
    char kept[288] = "";
    char err[mcdstore::ERR_SIZE] = "";

    memset(rubbish, 0x41, sizeof(rubbish));
    writeRaw("state.v1", rubbish, sizeof(rubbish), 0600);
    check("a state file this build will not read is refused",
          mcdstore::stateLoad(st, g_dir, err) == -1);

    check("it can be moved aside",
          mcdstore::stateQuarantine(g_dir, kept, sizeof(kept), err));
    check("the name it was kept under is reported", strstr(kept, "corrupt") != NULL);
    check("and state.v1 is gone", fileSize("state.v1") == -1);
    {
        struct stat sb;

        check("the original bytes are still there, unchanged",
              stat(kept, &sb) == 0 && sb.st_size == (off_t)sizeof(rubbish));
    }
    check("so a load now reports no file rather than a fault",
          mcdstore::stateLoad(st, g_dir, err) == 0);

    /* A second fault does not overwrite the first one's evidence. */
    {
        char kept2[288] = "";

        writeRaw("state.v1", rubbish, sizeof(rubbish), 0600);
        check("a second bad file can be moved aside too",
              mcdstore::stateQuarantine(g_dir, kept2, sizeof(kept2), err));
        check("under a different name", strcmp(kept, kept2) != 0);
        {
            struct stat sb;

            check("and the first one is still there",
                  stat(kept, &sb) == 0 && sb.st_size == (off_t)sizeof(rubbish));
        }
    }

    check("moving aside a file that is not there fails rather than pretending",
          !mcdstore::stateQuarantine(g_dir, kept, sizeof(kept), err));
}

static void test_dir_rules(void)
{
    char err[mcdstore::ERR_SIZE];
    char nested[512];
    struct stat st;

    snprintf(nested, sizeof(nested), "%s/deep/deeper", g_dir);
    check("a nested state directory is created", mcdstore::ensureDir(nested, err));
    check("and it exists", stat(nested, &st) == 0 && S_ISDIR(st.st_mode));
    /* It holds a private key, so nothing else on the device may look in. */
    check("with mode 0700", (st.st_mode & 07777) == 0700);
    check("creating it twice is not an error", mcdstore::ensureDir(nested, err));

    check("a relative state directory is refused", !mcdstore::ensureDir("relative/path", err));
    check("NULL is refused", !mcdstore::ensureDir(NULL, err));
    {
        char toolong[400];

        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[0] = '/';
        toolong[sizeof(toolong) - 1] = '\0';
        check("an over-long state directory is refused, not truncated",
              !mcdstore::ensureDir(toolong, err));
    }
}

/* ---- channels.v1 --------------------------------------------------------
 *
 * The same shape as state.v1 and the same rules, with one difference that
 * runs through every case below: this file holds KEY MATERIAL. A channel key
 * is the whole of a channel's identity and its confidentiality, so the mode
 * matters, a shrinking table must not leave an old key in the tail of the
 * file, and a record that is not exactly right is a refusal rather than
 * something to patch up.
 */

static mcdstore::ChannelRecord makeChannel(int slot, const char* name, uint8_t seed,
                                           int key_len)
{
    mcdstore::ChannelRecord r = mcdstore::ChannelRecord();

    r.slot = slot;
    r.key_len = key_len;
    snprintf(r.name, sizeof(r.name), "%s", name);
    memset(r.secret, 0, sizeof(r.secret));
    for (int i = 0; i < key_len; i++) {
        r.secret[i] = (uint8_t)(seed + i);
    }
    return r;
}

static void test_channels_round_trip(void)
{
    mcdstore::ChannelState in;
    mcdstore::ChannelState out = mcdstore::ChannelState();

    removeFile("channels.v1");
    check("no channels.v1 is not a fault",
          mcdstore::channelsLoad(in, g_dir, g_err) == 0 && in.count == 0);

    out.channels[0] = makeChannel(0, "SITE", 0x11, 32);
    out.channels[1] = makeChannel(3, "OPS", 0x71, 16);
    out.count = 2;
    check("the channels save", mcdstore::channelsSave(out, g_dir, g_err));
    /* 0600, like identity.id. This file is a secret, and the temporary it is
     * written through carries the mode from the start. */
    check("the file is not readable by anyone else", fileMode("channels.v1") == 0600);
    check("and is exactly the size two records need", fileSize("channels.v1") == 12 + 68 * 2);

    check("it loads", mcdstore::channelsLoad(in, g_dir, g_err) == 1);
    check("with both channels", in.count == 2);
    check("the first keeps its slot", in.channels[0].slot == 0);
    check("its name", strcmp(in.channels[0].name, "SITE") == 0);
    check("its key length", in.channels[0].key_len == 32);
    check("and its key, byte for byte",
          memcmp(in.channels[0].secret, out.channels[0].secret, 32) == 0);
    /* The slot is the identity, and it is NOT the position in the file: the
     * second record is in slot 3 because a channel between them was left. */
    check("the second keeps the slot it was in, not the index it was written at",
          in.channels[1].slot == 3);
    check("its 128-bit key is zero-padded above 16 bytes",
          in.channels[1].key_len == 16 && in.channels[1].secret[16] == 0 &&
              in.channels[1].secret[31] == 0);

    /* A table that shrinks must not leave the removed channel's key behind
     * in the tail of the file: the record it was in is gone, but the bytes
     * would still be a key somebody could read. */
    {
        char p[512];
        FILE* f;
        uint8_t raw[12 + 68 * 2];
        long n;
        bool found = false;

        out.count = 1;
        check("saving one channel replaces the file",
              mcdstore::channelsSave(out, g_dir, g_err));
        check("the file is one record shorter", fileSize("channels.v1") == 12 + 68);
        joinp(p, sizeof(p), "channels.v1");
        f = fopen(p, "rb");
        check("and can be read as bytes", f != NULL);
        if (f) {
            n = (long)fread(raw, 1, sizeof(raw), f);
            fclose(f);
            for (long i = 0; i + 16 <= n; i++) {
                if (memcmp(&raw[i], out.channels[1].secret, 16) == 0) {
                    found = true;
                }
            }
            check("the removed channel's key is nowhere in it", !found);
        }
        check("and only one channel loads",
              mcdstore::channelsLoad(in, g_dir, g_err) == 1 && in.count == 1);
    }
}

static void test_channels_corruption(void)
{
    mcdstore::ChannelState in;
    mcdstore::ChannelState out = mcdstore::ChannelState();
    uint8_t buf[12 + 68 * 2];
    const size_t one = 12 + 68;

    out.channels[0] = makeChannel(0, "SITE", 0x11, 32);
    out.count = 1;
    check("a good channels file to mutate", mcdstore::channelsSave(out, g_dir, g_err));
    {
        char p[512];
        FILE* f;

        joinp(p, sizeof(p), "channels.v1");
        f = fopen(p, "rb");
        check("which can be read as bytes", f != NULL && fread(buf, 1, one, f) == one);
        if (f) {
            fclose(f);
        }
    }

    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[0] = 'X';
        writeRaw("channels.v1", bad, one, 0600);
        check("a wrong magic is refused", mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and the reason names the magic", strstr(g_err, "magic") != NULL);
    }
    {
        /* A state.v1 renamed onto channels.v1. The two files have different
         * magic precisely so this is a refusal rather than a table of
         * nonsense read as keys. */
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        memcpy(bad, "MCDS", 4);
        writeRaw("channels.v1", bad, one, 0600);
        check("a node table in the channel file's place is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[4] = 7;
        writeRaw("channels.v1", bad, one, 0600);
        check("a version this build does not read is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says which version it found", strstr(g_err, "version 7") != NULL);
    }
    {
        /* Truncated inside a record: the case an "at least N bytes" check
         * would let through, loading a key that is half somebody else's. */
        writeRaw("channels.v1", buf, one - 20, 0600);
        check("a file truncated inside a record is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[8] = 4;  /* claims four channels */
        writeRaw("channels.v1", bad, one, 0600);
        check("a count larger than the file is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[8] = 99;
        writeRaw("channels.v1", bad, one, 0600);
        check("a count beyond the table is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[12] = 99;  /* slot */
        writeRaw("channels.v1", bad, one, 0600);
        check("a slot outside the table is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says so", strstr(g_err, "slot") != NULL);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[13] = 24;  /* key_len */
        writeRaw("channels.v1", bad, one, 0600);
        check("a key length that is neither 16 nor 32 is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        /* Claims a 128-bit key but carries bytes above 16. Those bytes are
         * part of the 32-byte HMAC key MeshCore derives the MAC with, so a
         * record like this would MAC differently from the peer that wrote
         * it - a channel that looks joined and silently works for nobody. */
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[13] = 16;
        writeRaw("channels.v1", bad, one, 0600);
        check("a 128-bit key that is not zero-padded is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says why", strstr(g_err, "zero-padded") != NULL);
    }
    {
        /* A 256-bit key whose upper half is all zero: the key
         * mcd_runtime_channel_add refuses as ambiguous, because MeshCore's
         * setChannel() reads it as a 128-bit key and hashes it over 16 bytes
         * while a peer that added it through addChannel() would hash it over
         * 32. A file must not be able to reintroduce a key the API will not
         * take - the node would report key_bits 256 while deriving the
         * 128-bit hash, and half its peers could not reach it. */
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        bad[13] = 32;               /* key_len says 256-bit */
        memset(&bad[48 + 16], 0, 16); /* ...and the upper half is zero */
        writeRaw("channels.v1", bad, one, 0600);
        check("a 256-bit key with an all-zero upper half is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says which half", strstr(g_err, "upper half") != NULL);
        /* The two halves of the same boundary: 16 needs the top zeroed, 32
         * needs it not zeroed, and nothing is accepted in between. */
        bad[13] = 16;
        writeRaw("channels.v1", bad, one, 0600);
        check("the same bytes as a 128-bit key are fine",
              mcdstore::channelsLoad(in, g_dir, g_err) == 1);
        check("and load as 16 bytes", in.channels[0].key_len == 16);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        memset(&bad[48], 0, 32);  /* the key: header(12) + record offset 36 */
        writeRaw("channels.v1", bad, one, 0600);
        /* An all-zero key is what an unused MeshCore slot holds, so it is
         * not a channel - it is the thing the receive-path guard exists to
         * keep out. */
        check("an all-zero key is refused", mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says so", strstr(g_err, "all-zero") != NULL);
    }
    {
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        memset(&bad[16], 0, 32);  /* the name: header(12) + record offset 4 */
        writeRaw("channels.v1", bad, one, 0600);
        check("a channel with no name is refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    }
    {
        /* Two records in one slot, and two records holding one key. Both
         * make the table ambiguous in a way nothing downstream can resolve:
         * the slot is what a client names a channel by, and the key is what
         * routes one. */
        uint8_t bad[12 + 68 * 2];

        memcpy(bad, buf, one);
        memcpy(&bad[one], &buf[12], 68);
        bad[8] = 2;
        writeRaw("channels.v1", bad, sizeof(bad), 0600);
        check("two channels in one slot are refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says which two", strstr(g_err, "slot") != NULL);

        bad[one + 0] = 1;  /* a different slot, same key */
        writeRaw("channels.v1", bad, sizeof(bad), 0600);
        check("two channels with the same key are refused",
              mcdstore::channelsLoad(in, g_dir, g_err) == -1);
        check("and says so", strstr(g_err, "same key") != NULL);
    }
    {
        /* A name with no terminator in it: the loader forces one rather
         * than reading off the end of the record. */
        uint8_t bad[12 + 68];

        memcpy(bad, buf, one);
        memset(&bad[16], 'Z', 32);
        writeRaw("channels.v1", bad, one, 0600);
        check("a name filling its field loads", mcdstore::channelsLoad(in, g_dir, g_err) == 1);
        check("and is terminated at the field's end",
              strlen(in.channels[0].name) == 31);
    }

    /* Put a good file back. */
    writeRaw("channels.v1", buf, one, 0600);
    check("the good file loads again", mcdstore::channelsLoad(in, g_dir, g_err) == 1);
}

static void test_channels_quarantine(void)
{
    mcdstore::ChannelState in;
    char kept[288] = "";
    char err[mcdstore::ERR_SIZE] = "";
    struct stat sb;
    char p[512];
    uint8_t junk[20];

    memset(junk, 0xEE, sizeof(junk));
    check("an unreadable channels.v1", writeRaw("channels.v1", junk, sizeof(junk), 0600));
    check("which is indeed refused", mcdstore::channelsLoad(in, g_dir, g_err) == -1);
    check("it can be moved aside",
          mcdstore::channelsQuarantine(g_dir, kept, sizeof(kept), err));
    check("the name it was kept under is reported", strstr(kept, "channels.v1.corrupt") != NULL);
    /* Renamed, never rewritten: the file is the only evidence of the fault,
     * and it still holds whatever key material was in it, so it keeps its
     * mode too. */
    check("the file is still there", stat(kept, &sb) == 0);
    check("with its bytes", sb.st_size == (off_t)sizeof(junk));
    check("and its mode", (sb.st_mode & 07777) == 0600);
    joinp(p, sizeof(p), "channels.v1");
    check("and is out of the way", stat(p, &sb) != 0);
    check("so a load now finds nothing rather than failing",
          mcdstore::channelsLoad(in, g_dir, g_err) == 0);

    /* A second fault does not overwrite the first. */
    check("a second unreadable file", writeRaw("channels.v1", junk, sizeof(junk), 0600));
    check("is kept under its own name",
          mcdstore::channelsQuarantine(g_dir, kept, sizeof(kept), err) &&
              strstr(kept, ".corrupt.1") != NULL);
    /* And quarantining the node table and the channels are separate acts:
     * one file being moved aside says nothing about the other. */
    check("quarantining a channels file that is not there is refused",
          !mcdstore::channelsQuarantine(g_dir, kept, sizeof(kept), err));
}

/* ---- settings.v1: the service's own settings ----------------------------- */
static void test_settings(void)
{
    mcdstore::Settings st;
    struct stat sb;
    char path[512];

    removeFile("settings.v1");
    st.path_hash_bytes = 3;
    check("no settings file is the defaults, and says so",
          mcdstore::settingsLoad(st, g_dir, g_err) == 1 && st.path_hash_bytes == 1);
    st.path_hash_bytes = 2;
    check("a path hash size is saved", mcdstore::settingsSave(st, g_dir, g_err));
    joinp(path, sizeof(path), "settings.v1");
    check("at 0600, like everything this service writes",
          stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600);
    st = mcdstore::Settings();
    check("and read back", mcdstore::settingsLoad(st, g_dir, g_err) == 0 &&
                               st.path_hash_bytes == 2);
    st.path_hash_bytes = 4;
    check("a size outside 1..3 is not written", !mcdstore::settingsSave(st, g_dir, g_err));
    st = mcdstore::Settings();
    check("and the file still holds the last good one",
          mcdstore::settingsLoad(st, g_dir, g_err) == 0 && st.path_hash_bytes == 2);
    {
        const char text[] = "# written by hand\r\nsomething_newer=7\npath_hash_bytes=3\n";

        writeRaw("settings.v1", (const uint8_t*)text, sizeof(text) - 1, 0600);
        st = mcdstore::Settings();
        check("comments, CR LF and keys this build does not know are passed over",
              mcdstore::settingsLoad(st, g_dir, g_err) == 0 && st.path_hash_bytes == 3);
    }
    {
        /* renamed_over: the mark of a configured name a rename replaced. */
        st = mcdstore::Settings();
        check("a new Settings carries no rename mark", st.renamed_over[0] == '\0');
        st.path_hash_bytes = 2;
        snprintf(st.renamed_over, sizeof(st.renamed_over), "%s", "0123456789abcdef");
        check("a rename mark is saved beside the path hash size",
              mcdstore::settingsSave(st, g_dir, g_err));
        st = mcdstore::Settings();
        check("and both are read back",
              mcdstore::settingsLoad(st, g_dir, g_err) == 0 && st.path_hash_bytes == 2 &&
                  strcmp(st.renamed_over, "0123456789abcdef") == 0);
        st.renamed_over[0] = '\0';
        check("saved without one", mcdstore::settingsSave(st, g_dir, g_err));
        st = mcdstore::Settings();
        snprintf(st.renamed_over, sizeof(st.renamed_over), "%s", "ffffffffffffffff");
        check("the file holds none",
              mcdstore::settingsLoad(st, g_dir, g_err) == 0 && st.renamed_over[0] == '\0');
    }
    {
        const char text[] = "path_hash_bytes=3\nrenamed_over=K230-B\n";

        writeRaw("settings.v1", (const uint8_t*)text, sizeof(text) - 1, 0600);
        st = mcdstore::Settings();
        check("a mark that is not 16 hex characters is no mark, and the rest is read",
              mcdstore::settingsLoad(st, g_dir, g_err) == 0 && st.path_hash_bytes == 3 &&
                  st.renamed_over[0] == '\0');
    }
    {
        const char text[] = "path_hash_bytes=9\n";

        writeRaw("settings.v1", (const uint8_t*)text, sizeof(text) - 1, 0600);
        st.path_hash_bytes = 2;
        check("a value out of range is refused, with the defaults and a reason",
              mcdstore::settingsLoad(st, g_dir, g_err) == -1 && st.path_hash_bytes == 1 &&
                  strstr(g_err, "path_hash_bytes") != NULL);
    }
    {
        const char text[] = "path_hash_bytes=22\n";

        writeRaw("settings.v1", (const uint8_t*)text, sizeof(text) - 1, 0600);
        check("and so is one with more than a digit",
              mcdstore::settingsLoad(st, g_dir, g_err) == -1 && st.path_hash_bytes == 1);
    }
    removeFile("settings.v1");
}

int main(void)
{
    char tmpl[] = "/tmp/meshcored-store-XXXXXX";
    char* dir = mkdtemp(tmpl);

    if (!dir) {
        fprintf(stderr, "cannot create a temporary directory: %s\n", strerror(errno));
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    check("the state directory is created", mcdstore::ensureDir(g_dir, g_err));

    test_identity_generate_and_reload();
    test_identity_corruption();
    test_state_round_trip();
    test_state_capacity();
    test_state_corruption();
    test_quarantine();
    test_channels_round_trip();
    test_channels_corruption();
    test_channels_quarantine();
    test_settings();
    test_directory_durability();
    test_dir_rules();

    /* Leave nothing behind; the files hold a private key. */
    {
        char cmd[512];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "note: could not remove %s\n", g_dir);
        }
    }
    printf("meshcored_store_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
