/*
 * Every word the Recorder screen shows (apps/recorder/rec_view.h): times,
 * sizes, the meter's decibels, the owner's version of each helper error, the
 * list rows, and what the chip, the timer, the status line and each button
 * say - and whether they can be pressed - in every state.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_protocol.h"
#include "rec_view.h"

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

static void test_format(void)
{
    char b[64];

    rec_view_duration(b, sizeof(b), 0);
    check("0:00", strcmp(b, "0:00") == 0);
    rec_view_duration(b, sizeof(b), 7999);
    check("7.999 s is 0:07", strcmp(b, "0:07") == 0);
    rec_view_duration(b, sizeof(b), 754000);
    check("12:34", strcmp(b, "12:34") == 0);
    rec_view_duration(b, sizeof(b), 3723000);
    check("1:02:03", strcmp(b, "1:02:03") == 0);
    rec_view_duration(b, sizeof(b), 22369000LL);
    check("the longest Standard recording, 6:12:49", strcmp(b, "6:12:49") == 0);
    rec_view_duration(b, sizeof(b), -5);
    check("a negative time is 0:00", strcmp(b, "0:00") == 0);

    rec_view_size(b, sizeof(b), 940);
    check("940 B", strcmp(b, "940 B") == 0);
    rec_view_size(b, sizeof(b), 12 * 1024);
    check("12 KB", strcmp(b, "12 KB") == 0);
    rec_view_size(b, sizeof(b), 5760044);
    check("a minute of Standard is 5.5 MB", strcmp(b, "5.5 MB") == 0);
    rec_view_size(b, sizeof(b), 0x7FFFFFFCull);
    check("the largest file is 2.0 GB", strcmp(b, "2.0 GB") == 0);

    check("silence is -60 dB and an empty meter", rec_view_level_db(0) == -60 && rec_view_level_pct(0) == 0);
    check("full scale is 0 dB and a full meter", rec_view_level_db(32767) == 0 && rec_view_level_pct(32767) == 100);
    check("half scale is -6 dB", rec_view_level_db(16384) == -6);
    check("-30 dB is half the meter", rec_view_level_pct(1036) == 50);
    check("below -60 dB is empty", rec_view_level_pct(30) == 0);
    check("an out-of-range reading is clamped", rec_view_level_pct(-5) == 0 && rec_view_level_pct(99999) == 100);

    rec_view_error_text("audio_busy another audio stream is open", b, sizeof(b));
    check("busy is 'Audio device in use', nothing more", strcmp(b, "Audio device in use") == 0);
    rec_view_error_text("storage_full not enough free storage to record", b, sizeof(b));
    check("full storage keeps the helper's reason",
          strcmp(b, "Storage is full: not enough free storage to record") == 0);
    rec_view_error_text("audio_nodev", b, sizeof(b));
    check("no device", strcmp(b, "No audio device found") == 0);
    rec_view_error_text("mystery thing", b, sizeof(b));
    check("an unknown code is shown as it came", strcmp(b, "Recorder error: mystery thing") == 0);
}

static void test_rows(void)
{
    struct rec_entry e;
    char t[300];
    char c[200];

    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "REC-20260926-101500.wav");
    e.status = REC_ENTRY_OK;
    e.ms = 42000;
    e.bytes = 1344044;
    e.rate = 16000;
    e.channels = 1;
    rec_view_row(&e, t, sizeof(t), c, sizeof(c));
    check("a row: its name", strcmp(t, "REC-20260926-101500.wav") == 0);
    check("and its length, size and rate", strcmp(c, "0:42 \xc2\xb7 1.3 MB \xc2\xb7 16 kHz") == 0);
    e.recovered = true;
    e.channels = 2;
    e.rate = 48000;
    rec_view_row(&e, t, sizeof(t), c, sizeof(c));
    check("a repaired stereo one says both", strstr(c, "48 kHz stereo") && strstr(c, "recovered"));
    e.status = REC_ENTRY_PART;
    rec_view_row(&e, t, sizeof(t), c, sizeof(c));
    check("an unfinished one says it will be repaired", strncmp(c, "Unfinished", 10) == 0);
    e.status = REC_ENTRY_UNREADABLE;
    rec_view_row(&e, t, sizeof(t), c, sizeof(c));
    check("a broken one says so", strncmp(c, "Not a readable WAV", 18) == 0);
}

static struct rec_ctl c;
static struct rec_view v;
static struct rec_entry entries[2];

static void at(enum rec_state s, enum rec_op op)
{
    memset(&c, 0, sizeof(c));
    c.m.state = s;
    c.m.op = op;
    c.free_bytes = -1;
    c.list.e = entries;
    c.list.n = 2;
    c.list.total = 2;
    memset(entries, 0, sizeof(entries));
    snprintf(entries[0].name, sizeof(entries[0].name), "REC-0002.wav");
    entries[0].status = REC_ENTRY_OK;
    entries[0].rate = 16000;
    snprintf(entries[1].name, sizeof(entries[1].name), "REC-0001.wav.part");
    entries[1].status = REC_ENTRY_PART;
}

static void test_states(void)
{
    at(REC_ST_IDLE, REC_OP_NONE);
    rec_view_refresh(&c, 1000, &v);
    check("idle: READY, RECORD enabled and primary", strcmp(v.chip, "READY") == 0 &&
                                                          strcmp(v.main_label, "RECORD") == 0 && v.main_enabled &&
                                                          v.main_primary);
    check("idle: nothing to pause, nothing selected to play or delete",
          !v.pause_enabled && !v.play_enabled && !v.delete_enabled);
    check("idle: the microphone is off, no header hint, an empty meter",
          !v.mic_on && v.hint == NULL && v.meter_pct == 0 && strcmp(v.meter_text, "-") == 0);
    check("idle: the preset can change", v.preset_enabled && strcmp(v.preset_label, "VOICE 16 kHz") == 0);
    check("the list caption counts", strcmp(v.list_caption, "RECORDINGS 2") == 0);
    snprintf(c.selected, sizeof(c.selected), "REC-0002.wav");
    rec_view_refresh(&c, 1000, &v);
    check("a playable selection can be played and deleted", v.play_enabled && v.delete_enabled);
    snprintf(c.selected, sizeof(c.selected), "REC-0001.wav.part");
    rec_view_refresh(&c, 1000, &v);
    check("an unfinished one can be deleted but not played", !v.play_enabled && v.delete_enabled);
    c.delete_armed_ms = 900;
    rec_view_refresh(&c, 1000, &v);
    check("an armed delete asks for confirmation", strcmp(v.delete_label, "CONFIRM") == 0);
    rec_view_refresh(&c, 900 + REC_DELETE_ARM_MS + 1, &v);
    check("and forgets it after the time", strcmp(v.delete_label, "DELETE") == 0);

    at(REC_ST_CHECKING, REC_OP_CHECK);
    rec_view_refresh(&c, 1000, &v);
    check("checking: RECORD waits", strcmp(v.chip, "CHECKING") == 0 && !v.main_enabled);

    at(REC_ST_STARTING, REC_OP_RECORD);
    rec_view_refresh(&c, 1000, &v);
    check("starting a recording: the button already says STOP, and the mic counts as on",
          strcmp(v.main_label, "STOP") == 0 && v.main_enabled && v.mic_on && strcmp(v.hint, "MIC ON") == 0);

    at(REC_ST_RECORDING, REC_OP_RECORD);
    snprintf(c.current, sizeof(c.current), "REC-0003.wav");
    c.rate = 16000;
    c.elapsed_ms = 65000;
    c.bytes = 2080044;
    c.level_peak = 8231;
    c.level_rms = 2000;
    c.level_at_ms = 950;
    c.peak_hold = 16422;
    c.peak_hold_at_ms = 500;
    c.free_bytes = (int64_t)REC_RESERVE_BYTES + 32000LL * 60 * 37;
    rec_view_refresh(&c, 1000, &v);
    check("recording: RECORDING, STOP, PAUSE, MIC ON", strcmp(v.chip, "RECORDING") == 0 &&
                                                            v.chip_kind == REC_CHIP_LIVE &&
                                                            strcmp(v.main_label, "STOP") == 0 &&
                                                            v.pause_enabled &&
                                                            strcmp(v.pause_label, "PAUSE") == 0 && v.mic_on);
    check("the timer is the audio written", strcmp(v.timer, "1:05") == 0);
    check("the status names the file, the rate and the size",
          strcmp(v.status, "REC-0003.wav \xc2\xb7 16 kHz \xc2\xb7 2.0 MB") == 0);
    check("the meter shows the reading in dB, with the peak held",
          v.meter_pct == 80 && v.hold_pct == 90 && strcmp(v.meter_text, "-12 dB") == 0);
    check("the free space in minutes at this preset", strcmp(v.space, "Room for about 37 min") == 0);
    check("nothing else can start: no preset change, no play, no delete",
          !v.preset_enabled && !v.play_enabled && !v.delete_enabled);
    rec_view_refresh(&c, 950 + REC_LEVEL_STALE_MS + 1, &v);
    check("a stale reading empties the meter and says so", v.meter_pct == 0 &&
                                                               strcmp(v.meter_text, "No reading") == 0);
    c.level_peak = 32000;
    rec_view_refresh(&c, 1000, &v);
    check("near full scale it says Too loud", strcmp(v.meter_text, "Too loud") == 0 && v.meter_tone == REC_TONE_WARN);
    c.level_peak = 0;
    rec_view_refresh(&c, 1000, &v);
    check("silence says Silence", strcmp(v.meter_text, "Silence") == 0 && v.meter_pct == 0);
    c.m.cmd_pending = true;
    rec_view_refresh(&c, 1000, &v);
    check("a pause already sent cannot be sent twice", !v.pause_enabled);
    snprintf(c.message, sizeof(c.message), "Storage is almost full: recording stopped");
    c.tone = REC_TONE_WARN;
    rec_view_refresh(&c, 1000, &v);
    check("a message replaces the status line, with its tone",
          strcmp(v.status, c.message) == 0 && v.status_tone == REC_TONE_WARN);

    at(REC_ST_PAUSED, REC_OP_RECORD);
    rec_view_refresh(&c, 1000, &v);
    check("paused: RESUME, STOP, the microphone off", strcmp(v.pause_label, "RESUME") == 0 &&
                                                          strcmp(v.main_label, "STOP") == 0 && !v.mic_on &&
                                                          v.hint == NULL);

    at(REC_ST_STOPPING, REC_OP_RECORD);
    rec_view_refresh(&c, 1000, &v);
    check("saving: SAVING, the big button waits", strcmp(v.chip, "SAVING") == 0 && !v.main_enabled);

    at(REC_ST_PLAYING, REC_OP_PLAY);
    snprintf(c.current, sizeof(c.current), "REC-0002.wav");
    snprintf(c.selected, sizeof(c.selected), "REC-0002.wav");
    c.elapsed_ms = 12000;
    c.total_ms = 63000;
    rec_view_refresh(&c, 1000, &v);
    check("playing: PLAYING, position of length, PLAY reads STOP",
          strcmp(v.chip, "PLAYING") == 0 && strcmp(v.timer, "0:12 / 1:03") == 0 &&
              strcmp(v.play_label, "STOP") == 0 && v.play_enabled && strcmp(v.hint, "PLAYING") == 0);
    check("RECORD stays available (it stops the playback first)", strcmp(v.main_label, "RECORD") == 0 &&
                                                                       v.main_enabled && !v.mic_on);
    snprintf(c.selected, sizeof(c.selected), "REC-0001.wav.part");
    rec_view_refresh(&c, 1000, &v);
    check("with another row selected PLAY plays that one", strcmp(v.play_label, "PLAY") == 0);

    at(REC_ST_ERROR, REC_OP_NONE);
    c.dir_error = 13;
    rec_view_refresh(&c, 1000, &v);
    check("a folder that cannot be used keeps RECORD off", !v.main_enabled);
    c.free_bytes = 1000;
    rec_view_refresh(&c, 1000, &v);
    check("under the reserve the space says full", strcmp(v.space, "Storage full") == 0);
    c.list.n = 1;
    c.list.total = 150;
    rec_view_refresh(&c, 1000, &v);
    check("a capped list says how many there are", strcmp(v.list_caption, "RECORDINGS 1 OF 150") == 0);
}

int main(void)
{
    test_format();
    test_rows();
    test_states();
    printf("rec_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
