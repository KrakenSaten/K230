/*
 * Video's state machine: choosing a file, the helper's answers, play, pause
 * and stop, rapid taps, seeks coalesced while one is answered, the progress
 * bar dragged, the end, every way an open can fail, a helper that errs,
 * hangs or crashes, fullscreen, BACK, and a helper's last words after BACK.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "video_state.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct video_event ev(enum video_ev_kind kind)
{
    struct video_event e;

    memset(&e, 0, sizeof(e));
    e.kind = kind;
    return e;
}

static unsigned opened(struct video_model *m, int64_t duration, const char *audio)
{
    struct video_event e = ev(VIDEO_EV_OPENED);

    e.ms = duration;
    e.w = 1280;
    e.h = 720;
    e.fps_x100 = 3000;
    snprintf(e.word, sizeof(e.word), "%s", audio);
    snprintf(e.codec, sizeof(e.codec), "h264");
    return video_model_event(m, &e);
}

static unsigned state(struct video_model *m, enum video_play s, int64_t ms)
{
    struct video_event e = ev(VIDEO_EV_STATE);

    e.value = s;
    e.ms = ms;
    return video_model_event(m, &e);
}

static unsigned at(struct video_model *m, enum video_ev_kind kind, int64_t ms)
{
    struct video_event e = ev(kind);

    e.ms = ms;
    return video_model_event(m, &e);
}

static unsigned exited(struct video_model *m, enum video_exit why)
{
    struct video_event e = ev(VIDEO_EV_EXITED);

    e.reason = why;
    e.value = why == VIDEO_EXIT_NORMAL ? 0 : 137;
    return video_model_event(m, &e);
}

/* A model playing a 60 s file. */
static void playing(struct video_model *m)
{
    video_model_init(m);
    video_model_choose(m, "clip.mp4");
    opened(m, 60000, VIDEO_AUDIO_ON);
    state(m, VIDEO_PLAY_PLAYING, 0);
}

int main(void)
{
    struct video_model m;
    unsigned a;

    /* ---- choosing and opening ---- */
    video_model_init(&m);
    check("starts on the list", m.status == VIDEO_ST_LIST && !m.fullscreen);
    check("the list's BACK goes home", video_model_back(&m) == VIDEO_ACT_HOME);
    check("no transport on the list", video_model_toggle(&m) == 0 && video_model_stop(&m) == 0 &&
                                          video_model_seek(&m, 100) == 0 &&
                                          video_model_fullscreen(&m) == 0);
    check("RESCAN on the list", video_model_rescan(&m) == VIDEO_ACT_RESCAN);
    check("a name the list would not show is refused", video_model_choose(&m, "notes.txt") == 0 &&
                                                           video_model_choose(&m, NULL) == 0 &&
                                                           m.status == VIDEO_ST_LIST);
    a = video_model_choose(&m, "clip.mp4");
    check("choosing starts a helper and lays the player out",
          a == (VIDEO_ACT_START | VIDEO_ACT_LAYOUT) && m.status == VIDEO_ST_OPENING &&
              m.want_play && strcmp(m.name, "clip.mp4") == 0);
    check("RESCAN is not offered in the player", video_model_rescan(&m) == 0);
    check("no transport while opening", !video_model_can_control(&m) && video_model_stop(&m) == 0 &&
                                            video_model_seek(&m, 10) == 0);
    check("a second choice while one is open is ignored", video_model_choose(&m, "b.mp4") == 0 &&
                                                              strcmp(m.name, "clip.mp4") == 0);
    a = video_model_toggle(&m);
    check("PAUSE while opening only changes the intent", a == 0 && !m.want_play);
    a = video_model_toggle(&m);
    check("... and PLAY back", a == 0 && m.want_play);
    check("a stray state before opened is ignored",
          state(&m, VIDEO_PLAY_PLAYING, 0) == 0 && m.status == VIDEO_ST_OPENING);
    a = opened(&m, 60000, VIDEO_AUDIO_ON);
    check("opened with the intent to play asks to play",
          a == VIDEO_ACT_PLAY && m.status == VIDEO_ST_PAUSED && m.duration_ms == 60000 &&
              m.src_w == 1280 && m.fps_x100 == 3000 && strcmp(m.audio, "on") == 0);
    check("a second opened is ignored", opened(&m, 1, VIDEO_AUDIO_NONE) == 0 && m.duration_ms == 60000);
    at(&m, VIDEO_EV_FRAME, 0);
    check("a picture is counted", m.frames == 1);
    state(&m, VIDEO_PLAY_PLAYING, 0);
    check("playing", m.status == VIDEO_ST_PLAYING && video_model_can_control(&m) &&
                         strcmp(video_model_status_text(&m), "Playing") == 0);
    at(&m, VIDEO_EV_POS, 1500);
    check("progress", m.pos_ms == 1500 && video_model_shown_pos(&m) == 1500);
    at(&m, VIDEO_EV_POS, 999999);
    check("progress past the end is clamped", m.pos_ms == 60000);

    video_model_init(&m);
    video_model_choose(&m, "clip.mp4");
    video_model_toggle(&m);
    check("opened after PAUSE while opening stays paused", opened(&m, 5000, VIDEO_AUDIO_NONE) == 0 &&
                                                               m.status == VIDEO_ST_PAUSED);
    check("sound words", strcmp(video_model_audio_text(&m), "No sound") == 0);
    snprintf(m.audio, sizeof(m.audio), "%s", VIDEO_AUDIO_ON);
    check("sound on says nothing", strcmp(video_model_audio_text(&m), "") == 0);
    snprintf(m.audio, sizeof(m.audio), "%s", VIDEO_AUDIO_MUTED);
    check("muted", strcmp(video_model_audio_text(&m), "Muted") == 0);

    /* ---- play, pause, stop, rapid taps ---- */
    playing(&m);
    a = video_model_toggle(&m);
    check("PAUSE", a == VIDEO_ACT_PAUSE && !m.want_play);
    check("the status follows the helper, not the tap", m.status == VIDEO_ST_PLAYING);
    state(&m, VIDEO_PLAY_PAUSED, 2000);
    check("paused at 2 s", m.status == VIDEO_ST_PAUSED && m.pos_ms == 2000);
    a = video_model_toggle(&m);
    check("PLAY", a == VIDEO_ACT_PLAY && m.want_play);
    {
        int i;
        unsigned plays = 0;
        unsigned pauses = 0;

        for (i = 0; i < 7; i++) {
            a = video_model_toggle(&m);
            plays += a == VIDEO_ACT_PLAY;
            pauses += a == VIDEO_ACT_PAUSE;
        }
        check("seven rapid taps send seven commands, the last one wins",
              plays + pauses == 7 && pauses == 4 && !m.want_play);
    }
    state(&m, VIDEO_PLAY_PLAYING, 2100);
    state(&m, VIDEO_PLAY_PAUSED, 2100);
    check("the answers in order leave it paused, as the last tap asked",
          m.status == VIDEO_ST_PAUSED && !m.want_play);
    a = video_model_stop(&m);
    check("STOP", a == VIDEO_ACT_STOP && !m.want_play);
    state(&m, VIDEO_PLAY_STOPPED, 0);
    check("stopped at the start", m.status == VIDEO_ST_STOPPED && m.pos_ms == 0 &&
                                      strcmp(video_model_status_text(&m), "Stopped") == 0);
    check("PLAY after STOP", video_model_toggle(&m) == VIDEO_ACT_PLAY);

    /* ---- the end ---- */
    playing(&m);
    state(&m, VIDEO_PLAY_ENDED, 60000);
    check("ended", m.status == VIDEO_ST_ENDED && !m.want_play && m.pos_ms == 60000 &&
                       video_model_can_control(&m));
    check("PLAY at the end plays again (the helper starts over)",
          video_model_toggle(&m) == VIDEO_ACT_PLAY && m.want_play);

    /* ---- seeking ---- */
    playing(&m);
    a = video_model_seek(&m, 30000);
    check("a seek is sent", a == VIDEO_ACT_SEEK && m.seek_sent == 30000 && m.seek_inflight &&
                                m.pos_ms == 30000);
    at(&m, VIDEO_EV_POS, 1000);
    check("progress from before the seek does not move the bar back", m.pos_ms == 30000);
    check("a second seek waits", video_model_seek(&m, 40000) == 0 && m.seek_waiting);
    check("a third replaces it", video_model_seek(&m, 45000) == 0 && m.seek_waiting_ms == 45000 &&
                                     m.pos_ms == 45000);
    a = at(&m, VIDEO_EV_SEEKED, 29900);
    check("the answer sends the newest waiting target only",
          a == VIDEO_ACT_SEEK && m.seek_sent == 45000 && m.seek_inflight && !m.seek_waiting &&
              m.pos_ms == 45000);
    a = at(&m, VIDEO_EV_SEEKED, 44960);
    check("the last answer settles it", a == 0 && !m.seek_inflight && m.pos_ms == 44960);
    check("a seek past the end is clamped", video_model_seek(&m, 999999) == VIDEO_ACT_SEEK &&
                                                m.seek_sent == 60000);
    at(&m, VIDEO_EV_SEEKED, 59960);
    check("a seek before the start is clamped", video_model_seek(&m, -5) == VIDEO_ACT_SEEK &&
                                                    m.seek_sent == 0);
    at(&m, VIDEO_EV_SEEKED, 0);

    /* The bar dragged: shown at once, sought once on release. */
    playing(&m);
    check("dragging sends nothing", video_model_drag(&m, 10000) == 0 && m.dragging &&
                                        video_model_shown_pos(&m) == 10000);
    at(&m, VIDEO_EV_POS, 500);
    check("the drag target is shown while progress arrives",
          video_model_shown_pos(&m) == 10000 && m.pos_ms == 500);
    video_model_drag(&m, 20000);
    a = video_model_drag_end(&m, 20000);
    check("released: one seek", a == VIDEO_ACT_SEEK && m.seek_sent == 20000 && !m.dragging);
    check("a release without a drag does nothing", video_model_drag_end(&m, 5) == 0);
    {
        struct video_model n;

        video_model_init(&n);
        video_model_choose(&n, "clip.mp4");
        opened(&n, 0, VIDEO_AUDIO_NONE);
        check("no seeking in a file of unknown length", video_model_seek(&n, 100) == 0 &&
                                                            video_model_drag(&n, 100) == 0);
    }

    /* ---- failures ---- */
    {
        static const struct {
            const char *reason;
            const char *title;
        } cases[] = {
            { VIDEO_OPENFAIL_MISSING, "File not found" },
            { VIDEO_OPENFAIL_UNSUPPORTED, "Can't play this video" },
            { VIDEO_OPENFAIL_CORRUPT, "This file is damaged" },
            { VIDEO_OPENFAIL_DEVICE, "Video decoder not available" },
            { VIDEO_OPENFAIL_IO, "The file could not be read" },
        };
        size_t i;

        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            struct video_event e = ev(VIDEO_EV_OPENFAIL);
            char name[96];

            video_model_init(&m);
            video_model_choose(&m, "clip.mp4");
            snprintf(e.word, sizeof(e.word), "%s", cases[i].reason);
            snprintf(e.text, sizeof(e.text), "detail %zu", i);
            video_model_event(&m, &e);
            snprintf(name, sizeof(name), "openfail %s: an error with its title", cases[i].reason);
            check(name, m.status == VIDEO_ST_ERROR && strcmp(m.error_title, cases[i].title) == 0 &&
                            !m.want_play && !video_model_can_control(&m));
        }
    }
    check("the error's title is the status text",
          strcmp(video_model_status_text(&m), "The file could not be read") == 0);
    check("no transport in an error", video_model_toggle(&m) == 0 && video_model_seek(&m, 5) == 0);
    check("events after an error change nothing", opened(&m, 5000, VIDEO_AUDIO_ON) == 0 &&
                                                      m.status == VIDEO_ST_ERROR);
    check("the helper leaving after an error adds nothing", exited(&m, VIDEO_EXIT_NORMAL) == 0 &&
                                                                strcmp(m.error_title, "The file could not be read") == 0);
    check("fullscreen can still be left in an error", video_model_fullscreen(&m) == VIDEO_ACT_LAYOUT &&
                                                          video_model_fullscreen(&m) == VIDEO_ACT_LAYOUT);
    a = video_model_back(&m);
    check("BACK from an error: helper ended, list again",
          a == (VIDEO_ACT_ABANDON | VIDEO_ACT_LAYOUT | VIDEO_ACT_RESCAN) &&
              m.status == VIDEO_ST_LIST && m.error_title[0] == '\0');

    playing(&m);
    {
        struct video_event e = ev(VIDEO_EV_ERROR);

        snprintf(e.word, sizeof(e.word), "decode");
        snprintf(e.text, sizeof(e.text), "damaged pictures");
        video_model_event(&m, &e);
        check("a decode error mid-file", m.status == VIDEO_ST_ERROR &&
                                             strcmp(m.error_title, "Playback failed") == 0 &&
                                             strcmp(m.error_detail, "damaged pictures") == 0);
    }
    playing(&m);
    exited(&m, VIDEO_EXIT_HUNG);
    check("a hung helper", m.status == VIDEO_ST_ERROR &&
                               strcmp(m.error_title, "The player stopped responding") == 0);
    playing(&m);
    exited(&m, VIDEO_EXIT_CRASHED);
    check("a crashed helper", m.status == VIDEO_ST_ERROR && strcmp(m.error_title, "The player crashed") == 0);
    playing(&m);
    exited(&m, VIDEO_EXIT_PROTOCOL);
    check("a misbehaving helper", m.status == VIDEO_ST_ERROR &&
                                      strcmp(m.error_title, "The player misbehaved") == 0);
    playing(&m);
    exited(&m, VIDEO_EXIT_NORMAL);
    check("a helper that just leaves", m.status == VIDEO_ST_ERROR &&
                                           strcmp(m.error_title, "The player ended") == 0);
    video_model_init(&m);
    video_model_choose(&m, "clip.mp4");
    video_model_start_failed(&m, "fork: no memory");
    check("a helper that could not start", m.status == VIDEO_ST_ERROR &&
                                               strcmp(m.error_detail, "fork: no memory") == 0);

    /* ---- sound changing while playing ---- */
    playing(&m);
    {
        struct video_event e = ev(VIDEO_EV_AUDIO);
        struct video_event s = ev(VIDEO_EV_STATS);

        snprintf(e.word, sizeof(e.word), "%s", VIDEO_AUDIO_BUSY);
        video_model_event(&m, &e);
        check("the sound card busy: said, playing on", strcmp(video_model_audio_text(&m), "Sound busy") == 0 &&
                                                          m.status == VIDEO_ST_PLAYING);
        s.stats.fps_x10 = 298;
        s.stats.dropped = 3;
        video_model_event(&m, &s);
        check("statistics kept", m.have_stats && m.stats.fps_x10 == 298 && m.stats.dropped == 3);
    }

    /* ---- fullscreen and BACK ---- */
    playing(&m);
    check("fullscreen on", video_model_fullscreen(&m) == VIDEO_ACT_LAYOUT && m.fullscreen);
    check("playback goes on in fullscreen", video_model_toggle(&m) == VIDEO_ACT_PAUSE);
    check("fullscreen off", video_model_fullscreen(&m) == VIDEO_ACT_LAYOUT && !m.fullscreen);
    video_model_fullscreen(&m);
    a = video_model_back(&m);
    check("BACK while playing in fullscreen ends the helper and returns to the list",
          a == (VIDEO_ACT_ABANDON | VIDEO_ACT_LAYOUT | VIDEO_ACT_RESCAN) && m.status == VIDEO_ST_LIST &&
              !m.fullscreen && !m.want_play && m.duration_ms == 0 && m.frames == 0);
    check("the abandoned helper's last words change nothing",
          state(&m, VIDEO_PLAY_PAUSED, 100) == 0 && exited(&m, VIDEO_EXIT_CRASHED) == 0 &&
              at(&m, VIDEO_EV_FRAME, 5) == 0 && m.status == VIDEO_ST_LIST && m.frames == 0);
    a = video_model_choose(&m, "again.mp4");
    check("reopen: a fresh start", a == (VIDEO_ACT_START | VIDEO_ACT_LAYOUT) &&
                                       m.status == VIDEO_ST_OPENING && !m.seek_inflight &&
                                       !m.have_stats && m.audio[0] == '\0');

    printf("video_state_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
