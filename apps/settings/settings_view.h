/*
 * Settings: everything the screen decides, with no LVGL in it.
 *
 * The screen (settings_app.c) builds panels and turns taps into calls; this
 * turns netd's wifi.status and wifi.networks, and the shell's brightness, into
 * the exact strings, tones and enabled states the panels show, and decides
 * what a tap on a network means. It is the settings_app equivalent of
 * system_view, and for the same reason: the parts of a settings screen that
 * go wrong - a failure shown as "connecting" for ever, an open network joined
 * without a warning, a passphrase sent where none was typed, a brightness
 * step past the floor - are host-tested here (tests/settings_view_test.c).
 *
 * Nothing here talks to a service, reads a file or holds a passphrase longer
 * than the call that is given one.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SETTINGS_VIEW_H
#define POCKETOS_SETTINGS_VIEW_H

#include <cjson/cJSON.h>
#include <stddef.h>

#define SV_TEXT 112
/* The list is a list, not a scanner: the strongest few are what anyone
 * picks from, and every row is a 64 px touch target. */
#define SV_MAX_NETWORKS 12
#define SV_SSID_TEXT 33
#define SV_SSID_HEX 65

enum sv_tone {
    SV_TONE_NORMAL = 0,
    SV_TONE_MUTED,
    SV_TONE_OK,
    SV_TONE_WARN,
    SV_TONE_ERROR,
};

struct sv_network {
    char ssid[SV_SSID_TEXT];
    char ssid_hex[SV_SSID_HEX];
    char security[16];      /* the API word: open, wpa2, ... */
    char detail[48];        /* "WPA2 · 3/4", "Open · 2/4", "WPA3 · not supported" */
    char badge[16];         /* "CONNECTED", "SAVED" or "" */
    int bars;
    int saved;
    int connected;
    int supported;
    int needs_passphrase;
};

struct sv_wifi {
    int reachable;          /* netd answered the last status poll */
    int available;
    int enabled;
    int toggle_enabled;     /* the on/off control can be used */
    char state[24];
    char reason[24];
    char ssid[SV_SSID_TEXT];
    char headline[SV_TEXT];
    enum sv_tone tone;
    char detail[SV_TEXT];   /* address and signal, or empty */
    int can_scan;
    int can_disconnect;
    int scanning;
    int list_visible;       /* Wi-Fi is on and working: a network list belongs on screen */
    int net_count;
    struct sv_network nets[SV_MAX_NETWORKS];
    char list_note[SV_TEXT]; /* what the list area says when it has no rows, or about them */
    char store_note[SV_TEXT];
};

void sv_wifi_init(struct sv_wifi *w);
/* status NULL: netd did not answer. */
void sv_wifi_apply_status(struct sv_wifi *w, const cJSON *status);
/* networks NULL: keep the last list (a missed poll does not blank it). */
void sv_wifi_apply_networks(struct sv_wifi *w, const cJSON *networks);

/* What a tap on a network row leads to. */
enum sv_join_kind {
    SV_JOIN_PASSPHRASE,   /* ask for a passphrase, then join */
    SV_JOIN_SAVED,        /* join with the saved passphrase; offer Forget */
    SV_JOIN_OPEN,         /* warn that it is unencrypted; join only on confirm */
    SV_JOIN_CONNECTED,    /* already joined: offer Disconnect and Forget */
    SV_JOIN_UNSUPPORTED,  /* explain; nothing to do */
};

enum sv_join_kind sv_join_kind(const struct sv_network *n);
/* Title and body for the network sheet. */
void sv_join_text(const struct sv_network *n, char *body, size_t n_body);

/* The same rule netd applies (8..63 printable ASCII), for feedback before a
 * round trip. Returns 0, or -1 with a message in why that never contains the
 * passphrase. */
int sv_passphrase_check(const char *pass, char *why, size_t n);

/* The wifi.connect parameters for a join of kind. passphrase is used only
 * for SV_JOIN_PASSPHRASE and must be NULL otherwise; a saved network is
 * joined without one so netd uses the saved passphrase, and an open one only
 * with allow_open. Returns NULL for a kind that does not join. */
cJSON *sv_connect_params(const struct sv_network *n, enum sv_join_kind kind, const char *passphrase);
/* wifi.forget / identifying params: {ssid_hex}. */
cJSON *sv_ssid_params(const struct sv_network *n);

/* ---- brightness ---------------------------------------------------------- */

struct sv_brightness {
    int supported;
    int percent;            /* -1 when unknown */
    char value[16];         /* "60 %" or an em dash substitute "--" */
    int can_down;
    int can_up;
    char note[SV_TEXT];
};

/* percent: pocketos_shell_brightness_get(), -1 when unsupported/unreadable.
 * min/max/step: the shell's range. */
void sv_brightness_apply(struct sv_brightness *b, int percent, int min, int max);
/* The level a - (direction < 0) or + (direction > 0) tap asks for: the next
 * multiple of step in that direction, clamped to [min, max]. A level off the
 * step grid (55) goes to the neighbouring step (50 or 60), and one below the
 * floor set by something else goes up to the floor. */
int sv_brightness_step(int percent, int direction, int min, int max, int step);

/* ---- rotation ------------------------------------------------------------ */

/* The three modes in the order the buttons show them; the numbers are
 * app.h's enum pocketos_rotation_mode. */
#define SV_ROTATION_MODES 3

struct sv_rotation {
    int selected;           /* the stored mode: 0 automatic, 1 portrait, 2 landscape */
    char note[160];         /* what the display is doing, and what a change costs */
};

/* The arguments are pocketos_shell_orientation()'s fields. The note says what
 * the display is doing; while a change is being applied it says what that
 * costs - the display is rotated when it opens, so Doors restarts itself and
 * comes back on the launcher. */
void sv_rotation_apply(struct sv_rotation *r, int mode, int mode_valid, int landscape, int next_landscape,
                       int applying, int keyboard_present);

/* The security word as shown: "Open", "WPA2", "WPA2/3", ... */
const char *sv_security_label(const char *api_word);

/* ---- sound --------------------------------------------------------------- */

struct sv_volume {
    int available;          /* there is a sound card to play through */
    char value[16];         /* "60 %" */
    int can_down;
    int can_up;
    int muted;
    char note[SV_TEXT];     /* why nothing is heard, or empty */
    char summary[SV_TEXT];  /* the line under Sound in the list of categories */
};

/* The shell's volume (app.h): level, mute, whether a card exists, and the
 * range. The level steps as brightness does (sv_brightness_step). */
void sv_volume_apply(struct sv_volume *v, int percent, int muted, int available, int min, int max);

/* ---- the keyboard base ---------------------------------------------------- */

/* keyboard: app.h enum pocketos_keyboard (0 unknown, 1 absent, 2 present).
 * light: the base's light 0..100, or -1 with no such light. state is the
 * chip ("ATTACHED"), summary the category's line. */
void sv_keyboard_text(int keyboard, int light, char *state, size_t n_state, char *summary, size_t n_summary);

/* ---- Power & Sleep (ui/shell/power_policy.h) ------------------------------ */

struct sv_timer {
    char value[16];         /* "1 min", "Never" */
    int can_down;           /* not at the shortest option */
    int can_up;             /* not at never */
};

/* which: power_policy.h enum power_timer. */
void sv_timer_apply(struct sv_timer *t, int which, int seconds);
/* "Screen off after 1 min, lock after 5 min", "Screen stays on, no auto lock". */
void sv_power_summary(int screen_off_s, int lock_s, char *out, size_t n);

/* ---- Power & Sleep: the battery, read-only (system.status.power) ---------- */

#define SV_BATTERY_ROWS 5

struct sv_battery {
    /* "Status", "Voltage", "Current", "Level", "Updated" and their values.
     * Every value is "-" unless a current reading vouches for it. */
    const char *label[SV_BATTERY_ROWS];
    char value[SV_BATTERY_ROWS][48];
};

/* status: sysd's system.status result, or NULL when sysd did not answer.
 * The level is a percentage only when sysd gives one (capacity_percent); the
 * keyboard base's gauge gives none, so it reads "Unknown". Nothing from a
 * reading that is not current is shown. */
void sv_battery_apply(struct sv_battery *b, const cJSON *status);

#endif
