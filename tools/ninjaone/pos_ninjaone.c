/* Read-only one-shot helper: all HTTP and JSON stay outside the shell.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ninjaone/ninja_api.h"
#include "pocketpaths.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef NINJA_CURL
#include <curl/curl.h>
#endif
struct transport { char base[128], token[4096]; const char *fake, *ca_file; };
#ifdef NINJA_CURL
struct response { char *body; size_t len; int retry; };
static size_t receive(void *ptr, size_t size, size_t nmemb, void *arg)
{
    struct response *r = arg;
    size_t n;
    char *p;
    if (nmemb && size > SIZE_MAX / nmemb) return 0;
    n = size * nmemb;
    if (n > NINJA_RESPONSE_MAX - r->len) return 0;
    p = realloc(r->body, r->len + n + 1);
    if (!p) return 0;
    r->body = p; memcpy(p + r->len, ptr, n); r->len += n; p[r->len] = 0;
    return n;
}
static size_t header(void *ptr, size_t size, size_t nmemb, void *arg)
{
    struct response *r = arg;
    size_t n = size * nmemb;
    char value[128], *end;
    long seconds;
    if (n > 12 && n < sizeof(value) && !strncasecmp(ptr, "Retry-After:", 12)) {
        memcpy(value, (char *)ptr + 12, n - 12); value[n - 12] = 0;
        seconds = strtol(value, &end, 10);
        while (*end == ' ' || *end == '\r' || *end == '\n') end++;
        if (*end || seconds < 0) {
            time_t date = curl_getdate(value, NULL), now = time(NULL);
            seconds = date > now ? (long)(date - now) : 0;
        }
        r->retry = seconds > 86400 ? 86400 : (int)seconds;
    }
    return n;
}
static int live_get(void *ctx, const char *path, char **body, size_t *len, int *retry)
{
    struct transport *t = ctx;
    struct response r = {0};
    char url[256], auth[4128];
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    long status = 0;
    CURLcode code;
    int rc;
    if (!curl) return NINJA_NETWORK;
    snprintf(url, sizeof(url), "%s%s", t->base, path);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", t->token);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/json");
    explicit_bzero(auth, sizeof(auth));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (t->ca_file) curl_easy_setopt(curl, CURLOPT_CAINFO, t->ca_file);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    /* Wipe the libcurl header copy before freeing it. */
    for (struct curl_slist *h = headers; h; h = h->next) explicit_bzero(h->data, strlen(h->data));
    curl_slist_free_all(headers);
    *retry = r.retry;
    if (status == 401) rc = NINJA_AUTH;
    else if (status == 403) rc = NINJA_DENIED;
    else if (status == 429) rc = NINJA_RATE;
    else if (code == CURLE_WRITE_ERROR) rc = NINJA_TOO_LARGE;
    else if (code != CURLE_OK || status != 200) rc = NINJA_NETWORK;
    else rc = NINJA_OK;
    if (rc) { free(r.body); return rc; } /* Never forward an error body. */
    *body = r.body; *len = r.len; return rc;
}
#else
static int live_get(void *ctx, const char *path, char **body, size_t *len, int *retry)
{
    (void)ctx; (void)path; (void)body; (void)len; (void)retry; return NINJA_SETUP;
}
#endif
/* Synthetic responses pass through exactly the live decoding/pagination path.
 * No implicit simulator demo: --fake is required and always labelled. */
static int fake_get(void *ctx, const char *path, char **body, size_t *len, int *retry)
{
    const struct transport *t = ctx;
    const char *json = "[]";
    if (!strcmp(t->fake, "expired")) return NINJA_AUTH;
    if (!strcmp(t->fake, "denied")) return NINJA_DENIED;
    if (!strcmp(t->fake, "rate")) { *retry = 180; return NINJA_RATE; }
    if (!strcmp(t->fake, "network")) return NINJA_NETWORK;
    if (!strcmp(t->fake, "hang")) { for (;;) pause(); }
    if (!strcmp(t->fake, "malformed")) json = "{invalid";
    else if (strcmp(t->fake, "empty")) {
        bool first = strstr(path, "after=0") != NULL;
        if (strstr(path, "/organizations")) json = first ?
            "[{\"id\":1,\"name\":\"Demo North\"},{\"id\":2,\"name\":\"Demo South\"}]" :
            "[{\"id\":3,\"name\":\"Demo Lab\"}]";
        else if (strstr(path, "/devices")) json = first ?
            "[{\"id\":11,\"organizationId\":1,\"displayName\":\"Demo server\",\"offline\":true,\"lastContact\":1791790000},{\"id\":12,\"organizationId\":2,\"systemName\":\"Demo laptop\",\"offline\":false}]" :
            "[{\"id\":13,\"organizationId\":3}]";
        else json = "[{\"uid\":\"demo-alert-1\",\"severity\":\"MAJOR\",\"deviceId\":11,\"message\":\"Demo: agent disconnected\"},{\"uid\":\"demo-alert-2\",\"deviceId\":12,\"message\":\"Demo: disk space condition triggered\"}]";
        if (!strcmp(t->fake, "missing") && strstr(path, "/devices")) json = first ? "[{\"id\":11},{\"id\":12}]" : "[]";
    }
    if (!strcmp(t->fake, "large")) {
        unsigned after = 0, limit, count;
        const char *cursor = strstr(path, "after=");
        if (cursor) after = (unsigned)strtoul(cursor + 6, NULL, 10);
        bool organizations = strstr(path, "/organizations") != NULL;
        bool devices = strstr(path, "/devices") != NULL;
        limit = organizations ? 250 : devices ? 500 : 501;
        count = after < limit ? limit - after : 0;
        if ((organizations || devices) && count > 100) count = 100;
        *body = calloc(1, 128000);
        if (!*body) return NINJA_NETWORK;
        size_t used = 1; (*body)[0] = '[';
        for (unsigned i = 0; i < count; i++) {
            unsigned id = after + i + 1;
            int n;
            if (organizations) n = snprintf(*body + used, 128000 - used,
                "%s{\"id\":%u,\"name\":\"Demo organization %u\"}", i ? "," : "", id, id);
            else if (devices) n = snprintf(*body + used, 128000 - used,
                "%s{\"id\":%u,\"organizationId\":%u,\"displayName\":\"Demo device %u\"}", i ? "," : "", id, id % 3 + 1, id);
            else n = snprintf(*body + used, 128000 - used,
                "%s{\"uid\":\"demo-%u\",\"deviceId\":%u,\"message\":\"Demo alert %u\"}", i ? "," : "", id, (id-1) % 500 + 1, id);
            if (n < 0 || (size_t)n >= 128000 - used) { free(*body); *body = NULL; return NINJA_TOO_LARGE; }
            used += (size_t)n;
        }
        (*body)[used++] = ']'; (*body)[used] = 0; *len = used; return NINJA_OK;
    }
    *body = strdup(json);
    if (!*body) return NINJA_NETWORK;
    *len = strlen(json); return NINJA_OK;
}
static int load_base(struct transport *t)
{
    char path[POCKETOS_PATH_MAX], buf[130];
    FILE *file;
    snprintf(path, sizeof(path), "%s/ninjaone.conf", pocketos_config_dir());
    file = fopen(path, "r");
    if (!file) return -1;
    if (!fgets(buf, sizeof(buf), file) || fgetc(file) != EOF) { fclose(file); return -1; }
    fclose(file); buf[strcspn(buf, "\r\n")] = 0;
    if (strncmp(buf, "base=", 5) || !ninja_base_valid(buf + 5)) return -1;
    snprintf(t->base, sizeof(t->base), "%s", buf + 5); return 0;
}
int main(int argc, char **argv)
{
    struct rlimit no_core = {0, 0};
    struct rlimit memory = {128u * 1024u * 1024u, 128u * 1024u * 1024u};
    struct transport t = {0};
    struct ninja_snapshot *s;
    int rc = 0;
    setrlimit(RLIMIT_AS, &memory);
    setrlimit(RLIMIT_CORE, &no_core); /* Tokens must not appear in core dumps. */
    prctl(PR_SET_DUMPABLE, 0);
    if (argc == 2 && !strcmp(argv[1], "set-token")) {
        char token[4097] = {0};
        size_t n;
        if (isatty(STDIN_FILENO)) {
            fputs("Use a secure input pipe, not a terminal, to provision a token\n", stderr); return 1;
        }
        n = fread(token, 1, sizeof(token) - 1, stdin);
        rc = -1;
        if (n && !memchr(token, 0, n) && fgetc(stdin) == EOF) {
            if (token[n - 1] == '\n') n--;
            if (n && token[n - 1] == '\r') n--;
            token[n] = 0;
            rc = ninja_token_write(token);
        }
        explicit_bzero(token, sizeof(token));
        if (rc) fputs("Token provisioning failed: check input and runtime directory permissions\n", stderr);
        return rc ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "snapshot") && !strcmp(argv[2], "--fake")) t.fake = argv[3];
    else if (argc != 2 || strcmp(argv[1], "snapshot")) {
        fputs("Usage: pos-ninjaone snapshot [--fake demo|large|empty|missing|expired|denied|rate|network|malformed|hang]\n       pos-ninjaone set-token < local-secure-pipe\n", stderr); return 2;
    }
    if (t.fake && strcmp(t.fake, "demo") && strcmp(t.fake, "empty") && strcmp(t.fake, "missing") &&
        strcmp(t.fake, "expired") && strcmp(t.fake, "denied") && strcmp(t.fake, "rate") &&
        strcmp(t.fake, "network") && strcmp(t.fake, "malformed") && strcmp(t.fake, "hang") && strcmp(t.fake, "large")) return 2;
    s = calloc(1, sizeof(*s));
    if (!s) return 1;
#ifdef NINJA_CURL
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) { free(s); return 1; }
#endif
    if (!t.fake && (load_base(&t) || ninja_token_read(t.token, sizeof(t.token)))) {
        s->magic = NINJA_MAGIC; s->error = NINJA_SETUP;
    } else ninja_collect(s, t.fake ? fake_get : live_get, &t, t.fake && strcmp(t.fake, "large") ? 2 : 100);
    explicit_bzero(t.token, sizeof(t.token));
    s->demo = t.fake != NULL;
    /* Error frames never carry incomplete data, nor tenant data in errors. */
    if (s->error) { s->norgs = s->ndevices = s->nalerts = 0; s->updated = 0;
        memset(s->orgs, 0, sizeof(s->orgs)); memset(s->devices, 0, sizeof(s->devices));
        memset(s->alerts, 0, sizeof(s->alerts)); }
    rc = fwrite(s, sizeof(*s), 1, stdout) == 1 && !fflush(stdout) ? 0 : 1;
    free(s);
#ifdef NINJA_CURL
    curl_global_cleanup();
#endif
    return rc;
}
