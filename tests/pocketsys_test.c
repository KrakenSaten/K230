/*
 * pocketsys tests against a fake root ($POCKETSYS_ROOT) and a fake runtime
 * directory ($POCKETOS_RUNTIME_DIR): every field, every absence, and the
 * CPU sampler's arithmetic.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketsys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static char root[] = "/tmp/pos_sys.XXXXXX";
static char run[] = "/tmp/pos_run.XXXXXX";

static void check(const char *name, int ok)
{
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static void mkdirs(const char *rel)
{
    char cmd[700];

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/%s'", root, rel);
    if (system(cmd) != 0) {
        fprintf(stderr, "mkdir %s failed\n", rel);
        exit(1);
    }
}

/* Write text (may contain NUL when len is given) below the fake root. */
static void put(const char *rel, const char *text, size_t len)
{
    char path[700];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fwrite(text, 1, len ? len : strlen(text), f);
    fclose(f);
}

static void rm(const char *rel)
{
    char path[700];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    unlink(path);
}

static const cJSON *get(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static int str_is(const cJSON *o, const char *key, const char *want)
{
    const cJSON *v = get(o, key);

    return cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;
}

static int num_is(const cJSON *o, const char *key, double want)
{
    const cJSON *v = get(o, key);

    return cJSON_IsNumber(v) && v->valuedouble > want - 1e-6 && v->valuedouble < want + 1e-6;
}

static const cJSON *find_named(const cJSON *arr, const char *name)
{
    const cJSON *e;

    cJSON_ArrayForEach(e, arr) {
        if (str_is(e, "name", name)) {
            return e;
        }
    }
    return NULL;
}

static void fake_root(void)
{
    char pidline[32];

    mkdirs("proc/device-tree");
    mkdirs("etc/version");
    mkdirs("sys/class/thermal/thermal_zone0");
    mkdirs("sys/class/net/eth0");
    mkdirs("sys/class/net/wlan0");
    mkdirs("sys/class/net/nosuch0"); /* a name no host kernel has: ipv4 must be null */
    mkdirs("sys/class/net/lo");
    mkdirs("sys/class/power_supply");
    mkdirs("boot");
    put("proc/device-tree/model", "Canaan CanMV-K230 with RM69A10 OLED\0", 36);
    put("proc/uptime", "5125.90 4959.28\n", 0);
    put("proc/loadavg", "0.52 0.30 0.21 1/95 1234\n", 0);
    put("proc/meminfo",
        "MemTotal:         990544 kB\nMemFree:          900952 kB\nMemAvailable:     928932 kB\n"
        "Buffers:            1848 kB\n", 0);
    put("proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 100 0 100 800 0 0 0 0 0 0\n", 0);
    put("proc/mounts",
        "/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw 0 0\n"
        "tmpfs /run tmpfs rw 0 0\n/dev/mmcblk1p1 /boot ext4 rw,relatime 0 0\n", 0);
    put("etc/pocketos-release", "0.0.6\n", 0);
    put("etc/version/release_version",
        "#############SDK VERSION####\nsdk:v1.2-20260909-22d02c6\nCONF:k230_pocketos\n", 0);
    put("etc/os-release", "NAME=Buildroot\nPRETTY_NAME=\"Buildroot 2025.02.1\"\n", 0);
    put("sys/class/thermal/thermal_zone0/type", "canaan_thermal_zone\n", 0);
    put("sys/class/thermal/thermal_zone0/temp", "49760\n", 0);
    put("sys/class/net/eth0/operstate", "up\n", 0);
    put("sys/class/net/eth0/carrier", "1\n", 0);
    put("sys/class/net/eth0/address", "00:e0:4c:3a:5e:d0\n", 0);
    put("sys/class/net/wlan0/operstate", "down\n", 0);
    put("sys/class/net/wlan0/address", "88:3b:dc:b7:9e:c7\n", 0);
    put("sys/class/net/nosuch0/operstate", "down\n", 0);
    put("sys/class/net/lo/operstate", "unknown\n", 0);

    /* supervisor files: this process stands in for a running radiod, and
     * the shell is in a crash loop with the marker pos-supervise writes */
    snprintf(pidline, sizeof(pidline), "%ld\n", (long)getpid());
    {
        char path[700];
        FILE *f;

        snprintf(path, sizeof(path), "%s/radiod.pid", run);
        f = fopen(path, "w");
        fputs(pidline, f);
        fclose(f);
        snprintf(path, sizeof(path), "%s/pocketos-shell.crashloop", run);
        f = fopen(path, "w");
        fputs("2026-09-09T16:14:23Z rc=139 restarts=6\n", f);
        fclose(f);
        snprintf(path, sizeof(path), "%s/stale.pid", run);
        f = fopen(path, "w");
        fputs("999999999\n", f);
        fclose(f);
    }
}

int main(void)
{
    cJSON *info;
    cJSON *st;
    const cJSON *arr;
    const cJSON *e;
    struct pocketsys_cpu cpu;
    char cmd[800];

    if (!mkdtemp(root) || !mkdtemp(run)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETSYS_ROOT", root, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    fake_root();

    /* ---- system.info ---- */
    info = pocketsys_info("0.0.7", "abc1234");
    check("info version", str_is(info, "version", "0.0.7"));
    check("info build", str_is(info, "build", "abc1234"));
    check("info release_file is what the card carries", str_is(info, "release_file", "0.0.6"));
    check("info model from the device tree (NUL-terminated)",
          str_is(info, "model", "Canaan CanMV-K230 with RM69A10 OLED"));
    check("info kernel present", cJSON_IsString(get(info, "kernel")));
    check("info hostname present", cJSON_IsString(get(info, "hostname")));
    check("info cpus is a positive number",
          cJSON_IsNumber(get(info, "cpus")) && get(info, "cpus")->valuedouble >= 1);
    check("info vendor_sdk is the sdk: line", str_is(info, "vendor_sdk", "v1.2-20260909-22d02c6"));
    check("info os drops the quotes", str_is(info, "os", "Buildroot 2025.02.1"));
    cJSON_Delete(info);

    info = pocketsys_info(NULL, NULL);
    check("info NULL version says unknown", str_is(info, "version", "unknown"));
    cJSON_Delete(info);

    /* ---- system.status, everything present ---- */
    pocketsys_cpu_init(&cpu);
    check("first cpu sample reads", pocketsys_cpu_sample(&cpu) == 0);
    check("one sample gives no percent", cpu.have_percent == 0);
    st = pocketsys_status(&cpu);
    check("status cpu_percent null before two samples", cJSON_IsNull(get(st, "cpu_percent")));
    cJSON_Delete(st);
    /* busy 200/1000 -> 400/1500: 200 busy of 500 elapsed = 40 % */
    put("proc/stat", "cpu  200 0 200 1100 0 0 0 0 0 0\n", 0);
    check("second cpu sample reads", pocketsys_cpu_sample(&cpu) == 0);
    check("cpu percent is the busy share of the interval",
          cpu.have_percent && cpu.percent > 39.999 && cpu.percent < 40.001);
    /* a sample with no elapsed time keeps the last percent */
    check("unchanged stat keeps the percent", pocketsys_cpu_sample(&cpu) == 0 && cpu.have_percent);

    st = pocketsys_status(&cpu);
    check("status uptime_s truncated to seconds", num_is(st, "uptime_s", 5125));
    arr = get(st, "load");
    check("status load has three values", cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3 &&
                                              cJSON_GetArrayItem(arr, 0)->valuedouble > 0.519);
    check("status cpu_percent 40", num_is(st, "cpu_percent", 40.0));
    e = get(st, "memory");
    check("status memory total", num_is(e, "total_kb", 990544));
    check("status memory available", num_is(e, "available_kb", 928932));
    check("status temperature in degrees C", num_is(st, "temperature_c", 49.76));
    check("status clock_set is a bool", cJSON_IsBool(get(st, "clock_set")));

    arr = get(st, "storage");
    check("storage lists / and /boot from /proc/mounts, nothing else",
          cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 2 &&
              str_is(cJSON_GetArrayItem(arr, 0), "mount", "/") &&
              str_is(cJSON_GetArrayItem(arr, 1), "mount", "/boot"));
    e = cJSON_GetArrayItem(arr, 0);
    check("storage total_bytes positive", cJSON_IsNumber(get(e, "total_bytes")) &&
                                              get(e, "total_bytes")->valuedouble > 0);
    check("storage avail_bytes present", cJSON_IsNumber(get(e, "avail_bytes")));

    arr = get(st, "network");
    check("network skips lo and is sorted", cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3 &&
                                                 str_is(cJSON_GetArrayItem(arr, 0), "name", "eth0") &&
                                                 str_is(cJSON_GetArrayItem(arr, 1), "name", "nosuch0") &&
                                                 str_is(cJSON_GetArrayItem(arr, 2), "name", "wlan0"));
    e = find_named(arr, "eth0");
    check("eth0 operstate up", str_is(e, "operstate", "up"));
    check("eth0 carrier true", cJSON_IsTrue(get(e, "carrier")));
    check("eth0 mac", str_is(e, "mac", "00:e0:4c:3a:5e:d0"));
    /* ipv4 always comes from the live kernel (the host may well have an
     * eth0), so the null is checked on a name no kernel has */
    e = find_named(arr, "nosuch0");
    check("ipv4 null for an interface the kernel does not have", cJSON_IsNull(get(e, "ipv4")));
    check("absent address file is null", cJSON_IsNull(get(e, "mac")));
    e = find_named(arr, "wlan0");
    check("wlan0 carrier unknown (null) while down", cJSON_IsNull(get(e, "carrier")));

    e = get(st, "power");
    check("power source external with no supply", str_is(e, "source", "external"));
    check("power supplies empty", cJSON_IsArray(get(e, "supplies")) &&
                                      cJSON_GetArraySize(get(e, "supplies")) == 0);

    arr = get(st, "services");
    check("services lists pid and crashloop names once each, sorted",
          cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3 &&
              str_is(cJSON_GetArrayItem(arr, 0), "name", "pocketos-shell") &&
              str_is(cJSON_GetArrayItem(arr, 1), "name", "radiod") &&
              str_is(cJSON_GetArrayItem(arr, 2), "name", "stale"));
    e = find_named(arr, "radiod");
    check("radiod pid is ours", num_is(e, "pid", (double)getpid()));
    check("radiod running", cJSON_IsTrue(get(e, "running")));
    check("radiod not in a crash loop", cJSON_IsFalse(get(e, "crashloop")));
    e = find_named(arr, "stale");
    check("stale pid file: not running", cJSON_IsFalse(get(e, "running")));
    e = find_named(arr, "pocketos-shell");
    check("crash-looped service has no pid", cJSON_IsNull(get(e, "pid")));
    check("crash-looped service not running", cJSON_IsFalse(get(e, "running")));
    check("crashloop flag", cJSON_IsTrue(get(e, "crashloop")));
    check("crashloop last_exit_code parsed", num_is(e, "last_exit_code", 139));
    check("crashloop restarts parsed", num_is(e, "restarts", 6));
    cJSON_Delete(st);

    /* ---- a base-board battery makes the source unknown, not a guess ---- */
    mkdirs("sys/class/power_supply/bq27220-0");
    put("sys/class/power_supply/bq27220-0/type", "Battery\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("power source unknown with a Battery supply", str_is(e, "source", "unknown"));
    check("power supply listed with its type",
          str_is(find_named(get(e, "supplies"), "bq27220-0"), "type", "Battery"));
    check("status without a sampler has cpu_percent null", cJSON_IsNull(get(st, "cpu_percent")));
    cJSON_Delete(st);

    /* ---- absences are null, never invented ---- */
    rm("sys/class/thermal/thermal_zone0/temp");
    rm("etc/pocketos-release");
    rm("proc/device-tree/model");
    rm("proc/uptime");
    rm("proc/loadavg");
    rm("proc/meminfo");
    rm("proc/mounts");
    rm("etc/version/release_version");
    rm("etc/os-release");
    info = pocketsys_info("x", "y");
    check("absent release file is null", cJSON_IsNull(get(info, "release_file")));
    check("absent model is null", cJSON_IsNull(get(info, "model")));
    check("absent vendor sdk is null", cJSON_IsNull(get(info, "vendor_sdk")));
    check("absent os-release is null", cJSON_IsNull(get(info, "os")));
    cJSON_Delete(info);
    st = pocketsys_status(NULL);
    check("absent thermal zone is null", cJSON_IsNull(get(st, "temperature_c")));
    check("absent uptime is null", cJSON_IsNull(get(st, "uptime_s")));
    check("absent loadavg is null", cJSON_IsNull(get(st, "load")));
    check("absent meminfo is null", cJSON_IsNull(get(st, "memory")));
    check("absent mounts gives an empty storage list",
          cJSON_IsArray(get(st, "storage")) && cJSON_GetArraySize(get(st, "storage")) == 0);
    cJSON_Delete(st);
    rm("proc/stat");
    check("absent /proc/stat fails the sample", pocketsys_cpu_sample(&cpu) < 0);
    check("failed sample keeps the last percent", cpu.have_percent);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", root, run);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("pocketsys_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
