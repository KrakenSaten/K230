/* Private helper implementation; included after the curl transport.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ninjaone/ninja_auth.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <fcntl.h>
static int renew_notice(const struct ninja_credentials *c)
{
    struct ninja_snapshot *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->magic = NINJA_MAGIC; s->error = NINJA_RENEWING;
    memcpy(s->profile, c->profile, sizeof(s->profile)); s->expires = c->expires;
    int rc = fwrite(s, sizeof(*s), 1, stdout) != 1 || fflush(stdout);
    free(s); return rc ? -1 : 0;
}
#ifdef NINJA_CURL
static int renew_token(struct transport *t, struct ninja_credentials *c, int *retry)
{
    CURL *curl = curl_easy_init();
    struct response r = {0};
    struct curl_slist *headers = NULL;
    char url[256], *client = NULL, *refresh = NULL, *form = NULL;
    long status = 0;
    int rc = NINJA_AUTH;
    if (!curl) return NINJA_RENEW_FAILED;
    client = curl_easy_escape(curl, c->client, 0);
    refresh = curl_easy_escape(curl, c->refresh, 0);
    if (!client || !refresh || asprintf(&form, "grant_type=refresh_token&client_id=%s&refresh_token=%s", client, refresh) < 0) goto done;
    snprintf(url, sizeof(url), "%s/ws/oauth/token", t->base);
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form); /* OAuth only; API stays GET. */
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
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    /* Only definite rejected requests may retry. Transport failure after sending
     * a single-use token is ambiguous: require a fresh sign-in, not replay. */
    if (code == CURLE_OK && (status == 429 || status == 503)) {
        *retry = r.retry > 60 ? r.retry : 60; rc = NINJA_RENEW_FAILED; goto done;
    }
    if (code != CURLE_OK || status != 200 || r.len > 65536 || !r.body) goto done;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(r.body, r.len + 1, &end, 1);
    cJSON *access = cJSON_GetObjectItemCaseSensitive(root, "access_token"),
          *rotated = cJSON_GetObjectItemCaseSensitive(root, "refresh_token"),
          *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in"),
          *scope = cJSON_GetObjectItemCaseSensitive(root, "scope"),
          *type = cJSON_GetObjectItemCaseSensitive(root, "token_type");
    struct ninja_credentials next = *c;
    if (end == r.body + r.len && cJSON_IsString(access) && cJSON_IsString(rotated) &&
        cJSON_IsString(type) && !strcasecmp(type->valuestring, "Bearer") && cJSON_IsString(scope) &&
        (!strcmp(scope->valuestring, "monitoring") || !strcmp(scope->valuestring, "monitoring offline_access") ||
         !strcmp(scope->valuestring, "offline_access monitoring")) &&
        cJSON_IsNumber(expires) && isfinite(expires->valuedouble) && expires->valuedouble >= 1 &&
        expires->valuedouble <= 604800 && floor(expires->valuedouble) == expires->valuedouble &&
        strlen(access->valuestring) < sizeof(next.access) && strlen(rotated->valuestring) < sizeof(next.refresh)) {
        strcpy(next.access, access->valuestring); strcpy(next.refresh, rotated->valuestring);
        next.expires = (int64_t)time(NULL) + (int64_t)expires->valuedouble;
        next.attempts = 0; next.retry_at = 0;
        if (ninja_credentials_valid(&next)) { *c = next; rc = NINJA_OK; }
    }
    explicit_bzero(&next, sizeof(next));
    if (root) for (cJSON *v = root->child; v; v = v->next)
        if (v->valuestring) explicit_bzero(v->valuestring, strlen(v->valuestring));
    cJSON_Delete(root);
done:
    if (form) { explicit_bzero(form, strlen(form)); free(form); }
    if (refresh) { explicit_bzero(refresh, strlen(refresh)); curl_free(refresh); }
    curl_free(client);
    if (r.body) { explicit_bzero(r.body, r.len); free(r.body); }
    curl_easy_cleanup(curl); curl_slist_free_all(headers);
    return rc;
}
#else
static int renew_token(struct transport *t, struct ninja_credentials *c, int *retry)
{
    (void)t; (void)c; (void)retry; return NINJA_SETUP;
}
#endif
static int authorize(struct transport *t, struct ninja_credentials *c, int dir, int *retry)
{
    int64_t now = (int64_t)time(NULL);
    if (ninja_credentials_pending(dir)) return NINJA_AUTH;
    if (c->expires > now) { memcpy(t->token, c->access, sizeof(t->token)); return NINJA_OK; }
    if (c->attempts >= 3) return NINJA_AUTH;
    if (c->retry_at > now) { *retry = (int)(c->retry_at - now); return NINJA_RENEW_FAILED; }
    if (renew_notice(c) || ninja_credentials_begin(dir)) return NINJA_AUTH;
    int rc = renew_token(t, c, retry);
    if (rc == NINJA_RENEW_FAILED) {
        c->attempts++; c->retry_at = now + *retry;
    }
    if (rc == NINJA_OK || rc == NINJA_RENEW_FAILED) {
        if (ninja_credentials_write(dir, c) || ninja_credentials_finish(dir)) return NINJA_AUTH;
    }
    if (!rc) memcpy(t->token, c->access, sizeof(t->token));
    /* Other failures leave the pending marker, ensuring no rotated token replay. */
    return rc;
}
static void authorized_snapshot(struct transport *t, struct ninja_credentials *c, int dir,
                                struct ninja_snapshot *s)
{
    /* At most one grant per snapshot, including a token expiring mid-page.
     * A server 401 can establish expiry earlier than our recorded TTL. */
    int initially_valid = c->expires > (int64_t)time(NULL);
    int rc = authorize(t, c, dir, &s->retry_s);
    s->error = rc;
    if (!rc) ninja_collect(s, live_get, t, 100);
    if (!rc && s->error == NINJA_AUTH && initially_valid) {
        c->expires = (int64_t)time(NULL);
        if (ninja_credentials_write(dir, c)) return;
        s->error = authorize(t, c, dir, &s->retry_s);
        if (!s->error) ninja_collect(s, live_get, t, 100);
    }
    if (s->error == NINJA_AUTH && !ninja_credentials_pending(dir))
        (void)ninja_credentials_begin(dir); /* Invalid replacement: require sign-in. */
}
static int import_credentials(void)
{
    char input[10001] = {0};
    struct ninja_credentials c = {0};
    struct transport t = {0};
    int dir = -1, rc = -1;
    if (isatty(STDIN_FILENO)) goto done;
    size_t n = fread(input, 1, sizeof(input) - 1, stdin);
    if (n && input[n - 1] == '\n') input[--n] = 0;
    if (!n || memchr(input, 0, n) || fgetc(stdin) != EOF ||
        ninja_credentials_decode(&c, input, n) || load_base(&t) || strcmp(t.base, c.base)) goto done;
    dir = ninja_credentials_lock();
    /* Explicit fresh login replaces uncertain interrupted state. The exclusive
     * directory lock prevents an in-flight rotation from resurrecting a login. */
    if (dir >= 0 && !ninja_credentials_forget(dir) && !ninja_credentials_write(dir, &c)) rc = 0;
done:
    if (dir >= 0) close(dir);
    explicit_bzero(input, sizeof(input)); explicit_bzero(&c, sizeof(c));
    if (rc) fputs("Sign-in provisioning failed: check registration, region, input and runtime permissions\n", stderr);
    return rc ? 1 : 0;
}
