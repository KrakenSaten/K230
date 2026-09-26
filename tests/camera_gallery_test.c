/*
 * Camera's gallery state machine on its own (apps/camera/camera_gallery.c):
 * opening and listing, pages and their thumbnails, stale answers after a
 * page turn, the photo view and its navigation, what is said about a photo
 * (a clock that was not set included), delete with its confirmation, export,
 * the slideshow - its order, its interval, skipping what cannot be shown, and
 * holding no more than its two pictures however long it runs - and the
 * helper failing.
 *
 * No helper, no LVGL: events are made here, as the session would deliver
 * them, and the clock is passed in.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_gallery.h"

#include <stdio.h>
#include <stdlib.h>
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

static struct camera_gallery G;
static char names[40][CAMERA_NAME_MAX];

static struct camera_event event(enum camera_ev_kind kind)
{
    struct camera_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    return ev;
}

/* n photos, newest first: IMG_<n>.jpg down to IMG_0001.jpg. */
static void list_of(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        snprintf(names[i], sizeof(names[i]), "IMG_%04d.jpg", n - i);
    }
}

/* Open the gallery as the app does, laid out for 6 cells a page. */
static void open_with(int n)
{
    struct camera_event ev = event(CAMERA_EV_READY);
    unsigned acts;

    camera_gallery_init(&G);
    acts = camera_gallery_open(&G);
    camera_gallery_set_sizes(&G, 6, 170, 528, 800, 528, 1000);
    acts = camera_gallery_event(&G, &ev, 0);
    list_of(n);
    ev = event(CAMERA_EV_LISTED);
    ev.value = n;
    ev.bytes = (uint64_t)n;
    acts = camera_gallery_event(&G, &ev, 0);
    (void)acts;
    camera_gallery_set_list(&G, names, n, (uint32_t)n);
}

/* Ask for everything the model wants, as the app would: slots from 0 up. */
static int ask_all(struct gallery_request *reqs, int max)
{
    struct gallery_request req;
    int n = 0;

    while (n < max && camera_gallery_next_request(&G, &req)) {
        int slot;

        for (slot = 0; slot < CAMERA_PICTURE_SLOTS && G.slots[slot].used; slot++) {
        }
        camera_gallery_requested(&G, &req, slot);
        reqs[n++] = req;
    }
    return n;
}

static int slots_used(void)
{
    int i;
    int n = 0;

    for (i = 0; i < CAMERA_PICTURE_SLOTS; i++) {
        n += G.slots[i].used;
    }
    return n;
}

/* The helper answers slot with a picture (ok) or a failure. */
static enum gallery_dest answer(int slot, bool ok, int64_t now, int *cell)
{
    struct camera_event ev = event(ok ? CAMERA_EV_IMAGE : CAMERA_EV_IMGFAIL);
    unsigned acts = 0;
    int c = -1;
    enum gallery_dest d;

    ev.value = slot;
    ev.w = 10;
    ev.h = 10;
    ev.reason = CAMERA_IMGFAIL_CORRUPT;
    ev.image.shown_w = 1080;
    ev.image.shown_h = 1920;
    ev.image.bytes = 412345;
    ev.image.jpeg = true;
    d = camera_gallery_arrived(&G, &ev, now, &c, &acts);
    if (cell) {
        *cell = c;
    }
    return d;
}

/* ---- opening -------------------------------------------------------------------- */

static void test_opening(void)
{
    struct camera_event ev;
    struct gallery_screen s;
    unsigned acts;

    camera_gallery_init(&G);
    acts = camera_gallery_open(&G);
    check("opening starts the library helper", acts == GALLERY_DO_START && G.view == GALLERY_OPENING);
    camera_gallery_screen(&G, &s);
    check("and says so, with a way back to the camera",
          s.show_panel && strcmp(G.title, "Opening photos") == 0 && strcmp(s.left, "CAMERA") == 0);
    ev = event(CAMERA_EV_READY);
    check("ready: list the photos", camera_gallery_event(&G, &ev, 0) == GALLERY_DO_LIST);
    ev = event(CAMERA_EV_LISTED);
    check("listed: take the list", camera_gallery_event(&G, &ev, 0) == GALLERY_DO_TAKE_LIST);
    camera_gallery_set_list(&G, names, 0, 0);
    camera_gallery_screen(&G, &s);
    check("an empty library says there are no photos",
          G.view == GALLERY_GRID && s.show_panel && !s.show_grid &&
              strcmp(G.title, "No photos yet") == 0 && s.middle == NULL);

    open_with(0);
    ev = event(CAMERA_EV_EXITED);
    ev.reason = CAMERA_EXIT_CRASHED;
    ev.value = 128 + 11;
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("a crashed helper: FAILED, with TRY AGAIN and CAMERA",
          G.view == GALLERY_FAILED && strstr(G.detail, "crashed (signal 11)") != NULL &&
              strcmp(s.middle, "TRY AGAIN") == 0 && strcmp(s.left, "CAMERA") == 0);
    check("try again starts it again", camera_gallery_middle(&G, 0) == GALLERY_DO_START &&
                                           G.view == GALLERY_OPENING);
    ev = event(CAMERA_EV_LISTFAIL);
    snprintf(ev.text, sizeof(ev.text), "Permission denied");
    camera_gallery_event(&G, &ev, 0);
    check("a folder that cannot be read: FAILED, saying why",
          G.view == GALLERY_FAILED && strstr(G.detail, "Permission denied") != NULL);
    ev = event(CAMERA_EV_ERROR);
    snprintf(ev.text, sizeof(ev.text), "exec /usr/bin/pos-camera: No such file");
    open_with(0);
    camera_gallery_event(&G, &ev, 0);
    check("a missing helper: FAILED, saying so",
          G.view == GALLERY_FAILED && strstr(G.detail, "could not be started") != NULL);
    check("CAMERA leaves from anywhere but the photo view",
          camera_gallery_left(&G) == GALLERY_DO_CAMERA);
}

/* ---- the grid ---------------------------------------------------------------------- */

static void test_grid(void)
{
    struct gallery_request reqs[32];
    struct gallery_screen s;
    int n;
    int cell;
    int i;
    bool newest_first = true;

    open_with(14); /* three pages of six: 6, 6, 2 */
    camera_gallery_screen(&G, &s);
    check("fourteen photos: the grid, page 1 of 3",
          G.view == GALLERY_GRID && s.show_grid && strstr(s.status, "14 photos") &&
              strstr(s.status, "page 1 of 3"));
    check("NEWER is off on the first page, OLDER on", !s.newer_enabled && s.older_enabled);
    for (i = 0; i < 6; i++) {
        newest_first &= camera_gallery_cell_index(&G, i) == i;
    }
    check("cells show the newest first", newest_first && strcmp(G.names[0], "IMG_0014.jpg") == 0);

    n = ask_all(reqs, 32);
    check("three thumbnails are asked for at once, no more", n == 3 && slots_used() == 3);
    check("covering a 170-pixel cell, the newest first",
          reqs[0].purpose == GALLERY_FOR_THUMB && reqs[0].cover && reqs[0].w == 170 &&
              strcmp(reqs[0].name, "IMG_0014.jpg") == 0 && reqs[2].cell == 2);
    check("the first comes back into cell 0",
          answer(0, true, 0, &cell) == GALLERY_DEST_THUMB && cell == 0 &&
              G.cells[0] == GALLERY_CELL_SHOWN);
    check("a damaged one marks its cell", answer(1, false, 0, &cell) == GALLERY_DEST_DROP &&
                                              G.cells[1] == GALLERY_CELL_BAD);
    n = ask_all(reqs, 32);
    check("freed slots are used for the next cells", n == 2 && reqs[0].cell == 3);

    /* Turning the page while three answers are out. */
    camera_gallery_older(&G);
    check("OLDER: page 2", G.page == 1 && camera_gallery_cell_index(&G, 0) == 6);
    check("answers for page 1 are dropped when they come",
          answer(2, true, 0, &cell) == GALLERY_DEST_DROP && answer(0, true, 0, &cell) ==
                                                                  GALLERY_DEST_DROP &&
              answer(1, true, 0, &cell) == GALLERY_DEST_DROP && slots_used() == 0);
    n = ask_all(reqs, 32);
    check("and page 2's are asked for", n == 3 && strcmp(reqs[0].name, "IMG_0008.jpg") == 0);
    for (i = 0; i < 3; i++) {
        answer(i, true, 0, NULL);
    }
    camera_gallery_older(&G);
    camera_gallery_screen(&G, &s);
    check("the last page has two", G.page == 2 && camera_gallery_cell_index(&G, 1) == 13 &&
                                       camera_gallery_cell_index(&G, 2) == -1 &&
                                       G.cells[2] == GALLERY_CELL_EMPTY && !s.older_enabled);
    check("OLDER past the last page does nothing", camera_gallery_older(&G) == 0 && G.page == 2);
    check("a tap on an empty cell does nothing", camera_gallery_tap_cell(&G, 4) == 0);

    /* The body changes: four a page now. The page keeps its first photo. */
    camera_gallery_set_sizes(&G, 4, 200, 528, 800, 528, 1000);
    check("a new page size keeps the first photo shown in view",
          G.page == 3 && camera_gallery_cell_index(&G, 0) == 12);
}

/* ---- one photo ----------------------------------------------------------------------- */

static void test_photo(void)
{
    struct gallery_request reqs[8];
    struct gallery_screen s;
    struct camera_event ev;
    char name[CAMERA_NAME_MAX];
    char when[64];
    char what[96];
    int n;

    open_with(5);
    ask_all(reqs, 8);
    check("a tap on a cell opens that photo",
          camera_gallery_tap_cell(&G, 2) == GALLERY_DO_PICTURES && G.view == GALLERY_PHOTO &&
              G.current == 2);
    n = ask_all(reqs, 8);
    check("its picture waits for a slot (the grid's three are still out)", n == 0);
    answer(0, true, 0, NULL);
    check("a thumbnail arriving in the photo view still fills its cell", G.cells[0] == GALLERY_CELL_SHOWN);
    n = ask_all(reqs, 8);
    check("the photo is asked for first, fitted into the photo box",
          n == 1 && reqs[0].purpose == GALLERY_FOR_PHOTO && !reqs[0].cover && reqs[0].w == 528 &&
              reqs[0].h == 800 && strcmp(reqs[0].name, "IMG_0003.jpg") == 0);
    camera_gallery_info(&G, name, sizeof(name), when, sizeof(when), what, sizeof(what));
    check("while it comes: its name and 'Opening...'",
          strcmp(name, "IMG_0003.jpg") == 0 && strcmp(what, "Opening...") == 0 && when[0] == '\0');
    check("the photo arrives", answer(0, true, 0, NULL) == GALLERY_DEST_PHOTO &&
                                   G.photo == GALLERY_PIC_SHOWN);
    camera_gallery_info(&G, name, sizeof(name), when, sizeof(when), what, sizeof(what));
    check("what is known: size, file size, format",
          strcmp(what, "1080 x 1920  |  403 KB  |  JPEG") == 0);
    check("no date in the file or the name: the clock was not set",
          strcmp(when, "Date unknown: the clock was not set") == 0);
    camera_gallery_screen(&G, &s);
    check("the view: BACK, EXPORT, DELETE, NEWER and OLDER, and where it is",
          s.show_photo && strcmp(s.left, "BACK") == 0 && strcmp(s.middle, "EXPORT") == 0 &&
              strcmp(s.right, "DELETE") == 0 && s.newer_enabled && s.older_enabled &&
              strcmp(s.status, "3 of 5") == 0);

    camera_gallery_older(&G);
    check("OLDER: the next older photo, asked for again",
          G.current == 3 && G.photo == GALLERY_PIC_NEED);
    camera_gallery_older(&G);
    camera_gallery_screen(&G, &s);
    check("OLDER stops at the oldest", G.current == 4 && !s.older_enabled &&
                                           camera_gallery_older(&G) == 0);
    camera_gallery_newer(&G);
    check("NEWER goes back", G.current == 3);

    /* A dated photo and an EXIF date. */
    snprintf(G.names[3], CAMERA_NAME_MAX, "IMG_20260926_101530_0002.jpg");
    camera_gallery_info(&G, name, sizeof(name), when, sizeof(when), what, sizeof(what));
    check("a dated name gives the date before the file is read",
          strcmp(when, "Taken 2026-09-26 10:15:30") == 0);
    ask_all(reqs, 8);
    ev = event(CAMERA_EV_IMAGE);
    for (n = 0; n < CAMERA_PICTURE_SLOTS; n++) {
        if (G.slots[n].used && G.slots[n].purpose == GALLERY_FOR_PHOTO) {
            break;
        }
    }
    ev.value = n;
    ev.image.shown_w = 72;
    ev.image.shown_h = 128;
    ev.image.bytes = 900;
    ev.image.damaged = true;
    ev.image.jpeg = true;
    snprintf(ev.image.taken, sizeof(ev.image.taken), "2026:09:25 23:59:58");
    snprintf(ev.text, sizeof(ev.text), "Simulated picture");
    {
        unsigned acts = 0;
        int cell;

        check("a photo whose file has its own date",
              camera_gallery_arrived(&G, &ev, 0, &cell, &acts) == GALLERY_DEST_PHOTO);
    }
    camera_gallery_info(&G, name, sizeof(name), when, sizeof(when), what, sizeof(what));
    check("the file's date wins over the name's", strcmp(when, "Taken 2026-09-25 23:59:58") == 0);
    check("damaged and simulated are said",
          strcmp(what, "72 x 128  |  900 B  |  JPEG  |  damaged  |  simulated") == 0);

    /* A file that cannot be shown. */
    camera_gallery_older(&G);
    ask_all(reqs, 8);
    for (n = 0; n < CAMERA_PICTURE_SLOTS; n++) {
        if (G.slots[n].used && G.slots[n].purpose == GALLERY_FOR_PHOTO) {
            answer(n, false, 0, NULL);
        }
    }
    camera_gallery_info(&G, name, sizeof(name), when, sizeof(when), what, sizeof(what));
    check("a damaged file: said, and nothing crashes",
          G.photo == GALLERY_PIC_BAD && strcmp(what, "The file is damaged and cannot be shown") == 0);
    camera_gallery_screen(&G, &s);
    check("it can still be deleted or exported", s.right_enabled && s.middle_enabled);

    check("BACK: the grid, on the page of the photo",
          camera_gallery_left(&G) == GALLERY_DO_PICTURES && G.view == GALLERY_GRID && G.page == 0);
}

static void test_delete_export(void)
{
    struct gallery_request reqs[8];
    struct gallery_screen s;
    struct camera_event ev;
    unsigned acts;

    open_with(3);
    camera_gallery_tap_cell(&G, 1);
    check("DELETE first asks", camera_gallery_right(&G) == 0 && G.confirm_delete);
    camera_gallery_screen(&G, &s);
    check("with CANCEL and a primary DELETE, and nothing else to tap",
          strcmp(s.left, "CANCEL") == 0 && strcmp(s.right, "DELETE") == 0 && s.right_primary &&
              s.middle == NULL && !s.newer_enabled && !s.older_enabled &&
              strcmp(s.status, "Delete this photo?") == 0);
    camera_gallery_left(&G);
    check("CANCEL goes back to the photo", !G.confirm_delete && G.view == GALLERY_PHOTO);
    camera_gallery_right(&G);
    check("DELETE twice deletes", camera_gallery_right(&G) == GALLERY_DO_DELETE && G.deleting &&
                                      strcmp(camera_gallery_current_name(&G), "IMG_0002.jpg") == 0);
    camera_gallery_screen(&G, &s);
    check("while deleting nothing can be tapped", !s.left_enabled && !s.right_enabled &&
                                                      strcmp(s.status, "Deleting...") == 0);
    ask_all(reqs, 8);
    ev = event(CAMERA_EV_DELETED);
    snprintf(ev.name, sizeof(ev.name), "IMG_0002.jpg");
    ev.value = 2;
    acts = camera_gallery_event(&G, &ev, 100);
    check("deleted: gone from the list", G.count == 2 && strcmp(G.names[1], "IMG_0001.jpg") == 0);
    check("the next older photo takes its place", G.view == GALLERY_PHOTO && G.current == 1 &&
                                                      G.photo == GALLERY_PIC_NEED &&
                                                      (acts & GALLERY_DO_PICTURES));
    check("answers asked for before the delete are dropped",
          answer(0, true, 0, NULL) == GALLERY_DEST_DROP);
    camera_gallery_screen(&G, &s);
    check("and the screen says so", strcmp(s.status, "Photo deleted") == 0);
    camera_gallery_tick(&G, 100 + GALLERY_NOTE_MS, &acts);
    camera_gallery_screen(&G, &s);
    check("for a while", strcmp(s.status, "2 of 2") == 0);

    camera_gallery_right(&G);
    camera_gallery_right(&G);
    ev = event(CAMERA_EV_DELFAIL);
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("a delete that fails: said, the photo stays",
          G.count == 2 && !G.deleting && s.status_warn &&
              strcmp(s.status, "The photo could not be deleted") == 0);

    check("EXPORT asks the helper", camera_gallery_middle(&G, 0) == GALLERY_DO_EXPORT && G.exporting);
    camera_gallery_screen(&G, &s);
    check("and waits, saying so", !s.middle_enabled && !s.left_enabled &&
                                      strcmp(s.status, "Exporting to Files...") == 0);
    ev = event(CAMERA_EV_EXPORTED);
    snprintf(ev.path, sizeof(ev.path), "/root/Pictures/IMG_0001.jpg");
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("exported: the screen says where",
          !G.exporting && strcmp(s.status, "Saved to Files: /root/Pictures/IMG_0001.jpg") == 0);
    camera_gallery_middle(&G, 0);
    ev.value = 1;
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("again: already there", strncmp(s.status, "Already in Files:", 17) == 0);
    camera_gallery_middle(&G, 0);
    ev = event(CAMERA_EV_EXPFAIL);
    ev.reason = CAMERA_EXPFAIL_NOSPACE;
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("a full disk: said", s.status_warn && strcmp(s.status, "Storage is full: nothing was exported") == 0);

    /* The last photo goes. */
    camera_gallery_right(&G);
    camera_gallery_right(&G);
    ev = event(CAMERA_EV_DELETED);
    snprintf(ev.name, sizeof(ev.name), "IMG_0001.jpg");
    camera_gallery_event(&G, &ev, 0);
    check("the one before the oldest takes the oldest's place",
          G.view == GALLERY_PHOTO && G.current == 0 &&
              strcmp(camera_gallery_current_name(&G), "IMG_0003.jpg") == 0);
    camera_gallery_right(&G);
    camera_gallery_right(&G);
    ev = event(CAMERA_EV_DELETED);
    snprintf(ev.name, sizeof(ev.name), "IMG_0003.jpg");
    camera_gallery_event(&G, &ev, 0);
    camera_gallery_screen(&G, &s);
    check("deleting the last photo leaves the empty grid",
          G.count == 0 && G.view == GALLERY_GRID && s.show_panel &&
              strcmp(G.title, "No photos yet") == 0);
}

/* ---- the slideshow --------------------------------------------------------------------- */

/* Answer every outstanding slide request; bad[] says which indices fail. */
static void answer_slides(const bool *bad, int64_t now, int *fronts, int *backs)
{
    int slot;

    for (slot = 0; slot < CAMERA_PICTURE_SLOTS; slot++) {
        if (G.slots[slot].used && G.slots[slot].purpose == GALLERY_FOR_SLIDE) {
            bool ok = !(bad && G.slots[slot].index >= 0 && bad[G.slots[slot].index]);
            enum gallery_dest d = answer(slot, ok, now, NULL);

            *fronts += d == GALLERY_DEST_SLIDE_FRONT;
            *backs += d == GALLERY_DEST_SLIDE_BACK;
        }
    }
}

static void test_slideshow(void)
{
    struct gallery_request reqs[8];
    struct gallery_screen s;
    unsigned acts;
    int fronts = 0;
    int backs = 0;
    int64_t now = 1000;
    int order[12];
    int i;
    bool in_order = true;
    int most_slots = 0;
    bool bad[6] = { false, false, true, false, true, false };

    open_with(4);
    camera_gallery_older(&G);
    check("SLIDESHOW from the grid", camera_gallery_middle(&G, now) == GALLERY_DO_PICTURES &&
                                         G.view == GALLERY_SLIDESHOW && G.slide_want == 0);
    camera_gallery_screen(&G, &s);
    check("before the first picture it says it is starting",
          s.show_slideshow && strcmp(s.status, "Starting the slideshow...") == 0);
    ask_all(reqs, 8);
    check("the first photo is asked for at the slideshow's size",
          reqs[0].purpose == GALLERY_FOR_SLIDE && reqs[0].w == 528 && reqs[0].h == 1000 &&
              reqs[0].index == 0 && !reqs[0].cover);
    answer_slides(NULL, now, &fronts, &backs);
    check("it is shown at once", fronts == 1 && G.current == 0 && G.slides_shown == 1);
    ask_all(reqs, 8);
    check("and the next is prepared behind it", reqs[0].index == 1 && G.slide_back == GALLERY_PIC_WAITING);
    answer_slides(NULL, now + 10, &fronts, &backs);
    check("prepared, not shown", backs == 1 && G.current == 0);
    acts = 0;
    camera_gallery_tick(&G, now + GALLERY_SLIDE_MS - 1, &acts);
    check("not before its time", !(acts & GALLERY_DO_SWAP) && G.current == 0);
    camera_gallery_tick(&G, now + GALLERY_SLIDE_MS, &acts);
    check("after GALLERY_SLIDE_MS the prepared one is shown",
          (acts & GALLERY_DO_SWAP) && G.current == 1 && G.slides_shown == 2);
    camera_gallery_screen(&G, &s);
    check("the status says which, and how to stop", strcmp(s.status, "Slideshow  |  2 of 4  |  tap to stop") == 0);

    /* Run on for a long time: it wraps, in order, and never holds more than
     * one request and two pictures. */
    order[0] = 0;
    order[1] = 1;
    for (i = 2; i < 12; i++) {
        now += GALLERY_SLIDE_MS;
        ask_all(reqs, 8);
        most_slots = slots_used() > most_slots ? slots_used() : most_slots;
        answer_slides(NULL, now, &fronts, &backs);
        acts = 0;
        camera_gallery_tick(&G, now + GALLERY_SLIDE_MS, &acts);
        now += GALLERY_SLIDE_MS;
        order[i] = G.current;
        in_order &= order[i] == (order[i - 1] + 1) % 4;
    }
    check("ten more: newest to oldest, then round again", in_order && G.slides_shown == 12);
    check("never more than one picture asked for at a time", most_slots == 1);
    check("the front picture is never replaced by an answer: only swaps show photos", fronts == 1);

    check("a tap stops it: back to the grid on the photo's page",
          camera_gallery_tap_slideshow(&G) == GALLERY_DO_PICTURES && G.view == GALLERY_GRID &&
              G.page == G.current / 6);
    check("answers still out are dropped", (ask_all(reqs, 8), 1) &&
                                               answer(0, true, now, NULL) != GALLERY_DEST_SLIDE_FRONT);

    /* Photos that cannot be shown are skipped. */
    open_with(6);
    camera_gallery_tap_cell(&G, 1);
    camera_gallery_slideshow(&G, now);
    check("from the photo view it starts on that photo", G.slide_want == 1);
    fronts = backs = 0;
    for (i = 0; i < 12; i++) {
        ask_all(reqs, 8);
        answer_slides(bad, now, &fronts, &backs);
        acts = 0;
        now += GALLERY_SLIDE_MS;
        camera_gallery_tick(&G, now, &acts);
        check(i == 0 ? "damaged photos are skipped" : "and again", G.current != 2 && G.current != 4);
        if (i >= 3) {
            break;
        }
    }
    {
        bool all_bad[6] = { true, true, true, true, true, true };

        open_with(6);
        camera_gallery_slideshow(&G, now);
        for (i = 0; i < 10 && G.view == GALLERY_SLIDESHOW; i++) {
            ask_all(reqs, 8);
            answer_slides(all_bad, now, &fronts, &backs);
        }
        camera_gallery_screen(&G, &s);
        check("when no photo can be shown it stops, and says so",
              G.view == GALLERY_GRID && strcmp(s.status, "No photo could be shown") == 0 && i <= 6);
    }
    open_with(1);
    camera_gallery_slideshow(&G, now);
    ask_all(reqs, 8);
    fronts = 0;
    answer_slides(NULL, now, &fronts, &backs);
    acts = 0;
    camera_gallery_tick(&G, now + 3 * GALLERY_SLIDE_MS, &acts);
    check("one photo: shown and kept, nothing prepared", fronts == 1 && !(acts & GALLERY_DO_SWAP) &&
                                                             G.slide_back == GALLERY_PIC_NONE &&
                                                             !camera_gallery_next_request(&G, reqs));
    open_with(0);
    check("no photos: no slideshow", camera_gallery_slideshow(&G, now) == 0 && G.view == GALLERY_GRID);
}

static void test_words(void)
{
    char t[32];

    camera_gallery_bytes_text(900, t, sizeof(t));
    check("bytes", strcmp(t, "900 B") == 0);
    camera_gallery_bytes_text(412345, t, sizeof(t));
    check("kilobytes", strcmp(t, "403 KB") == 0);
    camera_gallery_bytes_text(1468006, t, sizeof(t));
    check("megabytes", strcmp(t, "1.4 MB") == 0);
    check("every view has a name", strcmp(camera_gallery_view_name(GALLERY_SLIDESHOW),
                                          "GALLERY_SLIDESHOW") == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_opening();
    test_grid();
    test_photo();
    test_delete_export();
    test_slideshow();
    test_words();
    printf("camera_gallery_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
