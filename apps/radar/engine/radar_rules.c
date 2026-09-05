/*
 * PocketRadar run state and rules. See radar_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_rules.h"

#include <string.h>

/* ---- events ----------------------------------------------------------- */

void radar_run_clear_events(struct radar_run *run)
{
    if (!run) {
        return;
    }
    run->event_count = 0;
    run->events_dropped = 0;
}

/* Record something that happened. The newest event is the one dropped when
 * the queue is full: an event lost at the head would be a spawn the UI
 * never learned about, which is worse than a missed effect. */
static void emit(struct radar_run *run, enum radar_event_type type,
                 const struct radar_contact *contact, int32_t value)
{
    struct radar_event *e;

    if (run->event_count >= RADAR_EVENTS_MAX) {
        if (run->events_dropped < 0xFFFFu) {
            run->events_dropped++;
        }
        return;
    }
    e = &run->events[run->event_count++];
    e->type = (uint8_t)type;
    e->cls = contact ? contact->cls : (uint8_t)RADAR_CLASS_COUNT;
    e->bearing = contact ? contact->bearing : 0u;
    e->range = contact ? contact->range : 0u;
    e->id = contact ? contact->id : RADAR_NO_CONTACT;
    e->value = value;
}

int radar_run_take_event(struct radar_run *run, struct radar_event *out)
{
    int i;

    if (!run || !out || run->event_count == 0) {
        return 0;
    }
    *out = run->events[0];
    for (i = 1; i < run->event_count; i++) {
        run->events[i - 1] = run->events[i];
    }
    run->event_count--;
    return 1;
}

/* ---- contacts --------------------------------------------------------- */

static uint16_t range_for(const struct radar_contact *c)
{
    int32_t span = (int32_t)c->spawn_range - RADAR_RANGE_MIN;

    if (c->ttl_max == 0) {
        return RADAR_RANGE_MIN;
    }
    return (uint16_t)(RADAR_RANGE_MIN + span * (int32_t)c->ttl / (int32_t)c->ttl_max);
}

static struct radar_contact *find_mutable(struct radar_run *run, uint32_t id)
{
    int i;

    if (id == RADAR_NO_CONTACT) {
        return NULL;
    }
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        if (run->contacts[i].active && run->contacts[i].id == id) {
            return &run->contacts[i];
        }
    }
    return NULL;
}

const struct radar_contact *radar_run_find(const struct radar_run *run, uint32_t id)
{
    if (!run) {
        return NULL;
    }
    /* The lookup only reads; the cast keeps one copy of the search rather
     * than two that could drift apart. */
    return find_mutable((struct radar_run *)run, id);
}

const struct radar_contact *radar_run_slot(const struct radar_run *run, int slot)
{
    if (!run || slot < 0 || slot >= RADAR_CONTACTS_MAX) {
        return NULL;
    }
    return &run->contacts[slot];
}

int radar_run_contact_count(const struct radar_run *run)
{
    int i;
    int n = 0;

    if (!run) {
        return 0;
    }
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        n += run->contacts[i].active != 0;
    }
    return n;
}

const struct radar_contact *radar_run_selected(const struct radar_run *run)
{
    if (!run) {
        return NULL;
    }
    return radar_run_find(run, run->selected);
}

int radar_contact_lock_permille(const struct radar_contact *contact)
{
    int32_t lock;

    if (!contact || !contact->active) {
        return 0;
    }
    if (contact->state == RADAR_CONTACT_ACQUIRED) {
        return 1000;
    }
    lock = contact->lock;
    if (lock > RADAR_ACQUIRE_TICKS) {
        lock = RADAR_ACQUIRE_TICKS;
    }
    return (int)(lock * 1000 / RADAR_ACQUIRE_TICKS);
}

int radar_contact_ttl_permille(const struct radar_contact *contact)
{
    if (!contact || !contact->active || contact->ttl_max == 0) {
        return 0;
    }
    return (int)((int32_t)contact->ttl * 1000 / (int32_t)contact->ttl_max);
}

/* ---- selection -------------------------------------------------------- */

/* Return the selected contact to the unselected state, keeping whatever it
 * has already revealed about itself. */
static void drop_selection(struct radar_run *run)
{
    struct radar_contact *c = find_mutable(run, run->selected);

    if (c) {
        c->state = RADAR_CONTACT_NEW;
        c->lock = 0;
    }
    run->selected = RADAR_NO_CONTACT;
}

int radar_run_select(struct radar_run *run, uint32_t id)
{
    struct radar_contact *c;

    if (!run || run->state != RADAR_RUN_ACTIVE) {
        return -1;
    }
    c = find_mutable(run, id);
    if (!c) {
        return -1;
    }
    if (run->selected == id) {
        return 0; /* a second tap must not throw away the lock */
    }
    drop_selection(run);
    c->state = RADAR_CONTACT_SELECTED;
    c->lock = 0;
    run->selected = id;
    return 0;
}

void radar_run_deselect(struct radar_run *run)
{
    if (!run) {
        return;
    }
    drop_selection(run);
}

uint32_t radar_run_pick(const struct radar_run *run, int bearing, int range)
{
    int32_t best = (int32_t)RADAR_PICK_RADIUS * RADAR_PICK_RADIUS + 1;
    uint32_t id = RADAR_NO_CONTACT;
    int i;

    if (!run) {
        return RADAR_NO_CONTACT;
    }
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *c = &run->contacts[i];
        int32_t d2;

        if (!c->active) {
            continue;
        }
        d2 = radar_polar_dist2(bearing, range, c->bearing, c->range);
        /* Nearest wins; the older track breaks a tie, because slot order is
         * reuse order and would otherwise make the answer arbitrary. */
        if (d2 < best || (d2 == best && c->id < id)) {
            best = d2;
            id = c->id;
        }
    }
    return id;
}

/* ---- spawning --------------------------------------------------------- */

static enum radar_class class_for_roll(uint32_t roll)
{
    if (roll < RADAR_DECOY_PCT_BASE) {
        return RADAR_CLASS_DECOY;
    }
    roll -= RADAR_DECOY_PCT_BASE;
    if (roll < RADAR_HIGH_VALUE_PCT_BASE) {
        return RADAR_CLASS_HIGH_VALUE;
    }
    roll -= RADAR_HIGH_VALUE_PCT_BASE;
    if (roll < RADAR_FAST_PCT_BASE) {
        return RADAR_CLASS_FAST;
    }
    return RADAR_CLASS_NORMAL;
}

static struct radar_contact *free_slot(struct radar_run *run, int limit)
{
    int i;

    if (radar_run_contact_count(run) >= limit) {
        return NULL;
    }
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        if (!run->contacts[i].active) {
            return &run->contacts[i];
        }
    }
    return NULL;
}

static void spawn(struct radar_run *run)
{
    enum radar_class cls;
    int bearing;
    int spawn_range;
    int drift;
    struct radar_contact *c;

    /* Every draw happens before the scope is consulted, so a full scope
     * costs a contact but never shifts the stream. */
    cls = class_for_roll(radar_rng_below(&run->rng, 100u));
    bearing = (int)radar_rng_below(&run->rng, RADAR_BEARING_MAX);
    spawn_range = RADAR_SPAWN_RANGE_MIN +
                  (int)radar_rng_below(&run->rng,
                                       RADAR_RANGE_MAX - RADAR_SPAWN_RANGE_MIN + 1);
    drift = (int)radar_rng_range(&run->rng, -RADAR_DRIFT_BASE, RADAR_DRIFT_BASE);

    c = free_slot(run, RADAR_CONTACTS_BASE);
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->id = run->next_id++;
    c->active = 1;
    c->cls = (uint8_t)cls;
    c->state = (uint8_t)RADAR_CONTACT_NEW;
    c->bearing = (uint16_t)bearing;
    c->spawn_range = (uint16_t)spawn_range;
    c->drift = (int16_t)drift;
    c->ttl_max = radar_class_base_ttl(cls);
    c->ttl = c->ttl_max;
    c->range = range_for(c);
    emit(run, RADAR_EVENT_SPAWN, c, 0);
}

/* ---- run -------------------------------------------------------------- */

void radar_run_new(struct radar_run *run, uint32_t seed)
{
    if (!run) {
        return;
    }
    memset(run, 0, sizeof(*run));
    run->seed = seed;
    radar_rng_seed(&run->rng, seed);
    run->state = (uint8_t)RADAR_RUN_READY;
    run->next_id = 1;
    /* The first track is painted on the first tick rather than after a
     * silent interval: an empty scope is a dead opening. */
    run->spawn_timer = 1;
    run->selected = RADAR_NO_CONTACT;
}

int radar_run_start(struct radar_run *run)
{
    if (!run || run->state != RADAR_RUN_READY) {
        return -1;
    }
    run->state = (uint8_t)RADAR_RUN_ACTIVE;
    return 0;
}

int radar_run_is_over(const struct radar_run *run)
{
    return run && run->state == RADAR_RUN_OVER;
}

/* Retire a track that ran out of time. A decoy that fades is correct play
 * and the caller is told which class it was, so it can charge for one and
 * not the other. */
static void fade(struct radar_run *run, struct radar_contact *c)
{
    c->ttl = 0;
    c->range = RADAR_RANGE_MIN;
    emit(run, RADAR_EVENT_FADED, c, 0);
    if (run->selected == c->id) {
        run->selected = RADAR_NO_CONTACT;
    }
    memset(c, 0, sizeof(*c));
}

static void advance(struct radar_run *run, struct radar_contact *c)
{
    if (c->age < 0xFFFFu) {
        c->age++;
    }
    if (c->ttl > 0) {
        c->ttl--;
    }
    if (c->ttl == 0) {
        fade(run, c);
        return;
    }
    c->bearing = (uint16_t)radar_bearing_wrap((int)c->bearing + c->drift);
    c->range = range_for(c);
    if (c->state != RADAR_CONTACT_SELECTED) {
        return;
    }
    c->lock++;
    if (c->lock >= RADAR_ACQUIRE_TICKS) {
        c->state = (uint8_t)RADAR_CONTACT_ACQUIRED;
        c->classified = 1;
        emit(run, RADAR_EVENT_ACQUIRED, c, 0);
    }
}

void radar_run_tick(struct radar_run *run)
{
    int i;

    if (!run || run->state != RADAR_RUN_ACTIVE) {
        return;
    }
    run->ticks++;
    run->sweep = (uint16_t)radar_bearing_wrap((int)run->sweep + RADAR_SWEEP_DD_PER_TICK);

    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        if (run->contacts[i].active) {
            advance(run, &run->contacts[i]);
        }
    }

    if (run->spawn_timer > 0) {
        run->spawn_timer--;
    }
    if (run->spawn_timer == 0) {
        spawn(run);
        run->spawn_timer = RADAR_SPAWN_TICKS_BASE;
    }
}
