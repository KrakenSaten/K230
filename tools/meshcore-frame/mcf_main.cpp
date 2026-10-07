/* meshcore-frame: a command line over the MeshCore wire format.
 *
 * Build a frame a MeshCore node will accept, or take a frame apart and say
 * what it is. The hex it prints is what `pos radio send <hex>` transmits and
 * what radiod reports in a radio.rx event, so the two ends of an interop test
 * speak the same alphabet.
 *
 * This tool talks to no hardware and to no daemon. It reads a key file, does
 * arithmetic, and writes to stdout.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcf.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

#ifndef MESHCORE_FRAME_RIFT_COMMIT
#define MESHCORE_FRAME_RIFT_COMMIT "unknown"
#endif
#ifndef MESHCORE_FRAME_CRYPTO_COMMIT
#define MESHCORE_FRAME_CRYPTO_COMMIT "unknown"
#endif

static const char* prog = "meshcore-frame";

static int usage(void) {
  fprintf(stderr,
    "usage: %s <command> [options]\n"
    "\n"
    "  identity new <file>        create a MeshCore identity (96-byte .id, mode 0600)\n"
    "  identity show <file>       print the public key and node hash of an identity\n"
    "\n"
    "  advert --key <file> [--name <text>] [--type chat|repeater|room|sensor|none]\n"
    "         [--lat <deg> --lon <deg>] [--timestamp <unix>] [routing]\n"
    "  txtmsg --key <file> --peer <pubkey-hex> --text <text>\n"
    "         [--timestamp <unix>] [--attempt <n>] [--txt-type <n>] [routing]\n"
    "  ack --hash <hex>           [routing]\n"
    "\n"
    "  parse <hex> [--key <file>] [--peer <pubkey-hex>]\n"
    "\n"
    "  routing: --route flood|direct|transport-flood|transport-direct (default flood)\n"
    "           --path <hex>            the hop hashes of a direct route\n"
    "           --path-hash-size <1-3>  bytes per hop hash (default 1)\n"
    "           --transport <a> <b>     the two transport codes\n"
    "\n"
    "  version                    print the tool and the pinned upstream commits\n"
    "\n"
    "The frame_hex line of a build is what `pos radio send <hex>` transmits.\n",
    prog);
  return 2;
}

static bool parseU32(const char* s, uint32_t* out) {
  if (s == NULL || *s == 0) return false;
  char* end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno != 0 || end == s || *end != 0 || v > 0xFFFFFFFFull) return false;
  *out = (uint32_t)v;
  return true;
}

static bool parseDouble(const char* s, double* out) {
  if (s == NULL || *s == 0) return false;
  char* end = NULL;
  errno = 0;
  double v = strtod(s, &end);
  if (errno != 0 || end == s || *end != 0) return false;
  *out = v;
  return true;
}

static bool parseAdvType(const char* s, uint8_t* out) {
  if (strcmp(s, "none") == 0)     { *out = ADV_TYPE_NONE;     return true; }
  if (strcmp(s, "chat") == 0)     { *out = ADV_TYPE_CHAT;     return true; }
  if (strcmp(s, "repeater") == 0) { *out = ADV_TYPE_REPEATER; return true; }
  if (strcmp(s, "room") == 0)     { *out = ADV_TYPE_ROOM;     return true; }
  if (strcmp(s, "sensor") == 0)   { *out = ADV_TYPE_SENSOR;   return true; }
  uint32_t n;
  if (parseU32(s, &n) && n <= 0x0F) { *out = (uint8_t)n; return true; }
  return false;
}

static bool parseRouteType(const char* s, uint8_t* out) {
  if (strcmp(s, "flood") == 0)            { *out = ROUTE_TYPE_FLOOD;            return true; }
  if (strcmp(s, "direct") == 0)           { *out = ROUTE_TYPE_DIRECT;           return true; }
  if (strcmp(s, "transport-flood") == 0)  { *out = ROUTE_TYPE_TRANSPORT_FLOOD;  return true; }
  if (strcmp(s, "transport-direct") == 0) { *out = ROUTE_TYPE_TRANSPORT_DIRECT; return true; }
  return false;
}

/* Routing options are shared by every builder, so they are parsed in one
 * place. The path itself is parsed after the whole command line has been
 * read, because --path-hash-size may follow --path. */
struct RoutingArgs {
  const char* path_hex;
  const char* route_name;
  uint32_t path_hash_size;
  bool have_transport;
  uint32_t transport[2];
};

static void routingInit(RoutingArgs& ra) {
  ra.path_hex = NULL;
  ra.route_name = NULL;
  ra.path_hash_size = PATH_HASH_SIZE;
  ra.have_transport = false;
  ra.transport[0] = ra.transport[1] = 0;
}

/* Returns 0 when the argument was not a routing option, 1 when it was and was
 * consumed, -1 on an error already reported. *i is advanced past any value. */
static int routingArg(RoutingArgs& ra, int argc, char** argv, int* i) {
  const char* a = argv[*i];
  if (strcmp(a, "--route") == 0 && *i + 1 < argc) {
    ra.route_name = argv[++(*i)];
    return 1;
  }
  if (strcmp(a, "--path") == 0 && *i + 1 < argc) {
    ra.path_hex = argv[++(*i)];
    return 1;
  }
  if (strcmp(a, "--path-hash-size") == 0 && *i + 1 < argc) {
    if (!parseU32(argv[++(*i)], &ra.path_hash_size) ||
        ra.path_hash_size < 1 || ra.path_hash_size > 3) {
      fprintf(stderr, "%s: --path-hash-size must be 1, 2 or 3\n", prog);
      return -1;
    }
    return 1;
  }
  if (strcmp(a, "--transport") == 0 && *i + 2 < argc) {
    if (!parseU32(argv[*i + 1], &ra.transport[0]) || ra.transport[0] > 0xFFFF ||
        !parseU32(argv[*i + 2], &ra.transport[1]) || ra.transport[1] > 0xFFFF) {
      fprintf(stderr, "%s: --transport takes two numbers, each 0 to 65535\n", prog);
      return -1;
    }
    ra.have_transport = true;
    *i += 2;
    return 1;
  }
  return 0;
}

static bool routingBuild(const RoutingArgs& ra, mcf::Route& route, char* err) {
  mcf::routeInit(route);
  route.path_hash_size = (uint8_t)ra.path_hash_size;

  if (ra.route_name != NULL && !parseRouteType(ra.route_name, &route.type)) {
    snprintf(err, MCF_ERR_SIZE,
             "--route must be flood, direct, transport-flood or transport-direct");
    return false;
  }
  if (ra.route_name == NULL && ra.have_transport) {
    snprintf(err, MCF_ERR_SIZE, "--transport needs --route transport-flood or transport-direct");
    return false;
  }
  if (ra.path_hex != NULL && !mcf::routeParsePath(route, ra.path_hex, err)) return false;

  route.transport_codes[0] = (uint16_t)ra.transport[0];
  route.transport_codes[1] = (uint16_t)ra.transport[1];

  if (ra.have_transport && !(route.type == ROUTE_TYPE_TRANSPORT_FLOOD ||
                             route.type == ROUTE_TYPE_TRANSPORT_DIRECT)) {
    snprintf(err, MCF_ERR_SIZE, "--transport needs a transport route type");
    return false;
  }
  return true;
}

/* Frames the packet and prints the report every build command ends with. */
static int emit(mesh::Packet& pkt, const mcf::Route& route,
                const mesh::LocalIdentity* id, const uint8_t* peer) {
  char err[MCF_ERR_SIZE];
  if (!mcf::routeApply(pkt, route, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }
  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  if (!mcf::encode(pkt, raw, &raw_len, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }
  /* Read the frame back through the parser before printing it. A builder that
   * emits hex nothing can decode is the failure this tool exists to catch, so
   * it is caught here rather than on the air. */
  mesh::Packet check;
  if (!mcf::decode(check, raw, raw_len, err)) {
    fprintf(stderr, "%s: the frame just built does not parse: %s\n", prog, err);
    return 1;
  }
  return mcf::report(check, raw, raw_len, id, peer) ? 0 : 1;
}

/* ---- commands ---------------------------------------------------------- */

static int cmdIdentity(int argc, char** argv) {
  if (argc < 2) return usage();
  const char* sub = argv[0];
  const char* path = argv[1];
  char err[MCF_ERR_SIZE];
  mesh::LocalIdentity id;

  if (strcmp(sub, "new") == 0) {
    if (!mcf::identityCreate(id, err) || !mcf::identitySave(id, path, err)) {
      fprintf(stderr, "%s: %s\n", prog, err);
      return 1;
    }
  } else if (strcmp(sub, "show") == 0) {
    if (!mcf::identityLoad(id, path, err)) {
      fprintf(stderr, "%s: %s\n", prog, err);
      return 1;
    }
  } else {
    return usage();
  }

  char hex[2 * PUB_KEY_SIZE + 1];
  mcf::hexEncode(hex, id.pub_key, PUB_KEY_SIZE);
  printf("identity_file: %s\n", path);
  printf("public_key: %s\n", hex);
  mcf::hexEncode(hex, id.pub_key, PATH_HASH_SIZE);
  printf("node_hash: %s\n", hex);
  return 0;
}

static int cmdAdvert(int argc, char** argv) {
  const char* key_path = NULL;
  const char* lat_s = NULL;
  const char* lon_s = NULL;
  mcf::AdvertOpts o;
  RoutingArgs ra;
  char err[MCF_ERR_SIZE];

  o.name = "";
  o.adv_type = ADV_TYPE_CHAT;
  o.has_loc = false;
  o.lat = o.lon = 0.0;
  o.timestamp = (uint32_t)time(NULL);
  routingInit(ra);

  for (int i = 0; i < argc; i++) {
    int r = routingArg(ra, argc, argv, &i);
    if (r < 0) return 1;
    if (r > 0) continue;

    const char* a = argv[i];
    if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
      o.name = argv[++i];
    } else if (strcmp(a, "--type") == 0 && i + 1 < argc) {
      if (!parseAdvType(argv[++i], &o.adv_type)) {
        fprintf(stderr, "%s: --type must be none, chat, repeater, room, sensor or 0-15\n", prog);
        return 1;
      }
    } else if (strcmp(a, "--lat") == 0 && i + 1 < argc) {
      lat_s = argv[++i];
    } else if (strcmp(a, "--lon") == 0 && i + 1 < argc) {
      lon_s = argv[++i];
    } else if (strcmp(a, "--timestamp") == 0 && i + 1 < argc) {
      if (!parseU32(argv[++i], &o.timestamp)) {
        fprintf(stderr, "%s: --timestamp must be a UNIX time in seconds\n", prog);
        return 1;
      }
    } else {
      fprintf(stderr, "%s: unknown option %s\n", prog, a);
      return usage();
    }
  }

  if (key_path == NULL) {
    fprintf(stderr, "%s: advert needs --key <identity-file>\n", prog);
    return usage();
  }
  if ((lat_s == NULL) != (lon_s == NULL)) {
    fprintf(stderr, "%s: --lat and --lon go together\n", prog);
    return 1;
  }
  if (lat_s != NULL) {
    if (!parseDouble(lat_s, &o.lat) || !parseDouble(lon_s, &o.lon) ||
        o.lat < -90.0 || o.lat > 90.0 || o.lon < -180.0 || o.lon > 180.0) {
      fprintf(stderr, "%s: --lat and --lon are degrees, -90..90 and -180..180\n", prog);
      return 1;
    }
    o.has_loc = true;
  }

  mesh::LocalIdentity id;
  if (!mcf::identityLoad(id, key_path, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  mcf::Route route;
  if (!routingBuild(ra, route, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  mesh::Packet pkt;
  if (!mcf::buildAdvert(pkt, id, o, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }
  return emit(pkt, route, NULL, NULL);
}

static int cmdTxtMsg(int argc, char** argv) {
  const char* key_path = NULL;
  const char* peer_hex = NULL;
  mcf::TxtMsgOpts o;
  RoutingArgs ra;
  char err[MCF_ERR_SIZE];
  uint32_t attempt = 0;
  uint32_t txt_type = TXT_TYPE_PLAIN;

  o.dest_pub_key = NULL;
  o.text = NULL;
  o.timestamp = (uint32_t)time(NULL);
  o.attempt = 0;
  o.txt_type = TXT_TYPE_PLAIN;
  routingInit(ra);

  for (int i = 0; i < argc; i++) {
    int r = routingArg(ra, argc, argv, &i);
    if (r < 0) return 1;
    if (r > 0) continue;

    const char* a = argv[i];
    if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
      peer_hex = argv[++i];
    } else if (strcmp(a, "--text") == 0 && i + 1 < argc) {
      o.text = argv[++i];
    } else if (strcmp(a, "--timestamp") == 0 && i + 1 < argc) {
      if (!parseU32(argv[++i], &o.timestamp)) {
        fprintf(stderr, "%s: --timestamp must be a UNIX time in seconds\n", prog);
        return 1;
      }
    } else if (strcmp(a, "--attempt") == 0 && i + 1 < argc) {
      if (!parseU32(argv[++i], &attempt) || attempt > 255) {
        fprintf(stderr, "%s: --attempt must be 0 to 255\n", prog);
        return 1;
      }
      o.attempt = (uint8_t)attempt;
    } else if (strcmp(a, "--txt-type") == 0 && i + 1 < argc) {
      if (!parseU32(argv[++i], &txt_type) || txt_type > 63) {
        fprintf(stderr, "%s: --txt-type must be 0 to 63\n", prog);
        return 1;
      }
      o.txt_type = (uint8_t)txt_type;
    } else {
      fprintf(stderr, "%s: unknown option %s\n", prog, a);
      return usage();
    }
  }

  if (key_path == NULL || peer_hex == NULL || o.text == NULL) {
    fprintf(stderr, "%s: txtmsg needs --key, --peer and --text\n", prog);
    return usage();
  }

  uint8_t peer[PUB_KEY_SIZE];
  size_t peer_len = 0;
  if (!mcf::hexDecode(peer_hex, peer, sizeof(peer), &peer_len) || peer_len != PUB_KEY_SIZE) {
    fprintf(stderr, "%s: --peer must be a %d-byte public key in hex\n", prog, PUB_KEY_SIZE);
    return 1;
  }
  o.dest_pub_key = peer;

  mesh::LocalIdentity id;
  if (!mcf::identityLoad(id, key_path, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  mcf::Route route;
  if (!routingBuild(ra, route, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  mesh::Packet pkt;
  uint32_t expected_ack = 0;
  if (!mcf::buildTxtMsg(pkt, id, o, &expected_ack, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  int rc = emit(pkt, route, &id, peer);
  if (rc == 0) {
    char hex[2 * 4 + 1];
    mcf::hexEncode(hex, (const uint8_t*)&expected_ack, 4);
    printf("txt.ack_to_expect: %s\n", hex);
  }
  return rc;
}

static int cmdAck(int argc, char** argv) {
  const char* hash_hex = NULL;
  RoutingArgs ra;
  char err[MCF_ERR_SIZE];

  routingInit(ra);

  for (int i = 0; i < argc; i++) {
    int r = routingArg(ra, argc, argv, &i);
    if (r < 0) return 1;
    if (r > 0) continue;

    const char* a = argv[i];
    if (strcmp(a, "--hash") == 0 && i + 1 < argc) {
      hash_hex = argv[++i];
    } else {
      fprintf(stderr, "%s: unknown option %s\n", prog, a);
      return usage();
    }
  }

  if (hash_hex == NULL) {
    fprintf(stderr, "%s: ack needs --hash <hex> (the value txtmsg reports as ack_to_expect)\n", prog);
    return usage();
  }

  uint8_t ack[MAX_PACKET_PAYLOAD];
  size_t ack_len = 0;
  if (!mcf::hexDecode(hash_hex, ack, sizeof(ack), &ack_len)) {
    fprintf(stderr, "%s: --hash must be hex, two digits a byte, at most %d bytes\n",
            prog, MAX_PACKET_PAYLOAD);
    return 1;
  }

  mcf::Route route;
  if (!routingBuild(ra, route, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  mesh::Packet pkt;
  if (!mcf::buildAck(pkt, ack, ack_len, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }
  return emit(pkt, route, NULL, NULL);
}

static int cmdParse(int argc, char** argv) {
  if (argc < 1) return usage();
  const char* frame_hex = argv[0];
  const char* key_path = NULL;
  const char* peer_hex = NULL;
  char err[MCF_ERR_SIZE];

  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--key") == 0 && i + 1 < argc) {
      key_path = argv[++i];
    } else if (strcmp(a, "--peer") == 0 && i + 1 < argc) {
      peer_hex = argv[++i];
    } else {
      fprintf(stderr, "%s: unknown option %s\n", prog, a);
      return usage();
    }
  }

  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  if (!mcf::hexDecode(frame_hex, raw, sizeof(raw), &raw_len)) {
    fprintf(stderr, "%s: the frame must be hex, two digits a byte, at most %d bytes\n",
            prog, MAX_TRANS_UNIT);
    return 1;
  }

  mesh::LocalIdentity id;
  bool have_id = false;
  if (key_path != NULL) {
    if (!mcf::identityLoad(id, key_path, err)) {
      fprintf(stderr, "%s: %s\n", prog, err);
      return 1;
    }
    have_id = true;
  }

  uint8_t peer[PUB_KEY_SIZE];
  bool have_peer = false;
  if (peer_hex != NULL) {
    size_t peer_len = 0;
    if (!mcf::hexDecode(peer_hex, peer, sizeof(peer), &peer_len) || peer_len != PUB_KEY_SIZE) {
      fprintf(stderr, "%s: --peer must be a %d-byte public key in hex\n", prog, PUB_KEY_SIZE);
      return 1;
    }
    have_peer = true;
  }

  mesh::Packet pkt;
  if (!mcf::decode(pkt, raw, raw_len, err)) {
    fprintf(stderr, "%s: %s\n", prog, err);
    return 1;
  }

  /* A frame that decodes but does not authenticate is not a pass: a bad
   * advert signature and a MAC that does not match exit non-zero, so a bench
   * script can gate on this command rather than on grepping its output. */
  return mcf::report(pkt, raw, raw_len, have_id ? &id : NULL, have_peer ? peer : NULL) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const char* cmd = argv[1];

  if (strcmp(cmd, "identity") == 0)  return cmdIdentity(argc - 2, argv + 2);
  if (strcmp(cmd, "advert") == 0)    return cmdAdvert(argc - 2, argv + 2);
  if (strcmp(cmd, "txtmsg") == 0)    return cmdTxtMsg(argc - 2, argv + 2);
  if (strcmp(cmd, "ack") == 0)       return cmdAck(argc - 2, argv + 2);
  if (strcmp(cmd, "parse") == 0)     return cmdParse(argc - 2, argv + 2);
  if (strcmp(cmd, "version") == 0) {
    printf("meshcore-frame\n");
    printf("meshcore_source: KrakenSaten/RIFT %s\n", MESHCORE_FRAME_RIFT_COMMIT);
    printf("crypto_source: rweather/arduinolibs %s\n", MESHCORE_FRAME_CRYPTO_COMMIT);
    printf("mtu_bytes: %d\n", MAX_TRANS_UNIT);
    printf("max_payload_bytes: %d\n", MAX_PACKET_PAYLOAD);
    return 0;
  }
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
    usage();
    return 0;
  }
  fprintf(stderr, "%s: unknown command %s\n", prog, cmd);
  return usage();
}
