/*
 * The debug overlay's line (ui/shell/overlay_model.h): what it says from
 * scripted system.status and radio.stats answers, the network rate between
 * two answers, the cases where it must say nothing rather than a guess, and
 * that nothing but numbers ever reaches it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "overlay_model.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int has(const struct overlay_model *m, const char *s)
{
    if (!strstr(m->text, s)) {
        printf("     line: %s\n", m->text);
        return 0;
    }
    return 1;
}

/* A system.status with two real interfaces and the pseudo one sit0. */
static cJSON *status(double cpu, double rx_eth, double tx_eth, double rx_wlan, double tx_wlan)
{
    char buf[1024];

    snprintf(buf, sizeof(buf),
             "{\"cpu_percent\":%g,\"temperature_c\":51.4,"
             "\"memory\":{\"total_kb\":1000,\"available_kb\":580,\"free_kb\":100},"
             "\"network\":["
             "{\"name\":\"eth0\",\"mac\":\"00:e0:4c:3a:5e:d0\",\"ipv4\":\"192.168.10.157\",\"rx_bytes\":%.0f,\"tx_bytes\":%.0f},"
             "{\"name\":\"sit0\",\"mac\":\"00:00:00:00\",\"ipv4\":null,\"rx_bytes\":999999999,\"tx_bytes\":999999999},"
             "{\"name\":\"wlan0\",\"mac\":\"88:3b:dc:b7:9e:c7\",\"ipv4\":\"192.168.10.171\",\"rx_bytes\":%.0f,\"tx_bytes\":%.0f}"
             "]}",
             cpu, rx_eth, tx_eth, rx_wlan, tx_wlan);
    return cJSON_Parse(buf);
}

int main(void)
{
    struct overlay_model m;
    cJSON *st;
    cJSON *radio = cJSON_Parse("{\"rx_packets\":848,\"tx_packets\":1,\"last_rssi_dbm\":-74}");
    uint64_t rx = 0;
    uint64_t tx = 0;

    overlay_model_init(&m);
    check("before any answer every figure is a dash", has(&m, "CPU \xE2\x80\x94") && has(&m, "NET \xE2\x80\x94"));

    st = status(18, 1000000, 200000, 500000, 20000);
    check("net bytes: the real interfaces summed, sit0 left out",
          overlay_net_bytes(st, &rx, &tx) == 0 && rx == 1500000 && tx == 220000);
    overlay_model_update(&m, st, radio, 10000);
    cJSON_Delete(st);
    check("CPU 18%", has(&m, "CPU 18%"));
    check("RAM 42% (1000 kB, 580 available)", has(&m, "RAM 42%"));
    check("51\xC2\xB0" "C", has(&m, "51\xC2\xB0" "C"));
    check("one answer: no rate yet", has(&m, "NET \xE2\x80\x94"));
    check("LoRa packets received and sent", has(&m, "LORA \xE2\x86\x93" "848 \xE2\x86\x91" "1"));
    check("the parts are joined by a middle dot", has(&m, "CPU 18% \xC2\xB7 RAM 42%"));

    /* Two seconds later: 24576 + 4096 bytes down, 4096 up = 14.0 and 2.0 KB/s. */
    st = status(20, 1000000 + 24576, 200000 + 4096, 500000 + 4096, 20000);
    overlay_model_update(&m, st, radio, 12000);
    cJSON_Delete(st);
    check("the rate over the two answers: down 14.0 KB/s, up 2.0", has(&m, "NET \xE2\x86\x93" "14 \xE2\x86\x91" "2.0 KB/s"));
    check("no address, interface name or MAC reaches the line",
          !strstr(m.text, "192.168") && !strstr(m.text, "eth0") && !strstr(m.text, "wlan") && !strstr(m.text, ":5e:"));

    /* A busy second: 3 MiB down in one second. */
    st = status(90, 1000000 + 24576 + 3 * 1048576, 200000 + 4096, 504096, 20000);
    overlay_model_update(&m, st, NULL, 13000);
    cJSON_Delete(st);
    check("past a megabyte a second the unit is MB/s", has(&m, "NET \xE2\x86\x93" "3.0 \xE2\x86\x91" "0.0 MB/s"));
    check("radiod not answering: the radio part is left out, not zeroed", !strstr(m.text, "LORA"));

    /* A counter that went down (a driver reloaded) is a fresh start. */
    st = status(5, 10, 10, 10, 10);
    overlay_model_update(&m, st, NULL, 15000);
    cJSON_Delete(st);
    check("a counter going down shows a dash, never a wrapped figure", has(&m, "NET \xE2\x80\x94"));
    st = status(5, 10 + 1024, 10, 10, 10);
    overlay_model_update(&m, st, NULL, 16000);
    cJSON_Delete(st);
    check("and measures again from there", has(&m, "NET \xE2\x86\x93" "1.0 \xE2\x86\x91" "0.0 KB/s"));

    /* sysd not answering: the last count stands; the next answer measures
     * over the whole gap. */
    overlay_model_update(&m, NULL, NULL, 18000);
    check("sysd not answering: dashes, not zeros", has(&m, "CPU \xE2\x80\x94") && has(&m, "RAM \xE2\x80\x94"));
    st = status(5, 10 + 1024 + 4096, 10, 10, 10);
    overlay_model_update(&m, st, NULL, 20000);
    cJSON_Delete(st);
    check("the next answer measures over the whole gap (4 KB in 4 s)", has(&m, "NET \xE2\x86\x93" "1.0 "));

    /* An interface without counters makes the sum a guess: no rate. */
    st = cJSON_Parse("{\"cpu_percent\":1,\"network\":[{\"name\":\"eth0\",\"mac\":\"00:e0:4c:3a:5e:d0\","
                     "\"rx_bytes\":5,\"tx_bytes\":null}]}");
    check("an interface with a null counter: no sum", overlay_net_bytes(st, &rx, &tx) < 0);
    overlay_model_update(&m, st, NULL, 22000);
    overlay_model_update(&m, st, NULL, 24000);
    cJSON_Delete(st);
    check("and no rate from it", has(&m, "NET \xE2\x80\x94"));
    st = cJSON_Parse("{\"network\":[]}");
    check("no interfaces at all: no sum", overlay_net_bytes(st, &rx, &tx) < 0);
    cJSON_Delete(st);
    st = cJSON_Parse("{\"cpu_percent\":2,\"memory\":{\"total_kb\":0,\"available_kb\":0}}");
    overlay_model_update(&m, st, NULL, 26000);
    cJSON_Delete(st);
    check("a zero memory total is no share", has(&m, "RAM \xE2\x80\x94"));
    check("the line always fits its buffer", strlen(m.text) < sizeof(m.text));

    cJSON_Delete(radio);
    printf("overlay_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
