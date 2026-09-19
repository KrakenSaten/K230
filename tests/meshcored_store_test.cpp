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
        bad[40] = 200;  /* more nodes than the format allows */
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
    test_state_corruption();
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
