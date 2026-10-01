/*
 * Keyboard light test (ui/shell/kbd_light.c) against a fake sysfs tree:
 * finding PWM4 by its device-tree node rather than by a chip number, the
 * vendor's write sequence and its inverted duty, bounds, the stored value's
 * rules, and a PWM that refuses a write.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "hw_actions.h"
#include "kbd_light.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;
static char root[256];

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void run(const char *fmt)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), fmt, root, root, root);
    if (system(cmd) != 0) {
        fprintf(stderr, "setup failed: %s\n", cmd);
        exit(2);
    }
}

static void put(const char *rel, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static long get(const char *rel)
{
    char path[512];
    char buf[64] = "";
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "r");
    if (!f) {
        return -999;
    }
    if (!fgets(buf, sizeof(buf), f)) {
        buf[0] = '\0';
    }
    fclose(f);
    if (strncmp(buf, "inversed", 8) == 0) {
        return -2;
    }
    return strtol(buf, NULL, 10);
}

#define PWM "sys/class/pwm/pwmchip3/pwm1/"

int main(void)
{
    struct kbd_light l;
    char sys[300];
    int v;

    snprintf(root, sizeof(root), "/tmp/kbd_light_test.%d", (int)getpid());
    run("rm -rf %s; mkdir -p %s/sys/class/pwm/pwmchip0/device/of_node %s/sys/class/pwm/pwmchip3/device/of_node");
    /* pwmchip0 is pwm0_2 (the display's neighbours), pwmchip3 is pwm3_5:
     * the order in which readdir lists them must not matter. of_node/name
     * has no newline on a real board. */
    put("sys/class/pwm/pwmchip0/device/of_node/name", "pwm0_2");
    put("sys/class/pwm/pwmchip3/device/of_node/name", "pwm3_5");
    put("sys/class/pwm/pwmchip3/export", "");
    snprintf(sys, sizeof(sys), "%s/sys", root);

    check("PWM4 found by its node, pwm3_5", kbd_light_probe(&l, sys) == 0 && l.supported);
    check("channel 1 of that chip", strstr(l.dir, "pwmchip3/pwm1") != NULL);
    check("nothing applied yet", l.percent == -1);

    /* The channel is not exported: the export write is how it appears. A
     * real kernel creates the directory; the fake does it by hand. */
    check("unexported: a set fails rather than pretending", kbd_light_set(&l, 50) == -1);
    check("and it asked for channel 1", get("sys/class/pwm/pwmchip3/export") == 1);
    run("mkdir -p %s/" PWM "%.0s%.0s");
    put(PWM "enable", "0");
    put(PWM "period", "0");
    put(PWM "duty_cycle", "0");
    put(PWM "polarity", "normal");

    check("50%", kbd_light_set(&l, 50) == 50);
    check("period 20000 ns", get(PWM "period") == KBD_LIGHT_PERIOD_NS);
    check("inverted polarity", get(PWM "polarity") == -2);
    check("duty = period * (100 - 50) / 100", get(PWM "duty_cycle") == 10000);
    check("enabled", get(PWM "enable") == 1);
    check("remembered", l.percent == 50);

    check("100%: duty 0 (full light)", kbd_light_set(&l, 100) == 100 && get(PWM "duty_cycle") == 0);
    check("0%: duty is the whole period (dark)", kbd_light_set(&l, 0) == 0 &&
                                                     get(PWM "duty_cycle") == KBD_LIGHT_PERIOD_NS);
    check("above 100 is 100", kbd_light_set(&l, 250) == 100);
    check("below 0 is 0", kbd_light_set(&l, -5) == 0);
    check("off the grid rounds to it", kbd_light_set(&l, 44) == 40 && get(PWM "duty_cycle") == 12000);

    /* The stored value. */
    check("stored 0", kbd_light_parse("0", &v) == 0 && v == 0);
    check("stored 70", kbd_light_parse("70", &v) == 0 && v == 70);
    check("stored 100", kbd_light_parse("100", &v) == 0 && v == 100);
    check("75 is off the grid", kbd_light_parse("75", &v) != 0);
    check("110 is out of range", kbd_light_parse("110", &v) != 0);
    check("-10 is out of range", kbd_light_parse("-10", &v) != 0);
    check("junk is refused", kbd_light_parse("5x", &v) != 0 && kbd_light_parse("", &v) != 0 &&
                                 kbd_light_parse(NULL, &v) != 0);

    /* A write the driver refuses: a read-only attribute stands in for it. */
    kbd_light_set(&l, 30);
    run("chmod a-w %s/" PWM "polarity%.0s%.0s");
    if (geteuid() != 0) {
        check("a refused write fails the set", kbd_light_set(&l, 60) == -1);
        check("and the level stays what was applied", l.percent == 30);
    } else {
        printf("note: running as root, the read-only attribute cannot be refused\n");
    }
    run("chmod u+w %s/" PWM "polarity%.0s%.0s");

    /* Boards without it. */
    run("rm -rf %s/sys/class/pwm/pwmchip3%.0s%.0s");
    check("no pwm3_5: unsupported", kbd_light_probe(&l, sys) != 0 && !l.supported);
    check("unsupported: set refuses", kbd_light_set(&l, 50) == -1);
    run("rm -rf %s/sys/class/pwm%.0s%.0s");
    check("no pwm class at all: unsupported", kbd_light_probe(&l, sys) != 0);

    run("rm -rf %s%.0s%.0s");
    printf("kbd_light_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
