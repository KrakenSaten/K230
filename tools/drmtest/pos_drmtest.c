/*
 * pos-drmtest: a small DRM/KMS diagnostic for bringing up a display output
 * (docs/hardware/HDMI_OUTPUT.md, HDMI_GATE.md). Independent of LVGL and of
 * libdrm: it speaks the kernel's DRM ioctls directly, so it builds wherever
 * the kernel UAPI headers are, the build host included.
 *
 *   pos-drmtest [--device PATH] list
 *       every connector: state, size, encoder, and every mode with the clock
 *       the K230 DSI will really produce for it
 *   pos-drmtest [--device PATH] edid [CONNECTOR]
 *       the connector's EDID property, decoded and in hex
 *   pos-drmtest [--device PATH] pattern [--connector NAME] [--mode WxH[@R]]
 *                                        [--seconds N]
 *       a test frame (black, white border, red/green/blue blocks, the mode in
 *       text) on one connector for N seconds (default 15), then the previous
 *       CRTC state back. Without --mode the safest mode is chosen
 *       (drmtest_logic.h). Needs DRM master: stop the shell first. A primary
 *       plane rotation the shell left behind is set to rotate-0 for the
 *       pattern and put back afterwards.
 *
 * Exit status: 0 done; 1 error; 2 usage, or no connected connector / no
 * usable mode; 3 another process is DRM master.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "drmtest_logic.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_OBJS 16

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, req, arg);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

static const char *connector_type_name(uint32_t type)
{
    static const char *const names[] = {
        "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component", "DIN",
        "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
    };

    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "Unknown";
}

static const char *connection_name(uint32_t c)
{
    return c == 1 ? "connected" : c == 2 ? "disconnected" : "unknown";
}

struct card {
    int fd;
    int is_master;
    uint32_t crtcs[MAX_OBJS];
    uint32_t connectors[MAX_OBJS];
    uint32_t encoders[MAX_OBJS];
    uint32_t n_crtcs, n_connectors, n_encoders;
};

struct conn {
    struct drm_mode_get_connector info;
    struct drm_mode_modeinfo *modes;
    uint32_t *encoders;
    uint32_t *props;
    uint64_t *values;
    char name[40];
};

static void conn_free(struct conn *c)
{
    free(c->modes);
    free(c->encoders);
    free(c->props);
    free(c->values);
    memset(c, 0, sizeof(*c));
}

static int card_open(const char *path, struct card *card)
{
    struct drm_mode_card_res res;

    memset(card, 0, sizeof(*card));
    card->fd = open(path, O_RDWR | O_CLOEXEC);
    if (card->fd < 0) {
        fprintf(stderr, "pos-drmtest: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    /* The first opener of an idle card becomes master; anyone else is not,
     * and SET_MASTER fails while another process holds it. */
    card->is_master = xioctl(card->fd, DRM_IOCTL_SET_MASTER, NULL) == 0;

    memset(&res, 0, sizeof(res));
    if (xioctl(card->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "pos-drmtest: %s is not a KMS device: %s\n", path, strerror(errno));
        close(card->fd);
        return -1;
    }
    if (res.count_crtcs > MAX_OBJS || res.count_connectors > MAX_OBJS || res.count_encoders > MAX_OBJS) {
        fprintf(stderr, "pos-drmtest: %s has more objects than this tool handles\n", path);
        close(card->fd);
        return -1;
    }
    res.count_fbs = 0;
    res.crtc_id_ptr = (uintptr_t)card->crtcs;
    res.connector_id_ptr = (uintptr_t)card->connectors;
    res.encoder_id_ptr = (uintptr_t)card->encoders;
    if (xioctl(card->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "pos-drmtest: GETRESOURCES: %s\n", strerror(errno));
        close(card->fd);
        return -1;
    }
    card->n_crtcs = res.count_crtcs;
    card->n_connectors = res.count_connectors;
    card->n_encoders = res.count_encoders;
    return 0;
}

/* The first call with no mode array asks the kernel to probe the connector
 * (only honoured for the DRM master; everyone else gets the cached state).
 * The counts can change between the calls, so it retries. */
static int conn_get(int fd, uint32_t id, struct conn *c)
{
    int tries;

    for (tries = 0; tries < 4; tries++) {
        struct drm_mode_get_connector probe;

        memset(c, 0, sizeof(*c));
        memset(&probe, 0, sizeof(probe));
        probe.connector_id = id;
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &probe) < 0) {
            return -1;
        }
        c->info = probe;
        c->modes = calloc(probe.count_modes + 1, sizeof(*c->modes));
        c->encoders = calloc(probe.count_encoders + 1, sizeof(*c->encoders));
        c->props = calloc(probe.count_props + 1, sizeof(*c->props));
        c->values = calloc(probe.count_props + 1, sizeof(*c->values));
        if (!c->modes || !c->encoders || !c->props || !c->values) {
            conn_free(c);
            errno = ENOMEM;
            return -1;
        }
        c->info.modes_ptr = (uintptr_t)c->modes;
        c->info.encoders_ptr = (uintptr_t)c->encoders;
        c->info.props_ptr = (uintptr_t)c->props;
        c->info.prop_values_ptr = (uintptr_t)c->values;
        /* A zero mode count here would probe again; one placeholder avoids it. */
        if (c->info.count_modes == 0) {
            c->info.count_modes = 1;
        }
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c->info) < 0) {
            conn_free(c);
            return -1;
        }
        if (c->info.count_modes <= (probe.count_modes ? probe.count_modes : 1) &&
            c->info.count_encoders <= probe.count_encoders && c->info.count_props <= probe.count_props) {
            if (probe.count_modes == 0) {
                c->info.count_modes = 0;
            }
            snprintf(c->name, sizeof(c->name), "%s-%u", connector_type_name(c->info.connector_type),
                     c->info.connector_type_id);
            return 0;
        }
        conn_free(c);
    }
    errno = EAGAIN;
    return -1;
}

/* A connector property's value by name; 0 when found. */
static int conn_prop(int fd, const struct conn *c, const char *name, uint64_t *value)
{
    uint32_t i;

    for (i = 0; i < c->info.count_props; i++) {
        struct drm_mode_get_property p;

        memset(&p, 0, sizeof(p));
        p.prop_id = c->props[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0) {
            continue;
        }
        if (strcmp(p.name, name) == 0) {
            *value = c->values[i];
            return 0;
        }
    }
    return -1;
}

/* The EDID blob, malloc'd; NULL when the connector has none. */
static uint8_t *conn_edid(int fd, const struct conn *c, uint32_t *len)
{
    struct drm_mode_get_blob blob;
    uint64_t id = 0;
    uint8_t *data;

    *len = 0;
    if (conn_prop(fd, c, "EDID", &id) < 0 || id == 0) {
        return NULL;
    }
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = (uint32_t)id;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0 || blob.length == 0) {
        return NULL;
    }
    data = malloc(blob.length);
    if (!data) {
        return NULL;
    }
    blob.data = (uintptr_t)data;
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0) {
        free(data);
        return NULL;
    }
    *len = blob.length;
    return data;
}

static void to_dt_modes(const struct conn *c, struct dt_mode *out)
{
    uint32_t i;

    for (i = 0; i < c->info.count_modes; i++) {
        const struct drm_mode_modeinfo *m = &c->modes[i];

        out[i].width = m->hdisplay;
        out[i].height = m->vdisplay;
        out[i].refresh = (int)m->vrefresh;
        out[i].clock_khz = (int)m->clock;
        out[i].interlaced = (m->flags & DRM_MODE_FLAG_INTERLACE) != 0;
        out[i].preferred = (m->type & DRM_MODE_TYPE_PREFERRED) != 0;
    }
}

static void print_mode(const struct drm_mode_modeinfo *m, int index)
{
    int actual = 0;
    int err = 0;
    int ok = dt_clock_quantize((int)m->clock, &actual, &err) == 0;

    printf("    %2d  %4ux%-4u@%-3u%s %7u kHz -> DSI %7d kHz %+7.2f %%  h %u/%u/%u/%u v %u/%u/%u/%u%s\n", index,
           m->hdisplay, m->vdisplay, m->vrefresh, (m->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : " ", m->clock,
           ok ? actual : 0, ok ? err / 10000.0 : 0.0, m->hdisplay, m->hsync_start, m->hsync_end, m->htotal,
           m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal,
           (m->type & DRM_MODE_TYPE_PREFERRED) ? "  preferred" : "");
}

static void print_edid_summary(const uint8_t *edid, uint32_t len, const char *indent)
{
    struct dt_edid e;

    if (dt_edid_parse(edid, len, &e) < 0) {
        printf("%sEDID: %u bytes, too short to decode\n", indent, len);
        return;
    }
    printf("%sEDID: %u bytes, header %s, checksum %s, version %d.%d, %d extension block(s)\n", indent, len,
           e.header_ok ? "ok" : "BAD", e.checksum_ok ? "ok" : "BAD", e.version, e.revision, e.extensions);
    printf("%s  vendor %s product 0x%04x serial %u, week %d of %d, name \"%s\"\n", indent, e.vendor, e.product,
           (unsigned)e.serial, e.week, e.year, e.name);
    if (e.has_preferred) {
        printf("%s  preferred timing %dx%d, %d kHz\n", indent, e.pref_width, e.pref_height, e.pref_clock_khz);
    }
    if (e.has_range) {
        printf("%s  range limits: vertical %d-%d Hz, horizontal %d-%d kHz, pixel clock up to %d MHz\n", indent,
               e.vmin_hz, e.vmax_hz, e.hmin_khz, e.hmax_khz, e.max_clock_mhz);
    }
}

static int cmd_list(struct card *card)
{
    uint32_t i;
    int connected = 0;

    printf("DRM: %u CRTC(s), %u encoder(s), %u connector(s); %s\n", card->n_crtcs, card->n_encoders,
           card->n_connectors,
           card->is_master ? "this process is DRM master, connectors were probed"
                           : "another process is DRM master, connector state is the kernel's cached one");
    for (i = 0; i < card->n_encoders; i++) {
        struct drm_mode_get_encoder enc;

        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = card->encoders[i];
        if (xioctl(card->fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            printf("encoder %u: type %u, crtc %u, possible_crtcs 0x%x\n", enc.encoder_id, enc.encoder_type,
                   enc.crtc_id, enc.possible_crtcs);
        }
    }
    for (i = 0; i < card->n_crtcs; i++) {
        struct drm_mode_crtc crtc;

        memset(&crtc, 0, sizeof(crtc));
        crtc.crtc_id = card->crtcs[i];
        if (xioctl(card->fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0) {
            printf("crtc %u: fb %u, %s\n", crtc.crtc_id, crtc.fb_id,
                   crtc.mode_valid ? crtc.mode.name : "no mode (off)");
        }
    }
    for (i = 0; i < card->n_connectors; i++) {
        struct conn c;
        uint8_t *edid;
        uint32_t len;
        uint32_t m;

        if (conn_get(card->fd, card->connectors[i], &c) < 0) {
            printf("connector %u: cannot be read: %s\n", card->connectors[i], strerror(errno));
            continue;
        }
        connected += c.info.connection == 1;
        printf("connector %u %s: %s, %ux%u mm, encoder %u, %u mode(s)\n", c.info.connector_id, c.name,
               connection_name(c.info.connection), c.info.mm_width, c.info.mm_height, c.info.encoder_id,
               c.info.count_modes);
        for (m = 0; m < c.info.count_modes; m++) {
            print_mode(&c.modes[m], (int)m);
        }
        edid = conn_edid(card->fd, &c, &len);
        if (edid) {
            print_edid_summary(edid, len, "    ");
            free(edid);
        } else {
            printf("    EDID: none\n");
        }
        conn_free(&c);
    }
    return connected ? 0 : 2;
}

/* The connector the user named, or the first connected one. */
static int find_connector(struct card *card, const char *name, struct conn *out)
{
    uint32_t i;

    for (i = 0; i < card->n_connectors; i++) {
        if (conn_get(card->fd, card->connectors[i], out) < 0) {
            continue;
        }
        if (name ? strcmp(out->name, name) == 0 : out->info.connection == 1) {
            return 0;
        }
        conn_free(out);
    }
    return -1;
}

static int cmd_edid(struct card *card, const char *name)
{
    struct conn c;
    uint8_t *edid;
    uint32_t len;
    uint32_t i;

    if (find_connector(card, name, &c) < 0) {
        fprintf(stderr, "pos-drmtest: %s\n", name ? "no such connector" : "no connected connector");
        return 2;
    }
    edid = conn_edid(card->fd, &c, &len);
    printf("connector %s: %s\n", c.name, connection_name(c.info.connection));
    if (!edid) {
        printf("EDID: none (no monitor, no DDC answer, or a corrupt block the kernel rejected)\n");
        conn_free(&c);
        return 2;
    }
    print_edid_summary(edid, len, "");
    for (i = 0; i < len; i++) {
        printf("%02x%s", edid[i], (i % 16 == 15) ? "\n" : " ");
    }
    if (len % 16) {
        printf("\n");
    }
    free(edid);
    conn_free(&c);
    return 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int pick_crtc(struct card *card, const struct conn *c, uint32_t *crtc_id)
{
    uint32_t i;
    uint32_t j;

    for (i = 0; i < c->info.count_encoders; i++) {
        struct drm_mode_get_encoder enc;

        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = c->encoders[i];
        if (xioctl(card->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) {
            continue;
        }
        for (j = 0; j < card->n_crtcs; j++) {
            if (enc.possible_crtcs & (1u << j)) {
                *crtc_id = card->crtcs[j];
                return 0;
            }
        }
    }
    return -1;
}

/* The primary plane's "rotation" property, found through the universal-plane
 * view. The shell turns its picture with that property (DS §21 system
 * rotation) and the kernel keeps the value after the shell exits: with
 * rotate-90/270 left on, the legacy SETCRTC checks a WxH framebuffer against
 * the swapped HxW viewport and refuses it with ENOSPC (drm_crtc_check_viewport;
 * seen on unit A in landscape, 2026-09-27). */
struct plane_rot {
    uint32_t plane_id;
    uint32_t prop_id;
    uint64_t value;
};

#define ROTATE_0 1u /* DRM_MODE_ROTATE_0 */

static int prop_name_is(int fd, uint32_t prop_id, const char *name)
{
    struct drm_mode_get_property p;

    memset(&p, 0, sizeof(p));
    p.prop_id = prop_id;
    return xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 && strcmp(p.name, name) == 0;
}

static int primary_rotation(struct card *card, uint32_t crtc_id, struct plane_rot *out)
{
    struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
    struct drm_mode_get_plane_res res;
    uint32_t planes[MAX_OBJS];
    uint32_t crtc_bit = 0;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < card->n_crtcs; i++) {
        if (card->crtcs[i] == crtc_id) {
            crtc_bit = 1u << i;
        }
    }
    if (!crtc_bit || xioctl(card->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        return -1;
    }
    memset(&res, 0, sizeof(res));
    if (xioctl(card->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) < 0 || res.count_planes > MAX_OBJS) {
        return -1;
    }
    res.plane_id_ptr = (uintptr_t)planes;
    if (xioctl(card->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res) < 0) {
        return -1;
    }
    for (i = 0; i < res.count_planes; i++) {
        struct drm_mode_get_plane pl;
        struct drm_mode_obj_get_properties op;
        uint32_t props[32];
        uint64_t values[32];
        int primary = 0;
        int rot = -1;

        memset(&pl, 0, sizeof(pl));
        pl.plane_id = planes[i];
        if (xioctl(card->fd, DRM_IOCTL_MODE_GETPLANE, &pl) < 0 || !(pl.possible_crtcs & crtc_bit)) {
            continue;
        }
        memset(&op, 0, sizeof(op));
        op.obj_id = planes[i];
        op.obj_type = DRM_MODE_OBJECT_PLANE;
        op.props_ptr = (uintptr_t)props;
        op.prop_values_ptr = (uintptr_t)values;
        op.count_props = 32;
        if (xioctl(card->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0 || op.count_props > 32) {
            continue;
        }
        for (j = 0; j < op.count_props; j++) {
            if (prop_name_is(card->fd, props[j], "type")) {
                primary = values[j] == 1; /* the kernel's DRM_PLANE_TYPE_PRIMARY; not in the uapi */
            } else if (prop_name_is(card->fd, props[j], "rotation")) {
                rot = (int)j;
            }
        }
        if (primary && rot >= 0) {
            out->plane_id = planes[i];
            out->prop_id = props[rot];
            out->value = values[rot];
            return 0;
        }
    }
    return -1;
}

static int set_rotation(struct card *card, const struct plane_rot *r, uint64_t value)
{
    struct drm_mode_obj_set_property sp;

    memset(&sp, 0, sizeof(sp));
    sp.value = value;
    sp.prop_id = r->prop_id;
    sp.obj_id = r->plane_id;
    sp.obj_type = DRM_MODE_OBJECT_PLANE;
    return xioctl(card->fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp);
}

static int cmd_pattern(struct card *card, const char *name, const char *want, int seconds)
{
    struct plane_rot rot = { 0, 0, ROTATE_0 };
    int rot_changed = 0;
    struct conn c;
    struct dt_mode *modes;
    struct drm_mode_modeinfo mode;
    struct drm_mode_create_dumb dumb;
    struct drm_mode_map_dumb map;
    struct drm_mode_destroy_dumb destroy;
    struct drm_mode_fb_cmd fb;
    struct drm_mode_crtc saved;
    struct drm_mode_crtc set;
    char why[200];
    char line2[64];
    char line3[64];
    uint32_t crtc_id = 0;
    uint32_t conn_id;
    void *map_ptr;
    int actual = 0;
    int err = 0;
    int idx;
    int rc = 1;
    uint64_t t0;

    if (!card->is_master) {
        fprintf(stderr, "pos-drmtest: another process is DRM master; stop the shell first "
                        "(/etc/init.d/S90doors-shell stop)\n");
        return 3;
    }
    if (find_connector(card, name, &c) < 0) {
        fprintf(stderr, "pos-drmtest: %s\n", name ? "no such connector" : "no connected connector");
        return 2;
    }
    if (c.info.connection != 1 || c.info.count_modes == 0) {
        fprintf(stderr, "pos-drmtest: %s is %s with %u mode(s); nothing to show\n", c.name,
                connection_name(c.info.connection), c.info.count_modes);
        conn_free(&c);
        return 2;
    }
    modes = calloc(c.info.count_modes, sizeof(*modes));
    if (!modes) {
        conn_free(&c);
        return 1;
    }
    to_dt_modes(&c, modes);
    idx = dt_choose_mode(modes, (int)c.info.count_modes, want, why, sizeof(why));
    free(modes);
    printf("%s: %s\n", c.name, why);
    if (idx < 0) {
        conn_free(&c);
        return 2;
    }
    mode = c.modes[idx];
    conn_id = c.info.connector_id;
    if (pick_crtc(card, &c, &crtc_id) < 0) {
        fprintf(stderr, "pos-drmtest: %s has no usable CRTC\n", c.name);
        conn_free(&c);
        return 1;
    }

    memset(&dumb, 0, sizeof(dumb));
    dumb.width = mode.hdisplay;
    dumb.height = mode.vdisplay;
    dumb.bpp = 32;
    if (xioctl(card->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
        fprintf(stderr, "pos-drmtest: CREATE_DUMB %ux%u: %s\n", mode.hdisplay, mode.vdisplay, strerror(errno));
        conn_free(&c);
        return 1;
    }
    printf("framebuffer: %ux%u XRGB8888, pitch %u, %llu bytes\n", dumb.width, dumb.height, dumb.pitch,
           (unsigned long long)dumb.size);
    memset(&fb, 0, sizeof(fb));
    fb.width = dumb.width;
    fb.height = dumb.height;
    fb.pitch = dumb.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = dumb.handle;
    if (xioctl(card->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        fprintf(stderr, "pos-drmtest: ADDFB: %s\n", strerror(errno));
        goto out_dumb;
    }
    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    if (xioctl(card->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "pos-drmtest: MAP_DUMB: %s\n", strerror(errno));
        goto out_fb;
    }
    map_ptr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, card->fd, (off_t)map.offset);
    if (map_ptr == MAP_FAILED) {
        fprintf(stderr, "pos-drmtest: mmap: %s\n", strerror(errno));
        goto out_fb;
    }
    dt_clock_quantize((int)mode.clock, &actual, &err);
    snprintf(line2, sizeof(line2), "%ux%u@%u %s", mode.hdisplay, mode.vdisplay, mode.vrefresh, c.name);
    snprintf(line3, sizeof(line3), "DSI %d.%03d MHZ %+.2f%%", actual / 1000, actual % 1000, err / 10000.0);
    dt_draw_pattern(map_ptr, (int)dumb.width, (int)dumb.height, (int)(dumb.pitch / 4), "DOORS K230 HDMI TEST", line2,
                    line3);
    munmap(map_ptr, dumb.size);

    /* From here on a Ctrl-C or SIGTERM ends the hold early, and the previous
     * CRTC state is still put back. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    memset(&saved, 0, sizeof(saved));
    saved.crtc_id = crtc_id;
    xioctl(card->fd, DRM_IOCTL_MODE_GETCRTC, &saved);
    if (primary_rotation(card, crtc_id, &rot) == 0 && rot.value != ROTATE_0) {
        if (set_rotation(card, &rot, ROTATE_0) == 0) {
            rot_changed = 1;
            printf("primary plane %u: rotation 0x%llx left by the last user, rotate-0 for the pattern\n",
                   rot.plane_id, (unsigned long long)rot.value);
        } else {
            fprintf(stderr, "pos-drmtest: cannot reset the primary plane's rotation 0x%llx: %s\n",
                    (unsigned long long)rot.value, strerror(errno));
        }
    }

    memset(&set, 0, sizeof(set));
    set.crtc_id = crtc_id;
    set.fb_id = fb.fb_id;
    set.set_connectors_ptr = (uintptr_t)&conn_id;
    set.count_connectors = 1;
    set.mode = mode;
    set.mode_valid = 1;
    t0 = now_ms();
    if (xioctl(card->fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fprintf(stderr, "pos-drmtest: SETCRTC %s on %s: %s\n", mode.name, c.name, strerror(errno));
        rc = (errno == EACCES || errno == EPERM) ? 3 : 1;
        goto out_fb;
    }
    printf("mode set on %s (crtc %u) in %llu ms; showing for %d s (Ctrl-C ends it)\n", c.name, crtc_id,
           (unsigned long long)(now_ms() - t0), seconds);
    fflush(stdout);
    for (t0 = now_ms(); !stop_requested && now_ms() - t0 < (uint64_t)seconds * 1000u;) {
        usleep(100000);
    }
    rc = 0;

    /* Back to what was there: the rotation first (a previous framebuffer was
     * made for it), then the previous framebuffer and mode, or off. */
    if (rot_changed && set_rotation(card, &rot, rot.value) == 0) {
        rot_changed = 0;
        printf("primary plane rotation 0x%llx restored\n", (unsigned long long)rot.value);
    }
    if (saved.mode_valid && saved.fb_id) {
        saved.set_connectors_ptr = (uintptr_t)&conn_id;
        saved.count_connectors = 1;
        if (xioctl(card->fd, DRM_IOCTL_MODE_SETCRTC, &saved) == 0) {
            printf("previous CRTC state restored\n");
            goto out_fb;
        }
    }
    memset(&set, 0, sizeof(set));
    set.crtc_id = crtc_id;
    if (xioctl(card->fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0) {
        printf("CRTC switched off (it was off, or its framebuffer is gone)\n");
    }

out_fb:
    if (rot_changed && set_rotation(card, &rot, rot.value) < 0) {
        fprintf(stderr, "pos-drmtest: could not restore the primary plane's rotation 0x%llx: %s\n",
                (unsigned long long)rot.value, strerror(errno));
    }
    xioctl(card->fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
out_dumb:
    memset(&destroy, 0, sizeof(destroy));
    destroy.handle = dumb.handle;
    xioctl(card->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    conn_free(&c);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: pos-drmtest [--device PATH] list\n"
                    "       pos-drmtest [--device PATH] edid [CONNECTOR]\n"
                    "       pos-drmtest [--device PATH] pattern [--connector NAME] [--mode WxH[@R]] "
                    "[--seconds N]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/dri/card0";
    const char *connector = NULL;
    const char *want = NULL;
    const char *cmd = "list";
    int seconds = 15;
    struct card card;
    int i = 1;
    int rc;

    if (i + 1 < argc && strcmp(argv[i], "--device") == 0) {
        device = argv[i + 1];
        i += 2;
    }
    if (i < argc) {
        cmd = argv[i++];
    }
    if (strcmp(cmd, "edid") == 0 && i < argc) {
        connector = argv[i++];
    }
    for (; i < argc; i++) {
        if (strcmp(cmd, "pattern") == 0 && i + 1 < argc && strcmp(argv[i], "--connector") == 0) {
            connector = argv[++i];
        } else if (strcmp(cmd, "pattern") == 0 && i + 1 < argc && strcmp(argv[i], "--mode") == 0) {
            want = argv[++i];
        } else if (strcmp(cmd, "pattern") == 0 && i + 1 < argc && strcmp(argv[i], "--seconds") == 0) {
            char *end;
            long v = strtol(argv[++i], &end, 10);

            if (*end || v < 1 || v > 3600) {
                return usage();
            }
            seconds = (int)v;
        } else {
            return usage();
        }
    }
    if (strcmp(cmd, "list") && strcmp(cmd, "edid") && strcmp(cmd, "pattern")) {
        return usage();
    }
    if (card_open(device, &card) < 0) {
        return 1;
    }
    if (strcmp(cmd, "list") == 0) {
        rc = cmd_list(&card);
    } else if (strcmp(cmd, "edid") == 0) {
        rc = cmd_edid(&card, connector);
    } else {
        rc = cmd_pattern(&card, connector, want, seconds);
    }
    if (card.is_master) {
        xioctl(card.fd, DRM_IOCTL_DROP_MASTER, NULL);
    }
    close(card.fd);
    return rc;
}
