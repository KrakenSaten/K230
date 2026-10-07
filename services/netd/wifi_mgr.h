/*
 * netd's Wi-Fi manager: one wireless interface, one wpa_supplicant netd
 * starts and owns, one DHCP client per association, and the store of joined
 * networks. docs/api/network.md is the contract; this is how it is kept.
 *
 * Everything that takes time is asynchronous. A request starts something and
 * returns at once; wifi.status says how it is going. That is what lets the
 * shell call netd from the LVGL thread under its 200 ms UI deadline, and it
 * is the "accept now, finish later" service shape KNOWN_ISSUES asked for
 * before netd.
 *
 * The manager is driven by wifi_mgr_step(), called from netd's loop at least
 * every NETD_STEP_MS and whenever wpa_supplicant has sent something. It
 * reconciles what should be (the store says on or off, a network is being
 * joined) with what is (the interface exists, the supplicant runs, what its
 * STATUS says), so a missed event costs at most one step.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WIFI_MGR_H
#define POCKETOS_WIFI_MGR_H

#include "netd_sys.h"
#include "wifi_parse.h"
#include "wifi_store.h"
#include "wpa_ctrl.h"

#include <cjson/cJSON.h>
#include <sys/types.h>

#define NETD_STEP_MS 250
#define WIFI_CONNECT_TIMEOUT_MS_DEFAULT 30000
#define WIFI_DHCP_TIMEOUT_MS_DEFAULT 20000
#define WIFI_SCAN_TIMEOUT_MS 15000
#define WIFI_SUPPLICANT_START_MS 5000
#define WIFI_SUPPLICANT_MAX_RESTARTS 5
/* A supplicant is restarted as unresponsive only when it has answered
 * nothing for this long (and missed at least three commands in a row): one
 * busy inside a slow driver call - a scan, right after a runtime restart -
 * goes quiet for seconds and then works through what it was sent. */
#define WIFI_SUPPLICANT_UNRESPONSIVE_MS 10000

enum wifi_state {
    WIFI_STATE_UNAVAILABLE = 0, /* no wireless interface, or it is taken */
    WIFI_STATE_OFF,             /* turned off by the user */
    WIFI_STATE_STARTING,        /* netd is bringing wpa_supplicant up */
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_OBTAINING_IP,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,          /* see enum wifi_reason */
};

enum wifi_reason {
    WIFI_REASON_NONE = 0,
    WIFI_REASON_NO_INTERFACE,
    WIFI_REASON_INTERFACE_BUSY, /* another wpa_supplicant controls the interface */
    WIFI_REASON_AUTH_FAILED,    /* wrong passphrase */
    WIFI_REASON_NOT_FOUND,      /* the network did not answer */
    WIFI_REASON_ASSOC_FAILED,   /* the access point refused us */
    WIFI_REASON_TIMEOUT,        /* no outcome within the connect timeout */
    WIFI_REASON_DHCP_FAILED,    /* associated, but no address */
    WIFI_REASON_SUPPLICANT_FAILED,
};

struct wifi_mgr_config {
    const char *iface;       /* validated */
    const char *store_dir;   /* $POCKETOS_STATE_DIR/netd */
    const char *run_dir;     /* $POCKETOS_RUNTIME_DIR/netd */
    int connect_timeout_ms;
    int dhcp_timeout_ms;
};

struct wifi_mgr {
    struct netd_sys sys;
    char store_dir[NETD_PATH_MAX];
    char run_dir[NETD_PATH_MAX];
    char ctrl_dir[NETD_PATH_MAX];
    char conf_path[NETD_PATH_MAX];
    int connect_timeout_ms;
    int dhcp_timeout_ms;

    struct wifi_store store;
    enum wifi_store_load_result store_load;
    int store_set_aside_pending; /* damaged file still in place */
    int store_write_failed;

    pid_t supp_pid;
    long supp_started_ms;
    int supp_failures;
    long supp_retry_at_ms;
    struct wpa_ctrl req;
    struct wpa_ctrl ev;
    int ctrl_ready;
    int ctrl_failures;           /* commands in a row that got no answer */
    long ctrl_answered_ms;       /* when the supplicant last answered anything */
    int sae_capable;
    /* supplicant network id per store slot; -1 not handed over yet (the next
     * sync does), -2 refused by the supplicant (not retried until it restarts) */
    int saved_id[WIFI_STORE_MAX];
    long sync_at_ms;             /* the next sync of saved networks, not before */
    int sync_retries;            /* syncs cut short by a supplicant that did not answer */
    int sweep_due;               /* it may hold networks netd does not track */

    pid_t dhcp_pid;
    long dhcp_retry_at_ms;       /* a client that exited is not restarted before this */

    enum wifi_state state;
    enum wifi_reason reason;
    enum wifi_reason latched;    /* a failure kept until the next user action */
    long foreign_checked_ms;
    pid_t foreign_pid;

    /* the association as STATUS last reported it */
    char wpa_state[32];
    int completed;
    long assoc_since_ms;         /* when COMPLETED was first seen; the DHCP timeout runs from here */
    int cur_id;
    struct wifi_ssid cur_ssid;
    char bssid[WIFI_BSSID_LEN + 1];
    int freq_mhz;
    int signal_dbm;
    int have_signal;
    char ipv4[16];
    long status_due_ms;

    /* a join the user asked for, not yet decided */
    int pending;
    int pending_id;
    int pending_not_found;
    long pending_deadline_ms;
    struct wifi_saved pending_net;
    int pending_replaced_slot;   /* store slot whose supplicant entry was removed, -1 */

    /* the last scan */
    int scanning;
    long scan_started_ms;
    long scan_done_ms;           /* 0 = never */
    int scan_failed;
    struct wifi_bss bss[WIFI_SCAN_MAX];
    int bss_count;
    int hidden_count;
};

int wifi_mgr_init(struct wifi_mgr *m, const struct wifi_mgr_config *cfg);
void wifi_mgr_shutdown(struct wifi_mgr *m);
void wifi_mgr_step(struct wifi_mgr *m, long now_ms);
/* A child netd started has exited (netd's loop reaps with waitpid). */
void wifi_mgr_child_exited(struct wifi_mgr *m, pid_t pid, int status, long now_ms);
/* The descriptor that becomes readable when wpa_supplicant sends an event,
 * or -1. */
int wifi_mgr_event_fd(const struct wifi_mgr *m);

const char *wifi_state_name(enum wifi_state s);
const char *wifi_reason_name(enum wifi_reason r);

/* Requests. Each returns 0 with *result set, or a pocketipc error code with
 * a message in err. None of them blocks for longer than a few control
 * interface round trips (well under the shell's 200 ms). */
int wifi_mgr_status(struct wifi_mgr *m, cJSON **result, char *err, size_t n);
int wifi_mgr_set_enabled(struct wifi_mgr *m, int on, long now_ms, cJSON **result, char *err,
                         size_t n);
int wifi_mgr_scan(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n);
int wifi_mgr_networks(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n);
/* security_decl is only used for a hidden network ("open", "wpa", "wpa2",
 * "wpa3"); pass NULL otherwise. passphrase may be NULL for an open network. */
int wifi_mgr_connect(struct wifi_mgr *m, const struct wifi_ssid *ssid, const char *passphrase,
                     int hidden, const char *security_decl, int allow_open, long now_ms,
                     cJSON **result, char *err, size_t n);
int wifi_mgr_disconnect(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n);
int wifi_mgr_forget(struct wifi_mgr *m, const struct wifi_ssid *ssid, cJSON **result, char *err,
                    size_t n);
int wifi_mgr_saved(struct wifi_mgr *m, cJSON **result, char *err, size_t n);

#endif
