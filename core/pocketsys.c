/*
 * pocketsys implementation. See pocketsys.h and docs/api/system.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketsys.h"

#include "pocketpaths.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* The wall clock counts from 1970 on every boot until NTP syncs (no RTC on
 * the K230, VERIFIED unit A). A time before this is not a time. */
#define POCKETSYS_CLOCK_PLAUSIBLE_AFTER 1735689600L /* 2025-01-01T00:00:00Z */

/* Mount points system.status reports on when present in /proc/mounts. */
static const char *const watched_mounts[] = { "/", "/boot", "/data" };

/* ---- paths and small readers ------------------------------------------ */

static const char *root_prefix(void)
{
    const char *r = getenv("POCKETSYS_ROOT");

    return r ? r : "";
}

/* Absolute path under the (possibly faked) root. */
static const char *at(char *buf, size_t n, const char *path)
{
    snprintf(buf, n, "%s%s", root_prefix(), path);
    return buf;
}

/* First line of the file at an exact path, newline stripped, NUL-terminated
 * device-tree strings included (fread, not fgets). 0 on success. */
static int read_line_exact(const char *full, char *buf, size_t n)
{
    FILE *f = fopen(full, "r");
    size_t len;

    if (!f) {
        return -1;
    }
    len = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[len] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* The same for a system path (/proc, /sys, /etc), under the fake root when
 * one is set. The runtime directory is not a system path: it already comes
 * from pocketpaths with its own override, so it is read with read_line_exact. */
static int read_line(const char *path, char *buf, size_t n)
{
    char full[POCKETOS_PATH_MAX];

    return read_line_exact(at(full, sizeof(full), path), buf, n);
}

static FILE *open_at(const char *path, const char *mode)
{
    char full[POCKETOS_PATH_MAX];

    return fopen(at(full, sizeof(full), path), mode);
}

/* The first line starting with "<key>:" in a "key: value" file, as a long,
 * or -1 when the key is absent. */
static long keyed_long(const char *path, const char *key)
{
    FILE *f = open_at(path, "r");
    char line[256];
    size_t klen = strlen(key);
    long v = -1;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void add_string_or_null(cJSON *o, const char *key, const char *value)
{
    if (value) {
        cJSON_AddStringToObject(o, key, value);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Sorted directory entry names (excluding dot files) so the JSON arrays are
 * stable across calls; readdir order is not. Returns the count, names are
 * strdup'd into names[] (caller frees), at most max of them. */
static int list_dir(const char *path, char **names, int max)
{
    char full[POCKETOS_PATH_MAX];
    DIR *d = opendir(at(full, sizeof(full), path));
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') {
            continue;
        }
        names[n] = strdup(e->d_name);
        if (names[n]) {
            n++;
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), name_cmp);
    return n;
}

static void free_names(char **names, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        free(names[i]);
    }
}

/* ---- CPU sampler ------------------------------------------------------ */

void pocketsys_cpu_init(struct pocketsys_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
}

int pocketsys_cpu_sample(struct pocketsys_cpu *cpu)
{
    FILE *f = open_at("/proc/stat", "r");
    char line[512];
    unsigned long long v[8] = { 0 };
    unsigned long long busy;
    unsigned long long total;
    int n;

    if (!f) {
        return -1;
    }
    /* "cpu  user nice system idle iowait irq softirq steal ..." (aggregate). */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        errno = EINVAL;
        return -1;
    }
    fclose(f);
    n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
               &v[4], &v[5], &v[6], &v[7]);
    if (n < 4) {
        errno = EINVAL;
        return -1;
    }
    busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
    total = busy + v[3] + v[4];
    if (cpu->have_sample && total > cpu->total) {
        unsigned long long dbusy = busy >= cpu->busy ? busy - cpu->busy : 0;

        cpu->percent = 100.0 * (double)dbusy / (double)(total - cpu->total);
        cpu->have_percent = 1;
    }
    cpu->busy = busy;
    cpu->total = total;
    cpu->have_sample = 1;
    return 0;
}

/* ---- system.info ------------------------------------------------------ */

/* The "sdk:" line of the vendor SDK's release file, or NULL. */
static const char *vendor_sdk(char *buf, size_t n)
{
    FILE *f = open_at("/etc/version/release_version", "r");
    const char *found = NULL;

    if (!f) {
        return NULL;
    }
    while (fgets(buf, (int)n, f)) {
        if (strncmp(buf, "sdk:", 4) == 0) {
            buf[strcspn(buf, "\n")] = '\0';
            found = buf + 4;
            break;
        }
    }
    fclose(f);
    return found;
}

/* PRETTY_NAME from /etc/os-release without its quotes, or NULL. */
static const char *os_name(char *buf, size_t n)
{
    FILE *f = open_at("/etc/os-release", "r");
    const char *found = NULL;

    if (!f) {
        return NULL;
    }
    while (fgets(buf, (int)n, f)) {
        if (strncmp(buf, "PRETTY_NAME=", 12) == 0) {
            char *v = buf + 12;
            size_t len;

            v[strcspn(v, "\n")] = '\0';
            len = strlen(v);
            if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
                v[len - 1] = '\0';
                v++;
            }
            found = v;
            break;
        }
    }
    fclose(f);
    return found;
}

cJSON *pocketsys_info(const char *version, const char *build)
{
    cJSON *o = cJSON_CreateObject();
    struct utsname u;
    char buf[256];
    long cpus;

    cJSON_AddStringToObject(o, "version", version ? version : "unknown");
    cJSON_AddStringToObject(o, "build", build ? build : "unknown");
    add_string_or_null(o, "release_file",
                       read_line("/etc/pocketos-release", buf, sizeof(buf)) == 0 ? buf : NULL);
    add_string_or_null(o, "model",
                       read_line("/proc/device-tree/model", buf, sizeof(buf)) == 0 ? buf : NULL);
    if (uname(&u) == 0) {
        cJSON_AddStringToObject(o, "kernel", u.release);
        cJSON_AddStringToObject(o, "machine", u.machine);
        cJSON_AddStringToObject(o, "hostname", u.nodename);
    } else {
        cJSON_AddNullToObject(o, "kernel");
        cJSON_AddNullToObject(o, "machine");
        cJSON_AddNullToObject(o, "hostname");
    }
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0) {
        cJSON_AddNumberToObject(o, "cpus", (double)cpus);
    } else {
        cJSON_AddNullToObject(o, "cpus");
    }
    add_string_or_null(o, "vendor_sdk", vendor_sdk(buf, sizeof(buf)));
    add_string_or_null(o, "os", os_name(buf, sizeof(buf)));
    return o;
}

/* ---- system.status pieces --------------------------------------------- */

static void add_uptime_and_load(cJSON *o)
{
    char buf[128];
    double up;
    double l1;
    double l5;
    double l15;

    if (read_line("/proc/uptime", buf, sizeof(buf)) == 0 && sscanf(buf, "%lf", &up) == 1) {
        cJSON_AddNumberToObject(o, "uptime_s", (double)(long)up);
    } else {
        cJSON_AddNullToObject(o, "uptime_s");
    }
    if (read_line("/proc/loadavg", buf, sizeof(buf)) == 0 &&
        sscanf(buf, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
        cJSON *load = cJSON_CreateArray();

        cJSON_AddItemToArray(load, cJSON_CreateNumber(l1));
        cJSON_AddItemToArray(load, cJSON_CreateNumber(l5));
        cJSON_AddItemToArray(load, cJSON_CreateNumber(l15));
        cJSON_AddItemToObject(o, "load", load);
    } else {
        cJSON_AddNullToObject(o, "load");
    }
}

static void add_memory(cJSON *o)
{
    long total = keyed_long("/proc/meminfo", "MemTotal");
    cJSON *m;

    if (total < 0) {
        cJSON_AddNullToObject(o, "memory");
        return;
    }
    m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "total_kb", (double)total);
    cJSON_AddNumberToObject(m, "available_kb", (double)keyed_long("/proc/meminfo", "MemAvailable"));
    cJSON_AddNumberToObject(m, "free_kb", (double)keyed_long("/proc/meminfo", "MemFree"));
    cJSON_AddItemToObject(o, "memory", m);
}

static void add_temperature(cJSON *o)
{
    char buf[64];
    long milli;

    if (read_line("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf)) == 0 &&
        sscanf(buf, "%ld", &milli) == 1) {
        cJSON_AddNumberToObject(o, "temperature_c", (double)milli / 1000.0);
    } else {
        cJSON_AddNullToObject(o, "temperature_c");
    }
}

/* One entry per watched mount point that /proc/mounts lists. */
static void add_storage(cJSON *o)
{
    FILE *f = open_at("/proc/mounts", "r");
    cJSON *arr = cJSON_CreateArray();
    char line[512];

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char dev[128];
            char mnt[256];
            size_t i;

            if (sscanf(line, "%127s %255s", dev, mnt) != 2) {
                continue;
            }
            for (i = 0; i < sizeof(watched_mounts) / sizeof(watched_mounts[0]); i++) {
                char full[POCKETOS_PATH_MAX];
                struct statvfs st;
                cJSON *e;

                if (strcmp(mnt, watched_mounts[i]) != 0) {
                    continue;
                }
                if (statvfs(at(full, sizeof(full), mnt), &st) != 0) {
                    continue;
                }
                e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "mount", mnt);
                cJSON_AddNumberToObject(e, "total_bytes", (double)st.f_blocks * (double)st.f_frsize);
                cJSON_AddNumberToObject(e, "avail_bytes", (double)st.f_bavail * (double)st.f_frsize);
                cJSON_AddItemToArray(arr, e);
            }
        }
        fclose(f);
    }
    cJSON_AddItemToObject(o, "storage", arr);
}

/* IPv4 of an interface through the live kernel, or NULL. Under a fake root
 * the name is not a real interface and this is NULL, as intended. */
static const char *ipv4_of(int sock, const char *name, char *buf, size_t n)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;

    if (sock < 0 || strlen(name) >= IFNAMSIZ) {
        return NULL;
    }
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, name, strlen(name) + 1);
    if (ioctl(sock, SIOCGIFADDR, &ifr) != 0) {
        return NULL;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    return inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)n);
}

static void add_network(cJSON *o)
{
    char *names[32];
    cJSON *arr = cJSON_CreateArray();
    int n = list_dir("/sys/class/net", names, 32);
    int sock = n > 0 ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
    int i;

    for (i = 0; i < n; i++) {
        char path[128];
        char buf[64];
        char ip[INET_ADDRSTRLEN];
        cJSON *e;

        if (strcmp(names[i], "lo") == 0) {
            continue;
        }
        e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", names[i]);
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", names[i]);
        add_string_or_null(e, "operstate", read_line(path, buf, sizeof(buf)) == 0 ? buf : NULL);
        /* carrier reads EINVAL while the interface is down: unknown, not false */
        snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", names[i]);
        if (read_line(path, buf, sizeof(buf)) == 0 && (buf[0] == '0' || buf[0] == '1')) {
            cJSON_AddBoolToObject(e, "carrier", buf[0] == '1');
        } else {
            cJSON_AddNullToObject(e, "carrier");
        }
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", names[i]);
        add_string_or_null(e, "mac", read_line(path, buf, sizeof(buf)) == 0 ? buf : NULL);
        add_string_or_null(e, "ipv4", ipv4_of(sock, names[i], ip, sizeof(ip)));
        cJSON_AddItemToArray(arr, e);
    }
    if (sock >= 0) {
        close(sock);
    }
    if (n > 0) {
        free_names(names, n);
    }
    cJSON_AddItemToObject(o, "network", arr);
}

/* The power_supply class is empty on the main board (VERIFIED unit A): no
 * gauge means the board runs from its external supply. A supply of type
 * Battery would come from a base board; its state is not interpreted here. */
static void add_power(cJSON *o)
{
    char *names[16];
    cJSON *p = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int n = list_dir("/sys/class/power_supply", names, 16);
    int battery = 0;
    int i;

    for (i = 0; i < n; i++) {
        char path[160];
        char type[64];
        cJSON *e = cJSON_CreateObject();

        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", names[i]);
        cJSON_AddStringToObject(e, "name", names[i]);
        if (read_line(path, type, sizeof(type)) == 0) {
            cJSON_AddStringToObject(e, "type", type);
            if (strcmp(type, "Battery") == 0) {
                battery = 1;
            }
        } else {
            cJSON_AddNullToObject(e, "type");
        }
        cJSON_AddItemToArray(arr, e);
    }
    if (n > 0) {
        free_names(names, n);
    }
    cJSON_AddStringToObject(p, "source", battery ? "unknown" : "external");
    cJSON_AddItemToObject(p, "supplies", arr);
    cJSON_AddItemToObject(o, "power", p);
}

/* Supervised services, from what pos-supervise writes in the runtime dir:
 * <name>.pid while it watches a daemon, <name>.crashloop when it gave up
 * ("<utc> rc=<n> restarts=<m>"). A name is listed when either file exists. */
static void add_services(cJSON *o)
{
    char *entries[64];
    char *names[64];
    cJSON *arr = cJSON_CreateArray();
    const char *run = pocketos_runtime_dir();
    DIR *d = opendir(run);
    struct dirent *e;
    int n = 0;
    int count = 0;
    int i;

    if (d) {
        while ((e = readdir(d)) != NULL && n < 64) {
            size_t len = strlen(e->d_name);
            const char *suffix = NULL;

            if (len > 4 && strcmp(e->d_name + len - 4, ".pid") == 0) {
                suffix = e->d_name + len - 4;
            } else if (len > 10 && strcmp(e->d_name + len - 10, ".crashloop") == 0) {
                suffix = e->d_name + len - 10;
            }
            if (!suffix) {
                continue;
            }
            entries[n] = strndup(e->d_name, (size_t)(suffix - e->d_name));
            if (entries[n]) {
                n++;
            }
        }
        closedir(d);
    }
    qsort(entries, (size_t)n, sizeof(entries[0]), name_cmp);
    for (i = 0; i < n; i++) {
        if (count == 0 || strcmp(names[count - 1], entries[i]) != 0) {
            names[count++] = entries[i];
        } else {
            free(entries[i]);
        }
    }
    for (i = 0; i < count; i++) {
        char path[POCKETOS_PATH_MAX];
        char buf[160];
        cJSON *s = cJSON_CreateObject();
        long pid = -1;
        const char *p;

        cJSON_AddStringToObject(s, "name", names[i]);
        snprintf(path, sizeof(path), "%s/%s.pid", run, names[i]);
        if (read_line_exact(path, buf, sizeof(buf)) == 0 && sscanf(buf, "%ld", &pid) == 1 &&
            pid > 0) {
            cJSON_AddNumberToObject(s, "pid", (double)pid);
            cJSON_AddBoolToObject(s, "running", kill((pid_t)pid, 0) == 0 || errno == EPERM);
        } else {
            cJSON_AddNullToObject(s, "pid");
            cJSON_AddBoolToObject(s, "running", 0);
        }
        snprintf(path, sizeof(path), "%s/%s.crashloop", run, names[i]);
        if (read_line_exact(path, buf, sizeof(buf)) == 0) {
            long rc = -1;
            long restarts = -1;

            cJSON_AddBoolToObject(s, "crashloop", 1);
            if ((p = strstr(buf, "rc=")) != NULL) {
                rc = strtol(p + 3, NULL, 10);
            }
            if ((p = strstr(buf, "restarts=")) != NULL) {
                restarts = strtol(p + 9, NULL, 10);
            }
            if (rc >= 0) {
                cJSON_AddNumberToObject(s, "last_exit_code", (double)rc);
            } else {
                cJSON_AddNullToObject(s, "last_exit_code");
            }
            if (restarts >= 0) {
                cJSON_AddNumberToObject(s, "restarts", (double)restarts);
            } else {
                cJSON_AddNullToObject(s, "restarts");
            }
        } else {
            cJSON_AddBoolToObject(s, "crashloop", 0);
        }
        cJSON_AddItemToArray(arr, s);
    }
    free_names(names, count);
    cJSON_AddItemToObject(o, "services", arr);
}

cJSON *pocketsys_status(const struct pocketsys_cpu *cpu)
{
    cJSON *o = cJSON_CreateObject();

    add_uptime_and_load(o);
    if (cpu && cpu->have_percent) {
        cJSON_AddNumberToObject(o, "cpu_percent", cpu->percent);
    } else {
        cJSON_AddNullToObject(o, "cpu_percent");
    }
    add_memory(o);
    add_temperature(o);
    cJSON_AddBoolToObject(o, "clock_set", time(NULL) >= POCKETSYS_CLOCK_PLAUSIBLE_AFTER);
    add_storage(o);
    add_network(o);
    add_power(o);
    add_services(o);
    return o;
}
