/*
 * PocketOS shell: status bar, launcher and in-process app host (ADR-002).
 *
 * Options:
 *   --open <app-id>          open an app at start
 *   --screenshot <file.png>  save the screen after the first tick
 *   --exit-after-ms <n>      quit after n milliseconds (headless testing)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "app.h"
#include "platform.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "settings.h"
#include "shell_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef POCKETOS_DISPLAY_NAME
#define POCKETOS_DISPLAY_NAME "unknown"
#endif

/* How long the once-a-second status poll waits for radiod. Deliberately less
 * than the tick that drives it, so a wedged service costs at most one frame
 * and never accumulates. Only this poll has a deadline in v0.0.3; app calls
 * and radio.send still wait (docs/api/pocketipc.md, Request deadlines). */
#ifndef STATUS_POLL_TIMEOUT_MS
#define STATUS_POLL_TIMEOUT_MS 200
#endif

#if LV_USE_LODEPNG && LV_USE_SNAPSHOT
#include "src/libs/lodepng/lodepng.h"
#endif

extern const struct pocketos_app app_radio;
extern const struct pocketos_app app_system;
extern const struct pocketos_app app_fleet;
extern const struct pocketos_app app_radar;

static const struct pocketos_app *apps[] = { &app_radio, &app_system, &app_fleet,
                                            &app_radar };
#define APP_COUNT (sizeof(apps) / sizeof(apps[0]))

struct shell {
    lv_obj_t *status_bar;
    lv_obj_t *status_clock;
    lv_obj_t *status_radio;
    lv_obj_t *status_hint;
    lv_obj_t *content;      /* below the status bar */
    lv_obj_t *home;         /* launcher */
    lv_obj_t *app_root;     /* current app container or NULL */
    const struct pocketos_app *app;
    void *app_priv;
    lv_timer_t *tick;
    const char *screenshot_path;
    int screenshot_pending;
    struct pocketipc_server *server;
    const char *backend_name;
};

static struct shell sh;
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

static void status_update(void)
{
    time_t now = time(NULL);
    struct tm tm;
    char buf[16];
    char err[96];
    cJSON *st;

    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    lv_label_set_text(sh.status_clock, buf);

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
        radio_chip_set(POS_STYLE_CHIP_NA, "--");
    }
}

static void status_bar_create(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_t *title;

    lv_obj_remove_style_all(bar);
    pos_style_add(bar, POS_STYLE_STATUS_BAR, 0);
    lv_obj_set_size(bar, LV_PCT(100), POCKETUI_STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    title = pocketui_label(bar, "POCKETOS", POS_STYLE_CAPTION);
    (void)title;

    sh.status_hint = pocketui_label(bar, "", POS_STYLE_CAPTION);

    sh.status_radio = lv_label_create(bar);
    pos_style_add(sh.status_radio, POS_STYLE_CHIP, 0);
    /* glyphs come from the symbol font role until the DS icon set exists */
    pos_style_add(sh.status_radio, POS_STYLE_SYMBOL, 0);
    radio_chip_set(POS_STYLE_CHIP_NA, "?");

    sh.status_clock = pocketui_label(bar, "--:--", POS_STYLE_CAPTION);
    sh.status_bar = bar;
}

void pocketos_shell_set_status_hint(const char *text)
{
    lv_label_set_text(sh.status_hint, text ? text : "");
}

int pocketos_shell_reduced_motion(void)
{
    const char *v = settings_get("reduced_motion", "0");

    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
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
    if (sh.app->destroy) {
        sh.app->destroy(sh.app_priv);
    }
    sh.app = NULL;
    sh.app_priv = NULL;
    lv_obj_delete(sh.app_root);
    sh.app_root = NULL;
    pocketos_shell_set_status_hint("");
    announce_current();
}

void pocketos_shell_go_home(void)
{
    app_close();
    lv_obj_clear_flag(sh.home, LV_OBJ_FLAG_HIDDEN);
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

    sh.app_root = lv_obj_create(sh.content);
    lv_obj_remove_style_all(sh.app_root);
    lv_obj_set_size(sh.app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(sh.app_root, LV_FLEX_FLOW_COLUMN);

    header = lv_obj_create(sh.app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(header, 16, 0);

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

    body = lv_obj_create(sh.app_root);
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
    sh.app_priv = app->create(body);
    LOG_INFO("open app %s", app->id);
    if (app->tick) {
        app->tick(sh.app_priv);
    }
    announce_current();
}

static void on_tile(lv_event_t *e)
{
    app_open(lv_event_get_user_data(e));
}

static void home_create(void)
{
    size_t i;

    sh.home = lv_obj_create(sh.content);
    lv_obj_remove_style_all(sh.home);
    lv_obj_set_size(sh.home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(sh.home, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_gap(sh.home, POCKETUI_PAD, 0);
    lv_obj_set_layout(sh.home, LV_LAYOUT_GRID);
    {
        static const int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
        static const int32_t rows[] = { LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT,
                                        LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST };
        lv_obj_set_grid_dsc_array(sh.home, cols, rows);
    }
    for (i = 0; i < APP_COUNT; i++) {
        lv_obj_t *tile = pocketui_tile(sh.home, apps[i]->icon, apps[i]->name, on_tile,
                                       (void *)apps[i]);

        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, (uint8_t)(i % 2), 1,
                             LV_GRID_ALIGN_START, (uint8_t)(i / 2), 1);
    }
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
        cJSON_AddNumberToObject(display, "width", POCKETOS_PANEL_W);
        cJSON_AddNumberToObject(display, "height", POCKETOS_PANEL_H);
        cJSON_AddStringToObject(display, "backend", sh.backend_name);
        cJSON_AddItemToObject(result, "display", display);
    } else if (strcmp(method, "shell.open") == 0) {
        const cJSON *aid = params ? cJSON_GetObjectItemCaseSensitive(params, "id") : NULL;
        const struct pocketos_app *app = find_app(cJSON_IsString(aid) ? aid->valuestring : NULL);

        if (!app) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "unknown app id"));
            return;
        }
        app_open(app);
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "current", app->id);
    } else if (strcmp(method, "shell.home") == 0) {
        pocketos_shell_go_home();
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "current", "home");
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
    status_update();
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

int main(int argc, char **argv)
{
    const char *open_id = NULL;
    const char *theme_arg = NULL;
    const char *mode_arg = NULL;
    long exit_after_ms = -1;
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
        } else {
            fprintf(stderr, "usage: pocketos-shell [--open APP] [--screenshot F.png] [--exit-after-ms N]"
                            " [--theme ID] [--mode normal|outdoor|night]\n");
            return 2;
        }
    }

    pocketlog_init("shell");
    pocketlog_install_crash_handler();
    lv_init();
    disp = pocketos_platform_init();
    if (!disp) {
        LOG_ERROR("display init failed");
        return 1;
    }
    sh.backend_name = POCKETOS_DISPLAY_NAME;
    pocketui_init();
    {
        /* Appearance never blocks boot: any failure here logs and falls back. */
        int loaded = settings_init();
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
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    status_bar_create(screen);

    sh.content = lv_obj_create(screen);
    lv_obj_remove_style_all(sh.content);
    lv_obj_set_size(sh.content, LV_PCT(100), POCKETOS_PANEL_H - POCKETUI_STATUS_BAR_H);
    lv_obj_align(sh.content, LV_ALIGN_TOP_MID, 0, POCKETUI_STATUS_BAR_H);
    home_create();

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

    started = lv_tick_get();
    for (;;) {
        uint32_t wait = lv_timer_handler();

        if (exit_after_ms >= 0 && (long)(lv_tick_get() - started) >= exit_after_ms) {
            break;
        }
        if (sh.server) {
            pocketipc_server_poll(sh.server, 0);
        }
        pocketos_platform_sleep_ms(wait > 20 ? 20 : wait);
    }
    app_close();
    pocketipc_server_free(sh.server);
    shell_ipc_shutdown();
    pocketlog_close();
    return 0;
}
