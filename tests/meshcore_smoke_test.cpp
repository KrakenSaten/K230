/*
 * protocols/meshcore: two nodes, one in-memory air.
 *
 * Node A and node B are each a full BaseChatMesh over the portable core and
 * the Linux port, joined by test_support/mc_fake_radio.h. Nothing here is
 * mocked except the radio and the clock: the adverts are really signed, the
 * shared secret is a real X25519 agreement, the messages are really
 * AES-128-encrypted and MAC'd, and both nodes really parse what the other
 * one put on the air.
 *
 * What it proves: the protocol core encodes, signs, routes, decrypts and
 * verifies correctly, and the whole of it runs in an ordinary Linux process
 * with no LVGL, no RadioLib, no Arduino runtime and no hardware.
 *
 * What it does NOT prove: anything about radio behaviour. The fake air is
 * lossless, collision-free, instantaneous and has no range. Over-the-air
 * interoperability with real MeshCore hardware is the accepted P0 gate,
 * docs/hardware/MESHCORE_INTEROP_GATE.md, not this file.
 *
 * Built and run by protocols/meshcore/Makefile:
 *   make meshcore-core-test        (from the top of the repository)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <stdio.h>
#include <string.h>

#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "mc_fake_radio.h"
#include "mc_port.h"

static int failed;
static int checks;

static void check(const char* name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  checks++;
  failed += !ok;
}

/* ---- a node -------------------------------------------------------------
 *
 * BaseChatMesh leaves the presentation decisions to its subclass: what to do
 * with a discovered contact, what an ACK means, how long to wait. On a
 * device that subclass is the firmware's UI task. Here it is a recorder -
 * it remembers what happened and decides nothing - which is exactly the
 * boundary this task is establishing. No RIFT UI code is involved.
 */
class TestNode : public BaseChatMesh {
public:
  const char* label;

  int contacts_discovered;
  int messages_received;
  int acks_matched;
  int paths_updated;
  int send_timeouts;

  char last_text[MAX_TEXT_LEN + 1];
  char last_sender[32];
  uint32_t last_msg_timestamp;

  /* What sendMessage() told us to expect back. */
  uint32_t expected_ack;
  bool awaiting_ack;

  /* Set only by the crafted-PATH case at the end of this file. */
  bool record_raw_path;
  uint8_t observed_extra_len;
  uint8_t observed_extra_type;
  int raw_paths_seen;

  TestNode(const char* lbl, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
           mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), label(lbl) {
    contacts_discovered = messages_received = acks_matched = paths_updated = send_timeouts = 0;
    last_text[0] = 0;
    last_sender[0] = 0;
    last_msg_timestamp = 0;
    expected_ack = 0;
    awaiting_ack = false;
    record_raw_path = false;
    observed_extra_len = 0;
    observed_extra_type = 0;
    raw_paths_seen = 0;
  }

  /* The found contact is copied into a per-node slot. It must NOT be a
   * function-static: that is one object shared by every node in the process,
   * and node B's lookup would overwrite what node A was just handed. */
  ContactInfo _found;

  ContactInfo* contactByName(const char* name) {
    ContactInfo c;
    ContactsIterator it = startContactsIterator();
    while (it.hasNext(this, c)) {
      if (strcmp(c.name, name) == 0) { _found = c; return &_found; }
    }
    return NULL;
  }

  /* A PATH packet built by hand, for the hardening-debt case at the end of
   * this file. Mesh::createDatagram() refuses PAYLOAD_TYPE_PATH (Mesh.cpp:489
   * accepts only TXT_MSG, REQ and RESPONSE) and Mesh::createPathReturn()
   * builds a self-consistent payload, so neither can produce the malformed
   * inner payload the defect needs. This assembles the same envelope
   * createDatagram() does - dest hash, src hash, then encryptThenMAC with the
   * real shared secret - around a payload the caller chose. The crypto is
   * genuine; only the plaintext is crafted. */
  mesh::Packet* craftPathPacket(const ContactInfo& to, const uint8_t* inner, size_t inner_len) {
    const uint8_t* secret = to.getSharedSecret(self_id);
    mesh::Packet* packet = obtainNewPacket();
    if (packet == NULL) return NULL;

    packet->header = (PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT);
    int len = 0;
    len += to.id.copyHashTo(&packet->payload[len]);
    len += self_id.copyHashTo(&packet->payload[len]);
    len += mesh::Utils::encryptThenMAC(secret, &packet->payload[len], inner, (int) inner_len);
    packet->payload_len = (uint16_t) len;
    return packet;
  }

protected:
  /* ---- discovery ----
   * Every onDiscoveredContact call is counted, not only the ones flagged new.
   * BaseChatMesh::onAdvertRecv declares `bool is_new = false` and never
   * assigns it (vendor/RIFT src/helpers/BaseChatMesh.cpp:154 and 198), so a
   * contact added for the first time is still reported as not new. That is a
   * UI-notification quirk rather than a protocol fault, and it is upstream's
   * to decide; this test therefore does not depend on the flag. Noted in
   * protocols/meshcore/README.md under known debt. */
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {
    contacts_discovered++;
  }
  void onContactPathUpdated(const ContactInfo&) override { paths_updated++; }

  /* ---- messages ---- */
  void onMessageRecv(const ContactInfo& contact, mesh::Packet*, uint32_t sender_timestamp,
                     const char* text) override {
    messages_received++;
    last_msg_timestamp = sender_timestamp;
    snprintf(last_text, sizeof(last_text), "%s", text);
    snprintf(last_sender, sizeof(last_sender), "%s", contact.name);
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override { }
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override { }
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override { }

  /* ---- acks ----
   * BaseChatMesh hands us four bytes and asks whose ACK it is. A real client
   * looks the value up in its outbox; this one has a single outstanding
   * message. */
  ContactInfo* processAck(const uint8_t* data) override {
    uint32_t crc;
    memcpy(&crc, data, 4);
    if (awaiting_ack && crc == expected_ack) {
      acks_matched++;
      awaiting_ack = false;
      return contactByName(peer_name);
    }
    return NULL;
  }

  /* ---- the PATH hook, used by the hardening-debt case ---- */
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len,
                         uint8_t* out_path, uint8_t out_path_len,
                         uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override {
    if (record_raw_path) {
      /* Record only. Deliberately does NOT read through `extra`, and does not
       * chain to the base implementation, which would copy a 63-hop path
       * this packet does not really carry. */
      raw_paths_seen++;
      observed_extra_type = extra_type;
      observed_extra_len = extra_len;
      return false;
    }
    return BaseChatMesh::onContactPathRecv(from, in_path, in_path_len, out_path, out_path_len,
                                           extra_type, extra, extra_len);
  }

  /* ---- timeouts ----
   * Generous, and in the same shape the firmwares use: a multiple of the
   * airtime plus a fixed allowance. */
  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return 5000 + pkt_airtime_millis * 8;
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
    return 2000 + (pkt_airtime_millis * 2) * (path_len + 1);
  }
  void onSendTimeout() override { send_timeouts++; }

  /* ---- requests, which this node does not serve ---- */
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override {
    return 0;
  }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override { }

public:
  /* Set by the test so processAck can name the contact the ACK came from. */
  const char* peer_name = "";
};

/* ---- the world ---------------------------------------------------------- */

struct World {
  mctest::FakeAir air;
  mctest::TestClock clock;          /* one clock: both nodes share the world */
  mctest::TestRTCClock rtc_a;
  mctest::TestRTCClock rtc_b;
  mcport::HostRNG rng;

  mctest::FakeRadio radio_a;
  mctest::FakeRadio radio_b;
  StaticPoolPacketManager mgr_a;
  StaticPoolPacketManager mgr_b;
  SimpleMeshTables tables_a;
  SimpleMeshTables tables_b;

  TestNode a;
  TestNode b;

  World()
      : rtc_a(1789000000u), rtc_b(1789000000u),
        radio_a(air), radio_b(air),
        mgr_a(32), mgr_b(32),
        a("A", radio_a, clock, rng, rtc_a, mgr_a, tables_a),
        b("B", radio_b, clock, rng, rtc_b, mgr_b, tables_b) { }

  /* Turn both handles, advancing the shared clock. 25 ms a step is fine
   * grained enough for the 200 ms ACK delay to be observed rather than
   * skipped over. */
  void pump(int steps) {
    for (int i = 0; i < steps; i++) {
      a.loop();
      b.loop();
      clock.advance(25);
    }
  }
};

/* ---- 1. the ADVERT exchange -------------------------------------------- */

static void test_advert_exchange(World& w) {
  w.a.self_id = mesh::LocalIdentity(&w.rng);
  w.b.self_id = mesh::LocalIdentity(&w.rng);
  check("the two nodes have different identities",
        memcmp(w.a.self_id.pub_key, w.b.self_id.pub_key, PUB_KEY_SIZE) != 0);

  w.a.peer_name = "K230-B";
  w.b.peer_name = "K230-A";

  w.a.begin();
  w.b.begin();

  mesh::Packet* adv_a = w.a.createSelfAdvert("K230-A");
  mesh::Packet* adv_b = w.b.createSelfAdvert("K230-B");
  check("node A builds a signed self-advert", adv_a != NULL);
  check("node B builds a signed self-advert", adv_b != NULL);
  check("the advert is an ADVERT packet",
        adv_a != NULL && adv_a->getPayloadType() == PAYLOAD_TYPE_ADVERT);

  if (adv_a) w.a.sendFlood(adv_a);
  if (adv_b) w.b.sendFlood(adv_b);

  w.pump(40);

  check("node B heard node A's advert", w.b.contacts_discovered == 1);
  check("node A heard node B's advert", w.a.contacts_discovered == 1);
  check("both frames crossed the air", w.air.framesCarried() == 2);

  ContactInfo* a_sees_b = w.a.contactByName("K230-B");
  ContactInfo* b_sees_a = w.b.contactByName("K230-A");
  check("node A has node B as a named contact", a_sees_b != NULL);
  check("node B has node A as a named contact", b_sees_a != NULL);

  if (a_sees_b) {
    check("the contact carries node B's real public key",
          memcmp(a_sees_b->id.pub_key, w.b.self_id.pub_key, PUB_KEY_SIZE) == 0);
    check("the contact is a chat node", a_sees_b->type == ADV_TYPE_CHAT);
    check("no return path is known yet", a_sees_b->out_path_len == OUT_PATH_UNKNOWN);
    check("the advert's timestamp was recorded", a_sees_b->last_advert_timestamp > 0);
  }

  /* The signature is what made that contact trustworthy. A forged advert -
   * node B's public key, with the signature bytes disturbed - must not
   * produce a contact. This is the real Ed25519 verify saying no. */
  {
    mesh::Packet* forged = w.b.createSelfAdvert("K230-EVIL");
    check("a second advert can be built", forged != NULL);
    if (forged) {
      /* An advert payload is pub_key(32) | timestamp(4) | signature(64) |
       * app_data. Disturb one byte of the signature. */
      forged->payload[32 + 4 + 10] ^= 0x01;
      w.b.sendFlood(forged);
      int before = w.a.contacts_discovered;
      w.pump(40);
      check("node A rejects an advert whose signature does not verify",
            w.a.contacts_discovered == before);
      check("and does not learn the forged name", w.a.contactByName("K230-EVIL") == NULL);
    }
  }
}

/* ---- 2. a directed text message, and the ACK back ----------------------- */

static void test_text_and_ack(World& w) {
  ContactInfo* b_contact = w.a.contactByName("K230-B");
  check("node A still knows node B", b_contact != NULL);
  if (b_contact == NULL) return;

  const char* text = "hello from the K230";
  uint32_t est_timeout = 0;
  uint32_t ack = 0;

  int rc = w.a.sendMessage(*b_contact, w.rtc_a.getCurrentTime(), 0, text, ack, est_timeout);
  w.a.expected_ack = ack;
  w.a.awaiting_ack = true;

  /* The first message to a contact goes flood, because no return path is
   * known yet - that is what the ACK is about to supply. */
  check("the first message is sent flood", rc == MSG_SEND_SENT_FLOOD);
  check("an expected ACK value was produced", ack != 0);
  check("a timeout was estimated", est_timeout > 0);

  w.pump(60);

  check("node B received the message", w.b.messages_received == 1);
  check("the text arrived intact", strcmp(w.b.last_text, text) == 0);
  check("node B knows who sent it", strcmp(w.b.last_sender, "K230-A") == 0);
  check("the sender's timestamp came through", w.b.last_msg_timestamp == w.rtc_a.getCurrentTime());

  /* B answered a flood message with a PATH packet that carries the ACK. A
   * therefore learns a return path and matches its ACK from the same frame. */
  check("node A matched the ACK", w.a.acks_matched == 1);
  check("node A is no longer waiting", !w.a.awaiting_ack);
  check("node A learned a return path to node B", w.a.paths_updated >= 1);

  b_contact = w.a.contactByName("K230-B");
  check("the contact now has an out path",
        b_contact != NULL && b_contact->out_path_len != OUT_PATH_UNKNOWN);
  check("nothing timed out", w.a.send_timeouts == 0);

  /* ---- and now the directed case ----
   * With a path known, the second message goes direct rather than flood, and
   * the ACK comes back as a bare ACK packet rather than inside a PATH. */
  if (b_contact) {
    const char* second = "and a second one, directed";
    uint32_t ack2 = 0, timeout2 = 0;
    w.rtc_a.setCurrentTime(w.rtc_a.getCurrentTime() + 1);

    int rc2 = w.a.sendMessage(*b_contact, w.rtc_a.getCurrentTime(), 0, second, ack2, timeout2);
    w.a.expected_ack = ack2;
    w.a.awaiting_ack = true;

    check("the second message is sent DIRECT", rc2 == MSG_SEND_SENT_DIRECT);
    check("it has its own expected ACK", ack2 != 0 && ack2 != w.a.expected_ack - 1);

    w.pump(60);

    check("node B received the directed message", w.b.messages_received == 2);
    check("the directed text arrived intact", strcmp(w.b.last_text, second) == 0);
    check("node A matched the second ACK", w.a.acks_matched == 2);
    check("node A is not waiting after the second ACK", !w.a.awaiting_ack);
    check("still nothing timed out", w.a.send_timeouts == 0);
  }

  /* Neither node leaked a packet from its pool. */
  check("node A's packet pool is whole", w.mgr_a.getFreeCount() == 32);
  check("node B's packet pool is whole", w.mgr_b.getFreeCount() == 32);
}

/* ---- 3. a third party cannot read or forge ------------------------------ */

static void test_a_stranger_is_excluded(World& w) {
  /* A node C on the same air, which the other two have never heard of. It
   * hears every frame - the fake air is a broadcast medium - and must not be
   * able to read a message addressed to B. */
  mctest::FakeRadio radio_c(w.air);
  StaticPoolPacketManager mgr_c(16);
  SimpleMeshTables tables_c;
  mctest::TestRTCClock rtc_c(1789000000u);
  TestNode c("C", radio_c, w.clock, w.rng, rtc_c, mgr_c, tables_c);
  c.self_id = mesh::LocalIdentity(&w.rng);
  c.begin();

  ContactInfo* b_contact = w.a.contactByName("K230-B");
  if (b_contact == NULL) { check("node A still knows node B", false); return; }

  uint32_t ack = 0, timeout = 0;
  w.rtc_a.setCurrentTime(w.rtc_a.getCurrentTime() + 1);
  w.a.sendMessage(*b_contact, w.rtc_a.getCurrentTime(), 0, "for B only", ack, timeout);
  w.a.expected_ack = ack;
  w.a.awaiting_ack = true;

  int b_before = w.b.messages_received;
  for (int i = 0; i < 60; i++) {
    w.a.loop(); w.b.loop(); c.loop();
    w.clock.advance(25);
  }

  check("node B read the message", w.b.messages_received == b_before + 1);
  check("node C heard the frame but read nothing", c.messages_received == 0);
  check("node C did not acquire a contact from a message it cannot decrypt",
        c.contacts_discovered == 0);
  check("node C's pool is whole", mgr_c.getFreeCount() == 16);
  check("three radios are on the air while node C exists", w.air.attachedCount() == 3);
}

/* Node C's radio was built on this function's stack. Once it has returned,
 * the air must no longer hold a pointer to it - otherwise the next broadcast
 * writes into a dead frame, which is what ASan caught here before FakeRadio
 * had a destructor. Checked from the caller's scope, after C is gone. */
static void test_a_departed_node_leaves_the_air(World& w) {
  check("the air is back to two radios once node C has gone",
        w.air.attachedCount() == 2);

  /* And the survivors still talk. */
  ContactInfo* b_contact = w.a.contactByName("K230-B");
  if (b_contact == NULL) { check("node A still knows node B", false); return; }
  int before = w.b.messages_received;
  uint32_t ack = 0, timeout = 0;
  w.rtc_a.setCurrentTime(w.rtc_a.getCurrentTime() + 1);
  w.a.sendMessage(*b_contact, w.rtc_a.getCurrentTime(), 0, "still here", ack, timeout);
  w.a.expected_ack = ack;
  w.a.awaiting_ack = true;
  w.pump(60);
  check("a message still crosses after a node has left the air",
        w.b.messages_received == before + 1);
  check("and it still arrives intact", strcmp(w.b.last_text, "still here") == 0);
}

/* ---- 4. hardening debt: PATH extra_len underflow -----------------------
 *
 * FOLLOW-UP HARDENING WORK, NOT A PASS/FAIL OF CORRECT BEHAVIOUR.
 *
 * vendor/RIFT src/Mesh.cpp, the PAYLOAD_TYPE_PATH branch of onRecvPacket:
 *
 *     uint8_t* path = &data[k]; k += hash_size*hash_count;
 *     uint8_t extra_type = data[k++] & 0x0F;
 *     uint8_t* extra = &data[k];
 *     uint8_t extra_len = len - k;     // line 172
 *
 * `len` is what MACThenDecrypt returned and `k` is driven by the path_len
 * byte inside the decrypted payload. Nothing checks that k <= len. A payload
 * that declares a long path but does not carry one makes `len - k` negative,
 * and the uint8_t truncation turns that into a large positive length handed
 * to onPeerPathRecv() together with a pointer near the end of the buffer.
 *
 * Reaching it needs a valid MAC, so it is not an unauthenticated
 * over-the-air issue: the sender has to be a contact this node has already
 * agreed a key with. That is why it is debt rather than an emergency - but
 * "an authenticated peer can hand a subclass a 200-byte length over a
 * 184-byte buffer" is still worth fixing, and worth a test that says what
 * happens today.
 *
 * Below, node A crafts the inner payload by hand and encrypts it with the
 * real shared secret, so this is the genuine code path and not a simulation
 * of it. The node's handler records the length and does not read through the
 * pointer, so the demonstration does not itself perform the over-read.
 *
 * These checks assert what the code does TODAY. When the bounds check is
 * added, they are expected to fail and should be rewritten to assert the
 * fixed behaviour - that is the signal, not a regression.
 */
static void test_debt_path_extra_len_underflow(World& w) {
  ContactInfo* b_contact = w.a.contactByName("K230-B");
  if (b_contact == NULL) { check("node A still knows node B", false); return; }

  w.b.record_raw_path = true;
  w.b.raw_paths_seen = 0;

  /* A three-byte PATH payload that claims 63 hops of one-byte hashes.
   * After the path_len byte and the 63 path bytes, k is 64; the extra_type
   * byte takes it to 65. MACThenDecrypt will report 16 (one AES block), so
   * line 172 computes 16 - 65. */
  uint8_t crafted[3];
  crafted[0] = 63;      /* path_len: 63 hops, 1-byte hashes - a valid encoding */
  crafted[1] = 0xAA;
  crafted[2] = 0xBB;

  mesh::Packet* pkt = w.a.craftPathPacket(*b_contact, crafted, sizeof(crafted));
  check("the crafted PATH packet is built", pkt != NULL);
  if (pkt == NULL) { w.b.record_raw_path = false; return; }

  w.a.sendZeroHop(pkt);
  w.pump(60);

  check("DEBT the crafted PATH packet was accepted - its MAC is valid",
        w.b.raw_paths_seen == 1);
  check("DEBT extra_len underflowed to a length the payload cannot hold",
        w.b.observed_extra_len == (uint8_t) (16 - 65));
  check("DEBT that length is larger than the whole decrypted payload",
        w.b.observed_extra_len > 16);
  check("DEBT and larger than MAX_PACKET_PAYLOAD minus the offset it starts at",
        (int) w.b.observed_extra_len + 65 > MAX_PACKET_PAYLOAD);

  /* The well-formed case, for contrast: a PATH payload whose declared path
   * really is there gives a sane extra_len. */
  w.b.raw_paths_seen = 0;
  uint8_t good[8];
  good[0] = 2;             /* two hops of one-byte hashes */
  good[1] = 0x11;
  good[2] = 0x22;
  good[3] = 0;             /* extra_type */
  good[4] = 0xDE;          /* one byte of extra */
  mesh::Packet* ok = w.a.craftPathPacket(*b_contact, good, 5);
  if (ok) {
    w.a.sendZeroHop(ok);
    w.pump(60);
    check("a well-formed PATH gives a sane extra_len",
          w.b.raw_paths_seen == 1 && w.b.observed_extra_len <= 16);
  }

  w.b.record_raw_path = false;
  check("node A's pool is whole after the crafted frames", w.mgr_a.getFreeCount() == 32);
  check("node B's pool is whole after the crafted frames", w.mgr_b.getFreeCount() == 32);
}

int main(void) {
  World w;

  test_advert_exchange(w);
  test_text_and_ack(w);
  test_a_stranger_is_excluded(w);
  test_a_departed_node_leaves_the_air(w);
  test_debt_path_extra_len_underflow(w);

  printf("meshcore_smoke_test: %d check(s), %d failure(s)\n", checks, failed);
  return failed ? 1 : 0;
}
