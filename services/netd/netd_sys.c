/*
 * netd's view of the machine. See netd_sys.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "netd_sys.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int netd_iface_name_valid(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    size_t i;

    if (n == 0 || n >= IFNAMSIZ || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '.' && name[i] != '-') {
            return -1;
        }
    }
    return 0;
}

static void set_path(char *dst, const char *value)
{
    snprintf(dst, NETD_PATH_MAX, "%s", value);
}

void netd_sys_init(struct netd_sys *s, const char *iface)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->iface, sizeof(s->iface), "%s", iface);
    set_path(s->class_net, "/sys/class/net");
    set_path(s->proc, "/proc");
    set_path(s->wpa_supplicant, "/usr/sbin/wpa_supplicant");
    set_path(s->udhcpc, "/sbin/udhcpc");
    set_path(s->dhcp_script, "/usr/share/udhcpc/default.script");
#if defined(NETD_TEST_HOOKS) && NETD_TEST_HOOKS
    {
        const char *root = getenv("NETD_TEST_ROOT");
        const char *v;

        if (root && root[0]) {
            snprintf(s->class_net, NETD_PATH_MAX, "%s/sys/class/net", root);
            snprintf(s->proc, NETD_PATH_MAX, "%s/proc", root);
            snprintf(s->dhcp_script, NETD_PATH_MAX, "%s/default.script", root);
            s->test_root = 1;
        }
        if ((v = getenv("NETD_TEST_WPA_SUPPLICANT")) && v[0]) {
            set_path(s->wpa_supplicant, v);
        }
        if ((v = getenv("NETD_TEST_UDHCPC")) && v[0]) {
            set_path(s->udhcpc, v);
        }
    }
#endif
}

static int exists(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0;
}

int netd_iface_present(const struct netd_sys *s)
{
    char path[NETD_PATH_MAX + 64];

    snprintf(path, sizeof(path), "%s/%s/wireless", s->class_net, s->iface);
    if (exists(path)) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/%s/phy80211", s->class_net, s->iface);
    return exists(path);
}

int netd_iface_set_up(const struct netd_sys *s, int up)
{
    struct ifreq ifr;
    int fd;
    int rc;

    if (s->test_root) {
        /* The test tree records the request where the test can see it. */
        char path[NETD_PATH_MAX + 64];
        FILE *f;

        snprintf(path, sizeof(path), "%s/%s/netd_up", s->class_net, s->iface);
        f = fopen(path, "w");
        if (!f) {
            return -1;
        }
        fprintf(f, "%d\n", up ? 1 : 0);
        return fclose(f) == 0 ? 0 : -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, s->iface, strlen(s->iface));
    rc = ioctl(fd, SIOCGIFFLAGS, &ifr);
    if (rc == 0) {
        if (up) {
            ifr.ifr_flags |= IFF_UP;
        } else {
            ifr.ifr_flags &= ~IFF_UP;
        }
        rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    close(fd);
    return rc;
}

int netd_iface_ipv4(const struct netd_sys *s, char *buf, size_t n)
{
    struct ifreq ifr;
    struct in_addr addr;
    int fd;

    if (s->test_root) {
        char path[NETD_PATH_MAX + 64];
        char text[32];
        FILE *f;

        snprintf(path, sizeof(path), "%s/%s/ipv4", s->class_net, s->iface);
        f = fopen(path, "r");
        if (!f) {
            return -1;
        }
        if (!fgets(text, sizeof(text), f)) {
            fclose(f);
            return -1;
        }
        fclose(f);
        text[strcspn(text, "\r\n")] = '\0';
        if (inet_pton(AF_INET, text, &addr) != 1) {
            return -1;
        }
        return inet_ntop(AF_INET, &addr, buf, (socklen_t)n) ? 0 : -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, s->iface, strlen(s->iface));
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    if (addr.s_addr == 0) {
        return -1;
    }
    return inet_ntop(AF_INET, &addr, buf, (socklen_t)n) ? 0 : -1;
}

/* The NUL-separated argv of /proc/<pid>/cmdline. Returns the argument count
 * copied into args (pointers into buf), or -1. */
static int read_cmdline(const char *proc, const char *pid, char *buf, size_t n, const char **args,
                        int max)
{
    char path[NETD_PATH_MAX * 2 + 16];
    ssize_t len;
    int fd;
    int count = 0;
    ssize_t i;

    snprintf(path, sizeof(path), "%s/%s/cmdline", proc, pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    len = read(fd, buf, n - 1);
    close(fd);
    if (len <= 0) {
        return -1;
    }
    buf[len] = '\0';
    for (i = 0; i < len && count < max;) {
        args[count++] = buf + i;
        i += (ssize_t)strlen(buf + i) + 1;
    }
    return count;
}

pid_t netd_foreign_supplicant(const struct netd_sys *s, pid_t own)
{
    DIR *d = opendir(s->proc);
    struct dirent *e;
    pid_t found = 0;

    if (!d) {
        return 0;
    }
    while (!found && (e = readdir(d)) != NULL) {
        char buf[1024];
        const char *args[64];
        const char *base;
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        int argc;
        int i;

        if (*end != '\0' || pid <= 0 || pid == own || pid == getpid()) {
            continue;
        }
        argc = read_cmdline(s->proc, e->d_name, buf, sizeof(buf), args, 64);
        if (argc <= 0) {
            continue;
        }
        base = strrchr(args[0], '/');
        base = base ? base + 1 : args[0];
        if (strcmp(base, "wpa_supplicant") != 0) {
            continue;
        }
        for (i = 1; i < argc; i++) {
            /* "-i wlan0", "-iwlan0", and anything else naming it exactly */
            if (strcmp(args[i], s->iface) == 0 ||
                (strncmp(args[i], "-i", 2) == 0 && strcmp(args[i] + 2, s->iface) == 0)) {
                found = (pid_t)pid;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

pid_t netd_spawn(const char *const argv[])
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        long fd;
        long max = sysconf(_SC_OPEN_MAX);
        int devnull;
        sigset_t none;

        /* If netd dies, its children must not outlive it holding the radio:
         * the next netd would find the interface taken. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(127);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
        }
        if (max < 0 || max > 4096) {
            max = 4096;
        }
        for (fd = 3; fd < max; fd++) {
            close((int)fd);
        }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    return pid;
}

int netd_stop_child(pid_t pid, int timeout_ms)
{
    struct timespec step = { 0, 20 * 1000000L };
    int status;
    int waited;

    if (pid <= 0) {
        return -1;
    }
    kill(pid, SIGTERM);
    for (waited = 0; waited < timeout_ms; waited += 20) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) {
            return status;
        }
        if (r < 0) {
            return -1;
        }
        nanosleep(&step, NULL);
    }
    kill(pid, SIGKILL);
    return waitpid(pid, &status, 0) == pid ? status : -1;
}
