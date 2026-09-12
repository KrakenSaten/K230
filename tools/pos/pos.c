/*
 * pos - PocketOS command-line tool.
 *
 * Read-only system, hardware and network inspection for developers.
 * No dependencies beyond libc; reads /proc and /sys only.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef POCKETOS_VERSION
#define POCKETOS_VERSION "unknown"
#endif
#ifndef POCKETOS_BUILD_ID
#define POCKETOS_BUILD_ID "unknown"
#endif

/* Read the first line of a file into buf and strip the newline. Device-tree
 * strings are NUL-terminated, so fread plus explicit termination is used
 * instead of fgets. Returns 0 on success. */
static int read_first_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
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

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long value = -1;
    size_t klen = strlen(key);

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            value = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return value;
}

static void print_file_value(const char *label, const char *path)
{
    char buf[256];

    if (read_first_line(path, buf, sizeof(buf)) == 0) {
        printf("%-16s %s\n", label, buf);
    }
}

static int cmd_version(void)
{
    printf("pos %s (build %s)\n", POCKETOS_VERSION, POCKETOS_BUILD_ID);
    return 0;
}

static void print_vendor_sdk_version(void)
{
    /* Vendor SDK version file: a banner line, then "sdk:..." and "CONF:...". */
    FILE *f = fopen("/etc/version/release_version", "r");
    char buf[256];

    if (!f) {
        return;
    }
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "sdk:", 4) == 0 || strncmp(buf, "CONF:", 5) == 0) {
            buf[strcspn(buf, "\n")] = '\0';
            printf("%-16s %s\n", "vendor-sdk", buf);
        }
    }
    fclose(f);
}

/* /etc/pocketos-release, printed deliberately rather than dumped: line 1 is
 * the bare version and stays that way so first-line readers keep working, and
 * the build identity follows as a "BUILD_ID=<id>" line (Makefile install
 * target). A card flashed before v0.0.7 carries the version line only and is
 * printed without a build. sysd serves the same two facts as system.info's
 * release_file and release_build; this command reads the file directly so it
 * still answers when sysd is not running. */
static void print_release_file(void)
{
    FILE *f = fopen("/etc/pocketos-release", "r");
    char line[256];
    char version[256];
    char build[256];
    int have_version = 0;

    if (!f) {
        return;
    }
    version[0] = '\0';
    build[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!have_version) {
            snprintf(version, sizeof(version), "%s", line);
            have_version = 1;
        } else if (strncmp(line, "BUILD_ID=", 9) == 0) {
            snprintf(build, sizeof(build), "%s", line + 9);
        }
    }
    fclose(f);
    if (version[0] == '\0') {
        return;
    }
    if (build[0] != '\0') {
        printf("%-16s %s (build %s)\n", "pocketos", version, build);
    } else {
        printf("%-16s %s\n", "pocketos", version);
    }
}

static int cmd_system_info(void)
{
    struct utsname u;
    struct sysinfo si;

    print_file_value("model", "/proc/device-tree/model");
    if (uname(&u) == 0) {
        printf("%-16s %s %s (%s)\n", "kernel", u.sysname, u.release, u.machine);
        printf("%-16s %s\n", "hostname", u.nodename);
    }
    printf("%-16s %ld\n", "cpus", sysconf(_SC_NPROCESSORS_ONLN));
    if (sysinfo(&si) == 0) {
        printf("%-16s %ld s\n", "uptime", si.uptime);
        printf("%-16s %.2f %.2f %.2f\n", "load",
               si.loads[0] / 65536.0, si.loads[1] / 65536.0,
               si.loads[2] / 65536.0);
    }
    printf("%-16s total %ld kB, available %ld kB, free %ld kB\n", "memory",
           meminfo_kb("MemTotal"), meminfo_kb("MemAvailable"),
           meminfo_kb("MemFree"));
    printf("%-16s total %ld kB, free %ld kB\n", "swap",
           meminfo_kb("SwapTotal"), meminfo_kb("SwapFree"));
    print_release_file();
    print_vendor_sdk_version();
    return 0;
}

static void list_glob(const char *label, const char *pattern)
{
    glob_t g;
    size_t i;

    if (glob(pattern, 0, NULL, &g) != 0) {
        printf("%-16s (none)\n", label);
        return;
    }
    printf("%-16s", label);
    for (i = 0; i < g.gl_pathc; i++) {
        printf(" %s", g.gl_pathv[i]);
    }
    printf("\n");
    globfree(&g);
}

static void list_sysfs_names(const char *label, const char *dir,
                             const char *name_file)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[512];
    char name[128];

    if (!d) {
        printf("%-16s (none)\n", label);
        return;
    }
    printf("%s\n", label);
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/%s", dir, e->d_name, name_file);
        if (read_first_line(path, name, sizeof(name)) != 0) {
            name[0] = '\0';
        }
        printf("  %-14s %s\n", e->d_name, name);
    }
    closedir(d);
}

static int cmd_hardware_list(void)
{
    list_glob("spi", "/dev/spidev*");
    list_glob("gpio", "/dev/gpiochip*");
    list_glob("i2c", "/dev/i2c-*");
    list_glob("serial", "/dev/ttyS*");
    list_glob("usb-serial", "/dev/tty[AU][CS][MB]*");
    list_glob("video", "/dev/video*");
    list_glob("drm", "/dev/dri/card*");
    list_glob("mmc", "/dev/mmcblk*");
    list_glob("rtc", "/dev/rtc*");
    list_sysfs_names("input", "/sys/class/input", "device/name");
    list_sysfs_names("net", "/sys/class/net", "operstate");
    list_sysfs_names("backlight", "/sys/class/backlight", "brightness");
    list_sysfs_names("thermal", "/sys/class/thermal", "type");
    list_sysfs_names("power-supply", "/sys/class/power_supply", "type");
    list_sysfs_names("sdio", "/sys/bus/sdio/devices", "device");
    list_sysfs_names("usb", "/sys/bus/usb/devices", "product");
    return 0;
}

static int cmd_network_interfaces(void)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    int sock;
    char path[512];
    char state[32];
    char mac[32];

    if (!d) {
        fprintf(stderr, "cannot open /sys/class/net: %s\n", strerror(errno));
        return 1;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    printf("%-10s %-8s %-18s %s\n", "iface", "state", "mac", "ipv4");
    while ((e = readdir(d)) != NULL) {
        struct ifreq ifr;
        const char *ip = "-";
        char ipbuf[INET_ADDRSTRLEN];

        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", e->d_name);
        if (read_first_line(path, state, sizeof(state)) != 0) {
            strcpy(state, "?");
        }
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", e->d_name);
        if (read_first_line(path, mac, sizeof(mac)) != 0) {
            strcpy(mac, "?");
        }
        if (sock >= 0 && strlen(e->d_name) < IFNAMSIZ) {
            memset(&ifr, 0, sizeof(ifr));
            memcpy(ifr.ifr_name, e->d_name, strlen(e->d_name) + 1);
            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
                ip = inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
            }
        }
        printf("%-10s %-8s %-18s %s\n", e->d_name, state, mac, ip);
    }
    if (sock >= 0) {
        close(sock);
    }
    closedir(d);
    return 0;
}

int cmd_radio(int argc, char **argv); /* tools/pos/pos_radio.c */
int cmd_logs(int argc, char **argv);  /* tools/pos/pos_logs.c */
int cmd_app(int argc, char **argv);   /* tools/pos/pos_app.c */
int cmd_shell(int argc, char **argv); /* tools/pos/pos_app.c */
int cmd_system_status(void);          /* tools/pos/pos_system.c */
int cmd_call(int argc, char **argv);  /* tools/pos/pos_system.c */
int cmd_wifi(int argc, char **argv);  /* tools/pos/pos_wifi.c */

static int usage(int rc)
{
    fprintf(rc ? stderr : stdout,
            "usage: pos <command> [subcommand]\n"
            "  system info           kernel, memory, uptime, versions (read locally)\n"
            "  system status         the live view from sysd (system.status)\n"
            "  hardware list         device nodes and sysfs devices\n"
            "  network interfaces    interface state, MAC and IPv4\n"
            "  radio <command>       talk to radiod (pos radio help)\n"
            "  wifi <command>        Wi-Fi through netd (pos wifi help)\n"
            "  call <svc> <method>   any pocketipc method, key=value params\n"
            "  logs [name] [-n N]    service logs and crash reports\n"
            "  app list|start|home   drive the shell launcher\n"
            "  shell info|screenshot shell state and PNG capture\n"
            "  version               print pos version\n");
    return rc;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "help";
    const char *sub = argc > 2 ? argv[2] : "";

    if (strcmp(cmd, "version") == 0) {
        return cmd_version();
    }
    if (strcmp(cmd, "radio") == 0) {
        return cmd_radio(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "wifi") == 0) {
        return cmd_wifi(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "logs") == 0) {
        return cmd_logs(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "app") == 0) {
        return cmd_app(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "shell") == 0) {
        return cmd_shell(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "system") == 0 && strcmp(sub, "info") == 0) {
        return cmd_system_info();
    }
    if (strcmp(cmd, "system") == 0 && strcmp(sub, "status") == 0) {
        return cmd_system_status();
    }
    if (strcmp(cmd, "call") == 0) {
        return cmd_call(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "hardware") == 0 && strcmp(sub, "list") == 0) {
        return cmd_hardware_list();
    }
    if (strcmp(cmd, "network") == 0 && strcmp(sub, "interfaces") == 0) {
        return cmd_network_interfaces();
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        return usage(0);
    }
    return usage(2);
}
