/*
 * Camera's gallery. See camera_gallery.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_gallery.h"

#include <stdio.h>
#include <string.h>

const char *camera_gallery_view_name(enum gallery_view v)
{
    switch (v) {
    case GALLERY_OPENING: return "GALLERY_OPENING";
    case GALLERY_GRID: return "GALLERY_GRID";
    case GALLERY_PHOTO: return "GALLERY_PHOTO";
    case GALLERY_SLIDESHOW: return "GALLERY_SLIDESHOW";
    case GALLERY_FAILED: return "GALLERY_FAILED";
    }
    return "?";
}

void camera_gallery_init(struct camera_gallery *g)
{
    memset(g, 0, sizeof(*g));
    g->view = GALLERY_OPENING;
    g->current = -1;
    g->slide_back_index = -1;
}

static void panel(struct camera_gallery *g, const char *title, const char *detail)
{
    snprintf(g->title, sizeof(g->title), "%s", title);
    snprintf(g->detail, sizeof(g->detail), "%s", detail ? detail : "");
}

static void note(struct camera_gallery *g, const char *text, bool warn, int64_t now)
{
    snprintf(g->note, sizeof(g->note), "%s", text);
    g->note_warn = warn;
    g->note_until = now + GALLERY_NOTE_MS;
}

/* Whatever the slots were asked for no longer applies: their answers are
 * dropped when they come. The slots stay taken until then. */
static void forget_slots(struct camera_gallery *g, int purpose)
{
    int i;

    for (i = 0; i < CAMERA_PICTURE_SLOTS; i++) {
        if (g->slots[i].used && (purpose < 0 || (int)g->slots[i].purpose == purpose)) {
            g->slots[i].index = -1;
        }
    }
}

int camera_gallery_pages(const struct camera_gallery *g)
{
    if (g->per_page <= 0 || g->count <= 0) {
        return 1;
    }
    return (g->count + g->per_page - 1) / g->per_page;
}

int camera_gallery_cell_index(const struct camera_gallery *g, int cell)
{
    int idx;

    if (cell < 0 || cell >= g->per_page) {
        return -1;
    }
    idx = g->page * g->per_page + cell;
    return idx < g->count ? idx : -1;
}

static void reset_cells(struct camera_gallery *g)
{
    int c;

    for (c = 0; c < GALLERY_PAGE_MAX; c++) {
        g->cells[c] = camera_gallery_cell_index(g, c) >= 0 ? GALLERY_CELL_NEED : GALLERY_CELL_EMPTY;
    }
    forget_slots(g, GALLERY_FOR_THUMB);
}

static void set_page(struct camera_gallery *g, int page)
{
    int last = camera_gallery_pages(g) - 1;

    page = page < 0 ? 0 : page > last ? last : page;
    if (page != g->page) {
        g->page = page;
        reset_cells(g);
    }
}

static void to_photo(struct camera_gallery *g, int index)
{
    g->view = GALLERY_PHOTO;
    g->current = index;
    g->photo = GALLERY_PIC_NEED;
    g->confirm_delete = false;
    memset(&g->meta, 0, sizeof(g->meta));
    g->description[0] = '\0';
    forget_slots(g, GALLERY_FOR_PHOTO);
}

static void to_grid(struct camera_gallery *g)
{
    g->view = GALLERY_GRID;
    g->confirm_delete = false;
    g->photo = GALLERY_PIC_NONE;
    forget_slots(g, GALLERY_FOR_PHOTO);
    forget_slots(g, GALLERY_FOR_SLIDE);
    if (g->count == 0) {
        panel(g, "No photos yet", "Photos you take with Camera appear here.");
    }
    /* Back to the page of the photo that was open. */
    if (g->current >= 0 && g->per_page > 0) {
        set_page(g, g->current / g->per_page);
    }
}

static void fail(struct camera_gallery *g, const char *title, const char *detail)
{
    g->view = GALLERY_FAILED;
    g->deleting = false;
    g->exporting = false;
    g->confirm_delete = false;
    memset(g->slots, 0, sizeof(g->slots)); /* the helper and its slots are gone */
    panel(g, title, detail);
}

static void restart(struct camera_gallery *g)
{
    int per_page = g->per_page;
    uint32_t thumb = g->thumb;
    uint32_t pw = g->photo_w;
    uint32_t ph = g->photo_h;
    uint32_t sw = g->show_w;
    uint32_t sh = g->show_h;

    camera_gallery_init(g);
    g->per_page = per_page;
    g->thumb = thumb;
    g->photo_w = pw;
    g->photo_h = ph;
    g->show_w = sw;
    g->show_h = sh;
    panel(g, "Opening photos", "");
}

unsigned camera_gallery_open(struct camera_gallery *g)
{
    restart(g);
    return GALLERY_DO_START;
}

unsigned camera_gallery_retry(struct camera_gallery *g)
{
    if (g->view != GALLERY_FAILED) {
        return GALLERY_DO_NOTHING;
    }
    restart(g);
    return GALLERY_DO_START;
}

static void exited(struct camera_gallery *g, const struct camera_event *ev)
{
    char detail[96];

    if (g->view == GALLERY_FAILED) {
        return;
    }
    switch (ev->reason) {
    case CAMERA_EXIT_HUNG:
        fail(g, "Photos not responding", "The photo helper stopped responding and was closed.");
        break;
    case CAMERA_EXIT_PROTOCOL:
        fail(g, "Photos unavailable", "The photo helper sent something invalid and was closed.");
        break;
    case CAMERA_EXIT_CRASHED:
        snprintf(detail, sizeof(detail), "The photo helper crashed (signal %d).", ev->value - 128);
        fail(g, "Photos unavailable", detail);
        break;
    default:
        snprintf(detail, sizeof(detail), "The photo helper closed unexpectedly (exit %d).",
                 ev->value);
        fail(g, "Photos unavailable", detail);
        break;
    }
}

static int find(const struct camera_gallery *g, const char *name)
{
    int i;

    for (i = 0; i < g->count; i++) {
        if (strcmp(g->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static unsigned deleted(struct camera_gallery *g, const char *name, int64_t now)
{
    int i = find(g, name);

    g->deleting = false;
    g->confirm_delete = false;
    if (i >= 0) {
        memmove(g->names[i], g->names[i + 1], (size_t)(g->count - i - 1) * sizeof(g->names[0]));
        g->count--;
        if (g->total > 0) {
            g->total--;
        }
    }
    /* Every index past the deleted one moved: nothing asked for still means
     * what it meant. */
    forget_slots(g, -1);
    note(g, "Photo deleted", false, now);
    if (g->count == 0) {
        g->current = -1;
        to_grid(g);
        reset_cells(g);
        return GALLERY_DO_NOTHING;
    }
    if (g->view == GALLERY_PHOTO) {
        /* The next older photo takes its place; after the oldest, the one
         * before it. */
        to_photo(g, g->current < g->count ? g->current : g->count - 1);
    }
    set_page(g, g->page);
    reset_cells(g);
    return GALLERY_DO_PICTURES;
}

unsigned camera_gallery_event(struct camera_gallery *g, const struct camera_event *ev, int64_t now)
{
    char detail[CAMERA_EVENT_TEXT_MAX + 48];

    switch (ev->kind) {
    case CAMERA_EV_READY:
        return g->view == GALLERY_OPENING ? GALLERY_DO_LIST : GALLERY_DO_NOTHING;
    case CAMERA_EV_LISTED:
        return GALLERY_DO_TAKE_LIST;
    case CAMERA_EV_LISTFAIL:
        snprintf(detail, sizeof(detail), "The photo folder could not be read: %s.", ev->text);
        fail(g, "Photos unavailable", detail);
        return GALLERY_DO_NOTHING;
    case CAMERA_EV_ERROR:
    case CAMERA_EV_NODEVICE:
        if (strncmp(ev->text, "exec", 4) == 0) {
            snprintf(detail, sizeof(detail), "The photo helper could not be started:%s",
                     ev->text + 4);
        } else {
            snprintf(detail, sizeof(detail), "The photos could not be opened: %s", ev->text);
        }
        fail(g, "Photos unavailable", detail);
        return GALLERY_DO_NOTHING;
    case CAMERA_EV_EXITED:
        exited(g, ev);
        return GALLERY_DO_NOTHING;
    case CAMERA_EV_DELETED:
        return g->deleting ? deleted(g, ev->name, now) : GALLERY_DO_NOTHING;
    case CAMERA_EV_DELFAIL:
        if (g->deleting) {
            g->deleting = false;
            g->confirm_delete = false;
            note(g, "The photo could not be deleted", true, now);
        }
        return GALLERY_DO_NOTHING;
    case CAMERA_EV_EXPORTED:
        if (g->exporting) {
            char saved[sizeof(g->note)];

            g->exporting = false;
            snprintf(saved, sizeof(saved), "%s %s",
                     ev->value ? "Already in Files:" : "Saved to Files:", ev->path);
            note(g, saved, false, now);
        }
        return GALLERY_DO_NOTHING;
    case CAMERA_EV_EXPFAIL:
        if (g->exporting) {
            g->exporting = false;
            note(g,
                 ev->reason == CAMERA_EXPFAIL_EXISTS    ? "Files has another file of that name"
                 : ev->reason == CAMERA_EXPFAIL_NOSPACE ? "Storage is full: nothing was exported"
                 : ev->reason == CAMERA_EXPFAIL_MISSING ? "The photo is gone"
                                                        : "The photo could not be exported",
                 true, now);
        }
        return GALLERY_DO_NOTHING;
    default:
        return GALLERY_DO_NOTHING;
    }
}

unsigned camera_gallery_set_list(struct camera_gallery *g, char (*names)[CAMERA_NAME_MAX], int n,
                                 uint32_t total)
{
    char keep[CAMERA_NAME_MAX] = "";
    int i;

    if (g->view == GALLERY_FAILED) {
        return GALLERY_DO_NOTHING;
    }
    if (g->current >= 0 && g->current < g->count) {
        snprintf(keep, sizeof(keep), "%s", g->names[g->current]);
    }
    if (n < 0) {
        n = 0;
    }
    if (n > CAMERA_LIBRARY_MAX) {
        n = CAMERA_LIBRARY_MAX;
    }
    for (i = 0; i < n; i++) {
        memcpy(g->names[i], names[i], CAMERA_NAME_MAX);
        g->names[i][CAMERA_NAME_MAX - 1] = '\0';
    }
    g->count = n;
    g->total = total < (uint32_t)n ? (uint32_t)n : total;
    g->listed = true;
    forget_slots(g, -1);
    if (g->view == GALLERY_OPENING) {
        g->page = 0;
        g->current = -1;
        to_grid(g);
    } else if (g->view == GALLERY_PHOTO) {
        i = keep[0] ? find(g, keep) : -1;
        if (i >= 0) {
            to_photo(g, i);
        } else if (g->count > 0) {
            to_photo(g, g->current < g->count ? g->current : g->count - 1);
        } else {
            g->current = -1;
            to_grid(g);
        }
    }
    set_page(g, g->page);
    reset_cells(g);
    return GALLERY_DO_PICTURES;
}

unsigned camera_gallery_set_sizes(struct camera_gallery *g, int per_page, uint32_t thumb,
                                  uint32_t photo_w, uint32_t photo_h, uint32_t show_w,
                                  uint32_t show_h)
{
    int first = g->page * (g->per_page > 0 ? g->per_page : 1);

    per_page = per_page < 1 ? 1 : per_page > GALLERY_PAGE_MAX ? GALLERY_PAGE_MAX : per_page;
    if (per_page != g->per_page || thumb != g->thumb) {
        g->per_page = per_page;
        g->thumb = thumb;
        g->page = first / per_page;
        set_page(g, g->page);
        reset_cells(g);
    }
    if (photo_w != g->photo_w || photo_h != g->photo_h) {
        g->photo_w = photo_w;
        g->photo_h = photo_h;
        if (g->photo == GALLERY_PIC_SHOWN || g->photo == GALLERY_PIC_WAITING) {
            g->photo = GALLERY_PIC_NEED;
            forget_slots(g, GALLERY_FOR_PHOTO);
        }
    }
    if (show_w != g->show_w || show_h != g->show_h) {
        g->show_w = show_w;
        g->show_h = show_h;
        if (g->view == GALLERY_SLIDESHOW) {
            /* The pictures were of the old size: start again from the one shown. */
            g->slide_want = g->current >= 0 ? g->current : g->slide_want;
            g->current = -1;
            g->slide_front = GALLERY_PIC_NEED;
            g->slide_back = GALLERY_PIC_NONE;
            forget_slots(g, GALLERY_FOR_SLIDE);
        }
    }
    return GALLERY_DO_PICTURES;
}

/* ---- taps ------------------------------------------------------------------------ */

static bool busy(const struct camera_gallery *g)
{
    return g->deleting || g->exporting;
}

unsigned camera_gallery_tap_cell(struct camera_gallery *g, int cell)
{
    int idx = camera_gallery_cell_index(g, cell);

    if (g->view != GALLERY_GRID || idx < 0) {
        return GALLERY_DO_NOTHING;
    }
    to_photo(g, idx);
    return GALLERY_DO_PICTURES;
}

static unsigned step(struct camera_gallery *g, int delta)
{
    if (g->view == GALLERY_GRID) {
        int page = g->page + delta;

        if (page < 0 || page >= camera_gallery_pages(g)) {
            return GALLERY_DO_NOTHING;
        }
        set_page(g, page);
        return GALLERY_DO_PICTURES;
    }
    if (g->view == GALLERY_PHOTO && !busy(g) && !g->confirm_delete) {
        int idx = g->current + delta;

        if (idx < 0 || idx >= g->count) {
            return GALLERY_DO_NOTHING;
        }
        to_photo(g, idx);
        return GALLERY_DO_PICTURES;
    }
    return GALLERY_DO_NOTHING;
}

unsigned camera_gallery_newer(struct camera_gallery *g)
{
    return step(g, -1);
}

unsigned camera_gallery_older(struct camera_gallery *g)
{
    return step(g, 1);
}

unsigned camera_gallery_left(struct camera_gallery *g)
{
    switch (g->view) {
    case GALLERY_OPENING:
    case GALLERY_GRID:
    case GALLERY_FAILED:
        return GALLERY_DO_CAMERA;
    case GALLERY_PHOTO:
        if (g->confirm_delete) {
            if (!g->deleting) {
                g->confirm_delete = false;
            }
            return GALLERY_DO_NOTHING;
        }
        if (busy(g)) {
            return GALLERY_DO_NOTHING;
        }
        to_grid(g);
        return GALLERY_DO_PICTURES;
    case GALLERY_SLIDESHOW:
        return camera_gallery_tap_slideshow(g);
    }
    return GALLERY_DO_NOTHING;
}

unsigned camera_gallery_slideshow(struct camera_gallery *g, int64_t now)
{
    (void)now;
    if ((g->view != GALLERY_GRID && g->view != GALLERY_PHOTO) || g->count == 0 || busy(g)) {
        return GALLERY_DO_NOTHING;
    }
    /* From the photo open, else from the first on the page. */
    g->slide_want = g->view == GALLERY_PHOTO && g->current >= 0 ? g->current
                                                               : g->page * g->per_page;
    if (g->slide_want >= g->count) {
        g->slide_want = 0;
    }
    g->view = GALLERY_SLIDESHOW;
    g->confirm_delete = false;
    g->current = -1;
    g->slide_front = GALLERY_PIC_NEED;
    g->slide_back = GALLERY_PIC_NONE;
    g->slide_back_index = -1;
    g->slide_fails = 0;
    g->slide_due = 0;
    forget_slots(g, GALLERY_FOR_PHOTO);
    forget_slots(g, GALLERY_FOR_SLIDE);
    return GALLERY_DO_PICTURES;
}

unsigned camera_gallery_middle(struct camera_gallery *g, int64_t now)
{
    switch (g->view) {
    case GALLERY_GRID:
        return camera_gallery_slideshow(g, now);
    case GALLERY_PHOTO:
        if (g->confirm_delete || busy(g) || g->current < 0) {
            return GALLERY_DO_NOTHING;
        }
        g->exporting = true;
        g->note[0] = '\0';
        return GALLERY_DO_EXPORT;
    case GALLERY_FAILED:
        return camera_gallery_retry(g);
    default:
        return GALLERY_DO_NOTHING;
    }
}

unsigned camera_gallery_right(struct camera_gallery *g)
{
    if (g->view != GALLERY_PHOTO || g->current < 0 || g->deleting || g->exporting) {
        return GALLERY_DO_NOTHING;
    }
    if (!g->confirm_delete) {
        g->confirm_delete = true;
        g->note[0] = '\0';
        return GALLERY_DO_NOTHING;
    }
    g->deleting = true;
    return GALLERY_DO_DELETE;
}

unsigned camera_gallery_tap_slideshow(struct camera_gallery *g)
{
    if (g->view != GALLERY_SLIDESHOW) {
        return GALLERY_DO_NOTHING;
    }
    if (g->current < 0) {
        g->current = g->slide_want;
    }
    to_grid(g);
    reset_cells(g);
    return GALLERY_DO_PICTURES;
}

/* ---- time --------------------------------------------------------------------- */

static int next_index(const struct camera_gallery *g, int i)
{
    return g->count > 0 ? (i + 1) % g->count : 0;
}

bool camera_gallery_tick(struct camera_gallery *g, int64_t now, unsigned *acts)
{
    bool changed = false;

    if (g->note[0] && now >= g->note_until) {
        g->note[0] = '\0';
        changed = true;
    }
    if (g->view == GALLERY_SLIDESHOW && g->slide_front == GALLERY_PIC_SHOWN &&
        g->slide_back == GALLERY_PIC_SHOWN && now >= g->slide_due) {
        g->current = g->slide_back_index;
        g->slide_back = GALLERY_PIC_NONE;
        g->slide_back_index = -1;
        g->slide_due = now + GALLERY_SLIDE_MS;
        g->slides_shown++;
        g->slide_want = next_index(g, g->current);
        if (g->count > 1) {
            g->slide_back = GALLERY_PIC_NEED;
        }
        *acts |= GALLERY_DO_SWAP | GALLERY_DO_PICTURES;
        changed = true;
    }
    return changed;
}

/* ---- pictures ------------------------------------------------------------------ */

static bool slot_free(const struct camera_gallery *g)
{
    int i;

    for (i = 0; i < CAMERA_PICTURE_SLOTS; i++) {
        if (!g->slots[i].used) {
            return true;
        }
    }
    return false;
}

bool camera_gallery_next_request(struct camera_gallery *g, struct gallery_request *req)
{
    int c;

    memset(req, 0, sizeof(*req));
    if (!slot_free(g) || !g->listed) {
        return false;
    }
    if (g->view == GALLERY_PHOTO && g->photo == GALLERY_PIC_NEED && g->photo_w && g->photo_h &&
        g->current >= 0 && g->current < g->count) {
        req->purpose = GALLERY_FOR_PHOTO;
        req->index = g->current;
        req->w = g->photo_w;
        req->h = g->photo_h;
    } else if (g->view == GALLERY_SLIDESHOW && g->show_w && g->show_h &&
               (g->slide_front == GALLERY_PIC_NEED || g->slide_back == GALLERY_PIC_NEED) &&
               g->slide_want >= 0 && g->slide_want < g->count) {
        req->purpose = GALLERY_FOR_SLIDE;
        req->index = g->slide_want;
        req->cell = g->slide_front == GALLERY_PIC_NEED ? 0 : 1; /* 1: the prepared one */
        req->w = g->show_w;
        req->h = g->show_h;
    } else if (g->view == GALLERY_GRID && g->thumb) {
        for (c = 0; c < g->per_page; c++) {
            if (g->cells[c] == GALLERY_CELL_NEED) {
                break;
            }
        }
        if (c == g->per_page) {
            return false;
        }
        req->purpose = GALLERY_FOR_THUMB;
        req->index = camera_gallery_cell_index(g, c);
        req->cell = c;
        req->w = g->thumb;
        req->h = g->thumb;
        req->cover = true;
    } else {
        return false;
    }
    req->name = g->names[req->index];
    return true;
}

void camera_gallery_requested(struct camera_gallery *g, const struct gallery_request *req, int slot)
{
    if (slot < 0 || slot >= CAMERA_PICTURE_SLOTS) {
        return; /* not sent: still needed, asked for again at the next chance */
    }
    g->slots[slot].used = true;
    g->slots[slot].purpose = req->purpose;
    g->slots[slot].index = req->index;
    g->slots[slot].cell = req->cell;
    g->slots[slot].back = req->purpose == GALLERY_FOR_SLIDE && req->cell == 1;
    switch (req->purpose) {
    case GALLERY_FOR_THUMB:
        g->cells[req->cell] = GALLERY_CELL_WAITING;
        break;
    case GALLERY_FOR_PHOTO:
        g->photo = GALLERY_PIC_WAITING;
        break;
    case GALLERY_FOR_SLIDE:
        if (g->slots[slot].back) {
            g->slide_back = GALLERY_PIC_WAITING;
        } else {
            g->slide_front = GALLERY_PIC_WAITING;
        }
        break;
    }
}

/* A photo the slideshow could not show: the next one instead, unless none
 * of them can be shown. */
static void slide_skip(struct camera_gallery *g, bool back, int64_t now)
{
    g->slide_fails++;
    if (g->slide_fails >= g->count) {
        if (g->current < 0) {
            g->current = g->slide_want;
        }
        to_grid(g);
        reset_cells(g);
        note(g, "No photo could be shown", true, now);
        return;
    }
    g->slide_want = next_index(g, g->slide_want);
    if (back) {
        g->slide_back = GALLERY_PIC_NEED;
    } else {
        g->slide_front = GALLERY_PIC_NEED;
    }
}

enum gallery_dest camera_gallery_arrived(struct camera_gallery *g, const struct camera_event *ev,
                                         int64_t now, int *cell, unsigned *acts)
{
    struct gallery_slot rec;
    bool ok = ev->kind == CAMERA_EV_IMAGE;
    int slot = ev->value;

    *acts |= GALLERY_DO_PICTURES; /* a slot is free again either way */
    if (slot < 0 || slot >= CAMERA_PICTURE_SLOTS || !g->slots[slot].used) {
        return GALLERY_DEST_DROP;
    }
    rec = g->slots[slot];
    g->slots[slot].used = false;
    if (rec.index < 0) {
        return GALLERY_DEST_DROP;
    }
    switch (rec.purpose) {
    case GALLERY_FOR_THUMB:
        if (rec.cell < 0 || rec.cell >= GALLERY_PAGE_MAX ||
            camera_gallery_cell_index(g, rec.cell) != rec.index ||
            g->cells[rec.cell] != GALLERY_CELL_WAITING) {
            return GALLERY_DEST_DROP;
        }
        g->cells[rec.cell] = ok ? GALLERY_CELL_SHOWN : GALLERY_CELL_BAD;
        *cell = rec.cell;
        return ok ? GALLERY_DEST_THUMB : GALLERY_DEST_DROP;

    case GALLERY_FOR_PHOTO:
        if (g->view != GALLERY_PHOTO || rec.index != g->current ||
            g->photo != GALLERY_PIC_WAITING) {
            return GALLERY_DEST_DROP;
        }
        if (!ok) {
            g->photo = GALLERY_PIC_BAD;
            g->photo_fail = (enum camera_imgfail)ev->reason;
            return GALLERY_DEST_DROP;
        }
        g->photo = GALLERY_PIC_SHOWN;
        g->meta = ev->image;
        snprintf(g->description, sizeof(g->description), "%s", ev->text);
        return GALLERY_DEST_PHOTO;

    case GALLERY_FOR_SLIDE:
        if (g->view != GALLERY_SLIDESHOW || rec.index != g->slide_want ||
            (rec.back ? g->slide_back : g->slide_front) != GALLERY_PIC_WAITING) {
            return GALLERY_DEST_DROP;
        }
        if (!ok) {
            slide_skip(g, rec.back, now);
            return GALLERY_DEST_DROP;
        }
        g->slide_fails = 0;
        if (rec.back) {
            g->slide_back = GALLERY_PIC_SHOWN;
            g->slide_back_index = rec.index;
            return GALLERY_DEST_SLIDE_BACK;
        }
        g->slide_front = GALLERY_PIC_SHOWN;
        g->current = rec.index;
        g->slides_shown++;
        g->slide_due = now + GALLERY_SLIDE_MS;
        g->slide_want = next_index(g, rec.index);
        g->slide_back = g->count > 1 ? GALLERY_PIC_NEED : GALLERY_PIC_NONE;
        return GALLERY_DEST_SLIDE_FRONT;
    }
    return GALLERY_DEST_DROP;
}

void camera_gallery_lost(struct camera_gallery *g, enum gallery_dest dest, int cell, int64_t now)
{
    switch (dest) {
    case GALLERY_DEST_THUMB:
        if (cell >= 0 && cell < GALLERY_PAGE_MAX) {
            g->cells[cell] = GALLERY_CELL_BAD;
        }
        break;
    case GALLERY_DEST_PHOTO:
        g->photo = GALLERY_PIC_BAD;
        g->photo_fail = CAMERA_IMGFAIL_IO;
        break;
    case GALLERY_DEST_SLIDE_FRONT:
        /* The picture before it stays on screen for this photo's time. */
        break;
    case GALLERY_DEST_SLIDE_BACK:
        g->slide_back = GALLERY_PIC_WAITING; /* what slide_skip expects to replace */
        g->slide_want = g->slide_back_index >= 0 ? g->slide_back_index : g->slide_want;
        g->slide_back_index = -1;
        slide_skip(g, true, now);
        break;
    default:
        break;
    }
}

const char *camera_gallery_current_name(const struct camera_gallery *g)
{
    return g->current >= 0 && g->current < g->count ? g->names[g->current] : NULL;
}

/* ---- words ------------------------------------------------------------------------ */

void camera_gallery_bytes_text(uint64_t bytes, char *out, size_t len)
{
    if (bytes < 1024) {
        snprintf(out, len, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, len, "%u KB", (unsigned)((bytes + 512) / 1024));
    } else {
        uint64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);

        snprintf(out, len, "%u.%u MB", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
    }
}

static bool digits(const char *s, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static int number_at(const char *s, int n)
{
    int v = 0;
    int i;

    for (i = 0; i < n; i++) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* The date in a photo name made with the clock set: IMG_yyyymmdd_hhmmss_...
 * A name that only looks like one (a year before EXIF, a 13th month) has no
 * date, by the same floor as the helper's EXIF reader. */
static bool name_date(const char *name, char *out, size_t len)
{
    const char *d = name + 4;

    if (strncmp(name, "IMG_", 4) != 0 || strlen(name) < 20 || !digits(d, 8) || d[8] != '_' ||
        !digits(d + 9, 6) || d[15] != '_') {
        return false;
    }
    if (number_at(d, 4) < CAMERA_DATE_YEAR_MIN || number_at(d + 4, 2) < 1 ||
        number_at(d + 4, 2) > 12 || number_at(d + 6, 2) < 1 || number_at(d + 6, 2) > 31 ||
        number_at(d + 9, 2) > 23 || number_at(d + 11, 2) > 59 || number_at(d + 13, 2) > 60) {
        return false;
    }
    snprintf(out, len, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", d, d + 4, d + 6, d + 9, d + 11, d + 13);
    return true;
}

void camera_gallery_info(const struct camera_gallery *g, char *name, size_t name_len, char *when,
                         size_t when_len, char *what, size_t what_len)
{
    const char *n = camera_gallery_current_name(g);
    char date[32];
    char size[24];

    snprintf(name, name_len, "%s", n ? n : "");
    when[0] = '\0';
    what[0] = '\0';
    if (!n) {
        return;
    }
    if (g->photo == GALLERY_PIC_SHOWN && g->meta.taken[0]) {
        const char *t = g->meta.taken;

        snprintf(when, when_len, "Taken %.4s-%.2s-%.2s %.8s", t, t + 5, t + 8, t + 11);
    } else if (name_date(n, date, sizeof(date))) {
        snprintf(when, when_len, "Taken %s", date);
    } else if (g->photo == GALLERY_PIC_SHOWN || g->photo == GALLERY_PIC_BAD) {
        /* Camera names a photo without a date only when the clock is not set. */
        snprintf(when, when_len, "Date unknown: the clock was not set");
    }
    if (g->photo == GALLERY_PIC_SHOWN) {
        camera_gallery_bytes_text(g->meta.bytes, size, sizeof(size));
        snprintf(what, what_len, "%u x %u  |  %s  |  %s%s%s", g->meta.shown_w, g->meta.shown_h,
                 size, g->meta.jpeg ? "JPEG" : "PPM", g->meta.damaged ? "  |  damaged" : "",
                 strcmp(g->description, "Simulated picture") == 0 ? "  |  simulated" : "");
    } else if (g->photo == GALLERY_PIC_BAD) {
        snprintf(what, what_len, "%s",
                 g->photo_fail == CAMERA_IMGFAIL_MISSING       ? "The file is gone"
                 : g->photo_fail == CAMERA_IMGFAIL_CORRUPT     ? "The file is damaged and cannot be shown"
                 : g->photo_fail == CAMERA_IMGFAIL_UNSUPPORTED ? "This kind of file cannot be shown"
                 : g->photo_fail == CAMERA_IMGFAIL_TOOLARGE    ? "The picture is too large to show"
                                                               : "The file could not be read");
    } else {
        snprintf(what, what_len, "Opening...");
    }
}

void camera_gallery_screen(const struct camera_gallery *g, struct gallery_screen *out)
{
    memset(out, 0, sizeof(*out));
    if (g->note[0]) {
        snprintf(out->status, sizeof(out->status), "%s", g->note);
        out->status_warn = g->note_warn;
    }
    switch (g->view) {
    case GALLERY_OPENING:
        out->show_panel = true;
        out->left = "CAMERA";
        out->left_enabled = true;
        break;
    case GALLERY_FAILED:
        out->show_panel = true;
        out->left = "CAMERA";
        out->left_enabled = true;
        out->middle = "TRY AGAIN";
        out->middle_enabled = true;
        out->middle_primary = true;
        break;
    case GALLERY_GRID:
        out->left = "CAMERA";
        out->left_enabled = true;
        if (g->count == 0) {
            out->show_panel = true;
            break;
        }
        out->show_grid = true;
        out->show_newer = out->show_older = true;
        out->newer_enabled = g->page > 0;
        out->older_enabled = g->page + 1 < camera_gallery_pages(g);
        out->middle = "SLIDESHOW";
        out->middle_enabled = true;
        if (!out->status[0]) {
            if (g->total > (uint32_t)g->count) {
                snprintf(out->status, sizeof(out->status), "Newest %d of %u photos  |  page %d of %d",
                         g->count, g->total, g->page + 1, camera_gallery_pages(g));
            } else {
                snprintf(out->status, sizeof(out->status), "%d photo%s  |  page %d of %d", g->count,
                         g->count == 1 ? "" : "s", g->page + 1, camera_gallery_pages(g));
            }
        }
        break;
    case GALLERY_PHOTO:
        out->show_photo = true;
        out->show_newer = out->show_older = true;
        out->newer_enabled = !busy(g) && !g->confirm_delete && g->current > 0;
        out->older_enabled = !busy(g) && !g->confirm_delete && g->current + 1 < g->count;
        if (g->confirm_delete) {
            out->left = "CANCEL";
            out->left_enabled = !g->deleting;
            out->right = "DELETE";
            out->right_enabled = !g->deleting;
            out->right_primary = true;
            snprintf(out->status, sizeof(out->status), "%s",
                     g->deleting ? "Deleting..." : "Delete this photo?");
            out->status_warn = true;
        } else {
            out->left = "BACK";
            out->left_enabled = !busy(g);
            out->middle = "EXPORT";
            out->middle_enabled = !busy(g);
            out->right = "DELETE";
            out->right_enabled = !busy(g);
            if (g->exporting) {
                snprintf(out->status, sizeof(out->status), "Exporting to Files...");
            } else if (!out->status[0]) {
                snprintf(out->status, sizeof(out->status), "%d of %d", g->current + 1, g->count);
            }
        }
        break;
    case GALLERY_SLIDESHOW:
        out->show_slideshow = true;
        if (g->current >= 0) {
            snprintf(out->status, sizeof(out->status), "Slideshow  |  %d of %d  |  tap to stop",
                     g->current + 1, g->count);
        } else {
            snprintf(out->status, sizeof(out->status), "Starting the slideshow...");
        }
        out->status_warn = false;
        break;
    }
}
