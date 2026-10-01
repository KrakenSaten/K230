/*
 * pos-zabbix-mock: the fake Zabbix server (core/zabbix/zbx_fake.c) behind a
 * real HTTP socket, so the helper's real transport - libcurl, the TCP
 * connection, the headers, keep-alive, and with --tls the certificate check -
 * can be tested without a Zabbix server. A development and test tool: it is
 * built by `make test` and by name, and never installed into an image.
 *
 *   pos-zabbix-mock [--listen ADDR] [--port N] [--scenario NAME]
 *                   [--tls CERT KEY] [--port-file FILE]
 *
 * Listens on 127.0.0.1 unless told otherwise; --port 0 picks a free port and
 * --port-file writes it out for a test. Serves POST .../api_jsonrpc.php with
 * the fake's answers; GET or POST /__mock/scenario/NAME switches the
 * scenario, and GET /__mock/stats says how many logins, logouts and failed
 * logins it has seen. Faults: a hang keeps the connection open and says nothing, a
 * refusal closes it without an answer (curl: "Empty reply"), and DNS and
 * TLS faults have no socket equivalent and answer 502.
 *
 * One connection at a time, which is all the helper ever opens.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_fake.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef ZBX_MOCK_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define HEAD_MAX 8192
#define BODY_MAX (256 * 1024)
#define IDLE_MS 30000

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

struct conn {
    int fd;
#ifdef ZBX_MOCK_TLS
    SSL *ssl;
#endif
};

static ssize_t c_read(struct conn *c, void *buf, size_t len)
{
#ifdef ZBX_MOCK_TLS
    if (c->ssl) {
        int n = SSL_read(c->ssl, buf, (int)len);

        return n > 0 ? n : -1;
    }
#endif
    return read(c->fd, buf, len);
}

static int c_write(struct conn *c, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n;

#ifdef ZBX_MOCK_TLS
        if (c->ssl) {
            n = SSL_write(c->ssl, p, (int)len);
        } else
#endif
            n = send(c->fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Wait up to ms for the socket to become readable (TLS may already hold
 * decrypted bytes). */
static bool readable(struct conn *c, int ms)
{
    struct pollfd p = { .fd = c->fd, .events = POLLIN };

#ifdef ZBX_MOCK_TLS
    if (c->ssl && SSL_pending(c->ssl) > 0) {
        return true;
    }
#endif
    return poll(&p, 1, ms) > 0;
}

static void http_date(char *out, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;

    gmtime_r(&t, &tm);
    strftime(out, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static int respond(struct conn *c, int status, const char *type, const char *body, size_t len,
                   bool keep)
{
    char head[512];
    char date[64];
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" :
                         status == 502 ? "Bad Gateway" : "Internal Server Error";
    int n;

    http_date(date, sizeof(date));
    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: pos-zabbix-mock\r\n"
                 "Content-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n",
                 status, reason, date, type, len, keep ? "keep-alive" : "close");
    if (c_write(c, head, (size_t)n) != 0) {
        return -1;
    }
    return len ? c_write(c, body, len) : 0;
}

/* One request on the connection. 1 to keep it, 0 to close it. */
static int serve_one(struct conn *c, struct zbx_fake *f)
{
    char head[HEAD_MAX + 1];
    size_t hl = 0;
    char *end = NULL;
    char *body;
    char method[8];
    char path[256];
    char bearer[512] = "";
    long clen = 0;
    bool keep = true;
    size_t have;
    char *line;
    struct zbx_fake_answer a;

    while (!end) {
        ssize_t n;

        if (hl >= HEAD_MAX || !readable(c, IDLE_MS)) {
            return 0;
        }
        n = c_read(c, head + hl, HEAD_MAX - hl);
        if (n <= 0) {
            return 0;
        }
        hl += (size_t)n;
        head[hl] = '\0';
        end = strstr(head, "\r\n\r\n");
    }
    *end = '\0';
    if (sscanf(head, "%7s %255s", method, path) != 2) {
        return 0;
    }
    for (line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        const char *h = line + 2;

        if (strncasecmp(h, "Content-Length:", 15) == 0) {
            clen = strtol(h + 15, NULL, 10);
        } else if (strncasecmp(h, "Authorization: Bearer ", 22) == 0) {
            const char *v = h + 22;
            size_t k = strcspn(v, "\r\n");

            if (k < sizeof(bearer)) {
                memcpy(bearer, v, k);
                bearer[k] = '\0';
            }
        } else if (strncasecmp(h, "Connection:", 11) == 0 && strcasestr(h + 11, "close")) {
            keep = false;
        }
    }
    if (clen < 0 || clen > BODY_MAX) {
        respond(c, 500, "text/plain", "too large\n", 10, false);
        return 0;
    }
    body = malloc((size_t)clen + 1);
    if (!body) {
        return 0;
    }
    have = hl - (size_t)(end + 4 - head);
    if (have > (size_t)clen) {
        have = (size_t)clen; /* no pipelining from libcurl */
    }
    memcpy(body, end + 4, have);
    while (have < (size_t)clen) {
        ssize_t n;

        if (!readable(c, IDLE_MS) || (n = c_read(c, body + have, (size_t)clen - have)) <= 0) {
            free(body);
            return 0;
        }
        have += (size_t)n;
    }
    body[clen] = '\0';

    if (strncmp(path, "/__mock/scenario/", 17) == 0) {
        int ok = zbx_fake_set_scenario(f, path + 17) == 0;

        fprintf(stderr, "pos-zabbix-mock: scenario %s%s\n", path + 17, ok ? "" : " (unknown)");
        respond(c, ok ? 200 : 404, "text/plain", ok ? "ok\n" : "unknown\n", ok ? 3 : 8, keep);
        free(body);
        return keep;
    }
    if (strcmp(path, "/__mock/stats") == 0) {
        /* What a test needs to prove about sessions: every login logged out,
         * no password tried again on its own. */
        char stats[160];
        int n = snprintf(stats, sizeof(stats), "logins %u logouts %u open %d failed_logins %u requests %u\n",
                         f->logins, f->logouts, f->session[0] ? 1 : 0, f->failed_logins, f->requests);

        respond(c, 200, "text/plain", stats, (size_t)n, keep);
        free(body);
        return keep;
    }
    if (strcmp(method, "POST") != 0 || !strstr(path, "api_jsonrpc.php")) {
        respond(c, 404, "text/plain", "not found\n", 10, keep);
        free(body);
        return keep;
    }
    zbx_fake_answer(f, bearer[0] ? bearer : NULL, body, (int64_t)time(NULL), &a);
    explicit_bzero(bearer, sizeof(bearer));
    free(body);
    switch (a.fault) {
    case ZBX_FAKE_HANG: {
        /* Say nothing until the client gives up and closes (or a minute
         * passes, longer than any timeout_s allows). */
        int waited;

        for (waited = 0; !stop && waited < 60; waited++) {
            char sink[256];

            if (readable(c, 1000) && c_read(c, sink, sizeof(sink)) <= 0) {
                break;
            }
        }
        return 0;
    }
    case ZBX_FAKE_REFUSED:
        return 0;
    case ZBX_FAKE_DNS:
    case ZBX_FAKE_TLS:
        respond(c, 502, "text/plain", "fault\n", 6, false);
        return 0;
    case ZBX_FAKE_OK:
    default:
        break;
    }
    if (a.delay_ms > 0) {
        struct timespec ts = { a.delay_ms / 1000, (a.delay_ms % 1000) * 1000000L };

        nanosleep(&ts, NULL);
    }
    if (respond(c, a.status, a.status == 200 ? "application/json" : "text/html", a.body ? a.body : "",
                a.len, keep) != 0) {
        keep = false;
    }
    free(a.body);
    return keep;
}

int main(int argc, char **argv)
{
    const char *listen_addr = "127.0.0.1";
    const char *scenario = ZBX_FAKE_DEFAULT;
    const char *port_file = NULL;
    const char *cert = NULL;
    const char *key = NULL;
    int port = 18080;
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    struct zbx_fake f;
    int one = 1;
    int lfd;
    int i;
#ifdef ZBX_MOCK_TLS
    SSL_CTX *tls = NULL;
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--port-file") == 0 && i + 1 < argc) {
            port_file = argv[++i];
        } else if (strcmp(argv[i], "--tls") == 0 && i + 2 < argc) {
            cert = argv[++i];
            key = argv[++i];
        } else {
            fprintf(stderr, "usage: pos-zabbix-mock [--listen ADDR] [--port N] [--scenario NAME] "
                            "[--tls CERT KEY] [--port-file FILE]\n");
            return 2;
        }
    }
    if (zbx_fake_init(&f, scenario) != 0) {
        fprintf(stderr, "pos-zabbix-mock: unknown scenario %s\n", scenario);
        return 2;
    }
    if (cert) {
#ifdef ZBX_MOCK_TLS
        tls = SSL_CTX_new(TLS_server_method());
        if (!tls || SSL_CTX_use_certificate_chain_file(tls, cert) != 1 ||
            SSL_CTX_use_PrivateKey_file(tls, key, SSL_FILETYPE_PEM) != 1) {
            fprintf(stderr, "pos-zabbix-mock: cannot load %s / %s\n", cert, key);
            return 2;
        }
#else
        (void)key;
        fprintf(stderr, "pos-zabbix-mock: built without TLS (ZBX_MOCK_TLS)\n");
        return 2;
#endif
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (lfd < 0 || inet_pton(AF_INET, listen_addr, &sa.sin_addr) != 1 ||
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(lfd, 8) != 0 ||
        getsockname(lfd, (struct sockaddr *)&sa, &salen) != 0) {
        fprintf(stderr, "pos-zabbix-mock: cannot listen on %s:%d: %s\n", listen_addr, port,
                strerror(errno));
        return 2;
    }
    port = ntohs(sa.sin_port);
    fprintf(stderr, "pos-zabbix-mock: %s://%s:%d/api_jsonrpc.php scenario %s\n",
            cert ? "https" : "http", listen_addr, port, f.scenario);
    if (port_file) {
        FILE *pf = fopen(port_file, "w");

        if (!pf) {
            return 2;
        }
        fprintf(pf, "%d\n", port);
        fclose(pf);
    }
    while (!stop) {
        struct conn c;
        struct pollfd p = { .fd = lfd, .events = POLLIN };

        if (poll(&p, 1, 500) <= 0) {
            continue;
        }
        memset(&c, 0, sizeof(c));
        c.fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (c.fd < 0) {
            continue;
        }
#ifdef ZBX_MOCK_TLS
        if (tls) {
            c.ssl = SSL_new(tls);
            SSL_set_fd(c.ssl, c.fd);
            if (SSL_accept(c.ssl) != 1) {
                /* The client refused the certificate: that is the test. */
                SSL_free(c.ssl);
                close(c.fd);
                continue;
            }
        }
#endif
        while (!stop && serve_one(&c, &f)) {
        }
#ifdef ZBX_MOCK_TLS
        if (c.ssl) {
            SSL_shutdown(c.ssl);
            SSL_free(c.ssl);
        }
#endif
        close(c.fd);
    }
    close(lfd);
    return 0;
}
