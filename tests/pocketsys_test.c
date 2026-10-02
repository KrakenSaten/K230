/*
 * pocketsys tests against a fake root ($POCKETSYS_ROOT): every field, every
 * absence, and the CPU sampler's arithmetic.
 *
 * system.status.services is not here: it comes from the supervisor's state
 * file, which sysd reads (tests/sysd_services_test.c), not core.
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

/* The K230's own numbers (VERIFIED unit A), used twice: once whole and once
 * cut down to a kernel that does not carry MemAvailable. */
static const char meminfo_full[] =
    "MemTotal:         990544 kB\nMemFree:          900952 kB\nMemAvailable:     928932 kB\n"
    "Buffers:            1848 kB\n";

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

/* A symlink below the fake root pointing at target, as given. */
static void ln_s(const char *target, const char *rel)
{
    char path[700];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    unlink(path);
    if (symlink(target, path) != 0) {
        fprintf(stderr, "symlink %s failed: %s\n", rel, strerror(errno));
        exit(1);
    }
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
    mkdirs("proc/device-tree");
    mkdirs("etc/version");
    mkdirs("sys/class/thermal/thermal_zone0");
    mkdirs("sys/class/net/eth0/statistics");
    mkdirs("sys/class/net/wlan0/statistics");
    mkdirs("sys/class/net/nosuch0"); /* a name no host kernel has: ipv4 must be null */
    mkdirs("sys/class/net/lo");
    mkdirs("sys/class/power_supply");
    mkdirs("boot");
    put("proc/device-tree/model", "Canaan CanMV-K230 with RM69A10 OLED\0", 36);
    put("proc/uptime", "5125.90 4959.28\n", 0);
    put("proc/loadavg", "0.52 0.30 0.21 1/95 1234\n", 0);
    put("proc/meminfo", meminfo_full, 0);
    put("proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 100 0 100 800 0 0 0 0 0 0\n", 0);
    put("proc/mounts",
        "/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw 0 0\n"
        "tmpfs /run tmpfs rw 0 0\n/dev/mmcblk1p1 /boot ext4 rw,relatime 0 0\n", 0);
    /* what a card flashed from `make install` before Doors carries, under the
     * old name only: the bare version on line 1 for first-line readers, the
     * build identity below it */
    put("etc/pocketos-release", "0.0.6\nBUILD_ID=deadbee\n", 0);
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
    put("sys/class/net/eth0/statistics/rx_bytes", "23681229\n", 0);
    put("sys/class/net/eth0/statistics/tx_bytes", "105768542\n", 0);
    put("sys/class/net/wlan0/statistics/rx_bytes", "18289868\n", 0);
    put("sys/class/net/wlan0/statistics/tx_bytes", "x269402\n", 0);
    put("sys/class/net/nosuch0/operstate", "down\n", 0);
    put("sys/class/net/lo/operstate", "unknown\n", 0);
}

int main(void)
{
    cJSON *info;
    cJSON *st;
    const cJSON *arr;
    const cJSON *e;
    struct pocketsys_cpu cpu;
    char cmd[800];

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETSYS_ROOT", root, 1);
    fake_root();

    /* ---- system.info ---- */
    info = pocketsys_info("0.0.7", "abc1234");
    check("info version", str_is(info, "version", "0.0.7"));
    check("info build", str_is(info, "build", "abc1234"));
    check("info release_file is what the card carries", str_is(info, "release_file", "0.0.6"));
    check("info release_build is the BUILD_ID line", str_is(info, "release_build", "deadbee"));
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

    /* a card flashed before v0.0.7 carries the version line only */
    put("etc/pocketos-release", "0.0.5\n", 0);
    info = pocketsys_info("0.0.7", "abc1234");
    check("a release file without a BUILD_ID line still gives release_file",
          str_is(info, "release_file", "0.0.5"));
    check("a release file without a BUILD_ID line gives a null release_build",
          cJSON_IsNull(get(info, "release_build")));
    cJSON_Delete(info);
    put("etc/pocketos-release", "0.0.6\nBUILD_ID=deadbee\n", 0);

    /* ---- the Doors release file (ADR-005 Phase 2) ---- */
    /* the image's layout: one file under the new name, the old name a symlink */
    put("etc/doors-release", "0.0.10\nBUILD_ID=d00r5aa\n", 0);
    ln_s("doors-release", "etc/pocketos-release");
    info = pocketsys_info("0.0.7", "abc1234");
    check("new file plus compatibility symlink: release_file is the new file's",
          str_is(info, "release_file", "0.0.10"));
    check("new file plus compatibility symlink: release_build is the new file's",
          str_is(info, "release_build", "d00r5aa"));
    cJSON_Delete(info);

    rm("etc/pocketos-release");
    info = pocketsys_info("0.0.7", "abc1234");
    check("only /etc/doors-release: release_file comes from it", str_is(info, "release_file", "0.0.10"));
    check("only /etc/doors-release: release_build comes from it",
          str_is(info, "release_build", "d00r5aa"));
    cJSON_Delete(info);

    /* two independent files (e.g. a unit rolled back by an old deploy): the
     * new name is preferred, and both fields come from that one file */
    put("etc/pocketos-release", "0.0.9\nBUILD_ID=0ld0ld0\n", 0);
    put("etc/doors-release", "0.0.10\n", 0);
    info = pocketsys_info("0.0.7", "abc1234");
    check("a separate old file does not win over the new one", str_is(info, "release_file", "0.0.10"));
    check("release_build is not borrowed from the other file", cJSON_IsNull(get(info, "release_build")));
    cJSON_Delete(info);

    /* a new name that points nowhere is absent, not a reason for null */
    ln_s("nowhere", "etc/doors-release");
    info = pocketsys_info("0.0.7", "abc1234");
    check("a dangling /etc/doors-release falls back to the old file",
          str_is(info, "release_file", "0.0.9") && str_is(info, "release_build", "0ld0ld0"));
    cJSON_Delete(info);

    rm("etc/doors-release");
    put("etc/pocketos-release", "0.0.6\nBUILD_ID=deadbee\n", 0);

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
    check("wlan0 rx_bytes from its statistics counter",
          cJSON_IsNumber(get(e, "rx_bytes")) && get(e, "rx_bytes")->valuedouble == 18289868.0);
    check("a counter that is not a plain decimal is null, never a guess", cJSON_IsNull(get(e, "tx_bytes")));
    e = find_named(arr, "eth0");
    check("eth0 rx_bytes and tx_bytes", cJSON_IsNumber(get(e, "rx_bytes")) &&
                                            get(e, "rx_bytes")->valuedouble == 23681229.0 &&
                                            cJSON_IsNumber(get(e, "tx_bytes")) &&
                                            get(e, "tx_bytes")->valuedouble == 105768542.0);
    e = find_named(arr, "nosuch0");
    check("no statistics directory: both counters null",
          cJSON_IsNull(get(e, "rx_bytes")) && cJSON_IsNull(get(e, "tx_bytes")));

    e = get(st, "power");
    check("power source external with no supply", str_is(e, "source", "external"));
    check("power supplies empty", cJSON_IsArray(get(e, "supplies")) &&
                                      cJSON_GetArraySize(get(e, "supplies")) == 0);

    check("status carries no services key: that is sysd's, not core's",
          get(st, "services") == NULL);
    cJSON_Delete(st);

    /* ---- a mount point listed twice is one row ---- */
    put("proc/mounts",
        "rootfs / rootfs rw 0 0\n/dev/root / ext4 rw,relatime 0 0\nproc /proc proc rw 0 0\n"
        "/dev/mmcblk1p1 /boot ext4 rw,relatime 0 0\n", 0);
    st = pocketsys_status(NULL);
    arr = get(st, "storage");
    check("a mount point on two lines of /proc/mounts is one row",
          cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 2 &&
              str_is(cJSON_GetArrayItem(arr, 0), "mount", "/") &&
              str_is(cJSON_GetArrayItem(arr, 1), "mount", "/boot"));
    cJSON_Delete(st);

    /* ---- a kernel without MemAvailable: null, never -1 ---- */
    put("proc/meminfo", "MemTotal:         990544 kB\nBuffers:            1848 kB\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "memory");
    check("memory is still reported when only MemTotal is there",
          num_is(e, "total_kb", 990544));
    check("absent MemAvailable is null, not -1", cJSON_IsNull(get(e, "available_kb")));
    check("absent MemFree is null, not -1", cJSON_IsNull(get(e, "free_kb")));
    cJSON_Delete(st);
    put("proc/meminfo", meminfo_full, 0);

    /* ---- a base-board battery makes the source unknown, not a guess ---- */
    mkdirs("sys/class/power_supply/bq27220-0");
    put("sys/class/power_supply/bq27220-0/type", "Battery\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("power source unknown with a Battery supply", str_is(e, "source", "unknown"));
    check("power supply listed with its type",
          str_is(find_named(get(e, "supplies"), "bq27220-0"), "type", "Battery"));
    check("status without a sampler has cpu_percent null", cJSON_IsNull(get(st, "cpu_percent")));
    check("a battery that reports nothing: capacity and status null",
          cJSON_IsObject(get(e, "battery")) && cJSON_IsNull(get(get(e, "battery"), "capacity_percent")) &&
              cJSON_IsNull(get(get(e, "battery"), "status")) &&
              cJSON_IsNull(get(get(e, "battery"), "voltage_v")));
    check("a battery without `present` is present", cJSON_IsTrue(get(get(e, "battery"), "present")));
    check("no external supply listed: external_online null", cJSON_IsNull(get(e, "external_online")));
    cJSON_Delete(st);

    /* ---- battery: discharging on its own ---- */
    put("sys/class/power_supply/bq27220-0/capacity", "76\n", 0);
    put("sys/class/power_supply/bq27220-0/status", "Discharging\n", 0);
    put("sys/class/power_supply/bq27220-0/voltage_now", "3912000\n", 0);
    put("sys/class/power_supply/bq27220-0/present", "1\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("discharging: the gauge's percentage", num_is(get(e, "battery"), "capacity_percent", 76));
    check("discharging: status", str_is(get(e, "battery"), "status", "discharging"));
    check("discharging: voltage in volts", num_is(get(e, "battery"), "voltage_v", 3.912));
    check("discharging with no external supply listed: on battery", str_is(e, "source", "battery"));
    cJSON_Delete(st);

    /* ---- battery charging from USB ---- */
    mkdirs("sys/class/power_supply/bq25896-charger");
    put("sys/class/power_supply/bq25896-charger/type", "USB\n", 0);
    put("sys/class/power_supply/bq25896-charger/online", "1\n", 0);
    put("sys/class/power_supply/bq27220-0/status", "Charging\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("charging: status", str_is(get(e, "battery"), "status", "charging"));
    check("charging: USB online is external", str_is(e, "source", "external") &&
                                                   cJSON_IsTrue(get(e, "external_online")));
    check("the charger is listed online",
          cJSON_IsTrue(get(find_named(get(e, "supplies"), "bq25896-charger"), "online")));
    cJSON_Delete(st);

    /* ---- full, not charging, and the charger unplugged ---- */
    put("sys/class/power_supply/bq27220-0/status", "Full\n", 0);
    put("sys/class/power_supply/bq27220-0/capacity", "100\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("full: status and 100 %", str_is(get(e, "battery"), "status", "full") &&
                                        num_is(get(e, "battery"), "capacity_percent", 100));
    cJSON_Delete(st);
    put("sys/class/power_supply/bq27220-0/status", "Not charging\n", 0);
    put("sys/class/power_supply/bq25896-charger/online", "0\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("not charging: its own word", str_is(get(e, "battery"), "status", "not_charging"));
    check("every external supply offline: on battery", str_is(e, "source", "battery") &&
                                                           cJSON_IsFalse(get(e, "external_online")));
    cJSON_Delete(st);

    /* ---- values a driver should never give are not passed on ---- */
    put("sys/class/power_supply/bq27220-0/capacity", "140\n", 0);
    put("sys/class/power_supply/bq27220-0/status", "Exploding\n", 0);
    put("sys/class/power_supply/bq27220-0/voltage_now", "-5\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("a percentage above 100 is null, not clamped",
          cJSON_IsNull(get(get(e, "battery"), "capacity_percent")));
    check("an unknown status word is null", cJSON_IsNull(get(get(e, "battery"), "status")));
    check("a negative voltage is null", cJSON_IsNull(get(get(e, "battery"), "voltage_v")));
    cJSON_Delete(st);
    put("sys/class/power_supply/bq27220-0/capacity", "-1\n", 0);
    put("sys/class/power_supply/bq27220-0/present", "0\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("a negative percentage is null", cJSON_IsNull(get(get(e, "battery"), "capacity_percent")));
    check("present 0: not present", cJSON_IsFalse(get(get(e, "battery"), "present")));
    cJSON_Delete(st);

    /* ---- USB power only, no battery ---- */
    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s/sys/class/power_supply/bq27220-0'", root);
        if (system(cmd) != 0) {
            fprintf(stderr, "rm failed\n");
            return 1;
        }
    }
    put("sys/class/power_supply/bq25896-charger/online", "1\n", 0);
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("USB only: no battery object", cJSON_IsNull(get(e, "battery")));
    check("USB only: external", str_is(e, "source", "external") && cJSON_IsTrue(get(e, "external_online")));
    cJSON_Delete(st);
    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s/sys/class/power_supply/bq25896-charger'", root);
        if (system(cmd) != 0) {
            fprintf(stderr, "rm failed\n");
            return 1;
        }
    }
    st = pocketsys_status(NULL);
    e = get(st, "power");
    check("no supply at all (unit A): external, no battery, external_online null",
          str_is(e, "source", "external") && cJSON_IsNull(get(e, "battery")) &&
              cJSON_IsNull(get(e, "external_online")));
    check("bluetooth: no class directory, no controllers",
          cJSON_GetArraySize(get(get(st, "bluetooth"), "controllers")) == 0);
    cJSON_Delete(st);

    /* ---- Bluetooth controllers ---- */
    mkdirs("sys/class/bluetooth/hci0");
    mkdirs("sys/class/bluetooth/hci0:3");
    st = pocketsys_status(NULL);
    e = get(get(st, "bluetooth"), "controllers");
    check("a controller is listed, its connection is not",
          cJSON_GetArraySize(e) == 1 && strcmp(cJSON_GetArrayItem(e, 0)->valuestring, "hci0") == 0);
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
    check("absent release file gives a null release_build",
          cJSON_IsNull(get(info, "release_build")));
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

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("pocketsys_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
