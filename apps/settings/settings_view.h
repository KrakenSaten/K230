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

/* After sv_rotation_apply, on an external display (HDMI): the mode no longer
 * turns the screen, it says how the unit itself is held - which way up the
 * camera's picture is turned (app.h hold_rotation). The note says so, and
 * still says first when the stored value was not a mode (mode_valid 0). */
void sv_rotation_external(struct sv_rotation *r, int mode_valid, int keyboard_present);

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

/* The same values in two lines, for the wide shape, where Power & Sleep has
 * room for only two (DS §52.2): "On battery · 3.79 V · -0.57 A" and "Level
 * unknown · updated 12 s ago". A value that is "-" is left out; the second
 * line is empty when both of its values are. */
void sv_battery_compact(const struct sv_battery *b, char *first, size_t first_n, char *second, size_t second_n);

/* ---- Companion: the paired phones and what each is shown (gatewayd) ------- */

/* gatewayd's own limit (services/gatewayd/gw.h GW_MAX_CLIENTS). */
#define SV_MAX_PHONES 8
#define SV_PHONE_NAME 33
/* The phone's key as gatewayd names it: 64 hex digits. */
#define SV_PHONE_KEY 65

/* The six scopes of ADR-019 rule 24 and amendment 1, as bits. */
enum sv_scope_bit {
    SV_SCOPE_STATUS = 1u << 0,
    SV_SCOPE_MESH_READ = 1u << 1,
    SV_SCOPE_MESH_SEND = 1u << 2,
    SV_SCOPE_CAPTURE_VIEW = 1u << 3,
    SV_SCOPE_CAPTURE_CONTROL = 1u << 4,
    SV_SCOPE_VISION = 1u << 5,
};
#define SV_SCOPES 6
/* What a new pairing gets unless the person changes it (PAIRING.md §4). */
#define SV_SCOPES_DEFAULT (SV_SCOPE_STATUS | SV_SCOPE_MESH_READ | SV_SCOPE_MESH_SEND)

/* One permission as the unit shows it: the API's word, the switch's title,
 * the line under it, and the one it needs (0: none). */
struct sv_scope_info {
    const char *name;
    unsigned bit;
    const char *title;
    const char *caption;
    unsigned needs;
};
extern const struct sv_scope_info sv_scope_table[SV_SCOPES];

struct sv_phone {
    char name[SV_PHONE_NAME]; /* as the phone gave it, made safe to draw; "Unnamed phone" when it gave none */
    char key[SV_PHONE_KEY];   /* what gateway.scope is sent; never drawn whole */
    char detail[64];          /* "Key 1A2B 3C4D 5E6F 7A8B, connected": the start of the key as the phone shows its own */
    char key_show[24];        /* "1A2B 3C4D 5E6F 7A8B" */
    char perms[SV_TEXT];      /* "Status, RIFT, sending, Vision": what it may do, in words */
    long long paired_wall_s;  /* when it was paired by the wall clock, 0: unknown */
    int connected;
    int vision;               /* holds the scope `vision` */
    unsigned scopes;          /* every scope it holds, SV_SCOPE_* */
};

struct sv_companion {
    int reachable;            /* gatewayd answered */
    char headline[SV_TEXT];   /* "On, 192.168.1.20:47231", "Off: no phone is paired", "Not running" */
    enum sv_tone tone;
    char note[SV_TEXT];       /* what the list says when it has no rows; empty otherwise */
    char summary[SV_TEXT];    /* the line under Companion in the list of categories */
    int count;
    struct sv_phone phones[SV_MAX_PHONES];
    /* gatewayd's own words, kept for sv_companion_system() */
    int listening;
    char closed[24];          /* why it does not listen: no_clients, password_required, ... */
    char device_key[24];      /* this unit's key as phones show it, or "" */
    int max;                  /* how many phones there can be (gateway.clients' max) */

    /* ---- sysd's companion.status, folded in by sv_companion_system() ---- */
    int sys_known;            /* sysd answered companion.status */
    int installed;
    int enabled;              /* GATEWAYD_ENABLE=1: what the switch shows */
    int password_set;
    char ssh[12];             /* hardened, open, unknown, absent */
    int busy;                 /* a change runs */
    char job[12];             /* enable, disable, password: the running or the last one */
    char job_state[12];       /* idle, running, done, failed */
    char job_error[32];       /* its word, when it failed */
    /* The gateway panel */
    int switch_on;            /* the switch reads ON */
    int switch_can;           /* and can be tapped */
    char switch_label[16];    /* "ON", "OFF", "STARTING", "STOPPING" */
    char hint[2 * SV_TEXT];   /* what to do next, or why it is off; empty when nothing */
    enum sv_tone hint_tone;
    /* The device password panel */
    char pw_headline[SV_TEXT];
    enum sv_tone pw_tone;
    char pw_ssh[2 * SV_TEXT];
    enum sv_tone pw_ssh_tone;
    char pw_button[24];       /* "SET PASSWORD", "CHANGE PASSWORD" */
    int pw_can;
    char pw_note[2 * SV_TEXT]; /* the last password change's result, or empty */
    enum sv_tone pw_note_tone;
    /* The phones panel */
    int pair_can;
    char pair_hint[SV_TEXT];
};

/* status: gatewayd's gateway.status result; clients: its gateway.clients
 * result. Either NULL: gatewayd did not answer (it is off by default, and
 * then not running at all). A client whose key is not 64 hex digits is not
 * listed: nothing could be sent for it. */
void sv_companion_apply(struct sv_companion *c, const cJSON *status, const cJSON *clients);
/* The gateway.scope parameters that give (on) or take the scope `vision` of
 * this phone: {client, scope, on}, the client named by its whole key. */
cJSON *sv_companion_vision_params(const struct sv_phone *p, int on);
/* What the paired list was made of, for telling whether the rows on screen
 * are still right: names, keys, order. Not the switches' states, which are
 * repainted in place. */
void sv_companion_signature(const struct sv_companion *c, char *out, size_t n);

/* sysd's companion.status (NULL: sysd did not answer) folded into what
 * sv_companion_apply() made: the gateway switch and its words, the device
 * password panel, whether a phone can be paired, and the list's line.
 * Call after sv_companion_apply() with the same poll's answers. */
void sv_companion_system(struct sv_companion *c, const cJSON *sys);
/* The sentence for a word sysd or gatewayd refused with ("password_required",
 * "ssh_unknown", ...), for a person: what happened and what to do. Unknown
 * words give the fallback. */
const char *sv_companion_error_text(const char *word, const char *fallback);
/* "<word>: <sentence>" (sysd's error message) -> the word, into out. */
void sv_companion_error_word(const char *message, char *out, size_t n);

/* ---- permissions ----------------------------------------------------------- */

/* The scope bits of a JSON list of names; unknown names are left out. */
unsigned sv_scopes_from_json(const cJSON *list);
/* The JSON list of names of bits, in the table's order. */
cJSON *sv_scopes_json(unsigned scopes);
/* One switch tapped: the set with that scope turned over, and what it needs
 * or what needs it with it (sending on turns reading on; reading off turns
 * sending off; likewise PacketShark's control and its figures). */
unsigned sv_scope_toggle(unsigned scopes, unsigned bit);
/* "Status, RIFT, sending" - what a set allows, in words; "Nothing" for none. */
void sv_scopes_words(unsigned scopes, char *out, size_t n);
/* gateway.set_scopes {client: <whole key>, scopes: [...]} */
cJSON *sv_companion_set_scopes_params(const struct sv_phone *p, unsigned scopes);
/* gateway.revoke {client: <whole key>} */
cJSON *sv_companion_revoke_params(const struct sv_phone *p);
/* The phone with this whole key in the list, or NULL. */
const struct sv_phone *sv_companion_find(const struct sv_companion *c, const char *key);
/* "Paired 24 Nov 2026", or "" when the unit's clock was not set then. */
void sv_phone_paired_text(const struct sv_phone *p, char *out, size_t n);

/* ---- pairing a phone ------------------------------------------------------------ */

enum sv_pair_phase {
    SV_PAIR_SHOWING = 0, /* the code is on screen, nobody has come */
    SV_PAIR_CONNECTING,  /* a phone is at the door, it has not shown the code yet */
    SV_PAIR_DECIDE,      /* a phone showed the code: the person decides */
    SV_PAIR_DONE,        /* it is paired */
    SV_PAIR_CLOSED,      /* the window closed without a pairing: why is in text */
    SV_PAIR_ERROR,       /* the window could not be opened, or the decision failed */
};

/* A pairing URI is about 150 characters (README H10); room for a long host name. */
#define SV_PAIR_URI 400

struct sv_pair {
    enum sv_pair_phase phase;
    char uri[SV_PAIR_URI];     /* the QR code's content: holds the one-time token, wiped when done */
    long long left_ms;         /* the window's time left, as gatewayd last said */
    char countdown[32];        /* "Closes in 1:47" */
    char cand_name[SV_PHONE_NAME];
    char cand_key[SV_PHONE_KEY];
    char cand_key_show[24];    /* "3F2A 91C4 0B7E D115": what the phone shows as "This phone's key" */
    int keys_match;            /* the person said the keys are the same */
    unsigned scopes;           /* what it will get: SV_SCOPES_DEFAULT until changed */
    char headline[SV_TEXT];
    char text[2 * SV_TEXT];
    enum sv_tone tone;
};

/* gateway.pair_begin's answer (NULL with err: refused). */
void sv_pair_begin(struct sv_pair *p, const cJSON *res, const char *err);
/* gateway.status, once a second while the sheet shows (NULL: no answer). */
void sv_pair_poll(struct sv_pair *p, const cJSON *status);
/* ALLOW may be tapped: a phone waits and the person confirmed its key. */
int sv_pair_can_allow(const struct sv_pair *p);
/* gateway.pair_decide {allow, scopes[]}. NULL when allow and !can_allow. */
cJSON *sv_pair_decide_params(const struct sv_pair *p, int allow);
/* The decision's answer. */
void sv_pair_decided(struct sv_pair *p, int allow, const cJSON *res, const char *err);
/* The window is ours to cancel when the sheet goes: open and not decided. */
int sv_pair_open(const struct sv_pair *p);
/* The token goes: the URI is wiped. */
void sv_pair_wipe(struct sv_pair *p);

/* ---- the device password ------------------------------------------------------- */

/* The two fields as typed: 0 when they may be sent, or -1 with a sentence. */
int sv_device_password_check(const char *first, const char *second, char *why, size_t n);
/* companion.set_password {password} */
cJSON *sv_device_password_params(const char *password);

#endif
