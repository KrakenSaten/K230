/*
 * gatewayd unit suite, the capture group (services/gatewayd/gw_capture.h;
 * ADR-016 amendment 1; ADR-019 rule 6). Included by gatewayd_unit_test.c.
 *
 * What is checked: an answer of the unit is rebuilt from the contract's
 * fields and nothing of it is passed on; parameters are checked and written
 * again and a client can name neither who asks nor what a request is
 * called; the four rows, their scopes, and that the gateway's own cancel is
 * no row; the topic; that a state is said once per change and figures only
 * of the state that was said; the waiting request: whose it is, that only
 * an answer asked for after its sheet was up speaks for it, how it ends,
 * and how often a client may ask.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */

static const char *const CAP_TOP[] = {"available", "state",        "reason",      "iface",    "keep",   "elapsed_ms",
                                      "limit_s",   "seen",         "stored",      "held",     "kernel_drops",
                                      "iface_drops", "shed",       "bytes",       "pps",      "bps",    "fill_permille",
                                      "classes",   "findings",     "diag",        "requested_by_companion", NULL};

static int cap_keys_are(const cJSON *o, const char *const *want)
{
    const cJSON *el;
    int i = 0;

    cJSON_ArrayForEach(el, o) {
        if (want[i] == NULL || el->string == NULL || strcmp(el->string, want[i]) != 0) {
            return 0;
        }
        i++;
    }
    return want[i] == NULL;
}

static const char *cap_s(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : "(none)";
}

static double cap_n(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

/* A status as the unit gives it, with a capture in `state`. */
static cJSON *cap_raw(const char *state, const char *reason, const char *request, double seen, int by_companion)
{
    char buf[1400];

    snprintf(buf, sizeof(buf),
             "{\"available\":true,\"state\":\"%s\",\"reason\":\"%s\",\"iface\":\"eth0\",\"keep\":\"headers\","
             "\"elapsed_ms\":61250,\"limit_s\":900,\"seen\":%.0f,\"stored\":48000,\"held\":0,\"kernel_drops\":0,"
             "\"iface_drops\":0,\"shed\":0,\"bytes\":31022764,\"pps\":812,\"bps\":4120000,\"fill_permille\":310,"
             "\"classes\":{\"tcp\":39120,\"udp\":5210,\"dns\":1630,\"dhcp\":4,\"arp\":61,\"icmp\":12,\"ipv6\":2101,"
             "\"other\":73},\"findings\":[0,3,0,1,0,0],\"diag\":{\"dns_queries\":815,\"tcp_aborted\":2},"
             "\"requested_by_companion\":%s,\"request\":\"%s\"}",
             state, reason, seen, by_companion ? "true" : "false", request);
    return parse(buf);
}

static void test_capture(void)
{
    static const char *const top_false[] = {"available", NULL};
    static const char *const state_keys[] = {"state", "reason", "iface", "keep", "limit_s", NULL};
    static const char *const cmd_keys[] = {"state", "changing", NULL};
    static const char *const shown_keys[] = {"shown", NULL};
    const struct gw_method *m;
    struct gw_capture c;
    uint8_t fa[GW_FP_LEN], fb[GW_FP_LEN], fx[GW_FP_LEN];
    cJSON *a, *o, *st, *stats;
    const char *outcome;
    char *text;
    unsigned mask;
    int i;

    memset(fa, 0xa1, sizeof(fa));
    memset(fb, 0xb2, sizeof(fb));

    /* ---- the status: rebuilt, never passed on -------------------------------- */
    a = parse("{\"available\":true,\"state\":\"capturing\",\"reason\":\"none\",\"iface\":\"enx001122aabbcc\","
              "\"keep\":\"full\",\"elapsed_ms\":61250,\"limit_s\":900,\"seen\":48211,\"stored\":-5,\"held\":1.5,"
              "\"kernel_drops\":\"7\",\"iface_drops\":1e300,\"shed\":0,\"bytes\":31022764,\"pps\":812,\"bps\":4120000,"
              "\"fill_permille\":5000,\"classes\":{\"tcp\":39120,\"udp\":\"x\",\"hosts\":[\"192.0.2.7\"]},"
              "\"findings\":[0,3,\"LEAK\",1,0,0,99,99],\"diag\":{\"dns_queries\":815,\"dns_rtt_max_seq\":4242,"
              "\"slowest\":\"LEAK.example\"},\"requested_by_companion\":true,\"request\":\"shown\","
              "\"rows\":[{\"src\":\"192.0.2.7\",\"info\":\"LEAK\"}],\"client\":\"LEAK\",\"session_key\":\"LEAK\","
              "\"save_name\":\"capture-LEAK.pcap\",\"version\":\"LEAK\"}");
    o = gw_capture_status(a);
    text = cJSON_PrintUnformatted(o);
    CHECK(o != NULL && cap_keys_are(o, CAP_TOP));
    CHECK(strstr(text, "LEAK") == NULL && strstr(text, "192.0.2") == NULL && strstr(text, "enx") == NULL &&
          strstr(text, "4242") == NULL && strstr(text, "pcap") == NULL && strstr(text, "shown") == NULL &&
          strstr(text, "rows") == NULL);
    CHECK(strcmp(cap_s(o, "state"), "capturing") == 0 && strcmp(cap_s(o, "iface"), "") == 0 &&
          strcmp(cap_s(o, "keep"), "full") == 0);
    /* A figure that is not a whole number in range is zero, not passed on. */
    CHECK(cap_n(o, "seen") == 48211 && cap_n(o, "stored") == 0 && cap_n(o, "held") == 0 &&
          cap_n(o, "kernel_drops") == 0 && cap_n(o, "iface_drops") == 0 && cap_n(o, "fill_permille") == 1000);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(o, "classes")) == 8 &&
          cap_n(cJSON_GetObjectItem(o, "classes"), "tcp") == 39120 && cap_n(cJSON_GetObjectItem(o, "classes"), "udp") == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(o, "findings")) == 6 &&
          cJSON_GetArrayItem(cJSON_GetObjectItem(o, "findings"), 2)->valuedouble == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(o, "diag")) == 18 &&
          cap_n(cJSON_GetObjectItem(o, "diag"), "dns_queries") == 815 &&
          cJSON_GetObjectItem(cJSON_GetObjectItem(o, "diag"), "dns_rtt_max_seq") == NULL);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(o, "requested_by_companion")));
    cJSON_free(text);
    cJSON_Delete(o);
    cJSON_Delete(a);
    /* Words that are not of the lists become the plain ones. */
    a = parse("{\"available\":true,\"state\":\"192.0.2.7\",\"reason\":\"because LEAK\",\"iface\":7,\"keep\":\"all\"}");
    o = gw_capture_status(a);
    CHECK(strcmp(cap_s(o, "state"), "idle") == 0 && strcmp(cap_s(o, "reason"), "none") == 0 &&
          strcmp(cap_s(o, "iface"), "") == 0 && strcmp(cap_s(o, "keep"), "headers") == 0 && cap_keys_are(o, CAP_TOP));
    cJSON_Delete(o);
    cJSON_Delete(a);
    {
        static const char *const names[] = {"eth0", "wlan12", "usb0", "synth0"};
        static const char *const not_names[] = {"enx001122aabbcc", "wlx001122aabbcc", "Johns-iPhone", "eth", "eth123",
                                                "eth0a", "lo", "tun0", "ETH0", ""};
        char buf[120];
        int ok = 1;

        for (i = 0; i < 4; i++) {
            snprintf(buf, sizeof(buf), "{\"available\":true,\"iface\":\"%s\"}", names[i]);
            a = parse(buf);
            o = gw_capture_status(a);
            ok = ok && strcmp(cap_s(o, "iface"), names[i]) == 0;
            cJSON_Delete(o);
            cJSON_Delete(a);
        }
        for (i = 0; i < 10; i++) {
            snprintf(buf, sizeof(buf), "{\"available\":true,\"iface\":\"%s\"}", not_names[i]);
            a = parse(buf);
            o = gw_capture_status(a);
            ok = ok && strcmp(cap_s(o, "iface"), "") == 0;
            cJSON_Delete(o);
            cJSON_Delete(a);
        }
        CHECK(ok);
    }
    /* Not available, or not said: that and nothing else. */
    a = parse("{\"available\":false,\"state\":\"capturing\",\"seen\":5,\"rows\":[\"LEAK\"]}");
    o = gw_capture_status(a);
    CHECK(o != NULL && cap_keys_are(o, top_false) && cJSON_IsFalse(cJSON_GetObjectItem(o, "available")));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("{\"state\":\"capturing\",\"seen\":5}");
    o = gw_capture_status(a);
    CHECK(o != NULL && cap_keys_are(o, top_false));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("{\"available\":\"true\",\"seen\":5}");
    o = gw_capture_status(a);
    CHECK(o != NULL && cap_keys_are(o, top_false));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("[1,2]");
    CHECK(gw_capture_status(a) == NULL && gw_capture_status(NULL) == NULL && gw_capture_command(a) == NULL &&
          gw_capture_shown(a) == NULL);
    cJSON_Delete(a);

    /* ---- the other answers ----------------------------------------------------- */
    a = parse("{\"state\":\"hold\",\"changing\":true,\"helper_pid\":77,\"note\":\"LEAK\"}");
    o = gw_capture_command(a);
    CHECK(o != NULL && cap_keys_are(o, cmd_keys) && strcmp(cap_s(o, "state"), "hold") == 0 &&
          cJSON_IsTrue(cJSON_GetObjectItem(o, "changing")));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("{\"state\":\"hold\"}");
    CHECK(gw_capture_command(a) == NULL);
    cJSON_Delete(a);
    a = parse("{\"shown\":true,\"request\":\"0011223344556677\",\"client\":\"LEAK\"}");
    o = gw_capture_shown(a);
    CHECK(o != NULL && cap_keys_are(o, shown_keys));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("{\"shown\":false}");
    CHECK(gw_capture_shown(a) == NULL);
    cJSON_Delete(a);

    /* ---- parameters: checked, and written again ----------------------------------- */
    a = parse("{\"on\":true}");
    CHECK(gw_capture_params_hold(a, &o) == 0 && cJSON_GetArraySize(o) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(o, "on")));
    cJSON_Delete(o);
    cJSON_Delete(a);
    a = parse("{\"on\":false}");
    CHECK(gw_capture_params_hold(a, &o) == 0 && cJSON_IsFalse(cJSON_GetObjectItem(o, "on")));
    cJSON_Delete(o);
    cJSON_Delete(a);
    {
        static const char *const bad[] = {"{}", "{\"on\":1}", "{\"on\":\"true\"}", "{\"on\":true,\"request\":\"aa\"}",
                                          "{\"on\":null}", "[true]", "{\"ON\":true}"};
        int ok = gw_capture_params_hold(NULL, &o) != 0 && o == NULL;

        for (i = 0; i < 7; i++) {
            a = parse(bad[i]);
            ok = ok && gw_capture_params_hold(a, &o) != 0 && o == NULL;
            cJSON_Delete(a);
        }
        CHECK(ok);
    }
    a = parse("{\"iface\":\"eth0\",\"limit_s\":900}");
    CHECK(gw_capture_params_ask(a, &o) == 0 && cJSON_GetArraySize(o) == 2 && strcmp(cap_s(o, "iface"), "eth0") == 0 &&
          cap_n(o, "limit_s") == 900);
    cJSON_Delete(o);
    cJSON_Delete(a);
    {
        /* Who asks, what the request is called, FULL, no limit, a filter:
         * none of it is a client's to say. */
        static const char *const bad[] = {
            "{\"iface\":\"eth0\",\"limit_s\":900,\"client\":\"Flip8\"}",
            "{\"iface\":\"eth0\",\"limit_s\":900,\"request\":\"0011223344556677\"}",
            "{\"iface\":\"eth0\",\"limit_s\":900,\"keep\":\"full\"}",
            "{\"iface\":\"eth0\",\"limit_s\":900,\"promisc\":true}",
            "{\"iface\":\"eth0\",\"limit_s\":0}",
            "{\"iface\":\"eth0\",\"limit_s\":120}",
            "{\"iface\":\"eth0\",\"limit_s\":\"900\"}",
            "{\"iface\":\"eth0\",\"limit_s\":900.5}",
            "{\"iface\":\"eth0\"}",
            "{\"limit_s\":900}",
            "{\"iface\":\"\",\"limit_s\":900}",
            "{\"iface\":\"Eth0\",\"limit_s\":900}",
            "{\"iface\":\"eth0; rm\",\"limit_s\":900}",
            "{\"iface\":\"abcdefghijklmnop\",\"limit_s\":900}",
            /* A name the status would not show cannot be asked for either:
             * asking is no way to find out whether it exists. */
            "{\"iface\":\"enx001122aabbcc\",\"limit_s\":900}",
            "{\"iface\":\"wlx001122aabbcc\",\"limit_s\":900}",
            "{\"iface\":\"br0\",\"limit_s\":900}",
            "{\"iface\":7,\"limit_s\":900}",
            "{}",
            "[\"eth0\",900]",
        };
        int ok = gw_capture_params_ask(NULL, &o) != 0 && o == NULL;

        for (i = 0; i < 20; i++) {
            a = parse(bad[i]);
            ok = ok && gw_capture_params_ask(a, &o) != 0 && o == NULL;
            cJSON_Delete(a);
        }
        CHECK(ok);
    }
    {
        static const double limits[] = {300, 900, 3600};
        char buf[80];
        int ok = 1;

        for (i = 0; i < 3; i++) {
            snprintf(buf, sizeof(buf), "{\"iface\":\"wlan0\",\"limit_s\":%.0f}", limits[i]);
            a = parse(buf);
            ok = ok && gw_capture_params_ask(a, &o) == 0;
            cJSON_Delete(o);
            cJSON_Delete(a);
        }
        CHECK(ok);
    }

    /* ---- the rows: four, each with its scope; the cancel is no row ------------------- */
    m = gw_method_find("capture.status");
    CHECK(m != NULL && m->kind == GW_K_CAPTURE && m->scope == GW_S_CAPTURE_VIEW && strcmp(m->service, "shell") == 0 &&
          m->filter == gw_capture_status && m->params == NULL);
    CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_CAPTURE_VIEW) == GW_V_OK &&
          gw_policy_check(m, GW_W_PAIRED, GW_S_DEFAULT) == GW_V_FORBIDDEN &&
          gw_policy_check(m, GW_W_PAIRED, GW_S_ALL & ~GW_S_CAPTURE_VIEW & ~GW_S_CAPTURE_CONTROL) == GW_V_FORBIDDEN &&
          gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
    {
        static const char *const control[] = {"capture.hold", "capture.stop", "capture.request_start"};
        int ok = 1;

        for (i = 0; i < 3; i++) {
            m = gw_method_find(control[i]);
            ok = ok && m != NULL && m->kind == GW_K_CAPTURE && m->scope == GW_S_CAPTURE_CONTROL &&
                 strcmp(m->service, "shell") == 0 && m->filter != NULL &&
                 gw_policy_check(m, GW_W_PAIRED, GW_S_CAPTURE_VIEW | GW_S_CAPTURE_CONTROL) == GW_V_OK &&
                 /* Reading alone does not hold, stop or ask. */
                 gw_policy_check(m, GW_W_PAIRED, GW_S_CAPTURE_VIEW) == GW_V_FORBIDDEN &&
                 gw_policy_check(m, GW_W_PAIRED, GW_S_DEFAULT) == GW_V_FORBIDDEN &&
                 gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN;
        }
        CHECK(ok);
        CHECK(gw_method_find("capture.hold")->params == gw_capture_params_hold &&
              gw_method_find("capture.request_start")->params == gw_capture_params_ask &&
              gw_method_find("capture.stop")->params == NULL);
    }
    /* The gateway's own cancel, and everything else of a capture, is not a
     * method a client can name. */
    CHECK(gw_method_find("capture.request_cancel") == NULL && gw_method_find("capture.state") == NULL &&
          gw_method_find("capture.stats") == NULL && gw_method_find("capture.start") == NULL &&
          gw_method_find("capture.discard") == NULL && gw_method_find("capture.save") == NULL &&
          gw_method_find("capture.rows") == NULL && gw_method_find("shell.subscribe") == NULL &&
          gw_method_find("shell.open") == NULL);
    {
        size_t count, k;
        int capture_rows = 0, shell_rows = 0;
        const struct gw_method *all = gw_methods(&count);

        for (k = 0; k < count; k++) {
            capture_rows += strncmp(all[k].name, "capture.", 8) == 0;
            shell_rows += all[k].service != NULL && strcmp(all[k].service, "shell") == 0;
            CHECK(strncmp(all[k].name, "shell.", 6) != 0);
        }
        CHECK(capture_rows == 4 && shell_rows == 4);
    }
    /* A scope set with control and no view is not one a client can hold. */
    CHECK(!gw_scopes_valid(GW_S_CAPTURE_CONTROL) && gw_scopes_valid(GW_S_CAPTURE_VIEW | GW_S_CAPTURE_CONTROL));

    /* ---- the topic ---------------------------------------------------------------------- */
    a = parse("{\"topics\":[\"capture\"]}");
    CHECK(gw_topics_parse(a, GW_S_CAPTURE_VIEW, &mask) == 0 && mask == GW_T_CAPTURE);
    CHECK(gw_topics_parse(a, GW_S_DEFAULT, &mask) == 0 && mask == 0);
    CHECK(gw_topics_parse(a, GW_S_ALL & ~GW_S_CAPTURE_VIEW, &mask) == 0 && mask == 0);
    cJSON_Delete(a);
    CHECK(gw_topics_allowed(GW_T_ALL, GW_S_CAPTURE_VIEW) == GW_T_CAPTURE &&
          gw_topics_allowed(GW_T_ALL, GW_S_MESH_READ) == GW_T_MESH_ALL && (GW_T_MESH_ALL & GW_T_CAPTURE) == 0);

    /* ---- a state once per change, figures only of the state that was said ------------------ */
    gw_capture_init(&c);
    a = cap_raw("idle", "none", "none", 0, 0);
    gw_capture_step(&c, a, 100, &st, &stats, &outcome);
    CHECK(st != NULL && cap_keys_are(st, state_keys) && strcmp(cap_s(st, "state"), "idle") == 0 && stats == NULL &&
          outcome == NULL);
    cJSON_Delete(st);
    gw_capture_step(&c, a, 1100, &st, &stats, &outcome);
    CHECK(st == NULL && stats == NULL); /* setup, said: nothing more */
    cJSON_Delete(a);
    /* The unit's passing reason on setup is not a change of state. */
    a = cap_raw("idle", "refused", "refused", 0, 0);
    gw_capture_step(&c, a, 2100, &st, &stats, &outcome);
    CHECK(st == NULL && stats == NULL && outcome == NULL);
    cJSON_Delete(a);
    a = cap_raw("capturing", "none", "none", 100, 0);
    gw_capture_step(&c, a, 3100, &st, &stats, &outcome);
    CHECK(st != NULL && strcmp(cap_s(st, "state"), "capturing") == 0 && stats != NULL && cap_keys_are(stats, CAP_TOP) &&
          strcmp(cap_s(stats, "state"), "capturing") == 0 && cap_n(stats, "seen") == 100);
    CHECK(cJSON_GetObjectItem(stats, "request") == NULL); /* the unit's word for a request is not passed on */
    o = gw_capture_told(&c);
    CHECK(o != NULL && cap_keys_are(o, state_keys) && strcmp(cap_s(o, "state"), "capturing") == 0 && cap_n(o, "limit_s") == 900);
    cJSON_Delete(o);
    cJSON_Delete(st);
    cJSON_Delete(stats);
    cJSON_Delete(a);
    a = cap_raw("capturing", "none", "none", 900, 0);
    gw_capture_step(&c, a, 4100, &st, &stats, &outcome);
    CHECK(st == NULL && stats != NULL && cap_n(stats, "seen") == 900); /* running: figures with every answer */
    cJSON_Delete(stats);
    cJSON_Delete(a);
    a = cap_raw("stopped", "user", "none", 950, 0);
    gw_capture_step(&c, a, 5100, &st, &stats, &outcome);
    CHECK(st != NULL && strcmp(cap_s(st, "reason"), "user") == 0 && stats != NULL && cap_n(stats, "seen") == 950);
    cJSON_Delete(st);
    cJSON_Delete(stats);
    gw_capture_step(&c, a, 6100, &st, &stats, &outcome);
    CHECK(st == NULL && stats == NULL); /* a stopped capture's totals went out once */
    cJSON_Delete(a);
    /* PacketShark is left: the capture clients knew of has a last word. */
    a = parse("{\"available\":false}");
    gw_capture_step(&c, a, 7100, &st, &stats, &outcome);
    CHECK(st != NULL && strcmp(cap_s(st, "state"), "ended") == 0 && strcmp(cap_s(st, "reason"), "left") == 0 &&
          strcmp(cap_s(st, "iface"), "eth0") == 0 && stats == NULL);
    cJSON_Delete(st);
    gw_capture_step(&c, a, 8100, &st, &stats, &outcome);
    CHECK(st == NULL && stats == NULL);
    cJSON_Delete(a);
    /* And when it is there again, setup is said again. */
    a = cap_raw("idle", "none", "none", 0, 0);
    gw_capture_step(&c, a, 9100, &st, &stats, &outcome);
    CHECK(st != NULL && strcmp(cap_s(st, "state"), "idle") == 0);
    cJSON_Delete(st);
    cJSON_Delete(a);
    gw_capture_lost(&c, &st);
    CHECK(st == NULL && gw_capture_told(&c) == NULL); /* nothing was running: nothing to end, and nothing known */
    /* An answer that is no object changes nothing. */
    a = parse("[]");
    gw_capture_step(&c, a, 9200, &st, &stats, &outcome);
    CHECK(st == NULL && stats == NULL && outcome == NULL);
    cJSON_Delete(a);

    /* ---- the waiting request ------------------------------------------------------------------ */
    gw_capture_init(&c);
    CHECK(gw_capture_may_ask(&c, fa, 1000) == GW_ASK_OK);
    gw_capture_ask(&c, fa, 7, "00000000000000a1", 1000);
    CHECK(c.asking && !c.shown && c.sid == 7 && memcmp(c.fp, fa, GW_FP_LEN) == 0);
    /* One at a time, whoever asks. */
    CHECK(gw_capture_may_ask(&c, fb, 1100) == GW_ASK_BUSY && gw_capture_may_ask(&c, fa, 1100) == GW_ASK_BUSY);
    /* Until the unit said its sheet is up, no answer speaks for the
     * request: this one was asked before, and tells of an older request. */
    a = cap_raw("idle", "refused", "refused", 0, 0);
    gw_capture_step(&c, a, 900, &st, &stats, &outcome);
    cJSON_Delete(st);
    CHECK(outcome == NULL && c.asking);
    gw_capture_ask_shown(&c, 1200);
    CHECK(c.shown && c.shown_ms == 1200);
    /* An answer asked for before that still tells of the older request. */
    gw_capture_step(&c, a, 1150, &st, &stats, &outcome);
    CHECK(outcome == NULL && c.asking);
    cJSON_Delete(a);
    a = cap_raw("idle", "none", "shown", 0, 0);
    gw_capture_step(&c, a, 1300, &st, &stats, &outcome);
    CHECK(outcome == NULL && c.asking); /* up, and waiting for a finger */
    cJSON_Delete(a);
    /* START was tapped and the capture is starting: the sheet is down, the
     * unit says neither refused nor timeout, the state is still setup. That
     * is not a refusal; the request waits for the capture. */
    a = cap_raw("idle", "none", "none", 0, 0);
    gw_capture_step(&c, a, 1400, &st, &stats, &outcome);
    CHECK(outcome == NULL && c.asking && st == NULL);
    cJSON_Delete(a);
    /* Nor is an answer of an older PacketShark that has no word for it. */
    a = parse("{\"available\":true,\"state\":\"idle\",\"reason\":\"none\"}");
    gw_capture_step(&c, a, 1500, &st, &stats, &outcome);
    CHECK(outcome == NULL && c.asking);
    cJSON_Delete(st); /* (its interface and limit differ: a change of state of its own) */
    cJSON_Delete(a);
    a = cap_raw("idle", "refused", "refused", 0, 0);
    gw_capture_step(&c, a, 2300, &st, &stats, &outcome);
    CHECK(outcome != NULL && strcmp(outcome, "refused") == 0 && stats == NULL);
    cJSON_Delete(st);
    cJSON_Delete(a);
    gw_capture_ask_end(&c);
    CHECK(!c.asking && c.token[0] == '\0' && c.sid == 0);
    o = gw_capture_outcome("refused");
    CHECK(cap_keys_are(o, state_keys) && strcmp(cap_s(o, "state"), "idle") == 0 && strcmp(cap_s(o, "reason"), "refused") == 0 &&
          strcmp(cap_s(o, "iface"), "") == 0);
    cJSON_Delete(o);
    o = gw_capture_outcome("timeout");
    CHECK(strcmp(cap_s(o, "reason"), "timeout") == 0);
    cJSON_Delete(o);
    o = gw_capture_outcome("LEAK");
    CHECK(strcmp(cap_s(o, "reason"), "refused") == 0);
    cJSON_Delete(o);
    /* How often: the same client not within 30 s, another at once. */
    CHECK(gw_capture_may_ask(&c, fa, 2400) == GW_ASK_RATE && gw_capture_may_ask(&c, fa, 30999) == GW_ASK_RATE &&
          gw_capture_may_ask(&c, fa, 31000) == GW_ASK_OK && gw_capture_may_ask(&c, fb, 2400) == GW_ASK_OK);
    /* Timeout, said by the unit. */
    gw_capture_ask(&c, fb, 8, "00000000000000b2", 3000);
    gw_capture_ask_shown(&c, 3100);
    a = cap_raw("idle", "timeout", "timeout", 0, 0);
    gw_capture_step(&c, a, 64000, &st, &stats, &outcome);
    CHECK(outcome != NULL && strcmp(outcome, "timeout") == 0);
    cJSON_Delete(a);
    gw_capture_ask_end(&c);
    /* START, tapped: a capture, and the request is simply over. */
    gw_capture_ask(&c, fa, 9, "00000000000000a2", 40000);
    gw_capture_ask_shown(&c, 40100);
    a = cap_raw("capturing", "none", "none", 10, 1);
    gw_capture_step(&c, a, 41000, &st, &stats, &outcome);
    CHECK(outcome == NULL && !c.asking && st != NULL && stats != NULL &&
          cJSON_IsTrue(cJSON_GetObjectItem(stats, "requested_by_companion")));
    cJSON_Delete(st);
    cJSON_Delete(stats);
    cJSON_Delete(a);
    /* PacketShark left with the sheet up: refused. */
    gw_capture_ask(&c, fb, 10, "00000000000000b3", 80000);
    gw_capture_ask_shown(&c, 80100);
    a = parse("{\"available\":false}");
    gw_capture_step(&c, a, 81000, &st, &stats, &outcome);
    CHECK(outcome != NULL && strcmp(outcome, "refused") == 0);
    cJSON_Delete(st);
    cJSON_Delete(a);
    gw_capture_ask_end(&c);
    /* More clients than slots: the oldest is forgotten, never the newest. */
    gw_capture_init(&c);
    for (i = 0; i < GW_MAX_CLIENTS + 2; i++) {
        memset(fx, 0x10 + i, sizeof(fx));
        gw_capture_ask(&c, fx, 100 + (uint64_t)i, "0000000000000001", 1000 + i * 10);
        gw_capture_ask_end(&c);
    }
    memset(fx, 0x10 + GW_MAX_CLIENTS + 1, sizeof(fx));
    CHECK(gw_capture_may_ask(&c, fx, 1200) == GW_ASK_RATE);
    memset(fx, 0x10, sizeof(fx));
    CHECK(gw_capture_may_ask(&c, fx, 1200) == GW_ASK_OK); /* the first to ask was the one forgotten */
    /* A clock that starts again does not lock a client out. */
    memset(fx, 0x10 + GW_MAX_CLIENTS + 1, sizeof(fx));
    CHECK(gw_capture_may_ask(&c, fx, 5) == GW_ASK_OK);
}
