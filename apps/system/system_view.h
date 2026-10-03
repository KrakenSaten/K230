/*
 * System Status: everything the screen decides, with no LVGL in it.
 *
 * The app (apps/system/system_app.c) owns pixels; this owns the answers.
 * It turns system.info and system.status into the exact strings and states
 * the panels show, holds the confirm/terminal state machine behind the two
 * destructive actions, and is the only place that decides what "unknown"
 * looks like. That split is what lets the host suite test the parts that go
 * wrong - a null read as zero, a stale poll blanking the screen, an action
 * fired before it was confirmed - without an LVGL build.
 *
 * It never reads /proc, /sys or /run. Everything comes in as cJSON that the
 * app fetched over pocketipc.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SYSTEM_VIEW_H
#define POCKETOS_SYSTEM_VIEW_H

#include <cjson/cJSON.h>
#include <stddef.h>

/* Every unknown renders as this and never as 0, "", or a guess. U+2014 is in
 * the generated body and mono fonts (0x2013-0x2026); the symbol font is not
 * used for values. */
#define SYSTEM_VIEW_UNKNOWN "\xE2\x80\x94"

#define SYSTEM_VIEW_TEXT 96
#define SYSTEM_VIEW_MAX_MOUNTS 6
#define SYSTEM_VIEW_MAX_IFACES 8
#define SYSTEM_VIEW_MAX_SERVICES 12

enum system_view_phase {
    SYSTEM_VIEW_LIVE = 0,          /* normal, polling */
    SYSTEM_VIEW_CONFIRM_REBOOT,    /* dialog up, nothing called yet */
    SYSTEM_VIEW_CONFIRM_POWEROFF,
    SYSTEM_VIEW_TERMINAL_REBOOT,   /* accepted: stop polling, stop input */
    SYSTEM_VIEW_TERMINAL_POWEROFF,
    SYSTEM_VIEW_CONFIRM_EXPAND     /* dialog up; accepted, it is back to LIVE */
};

enum system_view_action {
    SYSTEM_VIEW_ACTION_REBOOT = 0,
    SYSTEM_VIEW_ACTION_POWEROFF,
    SYSTEM_VIEW_ACTION_EXPAND      /* storage.expand: only while it is offered */
};

/* The microSD card, from storage.status "internal" (docs/api/system.md). */
enum system_view_expand {
    SYSTEM_VIEW_EXPAND_UNKNOWN = 0, /* sysd has not said */
    SYSTEM_VIEW_EXPAND_NONE,        /* nothing to offer: the card is used, or cannot be grown */
    SYSTEM_VIEW_EXPAND_OFFER,       /* space is unused: Expand storage */
    SYSTEM_VIEW_EXPAND_RUNNING,
    SYSTEM_VIEW_EXPAND_RESTART      /* a restart finishes it */
};

/* The three states a service is ever shown in (docs/api/system.md). There is
 * deliberately no STARTING or BACKOFF: both are sub-second transients, and
 * telling them apart would mean reading the supervisor's files from the UI. */
enum system_view_service_state {
    SYSTEM_VIEW_SVC_RUNNING = 0,
    SYSTEM_VIEW_SVC_CRASHLOOP,
    SYSTEM_VIEW_SVC_STOPPED
};

/* What the radio chip is showing, so its treatment is a tested decision here
 * rather than a style fixed once in the LVGL builder. The status bar has
 * always coloured its own chip this way; this row was reading RX in the
 * unavailable treatment, which is the one thing on it that says "live". */
enum system_view_radio_chip {
    SYSTEM_VIEW_RADIO_RX = 0,
    SYSTEM_VIEW_RADIO_TX,
    SYSTEM_VIEW_RADIO_IDLE,     /* answering, but neither receiving nor sending */
    SYSTEM_VIEW_RADIO_UNKNOWN   /* radiod is not answering */
};

struct system_view_metric {
    char label[16];
    char value[SYSTEM_VIEW_TEXT];
    int warn;                      /* render with the warning treatment */
};

struct system_view_mount {
    char mount[32];
    char detail[SYSTEM_VIEW_TEXT]; /* "131 MB free of 574 MB" */
    int used_percent;              /* 0..100, only when have_percent */
    int have_percent;
};

struct system_view_iface {
    char name[24];
    char state[12];                /* chip text: UP / DOWN / operstate */
    char addr[SYSTEM_VIEW_TEXT];   /* IPv4 or the unknown dash */
    int up;
    /* Traffic (DS §52.5): the byte counters sysd reports, and the rate over
     * the time since the previous answer that had them for this interface.
     * "↓1.2 ↑0.3 KB/s · 23.7 MB in · 105.8 MB out", or the dash. */
    double rx_bytes;
    double tx_bytes;
    int have_bytes;
    unsigned long bytes_ms;        /* when they were counted */
    char traffic[SYSTEM_VIEW_TEXT];
};

struct system_view_service {
    char name[40];
    enum system_view_service_state state;
    char detail[SYSTEM_VIEW_TEXT];
};

struct system_view {
    /* identity, from system.info, fetched once */
    int have_info;
    char os_version[SYSTEM_VIEW_TEXT];   /* "0.0.7 · fdc795f" */
    char card_version[SYSTEM_VIEW_TEXT];
    int show_card;                       /* only when the card differs */
    char model[SYSTEM_VIEW_TEXT];
    char kernel[SYSTEM_VIEW_TEXT];
    char platform[SYSTEM_VIEW_TEXT];     /* os-release PRETTY_NAME: "Buildroot 2025.02.1" */
    char sdk[SYSTEM_VIEW_TEXT];          /* the vendor SDK's release line */
    char cpus[16];                       /* online CPUs */

    /* vitals, six cells in three rows of two */
    struct system_view_metric vitals[6];

    struct system_view_mount mounts[SYSTEM_VIEW_MAX_MOUNTS];
    int mount_count;
    struct system_view_iface ifaces[SYSTEM_VIEW_MAX_IFACES];
    int iface_count;
    int ifaces_hidden;                   /* filtered out of the human view */
    struct system_view_service services[SYSTEM_VIEW_MAX_SERVICES];
    int service_count;

    char radio_state[12];                /* chip text from the shell's poll */
    char radio_detail[SYSTEM_VIEW_TEXT];  /* "EU868 · mock" */
    /* Whether the live half is known. The detail is configuration and stays
     * readable when it is not; the screen mutes it rather than blanking it. */
    int radio_state_known;

    /* The network page's links (DS §52.5), each from its own service and
     * each a dash until it has answered once. */
    char radio_packets[SYSTEM_VIEW_TEXT]; /* radio.stats: "848 received · 1 sent · 36 CRC errors" */
    char radio_signal[SYSTEM_VIEW_TEXT];  /* "Last packet -74 dBm, SNR 12.3 dB" */
    char wifi[SYSTEM_VIEW_TEXT];          /* wifi.status: "Connected to Home, signal 4/4" */
    char mesh[SYSTEM_VIEW_TEXT];          /* mesh.status: "Online", "Waiting: ..." */
    int mesh_warn;

    /* freshness */
    int have_status;
    unsigned long last_ok_ms;
    int stale;
    char freshness[SYSTEM_VIEW_TEXT];    /* "LIVE" or "STALE 6s" */

    /* the microSD card's expansion */
    enum system_view_expand expand;
    char expand_line[SYSTEM_VIEW_TEXT];  /* "13.9 GB of the card is not used", "" when nothing to say */
    int expand_warn;                     /* the line is a failure */
    char expand_body[256];               /* the confirmation's text, with the size */

    /* actions */
    enum system_view_phase phase;
    char error[SYSTEM_VIEW_TEXT];        /* last action error, empty if none */
};

void system_view_init(struct system_view *v);

/* system.info, once. info may be NULL (the call failed): identity then stays
 * unknown rather than becoming wrong. */
void system_view_apply_info(struct system_view *v, const cJSON *info);

/* system.status. status NULL means the poll failed: every value already on
 * screen is kept and only the freshness line changes. now_ms is a monotonic
 * millisecond clock. */
void system_view_apply_status(struct system_view *v, const cJSON *status, unsigned long now_ms);

/* The live radio state the shell's status bar already polled; NULL while
 * radiod is not answering. Called every tick, and it touches nothing else. */
void system_view_set_radio_state(struct system_view *v, const char *state);
/* Region and backend from a single radio.info at create: they do not change
 * while radiod runs, so they are asked for once and kept. */
void system_view_set_radio_detail(struct system_view *v, const char *region,
                                  const char *backend);
/* Which treatment the chip should wear for the state it is showing. */
enum system_view_radio_chip system_view_radio_chip_state(const struct system_view *v);

/* The network page's three other answers. NULL: the service did not answer,
 * which is said in words (radiod, netd) or, for meshcored, which is off by
 * default, as the ordinary state it is. */
void system_view_apply_radio_stats(struct system_view *v, const cJSON *stats);
void system_view_apply_wifi(struct system_view *v, const cJSON *wifi_status);
void system_view_apply_mesh(struct system_view *v, const cJSON *mesh_status);

/* storage.status, for its "internal" object: whether the card can be
 * expanded, is expanding, or needs a restart to finish. NULL (no answer)
 * keeps what is on screen. */
void system_view_apply_storage(struct system_view *v, const cJSON *storage);

/* "23.7 MB", "512 kB", "1.4 GB" - a byte count as a person reads one. */
void system_view_bytes(double bytes, char *out, size_t n);

/* Recompute "LIVE" / "STALE 6s" without a new poll, so the age keeps counting
 * up while nothing is answering. */
void system_view_refresh_freshness(struct system_view *v, unsigned long now_ms);

/* True while the screen should keep polling and accepting input. False once a
 * power action has been accepted. */
int system_view_is_polling(const struct system_view *v);

/* Ask for an action: raises the confirmation, calls nothing. */
void system_view_request(struct system_view *v, enum system_view_action action);
/* Back out: calls nothing, changes nothing else. */
void system_view_cancel(struct system_view *v);
/* Confirm. Returns the method the app must call ("system.reboot",
 * "system.poweroff" or "storage.expand"), or NULL when nothing was pending -
 * which is what makes it impossible to reach the API without having gone
 * through a confirmation. */
const char *system_view_confirm(struct system_view *v);
/* The call succeeded: for a power action the machine is going, so stop
 * everything; an expansion runs on in sysd and the screen goes back to live. */
void system_view_action_ok(struct system_view *v);
/* The call failed: back to a usable screen carrying the reason. */
void system_view_action_failed(struct system_view *v, const char *message);

/* Which of a confirmation's two buttons the glass should emphasise.
 *
 * A restart is recoverable and the accent stays on the action. A power-off is
 * not - VERIFIED on unit A, the board needs USB power out for about 30 seconds
 * before it will start again - so the accent goes on Cancel and the action
 * takes the restrained treatment. This is decided here rather than in the
 * LVGL callback because on a touch-only panel there is no input group and
 * therefore no focus ring: the styling is the only thing that says which
 * choice is the safe one, so it has to be a tested decision. */
enum system_view_emphasis {
    SYSTEM_VIEW_EMPHASIS_CONFIRM = 0, /* accent on the action */
    SYSTEM_VIEW_EMPHASIS_CANCEL       /* accent on Cancel */
};

enum system_view_emphasis system_view_dialog_emphasis(const struct system_view *v);

/* Dialog and terminal copy, so the strings are testable and in one place. */
const char *system_view_dialog_title(const struct system_view *v);
const char *system_view_dialog_body(const struct system_view *v);
const char *system_view_dialog_confirm_label(const struct system_view *v);
const char *system_view_terminal_text(const struct system_view *v);

#endif
