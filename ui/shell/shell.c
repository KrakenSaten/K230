/*
 * PocketOS shell: status bar, launcher and in-process app host (ADR-002).
 *
 * Options:
 *   --open <app-id>          open an app at start
 *   --screenshot <file.png>  save the screen after the first tick
 *   --exit-after-ms <n>      quit after n milliseconds (headless testing)
 *   --rotation <mode>        automatic|portrait|landscape for this run only,
 *                            instead of the stored mode (nothing is stored)
 *   --no-lock                start open, without the lock screen
 *   --controls               start on DOORS Controls (implies --no-lock)
 *
 * The shell starts behind the lock screen (shell_lock.h) unless one of the
 * above says otherwise, --open names an app, the settings say lock_screen=0,
 * or this start is the shell restarting itself to apply a rotation.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "app.h"
#include "art.h"
#include "brightness.h"
#include "volume.h"
#include "chrome.h"
#include "controls.h"
#include "home.h"
#include "home_layout.h"
#include "shell_lock.h"
#include "clock_runtime.h"
#include "clock_time.h"
#include "platform.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_keyboard.h"
#include "settings.h"
#include "shell_alarm.h"
#include "shell_display.h"
#include "shell_ipc.h"
#include "shell_kb_state.h"
#include "shell_kbd.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef POCKETOS_DISPLAY_NAME
#define POCKETOS_DISPLAY_NAME "unknown"
#endif

/* How long the once-a-second status poll waits for radiod: the UI deadline
 * shared with every app tick (shell_ipc.h). Deliberately less than the tick
 * that drives it, so a wedged service costs at most one frame and never
 * accumulates. Only radio.send still waits (docs/api/pocketipc.md, Request
 * deadlines). */
#ifndef STATUS_POLL_TIMEOUT_MS
#define STATUS_POLL_TIMEOUT_MS SHELL_IPC_UI_TIMEOUT_MS
#endif

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

extern const struct pocketos_app app_radio;
extern const struct pocketos_app app_system;
extern const struct pocketos_app app_fleet;
extern const struct pocketos_app app_radar;
extern const struct pocketos_app app_notes;
extern const struct pocketos_app app_timber;
extern const struct pocketos_app app_clock;
extern const struct pocketos_app app_calendar;
extern const struct pocketos_app app_calculator;
extern const struct pocketos_app app_settings;
extern const struct pocketos_app app_wave;
extern const struct pocketos_app app_rift;
extern const struct pocketos_app app_files;
extern const struct pocketos_app app_camera;
extern const struct pocketos_app app_browser;
extern const struct pocketos_app app_recorder;
extern const struct pocketos_app app_vision;
extern const struct pocketos_app app_video;
extern const struct pocketos_app app_solitaire;
extern const struct pocketos_app app_blackjack;
extern const struct pocketos_app app_2048;
extern const struct pocketos_app app_deskbuddy;
extern const struct pocketos_app app_mp3;
/* Apps a shell can be configured without: Zabbix, in by default and left out
 * with -DPOCKETOS_WITH_ZABBIX=OFF (ui/shell/CMakeLists.txt, docs/apps/ZABBIX.md).
 * A macro rather than an #ifdef inside the list below, so the list reads the
 * same as the apps every build has (tests/app_icons_test.sh parses it). */
#ifdef POCKETOS_WITH_ZABBIX
extern const struct pocketos_app app_zabbix;
#define OPTIONAL_APPS , &app_zabbix
#else
#define OPTIONAL_APPS
#endif

/* The registry. Where each app is shown on the launcher - its group, its
 * place in the group, its colour - is the launcher's table (home_layout.c),
 * not this order; an app added here and nowhere else is shown under MORE. */
static const struct pocketos_app *const apps[] = { &app_radio, &app_system, &app_fleet,
                                            &app_radar, &app_timber, &app_notes,
                                            &app_clock, &app_calendar, &app_calculator,
                                            &app_settings, &app_wave, &app_rift, &app_files,
                                            &app_camera, &app_browser, &app_recorder, &app_vision,
                                            &app_video, &app_solitaire, &app_blackjack, &app_2048, &app_mp3,
                                            &app_deskbuddy OPTIONAL_APPS };
#define APP_COUNT (sizeof(apps) / sizeof(apps[0]))

struct shell {
    lv_obj_t *cluster;      /* the status cluster (DS §36): chip and clock, top right */
    lv_obj_t *status_clock;
    lv_obj_t *status_radio;
    char status_hint[96];   /* what the app last wrote with pocketos_shell_set_status_hint() */
    lv_obj_t *header_hint;  /* where the app header shows it, or NULL with no app open */
    lv_obj_t *content;      /* the content area: the whole display but the keyboard */
    enum pocketos_chrome chrome; /* the status chrome in force (chrome.h), resolved by the shell */
    bool cluster_shown;     /* the cluster as drawn now: the chrome's, but for the lock over NONE */
    /* The widest box the cluster can take (chrome_cluster_box of its widest
     * chip and clock): in an app, and on the shell's own screens, which show
     * no clock in it. What rows under it keep clear of, so they do not move
     * when the chip changes state (DS §36.1). */
    struct chrome_rect cluster_reserve_app;
    struct chrome_rect cluster_reserve_env;
    lv_obj_t *home;         /* launcher */
    lv_obj_t *controls;     /* DOORS Controls, over the launcher (controls.h) */
    lv_obj_t *backdrop;     /* the home photograph, behind the cluster and the launcher */
    lv_image_dsc_t *home_bg;
    bool landscape;
    lv_obj_t *app_root;     /* current app container or NULL */
    lv_obj_t *app_header;   /* its header row, or NULL: none open, or the app draws its own (app.h `header`) */
    lv_obj_t *app_body;     /* the body the app was created in, or NULL */
    const struct pocketos_app *app;
    void *app_priv;
    lv_timer_t *tick;
    const char *screenshot_path;
    int screenshot_pending;
    struct pocketipc_server *server;
    const char *backend_name;
    lv_obj_t *keyboard;     /* the one touch keyboard (DS 17.4), hidden by default */
    void (*kb_done_cb)(void *user);
    void *kb_done_user;
    struct brightness brightness; /* the panel's backlight device, probed once */
    struct volume_state volume;   /* the system volume and mute (volume.h) */
    struct shell_display display; /* this run's orientation and geometry, decided once */
};

static struct shell sh;

/* Set from SIGTERM or SIGINT; the main loop leaves through its normal exit
 * path so the app is destroyed and the socket is unlinked. */
static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* Whether the last status poll reached radiod, so the transition is logged
 * rather than the state repeated every second. */
static int radio_reachable = 1;

static int screenshot_save(const char *path);
static void app_open(const struct pocketos_app *app);

/* ---- status bar ------------------------------------------------------- */

/* Radio chip states (DS §9): RX, TX, OFF, NA share the chip geometry style
 * and differ only in the state style. */
static void radio_chip_set(enum pos_style_role state, const char *label)
{
    static const enum pos_style_role states[] = {
        POS_STYLE_CHIP_RX, POS_STYLE_CHIP_TX, POS_STYLE_CHIP_OFF, POS_STYLE_CHIP_NA
    };
    size_t i;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        lv_obj_remove_style(sh.status_radio, pos_style(states[i]), 0);
    }
    pos_style_add(sh.status_radio, state, 0);
    lv_label_set_text_fmt(sh.status_radio, LV_SYMBOL_WIFI " %s", label);
}

/* What the poll below last saw, so an app can show the radio without asking
 * radiod a second time (app.h, pocketos_shell_radio_state). The shell is
 * already asking once a second; a second timer on the same service would
 * double the IPC on the LVGL thread for no new information. */
static char radio_state_seen[16];

const char *pocketos_shell_radio_state(void)
{
    return radio_state_seen[0] ? radio_state_seen : NULL;
}

static void status_update(void)
{
    char buf[16];
    char err[96];
    cJSON *st;

    /* The same rule PocketClock uses, for the same reason: this board has no
     * battery-backed clock, so after a power-off the time is not a time. A
     * status bar reading 01:00 in the corner while the Clock app says the
     * time is not set would make one of the two a liar - and the one nobody
     * is looking at would win, because it looks like a clock. */
    clock_format_wall(&clock_runtime_now()->wall, buf, sizeof(buf));
    lv_label_set_text(sh.status_clock, buf);
    {
        /* The launcher's and the lock's large clock read the same reading,
         * through the same rule: an unset clock is "--:--", not 01:00. */
        char date[48];

        clock_format_date(&clock_runtime_now()->wall, date, sizeof(date));
        home_set_time(buf, date);
        shell_lock_set_time(buf, date);
    }

#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
    {
        /* Simulator only: hold the chip in one state, so every state can be
         * drawn under every chrome (tests/chrome_shell_test.sh). The mock
         * radio leaves TX as soon as it enters it. Compiled out of the
         * panel's build, like the chrome hook below. */
        const char *forced = getenv("POCKETOS_TEST_RADIO_STATE");

        if (forced && forced[0]) {
            if (strcmp(forced, "rx") == 0) {
                radio_chip_set(POS_STYLE_CHIP_RX, "RX");
            } else if (strcmp(forced, "tx") == 0) {
                radio_chip_set(POS_STYLE_CHIP_TX, "TX");
            } else if (strcmp(forced, "off") == 0) {
                radio_chip_set(POS_STYLE_CHIP_OFF, "OFF");
            } else {
                radio_chip_set(POS_STYLE_CHIP_NA, "--");
            }
            return;
        }
    }
#endif
    /* This runs on the LVGL thread once a second. Before the deadline, a
     * radiod that was alive but not answering held the whole UI: nothing
     * repainted, touch did nothing, and the supervisor saw a healthy process
     * because the shell had not crashed. One frame's worth of patience is
     * enough for a local service, and the next tick asks again. */
    st = shell_ipc_call_timeout("radiod", "radio.status", NULL,
                                STATUS_POLL_TIMEOUT_MS, err, sizeof(err));
    /* Once per transition, not once per second: the chip alone cannot say
     * whether radiod is gone or merely not answering, and that is the first
     * thing anyone debugging this asks. */
    if (st && !radio_reachable) {
        LOG_INFO("radiod is answering again");
        radio_reachable = 1;
    } else if (!st && radio_reachable) {
        LOG_WARN("radio.status poll failed: %s", err[0] ? err : "radiod unavailable");
        radio_reachable = 0;
    }
    if (st) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(st, "state");
        const char *s = cJSON_IsString(state) ? state->valuestring : "?";

        snprintf(radio_state_seen, sizeof(radio_state_seen), "%s", s);
        if (strcmp(s, "tx") == 0) {
            radio_chip_set(POS_STYLE_CHIP_TX, "TX");
        } else if (strcmp(s, "rx") == 0) {
            radio_chip_set(POS_STYLE_CHIP_RX, "RX");
        } else {
            radio_chip_set(POS_STYLE_CHIP_OFF, "OFF");
        }
        cJSON_Delete(st);
    } else {
        /* "--" not U+2014: the Montserrat symbol font used by this chip has
         * no em dash (PocketFleet finding 9); revisit with the DS icon set. */
        radio_state_seen[0] = '\0';
        radio_chip_set(POS_STYLE_CHIP_NA, "--");
    }
}

/* The status cluster (DS §36.1): one capsule anchored to the top-right
 * corner, as wide as what it holds - the radio chip, then the clock - and
 * never the screen. It replaces the full-width bar of §7 and §30 and keeps
 * its cells but the wordmark and the hint: the chip and the clock with the
 * same styles, the same poll and the same rules; the hint went to the app
 * header, where the fullscreen apps already had it (§30.8).
 *
 * It lies over the right end of whatever row is at the top - an app's
 * header, the launcher's header band, Controls' header row - and each of
 * those keeps clear of its widest box (sh.cluster_reserve_*). It takes no
 * touch: it has no action, and a finger landing on it reaches whatever is
 * under it, as it would have with nothing drawn there. */
static void status_cluster_create(lv_obj_t *screen)
{
    const struct pos_insets top = pos_display_bar_insets(pocketui_display_geometry(), POS_EDGE_TOP);
    lv_obj_t *c = lv_obj_create(screen);

    lv_obj_remove_style_all(c);
    pos_style_add(c, POS_STYLE_STATUS_CLUSTER, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, LV_SIZE_CONTENT, POCKETOS_CHROME_CLUSTER_H);
    lv_obj_set_style_pad_ver(c, 0, 0);
    lv_obj_set_style_pad_left(c, POCKETOS_CHROME_CLUSTER_PAD_L, 0);
    lv_obj_set_style_pad_right(c, POCKETOS_CHROME_CLUSTER_PAD_R, 0);
    lv_obj_set_style_pad_column(c, POCKETOS_CHROME_CLUSTER_GAP, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Anchored by its right edge: a wider chip or a hidden clock changes
     * where it starts, never where it ends (chrome_cluster_box). */
    lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -LV_MAX(POCKETOS_CHROME_EDGE_MIN, top.right),
                 POCKETOS_CHROME_CLUSTER_Y);

    sh.status_radio = lv_label_create(c);
    pos_style_add(sh.status_radio, POS_STYLE_CHIP, 0);
    /* glyphs come from the symbol font role until the DS icon set exists */
    pos_style_add(sh.status_radio, POS_STYLE_SYMBOL, 0);
    radio_chip_set(POS_STYLE_CHIP_NA, "?");

    sh.status_clock = pocketui_label(c, "--:--", POS_STYLE_CAPTION);
    sh.cluster = c;
}

/* The width text takes in label's font and tracking. */
static int32_t text_width(lv_obj_t *label, const char *text)
{
    lv_point_t p;

    lv_text_get_size(&p, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return p.x;
}

/* The widest the cluster can be, worked out from its fonts rather than from
 * whatever it happens to show now: every chip state radio_chip_set() writes,
 * and the clock at its widest digits (or "--:--"). Rows under the cluster
 * keep clear of this box, so nothing moves when the radio goes from RX to
 * TX or the minute turns. */
static void cluster_reserve_compute(void)
{
    static const char *const states[] = { LV_SYMBOL_WIFI " RX", LV_SYMBOL_WIFI " TX",
                                          LV_SYMBOL_WIFI " OFF", LV_SYMBOL_WIFI " --",
                                          LV_SYMBOL_WIFI " ?" };
    const struct pos_display_geometry *g = pocketui_display_geometry();
    const struct pos_insets top = pos_display_bar_insets(g, POS_EDGE_TOP);
    int32_t chip = 0;
    int32_t clock;
    /* The theme's hairline, which the app capsule is drawn with and which
     * is never thinner than the environment's 1 px glass edge. */
    int32_t border = 2 * LV_MAX(pos_theme_current()->hairline_px, 1);
    char digits[8];
    char widest = '0';
    size_t k;

    for (k = 0; k < sizeof(states) / sizeof(states[0]); k++) {
        chip = LV_MAX(chip, text_width(sh.status_radio, states[k]));
    }
    chip += lv_obj_get_style_pad_left(sh.status_radio, LV_PART_MAIN) +
            lv_obj_get_style_pad_right(sh.status_radio, LV_PART_MAIN);
    for (k = 0; k < 10; k++) {
        char a[2] = { (char)('0' + k), '\0' };
        char b[2] = { widest, '\0' };

        if (text_width(sh.status_clock, a) > text_width(sh.status_clock, b)) {
            widest = (char)('0' + k);
        }
    }
    snprintf(digits, sizeof(digits), "%c%c:%c%c", widest, widest, widest, widest);
    clock = LV_MAX(text_width(sh.status_clock, digits), text_width(sh.status_clock, "--:--"));
    sh.cluster_reserve_app =
        chrome_cluster_box(g->width, top.right, chrome_cluster_width(chip, clock) + border);
    sh.cluster_reserve_env = chrome_cluster_box(g->width, top.right, chrome_cluster_width(chip, 0) + border);
}

/* ---- the DOORS environment (DS §31) ------------------------------------ *
 *
 * On the shell's own screens - the launcher, Controls, the lock - the home
 * photograph lies behind everything and the status cluster is glass on it,
 * as the launcher's panels are, with no clock, because each of those
 * screens shows the time large already. In an app the cluster is the
 * theme's. One function decides which, from what is in front.
 */
static void cluster_fit(void);

static void environment_apply(void)
{
    bool env = !sh.app || shell_lock_is_locked();

    /* The lock over a fullscreen app brings the cluster back (cluster_fit). */
    cluster_fit();

    lv_obj_remove_style(sh.cluster, pos_style(POS_STYLE_ENV_CLUSTER), 0);
    if (env) {
        pos_style_add(sh.cluster, POS_STYLE_ENV_CLUSTER, 0);
        lv_obj_add_flag(sh.status_clock, LV_OBJ_FLAG_HIDDEN);
        /* The chip alone, with the same air on both sides of it
         * (chrome_cluster_width). */
        lv_obj_set_style_pad_right(sh.cluster, POCKETOS_CHROME_CLUSTER_PAD_L, 0);
    } else {
        lv_obj_remove_flag(sh.status_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_right(sh.cluster, POCKETOS_CHROME_CLUSTER_PAD_R, 0);
    }
    if (sh.backdrop) {
        if (!sh.app && sh.home_bg) {
            lv_obj_remove_flag(sh.backdrop, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sh.backdrop, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* The hint lives in the app header (DS §36.1, as §30.8 had it under NONE):
 * kept here as text so a header built later - the next app's, or this one's
 * after a rotation - starts from what was last written. */
void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(sh.status_hint, sizeof(sh.status_hint), "%s", text ? text : "");
    if (sh.header_hint) {
        lv_label_set_text(sh.header_hint, sh.status_hint);
    }
}

int pocketos_shell_reduced_motion(void)
{
    const char *v = settings_get("reduced_motion", "0");

    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

/* The date, from the reading the tick above already took, so an app does not
 * read a clock of its own (app.h). The validity rule is PocketClock's and is
 * not repeated here: below CLOCK_WALL_VALID_FROM the wall clock is not a
 * time, and -1 says so rather than letting the epoch be drawn as a date. */
int64_t pocketos_shell_system_day(void)
{
    const struct clock_wall *w = &clock_runtime_now()->wall;

    return w->valid ? w->day : -1;
}

/* ---- display brightness (docs/hardware/DISPLAY_BRIGHTNESS.md) ----------- *
 *
 * The shell owns the panel, so it owns the panel's brightness too: one probe
 * at start, one writer, and Settings asks here rather than touching sysfs
 * itself. The simulator build can point the probe at a fake sysfs tree for
 * the tests; the K230 build is compiled without that and always reads /sys.
 */

static const char *sysfs_root(void)
{
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
    const char *root = getenv("POCKETOS_TEST_SYSFS_ROOT");

    if (root && root[0]) {
        return root;
    }
#endif
    return "/sys";
}

int pocketos_shell_brightness_get(void)
{
    return brightness_get_percent(&sh.brightness);
}

int pocketos_shell_brightness_set(int percent)
{
    char value[12];
    int applied = brightness_set_percent(&sh.brightness, percent);

    if (applied < 0) {
        if (sh.brightness.supported) {
            LOG_WARN("brightness %d%% not applied to %s: %s", percent, sh.brightness.name,
                     strerror(errno));
        }
        return -1;
    }
    /* Only a level the panel accepted is remembered, so a failed write cannot
     * leave a value in the store that the next boot would try again. */
    snprintf(value, sizeof(value), "%d", applied);
    if (settings_set(BRIGHTNESS_SETTING, value) < 0) {
        LOG_WARN("brightness %d%% applied but not persisted to %s: %s", applied,
                 settings_path(), strerror(errno));
    }
    LOG_INFO("brightness %d%%", applied);
    return applied;
}

/* ---- system volume (volume.h) ------------------------------------------- */

int pocketos_shell_volume_get(void)
{
    return sh.volume.percent;
}

int pocketos_shell_volume_muted(void)
{
    return sh.volume.muted;
}

int pocketos_shell_volume_effective(void)
{
    return volume_effective(&sh.volume);
}

int pocketos_shell_volume_available(void)
{
    return volume_output_present("/proc");
}

int pocketos_shell_volume_set(int percent)
{
    char value[12];

    if (percent < VOLUME_MIN_PCT || percent > VOLUME_MAX_PCT || percent % VOLUME_STEP_PCT != 0) {
        return -1;
    }
    sh.volume.percent = percent;
    snprintf(value, sizeof(value), "%d", percent);
    if (settings_set(VOLUME_SETTING, value) < 0) {
        LOG_WARN("volume %d%% set but not persisted to %s: %s", percent, settings_path(),
                 strerror(errno));
        return -1;
    }
    LOG_INFO("volume %d%%", percent);
    return 0;
}

int pocketos_shell_volume_set_muted(int muted)
{
    if (muted != 0 && muted != 1) {
        return -1;
    }
    sh.volume.muted = muted;
    if (settings_set(VOLUME_MUTED_SETTING, muted ? "1" : "0") < 0) {
        LOG_WARN("volume %s but not persisted to %s: %s", muted ? "muted" : "unmuted",
                 settings_path(), strerror(errno));
        return -1;
    }
    LOG_INFO("volume %s", muted ? "muted" : "unmuted");
    return 0;
}

static void volume_restore(void)
{
    int bad = volume_load(&sh.volume, settings_get(VOLUME_SETTING, NULL),
                          settings_get(VOLUME_MUTED_SETTING, NULL));

    if (bad & 1) {
        LOG_WARN("volume: stored %s=%s is not %d..%d in steps of %d, using %d%%", VOLUME_SETTING,
                 settings_get(VOLUME_SETTING, ""), VOLUME_MIN_PCT, VOLUME_MAX_PCT, VOLUME_STEP_PCT,
                 VOLUME_DEFAULT_PCT);
    }
    if (bad & 2) {
        LOG_WARN("volume: stored %s=%s is not 0 or 1, not muted", VOLUME_MUTED_SETTING,
                 settings_get(VOLUME_MUTED_SETTING, ""));
    }
    LOG_INFO("volume: %d%%%s%s", sh.volume.percent, sh.volume.muted ? ", muted" : "",
             volume_output_present("/proc") ? "" : " (no sound card)");
}

/* Called once, after the settings are loaded and before the first frame. A
 * stored value is applied only when it is one this code could have written;
 * anything else is logged and left alone, like a rejected theme (DS §8), and
 * the panel keeps the level it booted with. */
static void brightness_restore(void)
{
    const char *stored;
    int pct;

    if (brightness_probe(&sh.brightness, sysfs_root()) != 0) {
        LOG_INFO("brightness: no backlight device, control unavailable");
        return;
    }
    stored = settings_get(BRIGHTNESS_SETTING, NULL);
    if (!stored) {
        LOG_INFO("brightness: %s, %d%% (nothing stored, left as booted)", sh.brightness.name,
                 brightness_get_percent(&sh.brightness));
        return;
    }
    if (brightness_parse_setting(stored, &pct) < 0) {
        LOG_WARN("brightness: stored %s=%s is not %d..%d, left as booted", BRIGHTNESS_SETTING,
                 stored, BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT);
        return;
    }
    if (brightness_set_percent(&sh.brightness, pct) < 0) {
        LOG_WARN("brightness: stored %d%% not applied to %s: %s", pct, sh.brightness.name,
                 strerror(errno));
        return;
    }
    LOG_INFO("brightness: %s restored to %d%%", sh.brightness.name, pct);
}

/* ---- display orientation (shell_display.h) ------------------------------ *
 *
 * One orientation per shell run, decided before the display existed. What
 * changes it while the shell runs is the stored mode (Settings,
 * shell.rotation) or the keyboard being attached or removed, and since the
 * display is rotated when it is opened (shell_display.h), applying either
 * means opening it again: the shell re-executes itself in place, keeping its
 * pid, so the supervisor sees no exit and the OS is never restarted.
 *
 * Two things keep that from becoming a loop or a flicker. The keyboard is
 * probed before the display is opened, so a boot with the base attached opens
 * landscape once instead of correcting itself afterwards; and a change waits
 * out a settle window here, on top of the debounce in kbd_presence, so a
 * connector making and breaking contact costs nothing.
 */

/* Long enough that a mode tapped twice in Settings, or a base board finding
 * its contacts, resolves before anything is applied; short enough that the
 * screen turns while the hand is still on the device. */
#define ROTATE_SETTLE_MS 800

/* The mark restart_in_place leaves for the shell it becomes (main). Its value
 * is the lock as the restart found it, so the next run puts it back exactly:
 * RESUME_LOCKED or RESUME_OPEN. Anything else - an empty mark, or "1" from a
 * build before the value meant anything - is not a continuation the shell can
 * vouch for, and starts as a cold start does. */
#define RESUME_ENV "DOORS_SHELL_RESUMED"
/* And the launcher folder that was open, if one was (home.h): the restart
 * is the same session turning round, so it comes back to the same page. */
#define RESUME_FOLDER_ENV "DOORS_LAUNCHER_FOLDER"
#define RESUME_LOCKED "locked"
#define RESUME_OPEN "open"
/* settings.conf: lock_screen=0 starts the shell open (a bench unit, a kiosk). */
#define LOCK_SETTING "lock_screen"

static bool restart_pending;
static lv_timer_t *rotate_timer;
static char **shell_argv;

_Static_assert((int)POCKETOS_ROTATION_AUTOMATIC == (int)ORIENTATION_AUTOMATIC &&
                   (int)POCKETOS_ROTATION_PORTRAIT == (int)ORIENTATION_PORTRAIT &&
                   (int)POCKETOS_ROTATION_LANDSCAPE == (int)ORIENTATION_LANDSCAPE,
               "app.h rotation modes are orientation.h's");
_Static_assert((int)POCKETOS_KEYBOARD_UNKNOWN == (int)KBD_PRESENCE_UNKNOWN &&
                   (int)POCKETOS_KEYBOARD_ABSENT == (int)KBD_PRESENCE_ABSENT &&
                   (int)POCKETOS_KEYBOARD_PRESENT == (int)KBD_PRESENCE_PRESENT,
               "app.h keyboard states are kbd_presence.h's");

static bool is_landscape(enum pos_rotation r)
{
    return pos_rotation_is_landscape_of(r, sh.display.panel.width, sh.display.panel.height);
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    enum pos_rotation next = shell_display_next_rotation(&sh.display);
    bool valid;

    out->mode = (enum pocketos_rotation_mode)orientation_mode_from_setting(
        settings_get(ORIENTATION_SETTING, NULL), &valid);
    out->mode_valid = valid;
    out->landscape = is_landscape(sh.display.geometry.rotation);
    out->next_landscape = is_landscape(next);
    out->applying = next != sh.display.geometry.rotation;
    out->keyboard = (enum pocketos_keyboard)kbd_presence_get();
}

static void rotation_to_json(cJSON *o)
{
    struct pocketos_orientation now;
    enum pos_rotation next = shell_display_next_rotation(&sh.display);

    pocketos_shell_orientation(&now);
    cJSON_AddStringToObject(o, "rotation_mode", orientation_mode_name((enum orientation_mode)now.mode));
    cJSON_AddBoolToObject(o, "rotation_mode_valid", now.mode_valid);
    cJSON_AddNumberToObject(o, "rotation", pos_rotation_degrees(sh.display.geometry.rotation));
    cJSON_AddStringToObject(o, "orientation", now.landscape ? "landscape" : "portrait");
    cJSON_AddNumberToObject(o, "next_rotation", pos_rotation_degrees(next));
    cJSON_AddStringToObject(o, "next_orientation", now.next_landscape ? "landscape" : "portrait");
    cJSON_AddBoolToObject(o, "applying", now.applying);
    cJSON_AddStringToObject(o, "keyboard", kbd_presence_name(kbd_presence_get()));
    cJSON_AddBoolToObject(o, "bench_override", sh.display.bench_override);
}

static void rotate_settled(lv_timer_t *timer)
{
    enum pos_rotation next = shell_display_next_rotation(&sh.display);

    (void)timer;
    lv_timer_delete(rotate_timer);
    rotate_timer = NULL;
    if (next == sh.display.geometry.rotation) {
        LOG_INFO("display: rotation %d again before it was applied; the shell stays up",
                 pos_rotation_degrees(next));
        return;
    }
    restart_pending = true;
}

/* Apply what the policy now says, unless it is already what this run is
 * showing. Every caller goes through here, so there is one settle window and
 * one restart however many times the mode or the keyboard changes inside it. */
static void rotation_apply_soon(const char *why)
{
    enum pos_rotation next = shell_display_next_rotation(&sh.display);

    if (restart_pending) {
        return;
    }
    if (next == sh.display.geometry.rotation) {
        if (rotate_timer) {
            lv_timer_delete(rotate_timer);
            rotate_timer = NULL;
            LOG_INFO("display: %s puts the orientation back to rotation %d; nothing to apply", why,
                     pos_rotation_degrees(next));
        }
        return;
    }
    if (rotate_timer) {
        return;
    }
    rotate_timer = lv_timer_create(rotate_settled, ROTATE_SETTLE_MS, NULL);
    if (!rotate_timer) {
        LOG_WARN("display: no timer to apply rotation %d; it applies at the next start",
                 pos_rotation_degrees(next));
        return;
    }
    LOG_INFO("display: %s asks for rotation %d; applying it in %d ms", why, pos_rotation_degrees(next),
             ROTATE_SETTLE_MS);
}

static void announce_rotation(void)
{
    cJSON *data;

    if (!sh.server) {
        return;
    }
    data = cJSON_CreateObject();
    rotation_to_json(data);
    pocketipc_server_broadcast(sh.server, pocketipc_event("shell.rotation", data));
}

int pocketos_shell_set_rotation_mode(enum pocketos_rotation_mode mode)
{
    enum pos_rotation next;

    if (mode != POCKETOS_ROTATION_AUTOMATIC && mode != POCKETOS_ROTATION_PORTRAIT &&
        mode != POCKETOS_ROTATION_LANDSCAPE) {
        return -1;
    }
    if (settings_set(ORIENTATION_SETTING, orientation_mode_name((enum orientation_mode)mode)) < 0) {
        LOG_WARN("rotation mode %s not persisted to %s: %s", orientation_mode_name((enum orientation_mode)mode),
                 settings_path(), strerror(errno));
        return -1;
    }
    next = shell_display_next_rotation(&sh.display);
    LOG_INFO("rotation mode %s stored: rotation %d", orientation_mode_name((enum orientation_mode)mode),
             pos_rotation_degrees(next));
    announce_rotation();
    rotation_apply_soon("Settings");
    return 0;
}

/* The keyboard was attached or removed (shell_kbd.c), or detection stopped
 * being able to tell. In Automatic that changes the orientation; in a forced
 * mode orientation_resolve ignores it, so rotation_apply_soon finds nothing
 * to do and the display is left alone. */
static void on_keyboard_presence(enum kbd_presence now, void *user)
{
    char why[48];

    (void)user;
    LOG_INFO("keyboard %s: rotation %d", kbd_presence_name(now),
             pos_rotation_degrees(shell_display_next_rotation(&sh.display)));
    announce_rotation();
    snprintf(why, sizeof(why), "the keyboard being %s", kbd_presence_name(now));
    rotation_apply_soon(why);
}

/* ---- the one touch keyboard (DS §17.3, §17.4) -------------------------- *
 *
 * The shell owns it so that there is exactly one, and so that an app cannot
 * reach past the stream to a keyboard of its own. Showing it takes the
 * sheet's height off the content area, which is where every app's body
 * lives, so an editor above it keeps a usable height without any app having
 * to know the keyboard's geometry.
 */

/* The content area: below whatever chrome is in force, above whatever is
 * reserved at the foot - the keyboard sheet while it is shown, nothing
 * otherwise. One function for both edges (chrome_content_box), so the box
 * reaches the keyboard exactly while it is up and reaches the foot again
 * when it hides, under FULL, COMPACT and NONE alike: a constant bar
 * subtracted here would leave a dead strip under every chrome but FULL. */
static void content_box(int32_t reserve_bottom)
{
    struct chrome_box b = chrome_content_box(sh.chrome, pocketui_display_geometry()->height,
                                             reserve_bottom);

    lv_obj_set_y(sh.content, b.y);
    lv_obj_set_height(sh.content, b.height);
}

static void on_keyboard_done(void *user)
{
    (void)user;
    if (sh.kb_done_cb) {
        sh.kb_done_cb(sh.kb_done_user);
    }
}

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    /* An alert owns the panel exclusively while it is up (DS §18.5, §18.8).
     * An app asking for the keyboard here is not misbehaving - it cannot see
     * that an alert exists, and must not be able to - so the request is
     * refused rather than reported. Hiding is never refused. */
    if (!sh.keyboard || !pocketos_shell_keyboard_may_show()) {
        return;
    }
    /* Nor while the lock covers the app: the sheet raises itself to the
     * foreground and would lie over the lock, taking the touches it is
     * there to keep off the app (DS §31.4). */
    if (shell_lock_is_locked()) {
        return;
    }
    sh.kb_done_cb = on_done;
    sh.kb_done_user = user;
    pos_keyboard_set_return(sh.keyboard, ret == POCKETOS_KB_NEWLINE ? POS_KB_RETURN_NEWLINE
                                                                    : POS_KB_RETURN_DONE);
    content_box(POS_KB_H);
    pos_keyboard_show(sh.keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    if (!sh.keyboard) {
        return;
    }
    pos_keyboard_hide(sh.keyboard);
    content_box(0);
    sh.kb_done_cb = NULL;
    sh.kb_done_user = NULL;
}

int pocketos_shell_keyboard_visible(void)
{
    return sh.keyboard && pos_keyboard_is_shown(sh.keyboard);
}

/* ---- status chrome (DS §30, §36, chrome.h) ------------------------------ *
 *
 * The shell owns the cluster and everything that follows from it: the
 * content area, the box the rows under it keep clear of, the keyboard
 * reserve. An app declares a policy and is created under the result; it
 * never learns a number and never touches the cluster. Resolved before
 * create() runs, so the body an app is created in is its final one and its
 * first layout pass is its only one.
 */

_Static_assert(POCKETUI_PAD == POCKETOS_CHROME_EDGE_MIN,
               "the cluster keeps the side margin the app header keeps");

/* The radio chip's height and padding, from the line height of the font it
 * draws in (chrome_chip_box, DS §36.1): its text centred in 32 px, never
 * clipped. The same under every chrome, so set once. */
static void status_chip_fit(void)
{
    const lv_font_t *font = lv_obj_get_style_text_font(sh.status_radio, LV_PART_MAIN);
    struct chrome_chip c = chrome_chip_box(lv_font_get_line_height(font));

    lv_obj_set_style_height(sh.status_radio, c.height, 0);
    lv_obj_set_style_pad_top(sh.status_radio, c.pad_top, 0);
    lv_obj_set_style_pad_bottom(sh.status_radio, c.pad_bottom, 0);
}

/* Show the cluster as the chrome in force says. The one exception is the
 * lock over a fullscreen (NONE) app: the lock lies under the cluster and
 * shows its chip, so while it is engaged the cluster comes back - the lock
 * looks exactly as it does over any other app (DS §31.4). Only the cluster:
 * the content area underneath is the same under every chrome, so the app
 * does not move while locked. It goes again as soon as the opening lock
 * starts to show the app through it (shell_lock_is_revealing), not when the
 * fade has ended: a fullscreen app is never seen with the cluster over it. */
static void cluster_fit(void)
{
    bool shown = sh.chrome != POCKETOS_CHROME_NONE ||
                 (shell_lock_is_locked() && !shell_lock_is_revealing());

    if (shown) {
        lv_obj_remove_flag(sh.cluster, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Hidden, not deleted: the clock and the chip keep being written
         * (status_update) and keep their state for the next screen that
         * shows them. */
        lv_obj_add_flag(sh.cluster, LV_OBJ_FLAG_HIDDEN);
    }
    sh.cluster_shown = shown;
}

static void chrome_apply(enum pocketos_chrome effective, const char *what)
{
    struct chrome_box b;

    sh.chrome = effective;
    /* Measured again here rather than once: a display mode can change the
     * hairline, and every screen is laid out after its chrome is applied. */
    cluster_reserve_compute();
    cluster_fit();
    content_box(pocketos_shell_keyboard_visible() ? POS_KB_H : 0);
    b = chrome_content_box(effective, pocketui_display_geometry()->height, 0);
    LOG_INFO("chrome: %s, content from y %d, cluster %s, for %s", chrome_name(effective), (int)b.y,
             sh.cluster_shown ? "shown" : "hidden", what);
}

/* What the app declared - or, in the simulator only, what a test asked for.
 * The hook is how NONE and an explicit CLUSTER are exercised in the running
 * shell on apps that declare otherwise (tests/chrome_shell_test.sh); the
 * panel's build has no such environment and compiles it out. */
static enum pocketos_chrome declared_chrome(const struct pocketos_app *app)
{
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
    const char *forced = getenv("POCKETOS_TEST_CHROME");

    if (forced && forced[0]) {
        if (strcmp(forced, "cluster") == 0) {
            return POCKETOS_CHROME_CLUSTER;
        }
        if (strcmp(forced, "none") == 0) {
            return POCKETOS_CHROME_NONE;
        }
        LOG_WARN("test hook: chrome '%s' is not cluster or none; ignored", forced);
    }
#endif
    return app->chrome;
}

/* ---- app hosting ------------------------------------------------------ */

static void announce_current(void)
{
    cJSON *data;

    if (!sh.server) {
        return;
    }
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "current", sh.app ? sh.app->id : "home");
    pocketipc_server_broadcast(sh.server, pocketipc_event("shell.app", data));
}

static void app_close(void)
{
    if (!sh.app) {
        return;
    }
    LOG_INFO("close app %s", sh.app->id);
    /* Before destroy(), so an app that autosaves on the way out is not
     * looking at a body that is about to change height. */
    pocketos_shell_keyboard_hide();
    if (sh.app->destroy) {
        sh.app->destroy(sh.app_priv);
    }
    sh.app = NULL;
    sh.app_priv = NULL;
    sh.header_hint = NULL; /* it goes with the header */
    sh.app_header = NULL;
    sh.app_body = NULL;
    lv_obj_delete(sh.app_root);
    sh.app_root = NULL;
    pocketos_shell_set_status_hint("");
    announce_current();
}

void pocketos_shell_go_home(void)
{
    app_close();
    /* The launcher's own chrome, whatever the app that just closed had. */
    chrome_apply(chrome_resolve(POCKETOS_CHROME_DEFAULT, is_landscape(sh.display.geometry.rotation), true),
                 "home");
    controls_hide();
    lv_obj_clear_flag(sh.home, LV_OBJ_FLAG_HIDDEN);
    home_keys_attach();
    environment_apply();
}

/* ---- the shell's own actions: Lock, Controls ---------------------------- */

static void shell_lock_now(void)
{
    shell_lock_engage("Lock");
}

static void controls_open(void)
{
    if (sh.app) {
        pocketos_shell_go_home();
    }
    lv_obj_add_flag(sh.home, LV_OBJ_FLAG_HIDDEN);
    home_keys_detach();
    controls_show();
}

static void controls_close(void)
{
    controls_hide();
    lv_obj_clear_flag(sh.home, LV_OBJ_FLAG_HIDDEN);
    home_keys_attach();
}

static void open_by_id(const char *id);

static void on_lock_engaged(void)
{
    /* The touch keyboard belongs to a field the lock now covers. */
    pocketos_shell_keyboard_hide();
    environment_apply();
}

static void on_lock_opened(void)
{
    environment_apply();
}

static void on_lock_revealing(void)
{
    environment_apply();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    pocketos_shell_go_home();
}

static void app_open(const struct pocketos_app *app)
{
    lv_obj_t *header;
    lv_obj_t *back;
    lv_obj_t *name;
    lv_obj_t *body;

    app_close();
    lv_obj_add_flag(sh.home, LV_OBJ_FLAG_HIDDEN);
    /* The launcher is under the app now; its keys are the app's. */
    home_keys_detach();
    controls_hide();
    /* Before anything of the app exists, so the body it is created in is
     * its final one and its first layout pass is its only one (DS §30.2). */
    chrome_apply(chrome_resolve(declared_chrome(app), is_landscape(sh.display.geometry.rotation), false),
                 app->id);

    sh.app_root = lv_obj_create(sh.content);
    lv_obj_remove_style_all(sh.app_root);
    lv_obj_set_size(sh.app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(sh.app_root, LV_FLEX_FLOW_COLUMN);

    /* An app that draws its own top row in landscape (app.h `header`, DS
     * §37.2) gets no header there: the body starts at the top edge, and
     * the app's own row takes the corner inset through the layout guard
     * as any content does. Portrait is the shell's header as ever. */
    if (app->header == POCKETOS_HEADER_NONE_LANDSCAPE &&
        is_landscape(sh.display.geometry.rotation)) {
        header = NULL;
        sh.header_hint = NULL;
        goto body;
    }
    header = lv_obj_create(sh.app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(header, 16, 0);
    /* The header is the top row of every app (DS §36): it runs corner to
     * corner along the top edge, where the panel's rounded corners are, so
     * the back slab and the title keep clear of them through the safe area
     * (DS §21.1). Under the cluster its right end is the cluster's: the
     * header stops short of the widest box the cluster can take, so the
     * title and the hint never run under it and never move when the chip
     * changes state. */
    pocketui_apply_bar_insets(header, POS_EDGE_TOP);
    {
        int32_t reserve = chrome_row_reserve(sh.chrome, pocketui_display_geometry()->width,
                                             &sh.cluster_reserve_app);

        if (reserve > lv_obj_get_style_pad_right(header, LV_PART_MAIN)) {
            lv_obj_set_style_pad_right(header, reserve, 0);
        }
    }

    back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    pos_style_add(back, POS_STYLE_SLAB, 0);
    pos_style_add(back, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(back, 72, 56); /* back slab, DS §7 */
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    name = lv_label_create(back);
    lv_label_set_text(name, LV_SYMBOL_LEFT);
    pos_style_add(name, POS_STYLE_SYMBOL, 0);
    pos_style_add(name, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(name);

    name = pocketui_label(header, app->name, POS_STYLE_TITLE);
    /* The hint: what an app writes with pocketos_shell_set_status_hint() is
     * state the player or the user needs - Fleet's turn, Radar's and
     * Timber's run state, Wave's MIC ON, a Clock or Notes storage error (DS
     * §30.2). It had a cell in the full-width bar; with the bar gone the
     * header carries it for every app, at its right end in the bar's caption
     * type, as it did for the fullscreen apps (§30.8, §36.1). A hint longer
     * than the room left wraps onto a second line inside the header. */
    sh.header_hint = pocketui_label(header, sh.status_hint, POS_STYLE_CAPTION);
    lv_obj_set_flex_grow(sh.header_hint, 1);
    lv_obj_set_style_text_align(sh.header_hint, LV_TEXT_ALIGN_RIGHT, 0);

body:
    sh.app_header = header;
    body = lv_obj_create(sh.app_root);
    sh.app_body = body;
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(body, POCKETUI_BODY_PAD_TOP, 0); /* DS §7 */
    lv_obj_set_style_pad_row(body, POCKETUI_PAD, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    sh.app = app;
    environment_apply();
    sh.app_priv = app->create(body);
    LOG_INFO("open app %s", app->id);
    if (app->tick) {
        app->tick(sh.app_priv);
    }
    announce_current();
}

static void open_app_from_home(const struct pocketos_app *app)
{
    app_open(app);
}

static void open_by_id(const char *id)
{
    size_t k;

    for (k = 0; k < APP_COUNT; k++) {
        if (strcmp(apps[k]->id, id) == 0) {
            app_open(apps[k]);
            return;
        }
    }
    LOG_WARN("no app %s to open", id);
}

/* The launcher and Controls, built once for this run's orientation, in the
 * content area at the launcher's chrome (DS §36.2: the cluster, in both),
 * each keeping clear of the widest box the cluster takes on them. The
 * content area starts at the top edge, so its coordinates are the screen's. */
static void home_build(void)
{
    static const struct home_actions ha = { open_app_from_home, shell_lock_now, controls_open };
    static const struct controls_actions ca = { open_by_id, shell_lock_now, controls_close };
    const struct chrome_rect *r = &sh.cluster_reserve_env;
    lv_area_t home_keepout = { r->x, r->y, r->x + r->w - 1, r->y + r->h - 1 };
    struct controls_rect controls_keepout = { r->x, r->y, r->w, r->h };

    sh.home = home_create(sh.content, apps, APP_COUNT, sh.landscape, &home_keepout, &ha);
    if (!sh.home) {
        sh.home = lv_obj_create(sh.content); /* never NULL for the rest of the shell */
        lv_obj_remove_style_all(sh.home);
    }
    sh.controls = controls_create(sh.content, sh.landscape, &controls_keepout, &ca);
}

/* ---- screenshot ------------------------------------------------------- */

static int screenshot_save(const char *path)
{
#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    unsigned char *rgb;
    uint32_t y;
    unsigned rc;

    if (!snap) {
        LOG_ERROR("snapshot failed");
        return -1;
    }
    rgb = malloc((size_t)snap->header.w * snap->header.h * 3);
    if (!rgb) {
        lv_draw_buf_destroy(snap);
        return -1;
    }
    /* LVGL RGB888 is stored B,G,R in memory; PNG wants R,G,B. */
    for (y = 0; y < snap->header.h; y++) {
        const unsigned char *src = (const unsigned char *)snap->data +
                                   (size_t)y * snap->header.stride;
        unsigned char *dst = rgb + (size_t)y * snap->header.w * 3;
        uint32_t x;

        for (x = 0; x < snap->header.w; x++) {
            dst[x * 3] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3];
        }
    }
    rc = lodepng_encode24_file(path, rgb, snap->header.w, snap->header.h);
    free(rgb);
    lv_draw_buf_destroy(snap);
    if (rc) {
        LOG_ERROR("PNG encode failed for %s: %u", path, rc);
        return -1;
    }
    LOG_INFO("screenshot written to %s", path);
    return 0;
#else
    (void)path;
    LOG_ERROR("built without LODEPNG/SNAPSHOT, no screenshot");
    return -1;
#endif
}

/* ---- theme ------------------------------------------------------------ */

/* Apply a theme/mode selection live; the shared styles follow through the
 * engine listener. Returns pos_theme_apply's result (-1 = fallback used). */
static int shell_set_theme(const char *theme, const char *mode, char *why, size_t why_len)
{
    int rc = pos_theme_apply(theme, mode, why, why_len);
    cJSON *data;

    if (rc < 0) {
        /* DS §8: fall back, warn, leave the stored value untouched */
        LOG_WARN("theme selection fell back: %s", why);
    } else {
        if (settings_set("theme", pos_theme_current_def()->id) < 0 ||
            settings_set("display_mode", pos_mode_name(pos_theme_current_mode())) < 0) {
            LOG_WARN("cannot persist theme to %s: %s", settings_path(), strerror(errno));
        }
    }
    LOG_INFO("theme %s mode %s", pos_theme_current_def()->id,
             pos_mode_name(pos_theme_current_mode()));
    if (sh.server) {
        data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "theme", pos_theme_current_def()->id);
        cJSON_AddStringToObject(data, "mode", pos_mode_name(pos_theme_current_mode()));
        pocketipc_server_broadcast(sh.server, pocketipc_event("shell.theme", data));
    }
    return rc;
}

/* The Settings app's way in: the same function shell.theme calls, so a
 * selection made on the glass is stored and announced exactly as one made
 * over IPC. */
int pocketos_shell_set_appearance(const char *theme_id, const char *mode_name)
{
    char why[128];

    return shell_set_theme(theme_id, mode_name, why, sizeof(why)) < 0 ? -1 : 0;
}

/* ---- shell.* service (docs/api/shell.md) ------------------------------ */

static const struct pocketos_app *find_app(const char *id)
{
    size_t k;

    for (k = 0; id && k < APP_COUNT; k++) {
        if (strcmp(apps[k]->id, id) == 0) {
            return apps[k];
        }
    }
    return NULL;
}

static void on_shell_request(struct pocketipc_server *s, struct pocketipc_client *c,
                             cJSON *req, void *user)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const char *method = cJSON_IsString(m) ? m->valuestring : "";
    cJSON *result = NULL;

    (void)user;
    if (strcmp(method, "shell.info") == 0) {
        cJSON *list = cJSON_CreateArray();
        cJSON *display = cJSON_CreateObject();
        size_t k;

        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "api_version", 0);
        cJSON_AddStringToObject(result, "version", pocketlog_version());
        cJSON_AddStringToObject(result, "build", pocketlog_build_id());
        for (k = 0; k < APP_COUNT; k++) {
            cJSON *a = cJSON_CreateObject();

            cJSON_AddStringToObject(a, "id", apps[k]->id);
            cJSON_AddStringToObject(a, "name", apps[k]->name);
            cJSON_AddItemToArray(list, a);
        }
        cJSON_AddItemToObject(result, "apps", list);
        cJSON_AddStringToObject(result, "current", sh.app ? sh.app->id : "home");
        cJSON_AddStringToObject(result, "theme", pos_theme_current_def()->id);
        cJSON_AddStringToObject(result, "mode", pos_mode_name(pos_theme_current_mode()));
        cJSON_AddNumberToObject(display, "width", sh.display.geometry.width);
        cJSON_AddNumberToObject(display, "height", sh.display.geometry.height);
        cJSON_AddStringToObject(display, "backend", sh.backend_name);
        rotation_to_json(display);
        cJSON_AddItemToObject(result, "display", display);
        {
            /* The status chrome in force for the current screen (DS §30,
             * §36): the policy, where the content area starts (the top
             * edge: no policy reserves a row), and the cluster as drawn. */
            cJSON *chrome = cJSON_CreateObject();
            cJSON *cluster = cJSON_CreateObject();
            const struct chrome_rect *reserve =
                sh.app && !shell_lock_is_locked() ? &sh.cluster_reserve_app : &sh.cluster_reserve_env;
            lv_area_t a;

            lv_obj_update_layout(sh.cluster);
            lv_obj_get_coords(sh.cluster, &a);
            cJSON_AddStringToObject(chrome, "policy", chrome_name(sh.chrome));
            cJSON_AddNumberToObject(chrome, "content_y", lv_obj_get_y(sh.content));
            cJSON_AddNumberToObject(chrome, "content_h", lv_obj_get_height(sh.content));
            /* shown: the chrome's, but for the lock over a fullscreen app,
             * which shows the cluster (cluster_fit). The box is where it is
             * drawn now; reserve is the widest box it can take on this
             * screen, which the rows under it keep clear of. */
            cJSON_AddBoolToObject(cluster, "shown", sh.cluster_shown);
            cJSON_AddNumberToObject(cluster, "x", a.x1);
            cJSON_AddNumberToObject(cluster, "y", a.y1);
            cJSON_AddNumberToObject(cluster, "w", lv_area_get_width(&a));
            cJSON_AddNumberToObject(cluster, "h", lv_area_get_height(&a));
            cJSON_AddBoolToObject(cluster, "clock", !lv_obj_has_flag(sh.status_clock, LV_OBJ_FLAG_HIDDEN));
            cJSON_AddNumberToObject(cluster, "reserve_x", reserve->x);
            cJSON_AddNumberToObject(cluster, "reserve_w", reserve->w);
            cJSON_AddItemToObject(chrome, "cluster", cluster);
            {
                /* The radio chip as drawn: its box, the box its text gets,
                 * and the line that text needs (tests/chrome_shell_test.sh
                 * holds content_h >= line_h in every state). */
                cJSON *chip = cJSON_CreateObject();
                const lv_font_t *font = lv_obj_get_style_text_font(sh.status_radio, LV_PART_MAIN);

                cJSON_AddNumberToObject(chip, "h", lv_obj_get_height(sh.status_radio));
                cJSON_AddNumberToObject(chip, "content_h", lv_obj_get_content_height(sh.status_radio));
                cJSON_AddNumberToObject(chip, "line_h", lv_font_get_line_height(font));
                cJSON_AddNumberToObject(chip, "y", lv_obj_get_y(sh.status_radio));
                cJSON_AddStringToObject(chip, "text", lv_label_get_text(sh.status_radio));
                cJSON_AddItemToObject(chrome, "chip", chip);
            }
            if (sh.app_root && sh.app_body) {
                /* The app's header and the first pixel of its body, so a
                 * test can hold both clear of the cluster without knowing a
                 * number of the layout. The header is the one this open
                 * built, never "the root's first child": an app that draws
                 * its own top row in landscape (app.h `header`) has none,
                 * and asking LVGL about a child that is not there halted the
                 * shell for good (2026-09-28). */
                lv_obj_t *header = sh.app_header;
                lv_obj_t *body = sh.app_body;
                cJSON *h = cJSON_CreateObject();

                lv_obj_update_layout(sh.app_root);
                cJSON_AddBoolToObject(h, "present", header != NULL);
                if (header) {
                    lv_obj_get_coords(header, &a);
                    cJSON_AddNumberToObject(h, "y", a.y1);
                    cJSON_AddNumberToObject(h, "h", lv_area_get_height(&a));
                    cJSON_AddNumberToObject(h, "content_x2",
                                            a.x2 - lv_obj_get_style_pad_right(header, LV_PART_MAIN));
                    cJSON_AddNumberToObject(h, "pad_left", lv_obj_get_style_pad_left(header, LV_PART_MAIN));
                    cJSON_AddNumberToObject(h, "pad_right", lv_obj_get_style_pad_right(header, LV_PART_MAIN));
                } else {
                    lv_obj_get_coords(body, &a);
                    cJSON_AddNumberToObject(h, "y", a.y1);
                    cJSON_AddNumberToObject(h, "h", 0);
                }
                if (sh.header_hint) {
                    lv_obj_get_coords(sh.header_hint, &a);
                    cJSON_AddNumberToObject(h, "hint_x2", a.x2);
                    cJSON_AddStringToObject(h, "hint", lv_label_get_text(sh.header_hint));
                }
                lv_obj_get_coords(body, &a);
                cJSON_AddNumberToObject(h, "body_y", a.y1);
                cJSON_AddItemToObject(chrome, "header", h);
            }
            cJSON_AddItemToObject(result, "chrome", chrome);
        }
        {
            /* The DOORS environment (DS §31): lock, launcher, Controls, art. */
            cJSON *lock = cJSON_CreateObject();
            cJSON *launcher = cJSON_CreateObject();
            cJSON *art = cJSON_CreateObject();
            struct home_info hi;

            cJSON_AddBoolToObject(lock, "locked", shell_lock_is_locked());
            cJSON_AddBoolToObject(lock, "opening", shell_lock_is_opening());
            cJSON_AddNumberToObject(lock, "engaged", shell_lock_engage_count());
            cJSON_AddNumberToObject(lock, "opened", shell_lock_open_count());
            cJSON_AddItemToObject(result, "lock", lock);
            home_info(&hi);
            cJSON_AddNumberToObject(launcher, "groups", hi.groups);
            cJSON_AddNumberToObject(launcher, "apps", hi.apps);
            cJSON_AddNumberToObject(launcher, "icons_art", hi.icons_art);
            cJSON_AddNumberToObject(launcher, "icons_fallback", hi.icons_fallback);
            cJSON_AddNumberToObject(launcher, "cell_width", hi.cell_w);
            cJSON_AddBoolToObject(launcher, "scrolls", hi.scrolls);
            cJSON_AddBoolToObject(launcher, "controls", controls_visible());
            {
                /* Where each app's cell is on the screen, for the bench and
                 * the tests: a tap there opens it. */
                cJSON *cells = cJSON_CreateArray();
                size_t k;

                for (k = 0; k < APP_COUNT; k++) {
                    lv_area_t a;

                    if (home_cell_area(apps[k]->id, &a)) {
                        cJSON *c = cJSON_CreateObject();

                        cJSON_AddStringToObject(c, "id", apps[k]->id);
                        cJSON_AddNumberToObject(c, "x", a.x1);
                        cJSON_AddNumberToObject(c, "y", a.y1);
                        cJSON_AddNumberToObject(c, "w", lv_area_get_width(&a));
                        cJSON_AddNumberToObject(c, "h", lv_area_get_height(&a));
                        cJSON_AddItemToArray(cells, c);
                    }
                }
                cJSON_AddItemToObject(launcher, "cells", cells);
            }
            {
                /* The folders (app groups): each one's cell on the
                 * launcher's page while it shows, and its size; which one
                 * is open; and where the keys are. */
                cJSON *folders = cJSON_CreateArray();
                bool shown = false;
                const char *focus = home_focus_id(&shown);
                int f;

                for (f = HOME_FOLDER_NONE + 1; f < HOME_FOLDER_COUNT; f++) {
                    const struct home_folder_def *def = home_folder_get((enum home_folder)f);
                    cJSON *o = cJSON_CreateObject();
                    lv_area_t a;

                    cJSON_AddStringToObject(o, "id", def->id);
                    cJSON_AddStringToObject(o, "name", def->name);
                    cJSON_AddNumberToObject(o, "apps", home_folder_size(def->id));
                    if (home_folder_area(def->id, &a)) {
                        cJSON_AddNumberToObject(o, "x", a.x1);
                        cJSON_AddNumberToObject(o, "y", a.y1);
                        cJSON_AddNumberToObject(o, "w", lv_area_get_width(&a));
                        cJSON_AddNumberToObject(o, "h", lv_area_get_height(&a));
                    }
                    cJSON_AddItemToArray(folders, o);
                }
                cJSON_AddItemToObject(launcher, "folders", folders);
                if (home_folder_current()) {
                    cJSON_AddStringToObject(launcher, "folder", home_folder_current());
                } else {
                    cJSON_AddNullToObject(launcher, "folder");
                }
                cJSON_AddNumberToObject(launcher, "home_cells", hi.cells);
                cJSON_AddNumberToObject(launcher, "folder_cells", hi.folders);
                if (focus) {
                    cJSON_AddStringToObject(launcher, "focus", focus);
                } else {
                    cJSON_AddNullToObject(launcher, "focus");
                }
                cJSON_AddBoolToObject(launcher, "focus_shown", shown);
                cJSON_AddBoolToObject(launcher, "keys",
                                      lv_obj_get_group(sh.home) != NULL && pos_input_focused() == sh.home);
            }
            {
                /* The time and the date, which keep clear of the cluster. */
                lv_area_t t;
                lv_area_t d;

                if (home_header_area(&t, &d)) {
                    const lv_area_t *both[] = { &t, &d };
                    const char *const names[] = { "time", "date" };
                    size_t k;

                    for (k = 0; k < 2; k++) {
                        cJSON *hd = cJSON_CreateObject();

                        cJSON_AddNumberToObject(hd, "x", both[k]->x1);
                        cJSON_AddNumberToObject(hd, "y", both[k]->y1);
                        cJSON_AddNumberToObject(hd, "w", lv_area_get_width(both[k]));
                        cJSON_AddNumberToObject(hd, "h", lv_area_get_height(both[k]));
                        cJSON_AddItemToObject(launcher, names[k], hd);
                    }
                }
            }
            cJSON_AddItemToObject(result, "launcher", launcher);
            cJSON_AddStringToObject(art, "dir", art_dir());
            cJSON_AddBoolToObject(art, "background", sh.home_bg != NULL);
            cJSON_AddNumberToObject(art, "files_read", art_loads());
            cJSON_AddNumberToObject(art, "bytes_held", (double)art_bytes_held());
            cJSON_AddItemToObject(result, "art", art);
        }
    } else if (strcmp(method, "shell.open") == 0) {
        const cJSON *aid = params ? cJSON_GetObjectItemCaseSensitive(params, "id") : NULL;
        const struct pocketos_app *app = find_app(cJSON_IsString(aid) ? aid->valuestring : NULL);

        if (!app) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "unknown app id"));
            return;
        }
        /* Opening an app from outside is somebody at the bench or a script
         * acting for the owner: it opens the device too, without the door
         * sequence (shell_lock.h - the lock is not security). */
        shell_lock_open(false, "shell.open");
        app_open(app);
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "current", app->id);
    } else if (strcmp(method, "shell.home") == 0) {
        shell_lock_open(false, "shell.home");
        pocketos_shell_go_home();
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "current", "home");
    } else if (strcmp(method, "shell.folder") == 0) {
        /* A launcher folder (home.h): {"id": "games"} opens it at home,
         * {"id": ""} goes back to the launcher's own page. For the bench and
         * the tests; a person taps the folder's cell. */
        const cJSON *fid = params ? cJSON_GetObjectItemCaseSensitive(params, "id") : NULL;

        if (!cJSON_IsString(fid)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "id must be a folder id or \"\""));
            return;
        }
        if (fid->valuestring[0]) {
            if (home_folder_size(fid->valuestring) <= 0) {
                pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                      "no such folder, or nothing in it"));
                return;
            }
            shell_lock_open(false, "shell.folder");
            if (sh.app || controls_visible()) {
                pocketos_shell_go_home();
            }
            home_folder_open(fid->valuestring);
        } else {
            home_folder_close();
        }
        result = cJSON_CreateObject();
        if (home_folder_current()) {
            cJSON_AddStringToObject(result, "folder", home_folder_current());
        } else {
            cJSON_AddNullToObject(result, "folder");
        }
    } else if (strcmp(method, "shell.lock") == 0) {
        shell_lock_engage("shell.lock");
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "locked", shell_lock_is_locked());
    } else if (strcmp(method, "shell.unlock") == 0) {
        const cJSON *an = params ? cJSON_GetObjectItemCaseSensitive(params, "animate") : NULL;

        shell_lock_open(cJSON_IsTrue(an), "shell.unlock");
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "locked", shell_lock_is_locked());
    } else if (strcmp(method, "shell.controls") == 0) {
        const cJSON *show = params ? cJSON_GetObjectItemCaseSensitive(params, "show") : NULL;

        if (show && !cJSON_IsBool(show)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "show must be true or false"));
            return;
        }
        if (cJSON_IsFalse(show)) {
            if (controls_visible()) {
                controls_close();
            }
        } else {
            shell_lock_open(false, "shell.controls");
            controls_open();
        }
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "controls", controls_visible());
    } else if (strcmp(method, "shell.screenshot") == 0) {
        const cJSON *p = params ? cJSON_GetObjectItemCaseSensitive(params, "path") : NULL;

        if (!cJSON_IsString(p)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "path required"));
            return;
        }
        lv_refr_now(NULL);
        if (screenshot_save(p->valuestring) < 0) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_BACKEND,
                                                                  "screenshot failed"));
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "path", p->valuestring);
    } else if (strcmp(method, "shell.theme") == 0) {
        const cJSON *th = params ? cJSON_GetObjectItemCaseSensitive(params, "theme") : NULL;
        const cJSON *md = params ? cJSON_GetObjectItemCaseSensitive(params, "mode") : NULL;
        char why[128];
        int fallback;

        if (!cJSON_IsString(th) && !cJSON_IsString(md)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "theme or mode required"));
            return;
        }
        fallback = shell_set_theme(cJSON_IsString(th) ? th->valuestring : NULL,
                                   cJSON_IsString(md) ? md->valuestring : NULL, why, sizeof(why)) < 0;
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "theme", pos_theme_current_def()->id);
        cJSON_AddStringToObject(result, "mode", pos_mode_name(pos_theme_current_mode()));
        cJSON_AddBoolToObject(result, "fallback", fallback);
        cJSON_AddStringToObject(result, "reason", why);
    } else if (strcmp(method, "shell.rotation") == 0) {
        const cJSON *md = params ? cJSON_GetObjectItemCaseSensitive(params, "mode") : NULL;
        enum orientation_mode mode;

        if (md) {
            if (!cJSON_IsString(md) || orientation_mode_parse(md->valuestring, &mode) < 0) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                    id, POCKETIPC_ERR_INVALID_PARAMS, "mode must be automatic, portrait or landscape"));
                return;
            }
            if (pocketos_shell_set_rotation_mode((enum pocketos_rotation_mode)mode) < 0) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                    id, POCKETIPC_ERR_BACKEND, "rotation mode could not be stored"));
                return;
            }
        }
        result = cJSON_CreateObject();
        rotation_to_json(result);
    } else if (strcmp(method, "shell.brightness") == 0) {
        const cJSON *p = params ? cJSON_GetObjectItemCaseSensitive(params, "percent") : NULL;
        int pct;

        if (p) {
            /* Exactly an integer in range: a method that changes the panel
             * does not round or clamp a request it was not given. */
            if (!cJSON_IsNumber(p) || !(p->valuedouble >= BRIGHTNESS_MIN_PCT) ||
                !(p->valuedouble <= BRIGHTNESS_MAX_PCT) ||
                p->valuedouble != (double)(int)p->valuedouble) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                    id, POCKETIPC_ERR_INVALID_PARAMS, "percent must be an integer 10..100"));
                return;
            }
            if (!sh.brightness.supported) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                    id, POCKETIPC_ERR_UNSUPPORTED, "no brightness control on this display"));
                return;
            }
            if (pocketos_shell_brightness_set((int)p->valuedouble) < 0) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                    id, POCKETIPC_ERR_BACKEND, "brightness could not be applied"));
                return;
            }
        }
        pct = pocketos_shell_brightness_get();
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "supported", sh.brightness.supported);
        if (pct >= 0) {
            cJSON_AddNumberToObject(result, "percent", pct);
        } else {
            cJSON_AddNullToObject(result, "percent");
        }
        cJSON_AddNumberToObject(result, "min", BRIGHTNESS_MIN_PCT);
        cJSON_AddNumberToObject(result, "max", BRIGHTNESS_MAX_PCT);
        cJSON_AddNumberToObject(result, "step", BRIGHTNESS_STEP_PCT);
        if (sh.brightness.supported) {
            cJSON_AddStringToObject(result, "device", sh.brightness.name);
        } else {
            cJSON_AddNullToObject(result, "device");
        }
    } else if (strcmp(method, "shell.volume") == 0) {
        const cJSON *p = params ? cJSON_GetObjectItemCaseSensitive(params, "percent") : NULL;
        const cJSON *m = params ? cJSON_GetObjectItemCaseSensitive(params, "muted") : NULL;

        /* Both checked before either is applied: a request is taken whole or
         * not at all. */
        if (p && (!cJSON_IsNumber(p) || !(p->valuedouble >= VOLUME_MIN_PCT) ||
                  !(p->valuedouble <= VOLUME_MAX_PCT) ||
                  p->valuedouble != (double)(int)p->valuedouble ||
                  (int)p->valuedouble % VOLUME_STEP_PCT != 0)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(
                id, POCKETIPC_ERR_INVALID_PARAMS, "percent must be 10..100 in steps of 10"));
            return;
        }
        if (m && !cJSON_IsBool(m)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(
                id, POCKETIPC_ERR_INVALID_PARAMS, "muted must be true or false"));
            return;
        }
        if ((p && pocketos_shell_volume_set((int)p->valuedouble) < 0) ||
            (m && pocketos_shell_volume_set_muted(cJSON_IsTrue(m) ? 1 : 0) < 0)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(
                id, POCKETIPC_ERR_BACKEND, "the volume could not be stored"));
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "available", pocketos_shell_volume_available());
        cJSON_AddNumberToObject(result, "percent", sh.volume.percent);
        cJSON_AddBoolToObject(result, "muted", sh.volume.muted);
        cJSON_AddNumberToObject(result, "effective", volume_effective(&sh.volume));
        cJSON_AddNumberToObject(result, "min", VOLUME_MIN_PCT);
        cJSON_AddNumberToObject(result, "max", VOLUME_MAX_PCT);
        cJSON_AddNumberToObject(result, "step", VOLUME_STEP_PCT);
    } else if (strcmp(method, "shell.subscribe") == 0) {
        pocketipc_client_set_subscribed(c, true);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", true);
    } else if (strcmp(method, "shell.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, false);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", false);
    } else {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_UNKNOWN_METHOD,
                                                              "unknown method"));
        return;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
}

/* ---- main ------------------------------------------------------------- */

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    /* The only place the clock runtime is stepped. Once a second is plenty
     * for an alarm, whose resolution is a minute, and for a countdown, whose
     * last second is the only one anybody watches - and one stepper means an
     * alarm cannot fire twice because two callers both advanced it
     * (clock_runtime.h). */
    clock_runtime_step();
    status_update();
    controls_tick();
    if (sh.app && sh.app->tick) {
        sh.app->tick(sh.app_priv);
    }
    if (sh.screenshot_pending) {
        /* first tick after start: let the layout settle, then capture */
        sh.screenshot_pending = 0;
        lv_refr_now(NULL);
        screenshot_save(sh.screenshot_path);
        lv_timer_set_period(sh.tick, 1000);
    }
}

/* The whole of applying an orientation: hand the same binary the same
 * arguments, in the same process. execv keeps the pid, so pos-supervise sees
 * no exit and counts no restart, and the settings the next start reads are
 * already written. Everything this image opened is closed first - the DRM
 * device above all, which the next one has to open as master - because only
 * a close-on-exec flag would do it otherwise, and LVGL's fd is not ours to
 * flag. If the exec fails the shell exits instead, and the supervisor starts
 * it again: one counted restart, same orientation, still no reboot.
 *
 * The lock goes across with it. A rotation can come from the keyboard base
 * being attached or removed while the device lies locked in a pocket, and
 * the lock is there to keep the pocket from opening apps; a restart that came
 * back open would defeat it. `locked` is the lock as the loop left it - a
 * door that was still opening counts as locked, so the owner swipes again
 * rather than finding it open. */
static void restart_in_place(bool locked)
{
    char exe[PATH_MAX];
    ssize_t n;
    int fd;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
    } else {
        snprintf(exe, sizeof(exe), "%s", shell_argv[0]);
    }
    LOG_INFO("display: restarting in place (%s) to open the display at rotation %d", exe,
             pos_rotation_degrees(shell_display_next_rotation(&sh.display)));
    pocketlog_close();
    for (fd = 3; fd < 64; fd++) {
        close(fd);
    }
    setenv(RESUME_ENV, locked ? RESUME_LOCKED : RESUME_OPEN, 1);
    if (home_folder_current()) {
        setenv(RESUME_FOLDER_ENV, home_folder_current(), 1);
    } else {
        unsetenv(RESUME_FOLDER_ENV);
    }
    execv(exe, shell_argv);
    pocketlog_init("shell");
    LOG_ERROR("display: cannot re-execute %s (%s); exiting so the supervisor starts the shell again", exe,
              strerror(errno));
    pocketlog_close();
}

int main(int argc, char **argv)
{
    const char *open_id = NULL;
    const char *theme_arg = NULL;
    const char *mode_arg = NULL;
    const char *rotation_arg = NULL;
    bool no_lock = false;
    bool start_controls = false;
    bool resumed = false;
    bool resumed_locked = false;
    bool restart_locked = false;
    const char *resume;
    char resume_folder[32] = "";
    long exit_after_ms = -1;
    int loaded;
    lv_display_t *disp;
    lv_obj_t *screen;
    uint32_t started;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
            open_id = argv[++i];
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            sh.screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--exit-after-ms") == 0 && i + 1 < argc) {
            exit_after_ms = atol(argv[++i]);
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme_arg = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode_arg = argv[++i];
        } else if (strcmp(argv[i], "--rotation") == 0 && i + 1 < argc) {
            rotation_arg = argv[++i];
        } else if (strcmp(argv[i], "--no-lock") == 0) {
            no_lock = true;
        } else if (strcmp(argv[i], "--controls") == 0) {
            start_controls = true;
            no_lock = true;
        } else {
            fprintf(stderr, "usage: pocketos-shell [--open APP] [--screenshot F.png] [--exit-after-ms N]"
                            " [--theme ID] [--mode normal|outdoor|night]"
                            " [--rotation automatic|portrait|landscape] [--no-lock] [--controls]\n");
            return 2;
        }
    }

    shell_argv = argv;
    /* A start that is this shell re-executing itself for a rotation carries
     * a mark in its environment (restart_in_place). It is the same session
     * turning round, so the lock comes back as the mark says it was - no
     * lock where it was open, the lock where it was locked. The mark is read
     * before it is spent (unsetenv may free the string), and it is spent here,
     * so whatever starts after this one - a supervisor restart after a
     * crash - locks as a cold start does. */
    resume = getenv(RESUME_ENV);
    if (resume) {
        resumed_locked = strcmp(resume, RESUME_LOCKED) == 0;
        resumed = resumed_locked || strcmp(resume, RESUME_OPEN) == 0;
    }
    pocketlog_init("shell");
    pocketlog_install_crash_handler();
    if (resume && !resumed) {
        LOG_WARN("lock: resume mark '%.16s' not understood; starting as a cold start", resume);
    }
    unsetenv(RESUME_ENV);
    /* The folder the launcher had open, taken the same way and only from a
     * continuation: a cold start opens on the launcher's own page. */
    {
        const char *rf = getenv(RESUME_FOLDER_ENV);

        if (rf && resumed) {
            snprintf(resume_folder, sizeof(resume_folder), "%s", rf);
        }
        unsetenv(RESUME_FOLDER_ENV);
    }
    /* The settings come first: the orientation is decided from them before
     * the display exists, because the display is rotated when it is opened.
     * The keyboard is probed in the same breath and for the same reason -
     * Automatic asks whether one is attached, and the answer has to be there
     * before the display is opened, not a second later. The probe needs no
     * LVGL: it takes the bus, asks the controller and gives an answer. */
    loaded = settings_init();
    shell_kbd_probe();
    shell_display_resolve(rotation_arg, &sh.display);
    kbd_presence_set_listener(on_keyboard_presence, NULL);
    lv_init();
    disp = pocketos_platform_init(&sh.display.panel, &sh.display.geometry);
    if (!disp) {
        LOG_ERROR("display init failed");
        return 1;
    }
    /* From here on the display, the touch transform and every layout use this
     * one geometry, as the backend confirmed it. */
    pocketui_set_display_geometry(&sh.display.geometry);
    {
        const struct pos_display_geometry *g = &sh.display.geometry;
        struct pos_insets bar = pos_display_bar_insets(g, POS_EDGE_TOP);

        LOG_INFO("display: %dx%d at rotation %d, corners %d,%d,%d,%d, status bar insets %d/%d",
                 (int)g->width, (int)g->height, pos_rotation_degrees(g->rotation), (int)g->corners.top_left,
                 (int)g->corners.top_right, (int)g->corners.bottom_right, (int)g->corners.bottom_left,
                 (int)bar.left, (int)bar.right);
    }
    sh.backend_name = POCKETOS_DISPLAY_NAME;
    /* After the display, so the stream's own device is created with one to
     * belong to; LVGL warns otherwise. The shell owns the stream and the
     * focus group, and adopts whatever key source the backend offers - an
     * app only ever sees a focused field (DS §17.4). */
    pos_input_init();
    if (pocketos_platform_keyboard()) {
        if (pos_input_add_source(pocketos_platform_keyboard())) {
            LOG_INFO("host keyboard adopted as an input source");
        } else {
            LOG_WARN("host keyboard not adopted; typing will not reach fields");
        }
    }
    /* The physical keyboard is a source of the same stream, pushing rather
     * than being adopted (DS §17.4, and KNOWN_ISSUES on LV_KEY_NEXT). No
     * keyboard attached is a normal outcome and says so once. This starts the
     * polling for a keyboard the probe found, and the watch that notices one
     * attached or removed later. */
    shell_kbd_create();
    pocketui_init();
    {
        /* Appearance never blocks boot: any failure here logs and falls back. */
        const char *theme = theme_arg ? theme_arg : settings_get("theme", NULL);
        const char *mode = mode_arg ? mode_arg : settings_get("display_mode", NULL);
        char why[128];

        if (loaded < 0) {
            LOG_WARN("settings file %s unreadable, using defaults", settings_path());
        }
        if ((theme || mode) && pos_theme_apply(theme, mode, why, sizeof(why)) < 0) {
            LOG_WARN("stored theme rejected, using fallback: %s", why);
        }
        LOG_INFO("appearance: theme %s mode %s (settings %s)", pos_theme_current_def()->id,
                 pos_mode_name(pos_theme_current_mode()),
                 loaded == 0 ? "loaded" : loaded == 1 ? "absent" : "unreadable");
    }
    /* After the settings are loaded and before anything is built: a caret
     * created now must already know whether it may blink (DS §12, §17.1). */
    pocketui_set_reduced_motion(pocketos_shell_reduced_motion() != 0);
    /* Also before the first frame, so a dimmed panel does not flash at the
     * boot level while the launcher draws. */
    brightness_restore();
    volume_restore();
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    sh.landscape = is_landscape(sh.display.geometry.rotation);
    /* The home photograph first, so everything else is drawn over it. Kept
     * for the whole run: the launcher is where every app returns to. */
    sh.home_bg = art_load_background("home", sh.landscape);
    sh.backdrop = lv_image_create(screen);
    pos_style_add(sh.backdrop, POS_STYLE_ENV_BG, 0);
    lv_obj_set_pos(sh.backdrop, 0, 0);
    lv_obj_remove_flag(sh.backdrop, LV_OBJ_FLAG_CLICKABLE);
    if (sh.home_bg) {
        lv_image_set_src(sh.backdrop, sh.home_bg);
    } else {
        lv_obj_add_flag(sh.backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    status_cluster_create(screen);
    status_chip_fit();

    sh.content = lv_obj_create(screen);
    lv_obj_remove_style_all(sh.content);
    lv_obj_set_width(sh.content, LV_PCT(100));
    lv_obj_align(sh.content, LV_ALIGN_TOP_MID, 0, 0);
    /* The launcher's chrome, applied before the launcher is built (DS §30,
     * §36); an app opened below resolves its own. This sets the content's
     * top and height, and the box the launcher keeps clear of, so nothing
     * above sized it. */
    chrome_apply(chrome_resolve(POCKETOS_CHROME_DEFAULT, is_landscape(sh.display.geometry.rotation), true),
                 "home");
    {
        uint32_t t0 = lv_tick_get();

        home_build();
        LOG_INFO("launcher: built with Controls in %u ms", (unsigned)lv_tick_elaps(t0));
    }
    /* Home is what shows until something covers it: the keys are its. */
    home_keys_attach();

    /* One keyboard for the whole shell, built hidden and never rebuilt. It
     * sits on the screen rather than inside an app, so leaving an app cannot
     * take it with it and no app can hold a pointer to it (DS §17.4). */
    sh.keyboard = pos_keyboard_create(screen);
    if (sh.keyboard) {
        pos_keyboard_set_done_cb(sh.keyboard, on_keyboard_done, NULL);
    } else {
        LOG_WARN("touch keyboard unavailable; text entry will not work");
    }

    /* The lock: over the apps, the launcher and the keyboard, under the
     * status cluster and the alarm alert (shell_lock.h). The cluster is
     * brought over all of them here: it lies over the content area's top
     * row rather than above it. */
    {
        static const struct shell_lock_hooks hooks = { on_lock_engaged, on_lock_opened,
                                                        on_lock_revealing };

        uint32_t t0 = lv_tick_get();

        shell_lock_create(screen, sh.landscape, &hooks);
        lv_obj_move_foreground(sh.cluster);
        LOG_INFO("lock: built in %u ms", (unsigned)lv_tick_elaps(t0));
#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS
        {
            const char *hold = getenv("POCKETOS_TEST_LOCK_HOLD");

            shell_lock_test_hold_at_door(hold && strcmp(hold, "door") == 0);
        }
#endif
    }

    /* The alarms, and the one alert that shows them. Both belong to the
     * shell and not to PocketClock: an alarm has to ring with the app shut,
     * and there is no background app to ring it (ADR-002, clock_runtime.h).
     * Built after the keyboard so it sits above it. */
    shell_alarm_create(screen);
    switch (clock_runtime_init(shell_alarm_sync)) {
    case 0:
        LOG_INFO("clock: %d alarm(s) loaded",
                 clock_alarm_count(clock_runtime_engine()));
        break;
    case 1:
        break; /* nothing stored yet */
    default:
        LOG_WARN("clock: stored alarms could not be read; starting with none");
        break;
    }
    /* Whether this start is a continuation. Worth a line either way: the
     * handoff is the only evidence that a rotation kept a running stopwatch
     * or countdown, which is otherwise invisible in a log. */
    switch (clock_runtime_handoff_result()) {
    case 0:
        LOG_INFO("clock: runtime state taken from the shell before this one");
        break;
    case 1:
        break; /* the first shell of this boot, or the last one crashed */
    default:
        LOG_WARN("clock: the handoff from the shell before this one could not "
                 "be used; the stored alarms stand on their own");
        break;
    }

    if (open_id) {
        size_t k;

        for (k = 0; k < APP_COUNT; k++) {
            if (strcmp(apps[k]->id, open_id) == 0) {
                app_open(apps[k]);
            }
        }
        if (!sh.app) {
            LOG_WARN("unknown app %s", open_id);
        }
    }
    if (start_controls && !sh.app) {
        controls_open();
    }
    /* Back in the folder that was open when the shell turned round. */
    if (resume_folder[0] && !sh.app && !controls_visible()) {
        if (home_folder_open(resume_folder)) {
            LOG_INFO("launcher: folder %s open again after the restart", resume_folder);
        }
    }
    /* A continuation decides before the arguments do: the restart keeps the
     * argv, so a shell started with --no-lock or --open and locked since is
     * locked again after it turns, not opened by its own command line. */
    if (resumed_locked) {
        shell_lock_engage("rotation restart, locked before it");
    } else if (no_lock || open_id || resumed || strcmp(settings_get(LOCK_SETTING, "1"), "0") == 0) {
        LOG_INFO("lock: not engaged at start (%s)", resumed   ? "rotation restart"
                                                    : no_lock ? "--no-lock"
                                                    : open_id ? "--open"
                                                              : LOCK_SETTING "=0");
    } else {
        shell_lock_engage("start");
    }
    environment_apply();
    sh.server = pocketipc_server_new("shell", on_shell_request, NULL);
    if (sh.server) {
        LOG_INFO("shell.* listening on %s", pocketipc_server_path(sh.server));
    } else {
        LOG_WARN("shell.* service unavailable: %s", strerror(errno));
    }
    status_update();
    sh.screenshot_pending = sh.screenshot_path != NULL;
    sh.tick = lv_timer_create(on_tick, 1000, NULL);
    lv_timer_set_period(sh.tick, sh.screenshot_pending ? 300 : 1000);

    /* After the display backend, so this wins over the handlers SDL installs
     * for itself in the simulator. */
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    started = lv_tick_get();
    for (;;) {
        uint32_t wait = lv_timer_handler();

        if (stop_requested) {
            LOG_INFO("stopping on signal");
            break;
        }
        if (restart_pending) {
            restart_locked = shell_lock_is_locked();
            break;
        }
        if (exit_after_ms >= 0 && (long)(lv_tick_get() - started) >= exit_after_ms) {
            break;
        }
        if (sh.server) {
            pocketipc_server_poll(sh.server, 0);
        }
        pocketos_platform_sleep_ms(wait > 20 ? 20 : wait);
    }
    /* The open app is closed the ordinary way, so it persists what it holds
     * exactly as it would on any other exit. */
    app_close();
    /* And then the one thing the app cannot persist, because it is not the
     * app's: a running stopwatch, a running countdown, a snooze. They are
     * elapsed time on the monotonic clock, which an exec does not disturb -
     * so when this exit is a rotation restarting the shell in place, the
     * clock the owner was watching carries on across it instead of starting
     * again from an empty engine (clock_runtime.h). The handoff is
     * boot-scoped, so a power cycle still ends all three, as it must. */
    switch (clock_runtime_handoff_save()) {
    case 0:
        LOG_INFO("clock: runtime state handed to the next shell");
        break;
    case 1:
        break; /* an idle clock: nothing to hand on, and no file left behind */
    default:
        LOG_WARN("clock: runtime state could not be handed on (%s); a running "
                 "stopwatch, countdown or snooze ends here", strerror(errno));
        break;
    }
    /* Before anything else on the way out: this is what puts the keyboard's
     * pin mux back the way it was found. */
    shell_kbd_destroy();
    pocketipc_server_free(sh.server);
    shell_ipc_shutdown();
    if (restart_pending) {
        restart_in_place(restart_locked); /* returns only when the exec failed */
        return 1;
    }
    pocketlog_close();
    return 0;
}
