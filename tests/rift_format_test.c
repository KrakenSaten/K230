/*
 * RIFT's words: every string the screens print, and the two rules the mesh
 * screens live or die by - a value nobody measured is never drawn as a
 * number, and a hop nobody named is never drawn as a hash.
 *
 * No LVGL, no service and no socket. What is checked here is the part of a
 * mesh UI that lies: a hop count printed as 0 because none was reported, an
 * RSSI attributed to a node nine relays away, a compressed path that hides
 * how many hops there were, a remote name cut in the middle of a character.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_format.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void text_is(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

/* A node with a path of `hops` relays, each one byte of hash. */
static void node_with_path(struct rift_node *n, int hops, const char *path_hex)
{
    memset(n, 0, sizeof(*n));
    snprintf(n->key, sizeof(n->key),
             "3f9ac21e7d0411223344556677889900aabbccddeeff0011223344556677" "88b1");
    snprintf(n->hash, sizeof(n->hash), "3f");
    n->have_name = 1;
    snprintf(n->name, sizeof(n->name), "HYTTA");
    n->path_known = 1;
    n->hops = hops;
    n->direct = hops == 0;
    snprintf(n->path_hex, sizeof(n->path_hex), "%s", path_hex ? path_hex : "");
}

static const char *resolve_rpt(const char *hop_id, void *user)
{
    (void)user;
    if (strcmp(hop_id, "a1") == 0) {
        return "RPT-NORD";
    }
    return NULL;
}

/* ---- UTF-8: checked, shown, and never cut through a character ------------ */
static void test_utf8_and_smileys(void)
{
    char out[64];
    char small[8];
    size_t n;

    check("ASCII is UTF-8", rift_utf8_valid("hei :)"));
    check("so are Latin-1 letters and an emoji",
          rift_utf8_valid("bl\xC3\xA5" "b\xC3\xA6r \xF0\x9F\x99\x82"));
    check("a stray continuation byte is not", !rift_utf8_valid("a\x80" "b"));
    check("nor a sequence cut short", !rift_utf8_valid("a\xF0\x9F\x99"));
    check("nor an overlong form", !rift_utf8_valid("\xC0\xAF"));
    check("nor a surrogate", !rift_utf8_valid("\xED\xA0\x80"));
    check("nor anything past U+10FFFF", !rift_utf8_valid("\xF4\x90\x80\x80"));
    check("nor a NULL", !rift_utf8_valid(NULL));

    rift_text_shown("hei \xF0\x9F\x99\x82", out, sizeof(out));
    text_is("a slight smile is drawn as :)", out, "hei :)");
    rift_text_shown("\xE2\x9D\xA4\xEF\xB8\x8F takk", out, sizeof(out));
    text_is("a heart with its variation selector is <3, the selector gone", out, "<3 takk");
    rift_text_shown("\xF0\x9F\x91\x8D\xF0\x9F\x98\x82", out, sizeof(out));
    text_is("a thumb and tears of joy", out, "(y):'D");
    rift_text_shown("\xF0\x9F\x9A\x80 ok", out, sizeof(out));
    text_is("an emoji with no smiley is left as it came", out, "\xF0\x9F\x9A\x80 ok");
    rift_text_shown("rpt \xE2\x98\x80\xEF\xB8\x8F", out, sizeof(out));
    text_is("and loses its variation selector, which would draw a second box", out,
            "rpt \xE2\x98\x80");
    rift_text_shown("a\xE2\x80\x8D\xEF\xB8\x8E" "b", out, sizeof(out));
    text_is("a zero-width joiner and a text selector are not drawn either", out, "ab");
    rift_text_shown("bl\xC3\xA5 \xE2\x80\xA6 \xE2\x86\x92", out, sizeof(out));
    text_is("letters and punctuation the fonts carry are untouched", out,
            "bl\xC3\xA5 \xE2\x80\xA6 \xE2\x86\x92");
    n = rift_text_shown("\xF0\x9F\x99\x82\xF0\x9F\x99\x82\xF0\x9F\x99\x82", small, sizeof(small));
    check("a short buffer stops before a smiley that does not fit", n == 6 &&
                                                                      strcmp(small, ":):):)") == 0);
    n = rift_text_shown("ab\xF0\x9F\x9A\x80\xF0\x9F\x9A\x80", small, sizeof(small));
    check("and never copies half a character", n == 6 && rift_utf8_valid(small));
    {
        char longest[RIFT_MSG_TEXT_MAX];
        char shown[RIFT_MSG_TEXT_MAX];
        int i;

        for (i = 0; i < 40; i++) {
            memcpy(longest + 4 * i, "\xF0\x9F\x98\x82", 4);
        }
        longest[160] = '\0';
        rift_text_shown(longest, shown, sizeof(shown));
        check("forty emoji in 160 bytes are drawn in no more than 160", strlen(shown) <= 160 &&
                                                                          strlen(shown) == 120);
    }

    /* The copy every remote string goes through cuts on a boundary. */
    rift_utf8_copy(small, 6, "ab\xF0\x9F\x99\x82" "c");
    text_is("a 4-byte character that does not fit is left out whole", small, "ab");
    rift_utf8_ellipsis(out, 8, "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5");
    check("and an ellipsis does not split one either", rift_utf8_valid(out));

    /* Names are remote text too. */
    {
        struct rift_node n;

        memset(&n, 0, sizeof(n));
        snprintf(n.key, sizeof(n.key), "%064d", 0);
        n.have_name = 1;
        snprintf(n.name, sizeof(n.name), "Hytta \xF0\x9F\x99\x82");
        rift_fmt_label(&n, out, sizeof(out));
        text_is("an emoji in a node's name is drawn as its smiley", out, "Hytta :)");
        check("and the name itself is untouched", strstr(n.name, "\xF0\x9F\x99\x82") != NULL);
    }
}

int main(void)
{
    char out[RIFT_CHAIN_MAX];
    char unit[8];
    struct rift_node n;
    struct rift_path p;
    struct rift_strip_cell cell[RIFT_STRIP_MAX];
    struct rift_ladder_row rung;
    int cells;

    /* ---- ages: a number and a unit, or "?" ------------------------------ */
    rift_fmt_age(0, 1, out, sizeof(out));
    text_is("no time at all is 0s", out, "0s");
    rift_fmt_age(59999, 1, out, sizeof(out));
    text_is("just under a minute is seconds", out, "59s");
    rift_fmt_age(60000, 1, out, sizeof(out));
    text_is("a minute is minutes", out, "1m");
    rift_fmt_age(12 * 60000, 1, out, sizeof(out));
    text_is("twelve minutes", out, "12m");
    rift_fmt_age(3600000, 1, out, sizeof(out));
    text_is("an hour", out, "1h");
    rift_fmt_age(5LL * 86400000, 1, out, sizeof(out));
    text_is("five days", out, "5d");
    rift_fmt_age(400LL * 86400000, 1, out, sizeof(out));
    text_is("past a hundred days the number stops being information", out, ">99d");
    rift_fmt_age(1000, 0, out, sizeof(out));
    text_is("an age nobody can measure is ?, not 0", out, RIFT_UNKNOWN);
    /* meshcored's monotonic clock ahead of ours can only be a fault, and a
     * fault is not a node heard in the future. */
    rift_fmt_age(-5000, 1, out, sizeof(out));
    text_is("a negative age is ?, never a time in the future", out, RIFT_UNKNOWN);

    rift_fmt_age_split(12 * 60000, 1, out, sizeof(out), unit, sizeof(unit));
    text_is("the detail panel's value", out, "12");
    text_is("and its unit in words", unit, "min");
    rift_fmt_age_split(0, 0, out, sizeof(out), unit, sizeof(unit));
    text_is("unknown splits to ?", out, RIFT_UNKNOWN);
    text_is("with no unit", unit, "");

    /* ---- signal: a real minus sign, one decimal, or "?" ------------------ */
    rift_fmt_rssi(-88.0, 1, out, sizeof(out));
    text_is("an RSSI uses U+2212, not a hyphen", out, RIFT_MINUS "88");
    rift_fmt_rssi(-88.4, 1, out, sizeof(out));
    text_is("and rounds to the nearest dBm", out, RIFT_MINUS "88");
    rift_fmt_rssi(-88.6, 1, out, sizeof(out));
    text_is("half up, away from zero", out, RIFT_MINUS "89");
    rift_fmt_rssi(0.0, 0, out, sizeof(out));
    text_is("an RSSI nobody measured is ?, never 0", out, RIFT_UNKNOWN);
    rift_fmt_snr(6.0, 1, out, sizeof(out));
    text_is("an SNR keeps one decimal", out, "6.0");
    rift_fmt_snr(-7.55, 1, out, sizeof(out));
    text_is("a negative SNR is negative", out, RIFT_MINUS "7.6");
    rift_fmt_snr(-0.02, 1, out, sizeof(out));
    text_is("and a value that rounds to zero is not printed as minus zero", out, "0.0");
    rift_fmt_snr(0.0, 0, out, sizeof(out));
    text_is("an SNR nobody measured is ?", out, RIFT_UNKNOWN);

    /* ---- the hop column ------------------------------------------------- */
    node_with_path(&n, 0, "");
    rift_fmt_hops(&n, out, sizeof(out));
    text_is("no relays is DIR, never 0", out, "DIR");
    rift_fmt_state(&n, out, sizeof(out));
    text_is("and the state word is DIRECT", out, "DIRECT");
    check("and the link is direct", rift_link_of(&n) == RIFT_LINK_DIRECT);

    node_with_path(&n, 3, "a1b2c3");
    rift_fmt_hops(&n, out, sizeof(out));
    text_is("relays are counted", out, "3");
    rift_fmt_state(&n, out, sizeof(out));
    text_is("state, hop count, and no uncertainty when there is none", out,
            "RELAYED" RIFT_SEP "3 HOPS");

    memset(&n, 0, sizeof(n));
    snprintf(n.hash, sizeof(n.hash), "c2");
    rift_fmt_hops(&n, out, sizeof(out));
    text_is("no route back is ?, never 0 hops", out, RIFT_UNKNOWN);
    rift_fmt_state(&n, out, sizeof(out));
    text_is("and the state word says so", out, "NO PATH");
    check("and the link is unknown", rift_link_of(&n) == RIFT_LINK_UNKNOWN);
    rift_fmt_label(&n, out, sizeof(out));
    text_is("a node with no name is called by its hash", out, "c2");

    /* ---- a path that cannot be divided into hops ------------------------- */
    /* Seven bytes over three hops divides into nothing: MeshCore packs a
     * whole number of bytes per hop, so this is a clipped or disagreeing
     * path. The hops are real; none of them can be named. */
    node_with_path(&n, 3, "a1b2c3d4e5f607");
    check("a path that does not divide is taken apart without error",
          rift_path_parse(&n, &p) == 0);
    check("the hops are still real", p.hops == 3);
    check("and every one of them is unknown", p.unknown == 3 && p.identified == 0);
    rift_fmt_state(&n, out, sizeof(out));
    text_is("which the state line says, in the fixed order", out,
            "RELAYED" RIFT_SEP "3 HOPS" RIFT_SEP "3 UNKNOWN HOPS");

    node_with_path(&n, 2, "");
    check("path_known with no bytes parses", rift_path_parse(&n, &p) == 0);
    check("and reports both hops unknown", p.hops == 2 && p.unknown == 2);

    node_with_path(&n, 2, "a1b2c");
    check("an odd number of hex characters is not a byte string",
          rift_path_parse(&n, &p) == -1);
    node_with_path(&n, 2, "a1zz");
    check("and neither is one with a character that is not hex",
          rift_path_parse(&n, &p) == -1);

    /* Two bytes per hop is what pathHashSize gives for a longer path. */
    node_with_path(&n, 2, "a1b2c3d4");
    check("two bytes a hop divides exactly", rift_path_parse(&n, &p) == 0);
    check("and both hops are named", p.identified == 2 && p.unknown == 0);
    check("with two-byte identifiers", strcmp(p.hop[0].id, "a1b2") == 0 &&
                                           strcmp(p.hop[1].id, "c3d4") == 0);

    /* ---- the strip compresses; the count never does ---------------------- */
    node_with_path(&n, 4, "a1b2c3d4");
    rift_path_parse(&n, &p);
    cells = rift_strip_build(&p, cell, RIFT_STRIP_MAX);
    check("four relays are all drawn", cells == 6);
    check("self first", cell[0].kind == RIFT_CELL_SELF);
    check("target last", cell[5].kind == RIFT_CELL_TARGET);
    check("and nothing is folded away", cell[1].kind != RIFT_CELL_MORE &&
                                            cell[4].kind != RIFT_CELL_MORE);

    node_with_path(&n, 8, "a1b2c3d4e5f60708");
    rift_path_parse(&n, &p);
    cells = rift_strip_build(&p, cell, RIFT_STRIP_MAX);
    check("eight relays compress to six cells", cells == 6);
    check("the first two relays are drawn", cell[1].hop == 0 && cell[2].hop == 1);
    check("then a +n cell", cell[3].kind == RIFT_CELL_MORE);
    check("folding the five in the middle", cell[3].more == 5);
    check("then the last relay", cell[4].hop == 7);
    check("and the target", cell[5].kind == RIFT_CELL_TARGET);
    rift_fmt_hops(&n, out, sizeof(out));
    text_is("while the numeric hop count is printed whole", out, "8");

    memset(&n, 0, sizeof(n));
    rift_path_parse(&n, &p);
    cells = rift_strip_build(&p, cell, RIFT_STRIP_MAX);
    check("no path is self and target and nothing between", cells == 2);
    check("with the target drawn as the unknown it is", cell[1].kind == RIFT_CELL_TARGET);

    /* ---- the inline chain ------------------------------------------------ */
    node_with_path(&n, 3, "a1b2c3");
    rift_path_parse(&n, &p);
    rift_path_chain("K230", &p, "HYTTA", resolve_rpt, NULL, out, sizeof(out));
    text_is("a resolved hop is named, an unresolved one is its hash", out,
            "K230" RIFT_ARROW "RPT-NORD" RIFT_ARROW "b2" RIFT_ARROW "c3" RIFT_ARROW "HYTTA");
    node_with_path(&n, 2, "");
    rift_path_parse(&n, &p);
    rift_path_chain("K230", &p, "HYTTA", resolve_rpt, NULL, out, sizeof(out));
    text_is("a hop the path does not name is ?, never a plausible hash", out,
            "K230" RIFT_ARROW RIFT_UNKNOWN RIFT_ARROW RIFT_UNKNOWN RIFT_ARROW "HYTTA");
    memset(&n, 0, sizeof(n));
    rift_path_parse(&n, &p);
    rift_path_chain("K230", &p, "HYTTA", resolve_rpt, NULL, out, sizeof(out));
    text_is("and with no path at all the chain says so rather than joining them", out,
            "K230" RIFT_ARROW RIFT_UNKNOWN RIFT_ARROW "HYTTA");

    /* A long path is not truncated in the data; only the rendering stops at
     * the buffer, and it stops without running past it. */
    {
        char longpath[RIFT_PATH_HEX_MAX];
        char small[24];
        int i;

        for (i = 0; i < 60; i++) {
            longpath[i * 2] = 'a';
            longpath[i * 2 + 1] = (char)('0' + (i % 10));
        }
        longpath[120] = '\0';
        node_with_path(&n, 60, longpath);
        check("a sixty-hop path parses", rift_path_parse(&n, &p) == 0);
        check("with all sixty named", p.identified == 60 && p.unknown == 0);
        cells = rift_strip_build(&p, cell, RIFT_STRIP_MAX);
        check("and still draws six cells", cells == 6);
        check("folding fifty-seven of them", cell[3].more == 57);
        rift_path_chain("K230", &p, "HYTTA", NULL, NULL, small, sizeof(small));
        check("a chain written into a short buffer stays inside it",
              strlen(small) < sizeof(small));
    }

    /* ---- the hop ladder --------------------------------------------------- */
    node_with_path(&n, 2, "a1b2");
    rift_path_parse(&n, &p);
    check("row 0 is this device", rift_path_ladder_row(&p, 0, "K230", "HYTTA", resolve_rpt,
                                                       NULL, &rung) == 0 &&
                                      rung.kind == RIFT_CELL_SELF);
    text_is("named as this device", rung.label, "K230");
    check("row 1 is the first relay", rift_path_ladder_row(&p, 1, "K230", "HYTTA", resolve_rpt,
                                                           NULL, &rung) == 0 &&
                                          rung.kind == RIFT_CELL_RELAY);
    text_is("resolved to a node this device knows", rung.label, "RPT-NORD");
    text_is("and said to be one", rung.note, "known node");
    check("row 2 is the relay nobody here knows",
          rift_path_ladder_row(&p, 2, "K230", "HYTTA", resolve_rpt, NULL, &rung) == 0);
    text_is("shown as its hash", rung.label, "b2");
    text_is("and said to be only that", rung.note, "hash only");
    check("row 3 is the target", rift_path_ladder_row(&p, 3, "K230", "HYTTA", resolve_rpt, NULL,
                                                      &rung) == 0 &&
                                     rung.kind == RIFT_CELL_TARGET);
    check("and there is no row 4",
          rift_path_ladder_row(&p, 4, "K230", "HYTTA", resolve_rpt, NULL, &rung) == -1);

    /* ---- names chosen by whoever is on the air ---------------------------- */
    {
        char narrow[10];

        /* æøå: two bytes each. Cutting one in half would leave text that is
         * no longer text, which is exactly what the field must not do. */
        rift_utf8_copy(narrow, sizeof(narrow), "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8");
        check("a name is never cut inside a character", strlen(narrow) % 2 == 0);
        text_is("it stops at the last whole one", narrow, "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6");
        rift_utf8_copy(narrow, sizeof(narrow), "hei");
        text_is("a name that fits is unchanged", narrow, "hei");
        rift_utf8_copy(narrow, sizeof(narrow), NULL);
        text_is("and no name at all is empty, not a crash", narrow, "");

        /* A four-byte character, and a sequence the sender cut short. */
        rift_utf8_copy(narrow, sizeof(narrow), "ab\xF0\x9F\x93\x9D\xF0\x9F\x93\x9D");
        text_is("a four-byte character is kept whole or not at all", narrow,
                "ab\xF0\x9F\x93\x9D");
        rift_utf8_copy(narrow, sizeof(narrow), "ab\xE2\x82");
        text_is("a sequence cut short at the source is not copied half", narrow, "ab");

        rift_utf8_ellipsis(narrow, sizeof(narrow), "abcdefghijklmnop");
        check("a shortened name is visibly shortened",
              strstr(narrow, RIFT_ELLIPSIS) != NULL && strlen(narrow) < sizeof(narrow));
        rift_utf8_ellipsis(narrow, sizeof(narrow), "abc");
        text_is("and one that fits gains nothing", narrow, "abc");
    }

    memset(&n, 0, sizeof(n));
    n.have_name = 1;
    snprintf(n.name, sizeof(n.name), "%s", "a name far longer than any column on this panel");
    rift_fmt_label(&n, out, 12);
    check("a long name in a short field ends in an ellipsis",
          strstr(out, RIFT_ELLIPSIS) != NULL);

    /* ---- the key, and the role -------------------------------------------- */
    rift_fmt_key_short("3f9ac21e7d0411223344556677889900aabbccddeeff00112233445566778"
                       "88b1",
                       out, sizeof(out));
    text_is("a key is four groups, an ellipsis and the last four", out,
            "3F9A C21E 7D04 " RIFT_ELLIPSIS " 88B1");
    rift_fmt_key_short("3f9a", out, sizeof(out));
    text_is("something shorter than a key is printed as far as it goes", out, "3F9A");
    rift_fmt_key_short(NULL, out, sizeof(out));
    text_is("and nothing is nothing", out, "");

    check("a repeater has a tag", rift_type_tag(2, 1) && strcmp(rift_type_tag(2, 1), "RPT") == 0);
    check("a chat node does not", rift_type_tag(1, 1) == NULL);
    check("a type nobody reported has no word", rift_type_word(0, 0) == NULL);
    check("and a number this build has no word for is not given one",
          rift_type_word(99, 1) == NULL);

    /* ---- stale is about time, not about the link --------------------------- */
    memset(&n, 0, sizeof(n));
    check("a node never heard is not stale; it is unheard",
          !rift_node_is_stale(&n, 100000000));
    n.have_heard = 1;
    n.heard_mono_ms = 0;
    check("heard longer ago than the boundary is stale",
          rift_node_is_stale(&n, RIFT_STALE_MS + 1));
    check("heard inside it is not", !rift_node_is_stale(&n, RIFT_STALE_MS - 1));
    n.heard_mono_ms = 5000;
    check("and a clock that says it was heard in the future is not evidence of staleness",
          !rift_node_is_stale(&n, 1000));

    /* ---- identity accents: who gets one, and the same one every time ---- */
    check("a chat node is somebody", rift_ident_for_type(1, 1));
    check("so is a room", rift_ident_for_type(3, 1));
    check("a repeater is not", !rift_ident_for_type(2, 1));
    check("nor a sensor", !rift_ident_for_type(4, 1));
    check("nor a node whose type nobody reported", !rift_ident_for_type(1, 0));
    check("the hash is FNV-1a, so any device hashes a key the same way",
          rift_ident_hash("") == 2166136261u && rift_ident_hash("a") == 0xe40c292cu);
    check("the same name is the same hash", rift_ident_hash("HYTTA") == rift_ident_hash("HYTTA"));
    check("and a different one is not", rift_ident_hash("HYTTA") != rift_ident_hash("HYTTB"));
    check("nothing hashes to nothing", rift_ident_hash(NULL) == 0);

    test_utf8_and_smileys();
    printf("rift_format_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
