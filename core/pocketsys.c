/*
 * pocketsys implementation. See pocketsys.h and docs/api/system.md.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketsys.h"

#include "battery_report.h"
#include "pocketpaths.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
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
#define WATCHED_MOUNT_COUNT (sizeof(watched_mounts) / sizeof(watched_mounts[0]))

/* ---- paths and small readers ------------------------------------------ */

/* The fake root is a test-only build option (pocketsys.h). tests/pocketsys_test
 * is compiled with -DPOCKETSYS_TEST_HOOKS=1 and honours $POCKETSYS_ROOT; sysd
 * is compiled without it and reads the real system, whatever its environment
 * says. A shipped service that takes its entire view of the machine from an
 * environment variable is one whose facts anyone able to set that variable can
 * fabricate, and every client of system.* believes those facts. */
#ifdef POCKETSYS_TEST_HOOKS
static const char *root_prefix(void)
{
    const char *r = getenv("POCKETSYS_ROOT");

    return r ? r : "";
}
#else
static const char *root_prefix(void)
{
    return "";
}
#endif

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

/* The first line starting with "<key>:" in a "key: value" file, as a long.
 * Returns 0 when the key was found and carried a number, -1 when the file or
 * the key is absent or the value does not parse. An absent key is an absence,
 * never a -1 the caller could mistake for a measurement. */
static int keyed_long(const char *path, const char *key, long *out)
{
    FILE *f = open_at(path, "r");
    char line[256];
    size_t klen = strlen(key);
    int found = -1;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *end;
        long v;

        if (strncmp(line, key, klen) != 0 || line[klen] != ':') {
            continue;
        }
        v = strtol(line + klen + 1, &end, 10);
        if (end != line + klen + 1) {
            *out = v;
            found = 0;
        }
        break;
    }
    fclose(f);
    return found;
}

/* A "key: value" line as a JSON number, or JSON null when the file does not
 * carry it. */
static void add_keyed_long_or_null(cJSON *o, const char *field, const char *path, const char *key)
{
    long v;

    if (keyed_long(path, key, &v) == 0) {
        cJSON_AddNumberToObject(o, field, (double)v);
    } else {
        cJSON_AddNullToObject(o, field);
    }
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
    struct pocketos_release rel;
    int have_release;
    char buf[256];
    long cpus;

    cJSON_AddStringToObject(o, "version", version ? version : "unknown");
    cJSON_AddStringToObject(o, "build", build ? build : "unknown");
    /* The release file the card carries (pocketpaths.h: /etc/doors-release,
     * or /etc/pocketos-release on a card that has only that), read once so
     * that both fields come from the same file. Line 1 is the bare version;
     * the build identity is the BUILD_ID= line below it, written by the
     * Makefile install target. A card flashed before v0.0.7 carries only the
     * version line and reports a null release_build. */
    have_release = pocketos_release_read(root_prefix(), &rel) == 0;
    add_string_or_null(o, "release_file", have_release ? rel.version : NULL);
    add_string_or_null(o, "release_build", have_release && rel.build[0] ? rel.build : NULL);
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

/* Without MemTotal there is no /proc/meminfo worth reporting and the whole
 * object is null. With it, a key the kernel does not carry (MemAvailable is
 * absent before Linux 3.14) is null on its own. */
static void add_memory(cJSON *o)
{
    long total;
    cJSON *m;

    if (keyed_long("/proc/meminfo", "MemTotal", &total) != 0) {
        cJSON_AddNullToObject(o, "memory");
        return;
    }
    m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "total_kb", (double)total);
    add_keyed_long_or_null(m, "available_kb", "/proc/meminfo", "MemAvailable");
    add_keyed_long_or_null(m, "free_kb", "/proc/meminfo", "MemFree");
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

/* One entry per watched mount point that /proc/mounts lists, and one only:
 * a mount point can appear on several lines (an initramfs leaves "rootfs /"
 * ahead of "/dev/root /", and any remount or bind adds another), and two
 * identical rows for / would be a defect of this reader, not a fact about the
 * card. A line whose statvfs fails does not consume the slot: a later line
 * for the same mount point may still answer. */
static void add_storage(cJSON *o)
{
    FILE *f = open_at("/proc/mounts", "r");
    cJSON *arr = cJSON_CreateArray();
    char line[512];
    int listed[WATCHED_MOUNT_COUNT] = { 0 };

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char dev[128];
            char mnt[256];
            size_t i;

            if (sscanf(line, "%127s %255s", dev, mnt) != 2) {
                continue;
            }
            for (i = 0; i < WATCHED_MOUNT_COUNT; i++) {
                char full[POCKETOS_PATH_MAX];
                struct statvfs st;
                cJSON *e;

                if (listed[i] || strcmp(mnt, watched_mounts[i]) != 0) {
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
                listed[i] = 1;
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

/* One of the interface's statistics counters (/sys/class/net/<if>/statistics,
 * a decimal count since the driver loaded) as a JSON number, or null when the
 * file is absent or not a plain unsigned decimal. A double holds a byte count
 * exactly up to 2^53 (8 PiB), which no interface here reaches. */
static void add_counter_or_null(cJSON *o, const char *field, const char *ifname, const char *stat)
{
    char path[128];
    char buf[32];
    char *end;
    unsigned long long v;

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, stat);
    if (read_line(path, buf, sizeof(buf)) == 0 && buf[0] >= '0' && buf[0] <= '9') {
        v = strtoull(buf, &end, 10);
        if (*end == '\0') {
            cJSON_AddNumberToObject(o, field, (double)v);
            return;
        }
    }
    cJSON_AddNullToObject(o, field);
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
        add_counter_or_null(e, "rx_bytes", names[i], "rx_bytes");
        add_counter_or_null(e, "tx_bytes", names[i], "tx_bytes");
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

/* One attribute of a power supply as a long, or -1 when absent or not a
 * number. */
static int supply_long(const char *name, const char *attr, long *out)
{
    char path[160];
    char buf[64];
    char *end;
    long v;

    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", name, attr);
    if (read_line(path, buf, sizeof(buf)) != 0 || !buf[0]) {
        return -1;
    }
    v = strtol(buf, &end, 10);
    if (end == buf || (*end && *end != ' ')) {
        return -1;
    }
    *out = v;
    return 0;
}

/* The kernel's POWER_SUPPLY_STATUS words, as this API spells them. Anything
 * else is not guessed at. */
static const char *battery_status(const char *name)
{
    char path[160];
    char buf[64];

    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", name);
    if (read_line(path, buf, sizeof(buf)) != 0) {
        return NULL;
    }
    if (strcmp(buf, "Charging") == 0) {
        return "charging";
    }
    if (strcmp(buf, "Discharging") == 0) {
        return "discharging";
    }
    if (strcmp(buf, "Full") == 0) {
        return "full";
    }
    if (strcmp(buf, "Not charging") == 0) {
        return "not_charging";
    }
    if (strcmp(buf, "Unknown") == 0) {
        return "unknown";
    }
    return NULL;
}

/* The first supply of type Battery, described only by what its driver
 * reports: the percentage is the driver's `capacity` (a fuel gauge's own
 * figure), never computed from a voltage here, and a value outside 0..100 is
 * a driver fault reported as null rather than clamped into a plausible
 * number. */
static cJSON *battery_json(const char *name)
{
    cJSON *b = cJSON_CreateObject();
    const char *st = battery_status(name);
    long v;

    cJSON_AddStringToObject(b, "name", name);
    /* `present` is optional in the class; a supply without it is there. */
    cJSON_AddBoolToObject(b, "present", supply_long(name, "present", &v) != 0 || v != 0);
    if (supply_long(name, "capacity", &v) == 0 && v >= 0 && v <= 100) {
        cJSON_AddNumberToObject(b, "capacity_percent", (double)v);
    } else {
        cJSON_AddNullToObject(b, "capacity_percent");
    }
    if (st) {
        cJSON_AddStringToObject(b, "status", st);
    } else {
        cJSON_AddNullToObject(b, "status");
    }
    /* voltage_now is in microvolts. */
    if (supply_long(name, "voltage_now", &v) == 0 && v > 0) {
        cJSON_AddNumberToObject(b, "voltage_v", (double)(v / 1000) / 1000.0);
    } else {
        cJSON_AddNullToObject(b, "voltage_v");
    }
    /* The fields the keyboard-base report adds (below), for one shape. A
     * driver's current_now sign is not settled across drivers, so it is not
     * passed on. */
    cJSON_AddNullToObject(b, "current_a");
    cJSON_AddStringToObject(b, "reading", "ok");
    cJSON_AddNullToObject(b, "age_s");
    cJSON_AddNullToObject(b, "gauge");
    return b;
}

/* ---- the keyboard base's gauge, as the shell reports it ------------------
 *
 * No kernel driver binds the base's BQ27220 and BQ25896 (VERIFIED unit A);
 * the shell reads them on the bus it owns and leaves the reading in the
 * runtime directory (core/battery_report.h). It is used only when the
 * power_supply class has no battery of its own.
 */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

#ifdef POCKETSYS_TEST_HOOKS
    const char *fake = getenv("POCKETSYS_MONOTONIC_MS");

    if (fake && *fake) {
        return strtoull(fake, NULL, 10);
    }
#endif
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The report, or -1 when there is none or it does not parse. */
static int read_report(struct battery_report *r)
{
    char path[POCKETOS_PATH_MAX];
    char text[1024];
    size_t n;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", pocketos_runtime_dir(), BATTERY_REPORT_FILE);
    f = open_at(path, "r");
    if (!f) {
        return -1;
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return battery_report_parse(text, r);
}

/* A number or null. */
static void add_num(cJSON *o, const char *key, bool have, double v)
{
    if (have) {
        cJSON_AddNumberToObject(o, key, v);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

/* The battery object for a report, or NULL when the report says there is no
 * battery (no keyboard base). *external is 1/0 from the charger, -1 unknown.
 *
 * capacity_percent stays null: the gauge's percentage rests on a gauge
 * configuration nobody has validated for the fitted pack (BATTERY_PROBE.md
 * §7), so it goes under `gauge`, marked unvalidated, and nowhere a normal
 * screen would take it for the battery level. A reading older than
 * BATTERY_REPORT_STALE_MS, or stamped in the future, carries no values at
 * all: an old reading is never passed on as the current one. */
static cJSON *battery_from_report(const struct battery_report *r, int *external)
{
    uint64_t now = monotonic_ms();
    bool fresh = r->monotonic_ms <= now && now - r->monotonic_ms <= BATTERY_REPORT_STALE_MS;
    bool ok = fresh && r->state == BATTERY_REPORT_OK;
    const char *word = ok ? battery_report_status_word(r) : NULL;
    const char *reading;
    cJSON *b;
    cJSON *g;

    *external = ok ? battery_report_external(r) : -1;
    if (fresh && r->state == BATTERY_REPORT_BASE_ABSENT) {
        return NULL;
    }
    if (!fresh) {
        reading = "stale";
    } else if (r->state == BATTERY_REPORT_OK) {
        reading = "ok";
    } else if (r->state == BATTERY_REPORT_STOPPED) {
        reading = "stopped";
    } else {
        reading = "no-answer";
    }
    b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "name", "bq27220");
    if (ok && r->has_battery_status) {
        cJSON_AddBoolToObject(b, "present", (r->battery_status & BATTERY_REPORT_BS_BATTPRES) != 0);
    } else {
        cJSON_AddNullToObject(b, "present");
    }
    cJSON_AddNullToObject(b, "capacity_percent");
    if (word) {
        cJSON_AddStringToObject(b, "status", word);
    } else {
        cJSON_AddNullToObject(b, "status");
    }
    add_num(b, "voltage_v", ok && r->has_voltage && r->voltage_mv > 0, r->voltage_mv / 1000.0);
    add_num(b, "current_a", ok && r->has_current, r->current_ma / 1000.0);
    cJSON_AddStringToObject(b, "reading", reading);
    add_num(b, "age_s", r->monotonic_ms <= now, (double)((now - r->monotonic_ms) / 1000u));
    if (ok && (r->has_soc || r->has_fcc || r->has_design)) {
        g = cJSON_CreateObject();
        cJSON_AddBoolToObject(g, "validated", false);
        add_num(g, "soc_percent", r->has_soc, r->soc_percent);
        add_num(g, "full_charge_capacity_mah", r->has_fcc, r->fcc_mah);
        add_num(g, "design_capacity_mah", r->has_design, r->design_mah);
        cJSON_AddItemToObject(b, "gauge", g);
    } else {
        cJSON_AddNullToObject(b, "gauge");
    }
    return b;
}

static bool external_type(const char *type)
{
    return strcmp(type, "Mains") == 0 || strncmp(type, "USB", 3) == 0;
}

/* The power_supply class is empty on the main board (VERIFIED unit A): no
 * gauge means the board runs from its external supply. A supply of type
 * Battery would come from a base board (BQ27220 gauge, BQ25896 charger,
 * DOCUMENTED; no kernel driver binds them on unit A). What its driver
 * reports is passed on as the battery object; nothing is inferred beyond
 * which source is in use, and that only from an explicit `online` or a
 * charging state. */
static void add_power(cJSON *o)
{
    char *names[16];
    cJSON *p = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON *battery = NULL;
    int n = list_dir("/sys/class/power_supply", names, 16);
    int externals = 0;
    int online = 0;
    int offline = 0;
    const char *source;
    int i;

    for (i = 0; i < n; i++) {
        char path[160];
        char type[64];
        cJSON *e = cJSON_CreateObject();

        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", names[i]);
        cJSON_AddStringToObject(e, "name", names[i]);
        if (read_line(path, type, sizeof(type)) == 0) {
            long v;

            cJSON_AddStringToObject(e, "type", type);
            if (strcmp(type, "Battery") == 0 && !battery) {
                battery = battery_json(names[i]);
            } else if (external_type(type)) {
                externals++;
                if (supply_long(names[i], "online", &v) == 0) {
                    cJSON_AddBoolToObject(e, "online", v != 0);
                    if (v != 0) {
                        online++;
                    } else {
                        offline++;
                    }
                }
            }
        } else {
            cJSON_AddNullToObject(e, "type");
        }
        cJSON_AddItemToArray(arr, e);
    }
    if (n > 0) {
        free_names(names, n);
    }
    if (!battery) {
        struct battery_report r;
        int external = -1;

        if (read_report(&r) == 0) {
            battery = battery_from_report(&r, &external);
        }
        /* The base's charger, when it was read, is the external supply. */
        if (external >= 0) {
            externals++;
            if (external) {
                online++;
            } else {
                offline++;
            }
        }
    }
    if (!battery) {
        source = "external";
    } else if (online > 0) {
        source = "external";
    } else if (externals > 0 && offline == externals) {
        source = "battery";
    } else {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(battery, "status");
        const char *w = cJSON_IsString(st) ? st->valuestring : "";

        /* Charging or full needs a supply; discharging is running on the
         * battery. Anything else says nothing about the source. */
        if (strcmp(w, "charging") == 0 || strcmp(w, "full") == 0) {
            source = "external";
        } else if (strcmp(w, "discharging") == 0) {
            source = "battery";
        } else {
            source = "unknown";
        }
    }
    cJSON_AddStringToObject(p, "source", source);
    if (online > 0) {
        cJSON_AddBoolToObject(p, "external_online", true);
    } else if (externals > 0 && offline == externals) {
        cJSON_AddBoolToObject(p, "external_online", false);
    } else {
        cJSON_AddNullToObject(p, "external_online");
    }
    if (battery) {
        cJSON_AddItemToObject(p, "battery", battery);
    } else {
        cJSON_AddNullToObject(p, "battery");
    }
    cJSON_AddItemToObject(p, "supplies", arr);
    cJSON_AddItemToObject(o, "power", p);
}

/* Bluetooth controllers the kernel has registered (/sys/class/bluetooth,
 * hci0 and so on; the hciN:conn children are connections, not controllers).
 * VERIFIED none on unit A: the RTL8189FTV has no Bluetooth and no HCI
 * device appears. Presence only: whether a controller is powered is an HCI
 * ioctl, not a sysfs file, and nothing in Doors owns Bluetooth yet. */
static void add_bluetooth(cJSON *o)
{
    char *names[16];
    cJSON *b = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int n = list_dir("/sys/class/bluetooth", names, 16);
    int i;

    for (i = 0; i < n; i++) {
        if (strncmp(names[i], "hci", 3) == 0 && !strchr(names[i], ':')) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
        }
    }
    if (n > 0) {
        free_names(names, n);
    }
    cJSON_AddItemToObject(b, "controllers", arr);
    cJSON_AddItemToObject(o, "bluetooth", b);
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
    add_bluetooth(o);
    return o;
}
