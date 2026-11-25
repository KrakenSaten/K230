/*
 * sysd's companion.* (services/sysd/sysd_companion.h, ADR-019 amendment 2):
 * how sshd_config is judged and hardened, which passwords are taken, the
 * shadow line that is written and nothing else, the hash, and the three jobs
 * - the gateway on and off and the device password - with every refusal and
 * every way back. The init scripts, sshd and gatewayd are fakes; the jobs run
 * in real children, so the fakes record what they were asked into a file.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_companion.h"

#include "../services/gatewayd/gw_gate.h"

#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
    }
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

#define VENDOR_SSHD                                                                                          \
    "#\t$OpenBSD: sshd_config,v 1.104 2021/07/02 05:11:21 dtucker Exp $\n"                               \
    "#Port 22\n"                                                                                         \
    "#PermitRootLogin prohibit-password\n"                                                               \
    "PermitRootLogin yes\n"                                                                              \
    "PasswordAuthentication yes\n"                                                                       \
    "PermitEmptyPasswords no\n"                                                                          \
    "Subsystem\tsftp\t/usr/libexec/sftp-server\n"
#define PASSWD "root:x:0:0:root:/root:/bin/sh\ndaemon:x:1:1:daemon:/usr/sbin:/bin/false\n"
#define SHADOW_EMPTY "root::::::::\ndaemon:*:::::::\n"
#define PW "correct horse 9"

/* ---- files ---------------------------------------------------------------- */

static char dir[] = "/tmp/sysd-companion-XXXXXX";
static char p_def[300], p_init[300], p_daemon[300], p_pid[300], p_passwd[300], p_shadow[300], p_sshdc[300],
    p_sshd[300], p_sshd_init[300], p_sshd_pid[300], p_proc[300], p_log[300], trace[300];

static void put(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
    chmod(path, mode);
}

static char *get(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int has(const char *path, const char *needle)
{
    char *t = get(path);

    return t && strstr(t, needle) != NULL;
}

/* ---- the fakes -------------------------------------------------------------- */

static int sshd_test_fails;
static int init_start_fails;
static int init_stop_fails;
static int reload_fails;
static int gateway_answers = 1;
static int run_sleeps_ms;

static void record(const char *line)
{
    FILE *f = fopen(trace, "a");

    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static int fake_run(const char *const argv[], int log_fd, void *user)
{
    char line[600];
    size_t o = 0;
    int i;

    (void)log_fd;
    (void)user;
    o += (size_t)snprintf(line, sizeof(line), "run %s", strrchr(argv[0], '/') + 1);
    for (i = 1; argv[i] && o < sizeof(line); i++) {
        const char *a = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];

        o += (size_t)snprintf(line + o, sizeof(line) - o, " %s", a);
    }
    record(line);
    if (run_sleeps_ms) {
        usleep((useconds_t)run_sleeps_ms * 1000);
    }
    if (strcmp(argv[0], p_sshd) == 0) {
        /* sshd -t -f <file>: the file must be the hardened one. */
        char *t = get(argv[3]);

        record(t && sysd_companion_ssh_judge(t) == SYSD_SSH_HARDENED ? "  (checked a hardened file)"
                                                                       : "  (checked something else)");
        return sshd_test_fails ? 255 : 0;
    }
    if (strcmp(argv[0], p_sshd_init) == 0) {
        return reload_fails ? 1 : 0;
    }
    if (strcmp(argv[0], p_init) == 0 && strcmp(argv[1], "start") == 0) {
        record(has(p_def, "GATEWAYD_ENABLE=1") ? "  (enabled in the file)" : "  (NOT enabled in the file)");
        return init_start_fails ? 1 : 0;
    }
    if (strcmp(argv[0], p_init) == 0 && strcmp(argv[1], "stop") == 0) {
        record(has(p_def, "GATEWAYD_ENABLE=0") ? "  (disabled in the file)" : "  (NOT disabled in the file)");
        return init_stop_fails ? 1 : 0;
    }
    return 0;
}

static cJSON *fake_call(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                        size_t errlen, void *user)
{
    char line[200];

    (void)timeout_ms;
    (void)user;
    cJSON_Delete(params);
    snprintf(line, sizeof(line), "call %s %s", service, method);
    record(line);
    if (gateway_answers && strcmp(service, "gatewayd") == 0 && strcmp(method, "gateway.status") == 0) {
        return cJSON_CreateObject();
    }
    snprintf(err, errlen, "not answering");
    return NULL;
}

static int locks;

static int fake_lock(void *user)
{
    (void)user;
    record("lock");
    return 0;
}

static void fake_unlock(void *user)
{
    (void)user;
    record("unlock");
    locks++;
}

static const struct sysd_companion_ops fake_ops = { fake_run, fake_call, fake_lock, fake_unlock, NULL };

static struct sysd_companion C;

static void setup(void)
{
    struct sysd_companion_paths p = { p_def, p_init, p_daemon, p_pid, p_passwd, p_shadow,
                                      p_sshdc, p_sshd, p_sshd_init, p_sshd_pid, p_proc, p_log };
    char pid[32];

    unlink(p_def);
    unlink(trace);
    unlink(p_log);
    put(p_passwd, PASSWD, 0644);
    put(p_shadow, SHADOW_EMPTY, 0600);
    put(p_sshdc, VENDOR_SSHD, 0644);
    {
        char orig[320];

        snprintf(orig, sizeof(orig), "%s.doors-orig", p_sshdc);
        unlink(orig);
    }
    put(p_sshd, "#!/bin/sh\n", 0755);
    put(p_sshd_init, "#!/bin/sh\n", 0755);
    put(p_daemon, "#!/bin/sh\n", 0755);
    put(p_init, "#!/bin/sh\n", 0755);
    /* sshd "runs": the pid file names this process. */
    snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    put(p_sshd_pid, pid, 0644);
    unlink(p_pid);
    sshd_test_fails = init_start_fails = init_stop_fails = reload_fails = run_sleeps_ms = 0;
    gateway_answers = 1;
    sysd_companion_init(&C, &p, &fake_ops);
    C.answer_ms = 600;
}

static void wait_job(void)
{
    int i;

    for (i = 0; i < 1000 && sysd_companion_busy(&C); i++) {
        usleep(10 * 1000);
        sysd_companion_reap(&C);
    }
}

static const char *status_str(const char *key)
{
    static char out[256];
    cJSON *st = sysd_companion_status(&C);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, key);

    if (cJSON_IsString(v)) {
        snprintf(out, sizeof(out), "%s", v->valuestring);
    } else if (cJSON_IsBool(v)) {
        snprintf(out, sizeof(out), "%s", cJSON_IsTrue(v) ? "true" : "false");
    } else {
        snprintf(out, sizeof(out), "(none)");
    }
    cJSON_Delete(st);
    return out;
}

static int set_password(const char *pw, char *word, char *msg)
{
    char buf[128];

    snprintf(buf, sizeof(buf), "%s", pw);
    return sysd_companion_set_password(&C, buf, word, 32, msg, 200);
}

int main(void)
{
    char word[32];
    char msg[200];
    char *h;
    char why[128];
    int r;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
#define P(var, name) snprintf(var, sizeof(var), "%s/%s", dir, name)
    P(p_def, "gatewayd.default");
    P(p_init, "S70gatewayd");
    P(p_daemon, "gatewayd");
    P(p_pid, "gatewayd.pid");
    P(p_passwd, "passwd");
    P(p_shadow, "shadow");
    P(p_sshdc, "sshd_config");
    P(p_sshd, "sshd");
    P(p_sshd_init, "S50sshd");
    P(p_sshd_pid, "sshd.pid");
    P(p_log, "companion.log");
    P(p_proc, "proc");
    mkdir(p_proc, 0755);
    P(trace, "trace");

    /* ---- 1. judging sshd_config ------------------------------------------------ */
    check("the 0.5.0 image's sshd_config lets root in by password", sysd_companion_ssh_judge(VENDOR_SSHD) == SYSD_SSH_OPEN);
    check("prohibit-password is hardened", sysd_companion_ssh_judge("PermitRootLogin prohibit-password\n") == SYSD_SSH_HARDENED);
    check("without-password, forced-commands-only and no are hardened",
          sysd_companion_ssh_judge("PermitRootLogin without-password\n") == SYSD_SSH_HARDENED &&
              sysd_companion_ssh_judge("PermitRootLogin forced-commands-only\n") == SYSD_SSH_HARDENED &&
              sysd_companion_ssh_judge("PermitRootLogin no\n") == SYSD_SSH_HARDENED);
    check("keywords and values in any case, after '=' or a tab, in quotes",
          sysd_companion_ssh_judge("permitrootlogin=NO\n") == SYSD_SSH_HARDENED &&
              sysd_companion_ssh_judge("  PermitRootLogin\tprohibit-password\n") == SYSD_SSH_HARDENED &&
              sysd_companion_ssh_judge("PermitRootLogin \"yes\"\n") == SYSD_SSH_OPEN &&
              sysd_companion_ssh_judge("PERMITROOTLOGIN Yes\n") == SYSD_SSH_OPEN);
    check("no line at all is not relied on (the compiled default)",
          sysd_companion_ssh_judge("PasswordAuthentication yes\n") == SYSD_SSH_OPEN);
    check("a comment is not a line", sysd_companion_ssh_judge("#PermitRootLogin no\n") == SYSD_SSH_OPEN);
    check("a Match block that lets root in by password is open",
          sysd_companion_ssh_judge("PermitRootLogin no\nMatch Address 10.0.0.0/8\n  PermitRootLogin yes\n") == SYSD_SSH_OPEN);
    check("only a line inside a Match is not enough",
          sysd_companion_ssh_judge("Match User root\n  PermitRootLogin no\n") == SYSD_SSH_OPEN);
    check("an Include cannot be judged", sysd_companion_ssh_judge("Include /etc/ssh/sshd_config.d/*.conf\nPermitRootLogin no\n") ==
                                             SYSD_SSH_UNKNOWN);
    check("an unreadable file cannot be judged", sysd_companion_ssh_judge(NULL) == SYSD_SSH_UNKNOWN);
    check("a missing value is open", sysd_companion_ssh_judge("PermitRootLogin\n") == SYSD_SSH_OPEN);

    /* ---- 2. hardening it ---------------------------------------------------------- */
    h = sysd_companion_ssh_harden(VENDOR_SSHD);
    check("the image's file hardens", h && sysd_companion_ssh_judge(h) == SYSD_SSH_HARDENED);
    check("only the unsafe line changes, every other line byte for byte",
          h && strcmp(h, "#\t$OpenBSD: sshd_config,v 1.104 2021/07/02 05:11:21 dtucker Exp $\n#Port 22\n"
                         "#PermitRootLogin prohibit-password\nPermitRootLogin prohibit-password\n"
                         "PasswordAuthentication yes\nPermitEmptyPasswords no\nSubsystem\tsftp\t/usr/libexec/sftp-server\n") == 0);
    free(h);
    h = sysd_companion_ssh_harden("Port 22\nMatch User guest\n  PermitRootLogin yes\n");
    check("no line before Match: one is added at the top, with a comment saying who",
          h && strncmp(h, "# Doors: root signs in over SSH with a key only.", 48) == 0 &&
              strstr(h, "\nPermitRootLogin prohibit-password\nPort 22\nMatch User guest\nPermitRootLogin prohibit-password\n") &&
              sysd_companion_ssh_judge(h) == SYSD_SSH_HARDENED);
    free(h);
    h = sysd_companion_ssh_harden("PermitRootLogin no\nX11Forwarding no");
    check("a hardened file comes back as it was (and a last line without a newline stays so)",
          h && strcmp(h, "PermitRootLogin no\nX11Forwarding no") == 0);
    free(h);
    check("a file with an Include is not rewritten", sysd_companion_ssh_harden("Include x\nPermitRootLogin yes\n") == NULL);
    check("nor no file", sysd_companion_ssh_harden(NULL) == NULL);

    /* ---- 3. which passwords --------------------------------------------------------- */
    check("seven characters are too few", sysd_companion_password_check("abcdefg", why, sizeof(why)) < 0 &&
                                              strcmp(why, "The password needs at least 8 characters.") == 0);
    check("eight are enough, spaces count", sysd_companion_password_check("abc def ", why, sizeof(why)) == 0);
    check("64 are the most",
          sysd_companion_password_check("0123456789012345678901234567890123456789012345678901234567890123", why,
                                        sizeof(why)) == 0 &&
              sysd_companion_password_check("01234567890123456789012345678901234567890123456789012345678901234", why,
                                            sizeof(why)) < 0);
    check("a control character is refused", sysd_companion_password_check("abcdefgh\n", why, sizeof(why)) < 0);
    check("a letter outside ASCII is refused (the console may not type it)",
          sysd_companion_password_check("abcdefg\xc3\xa9", why, sizeof(why)) < 0);
    check("no password is refused", sysd_companion_password_check(NULL, why, sizeof(why)) < 0);

    /* ---- 4. the password files --------------------------------------------------------- */
    check("root's password in shadow ('x') is the layout this writes", sysd_companion_passwd_shadowed(PASSWD));
    check("a hash or nothing in passwd itself is not",
          !sysd_companion_passwd_shadowed("root::0:0::/root:/bin/sh\n") &&
              !sysd_companion_passwd_shadowed("root:$6$a$b:0:0::/root:/bin/sh\n") &&
              !sysd_companion_passwd_shadowed("daemon:x:1:1::/:/bin/false\n") && !sysd_companion_passwd_shadowed(NULL));
    h = sysd_companion_shadow_set("daemon:*:::::::\nroot::19000:0:99999:7:::\nbin:*:::::::\n", "$6$salt$hash");
    check("root's field is replaced and every other line and field kept",
          h && strcmp(h, "daemon:*:::::::\nroot:$6$salt$hash:19000:0:99999:7:::\nbin:*:::::::\n") == 0);
    free(h);
    h = sysd_companion_shadow_set("root:$6$old$old:1::::::\n", "$6$new$new");
    check("a password that was set is replaced", h && strcmp(h, "root:$6$new$new:1::::::\n") == 0);
    free(h);
    check("no root line: nothing to write", sysd_companion_shadow_set("bin:*:::::::\n", "$6$a$b") == NULL);
    check("a hash that would break the line is refused", sysd_companion_shadow_set(SHADOW_EMPTY, "$6$a:b") == NULL);

    /* ---- 5. the hash ------------------------------------------------------------------- */
    {
        char a[256], b[256];
        struct crypt_data cd;

        memset(&cd, 0, sizeof(cd));
        check("SHA-512 crypt with a salt", sysd_companion_hash(PW, a, sizeof(a)) == 0 && strncmp(a, "$6$", 3) == 0 &&
                                               strlen(a) > 3 + 16 + 1 + 80);
        check("it verifies with crypt(3), as sshd and login check it", strcmp(crypt_r(PW, a, &cd), a) == 0);
        check("and another password does not", strcmp(crypt_r(PW "x", a, &cd), a) != 0);
        check("a new salt every time", sysd_companion_hash(PW, b, sizeof(b)) == 0 && strcmp(a, b) != 0);
    }

    /* ---- 6. refusals before anything runs --------------------------------------------- */
    setup();
    unlink(p_daemon);
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    check("on, without gatewayd installed: refused (3, not_installed)", r == 3 && strcmp(word, "not_installed") == 0);
    put(p_daemon, "#!/bin/sh\n", 0755);
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    check("on, without a device password: refused (3, password_required) - rule 9b",
          r == 3 && strcmp(word, "password_required") == 0);
    check("and nothing ran, nothing was written", get(trace) == NULL && get(p_def) == NULL && !sysd_companion_busy(&C));
    r = set_password("short", word, msg);
    check("a password that is too short: refused (2) with the reason",
          r == 2 && strcmp(word, "invalid_password") == 0 && strstr(msg, "at least 8"));
    put(p_sshdc, "Include /etc/ssh/sshd_config.d/*.conf\n", 0644);
    r = set_password(PW, word, msg);
    check("an SSH configuration that cannot be judged: refused (3, ssh_unknown)", r == 3 && strcmp(word, "ssh_unknown") == 0);
    put(p_sshdc, VENDOR_SSHD, 0644);
    put(p_passwd, "root::0:0:root:/root:/bin/sh\n", 0644);
    r = set_password(PW, word, msg);
    check("root's password not in shadow: refused (3, password_layout)", r == 3 && strcmp(word, "password_layout") == 0);
    check("still nothing ran", get(trace) == NULL);

    /* ---- 7. the password -------------------------------------------------------------- */
    setup();
    run_sleeps_ms = 150; /* long enough to be seen running */
    check("status before: not set, SSH open, idle",
          strcmp(status_str("password_set"), "false") == 0 && strcmp(status_str("ssh"), "open") == 0 &&
              strcmp(status_str("state"), "idle") == 0 && strcmp(status_str("enabled"), "false") == 0 &&
              strcmp(status_str("installed"), "true") == 0);
    {
        char buf[64];

        snprintf(buf, sizeof(buf), "%s", PW);
        r = sysd_companion_set_password(&C, buf, word, sizeof(word), msg, sizeof(msg));
        check("the password is accepted and the caller's copy wiped at once",
              r == 0 && memcmp(buf, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 15) == 0);
    }
    check("while it runs the status says so", strcmp(status_str("state"), "running") == 0 &&
                                                   strcmp(status_str("action"), "password") == 0);
    wait_job();
    check("the job finished", strcmp(status_str("state"), "done") == 0);
    check("SSH was checked with the hardened file, then reloaded, before the password was written",
          has(trace, "run sshd -t -f sshd_config.doors-check\n  (checked a hardened file)\nrun S50sshd reload\nlock\nunlock"));
    check("sshd_config now lets root in by key only", sysd_companion_ssh_judge(get(p_sshdc)) == SYSD_SSH_HARDENED);
    {
        char orig[320];

        snprintf(orig, sizeof(orig), "%s.doors-orig", p_sshdc);
        check("the original is kept beside it", get(orig) && strcmp(get(orig), VENDOR_SSHD) == 0);
    }
    check("root has a password, by gatewayd's own gate", gw_gate_password_set(p_passwd, p_shadow));
    {
        char *t = get(p_shadow);
        struct crypt_data cd;
        char hash[256] = "";
        struct stat sb;

        memset(&cd, 0, sizeof(cd));
        if (t && strncmp(t, "root:", 5) == 0) {
            snprintf(hash, sizeof(hash), "%.*s", (int)strcspn(t + 5, ":"), t + 5);
        }
        check("the shadow line holds a hash of exactly that password", hash[0] && strcmp(crypt_r(PW, hash, &cd), hash) == 0);
        t = get(p_shadow);
        check("and the other line is untouched", t && strstr(t, "\ndaemon:*:::::::\n"));
        check("shadow stays 0600", stat(p_shadow, &sb) == 0 && (sb.st_mode & 0777) == 0600);
    }
    check("the password is in no log and no trace", !has(p_log, PW) && !has(trace, PW));
    check("the status: set, hardened", strcmp(status_str("password_set"), "true") == 0 &&
                                         strcmp(status_str("ssh"), "hardened") == 0);
    /* Again: SSH is already hardened, so sshd -t is not asked again - but the
     * running sshd is still told to read its file, every time. */
    unlink(trace);
    r = set_password("another one", word, msg);
    wait_job();
    check("a second password: the file is not rewritten, the running sshd is still reloaded first",
          r == 0 && strcmp(status_str("state"), "done") == 0 && !has(trace, "sshd -t") &&
              has(trace, "run S50sshd reload\nlock"));

    /* sshd refuses the hardened file: nothing changes at all. */
    setup();
    sshd_test_fails = 1;
    r = set_password(PW, word, msg);
    wait_job();
    check("sshd refusing the hardened file fails the job (ssh_failed)",
          r == 0 && strcmp(status_str("state"), "failed") == 0 && strcmp(status_str("error"), "ssh_failed") == 0);
    check("and leaves sshd_config, shadow and the password as they were",
          strcmp(get(p_sshdc), VENDOR_SSHD) == 0 && strcmp(get(p_shadow), SHADOW_EMPTY) == 0 &&
              !gw_gate_password_set(p_passwd, p_shadow));
    /* The reload fails while sshd runs: the password is not set - and not on
     * the retry either, although the file on disk is hardened by then
     * (independent review, finding 1). */
    setup();
    reload_fails = 1;
    r = set_password(PW, word, msg);
    wait_job();
    check("an SSH server that does not reload: no password (ssh_failed)",
          strcmp(status_str("error"), "ssh_failed") == 0 && !gw_gate_password_set(p_passwd, p_shadow));
    check("the file is hardened by then, the running server may not be",
          sysd_companion_ssh_judge(get(p_sshdc)) == SYSD_SSH_HARDENED);
    unlink(trace);
    r = set_password(PW, word, msg);
    wait_job();
    check("a retry still reloads, still fails, and still sets no password",
          has(trace, "run S50sshd reload") && strcmp(status_str("error"), "ssh_failed") == 0 &&
              !gw_gate_password_set(p_passwd, p_shadow) && !has(trace, "lock"));
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    check("nor is the gateway switched on meanwhile (no password)", r == 3);
    reload_fails = 0;
    r = set_password(PW, word, msg);
    wait_job();
    check("once the reload works, the password is set", strcmp(status_str("state"), "done") == 0 &&
                                                          gw_gate_password_set(p_passwd, p_shadow));
    /* An sshd whose pid file is gone is found by name in /proc. */
    setup();
    unlink(p_sshd_pid);
    {
        char d[320], comm[340];

        snprintf(d, sizeof(d), "%s/4321", p_proc);
        mkdir(d, 0755);
        snprintf(comm, sizeof(comm), "%s/comm", d);
        put(comm, "sshd\n", 0644);
        r = set_password(PW, word, msg);
        wait_job();
        check("an sshd without its pid file is still reloaded", has(trace, "run S50sshd reload") &&
                                                                 strcmp(status_str("state"), "done") == 0);
        unlink(comm);
        rmdir(d);
    }
    /* No SSH server and no configuration: nothing to harden. */
    setup();
    unlink(p_sshd);
    unlink(p_sshdc);
    check("neither sshd nor its configuration is 'absent'", strcmp(status_str("ssh"), "absent") == 0);
    r = set_password(PW, word, msg);
    wait_job();
    check("and the password is set", strcmp(status_str("state"), "done") == 0 && gw_gate_password_set(p_passwd, p_shadow));
    /* A configuration whose sshd is not runnable is hardened all the same. */
    setup();
    unlink(p_sshd);
    unlink(p_sshd_pid);
    check("a configuration without a runnable sshd is judged", strcmp(status_str("ssh"), "open") == 0);
    r = set_password(PW, word, msg);
    wait_job();
    check("and hardened before the password, without sshd -t",
          strcmp(status_str("state"), "done") == 0 && sysd_companion_ssh_judge(get(p_sshdc)) == SYSD_SSH_HARDENED &&
              !has(trace, "sshd -t"));
    /* sshd not running: written, not reloaded. */
    setup();
    unlink(p_sshd_pid);
    r = set_password(PW, word, msg);
    wait_job();
    check("an sshd that is not running is not reloaded; its file is hardened for its next start",
          strcmp(status_str("state"), "done") == 0 && !has(trace, "S50sshd") &&
              sysd_companion_ssh_judge(get(p_sshdc)) == SYSD_SSH_HARDENED);
    /* Lines sshd may read otherwise than sysd: unknown, refused. */
    check("a quoted keyword is unknown",
          sysd_companion_ssh_judge("\"PermitRootLogin\" yes\nPermitRootLogin no\n") == SYSD_SSH_UNKNOWN);
    check("a line that starts with '=' is unknown",
          sysd_companion_ssh_judge("=PermitRootLogin yes\nPermitRootLogin no\n") == SYSD_SSH_UNKNOWN);
    check("a CR before the keyword is read as sshd reads it",
          sysd_companion_ssh_judge("PermitRootLogin no\n\rPermitRootLogin yes\n") == SYSD_SSH_OPEN);
    check("and CRLF line ends are fine", sysd_companion_ssh_judge("PermitRootLogin no\r\nPort 22\r\n") == SYSD_SSH_HARDENED);
    setup();
    {
        static const char with_nul[] = "PermitRootLogin no\n\0PermitRootLogin yes\n";
        FILE *f = fopen(p_sshdc, "w");

        fwrite(with_nul, 1, sizeof(with_nul) - 1, f);
        fclose(f);
    }
    r = set_password(PW, word, msg);
    check("a configuration with a NUL byte cannot be judged: refused",
          r == 3 && strcmp(word, "ssh_unknown") == 0 && strcmp(status_str("ssh"), "unknown") == 0);

    /* ---- 8. the gateway on -------------------------------------------------------------- */
    setup();
    put(p_shadow, "root:$6$x$y:::::::\n", 0600);
    put(p_def, "# per unit\nGATEWAYD_INTERFACE=eth0\nGATEWAYD_ENABLE=0\n", 0644);
    run_sleeps_ms = 150; /* long enough for a second request to find it busy */
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    check("on with a password: accepted", r == 0);
    r = sysd_companion_set_enabled(&C, false, word, sizeof(word), msg, sizeof(msg));
    check("a second change while one runs: busy (5)", r == 5 && strcmp(word, "busy") == 0);
    wait_job();
    check("on: done", strcmp(status_str("state"), "done") == 0 && strcmp(status_str("action"), "enable") == 0);
    check("SSH hardened first, then the switch, then start, then gatewayd answered",
          has(trace, "run sshd -t -f sshd_config.doors-check\n  (checked a hardened file)\nrun S50sshd reload\n"
                     "run S70gatewayd start\n  (enabled in the file)\ncall gatewayd gateway.status"));
    check("the file says enabled, and keeps the interface and the comment",
          strcmp(get(p_def), "# per unit\nGATEWAYD_INTERFACE=eth0\nGATEWAYD_ENABLE=1\n") == 0 &&
              strcmp(status_str("enabled"), "true") == 0);

    /* It starts but never answers: back off. */
    setup();
    put(p_shadow, "root:$6$x$y:::::::\n", 0600);
    put(p_sshdc, "PermitRootLogin no\n", 0644);
    gateway_answers = 0;
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a gateway that never answers: no_answer",
          r == 0 && strcmp(status_str("error"), "no_answer") == 0 && strstr(status_str("message"), "it is off again"));
    check("and it is off again: the file says 0 and it was stopped",
          has(p_def, "GATEWAYD_ENABLE=0") && has(trace, "run S70gatewayd stop\n  (disabled in the file)") &&
              strcmp(status_str("enabled"), "false") == 0);
    check("an sshd_config that was hardened already is not touched", !has(trace, "sshd -t"));
    /* The start fails. */
    setup();
    put(p_shadow, "root:$6$x$y:::::::\n", 0600);
    put(p_def, "GATEWAYD_ENABLE=0\nGATEWAYD_INTERFACE=wlan0\n", 0644);
    init_start_fails = 1;
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a start that fails: start_failed, and the file exactly as it was",
          strcmp(status_str("error"), "start_failed") == 0 &&
              strcmp(get(p_def), "GATEWAYD_ENABLE=0\nGATEWAYD_INTERFACE=wlan0\n") == 0);

    /* ---- 9. the gateway off -------------------------------------------------------------- */
    setup();
    put(p_def, "GATEWAYD_ENABLE=1\n", 0644);
    r = sysd_companion_set_enabled(&C, false, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("off is always accepted, without a password, and SSH is not touched",
          r == 0 && strcmp(status_str("state"), "done") == 0 && !has(trace, "sshd"));
    check("the switch is written before the stop", has(trace, "run S70gatewayd stop\n  (disabled in the file)") &&
                                                       has(p_def, "GATEWAYD_ENABLE=0"));
    setup();
    put(p_def, "GATEWAYD_ENABLE=1\n", 0644);
    init_stop_fails = 1;
    r = sysd_companion_set_enabled(&C, false, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a stop that fails says so, and the unit will not start it at the next boot",
          strcmp(status_str("error"), "stop_failed") == 0 && strstr(status_str("message"), "next boot") &&
              has(p_def, "GATEWAYD_ENABLE=0"));
    setup();
    r = sysd_companion_set_enabled(&C, false, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("off on a unit that never had the file: written as off", has(p_def, "GATEWAYD_ENABLE=0") &&
                                                                       strcmp(status_str("state"), "done") == 0);

    /* A hand-written file with shell in it is not edited line by line. */
    setup();
    put(p_shadow, "root:$6$x$y:::::::\n", 0600);
    put(p_sshdc, "PermitRootLogin no\n", 0644);
    put(p_def, "if [ -e /tmp/x ]; then\nGATEWAYD_ENABLE=0\nfi\n", 0644);
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a per-unit file with shell in it: refused, and left exactly as it was",
          strcmp(status_str("error"), "defaults_unsupported") == 0 &&
              strcmp(get(p_def), "if [ -e /tmp/x ]; then\nGATEWAYD_ENABLE=0\nfi\n") == 0 &&
              !has(trace, "S70gatewayd start"));
    /* Switched on by hand, not running, and the start fails: left off. */
    setup();
    put(p_shadow, "root:$6$x$y:::::::\n", 0600);
    put(p_sshdc, "PermitRootLogin no\n", 0644);
    put(p_def, "GATEWAYD_ENABLE=1\n", 0644);
    init_start_fails = 1;
    r = sysd_companion_set_enabled(&C, true, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a start that fails never leaves the switch on, even when it was on before",
          strcmp(status_str("error"), "start_failed") == 0 && has(p_def, "GATEWAYD_ENABLE=0") &&
              strcmp(status_str("enabled"), "false") == 0);
    /* A change that hangs is stopped. */
    setup();
    C.job_limit_s = 1;
    run_sleeps_ms = 1500;
    r = sysd_companion_set_enabled(&C, false, word, sizeof(word), msg, sizeof(msg));
    wait_job();
    check("a change that does not finish in its time is stopped and said (timed_out)",
          strcmp(status_str("state"), "failed") == 0 && strcmp(status_str("error"), "timed_out") == 0 &&
              !sysd_companion_busy(&C));

    /* ---- 10. a power action waits; the status of a running gateway ------------------------- */
    setup();
    {
        char pid[32];

        snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
        put(p_pid, pid, 0644);
        check("running: pos-supervise's record of the daemon is alive", strcmp(status_str("running"), "true") == 0);
        put(p_pid, "999999\n", 0644);
        check("a stale record is not running", strcmp(status_str("running"), "false") == 0);
    }

    {
        char cmd[400];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) {
            /* only a temporary directory */
        }
    }
    printf("sysd companion: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
